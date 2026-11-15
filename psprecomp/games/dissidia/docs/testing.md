# Automated battle tests (no menus, no display)

`tools/battle_test.py` boots the game headless (its own Xvfb display, real GPU if the box has one), walks the menus to a
Quick Battle (Squall vs a CPU fighter), plays a scripted pad input or starts moves directly, and quits. A private copy of the
work folder's memory stick and a private settings file are used, so the user's saves and settings are never touched.
Needs: Xvfb, python3 + Pillow, a save in `<work>/ms0` (Quick Battle unlocked) and a runtime built with the test driver.

    battle_test.py run     --work W --script S --fps 120 --out DIR     # one run
    battle_test.py compare --work W --script S --rates 30,120          # same script at each rate, paths lined up by game time
    battle_test.py actions --work W --actions 2423,2424 --rates 30,120 [--hold SQUARE --stick 128,255 --hold-ms 1500]
    battle_test.py actions --work W --all                               # every action the fighter has

## Rendering on the GPU without an X server (`BT_GPU=1`)

Xvfb has no GPU driver: every run on its virtual display renders with Mesa's llvmpipe (software GL, `[RT] GL renderer: llvmpipe` in
the log), whatever GPU the machine has. The NVIDIA driver can render without any display through EGL, and SDL2's `offscreen` video
driver makes its GL context through EGL:

    SDL_VIDEODRIVER=offscreen __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json  <run.sh ...>

No Xvfb and no `DISPLAY` are needed; the log shows `[RT] GL renderer: NVIDIA GeForce GTX 1660 SUPER/PCIe/SSE2` (the GPU of the
test host) and one harmless `xcb_connection_has_error() returned true` line. The debug socket's screenshots (`S`) still read the
picture, so the HUD checks of the menu walks work as before.

- `battle_test.py` (and every tool built on its `Game` / `start_xvfb`: `combo_timing.py`, `fps_parity.py`, ...): `BT_GPU=1` skips Xvfb
  and sets those two variables; `BT_GPU=auto` does so when the NVIDIA EGL vendor file exists; `BT_EGL_VENDOR=<json>` picks another
  vendor. `fps_parity.py --gpu auto|on|off` (default auto).
- Check that it took: `grep "GL renderer" run_*.log`. `eglinfo -B` lists what EGL offers (the GBM, surfaceless and device platforms
  all reach the NVIDIA GPU on the test host); `vulkaninfo --summary` shows the GPU for Vulkan too (not tried with the game headless).
- Speed: with the GPU, drawing is no longer what limits a fast-forwarded run (a 120 fps parity run took 78 s drawing and 79 s with
  drawing off; with llvmpipe, turning drawing off made 120 fps runs 2.6x faster). Several games share the GPU fine.
- Older notes in this file that say a run used the GPU on Xvfb were wrong: those runs were llvmpipe.

## Pieces

- **Script player** (`runtime/framerate.cpp`, `PSPRECOMP_TEST_SCRIPT=<file>`): lines `<game ms> set|tap|analog|mark|action|quit`.
  Game time starts when the fight's HUD is up (`PSPRECOMP_TEST_GO=<file>`, created by the harness) and advances by dt, so the
  same script means the same at every frame rate. The scripted pad replaces keyboard / pad / debug-socket input.
- **`action <id> [who]`**: starts any action of a fighter without a press (same path as the roster touchdown / chain-window
  starts: the actor's own plya record + `FUN_08B382B8`). `action -1 <slot>` lists the fighter's actions (`[TEST] plya action <id> script <script>`).
  `who`: `player` / `cpu` or a slot number (0 = the first fighter seen = the player, 1 = the CPU).
  Moves that end when a button is released (Revolver Drive: Square held, stick back) need the button held in the script too.
- **`PSPRECOMP_INPUT_RECORD=<file>`**: writes the pad state the game was handed as a script (play a move by hand once, replay at any rate).
- **`PSPRECOMP_TEST_NO_AI=1`** (set by the harnesses): CPU brains (brain +0x54 mode 0) never run their tick (roster.cpp), so the CPU fighter does not act once the test clock runs (earlier the intro needs the brains).
- **`PSPRECOMP_TEST_FREEZE_CPU=1`** (set by the harness): every fighter but the first gets time step 0 while a test runs (a dummy).
- **Logs**: `[FIGHTER] gt=<game ms> ... cur=<script now> pos=<x,z of the body root, +0x110/+0x118>` for every fighter every update in test runs;
  `[MOTION]` lines for spawned objects carry `gt=` and position.

## Setup the harness fixes

Stage 12 (`--stage N`, 1 = Random, default 12: the STAGE row of the battle setup page), Squall vs Lightning. The CPU is a dummy
(time step 0, HP kept at its starting value, actor +0x10B0, so a hit never ends the fight and resets the clock).
Revolver Drive is a ground HP attack (hold the HP attack button, Triangle by default): actions 2289 (script 10004, the start) and
2290 (script 10005, the drive); `--hold TRIANGLE --stick x,y`.

## Known limits (2026-11-10)

- The stage is random and the CPU is a frozen dummy; fighters still get hit reactions (script 800) in some runs, which cuts a move short.
- Body position (+0x110/+0x118) is what a rush changes; the actor origin (+0x80/+0x88) does not move during moves.
- Results are taken from the move's first script only (what follows is recovery / walking).
- First results (Revolver Drive 2290, stage 12, Triangle held, stick forward / left / right / back): 1.2 s, 2.5-3.4 m, peak ~10 m/s at both
  30 and 120 fps; the animation clock (+0x1A8) advances 60 units per game second at both. So the drive's logic position / timing and animation
  speed are NOT faster at 120; suspects left: the camera, effects, and turning (heading change per update).

## Assist gauge tools (2026-11-11, branch assist-gauge)

- `battle_test.py scan --work W --script S --rates 30,120 --at 3000,13000 [--range 08800000-0A000000]`: RAM snapshots at game times
  at each rate (debug socket `RAW`), lists float words whose change per game second differs between the rates. Noisy (heap
  addresses move between runs); good for narrowing a value down, then use a watch.
- Script command `gauge <side> <0..10000>`: sets a side's assist gauge (side 0 / 1 = HUD controller order, not fighter order).
- `PSPRECOMP_ASSIST_GAUGE=1`: `[ASSIST]` (gauge float changes), `[GAUGE_HUD]` (what the HUD reads, percent), `[GAUGE_ADD]`
  (every call of the add routine with the amount), `[GAUGE_DRAIN]` (changes made by the event handler FUN_08AEC7D8).
  `=2` arms the memory-write watch on the first gauge change after 0.5 s.
- `PSPRECOMP_FIGHTER_DUMP_RANGE=<lo>-<hi>` (hex actor offsets, below 0x1400) limits the field dump to one range.
- Layout: gauge float at `[ctrl+0x52C]+0x18`, ctrl = `[[0x089D6A10]+56+4*side]` (ovl FUN_089E95B0); 0..10000; the HUD shows
  value x 100 / 10000 (FUN_089E97FC). Writers: FUN_08AEC1EC (add, clamped), FUN_08AEC628 / 08AEC6FC (int add), FUN_08AEC000 (per
  update tick: passive gain via FUN_08AEC338 and a drain that multiplies by the global step 0x089D5EAC), FUN_08AEC7D8 / 08AECA90
  (event handlers that subtract).

### Findings so far (assist gauge, 2026-11-11)

- Side order in the gauge tables is NOT fighter order: Squall (the harness player) was side 1 and gained gauge from hits (+30 per
  hit step, +46 per update in some bursts); side 0 (Lightning, the CPU) starts at 2000 (20 %), side 1 at 0.
- Idle runs (gauge set to 8000, no input, or one tap then idle; CPU frozen or active; 30 / 60 / 120 fps): **no decay at all** for
  25-30 s. The per-update add FUN_08AEC1EC is called every update with amount 0 while idle. So a drain that is dt-wrong is not
  visible in these setups; the HUD's other moving bars (static word 0x08B937FC, red pixels in a screenshot) are NOT the assist
  gauge: judge only by `[GAUGE_HUD]` / `[ASSIST]`, never by pixels.
- Gauge changes come from event handlers (dispatcher FUN_08AEC558, jump table at 0x08AEC558-relative 0x08AF0000-13384, events 2..22);
  gains are per hit / action from tables, not per update.

## Making automated runs faster (notes for other sessions)

- Build only `cmake --build <dir> --target psprecomp_runtime` (the `test_debug_socket` target fails to link and aborts `all`).
  A fresh build dir recompiles ~100 generated files (~8 min at -j8); keep one per worktree and rebuild incrementally
  (a framerate.cpp edit relinks in well under a minute). Use `nice` so other sessions' runs are not disturbed.
- The menu walk dominates a run (3 min when other tests share the GPU/CPU; the fight itself is 10-40 s). Run the 30 and 120 fps
  runs in parallel (each `battle_test.py run` takes its own Xvfb display and debug port) instead of one after the other.
- Do not take a screenshot loop (`S` command) while measuring timing: it slows the game; and screenshots do not identify the
  gauge. Use the runtime logs.
- Set state directly instead of playing for it: script command `gauge <side> <value>`; `scan` + `RAW` for finding words.
- The CPU dummy (time step 0) is the default; `BT_NO_FREEZE=1` lets the CPU fight. Rounds can end early ("[TEST] battle ended") and
  restart: the game clock and the script restart with the next battle.
- A run sometimes fails with "the battle never started" or "Connection refused" when the machine is busy: just rerun it.
- Never `pkill -f` patterns; use exact pids; check `ps` for another session's psprecomp_runtime before touching anything.

## Assist gauge idle drain (fixed 11 Nov, branch assist-gauge)

- Gauge struct (`[ctrl+0x52C]`): `+0x18` the gauge, `+0x1C` the bar that starts at the battle's initial gauge (2000 for side 0) and
  runs down after a few seconds with nothing done, with an accelerating curve; `[GSTRUCT]` logs the struct (PSPRECOMP_ASSIST_GAUGE=1).
- Bug: the state machine FUN_08AED330 (-> 08AED83C -> 08AEE124) has drain terms per update instead of per step, so at 120 fps the bar
  emptied 3.7x faster (1500 -> 0 in 758 ms against 2767 ms at 30 fps; 60 fps 2x). Fix in `runtime/framerate.cpp` (`gauge_sm_hook`): above 30 fps
  the machine runs every rate/30-th update with the accumulated step (PSP 30 Hz behaviour); `PSPRECOMP_GAUGE30=0` turns it off.
- Test: `battle_test.py gauge-drain --work W` runs 30 / 60 / 120 in parallel with no input and checks the 1500 -> 0 time is equal
  (PASS: 2767 / 2767 / 2766 ms; with PSPRECOMP_GAUGE30=0 at 120: 758 ms). The time before the drain starts differs between runs because
  game time 0 is when the harness sees the HUD (a few hundred ms of jitter); compare durations, not start times.
- Gains from attacks still work at 120 with the fix (checked: gauge 2000 -> 2930 over the attack script).

## On an Android device (`tools/android_battle.py`)

Same menu walk, run on a phone over adb (the runtime's debug socket through `adb forward`, the game's `last_run.log`, screenshots pulled back):

    android_battle.py --serial <ip:port> --renderer vulkan [--driver ID] [--rt 1] [--fps 30] [--shots 4,8] [--env PSPRECOMP_SSAO=0] --out DIR

The game's `env.txt` is replaced for the run and put back afterwards; a private settings file is used. Saves are the device's own (Quick Battle must be
unlocked; autosave is off). The runtime's test driver only runs with a script, so an idle one is pushed when `--script` is not given.


## Debug menu: Enemy AI

Settings > Debug > **Enemy AI** (On / Off, this session only). Off skips the CPU brain tick (`FUN_08A64400`, brains in CPU mode), so the CPU fighter never chooses an
action: it stands and takes hits. It starts to apply about 7 s into a fight (skipping the tick during the intro leaves the fight stuck waiting for the brains).
`PSPRECOMP_DBG_NOAI=1` starts with it off; `PSPRECOMP_TEST_NO_AI=1` is the test harnesses' version (applies once the test clock runs).
The row comes from a generic hook, `psp_settings_add_debug_toggle` (runtime/include/psp_settings.h): a game module adds up to 6 on/off rows to the Debug page.

## Testing on the phone (Retroid Pocket 6) over adb - how it was set up (2026-11-12, branch ssr-effects-pass)

`tools/android_battle.py` is the Android twin of `battle_test.py`: same menu walk, same script player, but the game runs on the phone and
everything goes through adb (debug socket forwarded, log read from the game folder, screenshots pulled back).

    python3 games/dissidia/tools/android_battle.py --serial <ip:port> --renderer vulkan --driver Turnip_v26.0.0_R8 --char lightning \
        --fps 30 --render-scale 1 --shots 6.5,7.5 --env PSPRECOMP_VK_RT_REFL=1 --script S.txt --out DIR

- **Serial**: wireless debugging changes its port every time. `nmap -p 30000-45000 --open <phone ip>` finds it, then `adb connect ip:port`.
- **Build + install** (the Android toolchain lives on the desktop, 192.168.2.131, not on this host): `rsync` the `psprecomp` folder to
  `~/dissidia/ssrfx_src/` there, run `~/dissidia/ssrfx_game.sh` (a copy of `gpu_game.sh` pointed at that source and `~/dissidia/android_ssrfx/`;
  ~2 min incremental, writes `dissidia_ssrfx.apk`), `scp` the APK back, `adb install -r`. Same debug key, so saves and settings survive.
- **What the harness does to the device**: pushes `bt_settings.ini`, `bt_script.txt` and a replacement `env.txt` into
  `/sdcard/Android/data/org.psprecomp.game/files/game/`, force-stops and starts the app, waits for `[TEST] battle started` in `last_run.log`,
  then takes shots (`S` on the debug socket). The old `env.txt` is saved as `DIR/env.txt.orig` and put back at the end. **If a run dies
  (Python traceback) `env.txt` is NOT restored** and the next run saves the leftover as its "orig": check `env.txt` on the device by hand.
  The device's `env.txt` was already stale before these tests (RENDERER=gl, TEST_* lines); it is overridden per run, restore it afterwards.
- **Settings the harness writes** (`bt_settings.ini`): `rt_shadows=0` unless `--rt 1`. That saved `PSPRECOMP_VK_RT=0` used to leave ray query
  out of the device, which made Ray-Traced Reflections "unavailable" in the real menu too (fixed in vkgl_core.cpp `want_ray_query`).
  Use `--driver Turnip_v26.0.0_R8` for ray query (the phone's own driver has none); `--env KEY=VAL` passes any `PSPRECOMP_*` flag.
- **Player / actions**: `--char lightning` makes Lightning the controlled fighter (charId 31); the CPU is frozen unless `--ai`.
  `action -1 0` in a script lists the fighter's actions (`[TEST] plya action <id> script <n>`); `<ms> action <id> player` starts one.
  Spells are the actions whose script is a low shared number: 2293 / 2294 (scripts 54 / 55) look like Thundaga (lightning bolts), **2303
  (script 64) is the Watera water orb**; 2288, 2297, 2299, 2302, 2298, 2305 are other spells. Found by playing candidates and looking at
  contact sheets, not from names. Script times are game time (30 fps = real time); `--shots` are wall seconds after the fight starts.
- **Screenshots are 480x272** whatever the render scale; sheets of 9-12 frames at 480x270 each read well in one image.
- **Where to write output**: the harness copies a few MB per run. `/tmp` on this host is quota-limited by Claude Code's own temp dir
  (`/tmp/claude-1000`, 11 GB): write to `~/dissidia/ssrfx_out` instead (a full /tmp makes `adb pull` die at 8%).
- Never `pkill -f android_battle` from a shell whose own command line contains the pattern (it kills that shell); use `pgrep` and `kill <pid>`.

### Faster phone runs and profiling (2026-11-15)
- `android_battle.py --direct` skips the menu walk like the desktop harness (the recorded walk is pushed as `bt_walk.txt`; needs a build with
  `direct_battle.cpp`): in battle after ~34 s on the RP6. `--use-settings --drop rt_refl_level --env PSPRECOMP_VK_RT_REFL=2` tests the player's own
  settings with one row changed. The fight HUD check counts the two gauge probes double (`hud_dark`): with the player's settings the portraits
  next to the outer probes are bright and the old check never fired.
- The game folder moved to `/sdcard/Android/media/org.psprecomp.game/game` in newer builds; the harness picks whichever has `last_run.log`.
- Achieved frame rate: `[FPS] <n> GE frames/s` lines in `run.log` every 5 s. GPU per pass: `--env PSPRECOMP_VK_PASSPROF=1` (`[VKPASS]` lines).
- CPU hot spots (rooted device): `simpleperf record -p <pid> -f 2000 --duration 10 -o /data/local/tmp/perf.data` as root during the fight, push the
  unstripped `game_and/libmain.so` to `/data/local/tmp/symfs/<the app's lib path>`, then
  `simpleperf report -i ... --symfs /data/local/tmp/symfs --tids <render thread> --sort symbol` (the render thread is the one with samples in `vulkan.ad07xx.so`).
- Local APK build on this host: `~/dissidia/rtrefl_apk/build_perf.sh <psprecomp dir> <work dir>` (a copy of the tag-team script), ~1.5 min.

### Shader gotchas found on the phone
- The Vulkan layer allows **4 samplers per program** (`kMaxSamplerBindings`, "too many samplers" at link) and only `DEPTH24_STENCIL8`
  depth textures (`[VKGL] unsupported texture format 0x81a6` = DEPTH_COMPONENT24). A failing screen-space shader switches shadows, AO and
  reflections all off. Check `grep -a "SCREENFX\|unsupported texture\|link failed" last_run.log` after every shader change.
- `common` (and other GLSL reserved words) compile on desktop GL and fail on the phone.

### Reflection effects layer (issue #7) in short
- Screen-space reflections: blended draws are re-issued into a per-target layer (`fxl_*` in psp_ge_draw.cpp); the layer is cleared after the pass
  used it, so effects drawn after the 3D run are used one frame late. `PSPRECOMP_SSR_FX_DEBUG=1|2|3`, `PSPRECOMP_SSR_FX_GAIN`, `PSPRECOMP_SSR_FX_THICK`,
  `PSPRECOMP_SSR_FX_OFF`, `PSPRECOMP_SSR_FX_LOG`. Works on GL and the Vulkan layer; not used while ray-traced reflections are on.
- **Effects in the ray-traced reflections (2026-11-15, Medium / High).** Why the first try looked bad and what fixed it (psp_ge_draw.cpp `rt_capture`
  fx path, psp_screenfx.cpp `k_rt_refl`): (1) the effects lists were cleared at the frame's clear, but the game draws its spell / beam effects
  after the 3D run, so Watera / Thundaga were never traced (`PSPRECOMP_RT_REFL_DEBUG=1` showed no magenta): the lists are now kept and traced by
  the next frame. (2) Big blended triangles are background sheets (cloud / sky domes, flash planes) that covered half the floor: dropped by area
  (`PSPRECOMP_RT_FX_MAX_AREA`, 30). (3) Flat sheets on or just above the floor (splash rings, ground ripples) were hit by every floor ray: hits lower
  than `PSPRECOMP_RT_FX_MINH` (0.6) above the surface are skipped. (4) The hit colour came from the scene copy, which does not hold effects drawn
  after the 3D run (it showed the floor behind them as a haze): the effect triangle's own colour is used, and lit effects use their vertex colour
  (the material is plain white: the green rails came out white). (5) Effects drawn with their own (sometimes identity) view matrix were placed
  wrong (straight white bars over the floor): re-expressed with the main view like the opaque debris (`rt_fix_view`; the last completed scene's view
  for effects after the 3D run). (6) Effects are additive layers (a rail = white core + green ribbon): up to 4 effect triangles along the ray are
  summed in one non-opaque traversal (candidates, never committed). Contribution is additive over the floor with the screen-space layer's base
  (`PSPRECOMP_SSR_FX_GAIN`). `PSPRECOMP_RT_FX_LOG=1` prints the colour state of each kind of effect draw (`[RTFX]`).
  Test: `rtt_run.sh` / `android_battle.py --direct --char lightning` with `place 3`, `action 2303 player` (Watera) and `action 2293 player` (Thundaga).
  RP6 (Medium, the player's settings): normal fight ~84.6 fps (same as reflections Off); casting every 3 s ~67-72 fps (Off ~83, effect tracing off ~76).
  Not done: camera-facing sprite effects (RECTANGLES: the orb's bubbles, glow cards) are still not captured.

### Testing with the player's own settings (android_battle.py --use-settings)
- `--use-settings` copies the device's `settings.ini` (render scale, SSAO, RT rows, driver ...) into the private test settings; `--fps-override --fps 30`
  keeps the saved settings but plays at 30 fps (script times are game time); `--drop KEY` leaves one saved key out (bisecting).
- **`hd_textures=1` makes the scripted spell cancel itself** (`action 2303` plays script 10047 for one update, then idle, no orb): `--drop hd_textures`.
- The screenshot is taken when the debug socket answers: with the player's settings the fight clock starts ~1 s later, so catch the Watera orb with
  shots at 6.5 - 9 s (action 2303 at game time 3000).

## Making automated runs fast (notes for other sessions)

Measured 2026-11-11 while building the Jecht combo timing tests (`tools/combo_timing.py`). The menu trip dominates a naive run
(about 3 minutes with the box busy); the fight itself can go many times faster than real time.

- **Boot once, feed many trials.** The test driver's `batch <file>` command holds the script until the file exists, then plays
  (and deletes) its lines. A driver (`combo_timing.py` `Session.submit`) writes the next batch into the *running* battle, so the
  menus are walked once per frame rate, not once per experiment. Never start a new game per trial.
- **Fast-forward**: debug socket `FF <ms> [x]` makes the vblank wait skip its sleep, so the game runs as fast as frames render
  (x caps it at x times real time). Re-issue before the 120 s cap (`battle_test.run --ff` / `Session` do it every 50 s). Test scripts
  run in game time, so results are identical with or without it. Menus: `FF 120000 4` works (`ff_menus=4`); unthrottled `FF`
  breaks the title (it never opens the save list) and 6x made cursor presses get lost. Turn it off (`FF 0`) before the clock starts is
  not needed: the harness leaves it on for the script.
- **Run the rates in parallel**, each in its own process with its own Xvfb display, debug port, memory-stick copy and `--out`
  directory (the `Game` class already does this): 30 / 60 / 120 fps boots and sweeps overlap. Use absolute `--out` paths (a relative
  one breaks `PSPRECOMP_MS0`) and a fresh `--out` each run (the log is appended to, an old "Debug socket listening" line makes
  `wait_log` return at once).
- **Own work folder and build dir** (`~/dissidia/work_<name>/` with symlinks to `work/` plus its own `build/`, `cmake -S <worktree>/psprecomp/runtime -B
  work_<name>/build ...` with the same `PSPRECOMP_*` cache values as another work folder): other sessions run tests too; never share
  a build dir, work folder, display or port with them. An incremental runtime rebuild is a few seconds to a minute.
- **Time things relative to the move, not the clock.** `sync [timeout] [script id]` waits for the player's action to change (or
  become a given script id) and `+<ms>` lines count from that update; absolute times drift with press latency (a Rush tap is script 10000
  while the button is down and 10001 after release). Put a `waitidle` / `sync` before the first `+` line: `+` times count from
  the last sync, 0 at the start, so a press and its release can fall into the same update and be lost.
- **No dead time between trials**: `waitidle` (player out of any action) then `place <m>` (teleport next to the dummy) beats
  walking back. Taps shorter than two updates at 30 fps (67 ms is used) are sometimes missed.
- **Detect results from the log, not screenshots**: `[TEST] gt=<ms> scr <old> -> <new>` lines (the player's action script id) carry
  everything needed; screenshots (`S`) cost a settle wait each.
- Other findings: the harness CPU dummy (time step 0) still takes hits (HUD BRV changes); Jecht's Rush from the start position
  (20 m) never reaches the dummy, so `place 3` first; HP attack is Square, bravery Circle (not Triangle).
- Background long runs with `nohup` and send a Gotify message at the end (see ~/CLAUDE.md).

## Skipping the menu walk (`battle_test.py run --direct`)

The picture-by-picture menu walk takes about 100 s to reach the fight. `--direct` replays a recorded pad sequence inside the game
(`runtime/direct_battle.cpp`) and gets to `[TEST] battle started` in about 30 s (Squall vs Lightning, stage 12, no screenshots until the HUD is up):

    battle_test.py run --work W --script S --out /abs/dir --direct            # tests/replays/quick_battle_walk.txt
    battle_test.py run ... --direct my_walk.txt --ff-menus 4                  # another recording; --ff-menus defaults to 4 with --direct

- **How**: the game polls the pad once per frame, and the poll number is the one clock that does not change with fast-forward. The first part is rule
  driven (CROSS 3 polls of every 15 until the save has loaded; the runtime turns `PSPRECOMP_SAVEDATA_AUTO` on for the replay because the save list dialog
  reads the host pad, not the filtered one), the rest is the recorded rows, each as polls since the save loaded and a pad mask. The runtime logs
  `[DIRECT] replay finished`; the harness then waits for the HUD, starts the clock as the normal walk does, and fast-forward goes off.
- **Speed limit**: 4x. The replay finishes at the same poll at 8x and unthrottled too (the poll count is deterministic), but loads run in host time, so the
  recorded gaps get too short and the game falls back to the menu or never enters the fight. 6x failed in a try as well.
- **Flags** (test only, off by default): `PSPRECOMP_TEST_DIRECT_BATTLE=<file>` replays a recording; `PSPRECOMP_TEST_PAD_RECORD=1` makes a normal harness walk
  log its pad (`[PADREC] <poll> <mask>`, and `[PADREC] loaded <poll>`). Another character or stage: record a walk (`battle_test.py run --char X --stage N ...` with
  `PSPRECOMP_TEST_PAD_RECORD=1`), then `tools/walk_extract.py run_30.log walk.txt` and pass `--direct walk.txt`.
- The pad filter is the one in `framerate.cpp` (`jecht_grid_filter` calls `dissidia_walk_filter` first), and the save events are two counters in
  `psp_hle_utility.cpp` (`g_savedata_list_opened`, `g_savedata_completed`).

## Finding a stat the HUD shows (BRV, HP...) in guest RAM, and what keeps a test fight alive

Notes from the Jecht combo work (2026-11-11), for whoever needs a live value again.

- The fighter struct (`actor`, the block `g_fighters` points at) holds only *base* stats at `+0x10A8..+0x10B8` (`450 <BRV base> <HP> 100 23`);
  `+0x10B0` is the HP the harness keeps for the dummy. The **current bravery is not in the actor (0x0000-0x3000), nor in the info
  block at `+0x2F8..+0x304`**: dumps over 3 hits showed no int or float that follows the HUD there.
- `PSPRECOMP_TEST_DUMP=1` logs `[TESTDUMP]` word dumps of the player and the dummy at every action change
  (`PSPRECOMP_TEST_DUMP_RANGE=lo,hi` in hex offsets, default 1000,1400; also the 0x600 bytes behind the info pointers).
- **The `scan` script command** is the general tool: `scan <min drop> <max drop> <max value>` snapshots user RAM
  (0x08800000..0x0A000000) and keeps only words that fell by that much since the last scan (three encodings at once: int,
  int x 100, float). Put one scan before the first hit and one after every hit (`+0 scan 0 0 99999` first, then `+2300 scan 30 700 3000`
  after each Rush); after two or three hits few words are left, and the log lists them with the words around them and every word in RAM
  that points just before them (the owner). Guest addresses must be masked with `0x07FFFFFF` before indexing `g_ram`
  (forgetting it reads the wrong memory and finds nothing).
- Result so far: after 3 Rush hits the only survivor was a **float at 0x09AE9DCC** (2291 -> 2103 -> 1915 -> 1725, about -188 per
  hit, same address in every run) inside a small object (vtable-like `08AC03B8` before it). It does not match the HUD's own numbers
  (the HUD dropped about 234 a hit) and is probably a gauge animation object, not the stat; the real BRV is still unlocated.
- **Why fights ended mid-sweep**: every Rush takes ~190-230 bravery; after ~5 hits the dummy is BROKEN, from then on the attacker's
  hits do HP damage (the player's bravery, 4000+), and the game's KO check runs inside the update, before the harness rewrites the
  dummy's HP each update. Fix used: in AI-off mode the dummy's HP is rewritten to 100,000,000 every update, so a hit cannot reach 0.
  Taking Jecht's equipment off (the user's tip) lowers his damage and would delay the break; not needed with the big HP pool.
- A *frozen* dummy (`PSPRECOMP_TEST_FREEZE_CPU`, time step 0) is no good for combos: its hit reaction never finishes and the attacker
  hangs in hit-stop (Jecht stuck in script 10031 with a constant motion time). `PSPRECOMP_TEST_AI_OFF=1` instead blocks the CPU pad's
  bit setters (`FUN_08AB387C` / `FUN_08AB38C4`; pad object `+4` state, `+8` pending, `+12` previous) so the CPU never presses a
  button, and lets it run and react normally.

## Combo input windows across frame rates (`tools/combo_timing.py`)

Built 2026-11-11 for Jecht (his chain inputs have 3 to 5 frame windows at 30 fps; the wiki list is
https://dissidia.wiki/Jecht_(Dissidia_012)/Combos). One battle per frame rate runs in parallel (own display / port / memory stick); the CPU
fighter has no AI input (`PSPRECOMP_TEST_AI_OFF=1`) but is hit normally; trials are fed as script batches (`batch`), so menus are walked once
per rate.

    combo_timing.py sweep  --work ~/dissidia/work_x --rates 30,120 --prefix 'CIRCLE@800=10001' --probe CIRCLE:n --from 150 --to 700
    combo_timing.py combos --work ~/dissidia/work_x --rates 30,120      # Rush > N2/Up2 > N3/Up3 > HP, the window of each step

A trial: wait until the player and the dummy are idle (`waitidle`), `place` both at a fixed spot, press the prefix inputs (each pressed a fixed
delay after the previous move's script id appeared, `sync <script id>`), press the probe D ms after the last move started, and log which
script id follows. The accepted window of a transition is the set of D that start the chain move; the same D grid (every 1/120 s) is used at
every rate, so windows compare directly. Jecht's moves: Rush = Circle (script 10000 while the button is down, 10001 the rush itself);
Neutral 2 = Circle again (10004); a Circle outside the window starts 10030/10031 (the "repeat"); HP = Square.

Things that broke the first runs, now handled (see also "Making automated runs fast"):
- Every hit pushes the dummy on (42 m after 18 trials, out of the arena, where the game runs into unrecompiled code and logs a line per
  call: a 42 GB log). `place` puts both fighters back at fixed spots; `Session` aborts a game whose log passes 1.5 GB.
- A dummy still in a hit reaction does not take the next hit, and no hit means no chain: `waitidle` waits for the dummy's motion to be 1 / 2.
- The per-update fighter log (`PSPRECOMP_FIGHTER_LOG`) is tens of GB at 120 fps: off in these runs, only `[TEST]` lines are kept in memory
  (the first version kept every line: 23 GB of python memory).
- A fight has a time limit; when it ends the game starts a new fight and replays the initial script. `Session.submit` notices and plays the
  batch again in the new fight.
- The dummy gets 100,000,000 HP (a broken dummy takes HP damage and the KO check runs inside the update).

## Debug menu options for playtesting (Settings > Debug page)

Added with `psp_settings_add_debug_toggle` (runtime/include/psp_settings.h: a game module adds on/off rows, this session only; Dissidia's are registered in `runtime/framerate.cpp`).

- **Enemy AI** (On / Off): Off skips the CPU brain tick (`FUN_08A64400`, brains in CPU mode), so the CPU fighter never chooses an action; it stands and takes hits. It starts to apply about 7 s into a fight (the intro waits for the brains). `PSPRECOMP_DBG_NOAI=1` starts with it off.
- **Jecht Combo Timing** (On / Off): for the player's Jecht, any combo step (ground and air). The screen edge flashes green when the window for the next press opens, and each attack press (Circle, Square, Triangle) shows GOOD / TOO EARLY (n ms) / TOO LATE (n ms) as a toast and a red or green edge flash. It reads the game's own rules, nothing is tabled (framerate.cpp, "Debug menu: Jecht combo timing helper"):
  - **Brave chain steps** (Rush > Neutral 2 / Up 2 > Neutral 3 / Up 3, and the air chain): the move picker asks the chain check `FUN_08AB81E0` for each candidate; its table is a list of 8-byte elements `{action id, start, end}` where element j follows element j-1's action while `start <= actor+0x1A8 (motion time, ticks) < end`. The hook reads that table live. Jecht's table lists a "repeat" move (the id with more than one window: 1629 for Rush at [8,20) and [26,38)) around the chain moves (1664 / 1665 at [18,28)), and the repeat wins where they overlap, so the real window is the chain move's minus the repeat's: Rush > N2 / Up 2 ticks 20..26, N2 > N3 / Up 3 38..44, Stream > next (air) 40..46. Measured at 30 and 120 fps: every press the helper called GOOD started the chain move, every TOO EARLY / TOO LATE started the repeat move.
  - **HP finishers** (Square after Neutral 3 / Up 3 / Down 3): not in the table; timeline flags. Op 0x004C holds `+0x196` bit 4 (lockout range) and a press in it latches `+0x5B4` bit 2; the window is `+0x195` bits 0 / 1 or `+0x194` bit 7 (ops 0x36 / 0x37 / 0x3A). The helper hooks the three gate functions (`FUN_08B334D0`, `FUN_08B33508`, `FUN_08B33544`, a0 = actor, they return the bit unless the latch is set): open = GOOD, lockout = TOO EARLY, closed after having been open = TOO LATE. Checked at 30 fps: HP started only for presses the helper called GOOD.
  - Presses are the host pad's (`psp_ctrl_last_sample`: keyboard, pad, scripts): Circle (Brave) and Square (HP) only, never with L or R held (dodge, dash and dodge cancel are R + Cross / Triangle), and only while the fighter is in a move: after a move ends the game keeps its "current action" fields (`+0x101C` pointer, `+0x1A8` motion time), so a press from neutral was being judged against the last move and read TOO LATE. A press so early that the game ignores it (before the move accepts any input) is judged against the table with the motion time at the start of that update; (the chain table is read from the global, so this works from the first press.)
  - Only Jecht's own attack moves (script ids 10000-10199) count: the dodge, dash and guard have timeline windows of their own. The chain table is read from its global, the pointer at `0x08AB8388` (the move picker `FUN_08848160` loads it as `*(0x08AC0000 - 31864)`; it is the same table the chain check gets inside a move), every update, so the flashes work from the first hit without any press. (Caching the table from the chain check's calls was wrong: a move started from neutral calls it with the fresh-start table, which made the windows vanish after every fresh Rush; before that, nothing showed until a chain press had been made.) The edge flashes green when a window opens and orange when it closes with the move still running (not when a press started the next move). When the move is over the game keeps its last "current action" and frozen motion time, which looked like a window that never closes.
  - Env: `PSPRECOMP_DBG_COMBO=1` starts with it on; `PSPRECOMP_PRESS_LOG=1` logs `[COMBO]` lines, `[CHAIN]` (the elements reachable from the current action) and `[FLAGS]`.
- Not done: infinite enemy HP / bravery or zero player damage. HP is held by `+0x10B0` in test runs only; the live bravery value was not found in RAM (see "Finding a stat the HUD shows"); a hit takes exactly 115 BRV off in the test setup whatever attacker stat words (`+0x10A8`, `+0x10AC`, `+0x10B4`, `+0x10B8`) are zeroed.
- The game's own pressed test (`FUN_08AB399C`, the press latch's hook) returned no edges for the scripted Jecht chain presses at 30 or 120 Hz in test runs (`press latch: ... 0 edges` in the `[FPS]` line), so the press latch may not be doing anything for these inputs; not investigated.

- Jecht's Circle / Square are handed to the game on a 30 Hz grid above 30 fps (`jecht_grid_filter`, `PSPRECOMP_JECHT_GRID=0` to turn it off): the Jecht Combo Timing helper above therefore sees each press on a grid update, and a press can reach the game up to one 30 Hz frame after you pressed it. Details and measurements: docs/frame-rate.md "Jecht timed windows on the 30 Hz grid".

## Frame-rate parity tests (`tools/fps_parity.py`)

Every move of every playable character has to play at every frame rate the settings offer (30, 60, 90, 120, 150, 180, 210, 240) as it
does at 30 fps, the PSP's rate (issue #55: Exdeath's teleport and Squall's ground moves at 60 fps). `fps_parity.py` checks that,
character by character, with no menu walking per character:

    fps_parity.py run --work W [--chars all | exdeath,squall,10] [--rates 30,60,...,240] [--jobs 5] [--repeat-base] [--out DIR]
    fps_parity.py compare DIR [--frame-tol 1 --pos-tol 0.25 --pos-rel 0.05]   # judge finished runs again
    fps_parity.py show DIR squall 1454 mash                                    # one trial at every rate, side by side, 30 Hz frames
    fps_parity.py chars                                                        # the 31 playable characters and their charIds
    tests/run_fps_parity_test.sh W [out] [run options]                          # the same as a test (exit code 1 on a difference)

Output: `DIR/summary.txt` (characters x rates), `DIR/<char>/report.txt` (every failing trial and why), `DIR/results.json`.

### How a run works

- **One game per character and rate**, in parallel (`--jobs`), each with its own display / port / memory stick copy. On the GPU by
  default (`--gpu auto`, see "Rendering on the GPU without an X server").
- **Any character from the one recorded walk.** `--direct` replays `tests/replays/quick_battle_walk.txt` (Squall vs Lightning) and
  `PSPRECOMP_TEST_CHAR_SWAP=15=<charId>` (runtime/direct_battle.cpp) swaps the fighter. What the menu hands the battle is the block
  at 0x09AFB8A0 (FUN_088FB1A4 copies it): per side a 0x1060-byte fighter record (stride 0x1060) that starts with a copy of the
  character's 0x778-byte save record (byte 0 charId, byte 1 level, abilities...; the CPU side's is generated), and at +0xF3A the object
  table id of the model the loading screen (FUN_0888C4C8, `bs+2008`) loads (`P_<code>_1P`: object table `*(0x089D4968)`, byte 6
  charId, byte 7 costume 1, byte 8 type 0; +0xF3E is the assist's `_A` entry). Team structs (count byte, charIds, stride 0x38)
  follow at +0x272F. On every pad poll a side whose record holds `<from>` gets `<to>`'s own save record (save records at
  `*(0x089D712C) + 0xF58 + 0x778 * index in the 31-entry id list at 0x089CBC98`), its charId and its model id. Lightning under test
  uses `15=31,31=15` (the dummy becomes Squall). Things that did NOT change the fighter: the menu's selectable list, the grid cells
  (their +4 is not a charId), the battle setters FUN_08A48DB0 / 08A491E0 and FUN_089132A4 / 089121F4 (not on the Quick Battle path),
  FUN_08846E18 (charId -> object entry; not called there), the team structs alone.
- **The dummy**: the CPU fighter never presses a button and its brain does not tick (`PSPRECOMP_TEST_NO_AI`, `_AI_OFF`) but it runs at
  the normal time step and reacts to hits; 100,000,000 HP so no hit ends the fight.
- **Trials**: the game lists the player's plya actions (`action -1 player`: id, script, `later` +3, `side` +5 2 ground / 1 air,
  `button` +0x0F 1 brave / 2 HP); the tool writes the trials into the running battle (`batch`). Each trial: `waitidle`, `home 3.5 0 0`
  (dummy at stage point 0,0, player 3.5 m away, both turned to face each other with FUN_08835BA4, velocities zeroed), then
  - every action, **plain**: `action <id> player` (air actions 0.4 s after a jump);
  - first presses of an attack, **mash**: the same, plus the move's button (Circle / Square) tapped every 200 ms for 2.4 s, so the
    follow-up presses and chains run too;
  - movement every fighter has (pad input, not plya): jump, double jump, dodge, air dodge, dash (R+Triangle held), air dash, block
    (R held), run (stick forward 1.5 s), run + jump. They start after 1 s of settling. Running is camera-relative, so `run` also checks
    that the camera ends up in the same place.
- **Samples every update** (`PSPRECOMP_TEST_PARITY=1`, framerate.cpp): `[PAR] <ms> | player: script motion motion-time x y z fwd.x fwd.z |
  dummy: same | hits`; updates between two 30 Hz frames carry a `~` after the time. Every rate is a multiple of 30, so each has an update
  at game time k x 33.3 ms (the lattice) as well.
  `sync` / `waitidle` end on the lattice and event times are compared with a 0.01 ms tolerance (the sum of 8.33 ms steps is
  99.99999 at 100 ms), so a trial starts at the same game moment at every rate. `[PARHIT] <ms> dummy atkp=<id> react=<n> flags dmg by=`
  comes from FUN_0883A150 (take hit: a0 defender, a1 the 64-byte hit record).
- **Judging**, each rate against 30 fps, per trial: the player's script sequence and the dummy's motion sequence on the lattice (same
  ids in order, each change within `--frame-tol` 30 Hz frames); the dummy's hits (count, attack ids and reactions in order, time within
  `--frame-tol` frames, HP damage within `--dmg-tol` 3%: HP damage is the attacker's bravery, which varies by ~2% between two 30 fps
  runs); both paths (largest gap <= `--pos-tol` m or `--pos-rel` of the distance covered) at the best alignment within `--shift` 1 frame,
  in whole updates of the faster rate; peak heights within `--height-tol` 0.1 m; when the player is idle again.
- **Phase is not judged.** A move starts at the end of the first update after it was asked for: 33 ms after the press at 30 fps, 17 ms
  at 60, 8 ms at 120, so a faster rate can lead by up to a frame (the same 25 ms the Jecht chain windows showed). Comparing paths at a fixed
  game time would fail every move for that; the report counts the trials where the player ran ahead instead. `--repeat-base` makes a second 30 fps run: trials that differ between two 30 fps runs are reported as
  NONDETERMINISTIC, not as failures. With the GPU, two 30 fps runs agreed on every trial in the checks made (Squall, Exdeath).
- **Speed**: boot to fight ~35 s (`--direct`, 4x menus); then fast-forward. On the GPU about 10x real time at 30 fps, 3.5x at 120.
  `PSPRECOMP_TEST_NO_DRAW=2` (set by `--no-draw`) stops the draw calls once the clock runs (`ge_draw_request_no_raster`): 2.6x faster
  with llvmpipe, no gain on the GPU.

### Findings so far (15 Nov, Squall and Exdeath, 30 vs 60 / 120 fps)

- **Moves lead by up to a frame at high rates** (block, jump, dodge and most attacks were 0.3-0.8 m "off" in the first 100 ms when
  compared at the same game time): the phase described above, not a speed difference. Delaying the scripted presses by a whole frame
  (`PSPRECOMP_TEST_INPUT_DELAY=33.3`, `--input-delay`) makes the fast rates a frame late instead; the right delay would be 33.3 ms minus
  one update. Dodge and block still differ after the alignment (0.8 m / 0.3 m around the first frame).
- **Jumps get lower as the rate rises**: Squall's jump peaks at 4.10 m (30 fps), 3.94 (60), 3.85 (120), 3.80 (240); double jump 6.71,
  6.39, 6.21, 6.12; an Exdeath air action started 0.4 s after a jump starts 0.2 m lower at 120 fps and lands 67 ms earlier.
- **Multi-hit moves land a different number of hits**: Exdeath's action 1960 (script 10000, attack 1271 repeated): 19 hits at 30 fps,
  18 at 120; Squall's action 1454 (script 10015): 2 hits at 30, 1 at 60; follow-ups chosen by a mash differ.
- **Camera**: after `home` the 120 fps camera ends up on the other side (running forward goes +z instead of -z).
- Whole-roster results: see the latest `summary.txt` of a full run (`~/dissidia/fpsparity/full1` on the test host).
