# Oogie's Revenge runtime vs Phantom Dust: design review

Scope: read-only review. Oogie (`R\oogie`, GPLv3, untrusted) was read, never run or built. All descriptions below are in my own words; no Oogie code is to be copied. Phantom Dust (`C:\recomp\phantom-dust-re`) was read only.

Oogie file refs are `file:function` under `oogie\xboxrecomp\runtime\`. Note: Oogie's `docs\` contains only screenshots; the design documentation is the code comments and `README.md`.

Evidence grades: FACT = read in code. HYPOTHESIS = my inference, not verified by running anything. Each hypothesis has a cheap test.

Important context: PD's interpolation lives in `xboxrecomp\src\kernel\nv2a_hw_interp.inc` (1232 lines), included by `nv2a_hw_d3d11.inc`. It is much closer to Oogie's design than the brief implied. It already records draws, matches by key, lerps constants and vertices, has cut detection and a governor. The differences are in HOW the replay is scheduled and presented, in matching quality, and in what the extra picture does on a cut.

---------------------------------------------------------------------------------------------------

## 1. In-between-frame interpolation (the main question)

### 1.1 How Oogie does it (steps)

Where: `d3d8\renderer_d3d11.cpp`: namespace `rec` (recording), `draw_in_between`, `present_in_between`, `present_thread`, `show`, `gfx_present`. The game step is 59.94 Hz and the output is 120 pictures/s.

1. Recording (`rec::draw`, `gfx_clear`, `gfx_copy`, `gfx_set_render_target`, `gfx_backbuffer_snapshot`, `gfx_ambient_occlusion`). While interpolation is on, every renderer entry point appends a command to the current frame's list: TARGET, CLEAR, COPY, SNAPSHOT, AO, DRAW. A DRAW stores the full pipeline state struct, a copy of the vertices and indices, and an index into a per-frame table of the 192 vertex-shader constant registers (768 floats). The constants table is only appended to when the constants pointer or the "constants version" changed, so unchanged constants are shared between consecutive draws.
2. Match key per draw (`rec::key`): hash of current render target id, vertex-shader id, pixel-shader id, topology, the 4 texture ids, vertex count, stride, index count. It contains no per-frame data (no matrices, no vertex values).
3. Two frame slots, `frames[2]`, swap each present (`begin_frame`). `cur^1` is the previous frame.
4. At present time, on the guest's own thread, inside the Swap call and with the guest lock held (`d3d8.cpp: D3DDevice_Swap` -> `gfx_present` -> `present_in_between`), the in-between picture is built synchronously (`draw_in_between`). The game does not run until it is done. This is the key scheduling fact.
5. Matching in `draw_in_between`:
   a. Bucket the previous frame's draws by key, keeping order.
   b. For each draw of the current frame in order, look up its bucket. If the bucket has at most 16 entries, pick the not-yet-taken candidate whose constants changed the least (relative change metric below). Reason in the code comment: alike draws (same skeleton, same prop repeated) come in an order the game sorts, and the order changes as things move, so "n-th with the same key" pairs the wrong objects. If the bucket has more than 16, fall back to plain n-th-in-order.
   c. Relative constant change between two draws = (sum of absolute differences) / (sum of |a|+|b|), accumulated only over the 4-float registers that differ at all (so the large mass of never-changing registers does not dilute the number).
6. Gate 1, too few matches: if fewer than half of the current draws found a partner, or either frame has no draws, do not interpolate (`draw_in_between` returns false).
7. Gate 2, camera cut: compute the relative change for each matched draw that has constants, excluding stencil (shadow volume) draws. Take the MEDIAN. If median > 0.2, it is a cut: return false. (Median, not mean or max, so a few fast-moving objects never trigger it, but a whole-scene matrix jump does.)
8. On gate failure, the caller (`present_in_between`) simply copies the current finished frame into the in-between texture. So on a cut or a load the screen shows the new frame twice (once early, once on time). It never shows a blend and never shows black.
9. Otherwise replay (`replaying` flag stops re-recording): swap the real scene texture for the spare, copy the finished current frame into it (what a non-clearing frame starts on), then re-issue every command of the current frame through the normal renderer entry points. For each DRAW:
   - Exclusions: a draw is NOT blended (drawn exactly as in the current frame) if it has no partner, or uses stencil (shadow volumes are rebuilt each frame from silhouette edges, so n-th quad to n-th quad is not the same edge), or is flat/2D (`GfxState::flat` = vertex shader that does not project; 2D animates by jumping texture coordinates between sprite cells and the mid-point is a smear).
   - Constants: new register block = 0.5 * (current + previous) for ALL 768 floats (matrices, light, fog, bones, everything). Written to a static buffer with a fresh version number so the draw layer re-uploads only when the pair changes.
   - Vertices: only if the partner has identical stride, vertex count, index count and byte-identical indices, AND the vertex bytes differ, blend every float 0.5/0.5. This covers vertices the game animates itself (effects, 2D). Otherwise vertices are the current frame's.
   - Pixel shader constants, textures, UVs: current frame's (not blended).
10. Result lands in `g_scene_mid`. After the replay, the real render target selection is restored (`gfx_set_render_target(target, target_depth)`).
11. Presenting (`present_in_between` tail): two window-sized pictures are prepared on the guest thread, in a "shown set" (alternating between two sets, so one set can be on screen while the next is being built): the blit of the in-between picture and the blit of the real picture. Then a job (set index + timestamp) is handed to the present thread via condition variable.
12. Present thread (`present_thread` + `show`): per job, copy picture 0 to the swap chain buffer and Present with sync interval 0 and the DO_NOT_WAIT flag; then wait for the next display refresh (if the display is 119-121 Hz, via the output's vblank wait) or until job timestamp + 8.34 ms (1001/120000 s) otherwise; if a newer job arrived in the meantime, skip the second picture ("running late"); else copy and present picture 1. `show` takes only the small swap-chain mutex; the D3D11 device is set multithread-protected, and the thread touches no pipeline state, only copy + present. `DO_NOT_WAIT` is deliberate: a blocking Present would hold the shared device and stall the game's drawing.
13. Window resize: the swap chain and the shown-set textures are rebuilt only when `try_lock` on the swap mutex succeeds, at a moment the present thread is idle. A resized-but-not-yet-matching picture is never presented (it caused white flashes).
14. Enabling rule (`gfx_init`): 120 mode only if the compositor refresh is >= 100 Hz (auto) or forced by env. Reason in comment: on a 60 Hz screen, presenting extra pictures kept the device inside Present and slowed the game to half speed. So on a 60 Hz screen Oogie simply does not interpolate.
15. Governor (`present_in_between`, first block): measure time between presents. A gap above 18.5 ms three times in a row pauses interpolation for 3 s (doubling to 60 s while it keeps happening). 600 consecutive on-time frames (10 s) reset the pause length to 3 s. While paused, the in-between picture is a copy of the real frame (same fall-back as a cut), so cadence stays uniform.
16. Side-fill hysteresis (`gfx_flat_frame`): the "2D-only frame" flag only flips after 3 consecutive frames of the other kind. Widescreen only; irrelevant to PD.

### 1.2 Why it avoids shake and black frames

FACT-based reasons:
- Synchronous replay on the guest thread, guest lock held. The recording, the textures, the render targets and the vertex data of frame N are exactly what is alive when the replay runs. Nothing else is drawing. There is no race between the replay and the next frame.
- Matching picks the least-moved alike draw, so repeated models do not swap places between the two frames (swapping would produce a visible jump of those objects, which looks like shake).
- Blending is whole-state: all 768 constants. The camera, the world matrices and everything they feed move together. There are no per-draw "reject and draw at N" decisions for matched draws, so nothing is half-blended relative to the camera. (Rejected = unmatched, stencil, flat only.)
- A bad pair of frames never produces a blend: cut or load or low match count produces a repeat of the real frame, which is always a valid picture. So no black, no smear.
- Both pictures of a game frame are fully built before either is shown, and the present thread only copies and presents on a fixed cadence, without waiting for the device. The spacing between the two pictures is not affected by how long the replay took. On a 120 Hz panel it is locked to the display.
- It refuses to run where the cadence cannot be locked (display < 100 Hz).

### 1.3 What PD does differently (file:line)

All FACT unless marked HYPOTHESIS.

D1. Replay runs asynchronously, concurrently with the next game frame. `nv2a_hw_interp.inc:1074-1083` (`s_ip.defer` default on, `ip_flip` queues a job) and `ip_worker` at `:1170-1210` on its own thread, below-normal priority, replaying in 0.4 ms slices of the D3D lock. The executor keeps drawing frame N+1 into the same real surfaces in between slices. Everything the replay reads that is not a private copy is live shared state:
   - First-touch initial content of a twin: `ip_twin()` `:560-566` copies from the REAL surface (`CopyResource` or `ip_blit_down`) the first time a replay draw touches it (for a clear that is not a full colour clear, and for every draw). The real surface may by then hold frame N+1's clear or partial draws. HYPOTHESIS: this is the cause of the black cutscene frames. Cutscenes use fades, letterbox passes, partial clears and retained-framebuffer effects; the first record on a surface is often a blended draw over previous content, or a clear that N+1 has already executed.
   - Texture bindings: `ip_replay_draw` `:722-733` uses the recorded SRV of every texture; for render-target textures it uses the twin only if the twin was already touched this epoch (`tw->epoch == s_ip.epoch`), otherwise falls back to the recorded SRV, which is the real render target (live, being overwritten by N+1). Uploaded textures that the guest re-uploads (dynamic textures: movie, face, lightmap) also show N+1 content.
   - Oogie has none of this because its replay is synchronous. The cost PD pays for async is correctness.
   - Test (cheap, decisive): run a black-cutscene repro with `RECOMP_INTERP_DEFER=0` (already supported, `:` `ip_init`). If the black frames vanish, D1 is confirmed. Also try `RECOMP_INTERP_SCALE=0` to rule out the scaled-twin path.

D2. Matching takes the n-th draw with the same key. `ip_finalize` `:316-362` builds a hash of (kind, normalised surface, vskey, vmask, nvert, nidx, 4 texture identities) and appends an occurrence counter on collision. `ip_find` `:364` returns that. If the game's sort order of alike draws changes between frames, pairs are wrong. The wrong pair passes `ip_plan_draw` as long as the constants differ by <= 1.0 relative (`prog_max` default 1.0, `:407-431`), so the draw is lerped toward another object's matrices: it flies across the screen for one picture. HYPOTHESIS: this is a likely source of "shake", especially in battles with many identical cards, props and particles. Oogie's least-moved candidate rule exists for exactly this. Test: `RECOMP_INTERP_DBG=3` histogram (`[IPH]`) in a scene that shakes; if many draws sit in the 0.3-1.0 bins while the scene is calm, wrong pairs are happening. Or implement D2 fix and compare.

D3. The extra picture is shown on an unsynchronised, tearing swap chain. `nv2a_hw_d3d11.inc:723-739` (flip model + ALLOW_TEARING when the factory allows it), `:3186-3189` (Present interval 0 with ALLOW_TEARING), `hw_flip` pacing `:3472-3504` sleeps to the console rate (`pace_hz` 60). In windowed mode DWM composes once per refresh and takes the latest buffer; presents that land in the same refresh replace each other, and ones that straddle refreshes are shown for 1 or 2 refreshes. The presenter's deadline `:1152` is "16.7 ms after the interpolated picture was presented", and that presentation time is `flip + replay latency` (docs say 7-9 ms, `RECOMP_ANALYSIS.md` interpolation section), which varies per frame with scene cost and lock contention. So the time between real(N-1) and mid(N) changes by (replay_N - replay_N-1) every frame. HYPOTHESIS: a few ms of jitter on a 16.7 ms step, plus phase beating against the monitor, is visible as shake on camera pans, and gets worse on fast GPUs where the DWM catches every present. Test: `RECOMP_PERF=1`, look at `[PERF-PRES]` (std of present interval; the notes say std ~6 ms in a slow hub, which is far from a clean 16.7); correlate with a capture of a pan.

D4. The real frame is presented through `hw_present_locked`, which takes the big D3D lock and does clear + blit + gamma + overlay + masks (`:3011-3199`, called from `ip_presenter` `:1131-1137`). The executor holds this lock while it works. If the presenter thread waits for it, the real picture is late. Oogie's present thread avoids all shared locks: it only copies a pre-built picture.

D5. On a cut or mismatch PD shows the real frame once and no extra picture (`ip_flip` returns 0 -> `hw_present_locked` right at the flip, `:3721-3725`). This is safe, but it produces one off-cadence gap. Oogie repeats the real frame instead, keeping the cadence. Minor.

D6. Interpolation is applied on any display. Oogie turns it off if the display cannot lock the cadence. PD (30 Hz game, 60 pictures/s) is on a monitor of whatever rate. On a 144 Hz or 75 Hz monitor with tearing off-grid presents, uneven cadence is guaranteed. HYPOTHESIS, test: the shake reports vs monitor refresh. A user on exactly 60 Hz with vsync should have a good chance of clean results.

D7. Cut test differs. Oogie: median of the L1-type ratio over matched non-stencil draws, threshold 0.2. PD: fraction of PROGRAM draws (denominator includes unmatched) whose Frobenius ratio exceeds 0.5, cut when > 40 % (`:882`, `hi_rel` 0.5, `hi_frac` 0.4). The metrics differ in scale: for small changes Frobenius ratio is about 2x Oogie's L1 ratio, so 0.5 roughly matches Oogie's 0.25 (close to 0.2). Roughly equivalent; not the primary cause. But PD does not exclude stencil draws from the statistic (Oogie does); and PD's CPU-transformed draws do not take part in the cut decision at all.

D8. Partial per-draw lerp. In PD a matched draw may be flagged lerp bit 0 (constants) and bit 1 (position attribute) independently, and CPU-transformed draws are only lerped if they moved < 75 % of their extent and < 160 px (`:399-405`), position attr only if < 25 % of extent (`:447`). At 30 Hz game steps (twice the per-step motion of Oogie's 60 Hz), fast objects, particles and fast camera spins exceed those limits more often. Rejected draws are drawn at frame N while the rest of the scene (camera matrices) is at the midpoint: that object shifts relative to the world by half a step for one picture, then snaps. HYPOTHESIS for shake of fast things (projectiles, particles, character limbs); the notes in `RECOMP_ANALYSIS.md` already mention "particles that move more than 25 %" as unblended.

D9. Governor: PD compares the game's flip interval with and without interpolation (`:1032-1058`, the right metric for "does interpolation slow the sim"). Oogie's is a plain late-present counter. Both are valid; PD's is better for the speed question but ignores cadence irregularity (flips arriving at 33/50/33 ms patterns), which is what judder is made of. See design step 6.

### 1.4 Clean-room design for PD

Goal: make PD's 30 -> 60 pictures/s cadence deterministic and eliminate the async-replay correctness races, without losing the "replay off the guest's critical path" win.

Step 1 (P1, effort S): confirm D1. Run black-cutscene repro with `RECOMP_INTERP_DEFER=0`.

Step 2 (P1, effort M): make the replay safe while keeping it asynchronous. Options in order of cost:
   a. Ordered capture at flip time. In `ip_flip`, before queueing the job (executor thread, lock held), snapshot every real surface/texture the replay can read: create the twin start state at record time instead of first-touch at replay time. Concretely: for each surface touched this frame, when its FIRST record is created (`ip_commit_*`), copy the surface's current content into a per-frame "frame-start" texture (GPU copy, cheap, ordered on the immediate context). The replay starts twins from these snapshots, never from the live surface. Same for render-target textures sampled by draws (the self-shadow copy `ip_shadow` already does the right thing for self-reads; make the other SRV fallbacks use frame-start snapshots too).
   b. Or go synchronous like Oogie, but only the command issue (no waiting on the GPU): replay inline at the flip on the executor, and handle the 8-9 ms stall noted in `RECOMP_ANALYSIS.md` by shrinking what the replay does (see c).
   c. Or a mixed path: replay inline, but cheap: render the in-between picture at the reduced `RECOMP_INTERP_SCALE` and skip draws that cannot move (static draws with identical constants and vertices: copy the result of the real frame for them is not possible per draw; skip only whole passes that are unaffected).
   Recommendation: (a), because the measured problem with inline was the executor being unavailable to the guest for 8-9 ms, and (a) keeps the worker thread.

Step 3 (P1, effort S-M): least-moved matching. After the bucket lookup in `ip_finalize/ip_find`, for buckets of <= 16 draws, choose the candidate with the smallest relative constant change (same metric as `ip_plan_draw`) among those not yet taken; for CPU-transformed draws (no constants) use the smallest mean screen-space displacement. Larger buckets keep the order rule. Edge: candidates have equal nvert/nidx by key; ties break by order. Keep it O(n * bucket) with bucket <= 16.

Step 4 (P1, effort M): decouple presentation. Pre-compose two window-sized pictures in two alternating "shown sets" (mid and real), and let a dedicated present thread own the swap chain (lock only the swap-chain mutex, device set multithread-protected, as Oogie did; for PD the equivalent is the `s_hw.lock` -> a smaller swap lock). The thread presents picture 0 immediately and picture 1 at a vblank (display 59-61 Hz: next vblank via the output; any other rate: the 16.7 ms clock from the job timestamp), skipping picture 1 only if a newer job exists. Use sync interval 0 + DO_NOT_WAIT for the swap, plus an explicit wait on the output's vblank, so the device is never held. Fixes D3/D4.

Step 5 (P2, effort S): enabling rule. Interpolate (auto) only if the compositor rate is within 59-61 Hz, or >= 100 Hz and a clean multiple of 60 (120, 180, 240 -> present each picture for k refreshes). Otherwise fall back to 30 fps real frames (with a one-line log), as Oogie does, and expose "interpolation = on/off/auto" in `settings.ini`. Note `docs\PLAY_ON_A_NEW_PC.md` says interpolation is for "battles"; this makes the rule explicit.

Step 6 (P2, effort S): cut/fault fallback repeats instead of skips. Where `ip_replay` returns 0 for cut, mismatch or failure, schedule the real frame twice (mid = copy of real, no replay) so the cadence is unchanged. Keep the movie exclusion.

Step 7 (P2, effort S): add Oogie's late-frame pause as a second condition in the governor (3 consecutive present gaps beyond a threshold = pause with exponential back-off). For a 30 Hz game the threshold would be ~42 ms on flip gaps, not the 18.5 ms Oogie uses for 60 Hz.

Step 8 (P3, effort S): exclude stencil draws from the cut statistic and replace the cut metric by a median over matched non-stencil program draws with threshold calibrated on PD (use the existing histogram). Keep `HIFRAC` as an override.

Step 9 (P3, effort M): D8: widen the per-draw lerp limits at 30 Hz or replace them with "lerp if matched and topology identical, reject only on implausibly large (> 100 % extent) displacement", like Oogie (which has no displacement limit at all, only cut detection). Test with the dump scripts in `pd-game-v2` (`interp_viz_i.py`).

Verification plan: (1) black cutscenes: cutscene repro with `DEFER=0` vs default vs fixed build. (2) shake: capture with `RECOMP_INTERP_DUMP`; compare mid vs 0.5*(a+b); measure `[PERF-PRES]` std (goal < 1 ms with step 4 on a 60 Hz panel). (3) soak with governor logging.

---------------------------------------------------------------------------------------------------

## 2. Guest CPU lock, priorities, quantum, safepoints

### 2.1 Oogie (`kthreads.cpp`)

FACT, steps:
1. One host mutex + condition variable (`g_m`, `g_cv`) protect a `g_waiting` list of `{ticket, priority, yielded, thread}` and the flags `g_held`, `g_running_priority`, `g_running_since`.
2. Taking the lock (`lock_held`): enqueue a waiter with the thread's current priority, wake everyone, wait until the lock is free AND `next_owner()` names this ticket. `next_owner` prefers non-yielded waiters, then higher priority, then lowest ticket (FIFO among equals). Suspended threads (suspend count > 0) take the lock only to immediately release it and wait for resume.
3. Priorities: Xbox base priority 8 + increment, clamped 1-31, kept per guest thread; service threads (DPC, vblank, audio service) are priority 32. `KeSetBasePriorityThread` updates queued waiters too and, if the target outranks the running thread, preempts immediately (`preempt_for`).
4. Preemption points: generated code contains a `SAFEPOINT` at every function entry and backward branch: `if (g_yield_request) guest_yield(...)`. A 1 ms tick thread (`kernel.cpp: tick_thread` -> `preempt_tick`) sets the flag when `should_yield()` says a waiter should take over: a higher-priority waiter at once; an equal-priority waiter after a 20 ms quantum (`OOGIE_QUANTUM_MS`); a lower one never. A thread that stepped aside through `NtYieldExecution` (`yielded` true) waits until a quantum has passed. Nothing is preempted at IRQL >= 2 (DPC level); yield in `guest_yield` returns at once then.
5. `NtResumeThread`/`KeSetBasePriorityThread` call `preempt_for`: if the target is ready (or will be within 20 ms; a resumed host thread needs a moment to queue) and outranks the running thread, the running thread releases and requeues, so the new thread runs first. This replicates the Xbox behaviour "resumed thread with higher priority runs before the resumer continues". The code comment records two game dependencies: a job thread resumed at priority 10 must already be waiting on its semaphore before its creator releases it; and the frame-building thread at +2 must not interleave with the priority-0 render thread.
6. Kernel waits (`wait_objects`) release the lock while waiting and poll in <= 1 ms steps for timers. Critical sections spin through `wait_for_signal` releasing the guest lock. DPCs and timers run on a service thread at DISPATCH level.
7. `NtYieldExecution` returns "no yield performed" when nobody waits (the game's end-of-frame yield loop depends on that status).

### 2.2 PD (`kernel_bridge.c:9786-9863`, spawn handshake `:461-483, 585-615, 9847`)

FACT:
- `RECOMP_GUEST_LOCK=1` only (default OFF, `guest_cpu_enabled`). It is a single `CRITICAL_SECTION`. Held while running guest code, released around every kernel call. No priorities, no quantum, no safepoints, no tick. The comment itself lists the limitation: a guest thread spinning on a flag with no kernel call deadlocks; "the upgrade is a periodic yield point in the lifted code's backward branches".
- `docs\RECOMP_ANALYSIS.md` (pd-dz0/pd-ciz): the lock was tried, no crash difference seen, left off.
- The spawn handshake (`bd988f9`): the creator waits up to 50 ms for the new thread's first kernel call. It approximates "high-priority new thread runs first" by timing, not by scheduling. It fixed the profile `_ERROR_` bug.
- Host priorities are mapped to Win32 priorities by `kernel_thread.c:215-263`. Win32 priority classes do not reproduce one-CPU run order.

### 2.3 Worth adopting?

Not now. P3. PD has no open evidence that guest races matter (the notes retract the hypothesis), and the lock change is invasive (it needs codegen safepoints in the lifted code; PD's toolkit is MIT and Oogie's lifter is a different one).

If races return, do this in order:
1. Run with `RECOMP_GUEST_LOCK=1` (exists).
2. Replace the handshake's 50 ms timer by a real "ready" rule: on `PsCreateSystemThreadEx` (not suspended) and on resume, if the new thread's priority is above the creator's, yield the guest lock to it until it blocks or finishes its first kernel wait. (Clean-room from the Xbox rule "highest-priority ready thread runs".)
3. Only then add a safepoint macro to the lifted code at function entry and backward branches (a load of a volatile flag; cost small) and a 1 ms tick that sets it when a same-priority waiter has waited 20 ms.

Effort: step 2 = M, step 3 = L. Priority P3. Risk: the SAFEPOINT idea needs the lifter's cooperation, and Oogie's note about the 20 ms quantum choice is empirical for its game.

---------------------------------------------------------------------------------------------------

## 3. Audio output (`audio\audio_out.cpp`, `audio\dsound.cpp`)

### 3.1 Oogie (FACT)

- WASAPI shared mode, event-driven, float mix format (fails if not float 32), 20 ms buffer. The device's event wakes the audio thread (priority TIME_CRITICAL); on each wake it asks for the free frames (`buffer size - padding`) and calls the game's mixer callback for exactly that many stereo frames, then expands to the device channel count (front L/R, other channels silent).
- The mixer (`dsound.cpp: mix`) runs in the pull callback: voices read straight from guest memory under a mixer mutex; completions of stream packets are queued and handed to the game by a separate "audio service" guest thread every 4 ms. So the audio clock IS the device clock: guest-visible stream progress only advances as the device consumes samples. No drift, no queue dropping.
- Silent real-time clock fallback: if no device, the open fails, or the device is lost (GetCurrentPadding/GetBuffer failure), the thread runs `run_silent`: sleeps 10 ms, computes how many frames real time owes, caps a step at 20 ms (a PC stall is not made up), and calls the same mixer into a scratch buffer. After 2 s it retries the default device. Reason in comment: streams (movie sound, music) only complete as they are mixed, and movies wait for their sound, so audio must keep time without a device.
- Output stage: configurable volume (100 = Xbox level, up to 200%), then a peak limiter: gain dips immediately when a sample would exceed 0.95 and recovers with an ~80 ms time constant. Music volume only affects the stereo XACT stream on the default speakers.
- `audio_pause(true)` while the in-game menu is open: the mixer outputs silence and does not advance voices; the guest lock is held by the menu at the same time.

### 3.2 PD (FACT)

- Own APU model (xemu-derived) in `xboxrecomp\src\apu\apu_core.c`: a frame thread paced by wall-clock `throttle()` (`:543-571`, QEMU realtime clock, 5.33 ms frames, catch-up with re-anchor after 250 ms) generates 48 kHz samples regardless of the device. The XAudio2 backend (`apu_xaudio2.c`) is push: `xa2_submit_samples` drops a buffer when 6 are queued (`:132-135`), primes 2 silent buffers when the queue empties (`:149-165`).
- So PD's guest audio clock is the host wall clock, and the device clock is separate. Any rate mismatch (typical 0.01-0.1 %) shows as either dropped buffers (queue full) or underruns (primed silence); `RECOMP_APU_DIAG` exists to count them.
- No device-loss handling: `xa2_init` runs once; no engine callback; if the default output changes or a headset is unplugged XAudio2 signals a critical error and the voice stops accepting buffers; `xa2_submit_samples` returns 0 and the APU thread keeps running on the clock, so the game keeps time but there is no sound until restart. HYPOTHESIS: this is what "sound gone after changing device" would be. No retry.
- No master volume setting in `settings.ini` (`pd_load_settings`, `main.c:817-845`), no limiter, no mixer pause while an overlay is open.

### 3.3 Verdict

PD already has the wall-clock fallback property by construction (that is its normal mode). What Oogie does better:
1. Device loss recovery (P2, effort S): register an XAudio2 engine callback (critical error) and/or detect repeated submit failures; destroy and recreate the engine/mastering/source voice every 2 s until it works. Do not touch the APU thread; just swap the sink under a lock. Edge: re-prime queue after reopen.
2. Drift (P3, effort M): make the APU frame throttle follow the device: when the XAudio2 queue depth hovers at 1 or at the cap for seconds, nudge the throttle period by a small ratio (<= +/-0.5 %), or resample one buffer per N. Oogie avoided the problem by pulling. Moving PD to a pull model means rewriting the throttle; not worth it unless the diag shows drops.
3. Output stage (P3, effort S): master volume setting + a simple peak limiter (instant attack, ~80 ms release, ceiling 0.95), applied at `xa2_submit_samples`. Optional.
4. Pause while overlays are up (only if PD gets an in-game menu).

---------------------------------------------------------------------------------------------------

## 4. Vblank, pacing (`d3d8\d3d8.cpp: vblank_thread, D3DDevice_Swap`, `renderer_d3d11.cpp: gfx_wait_vblank`)

### 4.1 Oogie (FACT)

- A dedicated high-priority guest "vblank thread" fires the guest vblank callback and bumps the vblank counter at 59.94 Hz. The guest-visible Swap (flip) waits until the vblank counter has advanced since the last flip (the game's presentation interval 1) with the guest lock released, so other guest threads run meanwhile.
- Source of the tick: if the display (compositor rate read from DWM timing info, re-read every 10 s = 600 calls) is 59-61 Hz, the thread waits on the monitor's vertical blank (one wait per vblank); if it is 119-121 Hz, two waits per vblank; else it uses a 59.94 Hz clock (`sleep_until` a running deadline).
- Safety rules: a display that is off/asleep answers the vblank wait immediately, which would run the game many times too fast. So a vblank arriving sooner than 3/4 of a frame after the previous is rejected and replaced by the clock. A display asleep at about 30 Hz would halve the game speed, so a vblank later than 1.5 frames hands pacing to the clock for 2 s, then tries the display again.
- Clock deadline repair: if the deadline is more than a frame in the past, reset it to now + period (no burst of catch-up vblanks).
- `timeBeginPeriod(1)` at start (`main.cpp`).
- Frame pacing is therefore display-locked for 60/120 Hz panels, and the game keeps its speed on any monitor.

### 4.2 PD (FACT)

- Guest vblank: `kernel_bridge.c:2571-2600 kernel_vblank_tick`, a wall-clock (QPC) tick, only enabled when `RECOMP_VBLANK` is set; 60 Hz default. The flip is paced in `hw_flip` (`nv2a_hw_d3d11.inc:3472-3504`) by sleeping to `last + 1/pace_hz`, only when the tearing swap chain is in use. Otherwise it is Present(1) vsync (`:3189`).
- No sync with the monitor in the tearing mode, and the 60 Hz clock vs a 59.94/60.00 panel drifts across tear/beat phases. Display-off/sleep protection is not needed because it never waits on the display in tearing mode, but there is no vsync-quality mode either.

### 4.3 Verdict / design

- P2, effort M, depends on item 1.4 step 4. Provide a "pacing source" switch: display vblank when the compositor rate is 59-61 Hz (or 119-121 with two waits), else clock; keep Oogie's two guards (too-early vblank rejected, too-late vblank = distrust for 2 s). This removes the beat between clock and panel that, with interpolation, makes judder; it also gives true vsync without tearing artifacts. Needs `IDXGIOutput::WaitForVBlank` (from `GetContainingOutput`) on the vblank thread, not on the render thread. Keep the current tearing path as a fallback for unusual refresh rates.
- Do not copy Oogie's one-output-forever shortcut; re-fetch the output when the window moves (Oogie notes it as a known shortcut).

---------------------------------------------------------------------------------------------------

## 5. Config and input

### 5.1 Config (`config.cpp`, `menu\menu.cpp: Toml`, `config_reload`)

FACT: a tiny TOML subset parser (sections, key = value, '#' comments outside quotes); one global config object loaded once; the menu edits the file line by line (keeps comments/ordering, appends missing keys, removes keys to reset) and calls `config_reload()`, which re-reads everything except the "startup-only" fields (resolution, aspect, stretch, fps) so volume, music, ray-tracing flag, key bindings, fullscreen apply at once. Resolution/aspect/fps changes set a flag and the menu relaunches the process with `--play` at Play time.

PD (`pd-game-v2\src\main.c:817-845`): `settings.ini` read once at startup and turned into environment variables (`RECOMP_*`), with the rule "an env var already set in a .bat wins". Settings are not live. Works, and is simple. Keys: fullscreen, window, render_scale, aspect, interpolation, vsync, keys.

Adopt (P3, effort S each):
- Live-reloadable subset: audio volume and key bindings; needs the runtime to read `getenv`-style values from a config struct rather than process env (PD reads env in many static initializers, e.g. `ip_init`), so reload is not trivial. Skip unless an in-game menu is built.
- Add `interpolation = auto|on|off` and `pace = display|clock` to `settings.ini` as part of 1.4 step 5/4.

### 5.2 Input (`input.cpp`)

FACT: a thread polls XInput every 2 ms (once a second while no pad is connected) and the guest read returns the latest snapshot under a small mutex. Reason in the code comment: `XInputGetState` can stall for tens of ms on an unplugged port (re-probe), and the game polls inside its frame. Keyboard fallback via `GetAsyncKeyState` only while the game window is foreground; keys remappable; pad buttons remappable (Black/White = RB/LB). Packet number increments on every poll.

PD (`xboxrecomp\src\input\xinput_device.c:205,218`): reads XInputGetState directly from the guest call. HYPOTHESIS: on machines without a controller this can cause 5-40 ms hitches every poll (documented Windows behavior for empty ports). Test with `RECOMP_PERF=1` max frame interval on a PC with no pad.
Adopt: poll thread + snapshot (P2, effort S).

### 5.3 Logging

Oogie sets stderr fully buffered (64 KB) and flushes every 2 s or on fatal, because unbuffered logging produced 40-60 ms stutter when a disk or scanner delayed a write (`main.cpp`). PD sets stderr/stdout unbuffered (`pd-game-v2\src\main.c:1025-1026`) and logs heavily (`[PERF]`, `[KERNEL]` lines). Adopt: buffered stderr + flush on a timer and in the crash filter (P2, effort S). Edge: the unhandled-exception filter must flush.

---------------------------------------------------------------------------------------------------

## 6. Menu and disc setup (`menu\menu.cpp`, `menu\disc.cpp`)

FACT, Oogie:
- The game's own art is read from the disc files and drawn by CPU on a 720-line canvas; the canvas is shown with a dedicated "show canvas" path (letterboxed, one per refresh).
- First start: a setup screen asks for a disc image (Redump .iso, xiso, or default.xbe of an extracted folder); the image's file table is parsed with bounds checks on every read, file names rejected if they contain path tricks or odd characters, copied into a staging directory and renamed to the final directory only after success; the default.xbe SHA-256 must match the one the port was lifted from. Failure messages are plain sentences.
- F1 in game: the same menu drawn over the last shown frame (blurred and dimmed), with the guest lock held so every guest thread waits, and audio paused. Settings apply at once except resolution/aspect/fps. Controls page captures a key or pad button; reset per list; mods on/off via a "disabled" marker file.

PD today: installer scripts (`INSTALL.bat`, `install.ps1`), `settings.ini` + `Settings.bat`, no in-game menu (`docs` mention F10 overlay "not done").

Adopt candidates:
- P3, effort M: hash check + staging directory + atomic rename in the installer's copy step, if PD's installer does not already do it (I did not read `install.ps1`; check).
- P3, effort L: in-game overlay with pause. Requires holding the guest lock (needs item 2 to be useful) and an audio pause (item 3.3). PD has a name-entry typing layer in `main.c`, so the keyboard plumbing exists. Not a priority.

---------------------------------------------------------------------------------------------------

## 7. Other small mechanisms in Oogie worth noting

- Shader cache keyed by a hash of the source text + target, stored on disk (`renderer_d3d11.cpp: compile`). PD already has an on-disk shader cache (`RECOMP_ANALYSIS.md` mentions it); no action.
- Window on its own thread so the message pump never stops (avoids "not responding" while the guest thread is busy). PD: `nv2a_hw_idle_present` and a window thread exist (comment at `:3735`). No action.
- Device-removed handling shows a plain message and exits (`check_present`); PD has the same (`:3190-3195`).
- Hysteresis for flat-frame detection (three consecutive frames) and "hold a clear-only flip" are the same idea PD already uses in `hw_flip` (`:3584-3600`).
- Governor "late frames pause interpolation" and ours are discussed in section 1.

---------------------------------------------------------------------------------------------------

## 8. Ranked summary

| # | Item | Priority | Effort | Evidence |
|---|------|----------|--------|----------|
| 1 | Confirm black cutscenes with `RECOMP_INTERP_DEFER=0`; then make async replay start from per-frame snapshots, not live surfaces (D1) | P1 | S to confirm, M to fix | hypothesis, strong by code reading |
| 2 | Least-moved matching among alike draws (D2) | P1 | S-M | FACT difference, impact hypothesis |
| 3 | Pre-composed two-picture present on a dedicated thread, no shared lock, vblank-locked or clock-locked (D3, D4) | P1 | M | FACT difference, impact hypothesis |
| 4 | Enabling rule: interpolate only when the panel can lock the cadence; otherwise 30 fps real frames (D6) | P2 | S | FACT in Oogie |
| 5 | Repeat the real frame on cut/fail so cadence is uniform (D5) | P2 | S | FACT |
| 6 | Display-vblank pacing with the two guard rules (section 4) | P2 | M | FACT |
| 7 | Poll XInput on its own thread (5.2) | P2 | S | FACT in Oogie, hypothesis for PD |
| 8 | Buffer stderr, flush on timer/crash (5.3) | P2 | S | FACT |
| 9 | XAudio2 device-loss recovery (3.3) | P2 | S | FACT in Oogie, hypothesis for PD |
| 10 | Cut statistic over non-stencil matched draws, median (D7); widen per-draw lerp limits at 30 Hz (D8) | P3 | S-M | tuning |
| 11 | Master volume + limiter, audio drift control (3.3) | P3 | S / M | optional |
| 12 | Priority-aware guest scheduler with safepoints (2.3) | P3 | L | only if races return |
| 13 | In-game menu, atomic disc copy (6) | P3 | M-L | optional |

## 9. Not verified / limits

- Nothing was run. All "PD is broken because" statements are labelled hypotheses; items 1-3 each have a one-switch or one-log test.
- Oogie is a 60 Hz game at 120 pictures/s; PD is 30 Hz at 60. Per-step motion is twice as large, so thresholds copied by number from Oogie (0.2, 16, 18.5 ms) must be recalibrated on PD, not reused.
- I did not read PD's `install.ps1`, the rest of `pd-game-v2\src\main.c`, or the XAudio2 behaviour on device change on a real machine.
