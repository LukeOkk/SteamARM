# SteamARM launcher — specification (phase A)

A native macOS app "SteamARM" (SwiftUI, arm64, macOS 14+, bundle id
com.steamarm.launcher) that runs Linux apps through the SteamARM runtime (no VM),
styled like Ryujinx / GameHub. UI text in Spanish.

## Context
The launch path below predates the ARM64-first dispatch: `aarch64` entries
now run through `scripts/run-native.sh`, without FEX. The current lifecycle is
in `docs/APPLICATION_MANAGER.md`.

- `build/lxrun` = Linux-compat runtime; `scripts/run-fex.sh <guest-program> [args]`
  runs an x86 Linux program (env LXRT_ROOT = guest root; the Steam root is
  /tmp/lxrt-steamroot -> ~/SteamARM-roots/steamroot; guest /tmp/fexhome is $HOME;
  FEX_ROOTFS=/ for that root).
- Display modes (`STEAMARM_DISPLAY` for scripts, "display" in settings.json):
  - `native` (default): the native rootless X server (own XQuartz build,
    SteamARM-X11.app, bundle id org.steamarm.X11) managed by
    `scripts/run-x11-native.sh start|stop|restart|status [:2]`, display :2;
    every X window is a real macOS window. It is a host process, not lxrun.
  - `vnc`: Xvnc :1 (VNC 127.0.0.1:5901, password in
    ~/SteamARM-roots/vncpasswd.txt) shown through Apple Screen Sharing
    (`open vnc://127.0.0.1:5901`).
- `scripts/run-steam.sh` starts the X server of the chosen mode and Steam (and
  Screen Sharing in vnc mode). `--stop` kills guest processes (every
  `build/lxrun` process except Xvnc and FEXServer).
- Steam guest command: `/bin/bash /tmp/fexhome/.local/share/Steam/steam.sh -noverifyfiles`
  with DISPLAY=:2 (native) or :1 (vnc) LXRT_ROOT=/tmp/lxrt-steamroot FEX_ROOTFS=/.
  LXRT_GUEST_FAULTS=1 is no longer forced: `scripts/settings-env.py` sets it
  only while the "Informar fallos del invitado" setting is on (the default).
- Swift 6.4 / Xcode 27. SDL2 (sdl2-compat) at /opt/homebrew/opt/sdl2
  (include/SDL2, lib/libSDL2-2.0.0.dylib). unsquashfs and bsdtar available.
  Icon: resources/AppIcon.icns.
- Ryujinx config: ~/Library/Application Support/Ryujinx/Config.json,
  "input_config": [{backend "GamepadSDL2", id "<index>-<SDL GUID>", name,
  controller_type, player_index "Player1".., deadzone_left/right,
  range_left/right, trigger_threshold, left_joycon/right_joycon (button maps),
  left_joycon_stick/right_joycon_stick, motion, rumble, led, version}].

## 1. Home / library
- Built-in card Steam. User apps in ~/SteamARM-roots/launcher/apps.json
  (id, name, icon, guest command array, guest root, env, kind).
- One app at a time. Click: run `scripts/run-app.sh <id>`; show "Ejecutando
  <name>" with "Detener" and "Mostrar pantalla"; other launches disabled.
  "Mostrar pantalla": native mode activates the X server app
  (`tell application id "org.steamarm.X11" to activate`); vnc mode opens
  vnc://127.0.0.1:5901. The mode is the one the app was started with
  (~/SteamARM-roots/launcher/running.display), not the current setting.
  Poll every 2 s: a session run through scripts/session.py ends when its
  wrapper has exited and written running.status (or, if the wrapper was
  killed, when the program's process group is empty); what it left behind in
  its session (the native client's orphaned webhelper zygotes) is stopped by
  `run-app.sh --reap`. Only a run adopted without a wrapper (scripts/
  run-steam.sh) still waits until no guest `build/lxrun` other than
  Xvnc/FEXServer is left (ApplicationCore.swift, SessionLiveness). Then return
  home automatically; in
  vnc mode also quit Screen Sharing if the launcher opened it and it was not
  running before (`osascript -e 'quit app "Screen Sharing"'`).
- Per-app context menu: Editar, Eliminar (confirm; delete install dir).

## 2. scripts/run-app.sh <app-id> | --stop
- Like run-steam.sh (may factor shared display logic into scripts/lib-display.sh
  without changing run-steam.sh behaviour). Mode = STEAMARM_DISPLAY, else
  settings "display", else native. Native: `scripts/run-x11-native.sh start :2`,
  DISPLAY=:2, no Xvnc, no Screen Sharing. Vnc: ensure Xvnc (at "resolution"),
  DISPLAY=:1, then `open vnc://127.0.0.1:5901`. Start the app's command from
  apps.json (id "steam" = Steam command) in the background through
  scripts/run-fex.sh with its LXRT_ROOT/FEX_ROOTFS/env; PID to
  ~/SteamARM-roots/launcher/running.pid, id to running.id, mode to
  running.display; log ~/SteamARM-roots/logs/<id>-<time>.log. Export extra env
  from settings. `--dry-run` prints the mode and DISPLAY.
  Use `pgrep -f 'build/lxrun ...'` patterns (a bare pattern matches the caller's
  own shell). `--stop` = run-steam.sh --stop logic.

## 3. "Añadir app"
- Heroic Games Launcher: GitHub releases API
  Heroic-Games-Launcher/HeroicGamesLauncher latest, Linux x64 .tar.xz asset;
  extract to ~/SteamARM-roots/steamroot/opt/apps/heroic/ (guest /opt/apps/heroic);
  command ["/opt/apps/heroic/<dir>/heroic","--no-sandbox"].
- Minecraft Java (Prism Launcher): PrismLauncher/PrismLauncher latest Linux
  x86_64 AppImage; extract without running (squashfs offset = ELF end:
  e_shoff + e_shentsize*e_shnum, or first "hsqs" after the headers;
  `unsquashfs -o <offset> -d <dest> <file>`); install to .../opt/apps/prismlauncher/;
  command = its AppRun (guest path).
- Personalizada: pick .AppImage / .tar.gz/.tar.xz/.tgz / .deb / ELF; extract
  (deb: `bsdtar -xOf f 'data.tar*' | bsdtar -xf - -C dest`); user picks the
  executable (ELF or +x files), name, optional icon (png/svg near a .desktop).
- Progress and errors shown in the sheet. Installs under
  ~/SteamARM-roots/steamroot/opt/apps/<id>/.

## 4. "Ajustes" (~/SteamARM-roots/launcher/settings.json)
- Gráficos: MetalHUD toggle -> MTL_HUD_ENABLED=1 for launched apps
  (note: "Se aplica a apps que renderizan con Metal/MoltenVK").
- Pantalla: "Modo de pantalla" = "Ventanas nativas (recomendado)" (native) /
  "VNC (Compartir Pantalla)" (vnc), saved as "display". X resolution
  1280x720 / 1600x900 / 1920x1080 / 2560x1440 (vnc only, picker disabled
  otherwise; applied when Xvnc is (re)started with no app running). The
  Screen Sharing address/password section is shown only in vnc mode.
- Mandos (like Ryujinx input settings): tabs Jugador 1–4; device picker
  (SDL2 game controllers + "Desactivado"); type (Pro Controller / Xbox);
  mapping grid (A B X Y, D-pad, L R ZL ZR, - +, L3 R3, sticks) assigned by
  clicking a slot then pressing a button; deadzone sliders, trigger threshold,
  rumble toggle; live test view. "Importar de Ryujinx" maps Ryujinx
  input_config by SDL GUID. Save ~/SteamARM-roots/launcher/controllers.json.
  Detection via SDL2 game-controller API (same database Ryujinx uses), from
  Swift through a C shim / bridging header; pump SDL on a main-thread timer.
  Delivery to Linux apps is phase B (not here).
- Acerca de: version; everything runs without a VM.

## Build
- `make launcher` -> build/SteamARM.app (MacOS/SteamARM, Info.plist,
  Resources/AppIcon.icns, Frameworks/libSDL2-2.0.0.dylib with rpath fixed),
  swiftc -O -target arm64-apple-macos14 -parse-as-library launcher/*.swift,
  ad-hoc codesign. Project dir embedded at build time (Info.plist key
  SteamARMProjectDir) and overridable in settings.

## Rules
- Only write launcher/, scripts/run-app.sh (+ optional scripts/lib-display.sh),
  and the Makefile target. No personal names/emails/absolute home paths in
  sources (use ~ / $HOME). Never launch Steam, games, or Dead Cells; never touch
  /Applications/KytyPS5.app or the CrossOver bottle.
