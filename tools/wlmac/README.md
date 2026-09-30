# steamarm-wlmac

A Wayland compositor that is a native macOS program: every Wayland toplevel
(`xdg_toplevel`) is a macOS window, and each surface and subsurface is a
CALayer. No libwayland, no X server, no VM: the wire protocol is implemented
in `wlmac.m` (Objective-C, AppKit and QuartzCore only).

It is an alternative to Weston for Android sessions
(`ANDROID_SESSION_COMPOSITOR=wlmac scripts/android-session.py launch <pkg>`):
Waydroid's composer (`hwcomposer.waydroid`, a Wayland client in the Android
root) connects to it instead of to Weston under lxrun on SteamARM's X server.

## What it serves

`wl_compositor` 4, `wl_subcompositor` 1, `wl_shm` 1 (ARGB8888, XRGB8888,
ABGR8888, XBGR8888), `wl_output` 3 (the main screen), `wl_seat` 5 (pointer
and keyboard: Linux button and key codes, an evdev/pc105/us XKB keymap in
`us.xkb`, compiled from `keymap-source.xkb`), `xdg_wm_base` 2 and
`wp_viewporter` 1. Not yet: clipboard (`wl_data_device_manager`), touch,
tablets, `wp_presentation`, dmabuf.

Each commit copies the shm buffer into a CGImage (the buffer is released at
once). Frame callbacks fire at the display's refresh.

## Run it

    scripts/run-wlmac.sh start [--dump-dir DIR] [--verbose] [--selftest-input]
    scripts/run-wlmac.sh stop | status

`WLMAC_XDG` (a guest path under `/dev/shm`, default
`/dev/shm/steamarm-wlmac`) and `WLMAC_SOCKET` (`wayland-0`) name the socket as
guests see it; lxrun maps `/dev/shm` to `/tmp/lxrt-shm-<uid>`, where the
socket is. `--dump-dir` writes each window, as composed, to a PNG at most
once a second (rendered offscreen: no screen capture, so no macOS privacy
prompt). `--verbose` logs every request.

## Measured (stage 29, `benchmarks/stage29-android-input-network.txt`)

- `tests/wlmac/run.sh`: an aarch64 client under lxrun
  (`tests/android/wl_shm_client.c`) draws 60 frames at 59-60 fps into its own
  macOS window, whose dump has the client's four colours; a host client gets
  `wl_pointer` enter, motion, BTN_LEFT and `wl_keyboard` KEY_A from the
  compositor's input path.
- Android 11 (x86-64 under FEX) boots against it to `sys.boot_completed=1`
  in about 20 s and Simple Solitaire has the focus; the window (1920x974,
  the screen) shows Android's desktop mode with the app in a freeform window.
  Idle: 0.1% CPU.
- Not yet: one macOS window per Android app. With
  `persist.waydroid.multi_windows=true` the composer still puts everything
  in one "Waydroid" window: it sets `waydroid.active_apps=Waydroid` itself
  when it starts (its service passes `--desktop_file_hint=Waydroid.desktop`),
  and setting it to `none` afterwards created no other window (UNKNOWN why:
  its per-task windows need layer names from Waydroid's patched framework).
