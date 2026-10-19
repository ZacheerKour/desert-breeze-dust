/**
 * Render-side frame interpolation for the D3D11 presenter (worker i, 2026-10-19).
 *
 * The title's simulation advances one fixed step per frame at 30 Hz, so the
 * game cannot simply be run at 60. Instead every real frame is also recorded as
 * a command list (clears and draws with the complete bound state and a copy of
 * the vertex data / vertex-program constants). At the flip of frame N the list
 * of frame N is replayed into private "twin" render targets with the data of
 * each draw moved half way back toward the matching draw of frame N-1; that
 * image is presented first, and the real frame N follows 16.7 ms later from a
 * presenter thread. Game logic and audio are untouched.
 *
 * Opt-in: RECOMP_INTERP=1. Tuning / diagnostics:
 *   RECOMP_INTERP_DBG=1      one [IP] line per second (matches, cuts, timings)
 *   RECOMP_INTERP_GOV=0      disable the governor: it switches interpolation off while the game's own flip interval
 *                            averages above RECOMP_INTERP_GOVMS (36.5 ms; the extra GPU/CPU work would slow the
 *                            simulation, which runs one step per flip), and retries after 4 s, doubling up to 30 s
 *   RECOMP_INTERP_T=<0..1>   blend position of the extra frame (default 0.5)
 *   RECOMP_INTERP_PROG=<f>   program draws: max relative constant change that is still blended (1.0)
 *   RECOMP_INTERP_HIREL=<f>, RECOMP_INTERP_HIFRAC=<f>  scene cut when more than HIFRAC (0.4) of the program draws
 *                            changed by more than HIREL (0.5)
 *   interp_tune.txt (working directory, re-read every 16 flips): "<prog_max> <hirel> <hifrac> <attr> <cpu>"
 *   RECOMP_INTERP_ATTR=<f>   moving vertex data: max shift as a fraction of the draw's extent (0.25)
 *   RECOMP_INTERP_CPU=<f>    CPU-transformed draws: same, in screen space (0.75)
 *   RECOMP_INTERP_CUT=<f>    fraction of unmatched / rejected draws that makes the frame a cut (0.5)
 *   RECOMP_INTERP_DUMP=<prefix>  when the file interp_dump.flag exists (deleted after use), the next interpolated frame: <prefix>_<n>_i.bmp (interpolated),
 *                            _a.bmp (previous real) and _b.bmp (real N), in the working directory
 *   RECOMP_INTERP_FIXED=1    keep every constant of every program draw of frame N (position test)
 * Included by nv2a_hw_d3d11.inc before hw_bind_draw_state.
 */

static void hw_present_locked(int vsync);
static int hw_ring_map(ID3D11Buffer *buf, uint32_t *off, uint32_t size,
                       uint32_t bytes, uint32_t align, D3D11_MAPPED_SUBRESOURCE *map);
static void hw_dump_surface(ID3D11Texture2D *tex, const char *path);

#define IP_HDR_FLOATS (16 + NV_VERTEX_ATTRS * 4)
#define IP_CFLOATS    (NV2A_VSH_CONSTANTS * 4)

typedef struct {
    uint8_t kind;                       /* 0 CPU-transformed draw, 1 program draw, 2 clear */
    uint8_t lerp;                       /* planned: blend this draw */
    uint8_t first_touch_full_clear;
    int8_t surf, depth;                 /* indices in s_hw.surf / s_hw.depth, -1 none */
    int32_t partner;                    /* index in the previous frame, -1 none */
    uint32_t soff, sw, sh;
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    ID3D11RasterizerState *rs;
    ID3D11BlendState *bs;
    float bf[4];
    ID3D11DepthStencilState *dss;
    UINT sref;
    ID3D11PixelShader *ps;
    HwPsConst pc;
    ID3D11ShaderResourceView *srv[4];
    int8_t srv_surf[4];                 /* >= 0: a render target of that index */
    int8_t srv_self[4];                 /* ... which is the target itself (copy taken before the draw) */
    uint32_t toff[4];                   /* texture identity for matching */
    ID3D11SamplerState *smp[4];
    ID3D11VertexShader *vs;
    uint32_t vskey, vmask;
    float hdr[IP_HDR_FLOATS];
    uint32_t cofs;                      /* arena offset of the program constants (kind 1) */
    uint32_t vofs, nvert;               /* arena: HwVertex[] (kind 0) / slot vertices, 128 bytes each (kind 1) */
    uint32_t iofs, nidx;                /* arena: uint16 indices (kind 1) */
    uint32_t mkey;
    /* clear */
    uint32_t cparam, cx0, cy0, cx1, cy1;
    float ccol[4], cz;
    uint8_t cst;
} IpRec;

typedef struct {
    IpRec *r; int n, cap, ndraw;
    uint8_t *ar; size_t asz, acap;
    int *tab; int tabsz;
    uint32_t last_cgen; uint32_t last_cofs; int last_c_valid;
} IpFrame;

typedef struct { ID3D11Texture2D *tex; ID3D11RenderTargetView *rtv; ID3D11ShaderResourceView *srv;
                 UINT w, h; uint32_t epoch; } IpTwin;
typedef struct { ID3D11Texture2D *tex; ID3D11DepthStencilView *dsv; UINT w, h; uint32_t epoch; } IpTwinD;

static struct {
    int enabled, env_on, dbg, dbg_level, tried;
    int gov, gov_on; double gov_hi, gov_ema, gov_off_until, gov_backoff, gov_on_since; LONGLONG gov_last;
    float t, prog_max, attr_frac, cpu_frac, cut_frac, hi_rel, hi_frac;
    int fixed;
    const char *dump;
    IpFrame f[2];
    int cur, prev_valid;
    uint32_t epoch;
    IpTwin tw[HW_MAX_SURFACES];
    IpTwinD twd[HW_MAX_DEPTHS];
    struct { ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; UINT w, h; } shadow[HW_MAX_SURFACES];
    ID3D11Texture2D *out_tex; ID3D11ShaderResourceView *out_srv; UINT out_w, out_h;
    ID3D11ShaderResourceView *show;     /* what the next present shows instead of the real frame */
    /* presenter thread */
    HANDLE ev, th;
    volatile LONG pend;                 /* a real frame is waiting to be presented */
    volatile LONG pend_serial;
    LONGLONG deadline;                  /* QPC */
    LARGE_INTEGER qf;
    /* stats */
    unsigned st_frames, st_shown, st_cut_match, st_cut_prog, st_cut_other, st_draws, st_matched, st_lerped, st_fail;
    double st_ms;
    DWORD st_t0;
    unsigned dump_n, st_shown_total, hist[8];
    ID3D11Texture2D *dump_prev;
} s_ip;

static IpRec s_ip_cap;                  /* the last bound draw state, committed by the draw call */
static int s_ip_cap_ok;

static void ip_init(void)
{
    const char *e;
    if (s_ip.tried) return;
    s_ip.tried = 1;
    e = getenv("RECOMP_INTERP");
    /* On by default (60 presented frames over the title's 30, game speed unchanged; the governor
     * falls back to 30 where the guest cannot hold its own frame rate). RECOMP_INTERP=0 turns it off. */
    s_ip.enabled = s_ip.env_on = !(e && *e == '0');
    e = getenv("RECOMP_INTERP_DBG"); s_ip.dbg = e && *e && *e != '0'; s_ip.dbg_level = e ? atoi(e) : 0;
    s_ip.t = (e = getenv("RECOMP_INTERP_T")) ? (float)atof(e) : 0.5f;
    s_ip.prog_max = (e = getenv("RECOMP_INTERP_PROG")) ? (float)atof(e) : 1.0f;
    s_ip.hi_rel = (e = getenv("RECOMP_INTERP_HIREL")) ? (float)atof(e) : 0.5f;
    s_ip.hi_frac = (e = getenv("RECOMP_INTERP_HIFRAC")) ? (float)atof(e) : 0.4f;
    s_ip.attr_frac = (e = getenv("RECOMP_INTERP_ATTR")) ? (float)atof(e) : 0.25f;
    s_ip.cpu_frac = (e = getenv("RECOMP_INTERP_CPU")) ? (float)atof(e) : 0.75f;
    s_ip.cut_frac = (e = getenv("RECOMP_INTERP_CUT")) ? (float)atof(e) : 0.5f;
    e = getenv("RECOMP_INTERP_FIXED"); s_ip.fixed = e && *e && *e != '0';
    s_ip.dump = getenv("RECOMP_INTERP_DUMP");
    QueryPerformanceFrequency(&s_ip.qf);
    e = getenv("RECOMP_INTERP_GOV"); s_ip.gov = !(e && *e == '0');
    s_ip.gov_hi = (e = getenv("RECOMP_INTERP_GOVMS")) ? atof(e) : 36.5;
    s_ip.gov_on = 1; s_ip.gov_ema = 33.3; s_ip.gov_backoff = 4.0;
}

/* ---- arena / frames ---------------------------------------------------- */

static uint32_t ip_alloc(IpFrame *f, size_t bytes)
{
    size_t at = (f->asz + 15) & ~(size_t)15;
    if (at + bytes > f->acap) {
        size_t nc = f->acap ? f->acap : (4u << 20);
        while (at + bytes > nc) nc *= 2;
        f->ar = (uint8_t *)realloc(f->ar, nc);
        f->acap = nc;
        if (!f->ar) { f->acap = f->asz = 0; return 0; }
    }
    f->asz = at + bytes;
    return (uint32_t)at;
}

static void ip_rec_release(IpRec *r)
{
    int i;
    if (r->kind == 2) return;
    if (r->bs) ID3D11BlendState_Release(r->bs);
    if (r->dss) ID3D11DepthStencilState_Release(r->dss);
    if (r->ps) ID3D11PixelShader_Release(r->ps);
    if (r->vs) ID3D11VertexShader_Release(r->vs);
    for (i = 0; i < 4; i++)
        if (r->srv[i]) ID3D11ShaderResourceView_Release(r->srv[i]);
    r->bs = NULL; r->dss = NULL; r->ps = NULL; r->vs = NULL;
    for (i = 0; i < 4; i++) r->srv[i] = NULL;
}

static void ip_frame_reset(IpFrame *f)
{
    int i;
    for (i = 0; i < f->n; i++) ip_rec_release(&f->r[i]);
    f->n = 0; f->ndraw = 0; f->asz = 0; f->last_c_valid = 0;
}

static IpRec *ip_new_rec(IpFrame *f)
{
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 1024;
        IpRec *nr = (IpRec *)realloc(f->r, (size_t)nc * sizeof(IpRec));
        if (!nr) return NULL;
        f->r = nr; f->cap = nc;
    }
    return &f->r[f->n++];
}

/* ---- capture ----------------------------------------------------------- */

static void ip_capture(const HwSurface *s, const HwDepth *d, const HwVsp *prog, const D3D11_VIEWPORT *vp,
                       const D3D11_RECT *sc, ID3D11RasterizerState *rs, ID3D11BlendState *bs, const float bf[4],
                       ID3D11DepthStencilState *dss, ID3D11PixelShader *ps, const HwPsConst *pc,
                       ID3D11ShaderResourceView *const srv[4], ID3D11SamplerState *const smp[4],
                       const HwSurface *const alias[4], const int self[4], const float *hdr)
{
    IpRec *c = &s_ip_cap;
    int i;
    memset(c, 0, sizeof *c);
    c->kind = prog ? 1 : 0;
    c->surf = (int8_t)(s - s_hw.surf);
    c->depth = d ? (int8_t)(d - s_hw.depth) : -1;
    c->soff = s->offset; c->sw = s->w; c->sh = s->h;
    c->vp = *vp; c->sc = *sc; c->rs = rs; c->bs = bs; memcpy(c->bf, bf, 16);
    c->dss = dss; c->sref = s_gpu.stencil_ref & 0xFF; c->ps = ps; c->pc = *pc;
    for (i = 0; i < 4; i++) {
        c->srv[i] = srv[i]; c->smp[i] = smp[i];
        c->srv_surf[i] = alias[i] ? (int8_t)(alias[i] - s_hw.surf) : -1;
        c->srv_self[i] = (int8_t)self[i];
        c->toff[i] = s_gpu.texs[i].valid ? s_gpu.texs[i].offset : 0;
    }
    if (prog) { c->vs = prog->vs; c->vskey = prog->key; c->vmask = prog->mask; }
    memcpy(c->hdr, hdr, sizeof c->hdr);
    s_ip_cap_ok = 1;
}

static IpRec *ip_commit_common(IpFrame *f)
{
    IpRec *r;
    int i;
    if (!s_ip_cap_ok) return NULL;
    s_ip_cap_ok = 0;
    r = ip_new_rec(f);
    if (!r) return NULL;
    *r = s_ip_cap;
    r->partner = -1;
    if (r->bs) ID3D11BlendState_AddRef(r->bs);
    if (r->dss) ID3D11DepthStencilState_AddRef(r->dss);
    if (r->ps) ID3D11PixelShader_AddRef(r->ps);
    if (r->vs) ID3D11VertexShader_AddRef(r->vs);
    for (i = 0; i < 4; i++)
        if (r->srv[i]) ID3D11ShaderResourceView_AddRef(r->srv[i]);
    f->ndraw++;
    return r;
}

/* CPU-transformed draw: the HwVertex array that was drawn. */
static void ip_commit_cpu(const HwVertex *o, uint32_t count)
{
    IpFrame *f = &s_ip.f[s_ip.cur];
    uint32_t vofs;
    IpRec *r;
    if (!s_ip.enabled || !s_ip_cap_ok) return;
    vofs = ip_alloc(f, (size_t)count * sizeof(HwVertex));
    if (!f->ar) return;
    r = ip_commit_common(f);
    if (!r) return;
    memcpy(f->ar + vofs, o, (size_t)count * sizeof(HwVertex));
    r->vofs = vofs; r->nvert = count;
}

/* Program draw: distinct slot vertices (128 bytes each) and 16-bit indices. */
static void ip_commit_prog(const uint8_t *verts, uint32_t nv, const uint16_t *idx, uint32_t ni)
{
    IpFrame *f = &s_ip.f[s_ip.cur];
    uint32_t vofs, iofs, cofs = 0;
    uint32_t gen = nv2a_vsh_constant_generation();
    IpRec *r;
    if (!s_ip.enabled || !s_ip_cap_ok) return;
    vofs = ip_alloc(f, (size_t)nv * 128);
    iofs = ip_alloc(f, (size_t)ni * 2);
    if (f->last_c_valid && f->last_cgen == gen && f->last_cofs + sizeof(float) * IP_CFLOATS <= f->asz) {
        cofs = f->last_cofs;
    } else {
        cofs = ip_alloc(f, sizeof(float) * IP_CFLOATS);
        if (!f->ar) return;
        memcpy(f->ar + cofs, nv2a_vsh_constants(), sizeof(float) * IP_CFLOATS);
        f->last_cgen = gen; f->last_cofs = cofs; f->last_c_valid = 1;
    }
    if (!f->ar) return;
    r = ip_commit_common(f);
    if (!r) return;
    memcpy(f->ar + vofs, verts, (size_t)nv * 128);
    memcpy(f->ar + iofs, idx, (size_t)ni * 2);
    r->vofs = vofs; r->nvert = nv; r->iofs = iofs; r->nidx = ni; r->cofs = cofs;
}

static void ip_commit_clear(const HwSurface *s, const HwDepth *d, uint32_t param, uint32_t x0, uint32_t y0,
                            uint32_t x1, uint32_t y1, const float col[4], float z, uint8_t st)
{
    IpFrame *f = &s_ip.f[s_ip.cur];
    IpRec *r;
    if (!s_ip.enabled) return;
    r = ip_new_rec(f);
    if (!r) return;
    memset(r, 0, sizeof *r);
    r->kind = 2; r->partner = -1;
    r->surf = (int8_t)(s - s_hw.surf); r->depth = d ? (int8_t)(d - s_hw.depth) : -1;
    r->soff = s->offset; r->sw = s->w; r->sh = s->h;
    r->cparam = param; r->cx0 = x0; r->cy0 = y0; r->cx1 = x1; r->cy1 = y1;
    memcpy(r->ccol, col, 16); r->cz = z; r->cst = st;
}

/* ---- matching ---------------------------------------------------------- */

static uint32_t ip_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

/* The display buffers swap every frame; treat every surface of the display's size as one. */
static uint32_t ip_norm_off(uint32_t off, uint32_t w, uint32_t h)
{
    if (s_hw.disp_w && w == s_hw.disp_w && h >= s_hw.disp_h) return 0xD15B0000u;
    return off;
}

static void ip_finalize(IpFrame *f)
{
    int i, sz = 64;
    while (sz < f->n * 2 + 2) sz <<= 1;
    free(f->tab);
    f->tab = (int *)malloc((size_t)sz * sizeof(int));
    f->tabsz = sz;
    if (!f->tab) { f->tabsz = 0; return; }
    for (i = 0; i < sz; i++) f->tab[i] = -1;
    for (i = 0; i < f->n; i++) {
        IpRec *r = &f->r[i];
        uint32_t h = 2166136261u, mk;
        int occ = 0, k;
        h = ip_mix(h, r->kind); h = ip_mix(h, ip_norm_off(r->soff, r->sw, r->sh)); h = ip_mix(h, r->sw);
        h = ip_mix(h, ip_norm_off(r->soff, r->sw, r->sh) == r->soff ? r->sh : 0);
        if (r->kind == 2) {
            h = ip_mix(h, r->cparam);
        } else {
            h = ip_mix(h, r->vskey); h = ip_mix(h, r->vmask); h = ip_mix(h, r->nvert); h = ip_mix(h, r->nidx);
            for (k = 0; k < 4; k++) {
                uint32_t to = r->toff[k];
                if (r->srv_surf[k] >= 0) {          /* a render target: the double-buffered display surfaces alternate */
                    const HwSurface *as = &s_hw.surf[r->srv_surf[k]];
                    to = ip_norm_off(as->offset, as->w, as->h);
                } else if (to) {                    /* the same, named by address (full-screen fades) */
                    int q;
                    for (q = 0; q < HW_MAX_SURFACES; q++)
                        if (s_hw.surf[q].tex && s_hw.surf[q].offset == to) { to = ip_norm_off(to, s_hw.surf[q].w, s_hw.surf[q].h); break; }
                }
                h = ip_mix(h, to);
            }
        }
        for (;;) {
            uint32_t p;
            mk = h + (uint32_t)occ * 0x9E3779B1u;
            p = (mk * 2654435761u) >> 8;
            for (;; p++) {
                int e = f->tab[p & (uint32_t)(sz - 1)];
                if (e < 0) { f->tab[p & (uint32_t)(sz - 1)] = i; goto placed; }
                if (f->r[e].mkey == mk) break;     /* taken: next occurrence */
            }
            occ++;
        }
placed:
        r->mkey = mk;
    }
}

static int ip_find(const IpFrame *f, uint32_t mk)
{
    uint32_t p;
    if (!f->tabsz) return -1;
    p = (mk * 2654435761u) >> 8;
    for (;; p++) {
        int e = f->tab[p & (uint32_t)(f->tabsz - 1)];
        if (e < 0) return -1;
        if (f->r[e].mkey == mk) return e;
    }
}

/* ---- planning: which draws are blended -------------------------------- */

static void ip_plan_draw(const IpFrame *cur, IpRec *r, const IpFrame *prev, const IpRec *p, unsigned *prog_rej)
{
    uint32_t i;
    r->lerp = 0;
    if (r->kind == 0) {
        const HwVertex *a = (const HwVertex *)(prev->ar + p->vofs), *b = (const HwVertex *)(cur->ar + r->vofs);
        float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f, dmax = 0.0f;
        int any = 0;
        for (i = 0; i < r->nvert; i++) {
            float dx, dy;
            if (fabsf(b[i].pos[3] - 1.0f) < 1e-4f && fabsf(a[i].pos[3] - 1.0f) < 1e-4f) continue;   /* 2D / HUD */
            if (!isfinite(a[i].pos[0]) || !isfinite(a[i].pos[1]) || !isfinite(b[i].pos[0]) || !isfinite(b[i].pos[1])) return;
            any = 1;
            if (b[i].pos[0] < minx) minx = b[i].pos[0];
            if (b[i].pos[0] > maxx) maxx = b[i].pos[0];
            if (b[i].pos[1] < miny) miny = b[i].pos[1];
            if (b[i].pos[1] > maxy) maxy = b[i].pos[1];
            dx = fabsf(b[i].pos[0] - a[i].pos[0]); dy = fabsf(b[i].pos[1] - a[i].pos[1]);
            if (dx > dmax) dmax = dx;
            if (dy > dmax) dmax = dy;
        }
        if (any && dmax > 0.0f) {
            float ext = maxx - minx > maxy - miny ? maxx - minx : maxy - miny;
            float lim = ext * s_ip.cpu_frac;
            if (lim < 8.0f) lim = 8.0f;
            if (dmax <= lim && dmax <= 160.0f) r->lerp = 1;
        }
        return;
    }
    if (r->kind == 1) {
        const float *ca = (const float *)(prev->ar + p->cofs), *cb = (const float *)(cur->ar + r->cofs);
        const uint8_t *va = prev->ar + p->vofs, *vb = cur->ar + r->vofs;
        uint8_t flags = 0;
        /* constants: relative Frobenius change over the registers that moved */
        if (ca != cb && !s_ip.fixed) {
            double dsum = 0.0, nsum = 0.0;
            int reg;
            for (reg = 0; reg < NV2A_VSH_CONSTANTS; reg++) {
                const float *x = ca + reg * 4, *y = cb + reg * 4;
                double d2 = 0.0, n2 = 0.0;
                int k;
                for (k = 0; k < 4; k++) { double d = (double)y[k] - x[k]; d2 += d * d; n2 += (double)y[k] * y[k]; }
                if (d2 > 0.0) { dsum += d2; nsum += n2; }
            }
            if (dsum > 0.0) {
                double rel = sqrt(dsum / (nsum + 1e-9));
                if (s_ip.dbg_level >= 3) {
                    int bk = rel < 0.01 ? 0 : rel < 0.05 ? 1 : rel < 0.1 ? 2 : rel < 0.2 ? 3 : rel < 0.3 ? 4 : rel < 0.5 ? 5 : rel < 1.0 ? 6 : 7;
                    s_ip.hist[bk]++;
                }
                if (rel <= s_ip.prog_max) flags |= 1;
                if (rel > s_ip.hi_rel) (*prog_rej)++;
            }
        }
        /* position attribute (slot 0 when attribute 0 is enabled) */
        if ((r->vmask & 1) && memcmp(va, vb, (size_t)r->nvert * 128) != 0) {
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f}, dmax = 0.0f, ext = 0.0f;
            int k;
            for (i = 0; i < r->nvert; i++) {
                const float *pa = (const float *)(va + i * 128), *pb = (const float *)(vb + i * 128);
                for (k = 0; k < 3; k++) {
                    float d = fabsf(pb[k] - pa[k]);
                    if (!isfinite(d)) goto noattr;
                    if (d > dmax) dmax = d;
                    if (pb[k] < mn[k]) mn[k] = pb[k];
                    if (pb[k] > mx[k]) mx[k] = pb[k];
                }
            }
            for (k = 0; k < 3; k++) if (mx[k] - mn[k] > ext) ext = mx[k] - mn[k];
            if (dmax <= ext * s_ip.attr_frac) flags |= 2;
        }
noattr:
        r->lerp = flags;
    }
}

/* ---- twins ------------------------------------------------------------- */

static void ip_release_twins(void)
{
    int i;
    for (i = 0; i < HW_MAX_SURFACES; i++) {
        IpTwin *t = &s_ip.tw[i];
        if (t->tex) {
            ID3D11ShaderResourceView_Release(t->srv);
            ID3D11RenderTargetView_Release(t->rtv);
            ID3D11Texture2D_Release(t->tex);
        }
        memset(t, 0, sizeof *t);
        if (s_ip.shadow[i].tex) {
            ID3D11ShaderResourceView_Release(s_ip.shadow[i].srv);
            ID3D11Texture2D_Release(s_ip.shadow[i].tex);
        }
        memset(&s_ip.shadow[i], 0, sizeof s_ip.shadow[i]);
    }
    for (i = 0; i < HW_MAX_DEPTHS; i++) {
        IpTwinD *t = &s_ip.twd[i];
        if (t->tex) {
            ID3D11DepthStencilView_Release(t->dsv);
            ID3D11Texture2D_Release(t->tex);
        }
        memset(t, 0, sizeof *t);
    }
    s_ip.prev_valid = 0;
}

/* The twin of real surface `idx`; `copy_real` fills it from the real surface the first time
 * a replay touches it. NULL when it cannot be made. */
static IpTwin *ip_twin(int idx, int copy_real)
{
    HwSurface *rs = &s_hw.surf[idx];
    IpTwin *t = &s_ip.tw[idx];
    D3D11_TEXTURE2D_DESC td;
    if (!rs->tex) return NULL;
    ID3D11Texture2D_GetDesc(rs->tex, &td);
    if (t->tex && (t->w != td.Width || t->h != td.Height)) {
        ID3D11ShaderResourceView_Release(t->srv);
        ID3D11RenderTargetView_Release(t->rtv);
        ID3D11Texture2D_Release(t->tex);
        memset(t, 0, sizeof *t);
    }
    if (!t->tex) {
        if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &t->tex))) return NULL;
        ID3D11Device_CreateRenderTargetView(s_hw.dev, (ID3D11Resource *)t->tex, NULL, &t->rtv);
        ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)t->tex, NULL, &t->srv);
        t->w = td.Width; t->h = td.Height; t->epoch = 0;
    }
    if (t->epoch != s_ip.epoch) {
        t->epoch = s_ip.epoch;
        if (copy_real)
            ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)t->tex, (ID3D11Resource *)rs->tex);
    }
    return t;
}

static IpTwinD *ip_twin_depth(int idx)
{
    HwDepth *rd = &s_hw.depth[idx];
    IpTwinD *t = &s_ip.twd[idx];
    D3D11_TEXTURE2D_DESC td;
    if (!rd->tex) return NULL;
    ID3D11Texture2D_GetDesc(rd->tex, &td);
    if (t->tex && (t->w != td.Width || t->h != td.Height)) {
        ID3D11DepthStencilView_Release(t->dsv);
        ID3D11Texture2D_Release(t->tex);
        memset(t, 0, sizeof *t);
    }
    if (!t->tex) {
        if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &t->tex))) return NULL;
        ID3D11Device_CreateDepthStencilView(s_hw.dev, (ID3D11Resource *)t->tex, NULL, &t->dsv);
        t->w = td.Width; t->h = td.Height; t->epoch = 0;
    }
    if (t->epoch != s_ip.epoch) {
        t->epoch = s_ip.epoch;
        ID3D11DeviceContext_ClearDepthStencilView(s_hw.ctx, t->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    return t;
}

static ID3D11ShaderResourceView *ip_shadow(int idx, const IpTwin *t)
{
    D3D11_TEXTURE2D_DESC td;
    if (s_ip.shadow[idx].tex && (s_ip.shadow[idx].w != t->w || s_ip.shadow[idx].h != t->h)) {
        ID3D11ShaderResourceView_Release(s_ip.shadow[idx].srv);
        ID3D11Texture2D_Release(s_ip.shadow[idx].tex);
        memset(&s_ip.shadow[idx], 0, sizeof s_ip.shadow[idx]);
    }
    if (!s_ip.shadow[idx].tex) {
        ID3D11Texture2D_GetDesc(t->tex, &td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_hw.dev, &td, NULL, &s_ip.shadow[idx].tex))) return NULL;
        ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)s_ip.shadow[idx].tex, NULL, &s_ip.shadow[idx].srv);
        s_ip.shadow[idx].w = t->w; s_ip.shadow[idx].h = t->h;
    }
    ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)s_ip.shadow[idx].tex, (ID3D11Resource *)t->tex);
    return s_ip.shadow[idx].srv;
}

/* ---- replay ------------------------------------------------------------ */

static int ip_check_target(const IpRec *r)
{
    const HwSurface *s;
    if (r->surf < 0 || r->surf >= HW_MAX_SURFACES) return 0;
    s = &s_hw.surf[r->surf];
    if (!s->tex || s->offset != r->soff || s->w != r->sw || s->h != r->sh) return 0;
    if (r->kind != 2 && r->depth >= 0) {
        const HwDepth *d = &s_hw.depth[r->depth];
        if (!d->tex) return 0;
    }
    return 1;
}

static int ip_replay_clear(const IpRec *r)
{
    IpTwin *t;
    IpTwinD *td = NULL;
    uint32_t param = r->cparam;
    int full = (param & NV097_CLEAR_COLOR_MASK) == NV097_CLEAR_COLOR_MASK && r->cx0 == 0 && r->cy0 == 0
               && r->cx1 >= r->sw && r->cy1 >= r->sh;
    t = ip_twin(r->surf, !((param & NV097_CLEAR_COLOR_MASK) && full));
    if (!t) return 0;
    if (r->depth >= 0 && (param & 3)) {
        td = ip_twin_depth(r->depth);
        if (td)
            ID3D11DeviceContext_ClearDepthStencilView(s_hw.ctx, td->dsv,
                ((param & 1) ? D3D11_CLEAR_DEPTH : 0) | ((param & 2) ? D3D11_CLEAR_STENCIL : 0), r->cz, r->cst);
    }
    if (param & NV097_CLEAR_COLOR_MASK) {
        if (full) {
            ID3D11DeviceContext_ClearRenderTargetView(s_hw.ctx, t->rtv, r->ccol);
        } else if (r->cx1 > r->cx0 && r->cy1 > r->cy0) {
            HwPsConst pc;
            D3D11_MAPPED_SUBRESOURCE map;
            D3D11_VIEWPORT vp;
            D3D11_RECT sc;
            D3D11_BLEND_DESC bd;
            ID3D11BlendState *bs = NULL;
            static const float bf[4] = {0, 0, 0, 0};
            const HwSurface *s = &s_hw.surf[r->surf];
            memset(&pc, 0, sizeof pc);
            memcpy(pc.finc0, r->ccol, 16);
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, &pc, sizeof pc);
                ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0);
            }
            memset(&bd, 0, sizeof bd);
            bd.RenderTarget[0].RenderTargetWriteMask =
                (UINT8)(((param & 0x10) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) | ((param & 0x20) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                      | ((param & 0x40) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | ((param & 0x80) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
            ID3D11Device_CreateBlendState(s_hw.dev, &bd, &bs);
            ID3D11DeviceContext_OMSetRenderTargets(s_hw.ctx, 1, &t->rtv, NULL);
            vp.TopLeftX = 0; vp.TopLeftY = 0;
            vp.Width = (float)hw_px(s->w, s->sc); vp.Height = (float)(s->h * s->sc);
            vp.MinDepth = 0; vp.MaxDepth = 1;
            ID3D11DeviceContext_RSSetViewports(s_hw.ctx, 1, &vp);
            sc.left = (LONG)hw_px(r->cx0, s->sc); sc.top = (LONG)(r->cy0 * s->sc);
            sc.right = (LONG)hw_px(r->cx1, s->sc); sc.bottom = (LONG)(r->cy1 * s->sc);
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
    }
    return 1;
}

static int ip_replay_draw(const IpFrame *cur, const IpRec *r, const IpFrame *prev, float T)
{
    const IpRec *p = r->partner >= 0 ? &prev->r[r->partner] : NULL;
    IpTwin *t;
    IpTwinD *td = NULL;
    D3D11_MAPPED_SUBRESOURCE map, vmap, imap;
    ID3D11ShaderResourceView *srv[4];
    UINT stride = sizeof(HwVertex), zero = 0;
    float cl[IP_CFLOATS];
    const float *cuse = NULL;
    uint32_t i;
    int st;

    t = ip_twin(r->surf, 1);
    if (!t) return 0;
    if (r->depth >= 0) td = ip_twin_depth(r->depth);
    ID3D11DeviceContext_OMSetRenderTargets(s_hw.ctx, 1, &t->rtv, td ? td->dsv : NULL);
    ID3D11DeviceContext_RSSetViewports(s_hw.ctx, 1, &r->vp);
    ID3D11DeviceContext_RSSetScissorRects(s_hw.ctx, 1, &r->sc);
    ID3D11DeviceContext_RSSetState(s_hw.ctx, r->rs);
    ID3D11DeviceContext_OMSetBlendState(s_hw.ctx, r->bs, r->bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_hw.ctx, r->dss, r->sref);

    for (st = 0; st < 4; st++) {
        srv[st] = r->srv[st];
        if (r->srv_surf[st] >= 0) {
            int si = r->srv_surf[st];
            const IpTwin *tw = &s_ip.tw[si];
            if (r->srv_self[st]) {
                ID3D11ShaderResourceView *sh = ip_shadow(si, tw->tex ? tw : t);
                if (sh) srv[st] = sh;
            } else if (tw->tex && tw->epoch == s_ip.epoch) {
                srv[st] = tw->srv;
            }
        }
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &r->pc, sizeof r->pc);
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.ps_cb, 0);
    }
    if (r->kind == 1) {
        const float *cb = (const float *)(cur->ar + r->cofs);
        cuse = cb;
        if ((r->lerp & 1) && p) {
            const float *ca = (const float *)(prev->ar + p->cofs);
            for (i = 0; i < IP_CFLOATS; i++) cl[i] = ca[i] + (cb[i] - ca[i]) * T;
            cuse = cl;
        }
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_hw.ctx, (ID3D11Resource *)s_hw.vs_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, r->hdr, sizeof r->hdr);
        if (cuse)
            memcpy((uint8_t *)map.pData + offsetof(HwVsConst, c), cuse, sizeof(float) * IP_CFLOATS);
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vs_cb, 0);
    }
    ID3D11DeviceContext_IASetInputLayout(s_hw.ctx, r->kind == 1 ? s_hw.layout_prog : s_hw.layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_hw.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_hw.ctx, 0, 1, &s_hw.vb, &stride, &zero);
    ID3D11DeviceContext_VSSetShader(s_hw.ctx, r->kind == 1 ? r->vs : s_hw.vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_hw.ctx, 0, 1, &s_hw.vs_cb);
    ID3D11DeviceContext_PSSetShader(s_hw.ctx, r->ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_hw.ctx, 0, 1, &s_hw.ps_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_hw.ctx, 0, 4, srv);
    ID3D11DeviceContext_PSSetSamplers(s_hw.ctx, 0, 4, (ID3D11SamplerState *const *)r->smp);

    if (r->kind == 0) {
        const HwVertex *b = (const HwVertex *)(cur->ar + r->vofs);
        HwVertex *o;
        if (!hw_ring_map(s_hw.vb, &s_hw.vb_off, HW_VB_BYTES, r->nvert * (uint32_t)sizeof(HwVertex), sizeof(HwVertex), &vmap))
            return 0;
        o = (HwVertex *)((uint8_t *)vmap.pData + s_hw.vb_off);
        if ((r->lerp & 1) && p) {
            const HwVertex *a = (const HwVertex *)(prev->ar + p->vofs);
            for (i = 0; i < r->nvert; i++) {
                HwVertex v = b[i];
                if (!(fabsf(v.pos[3] - 1.0f) < 1e-4f && fabsf(a[i].pos[3] - 1.0f) < 1e-4f)) {
                    int k;
                    for (k = 0; k < 4; k++) v.pos[k] = a[i].pos[k] + (b[i].pos[k] - a[i].pos[k]) * T;
                }
                o[i] = v;
            }
        } else {
            memcpy(o, b, (size_t)r->nvert * sizeof(HwVertex));
        }
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vb, 0);
        ID3D11DeviceContext_Draw(s_hw.ctx, r->nvert, s_hw.vb_off / (uint32_t)sizeof(HwVertex));
        s_hw.vb_off += r->nvert * (uint32_t)sizeof(HwVertex);
    } else {
        const uint8_t *vb = cur->ar + r->vofs;
        uint8_t *o;
        if (!hw_ring_map(s_hw.vb, &s_hw.vb_off, HW_VB_BYTES, r->nvert * (uint32_t)sizeof(HwVertex), sizeof(HwVertex), &vmap))
            return 0;
        if (!hw_ring_map(s_hw.ib, &s_hw.ib_off, HW_IB_BYTES, r->nidx * 2, 2, &imap)) {
            ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vb, 0);
            return 0;
        }
        o = (uint8_t *)vmap.pData + s_hw.vb_off;
        if ((r->lerp & 2) && p) {
            const uint8_t *va = prev->ar + p->vofs;
            for (i = 0; i < r->nvert; i++) {
                float v[32];
                const float *pa = (const float *)(va + i * 128), *pb = (const float *)(vb + i * 128);
                int k;
                memcpy(v, vb + i * 128, 128);
                for (k = 0; k < 3; k++) v[k] = pa[k] + (pb[k] - pa[k]) * T;
                memcpy(o + i * 128, v, 128);
            }
        } else {
            memcpy(o, vb, (size_t)r->nvert * 128);
        }
        memcpy((uint8_t *)imap.pData + s_hw.ib_off, cur->ar + r->iofs, (size_t)r->nidx * 2);
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.ib, 0);
        ID3D11DeviceContext_Unmap(s_hw.ctx, (ID3D11Resource *)s_hw.vb, 0);
        ID3D11DeviceContext_IASetIndexBuffer(s_hw.ctx, s_hw.ib, DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_DrawIndexed(s_hw.ctx, r->nidx, s_hw.ib_off / 2, (INT)(s_hw.vb_off / (uint32_t)sizeof(HwVertex)));
        s_hw.vb_off += r->nvert * (uint32_t)sizeof(HwVertex);
        s_hw.ib_off += r->nidx * 2;
    }
    return 1;
}

/* Build the interpolated picture for frame `cur` (against `prev`) into s_ip.out_tex.
 * `disp` is the surface that is presented, (pw, ph) the size of the presented copy. Returns 1 when
 * s_ip.out_srv holds it, 0 when this frame should be shown once only (cut, mismatch, error). */
static int ip_replay(IpFrame *cur, IpFrame *prev, const HwSurface *disp, UINT pw, UINT ph)
{
    int i, ndraw = 0, nmatch = 0, nlerp = 0, nprog = 0;
    unsigned prog_rej = 0;
    int ok = 1;
    IpTwin *dt;
    D3D11_BOX box;
    D3D11_TEXTURE2D_DESC td;
    float T = s_ip.t;
    LARGE_INTEGER q0, q1;

    QueryPerformanceCounter(&q0);
    s_ip.st_frames++;
    for (i = 0; i < cur->n; i++) {
        IpRec *r = &cur->r[i];
        if (r->kind == 2) continue;
        ndraw++;
        if (r->kind == 1) nprog++;
        r->partner = ip_find(prev, r->mkey);
        r->lerp = 0;
        if (r->partner >= 0) {
            const IpRec *p = &prev->r[r->partner];
            if (p->kind == r->kind && p->nvert == r->nvert && p->nidx == r->nidx) {
                nmatch++;
                ip_plan_draw(cur, r, prev, p, &prog_rej);
                if (r->lerp) nlerp++;
            } else r->partner = -1;
        }
    }
    s_ip.st_draws += ndraw; s_ip.st_matched += nmatch; s_ip.st_lerped += nlerp;
    if (ndraw < 4 || (float)nmatch < (1.0f - s_ip.cut_frac) * (float)ndraw) {
        s_ip.st_cut_match++;
        if (s_ip.dbg_level >= 2) {
            static DWORD last; DWORD now = GetTickCount();
            if (now - last > 1500) {
                int k, shown = 0;
                last = now;
                fprintf(stderr, "  [IPD] cut: %d draws, %d matched; cur(n=%d) vs prev(n=%d)\n", ndraw, nmatch, cur->n, prev->n);
                for (k = 0; k < cur->n && shown < 40; k++) {
                    const IpRec *r = &cur->r[k];
                    if (r->kind != 2 && r->partner >= 0) continue;
                    fprintf(stderr, "  [IPD] cur#%d kind %d s %08X %ux%u vs %08X m %X nv %u ni %u tex %08X %08X %08X %08X as %d %d %d %d\n", k, r->kind,
                            (unsigned)r->soff, (unsigned)r->sw, (unsigned)r->sh, (unsigned)r->vskey, (unsigned)r->vmask, (unsigned)r->nvert, (unsigned)r->nidx,
                            (unsigned)r->toff[0], (unsigned)r->toff[1], (unsigned)r->toff[2], (unsigned)r->toff[3],
                            r->srv_surf[0], r->srv_surf[1], r->srv_surf[2], r->srv_surf[3]);
                    if (k < prev->n) {
                        const IpRec *q = &prev->r[k];
                        fprintf(stderr, "  [IPD] prv#%d kind %d s %08X %ux%u vs %08X m %X nv %u ni %u tex %08X %08X %08X %08X as %d %d %d %d\n", k, q->kind,
                            (unsigned)q->soff, (unsigned)q->sw, (unsigned)q->sh, (unsigned)q->vskey, (unsigned)q->vmask, (unsigned)q->nvert, (unsigned)q->nidx,
                            (unsigned)q->toff[0], (unsigned)q->toff[1], (unsigned)q->toff[2], (unsigned)q->toff[3],
                            q->srv_surf[0], q->srv_surf[1], q->srv_surf[2], q->srv_surf[3]);
                    }
                    shown++;
                }
            }
        }
        return 0;
    }
    if (nprog > 8 && (float)prog_rej > s_ip.hi_frac * (float)nprog) { s_ip.st_cut_prog++; return 0; }
    if (disp->tex == NULL) return 0;

    s_ip.epoch++;
    for (i = 0; i < cur->n && ok; i++) {
        const IpRec *r = &cur->r[i];
        if (!ip_check_target(r)) { ok = 0; break; }
        ok = r->kind == 2 ? ip_replay_clear(r) : ip_replay_draw(cur, r, prev, T);
    }
    if (!ok) { s_ip.st_fail++; return 0; }
    {
        int di = (int)(disp - s_hw.surf);
        dt = &s_ip.tw[di];
        if (!dt->tex || dt->epoch != s_ip.epoch) { s_ip.st_cut_other++; return 0; }
    }
    ID3D11Texture2D_GetDesc(dt->tex, &td);
    if (!s_ip.out_tex || s_ip.out_w != pw || s_ip.out_h != ph) {
        D3D11_TEXTURE2D_DESC od = td;
        if (s_ip.out_srv) ID3D11ShaderResourceView_Release(s_ip.out_srv);
        if (s_ip.out_tex) ID3D11Texture2D_Release(s_ip.out_tex);
        s_ip.out_srv = NULL; s_ip.out_tex = NULL;
        od.Width = pw; od.Height = ph; od.BindFlags = D3D11_BIND_SHADER_RESOURCE; od.MiscFlags = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_hw.dev, &od, NULL, &s_ip.out_tex)))
            ID3D11Device_CreateShaderResourceView(s_hw.dev, (ID3D11Resource *)s_ip.out_tex, NULL, &s_ip.out_srv);
        s_ip.out_w = pw; s_ip.out_h = ph;
    }
    if (!s_ip.out_srv || pw > td.Width || ph > td.Height) return 0;
    box.left = 0; box.top = 0; box.front = 0; box.right = pw; box.bottom = ph; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_hw.ctx, (ID3D11Resource *)s_ip.out_tex, 0, 0, 0, 0,
                                              (ID3D11Resource *)dt->tex, 0, &box);
    s_hw.vs_last_valid = 0;
    s_hw.ps_last_valid = 0;
    QueryPerformanceCounter(&q1);
    s_ip.st_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)s_ip.qf.QuadPart;
    s_ip.st_shown++;
    return 1;
}

/* ---- flip hooks -------------------------------------------------------- */

/* At hw_flip, after the real picture was copied (disp != NULL: a fresh picture of size pw x ph).
 * Sets s_ip.show when an interpolated picture is to be presented first. */
static void ip_flip(const HwSurface *disp, UINT pw, UINT ph)
{
    IpFrame *f = &s_ip.f[s_ip.cur], *pv = &s_ip.f[s_ip.cur ^ 1];
    int shown = 0;
    s_ip.show = NULL;
    if (s_ip.env_on) {                         /* interp_off.flag in the working directory: run-time A/B switch */
        static unsigned n;
        if ((n++ & 15u) == 0) {
            int want = GetFileAttributesA("interp_off.flag") == INVALID_FILE_ATTRIBUTES && (!s_ip.gov || s_ip.gov_on);
            FILE *tf = fopen("interp_tune.txt", "r");
            if (tf) {
                float a, b, c, d, e2;
                if (fscanf(tf, "%f %f %f %f %f", &a, &b, &c, &d, &e2) == 5) {
                    s_ip.prog_max = a; s_ip.hi_rel = b; s_ip.hi_frac = c; s_ip.attr_frac = d; s_ip.cpu_frac = e2;
                }
                fclose(tf);
            }
            if (want != s_ip.enabled) {
                ip_frame_reset(&s_ip.f[0]); ip_frame_reset(&s_ip.f[1]);
                s_ip.prev_valid = 0; s_ip_cap_ok = 0;
                s_ip.enabled = want;
                fprintf(stderr, "  [IP] %s\n", want ? "enabled" : "disabled");
                return;
            }
        }
    }
    if (s_ip.env_on && s_ip.gov) {
        LARGE_INTEGER q;
        double now_s, dt;
        QueryPerformanceCounter(&q);
        now_s = (double)q.QuadPart / (double)s_ip.qf.QuadPart;
        if (s_ip.gov_last) {
            dt = (double)(q.QuadPart - s_ip.gov_last) * 1000.0 / (double)s_ip.qf.QuadPart;
            if (dt > 80.0) dt = 80.0;
            s_ip.gov_ema = s_ip.gov_ema * 0.9 + dt * 0.1;
        }
        s_ip.gov_last = q.QuadPart;
        if (s_ip.gov_on && s_ip.gov_ema > s_ip.gov_hi) {
            s_ip.gov_on = 0;
            s_ip.gov_off_until = now_s + s_ip.gov_backoff;
            if (now_s - s_ip.gov_on_since > 20.0) s_ip.gov_backoff = 4.0;
            else if (s_ip.gov_backoff < 30.0) s_ip.gov_backoff *= 2.0;
            if (s_ip.dbg) fprintf(stderr, "  [IP] governor: off (flip interval %.1f ms), retry in %.0f s\n", s_ip.gov_ema, s_ip.gov_off_until - now_s);
            ip_frame_reset(&s_ip.f[0]); ip_frame_reset(&s_ip.f[1]);
            s_ip.prev_valid = 0; s_ip_cap_ok = 0; s_ip.enabled = 0;
        } else if (!s_ip.gov_on && now_s >= s_ip.gov_off_until && GetFileAttributesA("interp_off.flag") == INVALID_FILE_ATTRIBUTES) {
            s_ip.gov_on = 1; s_ip.gov_ema = 33.3; s_ip.gov_on_since = now_s;
            s_ip.enabled = 1;
            if (s_ip.dbg) fprintf(stderr, "  [IP] governor: probing\n");
        }
    }
    if (!s_ip.enabled) return;
    if (!disp || f->ndraw == 0) {              /* held / clear-only flip: not a frame */
        ip_frame_reset(f);
        return;
    }
    ip_finalize(f);
    if (s_ip.prev_valid && s_ip.t > 0.0f)
        shown = ip_replay(f, pv, disp, pw, ph);
    if (shown) {
        s_ip.show = s_ip.out_srv;
        if (s_ip.dump && s_ip.dump_prev && GetFileAttributesA("interp_dump.flag") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA("interp_dump.flag");
            char fn[256];
            s_ip.dump_n++;
            _snprintf(fn, sizeof fn, "%s_%u_a.bmp", s_ip.dump, s_ip.dump_n); hw_dump_surface(s_ip.dump_prev, fn);
            _snprintf(fn, sizeof fn, "%s_%u_i.bmp", s_ip.dump, s_ip.dump_n); hw_dump_surface(s_ip.out_tex, fn);
            _snprintf(fn, sizeof fn, "%s_%u_b.bmp", s_ip.dump, s_ip.dump_n); hw_dump_surface(s_hw.pres_tex, fn);
        }
    }
    if (s_ip.dump && s_hw.pres_tex) {            /* keep the real picture for the next flip's "_a" dump */
        D3D11_TEXTURE2D_DESC pd;
        ID3D11Texture2D_GetDesc(s_hw.pres_tex, &pd);
        if (s_ip.dump_prev) { D3D11_TEXTURE2D_DESC qd; ID3D11Texture2D_GetDesc(s_ip.dump_prev, &qd);
            if (qd.Width != pd.Width || qd.Height != pd.Height) { ID3D11Texture2D_Release(s_ip.dump_prev); s_ip.dump_prev = NULL; } }
        if (!s_ip.dump_prev) { pd.BindFlags = D3D11_BIND_SHADER_RESOURCE; pd.MiscFlags = 0;
            ID3D11Device_CreateTexture2D(s_hw.dev, &pd, NULL, &s_ip.dump_prev); }
        if (s_ip.dump_prev)
            ID3D11DeviceContext_CopyResource(s_hw.ctx, (ID3D11Resource *)s_ip.dump_prev, (ID3D11Resource *)s_hw.pres_tex);
    }
    s_ip.cur ^= 1;                              /* this frame becomes the previous one */
    ip_frame_reset(&s_ip.f[s_ip.cur]);
    s_ip.prev_valid = 1;
    if (s_ip.dbg) {
        DWORD now = GetTickCount();
        if (!s_ip.st_t0) s_ip.st_t0 = now;
        if (now - s_ip.st_t0 >= 1000) {
            fprintf(stderr, "  [IP] frames %u shown %u cut(match %u prog %u other %u) fail %u; draws/frame %u matched %u lerped %u; replay %.2f ms/frame\n",
                    s_ip.st_frames, s_ip.st_shown, s_ip.st_cut_match, s_ip.st_cut_prog, s_ip.st_cut_other, s_ip.st_fail,
                    s_ip.st_frames ? s_ip.st_draws / s_ip.st_frames : 0, s_ip.st_frames ? s_ip.st_matched / s_ip.st_frames : 0,
                    s_ip.st_frames ? s_ip.st_lerped / s_ip.st_frames : 0, s_ip.st_shown ? s_ip.st_ms / s_ip.st_shown : 0.0);
            if (s_ip.dbg_level >= 3) {
                fprintf(stderr, "  [IPH] rel-change histogram of program draws (<.01 <.05 <.1 <.2 <.3 <.5 <1 >=1): %u %u %u %u %u %u %u %u\n",
                        s_ip.hist[0], s_ip.hist[1], s_ip.hist[2], s_ip.hist[3], s_ip.hist[4], s_ip.hist[5], s_ip.hist[6], s_ip.hist[7]);
                memset(s_ip.hist, 0, sizeof s_ip.hist);
            }
            s_ip.st_frames = s_ip.st_shown = s_ip.st_cut_match = s_ip.st_cut_prog = s_ip.st_cut_other = s_ip.st_fail = 0;
            s_ip.st_draws = s_ip.st_matched = s_ip.st_lerped = 0; s_ip.st_ms = 0; s_ip.st_t0 = now;
        }
    }
}

static DWORD WINAPI ip_presenter(LPVOID unused)
{
    (void)unused;
    for (;;) {
        LONG serial;
        LONGLONG dl;
        WaitForSingleObject(s_ip.ev, INFINITE);
        for (;;) {
            LARGE_INTEGER q;
            double rem;
            serial = s_ip.pend_serial;
            dl = s_ip.deadline;
            QueryPerformanceCounter(&q);
            rem = (double)(dl - q.QuadPart) * 1000.0 / (double)s_ip.qf.QuadPart;
            if (rem <= 0.0) break;
            if (rem > 2.5) Sleep((DWORD)(rem - 2.0)); else YieldProcessor();
        }
        EnterCriticalSection(&s_hw.lock);
        if (s_ip.pend && serial == s_ip.pend_serial) {
            s_ip.pend = 0;
            s_ip.show = NULL;
            hw_present_locked(1);
        }
        LeaveCriticalSection(&s_hw.lock);
    }
    return 0;
}

/* Called right after the interpolated picture was presented: the real frame follows in half a frame. */
static void ip_schedule_real(void)
{
    LARGE_INTEGER q;
    if (!s_ip.ev) {
        s_ip.ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_ip.th = CreateThread(NULL, 0, ip_presenter, NULL, 0, NULL);
        if (s_ip.th) SetThreadPriority(s_ip.th, THREAD_PRIORITY_ABOVE_NORMAL);
    }
    QueryPerformanceCounter(&q);
    s_ip.deadline = q.QuadPart + (LONGLONG)((double)s_ip.qf.QuadPart / (s_hw.pace_hz > 0.0 ? s_hw.pace_hz : 60.0)) /* half a game frame = one 60 Hz period */;
    s_ip.pend = 1;
    InterlockedIncrement(&s_ip.pend_serial);
    SetEvent(s_ip.ev);
}

/* hw_flip start: a real frame still waiting from the previous flip is shown now (the game flipped early). */
static void ip_flush_pending(void)
{
    if (s_ip.pend) {
        s_ip.pend = 0;
        s_ip.show = NULL;
        hw_present_locked(1);
    }
}
