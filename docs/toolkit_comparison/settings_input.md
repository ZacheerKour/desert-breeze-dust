# Settings menu, overlay, launcher, input, graphics options: comparison (read only)

Scope: OURS = C:\recomp\phantom-dust-re (pd-4f7 = graphics settings UI). Short names below:
- DJ = R\defjam (src/hooks/*, tools/xboxrecomp/src/{input,settings,usb,d3d})
- TD = R\takedawn-xboxrecomp (src/{overlay,input,enhance,kernel}); TDG = R\takedawn (src/game, docs)
- UP = R\upstream-xboxrecomp, MC = R\mcdub\xboxrecomp
All of them are MIT. Dear ImGui is MIT. SDL3 is zlib. Nothing was built or run; line numbers were read from the files.
Sizes marked (est.) are estimates, not measurements.

## 0. How our stack differs (decides what can be copied)

- Ours is a Win32 window on its own thread (xboxrecomp/src/video/fb_present.c:fb_thread, ~L650). Rendering goes through a D3D11 swap chain inside xboxrecomp/src/kernel/nv2a_hw_d3d11.inc (hw_init L613, hw_present_locked L3011, Present L3187). Input is XInput plus VK key state (xinput_device.c) behind an emulated OHCI/USB hub (usb/ohci.c, usb/usb_gamepad.c).
- DJ is the same shape (Win32 window thread, D3D11 swap chain, OHCI/USB pads). Its overlay, window, cursor and settings code ports almost directly.
- TD's overlay is SDL2 + Vulkan/Metal (overlay_ui.cpp, 4974 lines, tied to Burnout art and fonts). Take its ideas (pause, input block, restart tags, combo guard, rumble curve), not its code.
- AA, sharpen, aniso and texture upscale exist only in TD, and only for Vulkan/Metal. There is no D3D11 version anywhere in R.
- Our build is C only (pd-game-v2/CMakeLists.txt:13 `project(pd_recomp C)`, MSVC via build_env.bat). ImGui needs CXX enabled for one small static library.

## 1. Rumble is dropped in ours (P1, S)

Gap: ours discards every OUT packet (xboxrecomp/src/usb/ohci.c:634-646 "accepted and discarded"). Nothing calls xbox_InputSetState (xinput_device.c:287). The USB model has no rumble function, and UP has none either. DJ, TD and MC all forward it.

Source:
- DJ tools/xboxrecomp/src/usb/ohci.c:750-775. The endpoint-2 interrupt OUT and the ep0 class SET_REPORT (21 09) data stage both call usb_gamepad_out(dev-1, bytes, len).
- DJ usb/usb_gamepad.c:224-262 usb_gamepad_out. It checks the report is `00 06 lL lH rL rH`, then calls xbox_InputSetState. RECOMP_RUMBLE_LOG=1 logs it.
- DJ input_host.c:402-462 apply_rumble. Options: floor % (a faint pulse is raised), minimum hold ms, strength %, a connect buzz. It refreshes about every 100 ms and mutes without focus or while the UI is open.
- TD input/rumble_curve.{c,h} (63+72 lines, pure). motor = (speed/peak)^(1/strength), a gate (off below 800, on from 1200), and a ~90 ms release.
- TD input_core.c:226-376 (rumble_step/silence/SetBlocked/SetBackground) and input_backend.h. An effect lasts 250 ms and is re-given every 100 ms, so a hung process stops by itself. Motors are zeroed at exit, at crash and on signals (input_core.c:393-460, xbox_InputPanicStop).

Design for PD:
1. Add usb_gamepad_out to our usb_gamepad.c and call it from the OUT branch of ohci.c:634. Keep our existing split where ep != 0 must not clear g_setup_pending.
2. Add a small rumble tick in xinput_device.c: low/high, then the curve, then XInputSetState. Mute while the overlay is open or the window is unfocused. Zero all motors in an atexit/crash path.
3. Setting `rumble=0..3` (strength), default 1. RECOMP_RUMBLE_LOG=1 first shows whether PD sends rumble at all (not verified; it should, it is an Xbox title).
Effort: ~120 lines plus the 63-line curve. Licence MIT. The risk is a stuck motor after a crash, hence the exit path.

## 2. DPI awareness is missing in ours (P1, XS)

Gap: nothing in pd-game-v2/src or xboxrecomp/src calls SetProcessDpiAwareness*, and there is no manifest. On a 125/150 % desktop the OS bitmap-stretches the window. `window=1920x1080` is therefore not 1920 real pixels, the picture is blurred, and "fullscreen" is not native resolution. A render scale taken from the window height (hw_auto_scale, nv2a_hw_d3d11.inc:2993) is then computed from a virtualized size.

Source: DJ src/hooks/d3d11_translator.c:336-352 (window_thread). It calls SetProcessDpiAwarenessContext(-4 = PER_MONITOR_AWARE_V2) via GetProcAddress, before the window exists. The overlay then scales fonts with GetDpiForWindow (pc_ui.cpp:455-470 display_scale).

Design: put the same 8 lines at the top of fb_thread (fb_present.c ~L650, before RegisterClassA) or at the start of WinMain (pd-game-v2/src/main.c). Also change AdjustWindowRect to AdjustWindowRectExForDpi where the window is sized (nv2a_hw_d3d11.inc:677-682, fb_present.c fb_toggle_fullscreen fallback). Optional: a manifest instead.
Effort: ~15 lines. Test at 150 % scaling: `window=1280x960` gives a 1280x960 client, fullscreen gives 3840x2160 on a 4K screen.

## 3. Replace the hand-rolled settings.ini parser with recomp_settings (P1, S-M; prerequisite for pd-4f7)

Ours: pd_load_settings (pd-game-v2/src/main.c:807-840) is read-only. It converts keys to RECOMP_* env vars and only helps at start-up. There is no write-back, no live apply and no range clamp. The comments in settings.ini are static text.

Source: DJ tools/xboxrecomp/src/settings/recomp_settings.{c,h} (456+135 lines, portable C, no dependencies, MIT; tests in DJ tests).
- A typed table (bool/int/enum/string with min/max and help text).
- Precedence default < file < env var (env wins, never saved). This is the same rule as our pd_setenv_default.
- Atomic save: write `.tmp`, then MoveFileExA(REPLACE_EXISTING) (recomp_settings.c:403-453).
- Unknown keys are kept and written back. Help lines are written above each key.
- recomp_settings_on_change callbacks, so the window code can apply a change live.
DJ usage: pc_settings.c:51-103 (table), 358-410 (pc_display, setting_changed, pc_display_set), 483-597 (init order).
TD alternative: overlay/xbox_settings.{c,h} (1131+428 lines, TOML, bound to its RENV table, writes only user-changed keys). It is heavier and coupled; use DJ's.

Design for PD:
1. Vendor recomp_settings.{c,h} into pd-game-v2/src (do not modify xboxrecomp/).
2. Table = fullscreen, window (w, h), render_scale (0 = auto), aspect, interpolation, vsync, keys, plus new filter/rumble/aa/sharpen. Keep the `RECOMP_*` env names as the `env` field so play_watched.bat overrides still win.
3. Keep the current flat settings.ini valid. recomp_settings expects [section] headers, so either migrate with the same key names under [display]/[input] (reading the old file once) or verify it accepts section-less keys. Not checked.
4. pd_load_settings becomes: register table, load, then export the values still needed as env vars for code that has not been made live yet (nv2a_hw_d3d11.inc reads getenv at L636-662).
Effort: ~1 day. Risk: low, it is pure C with tests.

## 4. In-game settings overlay (pd-4f7 core) (P1, M-L)

### What DJ and TD do

DJ (D3D11, Windows, our case):
- Present hook: d3d8_present_set_overlay(pc_ui_overlay) (tools/xboxrecomp/src/d3d/d3d8_present.c:297-303). It is called at L368 after the title's picture is scaled into the window, with (device, immediate context, window RTV, w, h).
- Overlay draw: pc_ui.cpp:1676-1752 pc_ui_overlay. First open runs ImGui CreateContext, ImGui_ImplWin32_Init, ImGui_ImplDX11_Init. Each frame: NewFrame, a dim rect (alpha 150), a settings panel (82x86 % of the window), Render. It saves and restores the pipeline RTV/DSV around RenderDrawData (L1744-1750).
- Window messages: translator_wndproc calls pc_ui_window_message first (d3d11_translator.c:173). That handles the overlay key, key capture for bindings, and ImGui_ImplWin32_WndProcHandler only while open (pc_ui.cpp:1652-1681).
- Game input is blocked while open: xbox_HostInputSetUiActive(1) (input_host.c:911). Every pad and the keyboard read at rest, and rumble stops. The UI reads the raw pad via xbox_HostInputRawPad (input_host.c:924) and feeds ImGui gamepad keys (pc_ui.cpp:418-450 ui_feed_gamepad).
- It does not pause: the game keeps running (docs/launcher-and-overlay.md, "keeps running; it does not pause").
- Open keys: F1 (configurable), or both stick clicks held 500 ms (pc_ui.cpp:1580-1620 chords).
- Settings are saved as you change them (the shared table). Live: fullscreen, window size, aspect, filter, vsync. Restart: render scale and widescreen.
- Environment-overridden settings show locked.

TD (Vulkan/Metal): same idea, plus the missing pieces:
- Pause: kernel/kernel_pause.{c,h} (316+~100 lines). The executor blocks at FLIP_STALL after the frame is presented (xbox_HostPauseHold, L238). The title's CPU then stalls on its own fence and pushbuffer waits, so no guest thread is suspended mid-instruction. Listeners (the APU, apu_core.c:1580) stop audio. The game clock is not frozen; Burnout's own catch-up cap (4 steps) bounds the jump after resume. overlay_ui.cpp:1886 pause_sync, 1903 set_open.
- Input latch after close: input_core.c:339-353 SetBlocked. After the overlay closes, each port stays neutral until its buttons are released once, so the press that closed it is not also the game's.
- Restart tags: overlay_ui.cpp:340 xbox_OverlayWantRestart, 1940 restart_reason, 1968 restart_now. The footer lists pending restart-only changes. Settings flagged XS_RESTART (xbox_settings.h) are compared with the launch value.
- Thread model: events from the window thread, frame built on the present thread, one mutex (overlay.h header comment).

### Where it lands in ours

| Piece | Our place | Change |
|---|---|---|
| draw hook | nv2a_hw_d3d11.inc hw_present_locked, just before `IDXGISwapChain_Present` (L3187). Objects: s_hw.dev, s_hw.ctx, s_hw.bb_rtv, s_hw.bb_w/bb_h | add `if (s_overlay_cb) s_overlay_cb(dev, ctx, bb_rtv, w, h)`. Save/restore RTV/DSV/viewport/scissor/blend/rasterizer/shader state like DJ (the present pass sets many, L3018-3046). ImGui_ImplDX11 also backs up and restores its own state. |
| window messages | fb_present.c fb_wndproc (L236 ff.) | call `overlay_wndproc(h,m,w,l)` first; when it returns 1, return 0. Forward WM_CHAR and the mouse messages too. Today WM_KEYDOWN updates s_key_down (L263) before anything could see it. |
| input block | xinput_device.c: xbox_InputGetState (L~213), keyboard_state | one global `g_ui_active`: return a neutral pad and skip the keyboard merge; keep a raw-read function for ImGui. Add the release-latch from TD. |
| cursor | fb_present.c WM_SETCURSOR (L337-343) always hides | show the arrow while the overlay is open (DJ update_cursor/WM_SETCURSOR, d3d11_translator.c:122-166, 230-245). |
| lock | hw_present_locked runs under the hw lock | the overlay needs its own mutex between wndproc (fb thread) and the present thread (DJ ui_lock). |
| pause | hw_flip (nv2a_hw_d3d11.inc:3467), called from nv2a_pb_exec.c:3412-3427 (FLIP_STALL) | phase 2: TD's hold-at-flip. Needs an audio pause hook (nothing like apu_core.c:1580 exists in ours). Interaction with our frame-interpolation thread (nv2a_hw_interp.inc) and the title's timing after resume is untested. |

Dependencies and size:
- ImGui 1.92.x: imgui.cpp, imgui_draw.cpp, imgui_tables.cpp, imgui_widgets.cpp, backends/imgui_impl_dx11.cpp, imgui_impl_win32.cpp. About 1.3 MB of source, roughly 0.5-0.9 MB added to the exe (est.). DJ fetches a pinned tarball (cmake/imgui.cmake, version 1.92.9b, SHA256 pinned); TD vendors it (third_party/imgui + VENDORED.txt, 3.4 MB with all its backends). Use the vendored approach: no network at configure time.
- Licence: MIT (Omar Cornut). Ship its licence text.
- Build: `LANGUAGES C CXX` and one static lib (C++17, MSVC) that links d3d11. The glue can stay C-callable: `overlay_init(hwnd, dev, ctx)`, `overlay_wndproc`, `overlay_draw`, `overlay_open()`.
- Hotkey: F10 (our docs/RECOMP_ANALYSIS.md:651 already notes "F10 overlay not done"; F1 also fine). Controller chord: DJ's both-stick-clicks for 500 ms is safe for PD (PD uses single stick clicks). TD's Back+Start guard (input_core.c:149-222) delays Back, which PD may use; avoid it.

MVP scope (what pd-4f7 needs):
1. One panel, default ImGui style. Rows: Fullscreen, Window size presets, Render scale (Auto, 1-8), Aspect, VSync, Filter, 60 FPS interpolation, Keyboard layout, Rumble.
2. Each row edits the recomp_settings table (section 3). A "restart needed" tag on aspect and interpolation (see section 5).
3. Keyboard/pad navigation (ImGui nav + the raw pad feed); the mouse works.
4. No pause on the first cut, and the panel says so. In a battle that is dangerous, so either block opening in battle if detectable, or ship pause as P2 (TD design above).
Effort: MVP ~600-900 lines of our own glue plus ImGui. About 3-5 days including cursor, input block and testing; pause adds 2-3 days. DJ's ui_draw pages are ~2000 lines for a much larger menu; do not copy those, build a small fresh panel.
Licence: MIT for all sources. Priority P1.

### What not to take
TD's gamefont/gameart/sounds (title-specific extraction), update checker, shader build, mixer pages, HDR, Android pieces. DJ's texture packs, recorder and dev menu.

## 5. Live vs restart-only per setting (design table for pd-4f7)

| Setting | Today | Live possible? | How |
|---|---|---|---|
| fullscreen | init-only env (nv2a_hw_d3d11.inc:662-672); F11/Alt+Enter toggles (fb_present.c:188-232) but is not saved | yes | call fb_toggle_fullscreen via PostMessage to the window thread (DJ WM_APP_FULLSCREEN, d3d11_translator.c:60-90, 305-315); persist on toggle (DJ pc_display_set) |
| window size | init-only (L674-684) | yes, windowed only | DJ WM_APP_RESIZE (d3d11_translator.c:197-209) and remember a dragged size (L158-166, WM_EXITSIZEMOVE) |
| render_scale | auto follows window height live (hw_auto_scale L2993-3008) but a numeric value is startup-only | yes | a setter that sets s_hw.scale and s_hw.auto_scale and calls hw_release_all_targets(), exactly what hw_auto_scale does. DJ and TD only apply it at restart, so ours can do better. |
| vsync | s_hw.vsync is a plain int read at L3189 | yes | a setter, 1 line |
| filter (blit sampler) | one sampler samp_blit (L869) | yes | create a point and a linear sampler, choose per present (DJ d3d8_present.c:27,54,234-237,331). Name the setting `filter`, not `interpolation`, which already means 60 FPS frame interpolation. |
| aspect | env; feeds s_hw.widen/aspect through the vertex path (L2059-2072, 2433-2444, 2563) | no (restart) | tag "restart needed" |
| interpolation | getenv once (nv2a_hw_interp.inc:126) | maybe | needs the interp thread to start/stop; restart tag first |
| keys layout | env read once (`static int on = -1`, xinput_device.c:~109) | yes | read from the settings table each poll |

## 6. Input

### 6a. Key bindings and remap (P2, M)
Ours: two hard-coded layouts, keyboard_state_mmo and the classic one (xinput_device.c:93-190) reading key_down(VK) from the window's s_key_down (fb_present.c:128-140, sided Shift/Ctrl/Alt via fb_vk_sided L154-180). The `keys=` setting only switches between them.
Source: DJ input/input_map.{c,h} (384+176 lines, pure C, no platform deps).
- InputControl enum: 16 pad controls plus 8 keyboard-stick directions.
- InputBindings: up to 4 keys per control.
- input_bindings_parse("L, Space, Mouse1") and input_bindings_format.
- input_keys_resolve builds the pad (a stick from keys is a full-deflection circle, so diagonals are not faster).
- input_apply_stick (radial deadzone with rescale) and input_apply_trigger.
- input_pad_map (pad remap by source name).
Key capture UI: DJ pc_ui.cpp:299-380 (capture_start/commit_key, Esc cancels, 250 ms arming so the click that started it is not the answer, mouse buttons and wheel bindable). The VK codes are already what our window records, mouse buttons included (fb_present.c comment at ~L128), so no translation layer is needed.
TD alternative: input/keyboard.c (183 lines): one table plus a README-generated check test. Simpler but fixed.
Design: make mmo and classic two default binding tables (the existing key chains become data). Add `[keys]` lines like `a=1, Mouse1` to settings.ini. An overlay page "Keyboard" lists controls with Add/Clear/Default buttons. Keep `keys=mmo|classic` as the preset selector.
Effort: M (2-3 days with UI). PD specifics: Space = left stick click (jump), Tab or right mouse = lock on, and name entry uses ne_poll_keys (main.c:874); keep those working when bindings change.

### 6b. Pad hot-plug and extra pad types (P2)
Ours: XInput only, ports 0-3 mapped 1:1 to hub ports (usb_gamepad.c:621 xbox_InputGetState(pad)). The number of pads the game sees is fixed at boot (ohci.c:1403-1416 RECOMP_USB_PADS; play_watched.bat sets it to 1; the arrival loop ohci.c:1204-1220 only adds pads up to s_npads). A second pad plugged in later is never presented to the game. DualSense, DualShock 4, Switch Pro and generic DirectInput pads work only if Steam Input wraps them.
Source:
- DJ ohci.c:1496-1500 xbox_UsbSetPadCount(n) raises s_npads at run time (4 lines). input_host.c:247-262 assign_device calls need_players(slot+1) when a pad takes a slot, so the emulated hub plugs in one more controller while the game runs.
- DJ input_host.c (1000 lines) is the full layer: SDL3 gamepad backend (DualSense, DS4, Switch Pro, DirectInput), XInput fallback scanned every 1 s (L612-650), Steam duplicate suppression (L319-376), keyboard as its own player (plan_slots L290-318), focus gating, ui_active, virtual test pads. SDL3 3.4.18 is fetched, hash-checked and built static with only the gamepad subsystem; zlib licence.
Options:
A. Cheap (P2, S): copy only xbox_UsbSetPadCount and call it from an XInput connect scan in xinput_device.c (a 1 s recheck of not-connected ports, TD input_core.c "probe backoff" L490-500). Make s_npads volatile (ohci.c:171 is a plain static read by the OHCI thread). Default RECOMP_USB_PADS to what is plugged in.
B. Full SDL3 backend (P3, L): only worth it if non-Xbox pads matter. ~1400 lines (input_host.c + input_map.c) plus a build dependency. Steam-duplicate logic comes with it.
Whether Phantom Dust uses more than one local pad is not verified here; if it is single-pad locally, A is only insurance.

### 6c. Pad chords and combo guard (P2, S)
DJ pc_ui.cpp:1580-1605 (chords list, 500 ms hold for two or more buttons). TD input_core.c:149-222 (Back+Start hotkey state machine: Back is delayed 250 ms so the pair never reaches the title; a tap is replayed for 50 ms). For PD use DJ's; it needs no delay on any game button.

### 6d. Mouse look (P3, S)
Ours already collects raw mouse deltas (fb_present.c:140-150 s_mouse_dx/dy, WM_INPUT) but xbox_FramebufferTakeMouse has no caller (grep; the stub at fb_present.c:818 is the no-window build). UP has the consumer: src/input/mkb_input.{c,h} (475+60 lines). mkb_merge() adds the mouse to the right stick with sensitivity, deadzone compensation, acceleration, invert-Y, swap sticks and an F9 capture toggle (env or mkb.ini). PD has a free camera on the right stick (Q/E in the mmo layout). Design: a mouse-to-right-stick merge after the keyboard merge in xbox_InputGetState, active only when the cursor is hidden and the window focused, plus DJ's cursor confine in fullscreen. Settings `mouse_look=0/1`, `mouse_sens`. Risks: PD's lock-on mode and menus must ignore it, and the overlay must release the cursor. P3.

### 6e. Button glyphs (P3, L)
TD input/pad_glyphs.{c,h} (242+231 lines, pure) picks the family from SDL type/USB ids (Xbox, Xbox modern, PS4, PS5, Nintendo, Steam, positional, keyboard) and swapped-face variants. TDG src/game/glyphs.{c,h} (168+28) replaces the title's prompt textures when they upload, identified by the hash of their bytes, using the toolkit's nv2a_texrep with art rendered at build time from SVG. Our tree has no nv2a_texrep. For PD this is only attractive for keyboard players (the mmo layout uses 1-4 for A/B/X/Y). First find out where PD draws controller icons (menu text or textures); not investigated.

### 6f. Not applicable
TD controls.c/h (PS2 driving scheme for Burnout), DJ save_compat.c (a hash fix for Def Jam save folders), android_vibration.c, touch_pad.*.

## 7. Window, cursor, DPI (P2, S-M)

- DPI: section 2.
- Window thread: DJ creates the window on a dedicated thread that only pumps messages (d3d11_translator.c:336-370) and posts WM_APP_* from other threads. Ours already has fb_thread pumping its own messages; changes from the overlay should PostMessage, not call SetWindowLong across threads (hw_init does it at L662-672 during start-up, which is fine once).
- Centre and clamp the initial window to the work area (DJ make_window, L267-300). Ours uses CW_USEDEFAULT (fb_present.c:669) and a 1920x1080 request is larger than a small laptop's desktop.
- Remember a dragged size (DJ remember_window_size L158-166).
- Cursor: hide after 2 s idle, always hide and ClipCursor in fullscreen, show while the overlay is open (DJ L99-156, 225-245). Ours hides always (fb_present.c:337).
- Persist the F11/Alt+Enter toggle (DJ routes both through pc_display_set("fullscreen")).
- Minimum size 320x240 via WM_GETMINMAXINFO and WM_ERASEBKGND returning 1 to avoid flicker (DJ L246-249).

## 8. Graphics options

| Option | Source | What it does | Port to ours | Effort | Priority |
|---|---|---|---|---|---|
| Render scale (live) | ours hw_auto_scale L2993 | see section 5 | setter | XS | P2 |
| Blit filter smooth/sharp | DJ d3d8_present.c:27,54,234-237,331 | bilinear or nearest when scaling the render target to the window | second sampler in hw_present_locked | XS | P2 |
| Fit / overscan modes | TD XS_DISPLAY_FIT, XS_DISPLAY_OVERSCAN (xbox_settings.c ~L94-98) | present-rect mode (integer, fill) | the present rect is computed at nv2a_hw_d3d11.inc:3028-3034; add modes | S | P3 |
| Sharpen (CAS) | TD RECOMP_GFX_SHARPEN (platform/recomp_env.h:161), nv2a_pb_vulkan.c:2941-2977 post passes | AMD FidelityFX CAS strength 0..1 | the GLSL does not carry over; write HLSL CAS (MIT) in ps_blit or a following pass | S-M | P3 |
| AA (FXAA/SMAA) | TD RECOMP_GFX_AA (recomp_env.h:148) | post-process AA on the game-resolution frame before the window scale | needs an intermediate render target + HLSL FXAA 3.11 (public domain) or SMAA (MIT). At render scale 2-4 the benefit is small. | M | P3 |
| Anisotropic filtering | TD RECOMP_GFX_ANISO (recomp_env.h:147), 2..16 | sampler setting for mipmapped minified textures | D3D11_FILTER_ANISOTROPIC where guest samplers are created in nv2a_hw_d3d11.inc (location not looked up) | S | P3 |
| LOD bias from render scale | TD XS_GRAPHICS_LOD_BIAS (auto) | keeps textures crisp at higher internal scale | sampler MipLODBias = -log2(scale); check against PD first | S | P3 |
| Texture upscale / UI scale | TD nv2a_texup.h, nv2a_texscale.h | GPU upscale of world textures, 2D art scaled before upload | large, Vulkan-coupled | L | skip |
| HDR, dither, bloom/motion blur toggles | TD | | game-specific | | skip |

## 9. Launcher (P3, M, depends on section 4)

DJ: pc_launcher.cpp (227 lines) + pc_launcher_home.cpp (96) + pc_launcher_sdl.cpp: a Win32/D3D11 window shown before the game loads. It uses the same pc_ui pages as the overlay. Play and Quit buttons, a "skip launcher next time" setting, `--launcher`/Shift to bring it back; restart-only options apply on Play. Docs: R\defjam\docs\launcher-and-overlay.md. TD has no launcher, only an installer GUI (TDG docs/installer-gui.md).
For PD: it replaces Settings.bat (Notepad) and covers settings that need a restart (aspect, interpolation) before a long load. The cheap version, once the overlay panel exists, is a separate small window created before the guest starts, running the same panel: ~300 lines. Alternative with no launcher: TD's restart footer and a relaunch button (overlay_ui.cpp:1940-1985). P2 for the restart footer, P3 for the full launcher.

## 10. Test hooks worth copying (P3, S)
DJ docs: RECOMP_LAUNCHER_SCRIPT (timed click/move/key/shot/ui:<target>), RECOMP_WINDOW_SHOT, RECOMP_INPUT_VIRTUAL_PADS. TD overlay_shot and test_pad/test_key in overlay_ui.cpp:2219-2300. We already have pad_live.txt scripting; adding "open overlay + screenshot" to it makes pd-4f7 testable unattended.

## 11. Ranked summary
1. DPI awareness (P1, XS). Biggest quality gain for the least code.
2. Rumble forwarding (P1, S). Currently silent; sources exist in three forks.
3. recomp_settings for settings.ini (P1, S-M). Prerequisite for any UI.
4. ImGui D3D11/Win32 overlay with an MVP panel (P1, M-L). Pattern from DJ pc_ui.cpp; hook point nv2a_hw_d3d11.inc:3187; input block, cursor and wndproc forward.
5. Live setters: vsync, fullscreen persist, window size, numeric render scale, filter (P2, XS each).
6. Pause when the overlay opens (P2, M). TD kernel_pause design; needs an audio hook and testing with the 60 FPS interpolation.
7. Key bindings table + capture UI (P2, M). DJ input_map.c.
8. Run-time pad count (xbox_UsbSetPadCount) + XInput connect scan (P2, S).
9. Window niceties: centre and clamp, remember size, cursor policy (P2, S).
10. Launcher, mouse look, AA/CAS/aniso in HLSL, glyphs, SDL3 backend (all P3).

## 12. Not verified
- Whether PD actually sends rumble reports or uses more than one local pad.
- Whether recomp_settings accepts a section-less file (it has [section] headers by design).
- The ImGui exe size and the cost of enabling C++ in our CMake build.
- The overlay's state changes against our present pass (ps_blit, gamma LUT, interp "show" texture) were read, not run.
- Behaviour of our frame-interpolation thread under a pause.
