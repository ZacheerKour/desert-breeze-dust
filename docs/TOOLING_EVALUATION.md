# Tooling evaluation: awesome-ai-game-dev-tools (2026-10-15)

Source list: https://github.com/SubmitGame/awesome-ai-game-dev-tools (section 1, "Decompile, remix & mash up games").
Method: READMEs read through a summarising fetch, plus the GitHub API for the one candidate that mattered.
Nothing was downloaded or installed. Star counts quoted by the list and by READMEs are unverified unless noted.

## Verdicts

| Tool | Verdict | Why |
|---|---|---|
| bethington/ghidra-mcp | Not now; recipe below | Verified via GitHub API: 5,645 stars, Apache-2.0, not archived, pushed 2026-10-14. Stable v6.0.0 targets Ghidra 12.1.2; our Ghidra is 12.1.4, and the extension "only loads in the exact Ghidra version it was built for", so it needs the 7.0.0-rc pre-release. Pre-release plus exact-version coupling for a modest gain. |
| LaurieWired/GhidraMCP | Skip | Overlaps the above with fewer tools; also pinned to 12.1.4. |
| clearbluejar/pyghidra-mcp | Skip | Same job; no reason to run two. |
| morluto/rea | Skip | Native analysis is delegated to Ghidra/Hopper/IDA, and the README never mentions XBE. Setup edits agent config; examples run `npx -y ...@latest` unpinned. The 265k-star figure is implausible and unverified. |
| rehan-remade/universal-modder | Skip install; borrow ideas | A plugin with hooks, MCP configs and input/process-control tooling is a supply-chain surface we do not need. Its workflow ideas are listed below. |
| N64Recomp README / wiki | Read, no action | Same technique as ours. Tail-call detection, `jr` jump tables to switch, per-function files, and an address-to-function lookup for indirect calls all match what we hit (see RECOMP_ANALYSIS.md). No new method. |
| RTX Remix, Upscayl, chaiNNer | Irrelevant | Texture and remaster tools. |
| Asset extraction / original-Xbox tools | None in the list | Our `pd_*.py` tools and xboxrecomp already cover this. |

## Why Ghidra MCP is not the bottleneck today
Current blockers are runtime behaviour (what the guest is waiting for, why a frame is black), found with the watchdog, kernel logs and `tools/analysis/stackmap.py` / `tools/analysis/mapsample.py`. Reading functions with capstone has been enough. Revisit if we need to understand large stretches of game logic (post-profile state machine, menu code), where repeated decompile/xref queries against an already-analysed project would save the per-call headless import time of `tools/analysis/ghidra-scripts/PdDecompAddr.java`.

## Install recipe if we revisit (needs the user's go-ahead: downloads code)
1. Pick release `v7.0.0-rc.3` (built for Ghidra 12.1.4): `GhidraMCP-7.0.0-rc.3.zip` (759,298 bytes) and `ghidra_mcp_bridge-7.0.0-py3-none-any.whl` (97,714 bytes) from https://github.com/bethington/ghidra-mcp/releases. Re-check the compatibility line in its README first.
2. Put them in their own empty folder under `tools/`; inspect before use. Do not run anything from the download directory.
3. Headless, keeping the project: `ghidra-mcp --headless --binary extracted/default.xbe --project <dir>`, with `GHIDRA_INSTALL_DIR=tools/ghidra/ghidra_12.1.4_PUBLIC`, Java 21 (`tools/jdk`) and `uv` (already installed).
4. Keep defaults: loopback or Unix socket only, `GHIDRA_MCP_ALLOW_SCRIPTS` unset, `GHIDRA_MCP_FILE_ROOT` set to this project.
5. The existing Ghidra project and scripts (`ghidra-scripts/`) stay the source of truth for addresses.

## Practices adopted from universal-modder (no install)
- Resumable status: tracked as beads issues in `.beads/` (prefix `pd-`); `RECOMP_ANALYSIS.md` holds findings.
- Round-trip verification: re-serialise or re-render what we decode and compare. Already used for formats; extend to checking lifted functions against guest behaviour with the ABI check and stack-delta probes.
- Pinned baseline plus private decompile: `extracted/default.xbe`, the Ghidra project and `.bak-YYYYMMDD` copies of every edited file.
- Lookup tables for symbol/API correspondence: `recomp-analysis/pd_analysis.json` (kernel ordinals) plus `seed_functions.json`.

## Related projects the user pointed to (2026-10-15)
Read via summarising fetches of the repo pages and (for rePOP) its docs; nothing downloaded. Star counts as shown by the pages.

### FulGerNet/rePOP, branch reSOT (Prince of Persia: Sands of Time, original Xbox) - most relevant
Same pipeline as ours (xboxrecomp, vendored and patched per target; MSVC x64; D3D11 renderer; emulated OHCI and APU). 8 commits, 3 stars: small but concrete. Lessons mapped to our work:
- Wrong seeds are worse than missing ones: a seed must start right after a decoded `ret`/`nop`/`int3` (never after `jmp`) and prebuilt library data may only name code in its own library. Their `tools/seed-scan.py` encodes this. Ours: `seed_vtable_thunk` fragments split real functions (e.g. 0x77080 at 0x7709D); we patched around it with jump-closure extension in `tools/pipeline/pd_extend_ends.py`.
- Jump targets with no function become empty stubs in `recomp_stubs_unresolved.c`: silently skipped code. Ours: 46 stubs; audit tracked in beads.
- Same bring-up order we followed: seeds from `[ICALL] Failed to resolve`, D3D fence via `BACK_END_WRITE_SEMAPHORE_RELEASE` (their demo: device global + offsets `+0x2C/+0x30`), watchdog stack dump for stalls, `RECOMP_FRAME_TRACE` for rendering. They also record the XBE address behind each target-specific change in `docs/VALIDATION.md`.
- `RECOMP_HEAP_RECLAIM=1` fixed an out-of-memory freeze (NtFreeVirtualMemory returned nothing). Trying it on Phantom Dust.
- Black screen after movies: a game loop polled `GetExitCodeThread` on a null ETHREAD; fix returns a per-handle block for `PsThreadObjectType` in `kernel_bridge.c`. Check whether our post-profile state polls a thread handle.
- Renderer fixes that can yield black or missing geometry: constant vertex attributes must return the values from `SET_VERTEX_DATA*` (not 0,0,0,1); `ARRAY_ELEMENT32` with a larger index batch; depth-disabled draws must not touch depth.
- Their retail "Saving profile" dialog also lingers (about 40 s after the save, cause unestablished), the same stage where ours goes black.
- USB pad enumeration there is flaky (about 1 start in 4); they bypass USB by replacing XAPI input on port 0. `RECOMP_NV2A_ACK_SPIN=1` changes enumeration odds.
- Hand-written overrides live in `port/game/`; per-target hooks in `source/<target>/target_hooks.c`.

### GTTeancum/OpenJKDF2ogx - not applicable
Source-level engine port that runs on real original-Xbox hardware (XDK 5558), not a recompilation. Its only transferable point: it abandoned a custom D3D8 backend after NV2A draw-submission hangs and used FakeGL instead. Ships a cutscene converter binary and FFmpeg builds; not something to adopt.

### FrankHUN88453/CoD3Recomp - not applicable to the CPU, mildly instructive
Xbox 360 (PowerPC) recompilation using XenonRecomp. Different CPU and GPU. Ideas we already have or could borrow: per-thread sampler naming the guest function (ours: `tools/analysis/mapsample.py` / `tools/analysis/stackmap.py`), a key that dumps the next frame's draw calls (ours: `RECOMP_FRAME_TRACE`), a separate jump-table config, and a boundary-repair scanner (CoD3Scan). Its claims (every instruction translated, full speed) are not shown on the page.
