# Pacing / vblank / flips / present timing / high refresh: comparison for Phantom Dust

Read-only comparison. Nothing under `R` was run or built. Paths: `R` = `...\scratchpad\research`.
Short names: TD = `R\takedawn-xboxrecomp` (toolkit) and `R\takedawn` (game); UP = `R\upstream-xboxrecomp`;
MC = `R\mcdub`; DJ = `R\defjam`; PD = `C:\recomp\phantom-dust-re`.
Licences: TD, UP, MC, DJ and their toolkits are all MIT. Oogie is GPL-3, so design only. MIT code can be copied with the
copyright notice kept. For MIT forks the notice names sp00nz (toolkit), plus the game author for game-layer files.

## 0. What PD does today (evidence, so the rest can be checked)

| Piece | Where (PD) | What it does |
|---|---|---|
| Emulated vblank | `xboxrecomp/src/kernel/kernel_bridge.c:2573` `kernel_vblank_tick` | Runs on the timer thread, which does `Sleep(1)` in a loop (`:2916` `timeBeginPeriod(1)`, `:2924`). A QPC schedule in double ms with `next_d += per`. A restart if more than 50 ms behind. The ISR is raised on the first 1 ms tick after the due time, so it is 0 to 2 ms late. `kernel_drain_dpcs()` runs right after it on the same thread. The D3D vblank DPC and the game's vblank callback `sub_000B08E0` run there. |
| Vblank "hold" | `kernel_bridge.c:2553-2570`, `xbox_memory_layout.c:905-925` | The PCRTC bit stays up for 1.5 ms so the ISR/DPC can see it. The NV2A ack thread `nv2a_ack_thread` spins forever with `Sleep(0)` (`:1114`). A high-priority flush thread (`:1124`) clears PMC and PCRTC when the hold is over. |
| FLIP_STALL | `nv2a_pb_exec.c:3412-3425` | "There is no scanout here to wait for, so that is now." The flip completes as soon as the executor reaches it. There is no vblank hold and the swap NOP's interval is ignored. |
| Present | `nv2a_hw_d3d11.inc:3187-3189`, `hw_flip` `:3467`, pacer `:3472-3505`, pace set up `:781-784` | If DXGI tearing is supported (almost always on Win10+): `Present(0, ALLOW_TEARING)`, and `pace_hz = 60` (`RECOMP_HW_PACE`). The pacer only gives a minimum of 16.67 ms between flips, and the next period is measured from the actual release time. At the game's 30 Hz it never acts. The `vsync=1` setting (`RECOMP_HW_VSYNC`) is ignored when tearing is on. Only the fallback blt swap chain uses `Present(1)`. Present runs inside `s_hw.lock`, `hw_flip` `:3707-3722` and idle present `:3753`. |
| Interpolation (`RECOMP_INTERP`) | `nv2a_hw_interp.inc`, `ip_flip` `:972`, `ip_presenter` `:1103`, `ip_schedule_real` `:1143` | Replays frame N with draws moved halfway back to N-1. That picture is presented at once, and the real frame N follows from a presenter thread: `deadline = now + QPC/60 hz`, `Sleep` then `YieldProcessor`. The deferred worker `ip_worker` presents when the replay job ends, not at the flip. A governor turns it off if the flip interval grows by more than 6 ms. |
| Game limiter | `pd-game-v2/.../recomp_0011.c` `sub_000B0960` | `target = [esi+0x54] + 2`; wait on the event `[0x39E710]` until the vblank count `[0x39E700] >= target`. Late by 1 or more vblanks: reset the count to 0 (resync). The vblank callback `sub_000B08E0` sets the event on every second vblank (odd count). |
| `RECOMP_FPS60` | `main.c:127-135`, `recomp_manual.c:92-106` | Lowers the target to +1 and sets the event every vblank. `docs/RECOMP_ANALYSIS.md:589` already records "game speed doubles ... RECOMP_FPS60 must stay off". It is a dead end, not an interpolation. |
| Subtitle pace | `recomp_manual.c:490-515` | Runs `sub_001A9F80` on a QPC*60/2 budget while bit 0x40 of `[0x39E7C0]` is set (movies). |
| Power throttling | `main.c:991-999` | Opts out of EcoQoS and out of "ignore timer resolution" (`StateMask = 0`). Good. |

Consequence: PD has no clock that ties the picture to a display refresh or a fixed edge. A picture goes to the
swap chain when the executor happens to finish drawing it. That is item 1 below, the root of the judder.

---

## 1. The flip hold: hold FLIP_STALL to a vblank edge  **[P1]**

- **Source.** TD `src/kernel/nv2a_pb_exec.c:5971-6326` (`flip_pacing_on`, `flip_pacing_swap` `:6156`,
  `flip_pacing_stall` `:6250`). The edge arithmetic is in `kernel_pacing.c:961` `xbox_VblankWaitUntil`; `:877`
  `xbox_VblankEdgeNs`; `:864` `xbox_VblankNextDue`. Related: UP `src/kernel/nv2a_flip_hold.{h,c}` plus
  `nv2a_pb_exec.c:3657-3740` and `xbox_memory_layout.c:1425-1450`. MC `xboxrecomp/.../nv2a_pb_exec.c:4055-4120`
  (`flip_wait_vblank`, `nv2a_pb_flip_retired`) and `R\mcdub\src\recomp_manual.c:985-1006` (`sub_003E4EF0` hook).
  DJ `tools/xboxrecomp/src/kernel/nv2a_pb_exec.c:554-594` `flip_pace`.
- **Ours.** `nv2a_pb_exec.c:3412` (the flip is immediate) and `nv2a_hw_d3d11.inc:3467` `hw_flip`.
- **Mechanism.** The XDK's Swap pushes a NOP whose parameter is `(address | interval) << 5 | 1`, then FLIP_INCREMENT_WRITE
  and FLIP_STALL. On the console the NOP raises a GPU interrupt that queues the flip for a vblank: `target = last target +
  interval`, and when that has passed, "now" if the IMMEDIATE flag is set, else the next vblank. FLIP_STALL stalls the
  GPU until then. TD reproduces it: `flip_pacing_swap` decodes the NOP and `flip_pacing_stall` waits on a condition
  variable that the timer thread signals at every vblank (`xbox_VblankSignal`, `kernel_bridge.c:3041`). In
  sleep mode it sleeps to a spin lead before the edge and spins the last stretch. It gives up after three 100 ms timeouts
  and turns itself back on when vblanks resume. UP has the same rule as a pure state machine (`target = last_flip +
  interval`, release when `(int32)(count - target) >= 0`, 100 ms cap, a "clock is moving" check). MC's simplest form
  (`RECOMP_FLIP_VBLANK`) is "at most one flip per vblank", a `Sleep(1)` poll for 50 ms.
- **Problem solved.** Flip time then no longer depends on how long the executor took to draw the frame, and every
  picture reaches the swap chain on a fixed edge, as it would at a console vblank.
- **Does PD have the problem? Yes (static evidence).** PD's game limiter fixes when the game *starts* a frame
  (after vblank k), but the picture goes out when the executor finishes the frame: `T_vbl(k) + exec_time(N)`. Execution
  time varies with draw count, shader compiles, texture uploads and the interpolation replay. The spacing of
  presented real frames is `33.33 ms + (exec(N+1) - exec(N))`, so judder follows scene cost. On a 60 or 144 Hz display
  that variation lands on or across refresh boundaries as the "1,3,2,2 refresh" pattern TD's `present-timing.md` describes.
  `RECOMP_HW_PACE` cannot fix this: it is a minimum interval, and measured from the actual release time. I did not run
  PD, so confirm with the `[PERF-PRES]` line (std of the present interval) before and after.
- **Port design for PD.**
  1. Step 0 (an hour): trace the NOP parameter (`NO_OPERATION` software method 1) for a few minutes of play, to learn the
     interval PD's D3D asks for (1, 2 or IMMEDIATE) and whether the NOP reaches `nv2a_pb_exec_method` at all (PD's D3D is
     XDK-era, with the vblank ISR at `0x24CC20`, DPC `0x24CA70`; the make-space event already goes through software method 5).
     TD's `RECOMP_TRACE=pacing` "swap interval a -> b" line is the model.
  2. Add, in `nv2a_pb_exec.c`, TD's `flip_pacing_swap`/`flip_pacing_stall` minus the host-grid branch (about 90 lines),
     or UP's `nv2a_flip_hold.{h,c}` (pure, testable, about 130 lines) driven from the existing ack-thread loop. TD's
     blocking wait is simpler to port and gives an exact edge. UP's variant makes the ack loop skip the pushbuffer walk
     while held, which is a larger change to PD's already heavily edited `xbox_memory_layout.c`.
  3. Add `xbox_VblankWaitUntil(count, timeout)` and `xbox_VblankSignal()`: a `CONDITION_VARIABLE` that
     `kernel_vblank_tick` signals after `g_vb_isr++`. That also removes polling in `hw_flip`'s movie-vblsync loop (`:3486`).
  4. Keep the cap: if no vblank arrives in 100 ms three times, switch the hold off, and back on after six vblanks. This
     matters while `RECOMP_VBLANK` is unset or the timer thread is blocked in a long DPC (see item 5).
  5. Swap interval: if PD asks for interval 1 with its own 2-vblank limiter, the hold releases every vblank after the
     frame is ready, so the picture goes out at the first vblank after the executor finishes: delay `<= 1` vblank, and
     spacing exactly 33.33 ms while exec < 16.67 ms. If it asks for 2, the hold is the second limiter and the game's
     resync branch will rarely fire. Either way the movie loop (bit 0x40, no limiter) is paced by the hold, which replaces
     `RECOMP_HW_MOVIE_HZ`/`getenv_vbsync` (`nv2a_hw_d3d11.inc:3256`, `:3482-3495`).
  6. Gate with `RECOMP_FLIP_PACING=vblank` first (default off for one release), then default on.
- **Risk.** Burnout and others needed a gate to avoid deadlocks: the hold must not stall the executor while the guest
  waits on an executor-side fence it needs in order to raise the vblank DPC. TD's executor holds only *after* the frame
  is complete, and the 100 ms cap covers the rest. MC's note: hooking D3D's own retirement (`sub_003E4EF0`) deadlocked
  to 4 fps because D3D retires only flips the GPU already passed. So do not copy MC's retirement hook; copy TD's.
- **Effort** M (2 to 3 days with the interval trace and tests). **Licence** MIT. **Priority P1.**

## 2. Present on the display's edge (flip grid) and lock the emulated vblank to the display  **[P1 after item 1; P2 for the vblank lock]**

- **Source.** TD `src/kernel/flip_grid.h:29` `fg_next_edge`; `kernel_pacing.c:47-472` (display lock:
  `xbox_VblankDisplayRefresh` `:181`, `display_grid` `:403`, `xbox_VblankSchedule` `:427`); `:1346` `xbox_DisplayGrid`; `:1336`
  `xbox_DisplayRateSet`; `src/video/dxgi_vblank_model.h`; `src/d3d/nv2a_pb_d3d11.c:445-476` `vblank_link_thread`
  (`IDXGIOutput::WaitForVBlank`); docs `docs/runtime/present-timing.md` and `takedawn/docs/present-timing.md`.
  Oogie (GPL, design only) `runtime/d3d8/d3d8.cpp:241-310`: a 60 Hz vblank thread that uses `WaitForVBlank` only on a
  59-61 Hz or 119-121 Hz display and distrusts it for 2 s after any step over 1.5 frames, otherwise falls back to a
  clock.
- **Ours.** `kernel_bridge.c:2573` (vblank), `nv2a_hw_d3d11.inc:3187` (Present), `main.c`/`settings.ini` `vsync=1` (inert with tearing).
- **Mechanism.** The 60.000 Hz timer drifts against the display's 59.94/60.0x Hz. The lock moves each vblank edge
  onto the display refresh nearest one guest period on (period = display period, or an exact multiple of it within 1 % of
  60 Hz). Reports come from a thread blocked in `WaitForVBlank` (Windows), filtered 1/32 per return, with the stated mode
  period from `QueryDisplayConfig`. Detection of variable refresh: median step over 1.5 stated periods held for 1 s, and
  then the lock stands down. `fg_next_edge` does the same for an arbitrary refresh (144 Hz): each edge is the refresh
  nearest `prev + period`, and a late swap restarts the grid from the last refresh before now.
- **Problem solved.** On a fixed-rate display, a present that lands a little early or late is shown a refresh early or
  late, so frames last 1,3,2,2 refreshes; with a free 60.000 Hz timer the phase against the panel drifts and the
  uneven repeats come and go in cycles of seconds. TD measured it: legacy macOS lock 1:874 2:1 versus unlocked 35 to
  60 ms vblank-to-glass. Also, a presenter that gets exactly one new frame per refresh queues nothing.
- **Does PD have the problem? Yes.** PD's vblank is a free-running QPC grid on a 1 ms-quantised thread; presents are
  `Present(0, TEARING)` at executor-completion time; `vsync=1` is ignored. On a 60 Hz panel (59.94 or 60.01) the content phase
  walks through the refresh, so there is a periodic dropped or doubled frame, and at 120/144/165 Hz there is a fixed
  1-3 repeat pattern for 30 Hz content that interpolation turns into uneven 2-3 refresh spacing.
- **Port design for PD (staged).**
  - Stage A (S, one day, **P1 if item 1 is done**): make the present follow the held edge. After item 1 releases at the
    vblank edge, call `Present(1, 0)` in a non-tearing flip-model chain, or `Present(0, ALLOW_TEARING)` in a window,
    with the swap chain's `SetMaximumFrameLatency(1)` and the latency waitable object (TD `nv2a_pb_d3d11.c:547-581`,
    `:3522` bounded 100 ms wait). Make `vsync=1` work (today it silently does nothing when tearing is on).
  - Stage B (M, P2): the display lock. Port `xbox_VblankDisplayRefresh` and `display_grid` (about 300 lines but self-contained,
    with `tests/vblank_schedule`), `dxgi_vblank_model.h`, and a 60-line `WaitForVBlank` thread. PD needs only the "fixed,
    a multiple of 60 Hz" case (60/120/240); everything else falls back to the free timer, which is today's behaviour. Report
    the lock state in the `[NV2A] vblank` log line (`kernel_bridge.c`: no 600-vblank line exists yet in PD; add one).
  - Stage C (P3): `fg_next_edge` for non-multiples (75/100/144/165 Hz). Only worth it together with item 3.
- **Effort** A: S. B: M. C: S. **Licence** MIT. **Priority** A P1 (after 1), B P2, C P3.

## 3. High refresh (more pictures than game steps): TD's design and how it maps to PD  **[P2 design; P3 build]**

### 3a. How TD's high-refresh works, step by step

1. The game layer sets a wanted rate each `Update` (`hirefresh.c:613` `bo3_update`, wrap of `CB3Game::Update` `sub_000165F0`,
   `recomp_manual.c:144`): `wanted_rate()` (`:227`) is the setting on, `MEM32(GAME_STATE) == 5`, no half-rate flags,
   one player, a stage object, and a display rate over 61 Hz. It calls `xbox_FlipHostRate(rate)` (`kernel_pacing.c:1385`) to ask
   the executor for a host grid at the display's rate instead of the swap's vblank.
2. The executor (`flip_pacing_swap`) turns each swap into a grid edge with `fg_next_edge`, using `xbox_DisplayGrid` for the real
   refresh (period + phase from the DXGI/Vulkan/Metal reports, else the stated period, else SDL's whole Hz), and publishes
   `{swaps seen, edge, period}` through `xbox_FlipHostGridNote` (a sequence lock, `:1417`). `flip_pacing_stall` holds with
   a timed sleep and a spin only for grids over 200 Hz (and `hr_spin` for the idle guard, item 7). The emulated vblank stays
   at 60 Hz for everything else.
3. The game layer computes the **picture time** (`picture_ns`, `hirefresh.c:465`): the host time at which the frame being
   built will flip. That is the last swap's edge plus `n` periods, where `n` is the number of swaps the game has issued
   (counted by wrapping D3D's swap `sub_00350DB0`, `bo3_swap`) beyond those the executor has seen, limited to 1..4. A
   learner (`learn`, `:386`) then corrects it from when frames were actually shown (`xbox_FlipHostShownAt`, a ring of the last
   64 frames filled by `xbox_FlipHostHanded/Shown`, `kernel_pacing.c:1473-1532`), using medians over 9 and 16 samples,
   so a slow machine still shows each picture at the time it was drawn for.
4. `s_ahead += dt` (the picture's time since the last picture). `steps = ceil(s_ahead/STEP)`, capped at 4, stored in
   the game's step-count word (0x4D53AC), so a picture may run **0 steps**. The title's stepper (`sub_001B58E0`) still runs
   but its count is ignored. `s_lag` is where the picture lies between the last two steps. Menu mode 2 (timer) keeps lag beyond the cap.
5. After the steps, every object that moved is **blended in place** between its last two states at `s_lag`. For RenderWare the
   hook is `_rwFrameSyncDirty` (`sub_001E15E0`, `bo3_frame_sync_dirty`) and `RwFrameGetLTM` (`sub_001DD280`): the LTM of each
   moved frame is recorded once per step, and lerped (position) plus Gram-Schmidt (axes). Vehicles are blended at the draw
   wrap (`sub_000303D0`). The view matrix is rebuilt from the blended camera (`blend_view`: `V' = B^-1 M V`). The blends are put
   back at the start of the next `Update`, so the simulation never reads one. Nothing is blended across a jump above 8 units or
   a turn above about 45 degrees (cuts, resets).
6. Things the game advanced once a *picture* instead of once a *step* are put back to once a step: RNG, play time, HUD,
   race audio manager, effects clock, the player cameras, and the sparks' history clock (about 12 wraps in total).
7. Latency: one 60 Hz step at most (blend of the last two steps at the picture's own flip time), nothing queued. Measured: 144 of
   144 pictures per second within 0.5 ms of their edge (macOS), 174 to 176 flips per second at 175 Hz with 59.8 to 60.1 steps per second.

### 3b. Mapping to PD's frame loop (`sub_000B13D0`, task loop `sub_000B42A0`)

PD's simulation is a fixed step per *frame* (2 vblanks, 30 Hz), counted in vblanks by `sub_000B0960`. There is no stepper with a
lag accumulator to take over. Instead, the frame boundary is the limiter wait. Mapping:

| TD concept | PD equivalent |
|---|---|
| `CB3Game::Update` wrap, `steps = N` | `sub_000B13D0` (frame loop) body, once per 33.3 ms. PD needs no "0 steps / n steps": the sim already runs exactly one step per 2 vblanks. Do not touch the limiter (`RECOMP_FPS60` shows what that does: 2x speed). |
| Picture time / grid edge | The same, host side: the flip hold edge of item 1/2, plus extra pictures at the other refreshes inside the 33.3 ms. |
| Blend of two states at `s_lag` | Today `RECOMP_INTERP` (host command-list replay at `t = 0.5`). To go beyond 60, make `t` a function of the picture time: `t = (picture_ns - edge_of_N-1) / 33.33 ms`, with several replays per frame (about 4.8 at 144 Hz). Replay cost is `s_ip.st_ms` per picture; the governor already exists. |
| Per-object blends in guest memory | **Not transferable as is.** TD's version is exact because RenderWare keeps LTMs in guest memory and the game's lists are known. PD's engine objects are not mapped. The generic equivalent is what `nv2a_hw_interp.inc` already does on the draw list. The way to a guest-side version is to find PD's camera/actor matrix writers (e.g. around the frame loop `sub_000B13D0`/`sub_000B42A0`) and blend those, as MC/TD do; this is a research item, not a port. |
| Cuts / jumps (8 units, 45 degrees) | PD's equivalent: `RECOMP_INTERP_CUT/HIREL/HIFRAC` thresholds exist; add TD's rule "a matrix that moves more than X is not blended" for the camera constants (see item 4). |
| Per-picture counters put back | PD has the opposite problem: things that run once per *loop pass* instead of once per frame during movies (subtitles). `sub_001A9F80` pace (`recomp_manual.c:490`) is PD's `bo3_race_audio`-style wrap. |

Recommended PD design for a 120/144 Hz target, in order of value:
1. Items 1 and 2 (so that real frames and the 0.5 picture are on a grid).
2. Make the interpolation picture and the real picture land on *display-derived* edges: interp at the held edge, real at
   `edge + 16.67 ms` taken from the grid (not `now + 16.67`), see item 4.
3. Only then, add variable `t` and extra pictures for >60 Hz. Gate by the display rate (above 61 Hz, as `wanted_rate`).

**Effort** 3a is documentation only; 3b step 3 is L (1 to 2 weeks, plus tuning of the matcher at other `t`). **Licence** MIT (`hirefresh.c` is from the Burnout game
layer, MIT, copyright of its author; reuse as a design, not code, since it is title-specific). **Priority** P2 for design, P3 for build.

## 4. Interpolation presentation timing: the second picture and the "extra" picture  **[P1: likely cause of judder and part of the shake]**

- **Ours.** `nv2a_hw_interp.inc:1103-1129` `ip_presenter`, `:1143-1156` `ip_schedule_real`, `:1158-1215` `ip_worker`, `:972` `ip_flip`;
  `nv2a_hw_d3d11.inc:3463` `hw_flip`.
- **Theirs (the comparable idea).** TD `hirefresh.c:309-513` (picture time learned from "shown" times) and `kernel_pacing.c:1473-1532`
  (`xbox_FlipHostHanded/Shown`), `present_cadence.c`/`present-timing.md` ("uneven" is counted on the glass, not on the flip
  interval). Also the lesson in `high-refresh.md` lines 55-64: the first version used the wall time between `Update`s, and
  because `Update` runs ahead of its flip by 3.7 to 10.6 ms at 144 Hz, the car moved 0.85 to 1.15 times its average per picture. Fixing it
  with the flip grid edge brought the ratio to 0.97 to 1.00.
- **Mechanism in PD (read from the code).** The interpolated picture is presented when the *replay job finishes* (`ip_worker`:
  `hw_present_locked(1)` at the end of the job, `s_ip.job_t0` to done = "flip->done" in the `[IPJ]` line), and the real picture
  is scheduled for `present_time + 16.67 ms` by `Sleep`/`YieldProcessor`. So the interval between the two pictures
  is 16.67 ms plus the thread's wake-up jitter, and the interval to the *next* pair is `33.33 - 16.67` plus flip-time variation (item 1)
  plus job time variation. If the next flip arrives before the pending real picture is out, `ip_flush_pending` (`:1161`) presents the real one
  at once, right before the next interpolated one: two presents within a millisecond, then a gap.
- **Problem and evidence.** README/docs say the 60 FPS mode causes "black cutscenes and camera shake on some fast PCs" and is off by default
  (`README.md:16,90`). *Fast* PCs are where the replay finishes inside the flip and the pair collapse (above) happens most
  often, while a slow PC runs the governor off. Judder follows from the unsteady spacing (static analysis; not measured). Camera shake is
  consistent with a camera whose constants alternate between a blend at `t = 0.5` and the real value at uneven times; the matcher
  (`RECOMP_INTERP_PROG` = 1.0, wide) also blends program constants for any draw whose relative change is under 100 %, which includes
  camera-shake offsets that change sign every frame. Black cutscenes: the movie path is excluded (`movie_seen`), but the exclusion happens at the *next* flip
  (`ip_flip:1056-1063` resets the frames and returns 0), and `ip_flush_pending`/`ip_presenter` can still present a stale `out_srv`
  after a scene change; the interpolated surface is never invalidated by a size or format change. This is a hypothesis to check with
  `RECOMP_INTERP_DBG=2` and `interp_dump.flag` at the first black frame.
- **Port design.**
  1. Compute both presents' times from one reference: the flip edge `E` (item 1). Present the interpolated picture at `E`, the real
     one at `E + P` where `P` is 16.67 ms rounded to the display grid (item 2). Do it with an absolute deadline, not `now + P`.
  2. Run the replay *before* `E`: start the job when the executor has the frame (TD's `s_host_ready_ns`, the stall start), because the hold
     leaves time (typically 5 to 12 ms of a 16.67 ms vblank). At the hold, the picture then needs no job wait.
  3. If the replay is late at `E`, present the real picture (not the interpolated one) at `E` and skip the interp for this frame
     (never present out of order). Count the skip.
  4. Take `s_hw.lock` off the present path (see item 6), or the ip presenter thread and `hw_flip` queue on each other.
  5. Check `ip_flush_pending` ordering: never present real N after interp N+1.
- **Effort** M. **Licence** MIT (design from TD). **Priority P1** (affects the feature the README calls broken).

## 5. Vblank thread: precision, the DPC guard, high-resolution timer, spin lead  **[P2]**

- **Source.** TD `kernel_bridge.c:3646-3765` (`timer_spin_ns`, `kernel_timer_loop_ns`: sleeps to the earlier of the next vblank
  edge, next kernel timer and a 10 ms cap on a nanosecond clock, then spins the last 600 us on Linux), `:3235-3300`
  `dpc_vblank_guard_ns`/`kernel_drain_dpcs_before` (no DPC is started within 1.2 ms before an edge), `platform/host_time.c:55-89`
  `xbox_HostSleepNs` (a `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer, fallback `Sleep`), `host_time.c:93-150` wakeable sleep (a DPC kick
  ends the sleep early), `kernel_pacing.c:40-45` and `:427` ns schedule `t0 + N*1e9/60` with a 100 ms restart. UP
  `kernel_bridge.c:2811-3040` has the QPC ms schedule and a dedicated vblank-ack thread.
- **Ours.** `kernel_bridge.c:2573-2625` and `:2916-2936` (`Sleep(1)` timer loop, double ms), `xbox_memory_layout.c:905-1120`.
- **Problem solved.** The vblank is raised after a sleep that ends 0 to 2 ms late, and the title counts vblanks from the top of
  each frame, so that error goes straight into the flip interval. TD saw 26/41 ms around 33.3 on Metal and a "yellow bar" of 17.7 then 15.7 ms from
  a 1 ms audio DPC that ran just before an edge. With the ns loop and the guard: 0 of 600 raises more than 0.5 ms late on the Deck.
- **Does PD have it?** Yes for the quantisation: `Sleep(1)` with `timeBeginPeriod(1)` gives 1.0 to 2.0 ms wakes, and the vblank is
  raised in the first loop pass after the due time. In PD, however, `kernel_drain_dpcs` runs the D3D vblank DPC and the game's callback on the same thread
  immediately after the raise, and the file's own comments record DPC holds of 25 to 130 ms, up to 1.8 s (`xbox_memory_layout.c:878-890`,
  `:1119-1130`: DPCLONG, OHCIDEAD). A long DPC stalls the vblank clock itself. `next_d < nowd - 50` then drops vblanks, so the
  game's counter runs slow (the same effect `recomp_manual.c:498-505` found: "the title's vblank counter ran slightly slow", which is why the
  subtitle pace uses QPC instead). PD's mitigations (flush thread, `xbox_Nv2aAckPoll`, `RECOMP_VBLANK_HOLD_MS`) work around it.
- **Port design.**
  1. S (hours): replace `Sleep((DWORD)slp)` in the timer loop with `xbox_HostSleepNs`-style high-resolution waitable timer sleep to
     `min(next vblank, next KeSetTimer due, +10 ms)`, with the 1 ms `Sleep` fallback. This also fixes the "throttled window" case in
     part: a high-resolution timer does not depend on the global timer resolution that Windows ignores for occluded windows
     (PD already opts out of that via `main.c:997`, but only for Win11 builds that honour the flag).
  2. S: DPC guard. Do not start a queued DPC within about 1.2 ms before the next edge. PD's long DPCs make this partial, but it removes the audio-DPC case.
  3. M: move the raise and the DPC apart: the timer thread raises, signals, and only then runs DPCs, ideally on a second thread,
     so a 100 ms DPC cannot stop the next raise. TD does this through the "holder" model (`xbox_IrqPost`, one-CPU gate), which is
     large; PD's gate is different, so this is a P3 refactor. A cheaper step is the vblank-ack event thread from UP
     (`kernel_vblank_ack_thread`, `xbox_VblankAckWait`, `kernel_bridge.c:2829-2990`), which replaces PD's 1.5 ms hold and the
     spinning `nv2a_ack_thread`.
  4. Optional S: `timer_spin_ns` (spin the last 600 us). Off by default on Windows in TD (unmeasured there); skip.
- **Effort** 1: S, 2: S, 3: M to L. **Licence** MIT. **Priority** P2 (1 and 2 are cheap and safe; do with item 1).

## 6. Present blocks on a throttled/covered window, FPS drops  **[P1]**

- **Source.**
  - TD `src/d3d/nv2a_pb_d3d11.c:77-90, 547-581, 3520-3525`: frame-latency waitable object (`SetMaximumFrameLatency`, default 1),
    waited with a **100 ms timeout** before `Present`, so a blocked swap chain can never hold the executor for long.
  - TD `kernel_pacing.c:773` `xbox_PacingSetVisible`, `pacing_watch.h`, `pacing_diag.h`: windows in which the title was paused, the window
    hidden or minimised, the machine slept or the process stopped are void for the pacing watchdog. A wake gap resets the lock.
    `fb_present_sdl.c:2326` feeds the visibility.
  - DJ `tools/xboxrecomp/src/kernel/nv2a_pb_exec.c:540-594` `flip_pace` + `xbox_Nv2aSetFlipHz`, and `src/hooks/d3d11_translator.c:602`:
    "A host whose presents are paced by the display makes the display the frame clock: this timer then only has to stop the title running away when
    the display is not pacing (a minimised or covered window presents at once)."
  - Oogie (design only) `d3d8.cpp:253-262`: distrust the display's pacing for 2 s after a step over 1.5 frames, fall back to the clock.
- **Ours.** `nv2a_hw_d3d11.inc:3185-3205` Present in `hw_present_locked`; the lock is held by `hw_flip` `:3707-3722`, `nv2a_hw_idle_present` `:3742-3753`,
  `ip_presenter` `:1119-1126` and `ip_flush_pending`. `main.c:991-999` already opts out of EcoQoS.
- **Mechanism of the problem.** A flip-model swap chain with the default frame latency of 3 can hold a `Present` for a refresh or more once the window is
  occluded, minimised, or throttled by DWM. With `DXGI_SWAP_EFFECT_DISCARD` (fallback when tearing is unavailable), `Present(1)` blocks until the vblank. In either
  case PD calls Present while holding `s_hw.lock`, which the executor (`hw_flip`), the interpolation presenter and the window thread's idle present all need.
  One blocked Present therefore stalls the executor, then the guest's fence wait, so FPS drops for a reason that has nothing to do with the game.
- **Port design.**
  1. S: create the swap chain with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (+ `ALLOW_TEARING` when supported), call
     `SetMaximumFrameLatency(1)` (TD default), and `WaitForSingleObjectEx(waitable, 100, TRUE)` before `Present`. On timeout, count it
     and carry on (never wait unbounded).
  2. S: treat `DXGI_STATUS_OCCLUDED` and a `Present` slower than about 50 ms as "display is not pacing": use the QPC pacer (DJ's
     `flip_pace`) at the game rate until a normal present returns again. A flag for `hw_flip`.
  3. S: do the `Present` call outside `s_hw.lock` (copy the frame into the back buffer under the lock, release it, then present). TD's executor
     is single-threaded in this part, so it has no equivalent; in PD the lock is shared with a window thread and the interpolation thread.
  4. S: pass window visibility from the window thread (`SW_` state / `WM_SIZE minimized` / `IsIconic`) to the pacing code and exclude those intervals from any
     governor decision (`ip` governor `s_ip.gov`: it currently counts a throttled window's slow flips as "interpolation is too heavy" and switches it off for 4 to 30 s).
  5. Keep PD's EcoQoS opt-out; also set `ProcessPowerThrottling` for the timer and ack threads' `ThreadPowerThrottling` (already done for the ip worker in the reverse direction, `nv2a_hw_interp.inc:1186`).
- **Does PD have it?** The user reports it. Static evidence is the three facts above: Present under the global lock, no latency bound, and the governor
  keyed on flip intervals. I did not reproduce.
- **Effort** S to M. **Licence** MIT. **Priority P1.**

## 7. Executor idle spin on the grid (`hr_spin`) and PD's permanently spinning ack thread  **[P2]**

- **Source.** TD `nv2a_pb_exec.c:6108-6153` (`hr_spin_asked`, `hr_spin_guard`, `nv2a_pb_idle_spin`), `:6269-6285`; `high-refresh.md` "The executor's idle time".
  The governor reads a thread that sleeps a third of each frame as half idle, so the Deck lost about 13 % clock at 90 Hz against 60 Hz mode (76 versus 89 to 90
  pictures/s). Guard: spin only when the hold takes under 30 % of the period (off again above 40 %).
- **Ours.** `xbox_memory_layout.c:905-1120`: the ack thread never sleeps (`Sleep(0)` yields only), so one core is at 100 % all the time; the flush
  thread sleeps `Sleep(1)` outside its hot windows.
- **Problem and evidence.** On a laptop or a thermally limited machine, a core burning in a `Sleep(0)` loop pulls the package clock down and shares the
  power budget with the executor and game threads; UP's comment (`xbox_memory_layout.c`, vblank ack) records that moving to a blocking event brought 35 to 43 %
  of a core under Proton down to 6.5 %. This is plausible for the "throttled" complaint, but unmeasured in PD.
- **Port design.** (a) Put the ack loop on an event: it needs to run on a kick (PUT change), a vblank and when a fence is polled; use UP's vblank-ack thread
  (`kernel_bridge.c:2863-2990`) and a `WaitForSingleObject(kick_event, 1)` loop instead of `Sleep(0)`. (b) Keep spinning *inside* the hold for the last
  0.5 ms only (TD's `spin`), and use `hr_spin_guard` only if the flip hold is at a grid faster than 60 Hz. Test CPU% with `RECOMP_TT_STATS`.
- **Effort** M (the ack thread is delicate: see its comments about lost-command races). **Licence** MIT. **Priority P2.**

## 8. Pacing while no new picture is presented (loads, movies, holds)  **[P1, applies as-is]**

- **Source.** DJ `docs/known-issues.md:25-40` ("Scene loads freeze the picture"), `docs/research/scene-perf-macos-2026-10-18.md` F2/F5; DJ
  `nv2a_pb_exec.c:4348` (`flip_pace()` called on every FLIP_STALL). MC `flip_wait_vblank` (`nv2a_pb_exec.c:4082`).
  TD `hirefresh.c:232-249` (`wanted_rate` returns 0 outside the running state: "loads, the stage's set-up and hand-overs step once a frame
  by their own code and keep flips at 60").
- **Mechanism.** DJ's executor slept in vsync pacing (18 % of a load) even while the load was not presenting a changing picture, and the game thread spun in the
  fence hold (`SwitchToThread`) waiting for it (38 % of samples). Fixes listed by DJ (not applied there): (1) do not pace while no new picture is presented;
  (2) replace the `SwitchToThread` loop with an event the executor signals on a fence; (3) read `PCRTC_START` only when the NV2A trace is on.
- **PD relevance.** PD's `hw_flip` has the same structure: its hold applies to every flip (`:3472`), and the idle present is separate (`:3742`). Item 1's hold must
  therefore be skipped when the frame is a held/clear-only flip (PD already knows: `ip_flip` `!disp || f->ndraw == 0`, `s_hw.biggest == NULL`) and during
  loads. Also check the PD guest fence-wait loop for a `SwitchToThread`/`Sleep(0)` spin equivalent: PD's make-space wait (`sub_00246261`, event at
  `0x2532A8 + 0x1A4`, `main.c:959`) already uses an event; the fence wait itself (`sub_00246590`?) is unchecked.
- **Port design.** (1) In item 1: do not hold when the frame drew nothing since the last flip, or when the vblank count has not advanced for 100 ms
  (TD's `flip_pacing_give_up`). (2) Count `holds / flips` in the `[PERF]` line. (3) One hour: grep PD's `framebuffer_probe_tick` (a trapped `PCRTC_START` read every loop) and put it behind
  an env flag, as DJ proposes, if PD's profile shows it.
- **Effort** S. **Licence** MIT. **Priority P1** (it is a precondition of item 1 not regressing loads).

## 9. Movie cap and the subtitle pace  **[P2]**

- **Ours.** `nv2a_hw_d3d11.inc:3467-3507` (movie flips: `RECOMP_HW_MOVIE_HZ` default 0, `RECOMP_HW_MOVIE_VBSYNC` waits for a vblank with `Sleep(1)`
  polls up to 20 ms); `recomp_manual.c:490-515` subtitle budget on QPC.
- **Theirs.** TD handles movies without a cap: XMV is paced by the 60 Hz emulated vblank, which stays at 60 for the XMV player while flips
  are held to the host grid (`high-refresh.md` line 85-88), and the stage state gate (`wanted_rate`) switches the whole feature off outside the
  running state. UP's frame-counter ownership check (`xbox_FrameCounterBump`, `nv2a_flip_hold.h:XBOX_FRAME_COUNTER_*`) decides whether the toolkit or the title moves a counter.
- **Evidence in PD.** The vblank hold (item 1) makes the movie loop's flips land on vblank edges, which is what the `getenv_vbsync` experiment tried with
  polling (`:3486-3490`) and what the comment at `:3484` says helps audio ("37 to 88 ms gaps at packet ends"). So item 1 should replace
  `RECOMP_HW_MOVIE_VBSYNC` and the 30 Hz movie cap together. The subtitle fix can stay on QPC, but its `qn.QuadPart * 60 / qf` should be
  converted to the emulated vblank clock once item 5 makes the vblank count accurate (then both subtitles and movie audio ride the same clock), or use
  `xbox_VblankEdgeNs` as TD does. Keep the pace until measured.
- **Effort** S after item 1. **Licence** MIT. **Priority P2.**

## 10. Watchdog and diagnostics: knowing why the frame rate fell  **[P2]**

- **Source.** TD `pacing_diag.h` / `pacing_watch.h` / `kernel_pacing.c:794-862` (`xbox_PacingFlipCadence`): per flip, whether the
  vblank had passed (late), the executor's busy/idle/hold split; in 2 s windows, classifies EXEC / TITLE / FAULT / MIXED and prints
  one always-on line; a FAULT window resets the display lock, with a 10 s doubling holdoff. Also `RECOMP_TRACE=pacing` (`:1250`): interval
  percentiles, vblank gap range, display period, per-site CPU. And `tools/perf/glass.py` on-glass histograms.
- **Ours.** `[PERF]`/`[PERF-PRES]`/`[MOVIE]` lines in `nv2a_hw_d3d11.inc` and `[TT]` in `kernel_bridge.c:2943`; no cause classification.
- **Port design.** Copy `pacing_diag.h` verbatim (about 250 lines, pure, with a test), feed it from `hw_flip` (flip interval, late, `g_nv2a_exec_ticks` busy, hold
  ns). Print the classification line. This answers "is judder the executor, the game or the pacing" without a debugger and is the A/B
  harness for items 1, 2, 4. The watchdog (reset on FAULT) is only needed once the display lock (item 2B) exists.
- **Effort** S (diag), M (watchdog). **Licence** MIT. **Priority P2** (diag P1 as a measuring tool for 1 and 4).

## 11. Smaller items

| # | Item | Source | Ours | Verdict |
|---|---|---|---|---|
| 11a | Interval trace of the swap NOP, "swap interval a -> b at flip n" | TD `nv2a_pb_exec.c:6163-6176` | no | P1, part of step 0 of item 1. |
| 11b | `flip_cost` test knob: add a fake executor cost with jitter to test the misses | TD `nv2a_pb_exec.c:6085-6106` | no | P3, handy for testing item 1 on a fast PC. |
| 11c | Auto-resume after the pacing gave up | TD `flip_pacing_on:6049-6058` | n/a | Part of item 1. |
| 11d | `RECOMP_DEBUG=vblank_clock=ms` kept for A/B | TD `kernel_bridge.c` | no | Cheap to keep for item 5. |
| 11e | Present-timing strategy `timed` for VRR, `lock` for 60/120/240 | TD `docs/runtime/present-timing.md` | no | Metal-only in TD; Windows presents are plain. Not needed. |
| 11f | `power_auto.c` spin-lead controller | TD | no | Linux/Deck motivated; skip. |
| 11g | Frame counters owned by the title or the toolkit (`xbox_FrameCounterBump`) | UP `nv2a_flip_hold.c:150-215` | PD has `frame_counters_tick` in the ack loop | P3: relevant only if PD's D3D frame counters ever double-advance after item 1. |
| 11h | DXGI refresh read for the window's display (`DwmGetCompositionTimingInfo`, `QueryDisplayConfig`) | Oogie `renderer_d3d11.cpp:380-410` (design); TD `dxgi_vblank_model.h` | no | Needed by 2B/2C. |
| 11i | Pause the pacing state while the overlay/pause is open | TD `kernel_pause.c`, `pwatch_void(PAUSED)` | PD has a console | Only for item 10's watchdog. |

## 12. What does **not** apply to PD

- `RECOMP_FLIP_PACING=host:<Hz>` and `graphics.high_refresh` step control (`steps = 0..4`): they rely on a lag-accumulating stepper (`sub_001B58E0`)
  and on game-state/scene flags of Burnout 3. PD has a hard limiter in `sub_000B0960`; shortening it doubles the game speed (`RECOMP_ANALYSIS.md:589`).
- Race-clock/"potato mode" (`race-clock.md`): a guest patch to make the title run steps by the clock when it is below 60 fps. PD's analogue is
  the resync branch in `sub_000B0960` (late by 1 or more vblanks resets the count) which makes PD run in slow motion, not catch up, below 30 fps.
  If slow motion on weak machines is ever a complaint, the TD design is the model: replace the "reset to 0" with an accumulator and run 2 steps per frame.
  Do not do this speculatively.
- TD's whole Metal/Vulkan present-timing machinery (`present_cadence.c`, `fb_present_metal.m`, `fb_present_vulkan.c`): not relevant to a D3D11 Windows target.
- Oogie's NTSC 59.94 Hz vblank period: PD (Japan, NTSC-J) could consider 59.94 Hz for exact audio sync, but that is not a pacing fix.

---

## Ranked summary

| Rank | Item | Pri | Effort | Why |
|---|---|---|---|---|
| 1 | Hold FLIP_STALL to a vblank edge, with a 100 ms cap and a trace of the swap interval (item 1, 8, 11a) | P1 | M | Root of the judder: presentation time = executor completion time. |
| 2 | Interpolation: present both pictures from one absolute edge, replay before the edge, no out-of-order present (item 4) | P1 | M | The "60 FPS" mode's shake/judder; fast PCs are the case where it breaks. |
| 3 | Bounded present: frame-latency waitable with a 100 ms timeout, `Present` out of `s_hw.lock`, occluded fallback (item 6) | P1 | S-M | The throttled-window block / FPS drop. |
| 4 | Make `vsync=1` real and present on the edge (item 2A) | P1 | S | `vsync` is ignored when tearing is on. |
| 5 | High-resolution waitable timer in the vblank loop, DPC guard (item 5, parts 1-2) | P2 | S | 0-2 ms vblank lateness goes straight into the flip interval. |
| 6 | Cause-classifying pacing diagnostics (item 10) | P2 (P1 as a tool) | S | Needed to prove items 1, 2, 4 on a real run. |
| 7 | Display lock for the emulated vblank (item 2B) | P2 | M | Removes the periodic drop/double on 59.94/60.0x panels. |
| 8 | Event-driven ack thread / UP's vblank-ack thread, no `Sleep(0)` spin (item 7) | P2 | M | A core burns 100 % and the 1.5 ms hold hack stays. |
| 9 | High-refresh beyond 60 (item 3) | P3 | L | Needs variable `t` replay and display-grid edges. |

I did not run or measure PD; the evidence above is static (code and the project's own docs). Verify the judder claim in item 1 with
`[PERF-PRES]` std/max before building.
