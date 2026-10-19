/**
 * RE-SoT: hardware rasterisation for the pushbuffer executor (Direct3D 11).
 *
 * Included by nv2a_pb_exec.c, after its state and helpers, so it reads the
 * executor's decoded NV2A state (s_gpu) directly. The division of labour:
 *
 *   - the executor keeps decoding methods and keeps running vertex programs on
 *     the CPU (nv2a_vsh_interp.c), exactly as for the software rasteriser;
 *   - this file takes the transformed vertices of a batch and draws them with
 *     the host GPU instead of filling pixels on the CPU. The register
 *     combiners are evaluated by one pixel shader that interprets the
 *     combiner control words (a port of nv2a_combiner.c), so no shader is
 *     generated per state.
 *
 * Surfaces live on the host GPU and are keyed by the guest surface offset; a
 * texture stage that names a surface offset samples that render target.
 * Ordinary textures are decoded from guest memory through sample_tex(), the
 * same decoder the software path uses, and cached.
 *
 * Enabled with RECOMP_HW=1. RECOMP_HW_SCALE=<n> renders at n times the guest
 * resolution (default 2). RECOMP_HW_NOCULL=1 ignores the cull state.
 *
 * Vertex programs run on the host GPU as well: each distinct program is
 * translated to HLSL once (nv2a_vsh_emit_hlsl) and fed the raw attributes.
 * RECOMP_HW_CPU_VSH=1 goes back to the interpreter, for comparison.
 *
 * RECOMP_HW_ASPECT=<w:h or ratio> widens the picture: every host surface gets
 * the extra width, vertices that arrive with w == 1 (the title's 2D) are drawn
 * at their 4:3 size around the centre, and the title's own projection is
 * widened by the target's hooks (src/<target>/target_hooks.c): Hor+, more
 * picture at the sides only.
 * RECOMP_HW_VSYNC=0 presents without waiting; RECOMP_HW_FULLSCREEN=1 makes
 * the window borderless over its monitor (0: a window; Alt+Enter or F11
 * switches while running).
 *
 * Known gaps: cube maps (face 0 only), bump-env and dot-product
 * texture modes, point/line primitives, guest-visible framebuffer contents
 * (nothing is copied back to guest memory).
 */
#if defined(_WIN32)

#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

extern void *xbox_FramebufferWindowHandle(void);
extern void  xbox_FramebufferWindowHwMode(int on);
extern int   xbox_FramebufferOverlayGet(const uint32_t **px, uint32_t *w,
                                        uint32_t *h, uint32_t rect[4],
                                        uint32_t *serial);

#define HW_MAX_SURFACES 32
#define HW_MAX_DEPTHS   16
#define HW_MAX_TEXTURES 1024
#define HW_MAX_VERTS    (NV_MAX_INDICES * 3)
#define HW_VB_BYTES     (16u << 20)     /* vertex ring, appended NO_OVERWRITE */
#define HW_IB_BYTES     (2u << 20)      /* index ring, likewise */
#define HW_VSP_MAX      192             /* translated vertex programs kept */
#define HW_VSP_SLOTS    8               /* attributes a translated program reads */

typedef struct {
    uint32_t offset, w, h;              /* guest (resolved) address and size */
    uint32_t sc;                        /* host pixels per guest pixel (before widening) */
    ID3D11Texture2D *tex;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    uint32_t drawn_frame;
} HwSurface;

/* pd-js7 diagnostic table (see RECOMP_HW_CLIPDBG) */
static struct { uint32_t off, sw, sh, cx, cy, cw, ch; unsigned n; } s_hw_clip[64];
static int s_hw_clip_n;

/* pd-js7 / pd-42q: Phantom Dust draws everything inside a 592x456+24+12 window of its 640x480 display
 * (the console's overscan margin) and never touches the margin again, so whatever an earlier scene
 * left there (sky blocks, a red gradient, a white "F" glyph) stays in the frame we present. The
 * console showed it only where the TV's overscan hid it. Track which clip the frame's draws use
 * and black out the margin at present. */
static struct { uint32_t cx, cy, cw, ch; unsigned n; } s_hw_vc[8];
static int s_hw_vc_n;
static uint32_t s_hw_view[4];           /* chosen view clip (x, y, w, h); w == 0: none yet */

typedef struct {
    uint32_t offset, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} HwDepth;

typedef struct {
    uint32_t offset, fmt, w, h, pitch, palette;
    uint32_t hash, checked_frame, used_frame;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} HwTexture;

typedef struct {
    float pos[4], d0[4], d1[4], fog[4], tex[4][4];
} HwVertex;

/* Vertex-shader constants; layout mirrors cbuffer VSC below. */
typedef struct {
    float vp[4];                        /* 2/w, 2/h, 1/zmax */
    float fogp[4];                      /* fog param 0, param 1, mode, enable */
    float ws[4];                        /* 2D squeeze, surface width, edge margin */
    float pad[4];
    float cur[NV_VERTEX_ATTRS][4];      /* RE-SoT: attribute values with no array */
    float c[NV2A_VSH_CONSTANTS][4];     /* the program's constants */
} HwVsConst;

/* One translated vertex program: the NV2A program by content, and which
 * attribute each of its input slots carries. */
typedef struct {
    uint32_t key, mask;
    ID3D11VertexShader *vs;             /* NULL: translation failed */
} HwVsp;

/* Pixel-shader constants; layout mirrors cbuffer PSC below. */
typedef struct {
    uint32_t cw[8][4];
    float    fac0[8][4], fac1[8][4];
    uint32_t ctl[4];
    uint32_t atest[4];
    float    finc0[4], finc1[4], fogc[4];
    float    tscale[4][4];
    uint32_t misc[4];
} HwPsConst;

/* RECOMP_PERF diagnostics, toggled at run time by flag files in the working directory:
 * psdiag.flag = every draw uses the trivial pixel shader (what does the combiner cost?). */
static volatile int s_hw_psdiag;

static struct {
    int tried, ok, scale, nocull;
    int fx_scale;                       /* the same for surfaces smaller than the display */
    int auto_scale;                     /* RECOMP_HW_SCALE unset: follow the window height */
    int vsync, cpu_vsh;
    int tearing;                        /* flip-model swap chain with ALLOW_TEARING in use */
    double pace_hz;                     /* flips per second cap (own pacer), 0 = none */
    float aspect;                       /* shown width / height */
    float widen;                        /* host surface width multiple over 4:3 */
    CRITICAL_SECTION lock;
    HWND hwnd;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *bb_rtv;
    uint32_t bb_w, bb_h;
    ID3D11VertexShader *vs, *vs_blit;
    ID3D11PixelShader *ps, *ps_blit, *ps_solid;
    ID3D11InputLayout *layout, *layout_prog;
    ID3D11Buffer *vb, *ib, *vs_cb, *ps_cb;
    uint32_t vb_off, ib_off;
    HwVsp vsp[HW_VSP_MAX];
    int nvsp;
    HwVsConst vs_last;                  /* what vs_cb holds */
    uint32_t vs_last_gen;
    int vs_last_full, vs_last_valid;
    HwPsConst ps_last;
    int ps_last_valid;
    ID3D11SamplerState *samp[6][6][2];
    ID3D11SamplerState *samp_blit;
    ID3D11SamplerState *samp_lut;
    ID3D11Texture2D *pres_tex; ID3D11ShaderResourceView *pres_srv; UINT pres_w, pres_h; int pres_valid;
    int flip_held; unsigned flip_prev_draws;
    uint32_t disp_w, disp_h, cand_w, cand_h; int cand_streak;
    ID3D11Texture2D *lut_tex; ID3D11ShaderResourceView *lut_srv; LONG lut_serial;
    ID3D11ShaderResourceView *white;
    struct { uint64_t key; ID3D11BlendState *st; } blend[64];
    int nblend;
    struct { uint64_t key; ID3D11DepthStencilState *st; } ds[32];
    int nds;
    ID3D11RasterizerState *rs[3];        /* none, back, front (front = CW) */
    ID3D11RasterizerState *rs_ccw[3];    /* same with front = CCW */
    ID3D11RasterizerState *rs_blit;
    HwSurface surf[HW_MAX_SURFACES];
    HwDepth depth[HW_MAX_DEPTHS];
    HwTexture *texs;                     /* HW_MAX_TEXTURES, open addressing */
    HwSurface *front, *biggest;          /* shown / largest drawn this frame */
    uint32_t biggest_area;
    uint32_t frame;
    DWORD last_flip_ms;
    ID3D11Texture2D *ovl_tex;
    ID3D11ShaderResourceView *ovl_srv;
    uint32_t ovl_w, ovl_h, ovl_serial;
    HwVertex *verts;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t clear_rect_h, clear_rect_v;
    int clear_rect_set;
    uint64_t draws, tris, tex_uploads;
    uint64_t present_ticks, gpu_vsh_batches, cpu_vsh_batches;
    /* The batch being drawn is 2D that spans the whole width (a fade, the
     * pause menu's fog grid): it keeps the full width in a wide picture. */
    int batch_full_width;
    int batch_projected_screen;          /* RE-SoT: screen quad already narrowed by Hor+ */
    int batch_hud;                       /* RE-SoT: flat upper-left demo HUD */
} s_hw;

/* Shared by every vertex shader: the constants and what a vertex leaves as.
 * vs_finish is the tail of the pipeline -- the program's oPos is already in
 * screen space (pixels, depth-buffer z, clip w), so this only maps it to
 * Direct3D's clip space. Colours are clamped to 0..1 as the NV2A does before
 * interpolating them. With a widened picture (ws.x < 1) a vertex that arrives
 * with w == 1 belongs to the title's 2D: it keeps its 4:3 size around the
 * centre, unless it sits on the surface's edge (full-screen fades and
 * post-processing quads, which must keep covering everything). */
static const char s_hw_hlsl_vs_common[] =
"cbuffer VSC : register(b0) { float4 vp; float4 fogp; float4 ws; float4 vpad; float4 cur[16];\n"
"                             float4 c[192]; };\n"
"struct VO { float4 pos:SV_Position; float4 d0:COLOR0; float4 d1:COLOR1; float4 fog:FOG;\n"
"            float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
"VO vs_finish(float4 p, float4 d0, float4 d1, float fog, float4 t0, float4 t1, float4 t2, float4 t3) {\n"
"  VO o; float w = p.w; float x = p.x, y = p.y;\n"
"  bool flat = abs(w - 1.0) < 0.0001;\n"
"  if (ws.x != 1.0 && flat && x > ws.z && x < ws.y - ws.z)\n"
"    x = (x - 0.5 * ws.y) * ws.x + 0.5 * ws.y;\n"
/* RE-SoT: HUD width is already corrected by the projection. Only translate
 * it to retain the existing left anchor; ws.w is zero for other batches. */
"  if (flat) x += ws.w;\n"
"  o.pos = float4((x * vp.x - 1.0) * w, (1.0 - y * vp.y) * w,\n"
"                 saturate(p.z * vp.z) * w, w);\n"
"  o.d0 = saturate(d0); o.d1 = saturate(d1); o.fog = float4(fog, 0, 0, 0);\n"
"  o.t0 = t0; o.t1 = t1; o.t2 = t2; o.t3 = t3; return o; }\n";

/* A translated vertex program: this, the attribute mapping, the body from
 * nv2a_vsh_emit_hlsl, then s_hw_hlsl_vsp_tail. The helpers follow
 * nv2a_vsh_interp.c's run_ilu and fog_factor above. */
static const char s_hw_hlsl_vsp_head[] =
"struct PI { float4 a0:ATTR0; float4 a1:ATTR1; float4 a2:ATTR2; float4 a3:ATTR3;\n"
"            float4 a4:ATTR4; float4 a5:ATTR5; float4 a6:ATTR6; float4 a7:ATTR7; };\n"
"static const float VSH_INF = asfloat(0x7F800000);\n"
"float vsh_rcp(float x) { return x == 0.0 ? VSH_INF : 1.0 / x; }\n"
"float vsh_rcc(float x) {\n"
"  float f = 1.0 / x;\n"
"  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
"  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
"  return f; }\n"
"float4 vsh_log(float4 cc) {\n"
"  float ax = abs(cc.x); float4 r = cc;\n"
"  if (ax == 0.0) { r.x = -VSH_INF; r.z = -VSH_INF; r.y = 1.0; }\n"
"  else { float e = floor(log2(ax)); r.x = e; r.y = ax / exp2(e); r.z = log2(ax); }\n"
"  r.w = 1.0; return r; }\n"
"float4 vsh_lit(float4 cc) {\n"
"  float nl = max(cc.x, 0.0), nh = max(cc.y, 0.0);\n"
"  float pw = clamp(cc.w, -127.9961, 127.9961);\n"
"  return float4(1.0, nl, cc.x > 0.0 ? pow(nh, pw) : 0.0, 1.0); }\n"
"int vsh_cidx(int i) { return (i < 0 || i >= 192) ? 0 : i; }\n"
"float vsh_fog(float d) {\n"
"  if (fogp.w < 0.5) return 1.0;\n"
"  float f; uint mode = (uint)fogp.z;\n"
"  if (mode == 0x800 || mode == 0x802) f = fogp.x + exp2(d * fogp.y * 16.0) - 1.5;\n"
"  else if (mode == 0x801 || mode == 0x803) f = fogp.x + exp2(-d * d * fogp.y * fogp.y * 32.0) - 1.5;\n"
"  else f = fogp.x + d * fogp.y - 1.0;\n"
"  if (mode == 0x802 || mode == 0x803 || mode == 0x804) f = abs(f);\n"
"  if (f != f) f = 1.0;\n"
"  return saturate(f); }\n"
"VO vs_prog(PI i) {\n"
"  float4 v[16]; float4 R[13]; float4 o[13]; int a0 = 0; int k;\n"
"  for (k = 0; k < 16; k++) v[k] = cur[k];\n"
"  for (k = 0; k < 13; k++) { R[k] = float4(0, 0, 0, 0); o[k] = float4(0, 0, 0, 0); }\n"
"  o[3].w = 1.0; o[4].w = 1.0; o[9].w = 1.0; o[10].w = 1.0; o[11].w = 1.0; o[12].w = 1.0;\n";
static const char s_hw_hlsl_vsp_tail[] =
"  return vs_finish(R[12], o[3], o[4], vsh_fog(o[5].x), o[9], o[10], o[11], o[12]); }\n";

static const char s_hw_hlsl[] =
"struct VI { float4 pos:POSITION; float4 d0:COLOR0; float4 d1:COLOR1; float4 fog:FOG;\n"
"            float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3; };\n"
"VO vs_main(VI i) {\n"
"  return vs_finish(i.pos, i.d0, i.d1, i.fog.x, i.t0, i.t1, i.t2, i.t3); }\n"
"struct BO { float4 pos:SV_Position; float2 uv:TEXCOORD0; };\n"
"BO vs_blit(uint id : SV_VertexID) {\n"
"  BO o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.uv = p; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"Texture2D tx0 : register(t0); Texture2D tx1 : register(t1);\n"
"Texture2D tx2 : register(t2); Texture2D tx3 : register(t3);\n"
"SamplerState sm0 : register(s0); SamplerState sm1 : register(s1);\n"
"SamplerState sm2 : register(s2); SamplerState sm3 : register(s3);\n"
"Texture2D lut : register(t4); SamplerState smL : register(s4);\n"
"float4 ps_blit(BO i) : SV_Target {\n"
"  float3 k = (saturate(tx0.Sample(sm0, i.uv).rgb) * 255.0 + 0.5) / 256.0;\n"
"  return float4(lut.Sample(smL, float2(k.r, 0.5)).r, lut.Sample(smL, float2(k.g, 0.5)).g, lut.Sample(smL, float2(k.b, 0.5)).b, 1.0); }\n"
"cbuffer PSC : register(b0) {\n"
"  uint4 cw[8]; float4 fac0[8]; float4 fac1[8];\n"
"  uint4 ctl; uint4 atest; float4 finc0; float4 finc1; float4 fogc;\n"
"  float4 tscale[4]; uint4 misc; };\n"
"float4 ps_solid(VO i) : SV_Target { return finc0; }\n"
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0.0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p;\n"
"  if (m == 1) return 1.0 - saturate(x);\n"
"  if (m == 2) return 2.0 * p - 1.0;\n"
"  if (m == 3) return 1.0 - 2.0 * p;\n"
"  if (m == 4) return p - 0.5;\n"
"  if (m == 5) return 0.5 - p;\n"
"  if (m == 6) return x;\n"
"  return -x; }\n"
"float3 rd_rgb(float4 r, uint b) {\n"
"  float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"  return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b)); }\n"
"float rd_a(float4 r, uint b) { return map_in((b & 0x10) ? r.a : r.b, b); }\n"
"float3 map_out3(float3 x, uint m) {\n"
"  if (m == 0x08) x = x - 0.5;\n"
"  else if (m == 0x10) x = x * 2.0;\n"
"  else if (m == 0x18) x = (x - 0.5) * 2.0;\n"
"  else if (m == 0x20) x = x * 4.0;\n"
"  else if (m == 0x30) x = x * 0.5;\n"
"  return clamp(x, -1.0, 1.0); }\n"
"float4 stage_tex(uint mode, float4 tc, float4 smp, float bound, inout bool keep, uint cpm) {\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode >= 1 && mode <= 3) return bound > 0.5 ? smp : float4(1, 1, 1, 1);\n"
"  if (mode == 4) return saturate(tc);\n"
"  if (mode == 5) {\n"
"    for (int j = 0; j < 4; j++) {\n"
"      bool ge = ((cpm >> j) & 1) != 0;\n"
"      if (ge ? (tc[j] >= 0.0) : (tc[j] < 0.0)) keep = false; }\n"
"    return float4(0, 0, 0, 0); }\n"
"  return float4(0, 0, 0, 0); }\n"
"float2 proj_uv(float4 tc, float4 sc) { float q = tc.w != 0.0 ? tc.w : 1.0; return tc.xy / q * sc.xy; }\n"
"float4 ps_main(VO i) : SV_Target {\n"
"  float4 s0 = tx0.Sample(sm0, proj_uv(i.t0, tscale[0]));\n"
"  float4 s1 = tx1.Sample(sm1, proj_uv(i.t1, tscale[1]));\n"
"  float4 s2 = tx2.Sample(sm2, proj_uv(i.t2, tscale[2]));\n"
"  float4 s3 = tx3.Sample(sm3, proj_uv(i.t3, tscale[3]));\n"
"  bool keep = true; uint sp = ctl.y; uint cpm = atest.w;\n"
"  float4 R[16];\n"
"  for (int k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[8]  = stage_tex(sp & 31, i.t0, s0, tscale[0].z, keep, cpm);\n"
"  R[9]  = stage_tex((sp >> 5) & 31, i.t1, s1, tscale[1].z, keep, cpm >> 4);\n"
"  R[10] = stage_tex((sp >> 10) & 31, i.t2, s2, tscale[2].z, keep, cpm >> 8);\n"
"  R[11] = stage_tex((sp >> 15) & 31, i.t3, s3, tscale[3].z, keep, cpm >> 12);\n"
"  if (!keep) discard;\n"
"  float4 o;\n"
"  if (misc.x == 0) {\n"
"    o = i.d0; if (tscale[0].z > 0.5) o *= s0;\n"
"  } else {\n"
"    R[3] = float4(fogc.rgb, i.fog.x); R[4] = i.d0; R[5] = i.d1;\n"
"    R[12].a = (sp & 31) ? R[8].a : 1.0;\n"
"    uint n = min(ctl.x & 0xFF, 8u); uint flags = ctl.x >> 8;\n"
"    [loop] for (uint s = 0; s < n; s++) {\n"
"      uint icw = cw[s].x, aicw = cw[s].y, ocw = cw[s].z, aocw = cw[s].w;\n"
"      uint fl = ocw >> 12, afl = aocw >> 12;\n"
"      R[1] = fac0[(flags & 0x010) ? s : 0]; R[2] = fac1[(flags & 0x100) ? s : 0];\n"
"      bool mux = (flags & 1) ? (R[12].a >= 0.5) : ((uint(R[12].a * 255.0) & 1) != 0);\n"
"      float3 A = rd_rgb(R[(icw >> 24) & 15], icw >> 24), B = rd_rgb(R[(icw >> 16) & 15], icw >> 16);\n"
"      float3 C = rd_rgb(R[(icw >> 8) & 15], icw >> 8), D = rd_rgb(R[icw & 15], icw);\n"
"      float aA = rd_a(R[(aicw >> 24) & 15], aicw >> 24), aB = rd_a(R[(aicw >> 16) & 15], aicw >> 16);\n"
"      float aC = rd_a(R[(aicw >> 8) & 15], aicw >> 8), aD = rd_a(R[aicw & 15], aicw);\n"
"      float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"      float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"      float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"      float3 ab = map_out3(pab, fl & 0x38), cd = map_out3(pcd, fl & 0x38), ms = map_out3(sum, fl & 0x38);\n"
"      float qab = aA * aB, qcd = aC * aD;\n"
"      float qsum = (afl & 4) ? (mux ? qcd : qab) : qab + qcd;\n"
"      float aab = map_out3(qab.xxx, afl & 0x38).x, acd = map_out3(qcd.xxx, afl & 0x38).x;\n"
"      float ams = map_out3(qsum.xxx, afl & 0x38).x;\n"
"      uint d;\n"
"      d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }\n"
"      d = ocw & 15;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }\n"
"      d = (ocw >> 8) & 15; if (d) { R[d].rgb = ms; }\n"
"      d = (aocw >> 4) & 15; if (d) R[d].a = aab;\n"
"      d = aocw & 15;        if (d) R[d].a = acd;\n"
"      d = (aocw >> 8) & 15; if (d) R[d].a = ams;\n"
"      R[0] = float4(0, 0, 0, 0);\n"
"    }\n"
"    if (ctl.z != 0 || ctl.w != 0) {\n"
"      uint f0 = ctl.z, f1 = ctl.w, ff = f1 & 0xFF;\n"
"      R[1] = finc0; R[2] = finc1;\n"
"      float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"      float3 b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"      R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0);\n"
"      float3 E = rd_rgb(R[(f1 >> 24) & 15], f1 >> 24), F = rd_rgb(R[(f1 >> 16) & 15], f1 >> 16);\n"
"      R[15] = float4(E * F, 0);\n"
"      float3 A = rd_rgb(R[(f0 >> 24) & 15], f0 >> 24), B = rd_rgb(R[(f0 >> 16) & 15], f0 >> 16);\n"
"      float3 C = rd_rgb(R[(f0 >> 8) & 15], f0 >> 8), D = rd_rgb(R[f0 & 15], f0);\n"
"      o.rgb = D + C * (1.0 - A) + B * A;\n"
"      o.a = rd_a(R[(f1 >> 8) & 15], f1 >> 8);\n"
"    } else o = R[12];\n"
"  }\n"
"  if (misc.y == 1) o = float4(s0.rgb, 1.0); else if (misc.y == 2) o = float4(s1.rgb, 1.0);\n"
"  else if (misc.y == 3) o = float4(s2.rgb, 1.0); else if (misc.y == 4) o = float4(s3.rgb, 1.0);\n"
"  else if (misc.y == 5) o = float4(i.d0.rgb, 1.0); else if (misc.y == 6) o = float4(i.d1.rgb, 1.0);\n"
"  else if (misc.y == 7) o = float4(s0.aaa, 1.0); else if (misc.y == 8) o = float4(R[13].rgb, 1.0);\n"
"  else if (misc.y == 9) o = float4(R[12].rgb, 1.0);\n"
"  else if (misc.y == 10) o = float4(float(misc.z & 255u) / 255.0, float((misc.z >> 8) & 255u) / 255.0, float((misc.z >> 16) & 255u) / 255.0, 1.0);\n"
"  o = saturate(o);\n"
"  if (atest.x != 0) {\n"
"    int v = int(o.a * 255.0 + 0.5), r = int(atest.z & 0xFF); uint f = atest.y; bool ok = true;\n"
"    if (f == 0x200) ok = false; else if (f == 0x201) ok = v < r;\n"
"    else if (f == 0x202) ok = v == r; else if (f == 0x203) ok = v <= r;\n"
"    else if (f == 0x204) ok = v > r; else if (f == 0x205) ok = v != r;\n"
"    else if (f == 0x206) ok = v >= r;\n"
"    if (!ok) discard; }\n"
"  return o; }\n";

/* RE-SoT: compiled shaders are kept on disk (bin\shadercache, or
 * RECOMP_HW_SHADER_CACHE=<dir>; "0" turns it off), keyed by a hash of the
 * HLSL, the entry point and the profile. D3DCompile takes tens of
 * milliseconds per vertex program, and every program the title uses for the
 * first time used to cost a dropped frame or two -- on every run. */
static const char *hw_shader_cache_dir(void)
{
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) {
        dir = getenv("RECOMP_HW_SHADER_CACHE");
        if (!dir)
            dir = "bin\\shadercache";
        if (!*dir || strcmp(dir, "0") == 0)
            dir = NULL;
        else {
            char parent[MAX_PATH]; const char *sl = strrchr(dir, 92);
            if (sl && (size_t)(sl - dir) < sizeof parent) {   /* bin\shadercache: the parent may not exist */
                memcpy(parent, dir, (size_t)(sl - dir)); parent[sl - dir] = 0;
                CreateDirectoryA(parent, NULL);
            }
            CreateDirectoryA(dir, NULL);
        }
    }
    return dir;
}

static void hw_shader_cache_path(char *path, size_t cap, const char *src, size_t len,
                                 const char *entry, const char *target, UINT cflags)
{
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < len; i++)
        h = (h ^ (uint8_t)src[i]) * 1099511628211ull;
    for (i = 0; entry[i]; i++)
        h = (h ^ (uint8_t)entry[i]) * 1099511628211ull;
    for (i = 0; target[i]; i++)
        h = (h ^ (uint8_t)target[i]) * 1099511628211ull;
    if (cflags != (D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS))
        h = (h ^ cflags) * 1099511628211ull;
    snprintf(path, cap, "%s\\%016llx.cso", hw_shader_cache_dir(), (unsigned long long)h);
}

static ID3DBlob *hw_compile_src_f(const char *src, size_t len, const char *entry,
                                  const char *target, UINT cflags)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    char path[MAX_PATH];
    FILE *f;

    if (hw_shader_cache_dir()) {
        hw_shader_cache_path(path, sizeof path, src, len, entry, target, cflags);
        f = fopen(path, "rb");
        if (f) {
            long n;
            fseek(f, 0, SEEK_END);
            n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &code))
                && fread(ID3D10Blob_GetBufferPointer(code), 1, (size_t)n, f) == (size_t)n) {
                fclose(f);
                return code;
            }
            if (code) { ID3D10Blob_Release(code); code = NULL; }
            fclose(f);
        }
    }
    hr = D3DCompile(src, len, "nv2a_hw", NULL, NULL, entry, target, cflags, 0, &code, &err);
    if (SUCCEEDED(hr) && hw_shader_cache_dir()) {
        f = fopen(path, "wb");
        if (f) {
            fwrite(ID3D10Blob_GetBufferPointer(code), 1, ID3D10Blob_GetBufferSize(code), f);
            fclose(f);
        }
    }
    if (FAILED(hr)) {
        fprintf(stderr, "  [HW] shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        fflush(stderr);
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3DBlob *hw_compile_src(const char *src, size_t len, const char *entry,
                                const char *target)
{
    return hw_compile_src_f(src, len, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS);
}

/* A shader already in the disk cache, without ever compiling: NULL when it is not there. */
static ID3DBlob *hw_cache_lookup(const char *src, size_t len, const char *entry, const char *target, UINT cflags)
{
    char path[MAX_PATH];
    ID3DBlob *code = NULL;
    FILE *f;
    long n;
    if (!hw_shader_cache_dir())
        return NULL;
    hw_shader_cache_path(path, sizeof path, src, len, entry, target, cflags);
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &code))
        && fread(ID3D10Blob_GetBufferPointer(code), 1, (size_t)n, f) == (size_t)n) {
        fclose(f);
        return code;
    }
    if (code) ID3D10Blob_Release(code);
    fclose(f);
    return NULL;
}

static ID3DBlob *hw_compile(const char *entry, const char *target)
{
    static char *src;
    static size_t len;
    if (!src) {
        len = sizeof s_hw_hlsl_vs_common - 1 + sizeof s_hw_hlsl - 1;
        src = (char *)malloc(len + 1);
        if (!src)
            return NULL;
        memcpy(src, s_hw_hlsl_vs_common, sizeof s_hw_hlsl_vs_common - 1);
        memcpy(src + sizeof s_hw_hlsl_vs_common - 1, s_hw_hlsl, sizeof s_hw_hlsl);
    }
    return hw_compile_src(src, len, entry, target);
}

/* Host pixels across a guest width, on a surface rendered `sc` times the
 * guest resolution. */
static uint32_t hw_px(uint32_t guest_x, uint32_t sc)
{
    return (uint32_t)((float)guest_x * (float)sc * s_hw.widen + 0.5f);
}

/* RE-SoT: how much finer than the guest a surface is rendered. The display
 * surface (and anything as wide) gets RECOMP_HW_SCALE. Smaller surfaces are
 * the title's post-processing chain (the demo's glow: 640 -> 320 -> 160 ->
 * 80 and back) and shadow buffers; their filters are fixed tap offsets in
 * guest texels, so at twice the resolution each blur covered half the area
 * and left sharp, shifted copies of the scene -- "layers out of register".
 * They stay at the guest's own resolution (RECOMP_HW_FX_SCALE to change). */
static uint32_t hw_surface_scale(uint32_t w)
{
    return w >= 640 ? (uint32_t)s_hw.scale : (uint32_t)s_hw.fx_scale;
}

static void hw_resize_backbuffer(void)
{
    RECT rc;
    ID3D11Texture2D *bb = NULL;

    GetClientRect(s_hw.hwnd, &rc);
    if (rc.right < 16 || rc.bottom < 16)
        return;
    if (s_hw.bb_rtv && (uint32_t)rc.right == s_hw.bb_w
        && (uint32_t)rc.bottom == s_hw.bb_h)
        return;
    if (s_hw.bb_rtv) {
        ID3D11DeviceContext_OMSetRenderTargets(s_hw.ctx, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(s_hw.bb_rtv);
        s_hw.bb_rtv = NULL;
        IDXGISwapChain_ResizeBuffers(s_hw.swap, 0, (UINT)rc.right,
                                     (UINT)rc.bottom, DXGI_FORMAT_UNKNOWN,
                                     s_hw.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    }
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_hw.swap, 0, &IID_ID3D11Texture2D,
                                           (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(s_hw.dev, (ID3D11Resource *)bb,
                                            NULL, &s_hw.bb_rtv);
        ID3D11Texture2D_Release(bb);
    }
    s_hw.bb_w = (uint32_t)rc.right;
    s_hw.bb_h = (uint32_t)rc.bottom;
}

static int hw_init(void)
{
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    DXGI_SWAP_CHAIN_DESC sd;
    D3D_FEATURE_LEVEL fl;
    ID3DBlob *b;
    HRESULT hr;
    int i, wait;
    RECT wr;
    const char *e;

    extern void xbox_FramebufferWindowStart(void);
    xbox_FramebufferWindowStart();
    for (wait = 0; wait < 300 && !xbox_FramebufferWindowHandle(); wait++)
        Sleep(10);
    s_hw.hwnd = (HWND)xbox_FramebufferWindowHandle();
    if (!s_hw.hwnd) {
        fprintf(stderr, "  [HW] no window (RECOMP_FB_WINDOW not set?); software path\n");
        return 0;
    }
    e = getenv("RECOMP_HW_SCALE");
    s_hw.scale = e ? atoi(e) : 2;
    s_hw.auto_scale = (e == NULL);
    if (s_hw.scale < 1) s_hw.scale = 1;
    if (s_hw.scale > 8) s_hw.scale = 8;
    e = getenv("RECOMP_HW_FX_SCALE");
    s_hw.fx_scale = e ? atoi(e) : 1;
    if (s_hw.fx_scale < 1) s_hw.fx_scale = 1;
    if (s_hw.fx_scale > s_hw.scale) s_hw.fx_scale = s_hw.scale;
    s_hw.nocull = getenv("RECOMP_HW_NOCULL") != NULL;
    s_hw.cpu_vsh = getenv("RECOMP_HW_CPU_VSH") != NULL;
    e = getenv("RECOMP_HW_VSYNC");
    s_hw.vsync = e ? atoi(e) != 0 : 1;
    s_hw.aspect = 4.0f / 3.0f;
    e = getenv("RECOMP_HW_ASPECT");
    if (e && *e) {
        const char *colon = strchr(e, ':');
        float a = (float)atof(e);
        if (colon && atof(colon + 1) > 0.0)
            a /= (float)atof(colon + 1);
        if (a >= 1.0f && a <= 4.0f)
            s_hw.aspect = a;
    }
    s_hw.widen = s_hw.aspect / (4.0f / 3.0f);
    if (s_hw.widen < 1.0f) s_hw.widen = 1.0f;

    e = getenv("RECOMP_HW_FULLSCREEN");          /* RE-SoT: "0" is a window */
    if (e && *e && strcmp(e, "0") != 0) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoA(MonitorFromWindow(s_hw.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtrA(s_hw.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(s_hw.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
    } else {
        /* A window large enough to be worth rendering above guest resolution:
         * 1280x960 at 4:3, the same width and less height when wider. */
        wr.left = 0; wr.top = 0; wr.right = 1280;
        wr.bottom = (LONG)(1280.0f / s_hw.aspect + 0.5f);
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(s_hw.hwnd, NULL, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    {
        /* Adapter: RECOMP_GPU = high (default: the high-performance GPU, the RTX on a
         * hybrid laptop), default (the OS / DXGI default), or a 0-based adapter index.
         * Falls back to the default adapter on any failure. */
        IDXGIFactory1 *fac = NULL;
        IDXGIAdapter1 *ad = NULL;
        const char *g = getenv("RECOMP_GPU");
        int pref_high = !(g && !_stricmp(g, "default"));
        int idx = (g && g[0] >= '0' && g[0] <= '9') ? atoi(g) : -1;
        int want_tear = 0;
        DXGI_ADAPTER_DESC1 adesc;
        char name[128];

        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) {
            IDXGIFactory6 *f6 = NULL;
            UINT k;
            for (k = 0; ; k++) {   /* list everything once, so the log shows the choice */
                IDXGIAdapter1 *a1 = NULL;
                if (FAILED(IDXGIFactory1_EnumAdapters1(fac, k, &a1))) break;
                IDXGIAdapter1_GetDesc1(a1, &adesc);
                WideCharToMultiByte(CP_ACP, 0, adesc.Description, -1, name, sizeof name, NULL, NULL);
                fprintf(stderr, "  [HW] adapter #%u: %s (%u MB VRAM, flags %u)\n", k, name,
                        (unsigned)(adesc.DedicatedVideoMemory >> 20), (unsigned)adesc.Flags);
                IDXGIAdapter1_Release(a1);
            }
            if (idx >= 0) {
                IDXGIFactory1_EnumAdapters1(fac, (UINT)idx, &ad);
            } else if (pref_high && SUCCEEDED(IDXGIFactory1_QueryInterface(fac, &IID_IDXGIFactory6, (void **)&f6))) {
                IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         &IID_IDXGIAdapter1, (void **)&ad);
                IDXGIFactory6_Release(f6);
            }
            {
                IDXGIFactory5 *f5 = NULL;
                BOOL t = FALSE;
                e = getenv("RECOMP_HW_TEARING");
                if ((!e || atoi(e) != 0) &&
                    SUCCEEDED(IDXGIFactory1_QueryInterface(fac, &IID_IDXGIFactory5, (void **)&f5))) {
                    if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t)))
                        want_tear = t ? 1 : 0;
                    IDXGIFactory5_Release(f5);
                }
            }
        }
        memset(&sd, 0, sizeof sd);
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = s_hw.hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = want_tear ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
        sd.Flags = want_tear ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        hr = E_FAIL;
        if (fac && ad) {
            hr = D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &s_hw.dev, &fl, &s_hw.ctx);
            if (SUCCEEDED(hr)) {
                hr = IDXGIFactory1_CreateSwapChain(fac, (IUnknown *)s_hw.dev, &sd, &s_hw.swap);
                if (FAILED(hr)) { /* flip model refused: the old model on the same device */
                    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.Flags = 0; want_tear = 0;
                    hr = IDXGIFactory1_CreateSwapChain(fac, (IUnknown *)s_hw.dev, &sd, &s_hw.swap);
                }
            }
            if (FAILED(hr)) {
                if (s_hw.ctx) { ID3D11DeviceContext_Release(s_hw.ctx); s_hw.ctx = NULL; }
                if (s_hw.dev) { ID3D11Device_Release(s_hw.dev); s_hw.dev = NULL; }
                s_hw.swap = NULL;
                fprintf(stderr, "  [HW] chosen adapter failed 0x%08lX; default adapter\n", hr);
            }
        }
        if (FAILED(hr)) {
            want_tear = 0; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.Flags = 0;
            hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                               NULL, 0, D3D11_SDK_VERSION, &sd,
                                               &s_hw.swap, &s_hw.dev, &fl, &s_hw.ctx);
        }
        if (SUCCEEDED(hr)) {
            IDXGIDevice *dd = NULL; IDXGIAdapter *da = NULL; DXGI_ADAPTER_DESC d0;
            if (SUCCEEDED(ID3D11Device_QueryInterface(s_hw.dev, &IID_IDXGIDevice, (void **)&dd))) {
                if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &da))) {
                    IDXGIAdapter_GetDesc(da, &d0);
                    WideCharToMultiByte(CP_ACP, 0, d0.Description, -1, name, sizeof name, NULL, NULL);
                    fprintf(stderr, "  [HW] adapter: %s (%u MB VRAM); swap effect %s, tearing %d\n", name,
                            (unsigned)(d0.DedicatedVideoMemory >> 20),
                            sd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ? "flip" : "blt", want_tear);
                    IDXGIAdapter_Release(da);
                }
                IDXGIDevice_Release(dd);
            }
        }
        s_hw.tearing = want_tear;
        if (ad) IDXGIAdapter1_Release(ad);
        if (fac) IDXGIFactory1_Release(fac);
        e = getenv("RECOMP_HW_PACE");
        s_hw.pace_hz = e ? atof(e) : 60.0;
        /* Own pacing replaces DXGI's vsync wait only when tearing presents are on. */
        if (!s_hw.tearing) s_hw.pace_hz = 0;
    }
    if (FAILED(hr)) {
        fprintf(stderr, "  [HW] D3D11CreateDeviceAndSwapChain failed 0x%08lX\n", hr);
        return 0;
    }
    hw_resize_backbuffer();

    if (!(b = hw_compile("vs_main", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &s_hw.vs);
    ID3D11Device_CreateInputLayout(s_hw.dev, il, 8, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), &s_hw.layout);
    ID3D10Blob_Release(b);
    if (!(b = hw_compile("vs_blit", "vs_4_0"))) return 0;
    ID3D11Device_CreateVertexShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                    ID3D10Blob_GetBufferSize(b), NULL, &s_hw.vs_blit);
    ID3D10Blob_Release(b);
    if (!(b = hw_compile("ps_main", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &s_hw.ps);
    ID3D10Blob_Release(b);
    if (!(b = hw_compile("ps_blit", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &s_hw.ps_blit);
    ID3D10Blob_Release(b);
    if (!(b = hw_compile("ps_solid", "ps_4_0"))) return 0;
    ID3D11Device_CreatePixelShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                   ID3D10Blob_GetBufferSize(b), NULL, &s_hw.ps_solid);
    ID3D10Blob_Release(b);
    if (!s_hw.vs || !s_hw.ps || !s_hw.layout || !s_hw.vs_blit || !s_hw.ps_blit
        || !s_hw.ps_solid)
        return 0;

    {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        /* Rings: a batch is appended with NO_OVERWRITE and the buffer is
         * only discarded when it wraps. Discarding a buffer sized for the
         * largest batch on every draw made the driver hand out megabytes of
         * fresh memory hundreds of times a frame. */
        bd.ByteWidth = HW_VB_BYTES;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateBuffer(s_hw.dev, &bd, NULL, &s_hw.vb);
        bd.ByteWidth = HW_IB_BYTES;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ID3D11Device_CreateBuffer(s_hw.dev, &bd, NULL, &s_hw.ib);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.ByteWidth = sizeof(HwVsConst);
        ID3D11Device_CreateBuffer(s_hw.dev, &bd, NULL, &s_hw.vs_cb);
        bd.ByteWidth = (sizeof(HwPsConst) + 15) & ~15u;
        ID3D11Device_CreateBuffer(s_hw.dev, &bd, NULL, &s_hw.ps_cb);
    }
    {
        D3D11_RASTERIZER_DESC rd;
        static const D3D11_CULL_MODE modes[3] = {
            D3D11_CULL_NONE, D3D11_CULL_BACK, D3D11_CULL_FRONT };
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.DepthClipEnable = FALSE;
        rd.ScissorEnable = TRUE;
        for (i = 0; i < 3; i++) {
            rd.CullMode = modes[i];
            rd.FrontCounterClockwise = FALSE;
            ID3D11Device_CreateRasterizerState(s_hw.dev, &rd, &s_hw.rs[i]);
            rd.FrontCounterClockwise = TRUE;
            ID3D11Device_CreateRasterizerState(s_hw.dev, &rd, &s_hw.rs_ccw[i]);
        }
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = FALSE;
        ID3D11Device_CreateRasterizerState(s_hw.dev, &rd, &s_hw.rs_blit);
    }
    {
        D3D11_SAMPLER_DESC sm;
        memset(&sm, 0, sizeof sm);
        sm.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_hw.dev, &sm, &s_hw.samp_blit);
    }
    {
        /* What an unusable stage samples: opaque white. */
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init;
        ID3D11Texture2D *t = NULL;
        static const uint32_t px = 0xFFFFFFFFu;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = &px; init.SysMemPitch = 4; init.SysMemSlicePitch = 4;
        ID3D11Device_CreateTexture2D(s_hw.dev, &td, &init, &t);
        if (t) {
            ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)t,
                                                  NULL, &s_hw.white);
            ID3D11Texture2D_Release(t);
        }
    }
    s_hw.texs = (HwTexture *)calloc(HW_MAX_TEXTURES, sizeof(HwTexture));
    s_hw.verts = (HwVertex *)malloc(sizeof(HwVertex) * HW_MAX_VERTS);
    if (!s_hw.texs || !s_hw.verts || !s_hw.vb || !s_hw.ib || !s_hw.white)
        return 0;
    s_hw.cull_face = 0x405;
    s_hw.front_face = 0x900;
    xbox_FramebufferWindowHwMode(1);
    fprintf(stderr, "  [HW] Direct3D 11 rasteriser up (feature level 0x%X, scale %dx,"
                    " aspect %.3f, vsync %d, vertex programs on the %s)\n",
            (unsigned)fl, s_hw.scale, s_hw.aspect, s_hw.vsync,
            s_hw.cpu_vsh ? "CPU" : "GPU");
    fflush(stderr);
    return 1;
}

/* Is the hardware path in use? Tries to bring it up on the first call that
 * finds a window, and gives a single answer for the rest of the run. */
static int hw_active(void)
{
    if (!s_hw.tried) {
        s_hw.tried = 1;
        InitializeCriticalSection(&s_hw.lock);
        if (getenv("RECOMP_HW"))
            s_hw.ok = hw_init();
    }
    return s_hw.ok;
}

static uint32_t hw_zmax_bits(void)
{
    return ((s_gpu.format >> 4) & 0xF) == 1 ? 0xFFFFu : 0xFFFFFFu;
}

/* The colour surface the title is drawing into. */
static HwSurface *hw_surface(void)
{
    uint32_t bpp = surface_bpp(), off, w, h, i;
    HwSurface *s = NULL, *oldest = &s_hw.surf[0];
    D3D11_TEXTURE2D_DESC td;

    if (!s_gpu.color_offset || !bpp || !s_gpu.pitch)
        return NULL;
    off = dma_resolve(s_gpu.color_offset);
    w = s_gpu.pitch / bpp;
    h = s_gpu.clip_y + s_gpu.clip_h;
    if (!w || !h || w > 4096 || h > 4096)
        return NULL;
    for (i = 0; i < HW_MAX_SURFACES; i++) {
        HwSurface *c = &s_hw.surf[i];
        if (c->tex && c->offset == off && c->w == w) {
            if (c->h >= h)
                return c;
            s = c;                      /* same surface, taller than thought */
            break;
        }
        if (!c->tex) { oldest = c; continue; }
        if (oldest->tex && c->drawn_frame < oldest->drawn_frame)
            oldest = c;
    }
    if (!s)
        s = oldest;
    if (s->tex) {
        if (s_hw.front == s) s_hw.front = NULL;
        if (s_hw.biggest == s) s_hw.biggest = NULL;
        ID3D11ShaderResourceView_Release(s->srv);
        ID3D11RenderTargetView_Release(s->rtv);
        ID3D11Texture2D_Release(s->tex);
        memset(s, 0, sizeof *s);
    }
    memset(&td, 0, sizeof td);
    td.Width = hw_px(w, hw_surface_scale(w));
    td.Height = h * hw_surface_scale(w);
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &s->tex)))
        return NULL;
    ID3D11Device_CreateRenderTargetView(s_hw.dev, (ID3D11Resource *)s->tex, NULL, &s->rtv);
    ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)s->tex, NULL, &s->srv);
    {
        static const float black[4] = {0, 0, 0, 1};
        ID3D11DeviceContext_ClearRenderTargetView(s_hw.ctx, s->rtv, black);
    }
    s->offset = off; s->w = w; s->h = h; s->sc = hw_surface_scale(w);
    s->drawn_frame = s_hw.frame;
    fprintf(stderr, "  [HW] surface 0x%08X %ux%u x%u (format %08X pitch %08X clip %ux%u+%u+%u mask %08X zeta %08X)\n",
            off, w, h, s->sc, s_gpu.format, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.color_mask, s_gpu.zeta_offset);
    return s;
}

static HwDepth *hw_depth(const HwSurface *s)
{
    uint32_t off = s_gpu.zeta_offset, i;
    HwDepth *d = NULL;
    D3D11_TEXTURE2D_DESC td;
    static uint32_t next;

    if (!off)
        return NULL;
    for (i = 0; i < HW_MAX_DEPTHS; i++)
        if (s_hw.depth[i].tex && s_hw.depth[i].offset == off
            && s_hw.depth[i].w == s->w && s_hw.depth[i].h == s->h)
            return &s_hw.depth[i];
    for (i = 0; i < HW_MAX_DEPTHS && !d; i++)
        if (!s_hw.depth[i].tex)
            d = &s_hw.depth[i];
    if (!d) {
        d = &s_hw.depth[next++ % HW_MAX_DEPTHS];
        ID3D11DepthStencilView_Release(d->dsv);
        ID3D11Texture2D_Release(d->tex);
        memset(d, 0, sizeof *d);
    }
    memset(&td, 0, sizeof td);
    td.Width = hw_px(s->w, s->sc);
    td.Height = s->h * s->sc;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &d->tex)))
        return NULL;
    ID3D11Device_CreateDepthStencilView(s_hw.dev, (ID3D11Resource *)d->tex, NULL, &d->dsv);
    ID3D11DeviceContext_ClearDepthStencilView(s_hw.ctx, d->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    d->offset = off; d->w = s->w; d->h = s->h;
    return d;
}

/* A cheap fingerprint of a texture's bytes: enough to notice a texture being
 * rewritten in place (streamed mips, a video frame) without reading all of
 * it for every batch. Checked once per frame per texture. */
static uint32_t hw_tex_bytes(const Texture *t)
{
    uint32_t bb = d3d8_format_dxt_block_bytes(t->color);
    if (bb)
        return ((t->width + 3) / 4) * ((t->height + 3) / 4) * bb;
    if (tex_size_from_format(t->color))
        return t->width * t->height * tex_texel_bytes(t->color);
    return t->pitch * t->height;
}

static uint32_t hw_tex_hash(const Texture *t)
{
    const uint8_t *p = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    uint32_t n = hw_tex_bytes(t) / 4, step = n > 2048 ? n / 2048 : 1, i;
    uint32_t h = 2166136261u;
    for (i = 0; i < n; i += step)
        h = (h ^ ((const uint32_t *)p)[i]) * 16777619u;
    if (t->palette)
        for (i = 0; i < 256; i += 8)
            h = (h ^ ((const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset()
                                         + t->palette))[i]) * 16777619u;
    return h;
}

static int hw_mem_ok(uint32_t va, uint32_t bytes)
{
    /* Everything a texture can name: the image and heap, or the contiguous
     * window and its tiled alias. */
    if (va >= 0x00010000u && va < 0x04000000u && bytes <= 0x04000000u - va)
        return 1;
    if (va >= 0x80000000u && va < 0x84000000u && bytes <= 0x84000000u - va)
        return 1;
    return 0;
}

/* A draw that samples the render target it is writing (the title's glow chain
 * does this per pixel, which the NV2A and the software path allow) would be a
 * read/write hazard in D3D11, and the path returned NULL, so the shader read
 * the white fallback texture and the whole frame whited out. Sample a copy of
 * the target taken just before the draw instead. */
static void hw_gamma_update(void);
static struct { ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; UINT w, h; } s_hw_shadow[HW_MAX_SURFACES];

static ID3D11ShaderResourceView *hw_self_shadow(const HwSurface *best)
{
    static int off = -1;
    size_t idx = (size_t)(best - s_hw.surf);
    D3D11_TEXTURE2D_DESC td;

    if (off < 0) off = getenv("RECOMP_HW_NOSHADOW") != NULL;
    if (off || idx >= HW_MAX_SURFACES || !best->tex)
        return NULL;
    ID3D11Texture2D_GetDesc(best->tex, &td);
    if (s_hw_shadow[idx].tex && (s_hw_shadow[idx].w != td.Width || s_hw_shadow[idx].h != td.Height)) {
        ID3D11ShaderResourceView_Release(s_hw_shadow[idx].srv);
        ID3D11Texture2D_Release(s_hw_shadow[idx].tex);
        memset(&s_hw_shadow[idx], 0, sizeof s_hw_shadow[idx]);
    }
    if (!s_hw_shadow[idx].tex) {
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &s_hw_shadow[idx].tex)))
            return NULL;
        ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)s_hw_shadow[idx].tex,
                                              NULL, &s_hw_shadow[idx].srv);
        s_hw_shadow[idx].w = td.Width; s_hw_shadow[idx].h = td.Height;
    }
    ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)s_hw_shadow[idx].tex,
                                     (ID3D11Resource *)best->tex);
    return s_hw_shadow[idx].srv;
}

static ID3D11ShaderResourceView *hw_texture(int stage, const HwSurface *target)
{
    const Texture *t = &s_gpu.texs[stage];
    HwTexture *c = NULL, *victim = NULL;
    uint32_t i, slot, hash, x, y, *px;
    D3D11_TEXTURE2D_DESC td;
    int mips;

    if (!t->valid || t->width > 4096 || t->height > 4096)
        return NULL;
    /* A render target used as a texture. */
    /* The same guest memory can have been drawn into under more than one
     * shape (the demo's glow chain reuses 0x806C0000 as 640x480 and as
     * 320x240): take the one whose width the stage declares, else the one
     * drawn into most recently. */
    {
        HwSurface *best = NULL;
        for (i = 0; i < HW_MAX_SURFACES; i++) {
            HwSurface *c2 = &s_hw.surf[i];
            if (!c2->tex || c2->offset != t->offset)
                continue;
            if (!best || (c2->w == t->width && best->w != t->width)
                || ((c2->w == t->width) == (best->w == t->width)
                    && c2->drawn_frame > best->drawn_frame))
                best = c2;
        }
        /* A surface registered at this address long ago, under a different width, is not
         * what the title is sampling: the memory was reused (Phantom Dust's second movie
         * buffer at 0x02589000 sat on an old 512x512 target, so every other movie frame
         * showed that stale target, i.e. black). Only a width match, or a surface drawn
         * within the last few frames, counts as a render-to-texture alias. */
        if (best && best->w != t->width && s_hw.frame > best->drawn_frame + 4 && !getenv("RECOMP_HW_ALIAS_OLD"))
            best = NULL;
        if (best) {
            static int dbg = -1; if (dbg < 0) dbg = getenv("RECOMP_HW_TEXDBG") ? 1 : 0;
            if (dbg && GetFileAttributesA("presdbg.flag") != INVALID_FILE_ATTRIBUTES)
                fprintf(stderr, "  [TEX] frame=%u off=%08X ALIAS-SURFACE %ux%u drawn_frame=%u (tex %ux%u fmt=%X)%c", (unsigned)s_hw.frame, (unsigned)t->offset,
                        (unsigned)best->w, (unsigned)best->h, (unsigned)best->drawn_frame, (unsigned)t->width, (unsigned)t->height, (unsigned)t->color, 10);
        }
        if (best)
            return best == target ? hw_self_shadow(best) : best->srv;
    }
    if (!hw_mem_ok(t->offset, hw_tex_bytes(t)))
        return NULL;

    slot = (t->offset >> 6) * 2654435761u >> 22;      /* 10 bits */
    for (i = 0; i < 16; i++) {
        HwTexture *e = &s_hw.texs[(slot + i) & (HW_MAX_TEXTURES - 1)];
        if (e->srv && e->offset == t->offset && e->fmt == t->color
            && e->w == t->width && e->h == t->height && e->pitch == t->pitch
            && e->palette == t->palette) {
            c = e;
            break;
        }
        if (!victim || !e->srv || (victim->srv && e->used_frame < victim->used_frame))
            victim = e;
    }
    {   /* RECOMP_HW_TEXDBG (armed by presdbg.flag): log every texture lookup that reaches the cache */
        static int dbg = -1, armed; static unsigned n;
        if (dbg < 0) dbg = getenv("RECOMP_HW_TEXDBG") ? 1 : 0;
        if (dbg && (++n & 31u) == 0) armed = GetFileAttributesA("presdbg.flag") != INVALID_FILE_ATTRIBUTES;
        if (dbg && armed) {
            fprintf(stderr, "  [TEX] frame=%u off=%08X fmt=%X %ux%u pitch=%u %s%c", (unsigned)s_hw.frame, (unsigned)t->offset, (unsigned)t->color,
                    (unsigned)t->width, (unsigned)t->height, (unsigned)t->pitch,
                    !c ? "new" : (c->checked_frame == s_hw.frame ? "cached" : "recheck"), 10);
        }
    }
    if (c && c->checked_frame == s_hw.frame) {
        c->used_frame = s_hw.frame;
        return c->srv;
    }
    hash = hw_tex_hash(t);
    { static int dbg2 = -1; if (dbg2 < 0) dbg2 = getenv("RECOMP_HW_TEXDBG") ? 1 : 0; if (dbg2 && GetFileAttributesA("presdbg.flag") != INVALID_FILE_ATTRIBUTES) fprintf(stderr, "  [TEXH] off=%08X hash=%08X old=%08X%c", (unsigned)t->offset, (unsigned)hash, c ? (unsigned)c->hash : 0u, 10); }
    if (c && c->hash == hash) {
        c->checked_frame = c->used_frame = s_hw.frame;
        return c->srv;
    }
    if (!c) {
        c = victim;
        if (c->srv) {
            ID3D11ShaderResourceView_Release(c->srv);
            ID3D11Texture2D_Release(c->tex);
        }
        memset(c, 0, sizeof *c);
    }

    px = (uint32_t *)malloc((size_t)t->width * t->height * 4);
    if (!px)
        return NULL;
    for (y = 0; y < t->height; y++)
        for (x = 0; x < t->width; x++) {
            uint32_t argb = 0xFFFFFFFFu;
            if (!sample_tex(t, 0, x, y, &argb))
                argb = 0xFFFFFFFFu;
            px[(size_t)y * t->width + x] = argb;
        }
    mips = t->levels > 1 && tex_size_from_format(t->color);
    if (!c->tex) {
        memset(&td, 0, sizeof td);
        td.Width = t->width; td.Height = t->height;
        td.MipLevels = mips ? 0 : 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0);
        td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &c->tex))) {
            free(px);
            memset(c, 0, sizeof *c);
            return NULL;
        }
        ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)c->tex,
                                              NULL, &c->srv);
    }
    ID3D11DeviceContext_UpdateSubresource(s_hw.ctx, (ID3D11Resource *)c->tex, 0,
                                          NULL, px, t->width * 4, 0);
    if (mips)
        ID3D11DeviceContext_GenerateMips(s_hw.ctx, c->srv);
    free(px);
    c->offset = t->offset; c->fmt = t->color; c->w = t->width; c->h = t->height;
    c->pitch = t->pitch; c->palette = t->palette;
    c->hash = hash;
    c->checked_frame = c->used_frame = s_hw.frame;
    s_hw.tex_uploads++;
    return c->srv;
}

static ID3D11SamplerState *hw_sampler(const Texture *t)
{
    static const D3D11_TEXTURE_ADDRESS_MODE am[6] = {
        D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_WRAP,
        D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_CLAMP };
    uint32_t u = t->addr_u < 6 ? t->addr_u : 3, v = t->addr_v < 6 ? t->addr_v : 3;
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int lin = mag == 2 || min == 2 || min == 4 || min == 6 || min == 3 || min == 5;

    if (!s_hw.samp[u][v][lin]) {
        D3D11_SAMPLER_DESC sm;
        memset(&sm, 0, sizeof sm);
        sm.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sm.AddressU = am[u]; sm.AddressV = am[v];
        sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_hw.dev, &sm, &s_hw.samp[u][v][lin]);
    }
    return s_hw.samp[u][v][lin];
}

static D3D11_BLEND hw_blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static ID3D11BlendState *hw_blend_state(void)
{
    int enable = s_gpu.blend_enable
              && !(s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 0);
    uint32_t mask = 0;
    uint64_t key;
    D3D11_BLEND_DESC bd;
    int i;

    if (s_gpu.color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (s_gpu.color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (s_gpu.color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (s_gpu.color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    key = (uint64_t)mask | ((uint64_t)enable << 4);
    if (enable)
        key |= ((uint64_t)(s_gpu.blend_sfactor & 0xFFFF) << 8)
             | ((uint64_t)(s_gpu.blend_dfactor & 0xFFFF) << 24)
             | ((uint64_t)(s_gpu.blend_equation & 0xFFFF) << 40);
    for (i = 0; i < s_hw.nblend; i++)
        if (s_hw.blend[i].key == key)
            return s_hw.blend[i].st;
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = enable;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    bd.RenderTarget[0].SrcBlend = hw_blend_factor(s_gpu.blend_sfactor, 0);
    bd.RenderTarget[0].DestBlend = hw_blend_factor(s_gpu.blend_dfactor, 0);
    bd.RenderTarget[0].SrcBlendAlpha = hw_blend_factor(s_gpu.blend_sfactor, 1);
    bd.RenderTarget[0].DestBlendAlpha = hw_blend_factor(s_gpu.blend_dfactor, 1);
    switch (s_gpu.blend_equation) {
    case 0x800A: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT; break;
    case 0x800B: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_SUBTRACT; break;
    case 0x8007: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MIN; break;
    case 0x8008: bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX; break;
    default:     bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; break;
    }
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    if (s_hw.nblend == 64)
        s_hw.nblend = 0;                 /* recycle: states are tiny */
    i = s_hw.nblend++;
    if (s_hw.blend[i].st)
        ID3D11BlendState_Release(s_hw.blend[i].st);
    s_hw.blend[i].st = NULL;
    ID3D11Device_CreateBlendState(s_hw.dev, &bd, &s_hw.blend[i].st);
    s_hw.blend[i].key = key;
    return s_hw.blend[i].st;
}

/* NV2A stencil compare function and ops are GL enums. */
static D3D11_STENCIL_OP hw_stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;       /* INCR_WRAP */
    case 0x8508: return D3D11_STENCIL_OP_DECR;       /* DECR_WRAP */
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

/* Stencil is live only with a depth/stencil target bound. RECOMP_HW_NOSTENCIL=1
 * restores the old behaviour (stencil ignored) for A/B comparison. */
static int hw_stencil_active(int have_depth)
{
    static int off = -1;
    if (off < 0) off = getenv("RECOMP_HW_NOSTENCIL") != NULL;
    return !off && have_depth && s_gpu.stencil_enable;
}

static ID3D11DepthStencilState *hw_depth_state(int have_depth)
{
    uint32_t test = have_depth && s_gpu.depth_test;
    uint32_t write = test && s_gpu.depth_mask;
    int sten = hw_stencil_active(have_depth);
    uint64_t key = test | (write << 1) | ((s_gpu.depth_func & 7) << 2);
    D3D11_DEPTH_STENCIL_DESC dd;
    int i;

    if (sten)
        key |= (1ull << 8) | ((uint64_t)(s_gpu.stencil_func & 7) << 9)
             | ((uint64_t)(s_gpu.stencil_fmask & 0xFF) << 12) | ((uint64_t)(s_gpu.stencil_wmask & 0xFF) << 20)
             | ((uint64_t)hw_stencil_op(s_gpu.stencil_op_fail) << 28)
             | ((uint64_t)hw_stencil_op(s_gpu.stencil_op_zfail) << 32)
             | ((uint64_t)hw_stencil_op(s_gpu.stencil_op_zpass) << 36);
    for (i = 0; i < s_hw.nds; i++)
        if (s_hw.ds[i].key == key)
            return s_hw.ds[i].st;
    memset(&dd, 0, sizeof dd);
    if (sten) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = (UINT8)(s_gpu.stencil_fmask & 0xFF);
        dd.StencilWriteMask = (UINT8)(s_gpu.stencil_wmask & 0xFF);
        dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc =
            (D3D11_COMPARISON_FUNC)((s_gpu.stencil_func & 7) + 1);
        dd.FrontFace.StencilFailOp = dd.BackFace.StencilFailOp = hw_stencil_op(s_gpu.stencil_op_fail);
        dd.FrontFace.StencilDepthFailOp = dd.BackFace.StencilDepthFailOp = hw_stencil_op(s_gpu.stencil_op_zfail);
        dd.FrontFace.StencilPassOp = dd.BackFace.StencilPassOp = hw_stencil_op(s_gpu.stencil_op_zpass);
    }
    /* RE-SoT: disabling depth testing disables depth writes too. Forcing
     * ALWAYS when only DEPTH_MASK is set lets the pause fog grids (z=0.95)
     * overwrite the depth buffer and reject the Prince's later alpha-only
     * silhouette (z about 0.97), even though the game disabled depth testing. */
    dd.DepthEnable = test;
    dd.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = test ? (D3D11_COMPARISON_FUNC)((s_gpu.depth_func & 7) + 1)
                        : D3D11_COMPARISON_ALWAYS;
    i = s_hw.nds < 32 ? s_hw.nds++ : 0;
    if (s_hw.ds[i].st)
        ID3D11DepthStencilState_Release(s_hw.ds[i].st);
    s_hw.ds[i].st = NULL;
    ID3D11Device_CreateDepthStencilState(s_hw.dev, &dd, &s_hw.ds[i].st);
    s_hw.ds[i].key = key;
    return s_hw.ds[i].st;
}

static void hw_color4(uint32_t argb, float out[4])
{
    nv2a_rc_unpack(argb, out);
}

/* Bind everything a batch needs and return the surface, or NULL. prog is the
 * translated vertex program to run, or NULL for vertices the interpreter has
 * already transformed. */
/* Debug switches, looked up once: RECOMP_HW_GLOWDBG, RECOMP_HW_ATDBG, and
 * RECOMP_HW_DBG (live per-texture shader override read from hw_dbg.txt:
 * "<texture offset hex> <mode>", mode 1-9 = show an input, 10 = flat colour). */
static int hw_dbg_env(const char *name, int slot)
{
    static int v[4] = { -1, -1, -1, -1 };
    if (v[slot] < 0) v[slot] = getenv(name) != NULL;
    return v[slot];
}


/* ---- Specialised pixel shaders ------------------------------------------------------------------
 * ps_main interprets the register-combiner words at run time: the register file is an indexable
 * temp array, every stage is decoded per pixel, and four textures are sampled whatever the stage
 * program says. On a power-limited laptop GPU that interpreter is the frame cost (hub at 2x: 6 FPS
 * from the pixel stage alone). A "variant" is the same source with the draw's combiner state turned
 * into compile-time constants (cw, ctl, alpha-test mode, texture-bound flags), so the loop unrolls,
 * the register indices resolve and unused samples disappear. Variants are compiled on a worker
 * thread (and kept in the shader cache on disk); until one is ready the draw uses the interpreter.
 * RECOMP_HW_PSSPEC=0 turns this off; a file psspec_off.flag in the working directory does the same
 * at run time (RECOMP_PERF=1 polls it). */
#define HW_PSV_SIZE 2048
typedef struct {
    uint32_t cw[8][4];
    uint32_t ctl[4];
    uint32_t ax, ay, mx, my, bound;
} HwPsSpec;
typedef struct {
    uint64_t key;
    volatile LONG state;                /* 0 empty, 1 queued, 2 ready, 3 failed */
    ID3D11PixelShader *volatile ps;
    HwPsSpec spec;
} HwPsv;
static HwPsv s_hw_psv[HW_PSV_SIZE];
static int s_hw_psv_n;
static volatile int s_hw_psspec_off;
static int s_hw_psspec_env = -1;
static HANDLE s_hw_psv_sem;
static CRITICAL_SECTION s_hw_psv_cs;
static int s_hw_psv_q[HW_PSV_SIZE], s_hw_psv_qh, s_hw_psv_qt;
static volatile LONG s_hw_psv_ready, s_hw_psv_failed, s_hw_psv_fb;   /* fb: draws that ran on the interpreter */

static char *hw_str_replace_all(const char *src, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), n = 0, cap;
    const char *q;
    char *out, *o;
    for (q = src; (q = strstr(q, from)) != NULL; q += fl)
        n++;
    cap = strlen(src) + n * (tl > fl ? tl - fl : 0) + 1;
    out = (char *)malloc(cap);
    if (!out)
        return NULL;
    o = out;
    for (;;) {
        q = strstr(src, from);
        if (!q) {
            strcpy(o, src);
            break;
        }
        memcpy(o, src, (size_t)(q - src));
        o += q - src;
        memcpy(o, to, tl);
        o += tl;
        src = q + fl;
    }
    return out;
}

/* The shader text for one combiner configuration: everything up to ps_main as it is, then constants,
 * then ps_main with the control reads replaced. Caller frees. */
static char *hw_psv_source(const HwPsSpec *sp, size_t *out_len)
{
    static const char marker[] = "float4 ps_main(VO i) : SV_Target {";
    const char *all_vs = s_hw_hlsl_vs_common;
    const char *body = strstr(s_hw_hlsl, marker);
    char *pm, *t, *res, hdr[4096], tmp[96];
    size_t pre_len = body ? (size_t)(body - s_hw_hlsl) : 0, vlen = strlen(all_vs), n, k, w;
    char *o = hdr;
    if (!body)
        return NULL;
    n = sp->ctl[0] & 0xFF;
    if (n > 8) n = 8;
    o += sprintf(o, "static const uint4 cwS[8] = {");
    for (k = 0; k < 8; k++)
        o += sprintf(o, "%suint4(%uu,%uu,%uu,%uu)", k ? "," : "",
                     (k < n && sp->mx) ? sp->cw[k][0] : 0u, (k < n && sp->mx) ? sp->cw[k][1] : 0u,
                     (k < n && sp->mx) ? sp->cw[k][2] : 0u, (k < n && sp->mx) ? sp->cw[k][3] : 0u);
    o += sprintf(o, "};\nstatic const uint4 ctlS = uint4(%uu,%uu,%uu,%uu);\n",
                 sp->ctl[0], sp->ctl[1], sp->ctl[2], sp->ctl[3]);
    o += sprintf(o, "static const uint4 atestS = uint4(%uu,%uu,0u,0u);\n", sp->ax, sp->ay);
    o += sprintf(o, "static const uint MX = %uu; static const uint MY = %uu;\n", sp->mx, sp->my);
    o += sprintf(o, "static const float B0 = %d.0, B1 = %d.0, B2 = %d.0, B3 = %d.0;\n",
                 (int)(sp->bound & 1), (int)((sp->bound >> 1) & 1), (int)((sp->bound >> 2) & 1), (int)((sp->bound >> 3) & 1));
    pm = hw_str_replace_all(body, "cw[s]", "cwS[s]");
    if (!pm) return NULL;
    t = hw_str_replace_all(pm, "ctl.", "ctlS."); free(pm); pm = t; if (!pm) return NULL;
    t = hw_str_replace_all(pm, "atest.x", "atestS.x"); free(pm); pm = t; if (!pm) return NULL;
    t = hw_str_replace_all(pm, "atest.y", "atestS.y"); free(pm); pm = t; if (!pm) return NULL;
    t = hw_str_replace_all(pm, "misc.x", "MX"); free(pm); pm = t; if (!pm) return NULL;
    t = hw_str_replace_all(pm, "misc.y", "MY"); free(pm); pm = t; if (!pm) return NULL;
    for (k = 0; k < 4; k++) {
        char from[24], to[8];
        sprintf(from, "tscale[%u].z", (unsigned)k);
        sprintf(to, "B%u", (unsigned)k);
        t = hw_str_replace_all(pm, from, to); free(pm); pm = t; if (!pm) return NULL;
    }
    t = hw_str_replace_all(pm, "[loop] for (uint s = 0; s < n; s++)", "[unroll] for (uint s = 0; s < n; s++)");
    free(pm); pm = t; if (!pm) return NULL;
    (void)tmp;
    w = vlen + pre_len + strlen(hdr) + strlen(pm) + 1;
    res = (char *)malloc(w);
    if (!res) { free(pm); return NULL; }
    memcpy(res, all_vs, vlen);
    memcpy(res + vlen, s_hw_hlsl, pre_len);
    memcpy(res + vlen + pre_len, hdr, strlen(hdr));
    strcpy(res + vlen + pre_len + strlen(hdr), pm);
    free(pm);
    *out_len = strlen(res);
    return res;
}

static DWORD WINAPI hw_psv_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        int idx;
        HwPsv *v;
        char *src;
        size_t len = 0;
        ID3DBlob *b;
        ID3D11PixelShader *ps = NULL;
        WaitForSingleObject(s_hw_psv_sem, INFINITE);
        EnterCriticalSection(&s_hw_psv_cs);
        idx = s_hw_psv_q[s_hw_psv_qh];
        s_hw_psv_qh = (s_hw_psv_qh + 1) % HW_PSV_SIZE;
        LeaveCriticalSection(&s_hw_psv_cs);
        v = &s_hw_psv[idx];
        src = hw_psv_source(&v->spec, &len);
        b = src ? hw_compile_src_f(src, len, "ps_main", "ps_4_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS) : NULL;
        free(src);
        if (b && SUCCEEDED(ID3D11Device_CreatePixelShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                                         ID3D10Blob_GetBufferSize(b), NULL, &ps)))
            ;
        else
            ps = NULL;
        if (b) ID3D10Blob_Release(b);
        if (ps) {
            v->ps = ps;
            InterlockedExchange(&v->state, 2);
            InterlockedIncrement(&s_hw_psv_ready);
        } else {
            InterlockedExchange(&v->state, 3);
            InterlockedIncrement(&s_hw_psv_failed);
        }
    }
    return 0;
}

/* The pixel shader for this draw's constants: a ready variant, else the interpreter. */
static ID3D11PixelShader *hw_ps_pick(const HwPsConst *pc)
{
    HwPsSpec sp;
    uint64_t h = 1469598103934665603ull;
    uint32_t *w = (uint32_t *)&sp;
    size_t i, nw, n;
    unsigned slot, probe;
    HwPsv *v;

    if (s_hw_psspec_env < 0) {
        const char *e = getenv("RECOMP_HW_PSSPEC");
        s_hw_psspec_env = !(e && atoi(e) == 0);
    }
    if (!s_hw_psspec_env || s_hw_psspec_off)
        return s_hw.ps;
    s_hw_psv_fb++;                      /* undone below when a variant is returned */
    memset(&sp, 0, sizeof sp);
    n = pc->ctl[0] & 0xFF;
    if (n > 8) n = 8;
    sp.mx = pc->misc[0];
    sp.my = pc->misc[1];
    if (sp.mx)
        for (i = 0; i < n; i++)
            memcpy(sp.cw[i], pc->cw[i], 16);
    memcpy(sp.ctl, pc->ctl, 16);
    sp.ax = pc->atest[0];
    sp.ay = pc->atest[1];
    for (i = 0; i < 4; i++)
        if (pc->tscale[i][2] > 0.5f)
            sp.bound |= 1u << i;
    nw = sizeof sp / 4;
    for (i = 0; i < nw; i++)
        h = (h ^ w[i]) * 1099511628211ull;
    if (!h) h = 1;
    slot = (unsigned)(h % HW_PSV_SIZE);
    for (probe = 0; probe < 16; probe++, slot = (slot + 1) % HW_PSV_SIZE) {
        v = &s_hw_psv[slot];
        if (v->state != 0 && v->key == h) {
            if (v->state == 2) { s_hw_psv_fb--; return v->ps; }
            return s_hw.ps;
        }
        if (v->state == 0)
            break;
    }
    if (probe == 16 || s_hw_psv_n >= HW_PSV_SIZE - 64)
        return s_hw.ps;
    if (!s_hw_psv_sem) {
        InitializeCriticalSection(&s_hw_psv_cs);
        s_hw_psv_sem = CreateSemaphoreA(NULL, 0, HW_PSV_SIZE, NULL);
        { int wi; for (wi = 0; wi < 3; wi++) CloseHandle(CreateThread(NULL, 0, hw_psv_worker, NULL, 0, NULL)); }
    }
    v = &s_hw_psv[slot];
    v->key = h;
    v->spec = sp;
    v->ps = NULL;
    s_hw_psv_n++;
    {   /* On disk already (an earlier run): create it right here, no frames on the slow interpreter. */
        size_t slen = 0;
        char *src = hw_psv_source(&sp, &slen);
        ID3DBlob *b = src ? hw_cache_lookup(src, slen, "ps_main", "ps_4_0",
                                            D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS) : NULL;
        ID3D11PixelShader *ps = NULL;
        free(src);
        if (b) {
            if (SUCCEEDED(ID3D11Device_CreatePixelShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                                         ID3D10Blob_GetBufferSize(b), NULL, &ps)) && ps) {
                v->ps = ps;
                InterlockedExchange(&v->state, 2);
                InterlockedIncrement(&s_hw_psv_ready);
                ID3D10Blob_Release(b);
                s_hw_psv_fb--;
                return ps;
            }
            ID3D10Blob_Release(b);
        }
    }
    EnterCriticalSection(&s_hw_psv_cs);
    s_hw_psv_q[s_hw_psv_qt] = (int)slot;
    s_hw_psv_qt = (s_hw_psv_qt + 1) % HW_PSV_SIZE;
    LeaveCriticalSection(&s_hw_psv_cs);
    InterlockedExchange(&v->state, 1);
    ReleaseSemaphore(s_hw_psv_sem, 1, NULL);
    return s_hw.ps;
}

static HwSurface *hw_bind_draw_state(const HwVsp *prog)
{
    HwSurface *s = hw_surface();
    HwDepth *d;
    HwPsConst pc;
    D3D11_MAPPED_SUBRESOURCE map;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    HwVsConst *vc = &s_hw.vs_last;
    /* The header (vp, fogp, ws, pad) and then cur[]: everything in HwVsConst
     * before the program constants. */
    float hdr[16 + NV_VERTEX_ATTRS * 4], bf[4];
    int st, i;
    UINT stride = sizeof(HwVertex), zero = 0;
    uint32_t area;

    if (!s)
        return NULL;
    d = hw_depth(s);
    ID3D11DeviceContext_OMSetRenderTargets(s_hw.ctx, 1, &s->rtv, d ? d->dsv : NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)hw_px(s->w, s->sc);
    vp.Height = (float)(s->h * s->sc);
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_hw.ctx, 1, &vp);
    sc.left = (LONG)hw_px(s_gpu.clip_x, s->sc);
    sc.top = (LONG)(s_gpu.clip_y * s->sc);
    sc.right = (LONG)hw_px(s_gpu.clip_x + s_gpu.clip_w, s->sc);
    sc.bottom = (LONG)((s_gpu.clip_y + s_gpu.clip_h) * s->sc);
    ID3D11DeviceContext_RSSetScissorRects(s_hw.ctx, 1, &sc);
    {
        /* pd-js7/pd-42q diagnostic (RECOMP_HW_CLIPDBG=1): histogram of the (surface, clip) pairs draws use,
         * printed with the five-second report, to find what draws into the margin outside the game's view. */
        static int on = -1;
        if (on < 0) on = getenv("RECOMP_HW_CLIPDBG") ? 1 : 0;
        if (on) {
            int k;
            for (k = 0; k < s_hw_clip_n; k++)
                if (s_hw_clip[k].off == s->offset && s_hw_clip[k].sw == s->w && s_hw_clip[k].sh == s->h
                    && s_hw_clip[k].cx == s_gpu.clip_x && s_hw_clip[k].cy == s_gpu.clip_y
                    && s_hw_clip[k].cw == s_gpu.clip_w && s_hw_clip[k].ch == s_gpu.clip_h) break;
            if (k == s_hw_clip_n && k < 64) {
                s_hw_clip[k].off = s->offset; s_hw_clip[k].sw = s->w; s_hw_clip[k].sh = s->h;
                s_hw_clip[k].cx = s_gpu.clip_x; s_hw_clip[k].cy = s_gpu.clip_y;
                s_hw_clip[k].cw = s_gpu.clip_w; s_hw_clip[k].ch = s_gpu.clip_h; s_hw_clip[k].n = 0;
                s_hw_clip_n++;
            }
            if (k < 64) s_hw_clip[k].n++;
        }
    }
    if (s->w == 640 && s_gpu.clip_w && s_gpu.clip_h) {
        int k;
        for (k = 0; k < s_hw_vc_n; k++)
            if (s_hw_vc[k].cx == s_gpu.clip_x && s_hw_vc[k].cy == s_gpu.clip_y
                && s_hw_vc[k].cw == s_gpu.clip_w && s_hw_vc[k].ch == s_gpu.clip_h) break;
        if (k == s_hw_vc_n && k < 8) {
            s_hw_vc[k].cx = s_gpu.clip_x; s_hw_vc[k].cy = s_gpu.clip_y;
            s_hw_vc[k].cw = s_gpu.clip_w; s_hw_vc[k].ch = s_gpu.clip_h; s_hw_vc[k].n = 0;
            s_hw_vc_n++;
        }
        if (k < 8) s_hw_vc[k].n++;
    }

    {
        int mode = 0;
        ID3D11RasterizerState **set = s_hw.front_face == 0x901 ? s_hw.rs_ccw : s_hw.rs;
        if (s_hw.cull_enable && !s_hw.nocull)
            mode = s_hw.cull_face == 0x404 ? 2 : (s_hw.cull_face == 0x405 ? 1 : 0);
        ID3D11DeviceContext_RSSetState(s_hw.ctx, set[mode]);
    }
    hw_color4(s_gpu.blend_color, bf);
    ID3D11DeviceContext_OMSetBlendState(s_hw.ctx, hw_blend_state(), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_hw.ctx, hw_depth_state(d != NULL), s_gpu.stencil_ref & 0xFF);

    if (hw_dbg_env("RECOMP_HW_GLOWDBG", 0) && s->w < 640) {
        static uint32_t seen[16]; static int ns;
        uint32_t sig = s->w * 31u + s_gpu.rc.color_icw[0] * 7u + s_gpu.rc.color_ocw[0] + s_gpu.rc.control * 13u + s_gpu.rc.final0 * 3u + s_gpu.texs[0].offset;
        int k, dup = 0;
        for (k = 0; k < ns; k++) if (seen[k] == sig) dup = 1;
        if (!dup && ns < 16) {
            int q;
            seen[ns++] = sig;
            fprintf(stderr, "[GLOWRT] draw into %dx%d off=%08X blend=%u %X/%X ctl=%08X stg=%08X fin=%08X/%08X tex0 off=%08X %ux%u fmt=%02X idx=%u\n",
                    (int)s->w, (int)s->h, (unsigned)s->offset, (unsigned)s_gpu.blend_enable,
                    (unsigned)s_gpu.blend_sfactor, (unsigned)s_gpu.blend_dfactor,
                    (unsigned)s_gpu.rc.control, (unsigned)s_gpu.rc.stage_program,
                    (unsigned)s_gpu.rc.final0, (unsigned)s_gpu.rc.final1,
                    (unsigned)s_gpu.texs[0].offset, (unsigned)s_gpu.texs[0].width, (unsigned)s_gpu.texs[0].height,
                    (unsigned)s_gpu.texs[0].color, (unsigned)s_gpu.idx_count);
            for (q = 0; q < (int)(s_gpu.rc.control & 0xFF) && q < 8; q++)
                fprintf(stderr, "[GLOWRT]  s%d icw=%08X aicw=%08X ocw=%08X aocw=%08X c0=%08X c1=%08X\n", q,
                        (unsigned)s_gpu.rc.color_icw[q], (unsigned)s_gpu.rc.alpha_icw[q],
                        (unsigned)s_gpu.rc.color_ocw[q], (unsigned)s_gpu.rc.alpha_ocw[q],
                        (unsigned)s_gpu.rc.factor0[q], (unsigned)s_gpu.rc.factor1[q]);
        }
    }

    if (hw_dbg_env("RECOMP_HW_ATDBG", 1) && s_gpu.alpha_test && s->w >= 640) {
        static uint32_t seen[64]; static int ns;
        const Texture *t0 = &s_gpu.texs[0];
        uint32_t sig = t0->offset * 31u + t0->color;
        int k, dup = 0;
        for (k = 0; k < ns; k++) if (seen[k] == sig) dup = 1;
        if (!dup && ns < 64) {
            char fn[96]; FILE *ff; uint32_t x, y;
            seen[ns++] = sig;
            fprintf(stderr, "[AT] tex0 off=%08X %ux%u fmt=%02X pitch=%u aref=%02X func=%X blend=%u %X/%X rc_seen=%d stg=%08X ctl=%08X fin=%08X/%08X s0icw=%08X s0ocw=%08X valid=%d\n",
                    (unsigned)t0->offset, (unsigned)t0->width, (unsigned)t0->height, (unsigned)t0->color, (unsigned)t0->pitch,
                    (unsigned)s_gpu.alpha_ref, (unsigned)s_gpu.alpha_func, (unsigned)s_gpu.blend_enable,
                    (unsigned)s_gpu.blend_sfactor, (unsigned)s_gpu.blend_dfactor, (int)s_gpu.rc_seen,
                    (unsigned)s_gpu.rc.stage_program, (unsigned)s_gpu.rc.control, (unsigned)s_gpu.rc.final0, (unsigned)s_gpu.rc.final1,
                    (unsigned)s_gpu.rc.color_icw[0], (unsigned)s_gpu.rc.color_ocw[0], (int)t0->valid);
            if (t0->valid && t0->width <= 1024 && t0->height <= 1024) {
                sprintf(fn, "dumps/at_%08X.ppm", (unsigned)t0->offset);
                ff = fopen(fn, "wb");
                if (ff) {
                    fprintf(ff, "P6\n%u %u\n255\n", (unsigned)(t0->width * 2), (unsigned)t0->height);
                    for (y = 0; y < t0->height; y++) {
                        for (x = 0; x < t0->width; x++) {
                            uint32_t argb = 0xFFFFFFFFu; unsigned char b3[3];
                            if (!sample_tex(t0, 0, x, y, &argb)) argb = 0xFFFF00FFu;
                            b3[0] = (argb >> 16) & 255; b3[1] = (argb >> 8) & 255; b3[2] = argb & 255; fwrite(b3, 1, 3, ff);
                        }
                        for (x = 0; x < t0->width; x++) {
                            uint32_t argb = 0xFFFFFFFFu; unsigned char b3[3];
                            if (!sample_tex(t0, 0, x, y, &argb)) argb = 0;
                            b3[0] = b3[1] = b3[2] = (argb >> 24) & 255; fwrite(b3, 1, 3, ff);
                        }
                    }
                    fclose(ff);
                }
            }
        }
    }

    memset(&pc, 0, sizeof pc);
    for (i = 0; i < 8; i++) {
        pc.cw[i][0] = s_gpu.rc.color_icw[i]; pc.cw[i][1] = s_gpu.rc.alpha_icw[i];
        pc.cw[i][2] = s_gpu.rc.color_ocw[i]; pc.cw[i][3] = s_gpu.rc.alpha_ocw[i];
        hw_color4(s_gpu.rc.factor0[i], pc.fac0[i]);
        hw_color4(s_gpu.rc.factor1[i], pc.fac1[i]);
    }
    pc.ctl[0] = s_gpu.rc.control; pc.ctl[1] = s_gpu.rc.stage_program;
    pc.ctl[2] = s_gpu.rc.final0;  pc.ctl[3] = s_gpu.rc.final1;
    pc.atest[0] = s_gpu.alpha_test; pc.atest[1] = s_gpu.alpha_func;
    pc.atest[2] = s_gpu.alpha_ref;  pc.atest[3] = s_gpu.clip_plane_mode;
    hw_color4(s_gpu.rc.final_c0, pc.finc0);
    hw_color4(s_gpu.rc.final_c1, pc.finc1);
    hw_color4(s_gpu.fog_color, pc.fogc);       /* FOG_COLOR has R in bits 0-7 */
    { float r = pc.fogc[2]; pc.fogc[2] = pc.fogc[0]; pc.fogc[0] = r; }
    pc.misc[0] = (uint32_t)s_gpu.rc_seen;
    {
        static uint32_t dbgtex = 0; static int dbgon = 0, dbgmode; static DWORD lastf = 0;
        if (hw_dbg_env("RECOMP_HW_DBG", 2) && GetTickCount() - lastf > 400u) {
            FILE *df = fopen("hw_dbg.txt", "r");
            unsigned t = 0; int m = 0;
            lastf = GetTickCount();
            dbgon = 0; dbgmode = 0;
            if (df) {
                if (fscanf(df, "%x %d", &t, &m) == 2) { dbgon = 1; dbgtex = t; dbgmode = m; }
                fclose(df);
            }
        }
        pc.misc[1] = (dbgon && (dbgtex == 0xFFFFFFFFu || s_gpu.texs[0].offset == dbgtex)) ? (uint32_t)dbgmode : 0;
        if (pc.misc[1] == 10) {
            uint32_t h = s_gpu.texs[0].offset * 2654435761u;
            uint32_t r = 64 + ((h >> 8) & 3) * 64, g = 64 + ((h >> 12) & 3) * 64, b = 64 + ((h >> 16) & 3) * 64;
            static uint32_t logged[512]; static int nl; int k, dup = 0;
            pc.misc[2] = r | (g << 8) | (b << 16);
            for (k = 0; k < nl; k++) if (logged[k] == s_gpu.texs[0].offset) dup = 1;
            if (!dup && nl < 512) {
                logged[nl++] = s_gpu.texs[0].offset;
                fprintf(stderr, "[DBGCOL] off=%08X rgb=%u,%u,%u %ux%u fmt=%02X cmask=%08X z=%X\n", (unsigned)s_gpu.texs[0].offset, r, g, b,
                        (unsigned)s_gpu.texs[0].width, (unsigned)s_gpu.texs[0].height, (unsigned)s_gpu.texs[0].color,
                        (unsigned)s_gpu.color_mask, (unsigned)s_gpu.depth_func);
            }
        }
    }
    for (st = 0; st < 4; st++) {
        const Texture *t = &s_gpu.texs[st];
        uint32_t mode = (s_gpu.rc.stage_program >> (st * 5)) & 0x1F;
        srv[st] = NULL;
        if ((s_gpu.rc_seen ? (mode >= 1 && mode <= 3) : st == 0) && t->valid)
            srv[st] = hw_texture(st, s);
        pc.tscale[st][0] = pc.tscale[st][1] = 1.0f;
        if (t->valid && !tex_size_from_format(t->color)) {
            pc.tscale[st][0] = 1.0f / (float)t->width;
            pc.tscale[st][1] = 1.0f / (float)t->height;
        }
        pc.tscale[st][2] = srv[st] ? 1.0f : 0.0f;
        if (!srv[st]) {
            if (hw_dbg_env("RECOMP_HW_GLOWDBG", 0) && (s_gpu.rc_seen ? (mode >= 1 && mode <= 3) : st == 0) && t->valid) {
                static int nw;
                if (nw++ < 40)
                    fprintf(stderr, "[WHITE] stage %d tex off=%08X %ux%u fmt=%02X into %dx%d -> white fallback\n",
                            st, (unsigned)t->offset, (unsigned)t->width, (unsigned)t->height, (unsigned)t->color,
                            (int)s->w, (int)s->h);
            }
            srv[st] = s_hw.white;
        }
        smp[st] = hw_sampler(t);
    }
    /* Consecutive batches mostly share their combiner set-up. */
    if (!s_hw.ps_last_valid || memcmp(&pc, &s_hw.ps_last, sizeof pc)) {
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &pc, sizeof pc);
            ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0);
            s_hw.ps_last = pc;
            s_hw.ps_last_valid = 1;
        }
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 2.0f / (float)s->w; hdr[1] = 2.0f / (float)s->h;
    hdr[2] = 1.0f / (float)hw_zmax_bits();
    hdr[4] = s_gpu.fog_param[0]; hdr[5] = s_gpu.fog_param[1];
    hdr[6] = (float)s_gpu.fog_mode; hdr[7] = s_gpu.fog_enable ? 1.0f : 0.0f;
    {
        /* Only the full display surface: the glow chain draws w == 1 quads
         * into parts of its own targets (320 wide into a 640 surface), and
         * moving those vertices left the level a flat colour.
         *   ws.x: the 2D (w == 1) keeps its 4:3 size around the centre.
         * The 3D needs nothing here: the target's hooks widen the title's
         * own projection (src/<target>/target_hooks.c). */
        int main_view = s->w == 640 && s_gpu.clip_w == 640 && s_gpu.clip_h >= 448;
        hdr[8] = main_view && !s_hw.batch_full_width ? 1.0f / s_hw.widen : 1.0f;
        /* RE-SoT: demo pause layers have already passed through the Hor+
         * projection. Undo that narrowing, rather than narrowing them again. */
        if (main_view && s_hw.batch_projected_screen)
            hdr[8] = s_hw.widen;
        hdr[9] = (float)s->w; hdr[10] = 1.0f;
        if (main_view && s_hw.batch_hud) {
            /* RE-SoT: the common HUD origin, observed at x=118.931 in the
             * demo's 16:9 trace, is x=51.9 before the projection hook.
             * Keep its old on-screen anchor while undoing the second squeeze.
             * All HUD batches share this origin so fills and frame stay aligned. */
            float origin = 320.0f + (51.9f - 320.0f) / s_hw.widen;
            hdr[8] = 1.0f;
            hdr[11] = (origin - 320.0f) / s_hw.widen + 320.0f - origin;
        }
    }
    /* RE-SoT: an attribute with no array reads the value last set with
     * SET_VERTEX_DATA*, as on the NV2A -- not 0,0,0,1. XDK D3D's
     * SetVertexData4ub gives a whole batch one diffuse colour that way; the
     * demo draws its shadow silhouettes with it, and with 0,0,0,1 every
     * silhouette came out black on black and no shadow was cast. */
    memcpy(&hdr[16], s_gpu.imm_attr, sizeof s_gpu.imm_attr);
    {
        /* A discard map loses the old contents, so the constants go with
         * every upload that needs them -- and the upload is skipped when
         * neither the header nor (for a program) the constants moved. */
        uint32_t gen = nv2a_vsh_constant_generation();
        int full = prog != NULL;
        if (!s_hw.vs_last_valid || memcmp(hdr, vc, sizeof hdr)
            || (full && (!s_hw.vs_last_full || s_hw.vs_last_gen != gen))) {
            memcpy(vc, hdr, sizeof hdr);
            if (full)
                memcpy(vc->c, nv2a_vsh_constants(), sizeof vc->c);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)s_hw.vs_cb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, vc, full ? sizeof *vc : sizeof hdr);
                ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vs_cb, 0);
                s_hw.vs_last_valid = 1;
                s_hw.vs_last_full = full;
                s_hw.vs_last_gen = gen;
            }
        }
    }
    ID3D11DeviceContext_IASetInputLayout(s_hw.ctx, prog ? s_hw.layout_prog : s_hw.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_hw.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_hw.ctx, 0, 1, &s_hw.vb, &stride, &zero);
    ID3D11DeviceContext_VSSetShader(s_hw.ctx, prog ? prog->vs : s_hw.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_hw.ctx, 0, 1, &s_hw.vs_cb);
    ID3D11DeviceContext_PSSetShader(s_hw.ctx, s_hw_psdiag ? s_hw.ps_solid : hw_ps_pick(&pc), NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_hw.ctx, 0, 1, &s_hw.ps_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_hw.ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(s_hw.ctx, 0, 4, smp);

    s->drawn_frame = s_hw.frame;
    area = s->w * s->h;
    if (!s_hw.biggest || area >= s_hw.biggest_area) {
        s_hw.biggest = s;
        s_hw.biggest_area = area;
    }
    return s;
}

/* RE-SoT: RECOMP_HW_GLOW_DUMP=<prefix>, under the frame trace: the display
 * surface as it is just before the first batch that samples another render
 * target into it (the title's glow composite), and as it is at the flip, at
 * host resolution. The two differ only by the post-processing, so they show
 * whether it lands where the scene is. */
static void hw_dump_surface(ID3D11Texture2D *tex, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11Texture2D *stage = NULL;
    FILE *f;
    uint32_t y, x, row, hdr[13];
    uint8_t *line;

    ID3D11Texture2D_GetDesc(tex, &td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &stage)))
        return;
    ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)stage, (ID3D11Resource *)tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)stage, 0,
                                          D3D11_MAP_READ, 0, &map))) {
        f = fopen(path, "wb");
        row = (td.Width * 3 + 3) & ~3u;
        line = (uint8_t *)calloc(1, row);
        if (f && line) {
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 54 + row * td.Height;             /* file size (after "BM") */
            hdr[2] = 54; hdr[3] = 40; hdr[4] = td.Width; hdr[5] = td.Height;
            hdr[6] = 1 | (24u << 16); hdr[8] = row * td.Height;
            fwrite("BM", 1, 2, f);
            fwrite(hdr, 4, 13, f);
            for (y = 0; y < td.Height; y++) {
                const uint8_t *src = (const uint8_t *)map.pData
                                   + (size_t)(td.Height - 1 - y) * map.RowPitch;
                for (x = 0; x < td.Width; x++)
                    memcpy(line + x * 3, src + x * 4, 3);
                fwrite(line, 1, row, f);
            }
        }
        if (f) fclose(f);
        free(line);
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)stage, 0);
    }
    ID3D11Texture2D_Release(stage);
}

static void hw_glow_dump_check(const HwSurface *s, int at_flip)
{
    static const char *prefix = (const char *)-1;
    static int before_done;
    char path[600];
    uint32_t i;

    if (prefix == (const char *)-1)
        prefix = getenv("RECOMP_HW_GLOW_DUMP");
    if (!prefix || !s || (s_ftrace != 2 && !(at_flip && before_done == 1)))
        return;
    if (at_flip) {
        if (before_done == 1) {
            snprintf(path, sizeof path, "%s_after.bmp", prefix);
            hw_dump_surface(s->tex, path);
            for (i = 0; i < HW_MAX_SURFACES; i++)
                if (s_hw.surf[i].tex && s_hw.surf[i].w < 640) {
                    snprintf(path, sizeof path, "%s_%08X_%u.bmp", prefix,
                             s_hw.surf[i].offset, s_hw.surf[i].w);
                    hw_dump_surface(s_hw.surf[i].tex, path);
                }
            before_done = 2;
        }
        return;
    }
    if (before_done || s->w != 640 || !s_gpu.texs[0].valid)
        return;
    for (i = 0; i < HW_MAX_SURFACES; i++)
        if (s_hw.surf[i].tex && &s_hw.surf[i] != s
            && s_hw.surf[i].offset == s_gpu.texs[0].offset) {
            snprintf(path, sizeof path, "%s_before.bmp", prefix);
            hw_dump_surface(s->tex, path);
            before_done = 1;
            return;
        }
}

/* RE-SoT: under the frame trace, how many pixels each batch really left on
 * the host GPU (an occlusion query around the draw) and the state that can
 * remove all of them. A batch with triangles and no pixels is geometry that
 * is missing from the picture; the state says which stage removed it. */
static ID3D11Query *s_hw_trace_q;

static void hw_trace_begin(void)
{
    if (s_ftrace != 2)
        return;
    if (!s_hw_trace_q) {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_OCCLUSION;
        qd.MiscFlags = 0;
        ID3D11Device_CreateQuery(s_hw.dev, &qd, &s_hw_trace_q);
    }
    if (s_hw_trace_q)
        ID3D11DeviceContext_Begin(s_hw.ctx, (ID3D11Asynchronous *)s_hw_trace_q);
}

static void hw_trace_end(void)
{
    UINT64 px = 0;
    DWORD t0 = GetTickCount();

    if (s_ftrace != 2 || !s_hw_trace_q)
        return;
    ID3D11DeviceContext_End(s_hw.ctx, (ID3D11Asynchronous *)s_hw_trace_q);
    /* The first batch of a frame waits behind the whole previous frame. */
    while (ID3D11DeviceContext_GetData(s_hw.ctx, (ID3D11Asynchronous *)s_hw_trace_q,
                                       &px, sizeof px, 0) == S_FALSE
           && GetTickCount() - t0 < 2000)
        ;
    fprintf(stderr, "[FTRACE]     hw px %llu | cull %u face %X front %X | atest %u"
                    " func %X ref %X | cmask %08X | zbuf %s | fog %u mode %X %g %g\n",
            (unsigned long long)px, s_hw.cull_enable, s_hw.cull_face, s_hw.front_face,
            s_gpu.alpha_test, s_gpu.alpha_func, s_gpu.alpha_ref, s_gpu.color_mask,
            s_gpu.zeta_offset ? "yes" : "no", s_gpu.fog_enable, s_gpu.fog_mode,
            s_gpu.fog_param[0], s_gpu.fog_param[1]);
}

/* Reserve `bytes` at the ring's write position (aligned to `align`), wrapping
 * with a discard when it does not fit. The caller writes at *off and then
 * advances it by what it used. */
static int hw_ring_map(ID3D11Buffer *buf, uint32_t *off, uint32_t size,
                       uint32_t bytes, uint32_t align, D3D11_MAPPED_SUBRESOURCE *map)
{
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;

    *off = (*off + align - 1) / align * align;
    if (bytes > size)
        return 0;
    if (*off == 0 || *off + bytes > size) {
        *off = 0;
        how = D3D11_MAP_WRITE_DISCARD;
    }
    return SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)buf, 0,
                                             how, 0, map));
}

/* The translated form of the vertex program that would run now, for a batch
 * whose enabled attributes are `mask` (bit per attribute, assigned to input
 * slots in ascending order). NULL when it cannot run on the GPU; the caller
 * then uses the interpreter. */
static const HwVsp *hw_vsp_get(uint32_t mask)
{
    static uint32_t seen_gen, seen_mask;
    static const HwVsp *seen;
    static int seen_valid;
    uint32_t gen = nv2a_vsh_program_generation(), key;
    HwVsp *e;
    char *src;
    size_t cap = 1u << 17, len, body;
    ID3DBlob *b;
    int ok, i, slot;

    if (seen_valid && seen_gen == gen && seen_mask == mask)
        return seen;
    seen_valid = 1; seen_gen = gen; seen_mask = mask; seen = NULL;

    key = nv2a_vsh_program_key(&ok);
    if (!ok)
        return NULL;
    for (i = 0; i < s_hw.nvsp; i++)
        if (s_hw.vsp[i].key == key && s_hw.vsp[i].mask == mask) {
            seen = s_hw.vsp[i].vs ? &s_hw.vsp[i] : NULL;
            return seen;
        }
    if (s_hw.nvsp == HW_VSP_MAX)
        return NULL;                        /* table full: interpreter */
    e = &s_hw.vsp[s_hw.nvsp++];
    e->key = key; e->mask = mask; e->vs = NULL;

    src = (char *)malloc(cap);
    if (!src)
        return NULL;
    len = (size_t)snprintf(src, cap, "%s%s", s_hw_hlsl_vs_common, s_hw_hlsl_vsp_head);
    for (i = 0, slot = 0; i < NV_VERTEX_ATTRS; i++)
        if (mask & (1u << i))
            len += (size_t)snprintf(src + len, cap - len, "  v[%d] = i.a%d;\n", i, slot++);
    body = nv2a_vsh_emit_hlsl(src + len, cap - len - sizeof s_hw_hlsl_vsp_tail);
    if (body) {
        len += body;
        memcpy(src + len, s_hw_hlsl_vsp_tail, sizeof s_hw_hlsl_vsp_tail);
        len += sizeof s_hw_hlsl_vsp_tail - 1;
        b = hw_compile_src(src, len, "vs_prog", "vs_4_0");
        if (b) {
            ID3D11Device_CreateVertexShader(s_hw.dev, ID3D10Blob_GetBufferPointer(b),
                                            ID3D10Blob_GetBufferSize(b), NULL, &e->vs);
            if (!s_hw.layout_prog) {
                D3D11_INPUT_ELEMENT_DESC il[HW_VSP_SLOTS];
                for (i = 0; i < HW_VSP_SLOTS; i++) {
                    memset(&il[i], 0, sizeof il[i]);
                    il[i].SemanticName = "ATTR";
                    il[i].SemanticIndex = (UINT)i;
                    il[i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    il[i].AlignedByteOffset = (UINT)i * 16;
                    il[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                }
                ID3D11Device_CreateInputLayout(s_hw.dev, il, HW_VSP_SLOTS,
                                               ID3D10Blob_GetBufferPointer(b),
                                               ID3D10Blob_GetBufferSize(b),
                                               &s_hw.layout_prog);
            }
            ID3D10Blob_Release(b);
        }
        /* RE-SoT: RECOMP_HW_VSH_DUMP=all prints every translation, not only
         * the ones that fail to compile. */
        {
            const char *dump = getenv("RECOMP_HW_VSH_DUMP");
            if (dump && (!e->vs || strcmp(dump, "all") == 0))
                fprintf(stderr, "[VSH %08X]\n%s\n", key, src);
        }
    }
    free(src);
    if (!s_hw.layout_prog && e->vs) {
        ID3D11VertexShader_Release(e->vs);
        e->vs = NULL;
    }
    fprintf(stderr, "  [HW] vertex program %08X (attributes %04X): %s (%d translated)\n",
            key, mask, e->vs ? "on the GPU" : "NOT translated, interpreter", s_hw.nvsp);
    seen = e->vs ? e : NULL;
    return seen;
}

/* RE-SoT: the demo's sub_000EF140 projection hook narrows a full-screen
 * flat layer to 640 / widen, centred on x=320 (observed: 80.531..560.531
 * at 16:9). Its fog grids must cover the display, including inner vertices.
 * In this retail copy it is on for xbox-retail (RESOT_PROJECTED_UI), whose
 * sub_000CD970 hook narrows the projection the same way. Without it the menu
 * backdrop stops short of the sides (2026-10-15). The HUD rule below
 * still has the demo's coordinates; the retail HUD has not been checked. */
static void hw_classify_screen(float left, float right, float top, float bottom)
{
    static int projected_ui = -1;
    if (projected_ui < 0) {
        const char *e = getenv("RECOMP_HW_PROJECTED_UI");
        projected_ui = e && *e && *e != '0';
    }
    s_hw.batch_full_width = left <= 8.0f && right >= 632.0f;
    s_hw.batch_projected_screen = projected_ui
        && s_hw.widen > 1.0f
        && fabsf((right - left) - 640.0f / s_hw.widen) < 2.0f
        && fabsf((left + right) * 0.5f - 320.0f) < 2.0f
        && top <= 1.0f && bottom >= 479.0f;
    /* RE-SoT: traced health frame/fill: flat, depth test off, upper
     * left (x=118.931..280.211, y=38.931..115.731 at 16:9). Use the shared
     * UI region, not texture heap addresses, which change across loads.
     * Include depth_mask=1: the white sand crescents use it with depth
     * testing disabled, while the health frame/fill and gold pips use 0. */
    s_hw.batch_hud = projected_ui && s_hw.widen > 1.0f
        && !s_gpu.depth_test
        && left >= 320.0f - 320.0f / s_hw.widen && right < 320.0f
        && top >= 16.0f && bottom < 144.0f;
}

/* Draw the current batch with its vertex program running on the GPU: the
 * distinct vertices of the batch go up as raw attributes, the primitive as an
 * index list. Returns 0 when the batch has to take the interpreter path. */

/* Bring-up bisect: hw_maxdraw.txt (in the working directory) holds the number of
 * draws to keep per frame; re-read every flip. Missing or negative = all. */
static long s_hw_maxdraw = -1;
static unsigned s_hw_framedraws;
static int hw_draw_allowed(void)
{
    if (s_hw_maxdraw >= 0 && (long)s_hw_framedraws >= s_hw_maxdraw) return 0;
    s_hw_framedraws++;
    return 1;
}
static void hw_maxdraw_refresh(void)
{
    FILE *f = fopen("hw_maxdraw.txt", "r");
    long v = -1;
    s_hw_framedraws = 0;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    s_hw_maxdraw = v;
}

/* RECOMP_HW_NOGLOW=1: skip the title's glow composite (a full-screen additive quad that
 * samples a 512-wide render target). The pixel-shader port mis-evaluates its combiner
 * setup and whites the frame out (software evaluation is correct); the proper fix is in
 * the HLSL combiner port. Phantom Dust only, off by default. */
static int hw_skip_zonly(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_HW_SKIPZONLY") != NULL;
    return on && (s_gpu.color_mask & 0x01010101u) == 0;
}

static int hw_skip_glow(void)
{
    static int on = -1;
    HwSurface *cur;
    uint32_t i;
    static int dbg = -1;
    if (on < 0) on = getenv("RECOMP_HW_NOGLOW") != NULL;
    if (dbg < 0) dbg = getenv("RECOMP_HW_GLOWDBG") != NULL;
    if ((!on && !dbg) || s_gpu.idx_count != 4) return 0;
    if (!(s_gpu.blend_enable && s_gpu.blend_sfactor == 1 && s_gpu.blend_dfactor == 1)) return 0;
    cur = hw_surface();
    if (!cur || cur->w < 600) return 0;
    for (i = 0; i < HW_MAX_SURFACES; i++)
        if (s_hw.surf[i].tex && s_hw.surf[i].w == 512 && s_hw.surf[i].offset == s_gpu.texs[0].offset) {
            if (dbg) {
                static int n;
                if (n++ < 6) {
                    const Texture *t = &s_gpu.texs[0];
                    fprintf(stderr, "[GLOW] quad: tex off=%08X fmt=%02X %ux%u surf=%dx%d sc=%d rc_seen=%d ctl=%08X stage=%08X fin=%08X/%08X\n",
                            (unsigned)t->offset, (unsigned)t->color, (unsigned)t->width, (unsigned)t->height,
                            (int)cur->w, (int)cur->h, (int)cur->sc, (int)s_gpu.rc_seen,
                            (unsigned)s_gpu.rc.control, (unsigned)s_gpu.rc.stage_program,
                            (unsigned)s_gpu.rc.final0, (unsigned)s_gpu.rc.final1);
                    for (int k = 0; k < (int)(s_gpu.rc.control & 0xFF) && k < 8; k++)
                        fprintf(stderr, "[GLOW]  stage %d icw=%08X aicw=%08X ocw=%08X aocw=%08X f0=%08X f1=%08X\n", k,
                                (unsigned)s_gpu.rc.color_icw[k], (unsigned)s_gpu.rc.alpha_icw[k],
                                (unsigned)s_gpu.rc.color_ocw[k], (unsigned)s_gpu.rc.alpha_ocw[k],
                                (unsigned)s_gpu.rc.factor0[k], (unsigned)s_gpu.rc.factor1[k]);
                    fprintf(stderr, "[GLOW]  final_c0=%08X final_c1=%08X blend %X/%X eq=%X\n",
                            (unsigned)s_gpu.rc.final_c0, (unsigned)s_gpu.rc.final_c1,
                            (unsigned)s_gpu.blend_sfactor, (unsigned)s_gpu.blend_dfactor, (unsigned)s_gpu.blend_equation);
                }
            }
            return on;
        }
    return 0;
}

static int hw_draw_program_batch(void)
{
    if (hw_skip_glow() || hw_skip_zonly()) return 1;
    if (!hw_draw_allowed()) return 1;
    static uint32_t stamp[65536], batch_id;
    static uint16_t remap[65536];
    D3D11_MAPPED_SUBRESOURCE vmap, imap;
    uint8_t slot_attr[HW_VSP_SLOTS];
    const HwVsp *prog;
    uint32_t n = s_gpu.idx_count, mask = 0, nslot = 0, a, i, nv = 0, ni = 0;
    HwSurface *bound;
    float (*vout)[4];
    uint16_t *iout;
    LARGE_INTEGER t0, t1;

    if (s_hw.cpu_vsh || n < 3 || n > NV_MAX_INDICES)
        return 0;
    for (a = 0; a < NV_VERTEX_ATTRS; a++) {
        const VertexAttr *at = &s_gpu.attr[a];
        if (!at->size || !at->stride || (!s_gpu.inline_active && !at->offset))
            continue;                       /* reads as 0,0,0,1 */
        if (nslot == HW_VSP_SLOTS)
            return 0;
        slot_attr[nslot++] = (uint8_t)a;
        mask |= 1u << a;
    }
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES: case NV_PRIM_TRIANGLE_STRIP: case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON: case NV_PRIM_QUADS: case NV_PRIM_QUAD_STRIP:
        break;
    default:
        return 1;                           /* points and lines: not yet */
    }
    prog = hw_vsp_get(mask);
    if (!prog)
        return 0;

    /* RE-SoT: classify the whole flat batch, including the pause layers
     * already narrowed by the target's projection. The full-height bounds
     * distinguish these from smaller UI widgets. */
    s_hw.batch_full_width = s_hw.batch_projected_screen = s_hw.batch_hud = 0;
    if (s_hw.widen > 1.0f && s_gpu.clip_w == 640 && s_gpu.clip_h >= 448) {
        float in[NV2A_VSH_INPUTS][4], lo = 1e9f, hi = -1e9f;
        float top = 1e9f, bottom = -1e9f;
        Nv2aVshOutput o;
        uint32_t k, a2;
        for (k = 0; k < n; k++) {
            for (a2 = 0; a2 < NV2A_VSH_INPUTS; a2++)
                fetch_attr(&s_gpu.attr[a2], s_gpu.idx[k], in[a2]);
            if (!nv2a_vsh_run((const float (*)[4])in, &o) || fabsf(o.pos[3] - 1.0f) > 0.0001f)
                break;
            if (o.pos[0] < lo) lo = o.pos[0];
            if (o.pos[0] > hi) hi = o.pos[0];
            if (o.pos[1] < top) top = o.pos[1];
            if (o.pos[1] > bottom) bottom = o.pos[1];
        }
        if (k == n)
            hw_classify_screen(lo, hi, top, bottom);
    }

    /* The frame trace (RECOMP_FRAME_TRACE) prints the executor's s_xf, which
     * this path never fills. Run the interpreter on the first and last
     * vertices of a traced batch so the trace shows where it really lands. */
    if (s_ftrace == 2) {
        float in[NV2A_VSH_INPUTS][4];
        Nv2aVshOutput o0, o1;
        uint32_t k, last = s_gpu.idx[n - 1];
        for (k = 0; k < NV2A_VSH_INPUTS; k++)
            fetch_attr(&s_gpu.attr[k], s_gpu.idx[0], in[k]);
        nv2a_vsh_run((const float (*)[4])in, &o0);
        for (k = 0; k < NV2A_VSH_INPUTS; k++)
            fetch_attr(&s_gpu.attr[k], last, in[k]);
        nv2a_vsh_run((const float (*)[4])in, &o1);
        fprintf(stderr, "[FTRACE]     gpu batch n %u: first %g %g %g w %g, last %g %g %g w %g"
                        " | d0 %g %g %g %g fog %g | vsh %08X\n",
                n, o0.pos[0], o0.pos[1], o0.pos[2], o0.pos[3],
                o1.pos[0], o1.pos[1], o1.pos[2], o1.pos[3],
                o0.d0[0], o0.d0[1], o0.d0[2], o0.d0[3], o0.fog[0], prog->key);
        {
            /* Where the whole batch lands, and its fog and w ranges. */
            float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
            float flo = 1e30f, fhi = -1e30f, alo = 1e30f, ahi = -1e30f;
            uint32_t c;
            for (k = 0; k < n; k++) {
                for (c = 0; c < NV2A_VSH_INPUTS; c++)
                    fetch_attr(&s_gpu.attr[c], s_gpu.idx[k], in[c]);
                nv2a_vsh_run((const float (*)[4])in, &o0);
                for (c = 0; c < 4; c++) {
                    if (o0.pos[c] < lo[c]) lo[c] = o0.pos[c];
                    if (o0.pos[c] > hi[c]) hi[c] = o0.pos[c];
                }
                if (o0.fog[0] < flo) flo = o0.fog[0];
                if (o0.fog[0] > fhi) fhi = o0.fog[0];
                if (o0.d0[3] < alo) alo = o0.d0[3];
                if (o0.d0[3] > ahi) ahi = o0.d0[3];
                if (n <= 4)
                    fprintf(stderr, "[FTRACE]     vtx %u pos %g %g t0 %g %g t1 %g %g"
                                    " t2 %g %g t3 %g %g\n", k, o0.pos[0], o0.pos[1],
                            o0.tex[0][0], o0.tex[0][1], o0.tex[1][0], o0.tex[1][1],
                            o0.tex[2][0], o0.tex[2][1], o0.tex[3][0], o0.tex[3][1]);
            }
            fprintf(stderr, "[FTRACE]     bbox x %g..%g y %g..%g z %g..%g w %g..%g"
                            " fog %g..%g d0.a %g..%g\n",
                    lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3],
                    flo, fhi, alo, ahi);
        }
    }

    QueryPerformanceCounter(&t0);
    EnterCriticalSection(&s_hw.lock);
    if (!(bound = hw_bind_draw_state(prog))
        || !hw_ring_map(s_hw.vb, &s_hw.vb_off, HW_VB_BYTES,
                        n * (uint32_t)sizeof(HwVertex), sizeof(HwVertex), &vmap)) {
        LeaveCriticalSection(&s_hw.lock);
        return 1;
    }
    if (!hw_ring_map(s_hw.ib, &s_hw.ib_off, HW_IB_BYTES, n * 3 * 2, 2, &imap)) {
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vb, 0);
        LeaveCriticalSection(&s_hw.lock);
        return 1;
    }
    vout = (float (*)[4])((uint8_t *)vmap.pData + s_hw.vb_off);
    iout = (uint16_t *)((uint8_t *)imap.pData + s_hw.ib_off);

    if (++batch_id == 0) {
        memset(stamp, 0, sizeof stamp);
        batch_id = 1;
    }
    for (i = 0; i < n; i++) {
        uint16_t ix = s_gpu.idx[i];
        float in[HW_VSP_SLOTS][4];
        if (stamp[ix] == batch_id)
            continue;
        stamp[ix] = batch_id;
        remap[ix] = (uint16_t)nv;
        for (a = 0; a < nslot; a++)
            fetch_attr(&s_gpu.attr[slot_attr[a]], ix, in[a]);
        for (; a < HW_VSP_SLOTS; a++)
            in[a][0] = in[a][1] = in[a][2] = in[a][3] = 0.0f;
        memcpy(vout[nv * HW_VSP_SLOTS], in, sizeof in);   /* write-combined: no reads */
        nv++;
    }
#define HW_IDX(x, y, z) do { iout[ni] = remap[s_gpu.idx[x]]; \
        iout[ni + 1] = remap[s_gpu.idx[y]]; iout[ni + 2] = remap[s_gpu.idx[z]]; \
        ni += 3; } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) HW_IDX(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) HW_IDX(i + 1, i, i + 2);      /* keep the winding */
            else       HW_IDX(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) HW_IDX(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            HW_IDX(i, i + 1, i + 2);
            HW_IDX(i, i + 2, i + 3);
        }
        break;
    default:                                         /* quad strip */
        for (i = 0; i + 3 < n; i += 2) {
            HW_IDX(i, i + 1, i + 3);
            HW_IDX(i, i + 3, i + 2);
        }
        break;
    }
#undef HW_IDX
    ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.ib, 0);
    ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vb, 0);
    if (ni) {
        ID3D11DeviceContext_IASetIndexBuffer(s_hw.ctx, s_hw.ib, DXGI_FORMAT_R16_UINT, 0);
        hw_glow_dump_check(bound, 0);
        hw_trace_begin();
        ID3D11DeviceContext_DrawIndexed(s_hw.ctx, ni, s_hw.ib_off / 2,
                                        (INT)(s_hw.vb_off / (uint32_t)sizeof(HwVertex)));
        hw_trace_end();
        s_hw.draws++;
        s_hw.gpu_vsh_batches++;
        s_hw.tris += ni / 3;
        s_gpu.tris_drawn += ni / 3;
        s_gpu.xf_drawn += ni / 3;
    }
    s_hw.vb_off += nv * (uint32_t)sizeof(HwVertex);
    s_hw.ib_off += ni * 2;
    s_hw_stat_verts += nv;
    LeaveCriticalSection(&s_hw.lock);
    QueryPerformanceCounter(&t1);
    s_hw_stat_draw_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
    return 1;
}

static void hw_put_vertex(HwVertex *o, const Nv2aVshOutput *v)
{
    memcpy(o->pos, v->pos, sizeof o->pos);
    memcpy(o->d0, v->d0, sizeof o->d0);
    memcpy(o->d1, v->d1, sizeof o->d1);
    o->fog[0] = fog_factor(v->fog[0]);
    o->fog[1] = o->fog[2] = o->fog[3] = 0.0f;
    memcpy(o->tex, v->tex, sizeof o->tex);
}

/* One batch of transformed vertices, in the primitive type the title used. */
static void hw_draw_batch(const Nv2aVshOutput *v, uint32_t n)
{
    if (hw_skip_glow() || hw_skip_zonly()) return;
    if (!hw_draw_allowed()) return;
    HwVertex *o = s_hw.verts;
    D3D11_MAPPED_SUBRESOURCE map;
    uint32_t i, count = 0;

    /* Full-width 2D keeps its width (see hw_draw_program_batch). */
    {
        float lo = 1e9f, hi = -1e9f, top = 1e9f, bottom = -1e9f;
        s_hw.batch_full_width = s_hw.batch_projected_screen = s_hw.batch_hud = 0;
        for (i = 0; i < n; i++) {
            if (fabsf(v[i].pos[3] - 1.0f) > 0.0001f)
                break;
            if (v[i].pos[0] < lo) lo = v[i].pos[0];
            if (v[i].pos[0] > hi) hi = v[i].pos[0];
            if (v[i].pos[1] < top) top = v[i].pos[1];
            if (v[i].pos[1] > bottom) bottom = v[i].pos[1];
        }
        if (i == n)
            hw_classify_screen(lo, hi, top, bottom);
    }
    if (s_ftrace == 2 && n <= 32)
        for (i = 0; i < n; i++)
            fprintf(stderr, "[FTRACE]     cpu vtx %u pos %g %g %g w %g t0 %g %g %g %g t1 %g %g\n",
                    i, v[i].pos[0], v[i].pos[1], v[i].pos[2], v[i].pos[3],
                    v[i].tex[0][0], v[i].tex[0][1], v[i].tex[0][2], v[i].tex[0][3],
                    v[i].tex[1][0], v[i].tex[1][1]);

/* Bring-up: drop triangles with a non-finite or zero-w vertex, or three coincident
 * vertices. An all-zero vertex batch (w = 0) was drawn as a full-screen white triangle. */
#define HW_VOK(p) (isfinite((p).pos[0]) && isfinite((p).pos[1]) && isfinite((p).pos[2]) && isfinite((p).pos[3]) && fabsf((p).pos[3]) > 1e-9f)
#define HW_TRI(a, b, c) do { if (HW_VOK(v[a]) && HW_VOK(v[b]) && HW_VOK(v[c]) \
        && !(v[a].pos[0] == v[b].pos[0] && v[b].pos[0] == v[c].pos[0] && v[a].pos[1] == v[b].pos[1] && v[b].pos[1] == v[c].pos[1])) { \
        hw_put_vertex(&o[count], &v[a]); \
        hw_put_vertex(&o[count + 1], &v[b]); \
        hw_put_vertex(&o[count + 2], &v[c]); count += 3; } } while (0)
    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) HW_TRI(i, i + 1, i + 2);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) HW_TRI(i + 1, i, i + 2);      /* keep the winding */
            else       HW_TRI(i, i + 1, i + 2);
        }
        break;
    case NV_PRIM_TRIANGLE_FAN:
    case NV_PRIM_POLYGON:
        for (i = 1; i + 1 < n; i++) HW_TRI(0, i, i + 1);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            HW_TRI(i, i + 1, i + 2);
            HW_TRI(i, i + 2, i + 3);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            HW_TRI(i, i + 1, i + 3);
            HW_TRI(i, i + 3, i + 2);
        }
        break;
    default:
        return;                                   /* points and lines: not yet */
    }
#undef HW_TRI
    if (!count)
        return;

    {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    EnterCriticalSection(&s_hw.lock);
    HwSurface *bound;
    if ((bound = hw_bind_draw_state(NULL)) != NULL
        && hw_ring_map(s_hw.vb, &s_hw.vb_off, HW_VB_BYTES,
                       (uint32_t)sizeof(HwVertex) * count, sizeof(HwVertex), &map)) {
        memcpy((uint8_t *)map.pData + s_hw.vb_off, o, sizeof(HwVertex) * count);
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vb, 0);
        hw_glow_dump_check(bound, 0);
        hw_trace_begin();
        ID3D11DeviceContext_Draw(s_hw.ctx, count, s_hw.vb_off / (uint32_t)sizeof(HwVertex));
        hw_trace_end();
        s_hw.vb_off += (uint32_t)sizeof(HwVertex) * count;
        s_hw.cpu_vsh_batches++;
        s_hw.draws++;
        s_hw.tris += count / 3;
        s_gpu.tris_drawn += count / 3;
        s_gpu.xf_drawn += count / 3;
    }
    LeaveCriticalSection(&s_hw.lock);
    QueryPerformanceCounter(&t1);
    s_hw_stat_draw_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
    }
}

/* NV097_CLEAR_SURFACE: bit 0 depth, bit 1 stencil, bits 4-7 colour channels. */
static void hw_clear(uint32_t param)
{
    HwSurface *s;
    HwDepth *d;
    uint32_t x0 = s_gpu.clip_x, y0 = s_gpu.clip_y;
    uint32_t x1 = s_gpu.clip_x + s_gpu.clip_w, y1 = s_gpu.clip_y + s_gpu.clip_h;

    EnterCriticalSection(&s_hw.lock);
    s = hw_surface();
    if (!s) {
        LeaveCriticalSection(&s_hw.lock);
        return;
    }
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RECOMP_HW_TEXDBG") ? 1 : 0;
      if (dbg && GetFileAttributesA("presdbg.flag") != INVALID_FILE_ATTRIBUTES)
          fprintf(stderr, "  [CLR] frame=%u surf=%08X %ux%u param=%02X color=%08X rect=%d%c", (unsigned)s_hw.frame, (unsigned)s->offset, (unsigned)s->w, (unsigned)s->h, (unsigned)param, (unsigned)s_gpu.clear_color, (int)s_hw.clear_rect_set, 10); }
    if (s_hw.clear_rect_set) {
        /* SET_CLEAR_RECT: min in the low half, max (inclusive) in the high. */
        uint32_t cx0 = s_hw.clear_rect_h & 0xFFFF, cx1 = (s_hw.clear_rect_h >> 16) + 1;
        uint32_t cy0 = s_hw.clear_rect_v & 0xFFFF, cy1 = (s_hw.clear_rect_v >> 16) + 1;
        if (cx0 > x0) x0 = cx0;
        if (cy0 > y0) y0 = cy0;
        if (cx1 < x1) x1 = cx1;
        if (cy1 < y1) y1 = cy1;
    }
    d = hw_depth(s);
    if ((param & 0x03) && d) {
        float z = (float)(hw_zmax_bits() == 0xFFFFu ? (s_gpu.zstencil_clear & 0xFFFF)
                                                    : (s_gpu.zstencil_clear >> 8))
                / (float)hw_zmax_bits();
        ID3D11DeviceContext_ClearDepthStencilView(
            s_hw.ctx, d->dsv,
            ((param & 1) ? D3D11_CLEAR_DEPTH : 0) | ((param & 2) ? D3D11_CLEAR_STENCIL : 0),
            z, (UINT8)(s_gpu.zstencil_clear & 0xFF));
    }
    if (param & NV097_CLEAR_COLOR_MASK) {
        float c[4];
        hw_color4(s_gpu.clear_color, c);
        if (x0 == 0 && y0 == 0 && x1 >= s->w && y1 >= s->h
            && (param & NV097_CLEAR_COLOR_MASK) == NV097_CLEAR_COLOR_MASK) {
            ID3D11DeviceContext_ClearRenderTargetView(s_hw.ctx, s->rtv, c);
        } else if (x1 > x0 && y1 > y0) {
            /* Part of the surface, or only some channels: a scissored fill. */
            HwPsConst pc;
            D3D11_MAPPED_SUBRESOURCE map;
            D3D11_VIEWPORT vp;
            D3D11_RECT sc;
            D3D11_BLEND_DESC bd;
            ID3D11BlendState *bs = NULL;
            static const float bf[4] = {0, 0, 0, 0};

            memset(&pc, 0, sizeof pc);
            memcpy(pc.finc0, c, sizeof c);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb,
                                                  0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, &pc, sizeof pc);
                ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0);
            }
            memset(&bd, 0, sizeof bd);
            /* CLEAR_SURFACE bits: 0x10 R, 0x20 G, 0x40 B, 0x80 A. */
            bd.RenderTarget[0].RenderTargetWriteMask =
                (UINT8)(((param & 0x10) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                      | ((param & 0x20) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                      | ((param & 0x40) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                      | ((param & 0x80) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
            ID3D11Device_CreateBlendState(s_hw.dev, &bd, &bs);
            ID3D11DeviceContext_OMSetRenderTargets(s_hw.ctx, 1, &s->rtv, NULL);
            vp.TopLeftX = 0; vp.TopLeftY = 0;
            vp.Width = (float)hw_px(s->w, s->sc);
            vp.Height = (float)(s->h * s->sc);
            vp.MinDepth = 0; vp.MaxDepth = 1;
            ID3D11DeviceContext_RSSetViewports(s_hw.ctx, 1, &vp);
            sc.left = (LONG)hw_px(x0, s->sc);
            sc.top = (LONG)(y0 * s->sc);
            sc.right = (LONG)hw_px(x1, s->sc);
            sc.bottom = (LONG)(y1 * s->sc);
            ID3D11DeviceContext_RSSetScissorRects(s_hw.ctx, 1, &sc);
            ID3D11DeviceContext_RSSetState(s_hw.ctx, s_hw.rs[0]);
            ID3D11DeviceContext_OMSetBlendState(s_hw.ctx, bs, bf, 0xFFFFFFFFu);
            ID3D11DeviceContext_OMSetDepthStencilState(s_hw.ctx, hw_depth_state(0), 0);
            ID3D11DeviceContext_IASetInputLayout(s_hw.ctx, NULL);
            ID3D11DeviceContext_IASetPrimitiveTopology(s_hw.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(s_hw.ctx, s_hw.vs_blit, NULL, 0);
            ID3D11DeviceContext_PSSetShader(s_hw.ctx, s_hw.ps_solid, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(s_hw.ctx, 0, 1, &s_hw.ps_cb);
            ID3D11DeviceContext_Draw(s_hw.ctx, 3, 0);
            if (bs) ID3D11BlendState_Release(bs);
        }
        s->drawn_frame = s_hw.frame;
        if (!s_hw.biggest || s->w * s->h >= s_hw.biggest_area) {
            s_hw.biggest = s;
            s_hw.biggest_area = s->w * s->h;
        }
    }
    s_gpu.clears++;
    LeaveCriticalSection(&s_hw.lock);
}

/* Drop every render target so the next draw recreates them at the current
 * scale. Called with the lock held, between frames. */
static void hw_release_all_targets(void)
{
    int i;
    for (i = 0; i < HW_MAX_SURFACES; i++) {
        HwSurface *t = &s_hw.surf[i];
        if (t->tex) {
            ID3D11ShaderResourceView_Release(t->srv);
            ID3D11RenderTargetView_Release(t->rtv);
            ID3D11Texture2D_Release(t->tex);
            memset(t, 0, sizeof *t);
        }
        if (s_hw_shadow[i].tex) {
            ID3D11ShaderResourceView_Release(s_hw_shadow[i].srv);
            ID3D11Texture2D_Release(s_hw_shadow[i].tex);
            memset(&s_hw_shadow[i], 0, sizeof s_hw_shadow[i]);
        }
    }
    for (i = 0; i < HW_MAX_DEPTHS; i++) {
        HwDepth *d = &s_hw.depth[i];
        if (d->tex) {
            ID3D11DepthStencilView_Release(d->dsv);
            ID3D11Texture2D_Release(d->tex);
            memset(d, 0, sizeof *d);
        }
    }
    s_hw.front = NULL;
    s_hw.biggest = NULL;
}

/* Render at a resolution that suits the window: one guest pixel becomes
 * ceil(window height / 480) host pixels, 1 to 4. A window taller than the
 * current scale supports gets sharper; a small one stops paying for pixels it
 * cannot show. */
static void hw_auto_scale(void)
{
    int want;
    if (!s_hw.auto_scale || !s_hw.bb_h)
        return;
    want = (int)((s_hw.bb_h + 479) / 480);
    if (want < 1) want = 1;
    if (want > 4) want = 4;
    if (want != s_hw.scale) {
        fprintf(stderr, "  [HW] window %ux%u: render scale %d -> %d\n",
                (unsigned)s_hw.bb_w, (unsigned)s_hw.bb_h, s_hw.scale, want);
        hw_release_all_targets();
        s_hw.scale = want;
    }
}

/* Put a surface (and the movie overlay, when one is showing) in the window.
 * Called with the lock held. */
static void hw_present_locked(int vsync)
{
    D3D11_VIEWPORT vp;
    const uint32_t *opx;
    uint32_t ow, oh, orect[4], oserial;
    float px, py, pw, ph;
    int ovl_active = 0;
    LARGE_INTEGER t0, t1;
    static const float black[4] = {0, 0, 0, 1}, bf[4] = {0, 0, 0, 0};
    ID3D11ShaderResourceView *none[4] = {NULL, NULL, NULL, NULL};

    hw_resize_backbuffer();
    hw_auto_scale();
    if (!s_hw.bb_rtv)
        return;
    ID3D11DeviceContext_OMSetRenderTargets(s_hw.ctx, 1, &s_hw.bb_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_hw.ctx, s_hw.bb_rtv, black);
    /* The picture keeps its shape in whatever shape the window has. */
    {
        float bw = (float)s_hw.bb_w, bh = (float)s_hw.bb_h;
        if (bw > bh * s_hw.aspect) { ph = bh; pw = bh * s_hw.aspect; }
        else                       { pw = bw; ph = bw / s_hw.aspect; }
        px = (bw - pw) * 0.5f; py = (bh - ph) * 0.5f;
    }
    vp.TopLeftX = px; vp.TopLeftY = py;
    vp.Width = pw; vp.Height = ph;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_hw.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_hw.ctx, s_hw.rs_blit);
    ID3D11DeviceContext_OMSetBlendState(s_hw.ctx, NULL, bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_hw.ctx, hw_depth_state(0), 0);
    ID3D11DeviceContext_IASetInputLayout(s_hw.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_hw.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_hw.ctx, s_hw.vs_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_hw.ctx, s_hw.ps_blit, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(s_hw.ctx, 0, 1, &s_hw.samp_blit);
    hw_gamma_update();
    if (s_hw.lut_srv) {
        ID3D11DeviceContext_PSSetShaderResources(s_hw.ctx, 4, 1, &s_hw.lut_srv);
        ID3D11DeviceContext_PSSetSamplers(s_hw.ctx, 4, 1, &s_hw.samp_lut);
    }
    if (s_hw.pres_valid && s_hw.pres_srv) {
        ID3D11DeviceContext_PSSetShaderResources(s_hw.ctx, 0, 1, &s_hw.pres_srv);
        ID3D11DeviceContext_Draw(s_hw.ctx, 3, 0);
    } else if (s_hw.front && s_hw.front->srv) {
        ID3D11DeviceContext_PSSetShaderResources(s_hw.ctx, 0, 1, &s_hw.front->srv);
        ID3D11DeviceContext_Draw(s_hw.ctx, 3, 0);
    }
    if (xbox_FramebufferOverlayGet(&opx, &ow, &oh, orect, &oserial) && ow && oh) {
        ovl_active = 1;
        if (!s_hw.ovl_tex || s_hw.ovl_w != ow || s_hw.ovl_h != oh) {
            D3D11_TEXTURE2D_DESC td;
            if (s_hw.ovl_srv) ID3D11ShaderResourceView_Release(s_hw.ovl_srv);
            if (s_hw.ovl_tex) ID3D11Texture2D_Release(s_hw.ovl_tex);
            s_hw.ovl_srv = NULL; s_hw.ovl_tex = NULL;
            memset(&td, 0, sizeof td);
            td.Width = ow; td.Height = oh; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &s_hw.ovl_tex)))
                ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)s_hw.ovl_tex,
                                                      NULL, &s_hw.ovl_srv);
            s_hw.ovl_w = ow; s_hw.ovl_h = oh;
            s_hw.ovl_serial = oserial - 1;
        }
        if (s_hw.ovl_srv) {
            if (s_hw.ovl_serial != oserial) {
                ID3D11DeviceContext_UpdateSubresource(s_hw.ctx, (ID3D11Resource *)s_hw.ovl_tex,
                                                      0, NULL, opx, ow * 4, 0);
                s_hw.ovl_serial = oserial;
            }
            /* The overlay's destination is in 640x480 display coordinates,
             * a 4:3 area in the middle of a wider picture. */
            float mw = ph * (4.0f / 3.0f), mx = px + (pw - mw) * 0.5f;
            vp.TopLeftX = mx + (float)orect[0] * mw / 640.0f;
            vp.TopLeftY = py + (float)orect[1] * ph / 480.0f;
            vp.Width = (float)orect[2] * mw / 640.0f;
            vp.Height = (float)orect[3] * ph / 480.0f;
            ID3D11DeviceContext_RSSetViewports(s_hw.ctx, 1, &vp);
            ID3D11DeviceContext_PSSetShaderResources(s_hw.ctx, 0, 1, &s_hw.ovl_srv);
            ID3D11DeviceContext_Draw(s_hw.ctx, 3, 0);
        }
    }
    if (s_hw.pres_valid && s_hw_view[2] && s_hw_view[3] && !getenv("RECOMP_HW_NOMASK")
        && (s_hw_view[2] < 640 || s_hw_view[3] < 480 || s_hw_view[0] || s_hw_view[1])
        && s_hw_view[0] + s_hw_view[2] <= 640 && s_hw_view[1] + s_hw_view[3] <= 480
        && !ovl_active) {
        /* Black bars over the overscan margin (see s_hw_view). Position in the 4:3 core of the picture. */
        HwPsConst pcb;
        D3D11_MAPPED_SUBRESOURCE map;
        D3D11_RECT r[4];
        float mw = ph * (4.0f / 3.0f), mx = px + (pw - mw) * 0.5f;
        float x0 = mx + (float)s_hw_view[0] * mw / 640.0f;
        float x1 = mx + (float)(s_hw_view[0] + s_hw_view[2]) * mw / 640.0f;
        float y0 = py + (float)s_hw_view[1] * ph / 480.0f;
        float y1 = py + (float)(s_hw_view[1] + s_hw_view[3]) * ph / 480.0f;
        int i;
        memset(&pcb, 0, sizeof pcb);
        pcb.finc0[3] = 1.0f;
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &pcb, sizeof pcb);
            ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0);
        }
        r[0].left = (LONG)mx;  r[0].right = (LONG)(mx + mw);  r[0].top = (LONG)py;  r[0].bottom = (LONG)(y0 + 0.5f);   /* top */
        r[1].left = (LONG)mx;  r[1].right = (LONG)(mx + mw);  r[1].top = (LONG)(y1 + 0.5f);  r[1].bottom = (LONG)(py + ph); /* bottom */
        r[2].left = (LONG)mx;  r[2].right = (LONG)(x0 + 0.5f);  r[2].top = (LONG)py;  r[2].bottom = (LONG)(py + ph);  /* left */
        r[3].left = (LONG)(x1 + 0.5f);  r[3].right = (LONG)(mx + mw);  r[3].top = (LONG)py;  r[3].bottom = (LONG)(py + ph); /* right */
        vp.TopLeftX = px; vp.TopLeftY = py; vp.Width = pw; vp.Height = ph;
        ID3D11DeviceContext_RSSetViewports(s_hw.ctx, 1, &vp);
        ID3D11DeviceContext_RSSetState(s_hw.ctx, s_hw.rs[0]);
        ID3D11DeviceContext_PSSetShader(s_hw.ctx, s_hw.ps_solid, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_hw.ctx, 0, 1, &s_hw.ps_cb);
        for (i = 0; i < 4; i++) {
            if (r[i].right <= r[i].left || r[i].bottom <= r[i].top) continue;
            ID3D11DeviceContext_RSSetScissorRects(s_hw.ctx, 1, &r[i]);
            ID3D11DeviceContext_Draw(s_hw.ctx, 3, 0);
        }
    }
    ID3D11DeviceContext_PSSetShaderResources(s_hw.ctx, 0, 4, none);
    if (getenv("RECOMP_HW_PRESDBG")) {
        /* Debug: read back the swap-chain buffer about to be shown and log its mean
         * brightness, to tell which presenter path produced a dark frame. */
        static ID3D11Texture2D *stg; static UINT sgw, sgh; static unsigned cnt; static unsigned pc, start = 0xFFFFFFFFu;
        ID3D11Texture2D *bbt = NULL;
        if (start == 0xFFFFFFFFu) start = (unsigned)atoi(getenv("RECOMP_HW_PRESDBG"));
        pc++;
        { static int armed; if ((pc & 63u) == 0) armed = GetFileAttributesA("presdbg.flag") != INVALID_FILE_ATTRIBUTES; if (!armed) start = 0xFFFFFFFEu; else if (start == 0xFFFFFFFEu) start = 0; }
        if (pc >= start && cnt < 3000 && SUCCEEDED(IDXGISwapChain_GetBuffer(s_hw.swap, 0, &IID_ID3D11Texture2D, (void **)&bbt))) {
            D3D11_TEXTURE2D_DESC d; D3D11_MAPPED_SUBRESOURCE mp;
            ID3D11Texture2D_GetDesc(bbt, &d);
            if (!stg || sgw != d.Width || sgh != d.Height) {
                D3D11_TEXTURE2D_DESC sd = d;
                if (stg) ID3D11Texture2D_Release(stg);
                stg = NULL;
                sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_hw.dev, &sd, NULL, &stg))) { sgw = d.Width; sgh = d.Height; }
            }
            if (stg) {
                ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)stg, (ID3D11Resource *)bbt);
                if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &mp))) {
                    double sum = 0; unsigned n = 0, x, y;
                    for (y = 0; y < sgh; y += 16) {
                        const uint8_t *row = (const uint8_t *)mp.pData + (size_t)y * mp.RowPitch;
                        for (x = 0; x < sgw; x += 16) { sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]; n++; }
                    }
                    ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)stg, 0);
                    cnt++;
                    fprintf(stderr, "  [PRES] #%u vsync=%d valid=%d mean=%.1f frame=%u tick=%lu%c", cnt, vsync, s_hw.pres_valid, sum / (3.0 * n), (unsigned)s_hw.frame, (unsigned long)GetTickCount(), 10);
                }
            }
            ID3D11Texture2D_Release(bbt);
        }
    }
    /* A flip waits for the host's vertical blank, which is what paces a title
     * that draws as fast as it is allowed to; repaints do not. */
    QueryPerformanceCounter(&t0);
    if (s_hw.tearing)   /* never blocks on the host monitor; hw_flip paces to the console rate */
        IDXGISwapChain_Present(s_hw.swap, 0, DXGI_PRESENT_ALLOW_TEARING);
    else
        IDXGISwapChain_Present(s_hw.swap, vsync && s_hw.vsync ? 1 : 0, 0);
    QueryPerformanceCounter(&t1);
    s_hw.present_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
}

/* NV097_FLIP_STALL: the frame the title just finished becomes the one shown. */
/* The console's display gamma ramp (D3DDevice::SetGammaRamp, which Phantom
 * Dust's Screen Brightness option drives): 256 R, 256 G, 256 B bytes. The game
 * programs it through the DAC ports, which are plain memory here, so the project
 * hands the ramp over by hook. Applied as a per-channel lookup in the final
 * present pass; identity until a ramp arrives. */
static uint8_t s_gamma_ramp[768];
static volatile LONG s_gamma_serial;
static int s_gamma_set;

void xbox_SetGammaRamp(const uint8_t *rgb768)
{
    memcpy(s_gamma_ramp, rgb768, 768);
    s_gamma_set = 1;
    InterlockedIncrement(&s_gamma_serial);
}

/* Make sure the lookup texture exists and matches the current ramp. */
static void hw_gamma_update(void)
{
    uint32_t px[256];
    int i;
    LONG ser = s_gamma_serial;
    if (!s_hw.lut_tex) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SAMPLER_DESC sm;
        memset(&td, 0, sizeof td);
        td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &s_hw.lut_tex))) return;
        ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)s_hw.lut_tex, NULL, &s_hw.lut_srv);
        memset(&sm, 0, sizeof sm);
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(s_hw.dev, &sm, &s_hw.samp_lut);
        s_hw.lut_serial = ser - 1;
    }
    if (s_hw.lut_serial == ser)
        return;
    for (i = 0; i < 256; i++) {
        uint32_t r = s_gamma_set ? s_gamma_ramp[i] : (uint32_t)i;
        uint32_t g = s_gamma_set ? s_gamma_ramp[256 + i] : (uint32_t)i;
        uint32_t b = s_gamma_set ? s_gamma_ramp[512 + i] : (uint32_t)i;
        px[i] = 0xFF000000u | (r << 16) | (g << 8) | b;      /* B8G8R8A8 */
    }
    ID3D11DeviceContext_UpdateSubresource(s_hw.ctx, (ID3D11Resource *)s_hw.lut_tex, 0, NULL, px, 256 * 4, 0);
    s_hw.lut_serial = ser;
}

/* RECOMP_HW_FLICKERDBG: after each completed frame is copied for presentation,
 * read it back, take its mean brightness, and report frames that differ sharply
 * from the median of the last few (and save the first few as BMPs under
 * dumps/flk_*.bmp). Debug only: it stalls the GPU every frame. */
static void hw_flicker_probe(void)
{
    static double hist[9]; static int nh, saved; static unsigned fno;
    static ID3D11Texture2D *stage; static UINT sw, sh;
    D3D11_TEXTURE2D_DESC td; D3D11_MAPPED_SUBRESOURCE mp;
    double sum = 0, med, tmp[9]; unsigned n = 0, x, y; int i, j;

    if (!s_hw.pres_tex) return;
    ID3D11Texture2D_GetDesc(s_hw.pres_tex, &td);
    if (!stage || sw != td.Width || sh != td.Height) {
        D3D11_TEXTURE2D_DESC sd = td;
        if (stage) ID3D11Texture2D_Release(stage);
        stage = NULL;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &sd, NULL, &stage))) return;
        sw = td.Width; sh = td.Height;
    }
    ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)stage, (ID3D11Resource *)s_hw.pres_tex);
    if (FAILED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &mp))) return;
    for (y = 0; y < sh; y += 8) {
        const uint8_t *row = (const uint8_t *)mp.pData + (size_t)y * mp.RowPitch;
        for (x = 0; x < sw; x += 8) { sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]; n++; }
    }
    fno++;
    sum /= (3.0 * n);
    if (nh >= 5) {
        for (i = 0; i < nh; i++) tmp[i] = hist[i];
        for (i = 0; i < nh; i++) for (j = i + 1; j < nh; j++) if (tmp[j] < tmp[i]) { double t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
        med = tmp[nh / 2];
        if (med > 4.0 && (sum < med * 0.6 || sum > med * 1.6)) {
            {
                volatile uint32_t *crtc = (volatile uint32_t *)((uintptr_t)(0xFD000000u + 0x600800u) + xbox_GetMemoryOffset());
                fprintf(stderr, "  [FLK] frame %u brightness %.1f vs median %.1f  tick=%lu  front=%08X %ux%u  PCRTC_START=%08X\n", fno, sum, med, (unsigned long)GetTickCount(),
                        s_hw.front ? (unsigned)s_hw.front->offset : 0u, s_hw.front ? (unsigned)s_hw.front->w : 0u, s_hw.front ? (unsigned)s_hw.front->h : 0u, (unsigned)*crtc);
            }
            { int q; for (q = 0; q < HW_MAX_SURFACES; q++) { HwSurface *t = &s_hw.surf[q];
                if (t->tex && t->w >= 320 && t->drawn_frame + 1 >= s_hw.frame)
                    fprintf(stderr, "     [FLK-surf] %08X %ux%u drawn_frame=%u (frame %u)\n", (unsigned)t->offset, (unsigned)t->w, (unsigned)t->h, (unsigned)t->drawn_frame, (unsigned)s_hw.frame); } }
            if (saved < 8 && fno > 5000) {
                char fn[64]; FILE *f; uint32_t hdr[14] = {0}; unsigned yy;
                sprintf(fn, "dumps/flk_%u.bmp", fno);
                f = fopen(fn, "wb");
                if (f) {
                    uint16_t magic = 0x4D42; uint32_t isz = sw * sh * 4;
                    hdr[0] = 54 + isz; hdr[2] = 54; hdr[3] = 40; hdr[4] = sw; hdr[5] = (uint32_t)-(int)sh; hdr[6] = 1 | (32u << 16); hdr[8] = isz;
                    fwrite(&magic, 2, 1, f); fwrite(hdr, 4, 13, f); /* 2 + 52 = 54 */
                    for (yy = 0; yy < sh; yy++) fwrite((const uint8_t *)mp.pData + (size_t)yy * mp.RowPitch, 4, sw, f);
                    fclose(f); saved++;
                }
            }
            /* do not let the outlier into the history */
            ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)stage, 0);
            return;
        }
    }
    ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)stage, 0);
    if (nh < 9) hist[nh++] = sum; else { for (i = 0; i < 8; i++) hist[i] = hist[i + 1]; hist[8] = sum; }
}


/* RECOMP_PERF=1: a [PERF] line every second: flips, frame interval avg/p99/max (ms),
 * time inside Present, in the pushbuffer executor and in draw calls (ms per second),
 * process CPU use in cores. */
static void hw_perf_flip(void)
{
    static int on = -1;
    static LARGE_INTEGER qf, lastq, t0;
    static double iv[512]; static int n;
    static uint64_t pr0, ex0, dr0;
    static ULONGLONG cpu0;
    LARGE_INTEGER q;
    if (on < 0) { const char *e = getenv("RECOMP_PERF"); on = e ? atoi(e) : 0; }
    if (!on) return;
    QueryPerformanceCounter(&q);
    s_hw_psdiag = GetFileAttributesA("psdiag.flag") != INVALID_FILE_ATTRIBUTES;
    s_hw_psspec_off = GetFileAttributesA("psspec_off.flag") != INVALID_FILE_ATTRIBUTES;
    if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); t0 = q; lastq = q; return; }
    if (n < 512) iv[n++] = (double)(q.QuadPart - lastq.QuadPart) * 1000.0 / (double)qf.QuadPart;
    lastq = q;
    if ((double)(q.QuadPart - t0.QuadPart) / (double)qf.QuadPart >= 1.0) {
        double secs = (double)(q.QuadPart - t0.QuadPart) / (double)qf.QuadPart, sum = 0, mx = 0, p99;
        int i, j, slow = 0;
        FILETIME c, x, k, u; ULONGLONG cpu;
        double ms = 1000.0 / (double)qf.QuadPart;
        for (i = 0; i < n; i++) { sum += iv[i]; if (iv[i] > mx) mx = iv[i]; if (iv[i] > 50.0) slow++; }
        for (i = 1; i < n; i++) { double v = iv[i]; for (j = i - 1; j >= 0 && iv[j] > v; j--) iv[j + 1] = iv[j]; iv[j + 1] = v; }
        p99 = n ? iv[(int)((n - 1) * 0.99 + 0.5)] : 0;
        GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
        cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
        fprintf(stderr, "  [PERF] flips %d in %.2fs (%.1f fps) interval avg %.1f p99 %.1f max %.1f ms, >50ms %d; per s: present %.0f ms, executor %.0f ms, draws %.0f ms; psfb %ld; cpu %.2f cores\n",
                n, secs, n / secs, n ? sum / n : 0.0, p99, mx, slow,
                (double)(s_hw.present_ticks - pr0) * ms / secs, (double)(g_nv2a_exec_ticks - ex0) * ms / secs,
                (double)(s_hw_stat_draw_ticks - dr0) * ms / secs, (long)InterlockedExchange(&s_hw_psv_fb, 0),
                cpu0 ? (double)(cpu - cpu0) / 1e7 / secs : 0.0);
        if (on >= 2) {   /* RECOMP_PERF=2: busiest threads this second (cores); the snapshot itself costs ~40 ms */
            static struct { DWORD tid; ULONGLONG t; } prev[256]; static int np;
            struct { DWORD tid; ULONGLONG d; } top[4] = {{0,0},{0,0},{0,0},{0,0}};
            HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            int nn = 0;
            static struct { DWORD tid; ULONGLONG t; } cur[256];
            if (sn != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te; te.dwSize = sizeof te;
                if (Thread32First(sn, &te)) do {
                    if (te.th32OwnerProcessID == GetCurrentProcessId() && nn < 256) {
                        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                        if (th) {
                            FILETIME c2, x2, k2, u2;
                            if (GetThreadTimes(th, &c2, &x2, &k2, &u2)) {
                                ULONGLONG tt = (((ULONGLONG)k2.dwHighDateTime << 32) | k2.dwLowDateTime) + (((ULONGLONG)u2.dwHighDateTime << 32) | u2.dwLowDateTime);
                                ULONGLONG d = tt; int q;
                                for (q = 0; q < np; q++) if (prev[q].tid == te.th32ThreadID) { d = tt - prev[q].t; break; }
                                cur[nn].tid = te.th32ThreadID; cur[nn].t = tt; nn++;
                                for (q = 0; q < 4; q++) if (d > top[q].d) {
                                    int z; for (z = 3; z > q; z--) top[z] = top[z - 1];
                                    top[q].tid = te.th32ThreadID; top[q].d = d; break;
                                }
                            }
                            CloseHandle(th);
                        }
                    }
                } while (Thread32Next(sn, &te));
                CloseHandle(sn);
            }
            memcpy(prev, cur, sizeof(cur[0]) * (size_t)nn); np = nn;
            fprintf(stderr, "  [PERF-PSV] variants queued %d ready %ld failed %ld off %d\n", s_hw_psv_n,
                    (long)s_hw_psv_ready, (long)s_hw_psv_failed, (int)(s_hw_psspec_off || s_hw_psspec_env == 0));
            fprintf(stderr, "  [PERF-THR] exec-thread %lu; top:", (unsigned long)GetCurrentThreadId());
            for (i = 0; i < 4; i++) fprintf(stderr, " %lu=%.2f", (unsigned long)top[i].tid, (double)top[i].d / 1e7 / secs);
            fprintf(stderr, " cores\n");
        }
        cpu0 = cpu; pr0 = s_hw.present_ticks; ex0 = g_nv2a_exec_ticks; dr0 = s_hw_stat_draw_ticks;
        n = 0; t0 = q;
    }
}

static void hw_flip(void)
{
    unsigned s_hw_framedraws_at_flip = s_hw_framedraws;
    if (s_hw.pace_hz > 0.0) {
        /* Presentation is paced here, by the console's rate, not by the host monitor:
         * a 30 Hz or 144 Hz display must not change how often the title may flip.
         * Sleep most of the wait, spin the rest. */
        static LARGE_INTEGER pf, last;
        LARGE_INTEGER q;
        if (!pf.QuadPart) QueryPerformanceFrequency(&pf);
        for (;;) {
            double rem;
            QueryPerformanceCounter(&q);
            rem = (double)(last.QuadPart + (LONGLONG)((double)pf.QuadPart / s_hw.pace_hz) - q.QuadPart) * 1000.0 / (double)pf.QuadPart;
            if (rem <= 0.0) break;
            if (rem > 2.5) Sleep((DWORD)(rem - 2.0)); else YieldProcessor();
        }
        last = q;
    }
    if (getenv("RECOMP_HW_FLICKERDBG")) {
        static unsigned prev; static DWORD last; static unsigned hits;
        unsigned d = s_hw_framedraws; DWORD now = GetTickCount();
        if (prev > 200 && hits < 400 && (d < prev * 6 / 10 || d > prev * 16 / 10)) {
            hits++; fprintf(stderr, "  [FLK] draws %u (prev %u) frame_ms %lu tick=%lu\n", d, prev, (unsigned long)(now - last), (unsigned long)now);
        } else if (last && now - last > 70 && hits < 400) {
            hits++; fprintf(stderr, "  [FLK] slow frame %lu ms (draws %u) tick=%lu\n", (unsigned long)(now - last), d, (unsigned long)now);
        }
        prev = d; last = now;
    }
    hw_maxdraw_refresh();
    EnterCriticalSection(&s_hw.lock);
    hw_glow_dump_check(s_hw.biggest, 1);
    /* Which surface is the picture? The largest one drawn this frame is usually
     * right, but the same memory is sometimes drawn under a second, larger shape
     * (a 1280x512 or 640x512 alias of a 640x480 display buffer, used by an effect
     * pass and left black elsewhere), and presenting that is a black flash.
     * Remember the display shape -- adopted once a new shape wins three flips in
     * a row, so a real mode change still works -- and prefer a surface of
     * exactly that shape. */
    if (s_hw.biggest) {
        HwSurface *best = NULL; int q;
        if (s_hw.biggest->w == s_hw.cand_w && s_hw.biggest->h == s_hw.cand_h) {
            if (s_hw.cand_streak < 1000) s_hw.cand_streak++;
        } else {
            s_hw.cand_w = s_hw.biggest->w; s_hw.cand_h = s_hw.biggest->h; s_hw.cand_streak = 1;
        }
        if (!s_hw.disp_w || (s_hw.cand_streak >= 3 && (s_hw.cand_w != s_hw.disp_w || s_hw.cand_h != s_hw.disp_h) && s_hw.cand_w >= 640)) {
            s_hw.disp_w = s_hw.cand_w; s_hw.disp_h = s_hw.cand_h;
        }
        for (q = 0; q < HW_MAX_SURFACES; q++) {
            HwSurface *t = &s_hw.surf[q];
            if (t->tex && t->w == s_hw.disp_w && t->h == s_hw.disp_h && t->drawn_frame == s_hw.frame
                && (!best || t->w * t->h >= best->w * best->h))
                best = t;
        }
        if (best && best != s_hw.biggest) {
            if (getenv("RECOMP_HW_FLICKERDBG"))
                fprintf(stderr, "  [FLK] present %08X %ux%u instead of %08X %ux%u\n", (unsigned)best->offset, (unsigned)best->w, (unsigned)best->h,
                        (unsigned)s_hw.biggest->offset, (unsigned)s_hw.biggest->w, (unsigned)s_hw.biggest->h);
            s_hw.biggest = best;
        }
    }
    if (s_hw.biggest && getenv("RECOMP_HW_FLIPDBG")) {
        static unsigned n; if (n++ < 400000)
            fprintf(stderr, "  [FLIP] #%u pres %08X %ux%u draws %u disp %ux%u\n", s_hw.frame, (unsigned)s_hw.biggest->offset,
                    (unsigned)s_hw.biggest->w, (unsigned)s_hw.biggest->h, s_hw_framedraws_at_flip, (unsigned)s_hw.disp_w, (unsigned)s_hw.disp_h);
    }
    if (s_hw.biggest) {
        /* A flip whose only work was a clear (no draw batches) right after a
         * real frame is a half-built frame, not a picture: showing it flashes
         * black. Hold the previous picture for that one flip; a second clear-only
         * flip in a row is a deliberate fade/black and is shown. */
        unsigned d = s_hw_framedraws_at_flip;
        int clear_only = (d == 0);
        if (clear_only && s_hw.flip_prev_draws > 100 && !s_hw.flip_held && s_hw.pres_valid) {
            s_hw.flip_held = 1;
            if (getenv("RECOMP_HW_FLICKERDBG")) fprintf(stderr, "  [FLK] held a clear-only flip (prev draws %u)\n", s_hw.flip_prev_draws);
        } else {
            D3D11_TEXTURE2D_DESC td;
            s_hw.flip_held = 0;
            s_hw.front = s_hw.biggest;
            ID3D11Texture2D_GetDesc(s_hw.biggest->tex, &td);
            /* One of the two display buffers can be registered taller than the
             * display (640x512 for a 640x480 picture, a race in how the title
             * sets it up): same width, so same row pitch, and the picture is its
             * top rows. Present only those, or the picture changes shape every
             * other frame. Sizes are logical; the texture may be render-scaled. */
            {
                HwSurface *b = s_hw.biggest;
                uint32_t keep = 0;
                if (s_hw.disp_w && b->w == s_hw.disp_w && b->h > s_hw.disp_h && b->h <= s_hw.disp_h + 64)
                    keep = s_hw.disp_h;
                if (b->w == 640 && b->h > 480 && b->h <= 544)
                    keep = 480;   /* NTSC display, whatever shape was adopted before */
                if (keep)
                    td.Height = (UINT)((uint64_t)td.Height * keep / b->h);
            }
            if (!s_hw.pres_tex || s_hw.pres_w != td.Width || s_hw.pres_h != td.Height) {
                if (s_hw.pres_srv) ID3D11ShaderResourceView_Release(s_hw.pres_srv);
                if (s_hw.pres_tex) ID3D11Texture2D_Release(s_hw.pres_tex);
                s_hw.pres_srv = NULL; s_hw.pres_tex = NULL;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = 0;
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &s_hw.pres_tex)))
                    ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)s_hw.pres_tex, NULL, &s_hw.pres_srv);
                s_hw.pres_w = td.Width; s_hw.pres_h = td.Height;
            }
            if (s_hw.pres_tex) {
                D3D11_BOX cbox; cbox.left = 0; cbox.top = 0; cbox.front = 0;
                cbox.right = td.Width; cbox.bottom = td.Height; cbox.back = 1;
                ID3D11DeviceContext_CopySubresourceRegion(s_hw.ctx, (ID3D11Resource *)s_hw.pres_tex, 0, 0, 0, 0,
                                                          (ID3D11Resource *)s_hw.biggest->tex, 0, &cbox);
                s_hw.pres_valid = 1;
                if (getenv("RECOMP_HW_FLICKERDBG")) hw_flicker_probe();
            }
            s_hw.flip_prev_draws = clear_only ? 0 : d;
        }
    }
    {
        /* The frame's view clip: the largest clip that is used by at least 8 draws (a one-off
         * full-screen quad must not widen it). Keeps the previous choice for a frame with none. */
        int k, best = -1;
        uint64_t barea = 0;
        for (k = 0; k < s_hw_vc_n; k++) {
            uint64_t a = (uint64_t)s_hw_vc[k].cw * s_hw_vc[k].ch;
            if (s_hw_vc[k].n >= 8 && a > barea) { barea = a; best = k; }
        }
        if (best >= 0) {
            s_hw_view[0] = s_hw_vc[best].cx; s_hw_view[1] = s_hw_vc[best].cy;
            s_hw_view[2] = s_hw_vc[best].cw; s_hw_view[3] = s_hw_vc[best].ch;
        }
        s_hw_vc_n = 0;
    }
    s_hw.biggest = NULL;
    s_hw.biggest_area = 0;
    s_hw.frame++;
    {
        /* Every five seconds: what a frame costs, so "slow" has a reason. */
        static DWORD t0;
        static uint64_t d0, tr0, up0, vs0, dr0, vx0, pr0, ex0, gb0, cb0;
        LARGE_INTEGER qf;
        static uint32_t f0;
        DWORD now = GetTickCount();
        if (now - t0 >= 5000) {
            uint32_t frames = s_hw.frame - f0;
            QueryPerformanceFrequency(&qf);
            if (t0 && frames) {
                double ms = 1000.0 / (double)qf.QuadPart / frames;
                double frame_ms = (double)(now - t0) / frames;
                double exec_ms = (double)(g_nv2a_exec_ticks - ex0) * ms;
                double present_ms = (double)(s_hw.present_ticks - pr0) * ms;
                /* "executor" is everything done for the pushbuffer except the
                 * wait in Present; "title" is what is left of the frame. */
                fprintf(stderr, "  [HW] %.1f fps (%.1f ms); per frame: %llu draws"
                                " (%llu GPU-transformed, %llu interpreted), %llu triangles,"
                                " %.1f texture uploads, %llu vertices;"
                                " %.1f ms interpreter, %.1f ms draw calls,"
                                " %.1f ms executor, %.1f ms present, %.1f ms title\n",
                        frames * 1000.0 / (now - t0), frame_ms,
                        (unsigned long long)((s_hw.draws - d0) / frames),
                        (unsigned long long)((s_hw.gpu_vsh_batches - gb0) / frames),
                        (unsigned long long)((s_hw.cpu_vsh_batches - cb0) / frames),
                        (unsigned long long)((s_hw.tris - tr0) / frames),
                        (double)(s_hw.tex_uploads - up0) / frames,
                        (unsigned long long)((s_hw_stat_verts - vx0) / frames),
                        (double)(s_hw_stat_vsh_ticks - vs0) * ms,
                        (double)(s_hw_stat_draw_ticks - dr0) * ms,
                        exec_ms - present_ms, present_ms, frame_ms - exec_ms);
            }
            if (getenv("RECOMP_HW_CLIPDBG") && s_hw_clip_n) {
                int k;
                for (k = 0; k < s_hw_clip_n; k++)
                    fprintf(stderr, "  [CLIPH] surf %08X %ux%u clip %ux%u+%u+%u : %u draws%c", (unsigned)s_hw_clip[k].off,
                            (unsigned)s_hw_clip[k].sw, (unsigned)s_hw_clip[k].sh, (unsigned)s_hw_clip[k].cw, (unsigned)s_hw_clip[k].ch,
                            (unsigned)s_hw_clip[k].cx, (unsigned)s_hw_clip[k].cy, s_hw_clip[k].n, 10);
                s_hw_clip_n = 0;
            }
            t0 = now; f0 = s_hw.frame;
            d0 = s_hw.draws; tr0 = s_hw.tris; up0 = s_hw.tex_uploads;
            vs0 = s_hw_stat_vsh_ticks; dr0 = s_hw_stat_draw_ticks; vx0 = s_hw_stat_verts;
            pr0 = s_hw.present_ticks; ex0 = g_nv2a_exec_ticks;
            gb0 = s_hw.gpu_vsh_batches; cb0 = s_hw.cpu_vsh_batches;
        }
    }
    s_hw.last_flip_ms = GetTickCount();
    hw_present_locked(1);
    hw_perf_flip();
    LeaveCriticalSection(&s_hw.lock);
}

/* Called by the window thread about 60 times a second. A title that is not
 * flipping (a movie playing through the overlay, a load) still needs its
 * window repainted; one that is flipping presents from hw_flip instead. */
void nv2a_hw_idle_present(void)
{
    if (!s_hw.ok)
        return;
    if (GetTickCount() - s_hw.last_flip_ms < 100)
        return;
    EnterCriticalSection(&s_hw.lock);
    {
        static unsigned idle_n; static DWORD idle_t0;
        idle_n++;
        if (getenv("RECOMP_HW_FLICKERDBG") && 1)
            fprintf(stderr, "  [HW] idle present #%u (%lu ms since last flip) tick=%lu\n", idle_n,
                    (unsigned long)(GetTickCount() - s_hw.last_flip_ms), (unsigned long)GetTickCount());
        (void)idle_t0;
    }
    hw_present_locked(0);
    LeaveCriticalSection(&s_hw.lock);
}

/* Copy the shown frame into a caller buffer of w x h XRGB pixels (for the
 * window's BMP dump). Returns 0 when there is nothing to copy. */
int nv2a_hw_readback(uint32_t *dst, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11Texture2D *stage = NULL;
    uint32_t x, y;
    int ok = 0;

    if (!s_hw.ok)
        return 0;
    EnterCriticalSection(&s_hw.lock);
    if (s_hw.front && s_hw.front->tex) {
        ID3D11Texture2D_GetDesc(s_hw.front->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &stage))) {
            ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)stage,
                                             (ID3D11Resource *)s_hw.front->tex);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)stage, 0,
                                                  D3D11_MAP_READ, 0, &map))) {
                for (y = 0; y < h; y++) {
                    const uint32_t *row = (const uint32_t *)
                        ((const uint8_t *)map.pData
                         + (size_t)(y * td.Height / h) * map.RowPitch);
                    for (x = 0; x < w; x++)
                        dst[(size_t)y * w + x] = row[x * td.Width / w];
                }
                ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)stage, 0);
                ok = 1;
            }
            ID3D11Texture2D_Release(stage);
        }
    }
    LeaveCriticalSection(&s_hw.lock);
    return ok;
}

/* Methods only the hardware path acts on. Returns 1 when it took the method. */
static int hw_method(uint32_t method, uint32_t param)
{
    switch (method) {
    case 0x0308: s_hw.cull_enable = param; return 1;   /* CULL_FACE_ENABLE */
    case 0x039C: s_hw.cull_face = param;   return 1;   /* CULL_FACE        */
    case 0x03A0: s_hw.front_face = param;  return 1;   /* FRONT_FACE       */
    case 0x1D98: s_hw.clear_rect_h = param; s_hw.clear_rect_set = 1; return 1;
    case 0x1D9C: s_hw.clear_rect_v = param; s_hw.clear_rect_set = 1; return 1;
    default:     return 0;
    }
}

#else  /* !_WIN32 */
static int  hw_active(void) { return 0; }
static void hw_draw_batch(const Nv2aVshOutput *v, uint32_t n) { (void)v; (void)n; }
static int  hw_draw_program_batch(void) { return 0; }
static void hw_clear(uint32_t param) { (void)param; }
static void hw_flip(void) {}
void xbox_SetGammaRamp(const uint8_t *rgb768) { (void)rgb768; }
static int  hw_method(uint32_t method, uint32_t param) { (void)method; (void)param; return 0; }
void nv2a_hw_idle_present(void) {}
int  nv2a_hw_readback(uint32_t *dst, uint32_t w, uint32_t h) { (void)dst; (void)w; (void)h; return 0; }
#endif
