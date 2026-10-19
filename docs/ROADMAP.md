# Phantom Dust (OG Xbox) native PC port - roadmap

Written 2026-10-14 from the state of `NOTES.md` and `FORMATS.md`. "Verified" means checked against data in this project; everything else is labelled as a guess. Effort sizes are relative (S/M/L/XL), not calendar estimates.

## 1. Where we are

### Decoded and verified (offline tools exist in this folder)
| Area | State | Tool |
|---|---|---|
| ISO extraction | 2062 files, 4.05 GB | `xdvdfs` |
| `default.xbe` | retail XBE, 17 sections, 8166 functions in Ghidra (663 named = XDK libs only), 151 kernel imports | Ghidra project |
| Videos (`.xmv`, 60 files) | play through FFmpeg (WMV2 + IMA-Xbox ADPCM) | ffmpeg |
| Resource container (`.alr`/`.e`) | record-chain format, 361 files parse with zero errors | `pd_alr.py` |
| Textures | 2954 / 2963 decode (DXT1/3/5, swizzled and linear 8/16/32-bit, V8U8) | `pd_tex.py` |
| Meshes | 43 files, 9181 meshes, ~1.29 M triangles; positions, normals, UV0 (+UV1), materials | `pd_model.py` |
| Skeleton | bone table (type 3), 1-820 bones per file, no cycles | `pd_skel.py` |
| Skinning | rigid and 2-influence palette skinning, verified on NPC/boss meshes | `pd_skin.py` |
| Animation clips | `.ak` packs (1694 clips), type-5 records: kind 7 rotation (+translation aux) fully decoded; kind 8 layout decoded | `pd_anim.py` |
| Model packs | `.mk` (93 files, 322 embedded models) | `pd_unpack.py` |

### Partly known
- `.alr` record types 1, 2, 3, 5, 10, 13, 17-22 have layouts; types 7, 16, 18, 19, 20 are only roughly understood (instances, per-object params, effect emitter).
- Animation kind 3 (4057 records, hands/fingers in the NPC sample), kinds 4/5/6 (1443 records).
- Vertex slots for tangent-style data (bits 12/13), UV1 meaning.
- Euler rotation order is assumed (Rz*Ry*Rx); right for the poses checked, unproven for multi-axis cases.

### Not started
Audio (`.stx` music 105 files, `.bin` banks 83, `.xwb/.xsb`), event scripts (`.ssb`, 599 files), data tables (`.dat`, `.qdt`, `.epa`, stage `.dat`/`.cad`/`.cam`), effects and particles (`.e`, type 20), vertex/pixel-shader behaviour (NV2A register combiners are set up in code, never examined), UI/menus, fonts, collision, AI, the skill/deck/battle rules, save format (`gstorage`), online/network code.

## 2. Architecture options

| Option | What it is | Pros | Cons |
|---|---|---|---|
| A. Emulate (xemu) | Already exists; needs BIOS/HDD from the user | zero work | not a native port; no mod/QoL, no modern renderer |
| B. Static recompilation | Translate `default.xbe` x86 to C++, HLE the kernel/D3D8/DSound | preserves exact game logic, fastest route to "plays" | NV2A/D3D8-LTCG state reproduction is hard, Xbox-specific timing/threads, binary is statically linked with XDK code, result is a black box that is hard to extend |
| C. Engine reimplementation (recommended) | New engine that reads the original assets (our decoders), reimplements game logic from decompilation | clean architecture, modern renderer/audio/input, extensible, assets stay the user's | most reverse-engineering work; behaviour must be matched by testing |
| D. Hybrid | C, but use B-style recompiled functions for subsystems that resist RE (e.g. a numerically sensitive routine) behind a stable C interface | pragmatic escape hatch | extra tooling; only worth it per-function |

**Recommendation: C with D as an escape hatch.** Reasons: the asset side is already largely solved, so the unknowns are the game logic and scripts, and those are data-driven (about 7.5k unnamed functions in `.text`, 599 scripts). A data-driven engine plus a script VM fits that shape. Static recompilation would still need every D3D/NV2A/DSound behaviour reimplemented, so it does not remove the hard graphics/audio work.

### Proposed module layout
```
engine/
  vfs/        read original files from the user's extracted disc folder or the XISO
  formats/    alr, ala/ak/mk, xmv(ffmpeg), stx/bin, ssb, dat/qdt/epa   (ports of the pd_*.py tools)
  render/     backend over Vulkan or D3D11 (e.g. via bgfx/sokol); replaces fixed-function + register combiners with shaders
  anim/       skeleton, kind 7/8/3 tracks, 2-influence palette skinning (CPU first, then GPU)
  audio/      Xbox IMA ADPCM decode, streaming, 3D/DSP via a mixer library (e.g. miniaudio/SDL)
  video/      libavformat/libavcodec playback of .xmv
  script/     .ssb virtual machine
  game/       stage loader, entities, battle/skill system, AI, camera, UI
  platform/   window, input (gamepad), saves, settings
tools/        converters, dumpers, test harnesses (current Python)
```
Language: C++17 or Rust. Python stays for research tools only. Keep the format code dependency-free so it can be unit-tested against the same files the Python tools already verified.

## 3. Phased plan

Each phase ends with a concrete exit test. Sizes are relative.

### Phase 1 - Asset completeness (M)
1. **Audio**: decode `.stx` (`STHD`, 44.1 kHz, probably IMA-Xbox ADPCM), `.bin` (`STH2` banks), `.xwb/.xsb`. Exit: play all 105 BGM tracks and the sound banks outside the game.
2. **Animation**: kind 3 tracks (3 bytes/frame, frame byte + value + constant), kinds 4-6, confirm Euler order. Exit: every `.ala` clip applies to its model with no unresolved records.
3. **Remaining `.alr` types** (7, 16, 18, 19, 20) and UV1/tangent slots. Exit: all record types of all 361 files have a named layout or are proven unused.
4. **Converter** to open formats (glTF with skins/animations, PNG/KTX2, OGG, MP4) as a debugging aid and quick preview, not as the shipping path.

### Phase 2 - Renderer and a static viewer (M-L)
1. Real renderer with the decoded textures/materials, skinned animated models, vertex colors, normal maps.
2. **Recover the Xbox render state** per material: read how `default.xbe` configures blending, alpha test, fog, register combiners, lighting (decompile the D3D wrapper in `.text`, plus the 58 `D3DDevice_*` functions already named). This is the largest rendering unknown and has not been looked at.
3. Maps: stage `.alr` + `.dat`/`.cad`/`.cam`, cameras, lighting. Exit: fly through every stage with correct materials; side-by-side screenshots against xemu for a fixed set of views.

### Phase 3 - Script VM and data (L, highest risk)
1. Locate the `.ssb` interpreter in the XBE (strings: `quest%03d.ssb`, `st%02d_%02d.ssb`, `initevent`, `bossprog00.ssb`, `light.ssb`), recover the opcode table, write a disassembler. Exit: disassemble all 599 scripts without unknown opcodes.
2. Decode the data tables: `arsenal.dat`, `system.dat`, `charaset*.dat`, `stageset*.dat`, `soundparam.dat`, `profile.dat`, `gsdata_*.dat`, `questdata.qdt`, `enemyparam.epa`. Exit: a documented schema for each, cross-checked against the in-game values seen in xemu.
3. Reimplement the VM and run an event end-to-end (a conversation scene with camera, animation, text, voice).

### Phase 4 - Gameplay core (XL)
Battle system (skills, decks, aura/stat rules), player/enemy controllers, physics and collision, AI (`.cad`, `enemyparam`), camera, menus/UI and fonts, save data (`gstorage`). Work function-by-function from Ghidra with xemu as the behavioural oracle. Exit: complete one story quest and one boss with matching outcomes.

### Phase 5 - Polish and scope decisions (M)
Resolution/widescreen, frame-rate handling (the original's frame timing needs study), controller mapping, saves/settings, and a decision on multiplayer: the XOnline/XNet code (about 46 + 27 named functions) targets Xbox Live, which no longer exists, so online play is out unless rebuilt (local split-screen or a new netcode are the realistic options).

## 4. Biggest risks and unknowns
1. **Script VM and battle rules** are where the game's behaviour lives, and neither has been touched. Expect most of the effort here.
2. **Shader/render state** was never examined; matching the look needs the NV2A configuration reproduced.
3. **Verification**: the user needs a BIOS and HDD image for xemu to capture reference behaviour; those cannot be supplied here. Without a reference, behavioural matching relies on static reading alone.
4. **Animation completeness**: some bones (hands/fingers) use undecoded track kinds.
5. **Legal/scope**: the project reads the user's own legally dumped disc. Do not redistribute game assets or the XBE; ship code only and have users point it at their own copy. An official 2017 Windows release of the game exists (a community mod loader for it, `pd_loader_core`, is public), so decide what the port should add over it (e.g. original-version fidelity, mod support, other platforms).

## 5. Recommended next three steps
1. Decode audio (`.stx`/`.bin`): small, self-contained, immediate payoff.
2. Find and disassemble the `.ssb` VM: it gates everything in Phase 3-4 and settles how large the project really is.
3. Decompile the D3D render-state setup to understand materials and lighting before committing to a renderer design.

## 6. Revision (2026-10-14): goal is a complete working copy for personal use

The recommendation in section 2 optimised for a clean, extensible engine. If the goal is simply a **complete, playable native build for one person**, the shortest path is different: **static recompilation of `default.xbe`** using an existing original-Xbox toolkit, with this project's reverse engineering used to debug and validate it. Web check (not independently verified):
- `sp00nznet/xboxrecomp` (MIT, uses LGPL parts from xemu) lifts XBE x86 to C, builds it with MSVC against a runtime with a kernel shim, D3D8 handling (register combiners, vertex shaders to HLSL, 4-stage multitexture), DirectSound/APU, XInput, and FMV playback. Needs your own disc; ships no game data.
- Its flagship port (Burnout 3) is **alpha**: all modes boot and play, but races run at 2-4 fps (menus ~14 fps) on a 12-core machine because the GPU is emulated in software; some bump-mapping/sky/menu artefacts remain; no save profile; Xbox Live/System Link not attempted. Other titles in it are earlier (boots to main, early crash, first build pending).
- Hard parts it names: vtable/indirect-call dispatch, rare x87/SSE instructions left as comments by the lifter.

What this means for Phantom Dust: our Ghidra data (8166 functions, entry point, kernel imports, named XDK libs) feeds the toolkit's seed-function config; our asset decoders verify that loads/renders are correct; the `.ssb`/battle logic does not need to be reverse engineered because the original code runs. Risks: the toolkit is young and untested on this game, performance may be poor until the GPU path improves, the game links XNet/XOnline (Xbox Live code must be stubbed), and the XDK D3D/DSound code here is statically linked (identifying and shimming it is the main bring-up task).

Fallback that already exists: xemu runs the game as-is (needs a BIOS and HDD image from you).

Suggested order for the personal-copy goal: (1) try the toolkit pipeline on `default.xbe` and see how far lifting gets, (2) stub the 151 kernel imports it does not yet cover, (3) get to the title screen, (4) fix rendering with our decoders as ground truth, (5) only then consider reimplementing pieces (e.g. a faster renderer using `pd_model`/`pd_tex` logic).

### 6.1 Viability read of the xboxrecomp docs (2026-10-14; docs read, nothing cloned or run)
Sources: the toolkit's `docs/INDEX.md`, `GETTING_STARTED.md`, `technical/gap-analysis.md`, `candidate-games.md`, `lessons-learned.md`, `d3d8ltcg-device-context.md`, and the Burnout 3 port's `docs/boot.md`.

**In Phantom Dust's favour (verified from our XBE):**
- Same XDK build as the toolkit's proven title: `default.xbe` lists XBOXKRNL, XMV, LIBCMT, D3D8LTCG, XGRAPHCL, XVOICE, XONLINES all at build **5849** (Burnout 3 is XDK 5849 too).
- `.text` is 1.74 MB, under the toolkit's Tier-1 size bar (<2 MB). Function count (8166) is below Burnout 3's 22,097.
- XMV video is supported (FMV via Media Foundation). Offline single-player story needs no Xbox Live; the toolkit stubs networking (no link).
- Our decoded assets give an independent oracle for "is it loading/rendering right" (textures, meshes, skeleton, clips), which the Burnout 3 write-up says it lacked and had to reverse engineer late.

**Risks specific to this game:**
- **Custom engine, not RenderWare**: library identification there relied on known RenderWare/XDK signatures; with an in-house engine (strings mention "NNX", `LoadModel`, `TextureAllocRes`) fewer functions will be named automatically. The toolkit docs do not address this case.
- **Threads**: the kernel imports include `PsCreateSystemThreadEx` (ordinal 255) and `PsTerminateSystemThread` (258); the toolkit uses a single-thread cooperative model. Needs checking how many threads this game runs and whether they can be serialized.
- **D3D8LTCG per-title work**: the statically linked D3D must be handled by capturing the ~16 KB device context from a live **xemu** session (so a BIOS/HDD image is required) and fixing up many mid-entry stubs with exact stack cleanup. Documented only for Burnout 3; XDK build support scope is "undefined" in the doc.
- **Audio**: the XBE has `DSOUND`, `WMADEC`, `DOLBY` sections. WMA decoding is missing in the toolkit (DirectSound streams/3D/DSP are stubs); whether this game's assets need WMA is unknown (our `.stx` are probably ADPCM, but the WMADEC section is present).
- **Performance**: the reference port runs at 2-4 fps in races (software GPU path) on a 12-core machine; expect poor speed initially.
- **Maturity**: toolkit is at v0.13.x, Burnout 3 is alpha, other titles earlier; open problems include dot-product bump textures and indirect-call (vtable) failures, which the docs call the hardest part.

**Measured update (analysis stages run, see `RECOMP_ANALYSIS.md`)**: 10,821 function entries (12,423 after func_id), 84% instruction reachability, 671 vtables / 5,408 virtual-method entries, 1,199 indirect calls, 270 indirect jumps, 69% of ABI results low-confidence, 49% of functions unclassified, ~48k x87 and ~25k SSE1 instructions, 226 functions in library sections to shim. The lifter has not been run yet.

**Verdict**: plausible but not assured. Best case is a playable-but-slow personal build after many bring-up fixes (Burnout 3 needed 23 numbered fixes, 17 in the toolkit itself); worst case is stalling at the D3D8LTCG/threading layer. The analysis stages (XBE parse, disassembly, function ID, ABI analysis, lift) do not need the GPU and are cheap, so they are the right first experiment: they measure unidentified-function count, indirect-call volume and unsupported instructions before any heavy investment.
