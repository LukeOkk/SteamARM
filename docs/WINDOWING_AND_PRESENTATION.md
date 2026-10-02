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
- **Stacking**: the frame is put in front by the window manager's restack on
  a click or a title-bar drag, on Xplugin's own thread, and the overlay could
  only be ordered above it afterwards: the X window showed for a few
  milliseconds (white; "everything goes white when I shoot"), and when the
  re-ordering was missed the layer stayed hidden and got no drawable
  (MEASURED, `benchmarks/stage53-fullscreen-layer-order.txt`). Since
  `patches/xquartz-remote-order.patch` the overlay is one window level above
  its frame while the X server is the active application and no other X
  toplevel is stacked over the layer, and an overlay found below its frame
  is put back every 100 ms. A window-server ordering group did not hold the
  order and hid frames (MEASURED, same file).
- **X drawing under the overlay is hidden** while the property is set, and
  XShape is ignored.

## Fullscreen

MEASURED, `benchmarks/stage53-fullscreen-layer-order.txt`:

- The rootless X screen is the whole display (1920x1080 on the test
  display, not 1920x1050): XQuartz leaves out the strip under the menu bar,
  SteamARM's server does not (`patches/xquartz-remote-order.patch`;
  `STEAMARM_X11_FULL_DISPLAY=0` restores it). The window manager places
  ordinary windows in `NSScreen`'s visible frame as before.
- quartz-wm's fullscreen is the whole head
  (`patches/quartz-wm-fullscreen-head.patch`), and a borderless window of
  the display's size at 0,0 is left there.
- While a Vulkan layer covers a display, the server hides the menu bar and
  the Dock (`NSApplicationPresentationHideDock | HideMenuBar`); they return
  when another application is activated or the layer goes.
- X windows without a layer (indirect GLX) get the geometry but not the
  hidden menu bar. UNKNOWN: Mission Control and Stage Manager with a
  fullscreen layer.

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
`benchmarks/stage22-kosmickrisp.txt` §6). The layer is shown by another
process and gives up one drawable per refresh whatever
`displaySyncEnabled` says, and `CAMetalLayer` has no acquire that does not
block: a frame a little longer than a refresh waited for the second one
(Counter-Strike 2 stopped at 82.5 fps on the 165 Hz display).

So a swapchain created with IMMEDIATE or MAILBOX is the shim's
(`shim/mailbox.c`): the game draws into images of the shim's own and never
waits; a thread acquires the driver's image; a present copies the game's
image into it if one is ready and drops the frame if not (no tearing).
`vk_x11_present` with IMMEDIATE on KosmicKrisp: 0.33 ms per frame instead
of 6.06 (MEASURED, `benchmarks/stage52-mailbox-present.txt`;
`tests/elf/run_vk_arm64.sh`). `LXRT_VK_MAILBOX=0` leaves such swapchains to
the driver; FIFO swapchains and scaled ones are not touched.

A FIFO swapchain of the game's (V-Sync on) goes through the mailbox too
since stage 53, with no frame ever dropped: the game waits for a driver
image, one of which comes back per refresh, so a fast game is held to the
display's rate and a slow one shows every frame on a refresh. The driver's
own FIFO made a GPU-bound game wait at every acquire until the frame before
had been shown: Counter-Strike 2 at its highest settings 23.5 frames a
second against 35 through the mailbox (MEASURED, stage 53).
`LXRT_VK_MAILBOX=immediate` leaves FIFO swapchains to the driver.

A swapchain replaced while the mailbox holds an acquired image (always,
one ahead) lost a drawable for good on KosmicKrisp, which never released
its retain on a drawable presented or acquired: after two remakes the
layer had none left and the game's window froze (MEASURED, stage 54;
`patches/kosmickrisp-07-release-acquired-drawable.patch`,
`tests/elf/vk_x11_modes.c`).

The driver's swapchain under the mailbox is FIFO with three images since
stage 53: with the game's IMMEDIATE, a layer covering the display was
flipped outside the display's refreshes (times between shown frames
29.7-36.3 ms; now whole refreshes). A frame that finds no driver image is
dropped only when the one before was shown less than 13 ms ago; a slower
game waits for the image (3.5 ms on average at 33 fps) and has every frame
shown. `LXRT_VK_MAILBOX_REAL=immediate` restores the old presents.
`LXRT_VK_PROBE=1|2` prints the mean colour of eight zones of the game's
image (2: only black, white or jumping frames); KosmicKrisp's
`KK_PRESENT_LOG=1` prints, per present, when the GPU finished and when the
display showed it (`patches/kosmickrisp-06-present-timing.patch`).
