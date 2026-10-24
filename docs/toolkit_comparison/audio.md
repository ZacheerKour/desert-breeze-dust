# Audio comparison: Phantom Dust recomp (ours) vs upstream and three forks

Read-only code comparison. Nothing was built or run; "evidence" below is from reading code and docs.
Paths: OURS = `C:\recomp\phantom-dust-re\xboxrecomp\src`. R = the research folder.
UP = `R\upstream-xboxrecomp\src`, TD = `R\takedawn-xboxrecomp\src`, MC = `R\mcdub\xboxrecomp\src`, DJ = `R\defjam\tools\xboxrecomp\src`.
Game layers: `R\takedawn\docs\{mixer,audio-gp,custom-music}.md`, `R\takedawn\src\game\dsound_notify.c`, `R\defjam\docs\known-issues.md`, `R\defjam\scripts\audio-holes.py`, `R\mcdub\src\bink_native.c`.

## Licence note (applies to every item)
All four repos are MIT (sp00nz toolkit, plus the game owners' own MIT). `apu_core.c`, `apu_vp.c`, `apu_dsp.c`, `apu_regs.h`, `apu_state.h` are LGPL-2.1+ files extracted from xemu (see NOTICE); a patch to them stays LGPL for those files. The new files in TD (`apu_limit.c`, `apu_lowpass.h`, `apu_dsp56k.c`, `apu_gp.c`, `apu_bus.c`, `soundtrack/*`) are the toolkit's own MIT code. TD's soundtrack decoders are public domain / MIT-0 (dr_libs, stb_vorbis). Pulling any of this in is licence-safe for us.

## Scope facts found first
- `audio/dsound_device.c`, `dsound_xbox.h`, `wma_decoder.*` are byte-identical across ours, UP, TD, MC and DJ (only `#if` guards differ). There is no DirectSound-bridge or WMA-decoder fix to take from anyone.
- Nobody changed the SSL / stream logic in `voice_get_samples` (LISTDONE / EMPTY): ours, UP and TD are equal apart from diagnostics and an ADPCM guard. The SSL behaviour is the stock xemu one in all of them. The wins are around it (traps, resampling, pacing, locking).
- Ours already has the fixes TD, UP and DJ describe for: whole-mix silence while the front end is trapped (pd-ukj), `PIO_FREE` 0x800, `mcpx_apu_phys` window mapping, ISR delivery from the frame thread, and `HIGHEST` thread priority with `timeBeginPeriod(1)`.

---------------------------------------------------------------------------------------------------

## P1 items

### 1. Voice pitch is ignored: every voice plays at 48 kHz speed  (P1, effort S-M)
- Ours: `OURS\apu\apu_vp.c:1027-1047` `voice_resample` ends with `(void)rate; /* Ignored until we add proper resampling */`. `rate` is computed at `apu_vp.c:1085` and passed in at `apu_vp.c:1124`. Our own notes already say so: `docs\RECOMP_ANALYSIS.md:678` ("voice_resample ignores the voice pitch... wrong for any voice not at 48 kHz").
- Theirs:
  - UP `apu\apu_vp.c:1318-1400` (linear interpolation, per-voice state `rs_have/rs_last/rs_next/rs_pos` in `d->vp.filters[v]`, reset on voice on at UP `apu_vp.c:~77`). Its comment says the old stub "played everything... 8.8 % too fast and drained each stream buffer faster than the title filled it, which is where the periodic dropouts came from".
  - MC `apu\apu_vp.c:1023-1082` (same idea, 32-sample refill, "chipmunk voices").
  - DJ `apu\apu_vp.c:964-1090` (chunk of 8 source samples, see item 12).
- Bug in ours: yes, by inspection. Any voice whose source rate is not 48 kHz (44.1, 32, 22.05 kHz effects and streams) plays too fast and too high.
- Phantom Dust: wrong pitch/speed of music, voices and SFX not at 48 kHz. A 44.1 kHz stream (the three XMV movie voices 0xF5-F7 if the movie audio is 44.1 kHz) is drained 8.8 % faster than the title feeds it, so its packet queue runs dry periodically: that is an EMPTY event, i.e. a gap, and would make "loop rate" effects worse. Cheap check: `g_dbg.vp.v[0xF5].rate` (set at `apu_vp.c:1086`) is != 1.0 for the movie voices if this applies. The previous investigation (RECOMP_ANALYSIS.md 2026-10-19/20) found gaps tied to the next packet arriving late; a stream consumed 8.8 % too fast shrinks exactly that margin.
- Take UP's version (it is the tested one, MIT patch on LGPL file). Needs two fields added to `MCPXAPUVoiceFilter` in `apu_state.h` (UP `apu_state.h`).
- Quality: all three are linear interpolation. UP's comment also warns about steps > 4.2 (voice pitched > 2 octaves up) producing short frames. No fork has a better resampler; a windowed-sinc is the stated upgrade (MC comment).

### 2. Front-end trap handling is racy: FEDECMETH/FEDECPARAM overwritten and FETINTSTS re-latched  (P1, effort S)
Two coupled defects, both fixed in UP and TD, the same flaw DJ documents (`R\defjam\docs\research\dsound-voice-list-crash.md` section 4 and `known-issues.md` "A rare crash in the sound library").
- (a) Ours: `OURS\apu\apu_vp.c:194-195` `fe_method` writes `FEDECMETH/FEDECPARAM` for every call, and `apu_vp.c:1276-1282` (frame walk) calls `fe_method(SE2FE_IDLE_VOICE, v)` for every non-active voice on the list every frame, even while FECTL is already TRAPPED. DirectSound's ISR then reads whichever idle voice was written last, not the one that trapped. Fix: UP `apu_vp.c` "One trap at a time" guard (skip the call when `(FECTL & FEMETHMODE) == TRAPPED`). TD does the same in `mcpx_apu_vp_idle_scan` (TD `apu_vp.c:2242-2271`).
- (b) Ours: `OURS\apu\apu_core.c:107-111` `update_irq` ORs `FETINTSTS` into ISTS whenever FECTL says TRAPPED. DirectSound acks ISTS and resumes the FE afterwards, so its own ack re-latches the bit with a stale trap reason, and the next delivery handles the same FEDECPARAM twice. UP `apu_core.c:196` and TD `apu_core.c:217` removed that and latch `FETINTSTS` once, where the trap happens (UP/TD `apu_vp.c`, in the `SE2FE_IDLE_VOICE` case; ours is `apu_vp.c:515-523`).
- Evidence ours has it: `docs\RECOMP_ANALYSIS.md:449` records a crash "read at guest VA 0xFFFFFFBE in sub_001BF7C1 ... `eax=[esi+0x50]` list head used as `edi=eax-76`". That is the exact signature DJ's analysis traces to this bug (`this == NULL`, fault address `0xFFFFFFBE`, voice client looked up for a stale trapped voice). It was attributed to guest-thread races and did not reproduce. Also `RECOMP_ANALYSIS.md` mentions the same class of crash at `sub_0025C825` (that one is USB/XPP, separate).
- Phantom Dust: idle-voice notifications for the wrong voice or doubled, voices not released/unlinked in DirectSound's lists, occasional crash in the DSOUND voice-list code, and extra ISR calls per trap (cost).
- Keep our pd-ukj decision (the VP keeps running while trapped). UP/TD instead produce no frame while trapped and wait 1 ms; DJ waits up to 20 ms with the clock stopped; MC waits 1 ms per loop. Those are other ways to avoid silence holes; ours is closer to hardware and has no stall. Do not copy them.

### 3. DSP doorbell address is hard-coded in the .bat  (P1, effort S)
- Ours: `OURS\apu\apu_dsp.c:59-82` only acks addresses given in `RECOMP_APU_DSP_ACK`, and PD sets it only in `dist\...\run_game.bat` / `play_watched.bat` (`set RECOMP_APU_DSP_ACK=0x83050810`). `pd-game-v2\src\main.c` does not set a default. Any launch without the .bat (or a different allocation order, different RAM layout, profile/save flow) has no ack, and DirectSound's GP command post spins forever (`apu_core.c:636-641` comment, same stall as Burnout 3).
- Theirs: UP `apu\apu_dsp.c:121-170` (default `auto`: doorbell = GP scratch SGE[0] page + 0x810, derived from `NV_PAPU_GPSADDR`, re-read every poll, paused while GPRST holds the GP in reset). DJ `apu_dsp.c:97-164` has the first version. TD's `apu_dsp.c` carries it too.
- Bug in ours: latent. 0x83050810 = window 0x80000000 + page 0x03050000 + 0x810, so it matches the `SGE[0]+0x810` rule today, but only for the current allocation order.
- PD symptom: silent boot / hang at DirectSound init when launched outside the .bat, or after any change that moves the first GP scratch page.
- Needs `mcpx_apu_dsp_gprst_write` hook in `mcpx_apu_dispatch_mmio` (UP `apu_core.c`, GP region 0xFFFC). Our `mcpx_apu_dispatch_mmio` (`apu_core.c:837-854`) ignores GP writes today.

---------------------------------------------------------------------------------------------------

## P2 items

### 4. Title FE methods run unlocked against the frame thread; voice_lock waits out a whole frame  (P2, effort S-M)
- Ours: `OURS\apu\apu_vp.c:599-606` `mcpx_apu_vp_write` calls `fe_method` with no lock, while the frame thread rewrites the same voice words (`PAR_STATE`, `PAR_OFFSET`, links) read-modify-write. `voice_lock` (`apu_vp.c:152-165`) takes `d->lock`, which the frame thread holds for its whole loop except in `throttle` and one `SwitchToThread` per frame (`apu_core.c:701-703`).
- Theirs: TD `apu_vp.c:661-690` (all FE methods under `d->lock`, "a VOICE_OFF landing inside the frame thread's read-modify-write of PAR_STATE was undone... 0.35-1.6 s mid-race freezes", BO-138) plus `voice_off_sticky` (TD `apu_vp.c:176-206`) re-clearing an off that was undone. UP `apu_vp.c:151-165` per-voice spin lock so a title's VOICE_LOCK does not wait for the whole frame. MC `apu_state.h:474,522` `mcpx_apu_lock_guest` plus lock-free bitmask, frame thread yields the lock to waiters (`apu_core.c:610-612`).
- Bug in ours: probable. The lost-update race is structural in ours; evidence of occurrence is not in our notes, but TD measured it as 0.35-1.6 s stalls of the game thread inside DirectSound's StopEx-and-wait.
- PD symptom: a sound that should stop keeps playing (a looping SFX, music that will not stop), an occasional hitch of the game thread during voice stops, `voice_lock` latency on every DirectSound update that touches many voices.
- Cheapest: TD's approach (lock around `fe_method` in `mcpx_apu_vp_write`, signal the cond if `set_irq`) plus UP's per-voice lock for VOICE_LOCK. Watch the lock order with the ISR delivery in `apu_deliver_irq` (it drops `d->lock` first, as UP/TD note).

### 5. Slice pacing and idle-trap latency (play-cursor granularity)  (P2, effort M)
- Ours: `OURS\apu\apu_core.c:543-571` throttles only every 8th frame; the eight 32-sample slices run back to back and the thread then sleeps ~5.33 ms. A voice's CBO (the play cursor the title reads) jumps 256 samples at once instead of 32 per 0.67 ms; a voice turned off during the sleep is not seen idle until the next period.
- Theirs:
  - DJ `apu_core.c:517-560`: per-slice deadlines (`period_base_us + slice*EP_FRAME_US/8`), `usleep` below 2 ms. Measured on a title that mixes into a looping ring read by the play cursor: stale-lap replays 4.3/s -> 2.2/s. POSIX only ("waits shorter than a millisecond need a sleep the host's condition variable does not have").
  - TD `apu_vp.c:2242` `mcpx_apu_vp_idle_scan` called from `throttle` (TD `apu_core.c:1260-1275`): reports a voice that went off during the sleep right away (BO-78; Burnout 3's StopEx-and-wait spun ~10 ms in guest-main).
- Bug in ours: yes (same structure). Windows port: use a high-resolution waitable timer; UP `platform\host_time.c:55-100` already shows the pattern (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), none of the forks use it for the APU.
- PD symptom: with a title that paces its own streaming off CBO (PD's stream voices 0xF5-F7, and its DirectSound `GetCurrentPosition`-style polling), burstier cursor means more late/early writes; also lets the game thread wait up to ~5 ms inside StopEx-and-wait.
- Pair with item 12 (smaller fetch chunk).

### 6. Output pacing and buffering: wall clock + drop vs device-queue pacing  (P2, effort M)
- Ours: `OURS\apu\apu_core.c:543-571` (wall clock, re-anchor only after > 6 periods late), `apu_core.c:454-463` + `apu_xaudio2.c:140-146` (a full queue drops the 1024-sample buffer, `DROP` event), `apu_xaudio2.c:148-163` (queue empty -> insert two silent 1024-sample buffers = 43 ms of silence, permanent latency). Buffers are 1024 samples x 6; a submission only happens after 1024 samples are staged, so there is 21 ms staging + 43 ms prime + queue.
- Theirs: UP/TD `apu_core.c` `mcpx_apu_pace_step` (UP `apu_core.c:975`): the XAudio2 queue paces the APU with low/high marks (`xa2_pacing_marks`, UP `apu_xaudio2.h:49`), renders ahead by up to the queue capacity, stall fallback to the host clock after 200 ms with periods dropped, no silence priming; device buffers 512 x 4 (10.7 ms each), staging in the backend (`xa2_submit_samples` accumulates 256-sample periods into device buffers; TD `apu_xaudio2.c:140-185`) with one extra staging slot so a queued buffer is never overwritten. MC `apu_core.c:~417-432` paces on `xa2_queued() >= 8` blocks of 256 (4 was too few: "20-44 ms between blocks while the game was busy").
- Bug in ours: drift and jitter policy. The wall-clock producer and the device clock drift apart (a drop or a prime per drift cycle); a producer stall > 32 ms loses time for good (`next_frame_time_us` re-anchor) instead of making it up; there is no recovery if the device stops consuming (XAudio2 critical error / device change): queue stays full, every submit drops, silence until restart. No fork handles XAudio2 device loss either (add an `IXAudio2EngineCallback::OnCriticalError` re-init for ours; own work).
- PD symptom: occasional underrun gaps when the host is busy, constant ~60 ms output latency (audible lag vs picture, A/V offset for movies), long-session drift drops.
- Ours' hazard if converted to device pacing: the APU clock (and CBO, DirectSound's play cursor, XMV movie pacing) then follow the sound card; MC measured titles that pace movies on it. Test with the movie stream voices.

### 7. XGSCNT and the SSL sample-limit tick are not modelled  (P2, effort S)
- Ours: `OURS\apu\apu_core.c:217-219` `XGSCNT` read returns `qemu_clock_get_ns(VIRTUAL)/100` (host time, units not samples); `XGSCNT` writes are plain stores; the SSL limit interrupt (`ISTS_SSLINTSTS`) is never raised (`grep SSLINT` in ours: no hit).
- Theirs: UP `apu_core.c:377,409` (`d->sample_count`, reset by writing XGSCNT) and `apu_core.c:1199-1215` (every 32-sample frame adds to `sample_count`; when it reaches `NV_PAPU_SSL`, set `SSLINTSTS`, `set_irq`). UP's comment: DirectSound (XDK 5849) resets XGSCNT, programs SSL = 2048 and enables ISTS bit 3; its service routine, kicking the GP doorbell and reposting FE commands, "lets the rest of DirectSound initialisation finish". Nobody else has it.
- Bug in ours: unknown. PD (XDK 5849-era DSOUND) probably programs it as well; check `NV_PAPU_IEN` bit 3 and `NV_PAPU_SSL` writes under `RECOMP_APU_TRACE`. If IEN enables it, ours silently never services DirectSound's sample-count timer.
- PD symptom if it applies: time-based DirectSound housekeeping (command reposting, `DirectSoundDoWork` ordering) late or missing; the doorbell ack covers the worst of it.

### 8. Mixdown: asymmetric 5.1 fold, effect bins summed in, hard clamp  (P2, effort S)
- Ours: `OURS\apu\apu_dsp.c:175-200` sums all 32 mixbins, even bins to left, odd to right, then hard clamps at +/-1.0. For a 5.1 title: C (bin 2) goes left only, LFE (bin 3) right only, SL/SR (bins 4/5) split by parity; any effect/FX send bins (reverb input etc.) are added dry on top.
- Theirs: DJ `apu_dsp.c:97-110,250-254` `RECOMP_APU_MIXDOWN=six` (FL/FR + 0.707 C + 0.5 LFE + 0.707 S, x0.7). TD `apu_speakers.c`, `apu_limit.c` (-0.5 dBFS look-free safety limiter, 84 lines), MC `apu_xaudio2.c` `apu_output_safety` (tanh soft limiter at -6 dBFS, master volume). TD's `docs\audio-gp.md` shows what the real GP does with Burnout 3's bins (sends + gains + reverb); that is image-specific.
- Bug in ours: the clamp and parity fold are by inspection; whether PD routes to bins >1 is unknown. DJ's routing report is the tool: `RECOMP_APU_LEVEL=1` (`apu_vp.c` `g_apu_route`, `mcpx_apu_vp_dump_routing`) prints each voice's bin/gain/energy.
- PD symptom: crackle/hard clipping when many bins add up; centre channel panned left and LFE panned right if PD uses 5.1; reverb sends heard dry.

### 9. QueryPerformanceCounter overflow in the shim  (P2, effort XS)
- Ours: `OURS\apu\apu_shim.h:45-52` `count.QuadPart * 1000000LL / freq.QuadPart`. At the usual 10 MHz QPC, `count*1e6` overflows int64 at ~9.2e12 ticks = about 10.7 days of host uptime (QPC counts from boot; a PC kept on with Fast Startup/sleep can reach it). Then `qemu_clock_get_us` jumps/negative and the throttle either never waits or waits forever.
- Theirs: MC `apu_shim.h:51` + `nv2a\qemu_shim.h:379` `qemu_qpc_scale` (split multiply). UP and TD still have the old form, so this is not in the toolkit upstream.
- Bug in ours: yes (arithmetic). Hits only after long host uptime; effect is audio dead or APU clock racing.

### 10. waveOut fallback throws away the game's audio  (P2, effort S)
- Ours: `OURS\apu\apu_core.c:485-537` the waveOut path clears `frame_buf` and renders only the software mixer; the voice processor's mix is never written to it. Only reached if XAudio2 fails to initialise (no device, some Wine setups, locked-down hosts).
- Theirs: UP `apu_core.c` `waveout_submit` (accumulates each real 256-sample EP period into the waveOut buffer; every sample rendered once). Also UP `apu_sdl2.c` for a non-XAudio2 host.
- PD symptom: no sound at all (silent game, clock still runs) on any host where XAudio2 init fails.

### 11. Voice-off undone behind the VP (StopEx-and-wait freeze)  (P2, effort S, depends on item 4)
- TD `apu_vp.c:15-17,176-206` `voice_off_sticky` + `s_off_pending`; if the ACTIVE bit is found set again with no VOICE_ON since the off, it is cleared again. Ours has no such guard, and ours is exposed to the same race (item 4). Take it only if item 4's lock does not close the hole.

### 12. Play-cursor lead of resampler fetch (VRS_CHUNK)  (P2, effort XS once item 1 lands)
- DJ `apu_vp.c:964-1090` fetches the source in 8-sample chunks so the CBO a title reads leads what was heard by at most 8 source samples, not 64 or 130 (UP fetches up to ~130). Measured: step discontinuities in a title-mixed ring (voices 0xF8-0xFA) went from about two a second to none with 8; reason "not traced". If PD streams via a title-written looping ring (not known), use DJ's chunk size with UP's state handling.

### 13. Diagnostics that would pin down PD's "audio gaps"  (P2, effort S each, all env-gated)
Our WAV tap and `RECOMP_AUDIO_DUMP` record what is handed to XAudio2, so a late period reaching the device leaves no hole in the file (UP states this in `apu_core.c` "Host playback accounting"). Take:
- UP `apu_core.c` `hs_*` / `apu_starve_*` (`RECOMP_AUDIO_HOST`): compares `IXAudio2SourceVoice::SamplesPlayed` against the wall clock, reports device-side starvation with the WAV sample index (`xa2_samples_played`, `xa2_underruns` in UP `apu_xaudio2.c`).
- UP `apu_vp.c` ring monitor `RECOMP_APU_RING` (`vp_ring_check`): flags stale-lap replays and the writer's lead on looping PCM buffers. MC `apu_vp.c:752-801` has a smaller version (`mcpx_apu_ring_stats`). DJ `scripts\audio-holes.py` + `RECOMP_APU_VOICE_PCM` is the offline equivalent.
- UP `apu_vp.c` `RECOMP_APU_TRACE` `[APU-VSTAT]` per voice (rate, volumes, bins, RMS, zero-crossing rate, where its data resolved from) and `RECOMP_APU_VOICE_DUMP=all`. TD `[APU-IRQ]` stats line. MC `mcpx_apu_pacing_stats` (waits, lateness, clock restarts, trap time).
- PD symptom they would explain: whether gaps are in the mix (VP) or after it (device); which voice and rate; whether loop-rate related.

---------------------------------------------------------------------------------------------------

## P3 items

### 14. Voice volume ramp for 3D/HRTF voices  (P3, effort S)
UP `apu_vp.c` (HRTF mix loop): ramps from the previous frame's gain (`gain_prev`, `gain_valid`) across the 32 samples; ours (`apu_vp.c:1236-1243`) applies each frame's gain as a step. A volume the title changes every frame (engine-like) buzzes at the 1500 Hz frame rate. Applies to PD only if it moves 3D voice volumes continuously; fades on 2D voices step the same way. `RECOMP_APU_HRTF=0` switch exists upstream.

### 15. ADPCM block stack buffers  (P3, effort XS)
Ours `OURS\apu\apu_vp.c:814-815` sizes `adpcm_block`/`adpcm_decoded` for 2 blocks; the descriptor's `samples_per_block` is a 5-bit field with no check (`apu_vp.c:787`). UP `apu_vp.c:1119-1190` + `apu_regs.h:342` `ADPCM_MAX_BLOCKS 32` sizes for 32 and skips an out-of-range voice with a warning. Stack overflow only if a title sets more than 2.

### 16. Custom soundtracks without ffmpeg  (P3, effort L)
Ours: user converts music with `make_soundtrack.py` (ffmpeg, WMA v2 44.1 kHz) into `TDATA\FFFE0000\MUSIC` and an `ST.DB` (RECOMP_ANALYSIS.md:283-295); the game's lifted WMA decoder plays it; not yet verified in battle. Theirs: TD `src\soundtrack\*` (3.5 kLOC, MIT): builds the `ST.DB` image in memory from a folder of mp3/flac/ogg/wav (dr_libs / stb_vorbis), redirects the MUSIC path through `xbox_PathRedirectHook` to a generated `MUSIC.HOST`, and answers the title's decoder hooks; docs in `R\takedawn-xboxrecomp\docs\runtime\soundtrack.md`. The title-side hooks (which guest functions open a song / create a decoder) are per title and would need PD's addresses. Also TD's loudness normaliser (`custom-music.md` "Loudness"). Nice-to-have; ours works today. The WMA decoder itself is identical in all forks.

### 17. Thread and timer policy  (P3, effort XS)
Ours already sets `THREAD_PRIORITY_HIGHEST` and `timeBeginPeriod(1)` (`apu_core.c:615-619`); UP/TD do not. DJ raises QoS on macOS, MC raises the Horizon host thread (platform specific). Not in any fork but worth doing on Windows 11 (own idea, not verified): `timeBeginPeriod` is per-process and ignored for a minimised/occluded window unless `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` is cleared, so the 1 ms `SleepConditionVariableCS` waits can stretch to 15 ms when the window loses focus; and MMCSS ("Pro Audio") via `AvSetMmThreadCharacteristicsW` for the frame thread.

### 18. Game-layer DirectSound done-list guard  (P3, effort M, per title)
TD `R\takedawn\src\game\dsound_notify.c`: second "voice done" for a voice whose first is still on the core's done list is not inserted again (list could point to itself and the DPC spun at DISPATCH forever). Needs PD's addresses for the same XDK routines (done list at core+0x4A0, entry voice+0x54, handler `sub_002FAAEF`-equivalent; the XDK DSOUND code is the same library). Cause there was the DPC running up to 10 ms after the ISR; TD fixed it in `kernel_hal.c` by waking the timer thread for queued DPCs (BO-173, comment at `kernel_hal.c:561-587`). Ours runs a queued DPC inline in `KeInsertQueueDpc` (`OURS\kernel\kernel_bridge.c:2251-2435`), so the window is already small; kernel side is another report's area. Only add if a spin on `sub_...`-style done list is ever seen.

### 19. GP effects (DSP56300 interpreter, mixer)  (P3, effort XL)
TD `apu_dsp56k.c`, `apu_gp.c`, `apu_bus.c` (host mixer with buses, `docs\mixer.md`, `docs\audio-gp.md`): runs a title's own GP effects image (reverb, distortion) through a small DSP56300 interpreter, bit-exact vs a reference on Burnout 3's image. Ours stubs the GP (`apu_dsp.c:140-216`). Not a fix, a fidelity feature, and the model is partly image-specific (`gp_image.c` in the game layer). Phantom Dust loads a DSP effects image (`RECOMP_ANALYSIS.md:192` mentions `Sound\DSP_Underground.bin`), so its reverb/effects are missing today. Lowest value per effort. The mixer (user bus gains, 5.1 placement) is a user feature, not a bug fix.

### 20. Movie audio sync (mcdub `bink_native.c`)  (P3, effort L, probably not applicable)
MC replaces Bink's audio with host-decoded audio pushed through the software mixer (`apu_mixer_*`), resynced to the video frame (RESYNC_MS 120, drop/insert), because Bink's own audio starved on a busy game thread. For PD the movie audio is fed to the APU by the XMV decoder on the event task `sub_0009C7C0` through stream voices 0xF5-0xF7 (our notes). Same idea would be: decode XMV audio on a host thread, feed the software mixer, keep the XMV audio voices muted. Large, format-specific, and item 1 (rate) plus the already-found no-movie-cap fix may remove the cause. Revisit only if gaps remain after items 1, 2, 4, 5.
DJ's `docs\research\movie-audio-clock.md` confirms (in Def Jam's engine) that the movie clock there is a tick counter and the audio backlog only gates the feeder; useful as a model for reading our event-task code, not as code.

---------------------------------------------------------------------------------------------------

## Considered and not recommended
- UP/TD/MC/DJ "stall the APU while FECTL is TRAPPED" variants: ours keeps rendering (matches hardware) and already removed the silence holes; see item 2.
- TD `apu_sdl2.c` (stall detection, reopen, hot-plug, 5.1 probe): POSIX/SDL only. The idea (device stops consuming -> pretend queue) is item 6 for XAudio2.
- DJ `known-issues.md` "dropped audio buffer at a knockout": not investigated there.
- DJ `PIO_FREE` 0x800 fix: ours already has it (`apu_vp.c:583-587`).
- `apu_mmio_hook.c`: UP/TD replaced our private x86 decoder with a shared `platform\mmio_decode.h`; their reason is coverage of ALU/TEST forms and arm64. Ours already decodes MOV/TEST/CMP/OR/AND; 8-byte (REX.W) accesses are passed whole with size 8 to the 32-bit register model (UP splits them into two 4-byte accesses, `apu_mmio_hook.c` `apu_mmio_rd/wr`). Low risk for PD, cheap to copy if an 8-byte APU access ever shows.

## Suggested order
1. Item 1 (resampler) and item 2 (trap latch + one trap at a time). Together they target the stream underruns, the missed/duplicate voice notifications and the 0xFFFFFFBE crash class.
2. Item 3 (auto doorbell) for launch robustness, item 9 (QPC) and item 10 (waveOut) as one-hour fixes.
3. Items 4, 5, 6 (locking, slice pacing, queue pacing) with item 13's host-starvation counter in place first, so each change can be judged.
4. Items 7, 8 after checking PD's IEN/SSL writes and bin usage with the trace tools.
