# Renderer / pushbuffer executor comparison (Phantom Dust vs upstream, takedawn, mcdub, defjam)

Scope: NV2A pushbuffer walker + executor + D3D11 renderer. Read-only study; nothing built or run.
Paths: OURS = C:\recomp\phantom-dust-re\xboxrecomp\src\kernel (pb_exec.c, pb_scan.c, nv2a_hw_d3d11.inc, nv2a_hw_interp.inc).
TD = takedawn-xboxrecomp (src/kernel, src/d3d/nv2a_pb_d3d11.c). MC = mcdub/xboxrecomp (src/nv2a_gl/nv2a_gl.c, gl_psh.c).
Licences: all four forks are MIT (sp00nz toolkit; takedawn adds xenophobentx, mcdub Laure Ladastra, defjam Vlad Yanhursky). Porting code needs only the MIT notice kept. Everything below is reading evidence, not tested in PD.

Big picture
- Ours is a hybrid: CPU raster executor + a hand-grown D3D11 "hw" path (nv2a_hw_d3d11.inc) with HLSL vertex programs, specialised async pixel-shader variants, an on-disk DXBC cache, 16-bit index batches, render-target aliasing heuristics. It is ahead of the forks on: disk shader cache (defjam v0.10.2 notes say Windows D3D11 has none; TD d3d11 has none either, its pcache is Metal/Vulkan only), PS specialisation, z-clip, swizzled-RT sizing, movie-alias fixes.
- TD is ahead on: faithful sampler state, guest mip upload, full hashing, native-format vertex copy, 32-bit indices, lines/points, real occlusion queries, partial command carry-over across PUT, render-target ownership by memory hash, state-cache dedupe.
- MC (GL) adds: adaptive "rewritten texture" hashing, native DXT upload, executor/GPU queue thread, dependent-AR/GB texture modes.
- Neither TD nor MC nor upstream implements polygon offset, point sprites, bump-env/dot-product modes in D3D11, multisample, or lost-device recovery. Nothing to copy there; those stay "ours to invent".

---------------------------------------------------------------------------------------------------
## Ranked findings

### 1. [P1] D3D11 blend op SUBTRACT / REV_SUBTRACT are swapped in the hw path
- Ours: nv2a_hw_d3d11.inc:1453-1454 maps 0x800A -> REV_SUBTRACT, 0x800B -> SUBTRACT. Ours' own CPU path says the opposite: nv2a_pb_exec.c:1265-1266 (0x800A = a-b, 0x800B = b-a). The same swap is in the .bak from 2026-10-18, so it is original, not a regression.
- Theirs: TD nv2a_pb_d3d11.c:1383-1392 (0x800A SUBTRACT; 0x800B and the signed 0xF005 REV_SUBTRACT); MC nv2a_gl.c:3014 passes the GL enum straight through (GL_FUNC_SUBTRACT = 0x800A).
- Fix: swap the two cases; map 0xF005 -> REV_SUBTRACT, 0xF006 -> ADD (TD does the same). Two lines.
- PD symptom: any subtractive blend (darkening passes, some glow/fade/shadow-darken effects) draws inverted (clamps to black instead of darkening, or vice versa). Only visible if PD uses equation != ADD; check with a log of s_gpu.blend_equation values (not currently counted).
- Effort XS. Licence n/a (own bug). Confidence in the swap: high (GL/xemu semantics, and ours' CPU path disagrees with ours' GPU path).

### 2. [P1] Constant-alpha blend factors use RGB of the blend colour; SET_BLEND_COLOR alpha-only cases wrong
- Ours: nv2a_hw_d3d11.inc:1877 `hw_color4(blend_color, bf)` then maps 0x8003/0x8004 (CONSTANT_ALPHA / INV) to BLEND_FACTOR (hw_blend_factor 1416-1417), which reads RGB of bf, not alpha. D3D11 has no CONSTANT_ALPHA.
- Theirs: TD blend_color() 1471-1490 + nv2a_blend_constant (nv2a_backend_common.h:841): when only the _ALPHA factors are in use the alpha is replicated into all four channels; mixed use logged.
- PD symptom: a fade/dissolve using D3DBLEND_CONSTANTALPHA would blend with the colour channels' constant (often 0 or wrong grey) -> wrong fade level, popping, or fully transparent/opaque overlays. Unverified whether PD uses 0x8003/4; cheap to cover.
- Effort S (about 20 lines, port nv2a_blend_constant). Licence MIT (TD). 

### 3. [P1] Texture change detection can miss small CPU rewrites (hash samples at most 2048 dwords, once per frame)
- Ours: nv2a_hw_d3d11.inc:1074-1086 hw_tex_hash (<=2048 sampled dwords, step n/2048; 512x512 ARGB samples 1 dword in 128), checked once per frame (checked_frame, 1316-1321). The text itself calls it a "cheap fingerprint".
- Theirs: (a) TD tex_hash_pal -> nv2a_tex_hash (nv2a_backend_common.c:107): 4-lane 64-bit FNV over the whole extent incl. all mip levels, plus palette hash; once per flip (pd 1689-1760). (b) MC nv2a_gl.c:525-570 + 785-840: sampled hash first, but a texture that has ever changed (t->rewrites) is re-checked at every use with a full-bytes hash ("MC3 writes text into a swizzled texture between draws... scrambled"), linear images always hashed in full (bytes_hash_full, ~0.2 ms per 1.2 MB).
- PD symptom: text/HUD/dynamic textures (name plates, damage numbers, text rendered into a texture, movie letterbox bars, CPU-composited icons) that change in a small region show stale content or update late. Also palette: ours hashes only every 8th palette entry (1087-1090 loop i += 8): palette swaps touching other entries are missed.
- Recommendation: adopt MC's adaptive scheme (cheap for static textures, exact for ones seen changing) and hash all 256 palette entries. TD's every-texture full hash is simpler but costs several ms/frame on a texture-heavy scene.
- Effort S-M. Licence MIT. Priority P1 because it is a silent-correctness gap you already flagged.

### 4. [P1/P2] Pushbuffer: a method that spans a PUT boundary is truncated and the leftover words are decoded as headers
- Ours: nv2a_pb_scan.c:326 `for (i = 0; i < count && va != put_va; i++)` (also the pb_walk loop, `va < end_va`). If PUT lands inside a method's data, the rest of that method is lost; the next walk (nv2a_pb_scan_to(last_put, put)) starts at the leftover parameter words and reads them as headers (desync -> garbage methods, `unknown++` counts them but the walk keeps going).
- Theirs: TD nv2a_pb_scan.c:291 keeps `s_left, s_subch, s_method, s_noninc` across walks ("A method header's parameters can go past PUT: the title may submit the header and some of its data now and the rest with the next PUT"); a desync stops the walk, drops the partial method, runs `stop_salvage` (finds the last plausible BACK_END_WRITE_SEMAPHORE_RELEASE 0x1D70 in the unexecuted span and runs it so a fence wait cannot hang), and requests a fence catch-up; also a single CALL/RETURN state (`in_call`, return == exactly 0x00020000) instead of recursion.
- Ours's comment "A PUT is always on a command boundary" (pb_scan.c ~250) is the assumption TD says is false for XDK-style partial submits.
- PD symptom: rare one-frame glitches (flat polygons, missing/stretched meshes) like the ring-wrap bug you just fixed; and, after a desync, a title hanging on a fence wait because the release word was in the skipped span.
- First step is instrumentation, not a rewrite: count walks where the for-loop exits with i < count. If zero over a long soak, drop to P3.
- Effort M (port s_left carry-over + stop_salvage, ~120 lines). Licence MIT.

### 5. [P2] Texture sampler state: min/mag/mip filters are not independent; no LOD bias or LOD clamp; guest mip chain ignored
- Ours: hw_sampler nv2a_hw_d3d11.inc:1382-1402: one boolean `lin` (mag==2 || any linear min incl. 3 and 5) picks MIN_MAG_MIP_LINEAR or ..._POINT; MaxLOD = FLT_MAX, no MipLODBias, no MinLOD. And hw_texture:1364-1372 uploads level 0 only and calls GenerateMips (box filter) when `levels > 1`, discarding the authored mip levels the title shipped.
- Theirs: TD sampler() 1794-1855 builds D3D11 Filter from separate min/mag/mip (nv2a_tex_filter_linear/mip), LOD bias (5.8 fixed, filter bits 0-12), MinLOD/MaxLOD from texture CONTROL0, cache of 256 sampler states keyed on all of it. TD tex_upload 1632-1668 uploads every guest mip level (swizzled/DXT levels decoded, count clamped to the chain length and CONTROL0 max-LOD+1; level count is in the cache key and in the hash). MC and Metal/Vulkan add `nv2a_host_aniso()` and `nv2a_host_lod_bias()` options.
- PD symptom: point-sampled-min/linear-mag textures (UI, pixel-art, masks) look soft; nearest-mipmap-nearest textures blur; authored mips with alpha-tested foliage/hair or pre-filtered masks differ from GenerateMips (thinning or fattening alpha, shimmer); textures with a LOD bias for sharpness look blurry. Also ours' `levels` is not in the texture cache key (nv2a_hw_d3d11.inc:1229), so a texture whose level count changes keeps the old SRV.
- Effort M. Licence MIT. Note TD (and MC) still decode DXT to BGRA on CPU: fine but see #8.

### 6. [P2] Missing texture formats (white fallback) and palette bounds
- Ours: linear_twin nv2a_pb_exec.c:893-905 and sample_tex lack: swizzled 0x27 (R6G5B5), 0x28 (G8B8), 0x32 (Y16), 0x38 (R5G5B5A1), 0x39 (R4G4B4A4), 0x3A-0x3C (swizzled A8B8G8R8/B8G8R8A8/R8G8B8A8); linear 0x17 (G8B8/V8U8), 0x35 (Y16), 0x37, 0x3D, 0x3E. They land on `argb = 0xFFFFFFFF` + s_hw_fmt[].texels_bad (nv2a_hw_d3d11.inc:1228) -> opaque white.
- Theirs: TD nv2a_backend_common.h:44-60 (linear_twin full table) and the per-format decoders in nv2a_backend_common.c nv2a_tex_decode_texel (0x17, 0x35, 0x37, 0x3D, 0x3E, 0x3F-0x41); undecodable formats draw magenta and are logged once (instead of white); palette index checked against pal_len.
- PD symptom: any normal map / lightmap / mask in G8B8, 16-bit variants appears white. Check cheaply: RECOMP_HW fmtaudit already counts texels_bad per format (s_hw_fmtaudit); if it ever reports non-zero, this is the cause.
- Effort S (table + 6 decoders, each ~5 lines). Licence MIT.

### 7. [P2] GET_REPORT / visibility test never counts pixels on the hw path
- Ours: nv2a_pb_exec.c:3276-3295 writes `s_gpu.zpass_count` into the report; that counter is only incremented by the CPU rasteriser (pb_exec.c:2131), and the hw draw path never touches it. With RECOMP_HW on, every report says 0 pixels visible and immediately "done".
- Theirs: TD nv2a_pb_d3d11.c:3593-3700 (occ_* + D3D11_QUERY_OCCLUSION via nv2a_occ in nv2a_backend_common.h:951-1013): status 0xFFFFFFFF "pending" first, filled when the query completes (so the title's GetVisibilityTestResult poll works), counts scaled down for render scale; nv2a_pb_poll lets a deferred report finish while the title only spins. Documented in TD's source as needed by Burnout 3's culling.
- PD symptom: only if PD calls BeginVisibilityTest (lens flares, occlusion-culled objects/lights, shadow culling): everything is judged hidden. First verify: run the existing report and see the "visibility reports N" line, or log method 0x17D0 hits.
- Effort M (~150 lines; D3D11 has the query objects). Licence MIT. Drop to P3 if PD never issues 0x17D0.

### 8. [P2] DXT upload decodes every texel on the CPU and uploads BGRA8
- Ours: hw_texture 1313-1340 calls sample_tex per texel into a malloc'd BGRA buffer for all formats, frees, and repeats whenever the hash changes; DXT1 costs 8x the VRAM/bandwidth. A 1024x1024 DXT costs a few ms on the executor thread (stutter when a level loads).
- Theirs: MC tex_get (nv2a_gl.c:880-905, tex_mips_dxt 708) uploads DXT1/3/5 compressed straight from guest memory (no decode, 4-8x less data; the file comment quotes about 70 ms per MB of textures decoded on a console); linear A8R8G8B8 also uploaded directly with GL_UNPACK_ROW_LENGTH. D3D11 equivalent: DXGI_FORMAT_BC1/BC2/BC3_UNORM with UpdateSubresource of the block data (swizzled-DXT is not swizzled for compressed formats: blocks are linear), plus a CPU decode fallback if the device refuses (feature level 10+ always supports BC).
- Side benefit: removes the DXT1 one-bit-alpha black-fringe experiment (RECOMP_HW_DXT1DILATE) question: BC1 hardware filtering is premultiplied-free the same way, but check.
- Effort M. Licence MIT. Perf only; do after #5 (shares the per-level upload loop).

### 9. [P2] Render-target aliasing decided by frame age, not by whether the guest memory still holds the target
- Ours: hw_texture:1196-1232 uses "drawn within 4 frames / 30 frames / same width" heuristics (RECOMP_HW_ALIAS_STALE, ALIAS_OLD) to decide whether a texture at a render target's address is the target or CPU data (movie buffer at 0x02589000 vs old 512x512 glow target).
- Theirs: TD rt_drop_if_stale (nv2a_pb_d3d11.c:693) + nv2a_rt_own_* (nv2a_backend_common.c:126-162): a hash of the guest bytes under the target is taken when the target is made/drawn; once per flip a changed hash means the title rewrote that memory, the target is dropped, and the memory is read as a texture. Also retires any target whose memory range overlaps a new target (nv2a_rt_overlap). No time constants.
- PD symptom: the flat/black/olive movie frames and the intermittent stale-target problems you fixed with timers; a streaming level that loads textures over an old render target's memory can still sample the dead target for up to 30 frames.
- Cost: one hash per live target per flip (RTs are the big surfaces: 640x480x4 = 1.2 MB each: ~0.2 ms each with the 4-lane hash) - only for targets not drawn this flip. Effort M; interacts with s_hw.biggest/front logic. Licence MIT.

### 10. [P2] Points and lines are dropped; FRONT_AND_BACK culling draws everything
- Ours: nv2a_hw_d3d11.inc:2553 and 2816, nv2a_pb_exec.c:2571 "points and lines: not yet" - a silent skip (draw_program_batch returns 1 = handled). Cull: nv2a_hw_d3d11.inc:1873 `cull_face == 0x404 ? FRONT : 0x405 ? BACK : none` so 0x408 (FRONT_AND_BACK) draws the geometry unculled.
- Theirs: TD convert_indices (1220-1229) + nv2a_prim_to_list handle POINTS/LINES/LINE_STRIP/LOOP into D3D11 POINTLIST/LINELIST; TD rasterizer_state():1357-1374 `cull_all` skips triangle batches for FRONT_AND_BACK (counted in s_draws_culled).
- PD symptom: any debug/aim lines, laser beams drawn as line lists, sparkle/dust particles drawn as points (point size is a vertex attribute, SET_POINT_SIZE 0x043C, point sprites 0x0A30; neither TD nor MC implements sprites either, so they would be 1-pixel points). 0x408 culling is rare (shadow volume pre-passes?) - check with the cull log.
- Effort S for lines/points as lists (expand index conversion; HwVertex path already has per-batch indices), plus 2 lines for cull-all. Licence MIT.

### 11. [P2] Format coverage of the combiner shader: dependent AR/GB texture modes (15/16) missing; texture-stage disable and alpha-kill not decoded
- Ours: stage_tex() in the HLSL (nv2a_hw_d3d11.inc:~326) handles modes 0-5 only; 6-17 (BUMPENVMAP, DOT_*, DEPENDENT_AR/GB...) return (0,0,0,0). SET_TEXTURE_CONTROL0 (0x1B0C: enable, min/max LOD, ALPHA_KILL_ENABLE) is not captured in tex_stage_method (pb_exec.c:3050).
- Theirs: MC gl_psh.c:266-300 does DEPENDENT_AR/GB (colour grading lookups), CLIP_PLANE; TD nv2a_stage_decode (nv2a_backend_common.h:518) reads enable/alpha-kill/LOD clamp, nv2a_alpha_kill_mask:496. Nobody has bump-env/dot-product on D3D11 (TD notes it as a known ponytail), so no code to lift for those.
- PD symptom: if PD uses reflective/bump materials or colour-grading via dependent reads, they render black/flat. Probably few (PD is 2005 Xbox, mostly fixed-function and basic pixel shaders) - ours' own comment (pb_exec.c:1792) agrees. Low risk; log stage modes seen first.
- Effort S for AR/GB (10 lines HLSL); M for decode of CONTROL0. Licence MIT.

### 12. [P2] AA surfaces
- Ours: no handling of SET_SURFACE_FORMAT antialias bits (format bits 12-15); a multisampled NV2A surface is stored 2x/4x larger and the clip/vertices scale by it.
- Theirs: MC nv2a_gl.c:199, 310, 413, 1485, 3045 (aa_sx/aa_sy per surface, scissor and tex-scale adjusted); MC executor nv2a_pb_exec.c:556, 890-912 (vertex scale), 1038.
- PD symptom: if the format dword in the "[HW] surface 0x... (format %08X ...)" log line (nv2a_hw_d3d11.inc:1015) has bits 12-15 non-zero, the picture would be half-size/offset. No evidence either way in the logs I could see; verify with one log line. Skip if zero.
- Effort M if needed. Licence MIT.

### 13. [P3] Per-draw performance: state dedupe, native vertex formats, 32-bit index range, thread decoupling
- Ours: hw_bind_draw_state (nv2a_hw_d3d11.inc:1797-2190) sets RT, viewport, scissor, rasteriser, blend, depth/stencil, input layout, topology, VB/IB, shaders, 4 SRVs and 4 samplers on every draw; vertices are expanded to HW_VSP_SLOTS float4 via CPU fetch_attr (128 bytes/vertex) even for stride-aligned float streams; draw_program_batch (2521) allocs 16-bit indices, `n > NV_MAX_INDICES (65536)` falls back to the interpreter, remap/stamp tables of 64K entries, all under EnterCriticalSection on the executor thread.
- Theirs: TD `s_sc` shadow (nv2a_pb_d3d11.c:100-160, sc_on) skips re-binding blend/RS/IL/topology/VS/PS/CB/viewport when unchanged; constant buffers memcmp'd against a shadow before Map (2994-3030); native-format vertex copy (native_format 1123, d3d11_on_draw 2874-2940: whole interleaved block memcpy'd, input layout per format set) instead of float4 expansion; 32-bit index buffer (NV_MAX_INDICES 0x80000 in TD nv2a_pb_state.h:58-60, with the "no title meets a limit; BLiNX 2 water grid dropped its far rows at 4096" note); tex lookup hint table (tc_slot) before the linear scan. MC: separate GL thread fed by a command queue (q_draw 4704, q_gl_thread 4811) so decode and GL submission overlap; ring-buffer streaming (ring_alloc 3216). Upstream: xf_queue_* batches CPU triangles (CPU path only, no gain on hw).
- Where it could show in PD: CPU-bound frames with many small draws (menus/HUD, characters in many pieces). I have no frame-time numbers; ours already has hw_perf_flip / RECOMP_PERF, so measure first (draw count vs. D3D calls vs. fetch_attr time).
- Effort: state dedupe S; native vertex copy M-L; queue thread L. Licence MIT.

### 14. [P3] Large batches: indices cap, 16-bit aliasing
- Ours: NV_MAX_INDICES = 65536 (pb_exec.c:234); ARRAY_ELEMENT32 truncates to 16 bit (pb_exec.c, case ARRAY_ELEMENT32); excess indices dropped with `note_index_overflow` (silent mesh tail loss, like the earlier "vanishing meshes"). TD raised to 0x80000 and counts s_idx_over_ffff (value > 0xFFFF) so aliasing is visible.
- Effort XS (raise the macro + check stamp/remap tables, which are 65536-sized in draw_program_batch:2524-2525 and fine as is because the index values stay < 65536). Licence MIT.

### 15. [P3] Semaphore handling hard-codes the DMA base
- Ours: pb_exec.c:3245-3270 `sem_base = 0x3000` hack (documented), SET_CONTEXT_DMA_SEMAPHORE ignored. TD: stores sema_dma/sema_offset, resolves through dma_resolve, refuses writes into the loaded image (nv2a_pb_exec.c:6791-6835) and wakes sleepers (xbox_SpinWake). Fine for PD today; only matters if another D3D version moves the fence page. Effort XS to add the image-write guard.

### 16. [P3] Depth clear ignores clip / clear rect; surfaces never write back
- Ours: hw_clear nv2a_hw_d3d11.inc:2881 clears the whole depth-stencil view for any Z/stencil clear; the colour clear honours clip/clear rect (scissored fill, 2899-2940). TD d3d11_on_zclear (1020) is equally whole-buffer, so this is a shared limitation: a partial Z clear (viewport sub-rect, shadow-map slice) wipes depth outside the rect. Fix needs a scissored depth-only quad (ClearView does not take DSVs). Only matters if PD issues partial Z clears; relates to your shadow-fidelity items.
- Effort M. Not in any fork.

### 17. [P3] Lost device: nobody handles it
- Ours exits on DXGI_ERROR_DEVICE_REMOVED/RESET (nv2a_hw_d3d11.inc:3190-3195). TD/MC/defjam/upstream have no recovery either (grep found none). A recovery path would need a re-create of device, swapchain, all HwSurface/HwTexture/HwDepth/shader objects and a replay of the last-known state; textures/shaders are recreatable (cache keys exist), render-target contents are lost. Approach if wanted: set a flag, free all s_hw.* resources, call hw_init() again, invalidate caches (the blend/depth caches hold device objects too). Effort L.

### 18. [P3] Shader caching and pre-warm
- Ours already persists DXBC by HLSL hash (nv2a_hw_d3d11.inc:430-545) and compiles pixel-shader variants on 3 worker threads with interpreter fallback. Gaps vs TD/MC: no key list/pre-warm at start (first run still stutters one frame per new combiner/VS); cache dir is cwd-relative "bin\shadercache"; no cache hygiene (age/size/version).
- Theirs: TD docs/shader-prebuild.md + nv2a_pcache.c: an append-only key list (vertex-program slots, combiner states, pipeline keys) with versioned header and per-record checksum, replayed on up to 4 threads at startup before the first draw ("Preparing graphics N/M"), plus a shipped "manifest" produced by sweeping the game; MC prog_cache_prewarm (nv2a_gl.c:2356). Defjam v0.10.2 notes: per-shader compile-once-per-run and driver cache; Windows D3D11 explicitly "planned".
- For PD: log each new (VS hash, combiner hash) when first used into a small file and replay it at startup on the existing psv workers; optionally pre-build in an installer step like TD. Effort M. Licence MIT.

### 19. [P3] Frame pacing / present latency
- TD: waitable swap chain with SetMaximumFrameLatency (nv2a_pb_d3d11.c:82-110) and a vblank-link thread; upstream nv2a_flip_hold.c/.h (hold FLIP_STALL until the guest vblank count + swap interval, opt-in RECOMP_FLIP_PACING). Ours presents with ALLOW_TEARING at interval 0 and paces in hw_flip. Compare only if frame-time variance is a complaint. Effort S-M.

---------------------------------------------------------------------------------------------------
## Method / register coverage differences (executor)

Checked by comparing method switch tables (ours nv2a_pb_exec.c:3108-3630, TD nv2a_pb_exec.c:6494-7250 + vsh_method 2124-2230).
- Ours handles equivalently: surface clip/format/pitch/offset, blend enable/func/colour/equation, depth/stencil/alpha test/fog (mode and params), cull/front-face (hw_method), colour mask (4 bits), combiners (ICW/OCW/factors/final), shader stage program, clip-plane mode, viewport offset/scale as c[59]/c[58], ZMIN_MAX_CONTROL (z-clip opt-in), clear rect, semaphore release, GET_REPORT shell, NOP/software method 0x100 hook.
- TD captures but does not use (so nothing to port): polygon offset (3 methods, only traced at nv2a_pb_d3d11.c:2412), SET_SPECULAR_ENABLE/light control/fog gen mode as state.
- Neither implements: point size/sprites, line width, smooth shading modes, texture border colour, multisample, 3D/cube textures on D3D11 (face 0 only; ours sw path samples cubes but hw does not), stencil two-sided, depth bounds.
- TD-only methods worth noting: SET_CONTEXT_DMA_REPORT/SEMAPHORE (stored), CLEAR_REPORT_VALUE + SET_ZPASS_PIXEL_COUNT_ENABLE (wired to the backend query), DRAW_ARRAYS start-over-0xFFFF counter, NO_OPERATION as swap hint for flip pacing.
- Pushbuffer decoder edge cases (ours vs TD): NOP/0x0 headers fine; non-incrementing fine; ours accepts any `(w & 0x00030003) == 0` as a method header so bits 29-31 values other than 0 and 2 (e.g. 0x6...) decode as methods, TD accepts only `(w & 0xE0030003) in {0, 0x40000000}` (stricter: fewer false methods after desync); RETURN match identical; JUMP/old-JUMP identical; ours limits 8 jumps per walk (TD none, word limit 1M both). CALL: ours recurses 4 MB deep; TD single-level with in_call guard ("call inside a call" stops).

## Suggested order (smallest evidence-backed wins first)
1. #1 blend op swap, #2 constant-alpha, #10 cull FRONT_AND_BACK (all trivial).
2. #3 adaptive/full texture hash + full palette hash.
3. #6 missing texture formats; keep the "white" fallback but log once and count.
4. #5 sampler + guest mips; then #8 BC upload on the same upload path.
5. #4 instrumentation first, then carry-over + stop_salvage; #7 occlusion only if 0x17D0 shows up.
6. #9 RT ownership by hash to replace the 4/30-frame heuristics.
7. Perf items (#13, #14, #18) after a profile.

Not verified: none of the above was run; PD usage of blend equations, constant-alpha factors, visibility tests, AA and point/line primitives is unknown and each should be confirmed with one counter/log line before spending effort. Paths in TD/MC are line-exact as read; ours' line numbers are from the current files (the .bak copies were not consulted except where noted).
