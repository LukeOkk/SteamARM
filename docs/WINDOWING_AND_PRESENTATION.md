# Windows and presentation

Linux programs draw with X11. SteamARM gives them an X server whose windows
are real macOS windows, and lets Vulkan frames bypass that server
altogether. Line numbers are at `dbd1657`, except where a line names
another commit; the V-Sync and native-client sections were updated on
2026-09-29 after stages 22-23.

Labels: MEASURED (a command and its result), VERIFIED IN SOURCE (file:line),
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN.

## Two display modes

| mode | X server | how you see it | state in the launcher model |
|---|---|---|---|
| native windows (default) | SteamARM's own build of XQuartz's X server, rootless, display `:2` (`scripts/run-x11-native.sh`) | each X toplevel is a macOS window | ready |
| VNC | Fedora's Xvnc (aarch64) under lxrun, display `:1` (`scripts/run-app.sh`, `ensure_xvnc`) | Screen Sharing at `vnc://127.0.0.1:5901` | experimental: fine for the Steam UI; Vulkan games cannot present |

The mode is `display` in `settings.json` or `STEAMARM_DISPLAY`
(`launcher/SPEC.md`). `launcher/ApplicationCore.swift:522-523` (at
`b3f64c8`) carries the states and reasons.

## Native windows

MEASURED, `benchmarks/stage11-native-x11.txt`:

- xorg-server 21.1.24 (hw/xquartz), built from source for arm64 by
  `scripts/build-xquartz.sh`, run as `SteamARM-X11.app` without the XQuartz
  package, `sudo` or launchd. Command line
  `X11.bin :2 -nolisten tcp +iglx`; `+iglx` enables indirect GLX, the only
  GLX a Linux guest can use against this server.
- Every mapped X toplevel is one window of the server's process in
  `CGWindowListCopyWindowInfo`. The server publishes its CGWindowID as
  `_NATIVE_WINDOW_ID`.
- quartz-wm (pinned commit, one patch) gives native title bars, moving and
  resizing (stage 14).
- 21 extensions, including MIT-SHM, GLX, RANDR, RENDER, Present and
  XKEYBOARD, and XTEST (22) since stage 29: `run-x11-native.sh` sets
  XQuartz's `enable_test_extensions` in the server's own defaults domain
  (`STEAMARM_X11_XTEST=0` leaves it off). XTEST acts inside this server only,
  whose clients are SteamARM's guests (no TCP); Steam Input's desktop
  configuration and `tests/android/xtest_input.py` use it. **No Composite.**
- Keycodes as on Linux (stage 29, `xquartz-evdev-keycodes`): XQuartz
  numbered keys as macOS virtual key codes plus 8, Xorg with evdev as Linux
  key codes plus 8, and Linux programs assume the latter (Weston's X11
  backend, and through it Android's key layouts; Chromium's and Electron's
  `KeyboardEvent.code`). The keysyms still come from the macOS layout key by
  key, so what a key types is unchanged. MEASURED: `xmodmap -pke` shows
  keycode 38 = a, 9 = Escape, 50 = Shift_L, 133 = the Command key (Meta_L).
  `STEAMARM_X11_EVDEV_KEYCODES=0` keeps XQuartz's numbering. Both take effect
  when the server starts; `scripts/setup.sh` restarts it after a rebuild when
  no guest is running.
- Patches: `xquartz-log-file-env`, `xquartz-signals-to-server-thread`
  (clean stops in 0.22 s instead of SIGKILL after 20 s),
  `xquartz-evdev-keycodes`, and the remote layer below.

## Vulkan frames without a copy (CALayerHost)

MEASURED, `benchmarks/stage12-native-present.txt`:

1. The shim's `vkCreateXlibSurfaceKHR` / `vkCreateXcbSurfaceKHR` ask the
   runtime for a remote `CAMetalLayer`, create the Metal surface on it and
   set `_STEAMARM_LAYER` (the layer's CAContext id) on the X window
   (`shim/wsi.c:185-189`). Surface-capability queries resize the layer to
   the window first.
2. The patched X server (`patches/xquartz-remote-layer.patch`) watches that
   property and hosts the layer with a `CALayerHost` over the window, child
   windows included, following moves, resizes, map and unmap.
3. Frames go MoltenVK → `CAMetalLayer` → WindowServer. The X server's CPU
   use during presentation is 0.0 %.

The x86 path is the same after the thunk: x86-64 xcb → FEX thunk → aarch64
`wsi.c` → remote layer → MoltenVK (MEASURED, 122.9 fps at 800×600 into Xvnc).

Pacing, hosted in the native X server, 3000 frames without interaction:
median 6.056 ms (165.1 fps, the display's refresh), 1% low 6.798 ms, max
7.196 ms. With interaction (moves, unmap/map, new windows): the same median,
1% low about 21.5 ms, max about 424 ms, around unmap/map. Not investigated.

Limitations (stage 12):

- **Retina:** the drawable is X-pixel sized, so a 2× screen shows it
  upscaled (the test display was 1×).
- **Two windows per toplevel** for the window server: Mission Control and
  Stage Manager move the X frame and the overlay separately.
- **Stacking** relies on re-ordering after Xplugin's changes; a missed hook
  can leave the overlay below its frame for up to 300 ms.
- **X drawing under the overlay is hidden** while the property is set, and
  XShape is ignored.

## VNC

- `run-app.sh` runs `build/lxrun /usr/bin/Xvnc :1 ... -SecurityTypes VncAuth
  -localhost -rfbport 5901 +extension GLX` (VERIFIED IN SOURCE,
  `scripts/run-app.sh:223-225`). Xvnc compiles its keymap through the Steam
  root's `/bin/sh`, an x86 bash, so VNC mode needs FEX and a FEXServer
  (`:219-221`).
- **The Metal layer is not visible in VNC.** Only the patched native server
  hosts `_STEAMARM_LAYER`; Xvnc is a framebuffer and never shows it
  (MEASURED, stage 12). The X server sends nothing back, so the shim cannot
  tell whether anyone shows its layer.
- What VNC is good for: the Steam UI and other 2D X programs (the x86 client
  ran on Xvnc through stage 8).
- Its Fedora packages (Xvnc and about 40 dependencies, `xkbcomp`, Mesa
  swrast) and the `scripts/vnc_*.py` helpers are REMOVE_LATER once native
  windows cover every use (`docs/CURRENT_STEAM_ENVIRONMENT.md` §5.2).

## Other presentation code

- `runtime/window.m`: a lazy main-thread pump used by every process, and an
  NSWindow + `CAMetalLayer` path (private calls `0x4C580010`/`11`) used only
  by `tests/elf/vk_present.c` and `vk_triangle.c` (stage 4).
- The native arm64 client uses `DISPLAY=:2`. Its webhelper's GPU process
  loaded Mesa's software Vulkan (lavapipe), not the shim
  (`docs/STEAMWEBHELPER_BRINGUP.md`), and cannot initialise GL on this X
  server, so the webhelper composites in software; its "Sign in to Steam"
  window is an ordinary X toplevel, 700x440, class `steamwebhelper`
  (MEASURED, stages 22-23). How its frames reach the X server in detail is
  not recorded (UNKNOWN). On the Steam Frame root the client's own GL
  windows use indirect GLX through `+iglx` (`benchmarks/stage23-frame-root.txt`).

## V-Sync

The setting, and the VKD3D-Proton value that was ignored until `7ad4919`,
are in `docs/GRAPHICS_BACKEND_ARCHITECTURE.md`. On a Metal surface only
FIFO and IMMEDIATE exist. Stage 22 recorded IMMEDIATE runs of
`vk_x11_present` through the cross-process layer on MoltenVK and
KosmicKrisp: the median stayed at the 165 Hz refresh (6.04-6.07 ms), the
mean dropped below it in some runs (MEASURED,
`benchmarks/stage22-kosmickrisp.txt` §6). Whether IMMEDIATE unlocks or
tears there is UNKNOWN: that probe waits for its fence every frame.
