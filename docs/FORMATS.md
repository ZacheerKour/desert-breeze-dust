# Phantom Dust (OG Xbox) asset formats

Status key: CONFIRMED = parsed across every file of that type without exceptions; LIKELY = consistent with data, not proven; GUESS = from names/headers only.
All integers little-endian. Tool: `tools/analysis/pd_unpack.py` (ak/mk). Survey date 2026-10-14.

## .xmv  (Movie/, 60 files, 2.9 GB) - CONFIRMED
Standard Xbox Media Video. FFmpeg's `xmv` demuxer reads it: WMV2 video (640x352 on the sample) plus 1-3 IMA-Xbox-ADPCM (tag 0x69) 44.1 kHz stereo audio streams.
`ffmpeg -i x.xmv -map 0:0 -map 0:1 out.mp4` worked on `v_demo09_en.xmv` (5 s test, output h264 + aac). Not yet run on all 60 files; widths vary (header dword 0x7F000/0x7E000/0x7D000 prefixes observed).
Port impact: no custom decoder needed, convert offline or use libavformat.

## .ak  (Event/, 93 files) - CONFIRMED animation pack
```
u32 header_size        ; == offset of first entry
u32 n
u32 offset[n]          ; ascending, absolute, last entry ends at EOF
char name[n][]         ; NUL-terminated, e.g. "npc08a_u-stand.ala"
```
Unpacks to 1694 `.ala` blobs. Leading dword of each blob (version) is 5, 7 or 10.

## .ala  (Player/*/ and inside .ak; 1850 files) - MOSTLY CONFIRMED animation clip
An `.ala` is a **chain of type-5 records** (same record framing as `.alr`: `[u32 type][u32 size]...`, ends with an 8-byte terminator). All 1850 files / 46,577 records are type 5.
Type-5 record (this is also the "curve" record that appears in `.alr` root chains):
```
+0x00 u32 5 | +0x04 u32 size | +0x08 f32 duration (frames)
+0x0C u16 target (bone index; in range for every NPC model/clip pair checked) | +0x0E u16 kind
+0x10 u32 n_aux | +0x14 u32 n_main | +0x18 u32 0
+0x1C u32 w7 = (aux_element_bytes << 16) | (aux_channel << 8) | low byte
+0x20 aux track (n_aux elements), then main track (n_main elements); total padded to 4 bytes
```
| kind | main track | status |
|---|---|---|
| 7 | rotation: 7 B/key = `u8 frame` (strictly increasing in all 8,975 sampled tracks) + 3 x `s16` Euler angles, **radians = raw * pi / 16384** (max raw seen is exactly +-16384 = +-pi). Size rule `32 + n_aux*aux_bytes + 7*n_main` (padded) fits **27,010 / 27,010** records | CONFIRMED |
| 8 | scalar curve: `n_aux` keys of (f32 time, f32 value), `w7 = 0x80500` style. Size rule fits 14,067 / 14,067 | LAYOUT CONFIRMED, meaning of targets not mapped |
| 3 | `32 + n_aux*aux_bytes + 3*n_main` fits 4,057 / 4,057. Subtype `w7 = 0x10000B`: 3 B/frame = `u8 frame`, one smoothly varying `u8`, one constant `u8` (scale unknown). Other subtypes (`0x10000C`/`0x10000D`) look different | NOT DECODED |
| 4, 5, 6 | 1357 / 65 / 21 records, not examined | UNKNOWN |

Aux track of a kind-7 record (element size = `w7 >> 16`; channel = `(w7 >> 8) & 0xFF`; matched against the bone's bind translation across all NPC model/clip pairs):
- 16 B, channel 0x00: timed translation `(f32 t, x, y, z)` (594 of ~640 within 0.5 of bind)
- 8 B, channel 0x04: timed translation **X** `(t, v)` (105/108 match bind X); channel 0x05: timed translation **Y** (54/54)
- 12 B, channel 0x03: constant translation vec3 (529 of 542 equal bind); channel 0x01: another constant vec3 (scale-like, not verified)

**Evaluation** (`tools/analysis/pd_anim.py`): per bone, linearly interpolate the Euler angles and the timed translation between keys, replace the bone's local `rot`/`t` from the skeleton (type 3), compose `parent * (T * Rz*Ry*Rx * S)`. Verified visually: for NPC `npc05a` the skeleton-driven mesh group goes from the bind T-pose to a natural standing pose (arms down, weight shift, legs apart) at frames 30 and 75 of `npc05a_u-stand.ala`, which confirms the angle scale, Euler order (at least for these poses) and translation handling. Frame rate is not known (clip durations seen: 30, 40, 65, 120, 240 frames).
**Textured bodies**: resolved, they are palette-skinned (see `.alr` Skinning). `tools/analysis/pd_skin.py <model.alr> <outdir> <frame> [clip.ala]` exports a posed, skinned, textured OBJ.
Tools: `tools/analysis/pd_anim.py`, `tools/analysis/pd_skin.py` (skinned posing), `tools/analysis/export_anim.py` (skeleton + sampled clip to JSON), `tools/analysis/render_pose.py`, `tools/analysis/render_groups.py`, `tools/analysis/anim_coverage.py`.

## .mk  (Event/, 93 files) - CONFIRMED model pack
```
u32 type_or_total      ; meaning unknown (0x17, 0x41, 0x50, ...)
u32 n
u32 data_offset        ; always 0x1000 (page-aligned)
u32 offset[n-1]        ; offsets of files 2..n (page-aligned)
char name[n][]         ; NUL-terminated, e.g. "npc08a.alr"
```
Each embedded file starts with `u32 0x11`, i.e. it is an `.alr`. Unpacks to 322 `.alr`.

## .alr / .e  (Map/, Menu/, Player/, boss/, Effect/ and inside .mk; 361 on disc + 322 embedded) - MOSTLY CONFIRMED
Engine resource file, loaded whole into RAM and relocated in place by `FUN_001509b0` (called from `LoadModel`, 0x1520A0). Parsed without error across all 361 on-disc files.

**Layout** (all LE): `file = meta region (meta_size bytes) + data region (data_size bytes)`; `len == meta + data` for every on-disc file.
```
0x00 u32 0x11            ; record type 0x11 = header record
0x04 u32 header_size     ; the header IS record #0 of a chain
0x08 u32 flags           ; low byte 'R' (0x52) required by loader
0x0C u32 meta_size       ; records live here; GPU data starts at file offset meta_size
0x10 u16 n_roots
0x14 u32 data_size
0x20 u32 root_offset[n]  ; each is the start of a record chain (walk until type 0)
```
**Record chain**: `[u32 type][u32 size] payload...`, next record at `+size`, chain ends at type 0. The top chain from offset 0 holds the header, texture table and shared tables; each root chain is mostly animation curves. The loader dispatches per type through a handler table at VA 0x2A40E8 (8 bytes/entry: load fn, unload fn). Types 1-22 are live (types 24-45 mirror them). Types 5, 7, 8, 10, 13, 14, 16, 17 have the no-op handler (pure data, no relocation).

| type | meaning | status |
|---|---|---|
| 0x15 (21) | texture table: `u32 n`, then n x 28 B = D3DTexture{Common,Data,Lock,Format,Size} + u64 hash. `Data` = offset into data region. Format byte `(fmt>>8)&0xFF`: 0x0C DXT1, 0x0E DXT3, 0x0F DXT5; w/h = `1<<((fmt>>20)&15)`, `1<<((fmt>>24)&15)`; mips `(fmt>>16)&15`; swizzled 16/32-bit formats also occur | CONFIRMED (2954 of 2963 textures decode; the other 9 are 1x1 placeholders of code 0x1F). Codes seen: 0x00 L8, 0x02 A1R5G5B5, 0x04 A4R4G4B4, 0x05 R5G6B5, 0x06 A8R8G8B8 (all Morton-swizzled), 0x0C/0x0E/0x0F DXT1/3/5 (linear blocks), 0x12 LIN_A8R8G8B8 and 0x13 LIN_L8 (dims from the Size field: w-1 in bits 0-11, h-1 in bits 12-23), 0x28 V8U8 (bump). Visual check on every non-DXT format (logos, text, clouds, normal maps all legible). `tools/analysis/pd_tex.py` |
| 0x16 (22) | vertex-stream table: `u32 n`, then n x 28 B: `[u32 fmt: byte2=stride, byte0=vtype][u32 ?][u32 vertex_count][0][1][u32 data_off][0]`. Streams are packed back-to-back in the data region (`off_next = off + count*stride`) | CONFIRMED |
| 2 | mesh: `+0x0C f32 center[3], +0x18 min[3], +0x24 max[3]`, `+0x3C u32` mesh ordinal, `+0x40 u16 material idx`, `+0x42 u16 bone idx`, `+0x44 u16 vertex-stream idx`, `+0x46 u16 D3DPRIMITIVETYPE (5 list, 6 strip, 7 fan)`, `+0x48 u32 first vertex (base)`, `+0x4C u32 nverts`, `+0x50 u32 nidx`, `+0x64` header word (0), **`+0x68 u16 idx[nidx]`** (absolute indices into the stream, `min == base`), then padding to 4 bytes, then (skinned meshes only) the skin block below. **Correction (2026-10-14):** earlier notes put the index list at +0x64; that shifted every list by one index. Validated: with +0x68 `min(idx) == base` holds for 22,287 of 22,329 meshes (964 for +0x64), mean triangle edge length median 8.3 vs 14.2 (smaller in 89% of 487 sampled meshes), vertex-normal alignment rose 0.73 -> 0.94, and the textured `boss00` stopped looking shattered | CONFIRMED |
| 1 | **material table**: `u16 n` at +8, entries of 76 B from +0x0C. `+0x16 u16` = nibble mask (nibble j describes texture slot j: 1 = diffuse, 2 = normal map, 4 = other), `+0x18 4 x u16` = indices into the 0x15 texture table; `+0x34` ff000000 and `+0x38` RGBA-like color words also seen. A mesh's `+0x40 u16` indexes this table. Mask 0 means untextured (collision/shadow/helper meshes in boss files) | CONFIRMED (diffuse+normal pairs match the texture sheets; UVs line up on the right textures) |
| 5 | animation record (same layout as an `.ala` record, see below). In `.alr` root chains these are mostly texture/UV/effect curves for Player/Menu files | CONFIRMED layout |
| 3 | **bone table / skeleton**: `u16 n` at +8, entries of 64 B from +0x10: `f32 t[3]` (local translation), `f32 rot[3]` (Euler radians), `f32 scale[3]` (always 1.0 seen), `u16 flags, u16 id` (id = own index), `u16 ?, u16 parent` (0xFFFF = root), 16 B of unknown (hash/stale pointer-like). After the n entries come n runtime 4x4 matrices (identity in file) plus padding. | CONFIRMED structure (43/43 mesh files have one; 1-820 bones; zero bad parents, zero cycles). Euler order unverified (Rz*Ry*Rx assumed; almost all non-zero rotations are single-axis) |
| 17 (0x11) | not a separate record: it is the file header (record #0) | CONFIRMED |
| 18 (0x12) | 52 B mesh instance: handler resolves the `u16` at +0x20 as an index into the type 2/18 records of the current chain and stores the owning node; floats at +0x0C/+0x14 (x,z?) and +0x18 (scale?) | LIKELY |
| 19 (0x13) | 136 B per-object parameter block (e.g. floats 100, 430, 350 at +8..+0x10, rest zero). Its handler only stores a "current node" pointer which the *next* records of type 2/18/20 capture at +0x54/+0x30/+0x28 | LIKELY (role as owner node), contents unknown |
| 20 (0x14) | 44 B, handler instantiates a runtime object via a vtable call (probably an effect emitter) | GUESS |
| 10 | 48 B camera keyframe in `stNNcam.alr` (floats: position 26.9/414.6/60.0, rotation/target, one 0.95 value) | LIKELY |
| 13 (0x0D) | 12 B end-of-object marker, one per root chain | LIKELY |
| 7, 16 | larger data records (7: 208-11392 B in camera files, 16: 144-72044 B in boss files), meaning unknown | UNKNOWN |

**Skinning** (two kinds). (1) *Rigid*: a mesh carries one bone index (`+0x42`) and its vertices are bone-local (bounds equal the header bounds). (2) *Palette skinning* (vertex masks with bits 9 and 10, e.g. the textured bodies of NPCs and bosses): after the index list (padded to 4 bytes) comes the skin block `[u32 flag][u32 n][n bone-index bytes, padded to 4][n x 4x4 matrices]` (matrices are identity in the file, runtime-filled). Per vertex, two influences: palette index `(reg - 120)/3` from slot bit 10, weights from slot bit 9 (see Vertices). Skinned vertex in bind model space: `v' = sum_i w_i * World_pose[bone_i] * inverse(World_bind[bone_i]) * v`. Evidence: palette bones lie within ~1 unit of the mesh centroid; the arm mesh has a 12-bone palette (6 per hand) and its register values come in left/right groups offset by exactly 18 (= 6 slots x 3); weights split e.g. 64/191 between adjacent finger bones; skinning the NPC arm mesh at frame 30 reduced its mean distance to the animated body from 1.09 to 0.23 (swapped weights: 0.82), and the textured body visibly follows the clip (arms down, aligned with the proxy silhouette). `tools/analysis/pd_skin.py` implements it. The untextured meshes under the animated root are a rigid *proxy* of the body that follows the clip directly (purpose unknown: shadow/collision?).

**Vertices** (stream entry `w1` is a dword-slot mask): `popcount(mask) * 4 == stride` for all 12 observed formats (strides 12-32); slots are laid out in ascending bit order, 4 bytes each.
- bits 0-2: position xyz (f32). Verified by exact match with mesh header bounds.
- bit 3: packed normal, `x` = bits 0-10, `y` = bits 11-21 (both signed 11-bit, /1023), `z` = bits 22-31 (signed 10-bit, /511). Verified on 775 meshes: unit length (median |n| 1.00, the other bit orders 0.81) and mean dot 0.73 with geometric normals (others <= 0.24); 48% of meshes >0.9, 1% negative.
- bit 6: **UV0** = two u16, scale **/4096**, but **/32768 when the mask also has bit 9** (0x647 / 0x64F; boss00 UVs that read 0-7.9 at /4096 become 0-1.0). Verified: UV-edge/3D-edge length consistency median 0.56 on 413 meshes vs 1.60 for shuffled UVs and 1.2-2.0 for other slots; UV triangles trace the painted islands of the diffuse texture; boss00 UVs sit in 0.0-0.98.
- bit 7: vertex color RGBA8 (often 0xFFFFFFFF).
- bit 20 (8 streams): second UV set, u16 / 4096, very uniform texel density (consistency median 0.16), likely lightmap-style.
- bits 9 and 10 (skinned meshes, e.g. masks 0x647 / 0x64F): **bit 10** = 2 x u16 matrix-register offsets `regA, regB` (multiples of 3; palette index = `(reg - 120) / 3`, reg 96 means no second influence); **bit 9** = 4 bytes, `byte2` = weight of A, `byte1` = weight of B (always summing to 255), `byte0`/`byte3` = other flags (0/255). Bits 12 and 13 look like tangent/binormal-style packed vectors, not decoded. Stride/mask pairs seen: 12/0x7, 16/0xF, 16/0x47, 16/0x87, 20/0x8F, 20/0xC7, 24/0xCF, 24/0x647, 24/0x1000C7, 28/0x64F, 28/0x304F, 32/0x30CF, 32/0x10304F.
Skeleton: see type 3 above.
Tools: `tools/analysis/pd_alr.py` (textures to PNG), `tools/analysis/pd_mesh.py` (positions + triangles to OBJ), `tools/analysis/pd_skel.py` (bones + posed OBJ), `tools/analysis/pd_tex.py` (all texture formats), `tools/analysis/pd_model.py` + `tools/analysis/export_models.py` (textured OBJ/MTL/PNG with bones applied), `tools/analysis/export_all.py`, `tools/analysis/raster.py`/`tools/analysis/uvoverlay.py` (software preview, UV overlay), `tools/analysis/verify_normals.py`/`tools/analysis/verify_uv.py`, `tools/analysis/render.py` (wireframe preview), `tools/analysis/validate_skel.py`. Decompiled loader code is in `out\decomp2` and `out\handlers`.

## .stx  (BGM/, 105 files, 391 MB) - LIKELY streamed music
Magic `STHD`, `u32 0x20030610` (version?), header holds sample rate 0xAC44 = 44100 at +0x20. Audio payload likely Xbox IMA ADPCM (same codec as XMV) but NOT yet decoded. Candidate for vgmstream or a small custom decoder.

## .bin  (Sound/, 83 files) - LIKELY sound bank
73 files magic `STH2`, 10 files start with zeros. Contains multiple sub-sounds (header table with 0x20-byte entries). Not decoded.

## .ssb  (Event/en|jp, Menu, Questdata, System, boss/*; 599 files) - GUESS event script
Header `u32 0x20 (header size), u32 off_a, u32 off_b, u32 off_b, 0, 0xFFFFFFFF, ...`, body of 16-bit words dominated by `0x000E`, so likely bytecode with 2-byte opcodes/operands. Localized: `Event/en/` vs `Event/jp/`. Not disassembled. The interpreter is in `default.xbe` (find via strings/xrefs in Ghidra).

## .cad / .cam  (com/stNN/; 18 each) - GUESS
`.cad`: `u32 n` then float triples (e.g. 129.8, 119.2, -299.4), looks like waypoint/camera path points, up to 674 KB. `.cam`: fixed 776 bytes, leading dword 0x11-0x1C, then float triples. Semantics unconfirmed.

## Others - GUESS
- `.qdt` Questdata: UTF-8 Japanese strings in `jp/`.
- `.epa` com/param/enemyparam.epa: 1004 bytes, `u32 0x52` then repeating small records (enemy tuning).
- `.dat`: `ST00` (22 files, stage data in Map/), `AR01..AR03`, some unknown.
- `.xpr` (6 MB), `.xwb`/`.xsb`, `.ddx`, `.ttf`: standard Xbox/XDK formats.
