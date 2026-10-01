# Using SteamARM

The launcher (**SteamARM.app**) starts Steam and other Linux programs and
holds every setting. Its interface is in Spanish, and its settings window
follows the layout of the [Ryujinx](https://ryujinx.app) emulator's.

## The launcher

- **Steam** starts the Linux Steam client (the x86 client under FEX: the
  working route). Its windows are ordinary macOS windows (a rootless X
  server). The card shows **Detener** while it runs.
- **Steam ARM64 (experimental)** and **Steam ARM64 · Steam Frame
  (experimental)** start Valve's native arm64 client with no FEX, on two
  different ARM64 roots. It reaches its "Sign in to Steam" window
  (MEASURED, `benchmarks/stage23-*.txt`); sign-in, the library, downloads
  and games under it are not verified, and Proton ARM64 does not run on
  macOS. Neither root nor the client is set up by **Instalar**: each card
  is disabled, with the reason, until they exist
  (`docs/STEAM_ARM64_BRINGUP.md`). Opening one asks for confirmation first.
- **+** adds other Linux programs: a `.tar.gz`/`.zip`/AppImage you pick, or
  a known installer (Heroic, Prism Launcher). **Heroic Games Launcher
  (ARM64, experimental)** installs Heroic's linux-arm64 build into the ARM64
  root, where it runs without FEX: its window, pages and a clean close are
  measured; store sign-in, downloads and games are not, and Amazon Games
  does not work yet (`docs/HEROIC_INTEGRATION.md`). The launcher reads the
  program's ELF header first: an aarch64 program goes to the ARM64 root
  and runs without FEX, anything else to the x86 root. Right-click a card
  to open it, see or change its settings (**Ajustes…**: an added app can
  override display, V-Sync, synchronization and the graphics engine; the
  built-in entries show theirs read-only), open its folder or logs, mark
  it as a favourite, or delete it (added apps only).
- **+ → Añadir APK (Android)** reads an Android `.apk` (name, icon, package,
  version, SDK levels, native code, permissions, signing schemes) and
  installs it into `~/SteamARM-roots/android/` (`scripts/android-pm.py`). An
  update keeps the app's data and is refused when the new APK is signed by
  another certificate or is an older version. **Abrir** starts SteamARM's
  Android session (Android 11 x86-64 under FEX, no VM,
  `scripts/android-session.py`): a Weston window on the native X server is
  Android's screen, the app is installed there with Android's own
  `pm install` and started; the first start takes a minute or two, later
  ones about half a minute. **Detener** ends the session. It runs apps with no native code and apps with x86-64 code; the
  card is disabled, and says why, for arm64-v8a-only code (Android's arm64
  runtime does not start on macOS), 32-bit ARM or x86 code and apps that
  need a newer Android than 11. No sound or network inside Android yet.
  XAPK, APKS, APKM and AAB bundles are refused for now. Right-click an
  Android card for **Información…**, **Abrir carpeta de datos** and
  **Desinstalar…** (with or without its data). Details:
  [APK_SUPPORT.md](APK_SUPPORT.md).
- The library has a search field and the filters Todas, Favoritas,
  Recientes, ARM64, x86, Windows and Android; each card shows its last
  launch, how many times it ran and for how long.
- Only one program runs at a time. Stopping it stops all its Linux
  processes. When a program ends with an error, the launcher shows its exit
  code or signal, or the memory guard's reason (see
  `docs/TROUBLESHOOTING.md`).
- Starting a program also starts the memory guard, sound and the controller
  service (below).

Settings changes apply to programs started **after** you apply them. Games
inherit Steam's environment, so restart Steam after changing a setting that
affects games (the settings window reminds you).

## Settings (Configuración)

| Section | What it controls |
|---|---|
| **Interfaz** | The **primary Steam** (Steam ARM64 · Steam Frame by default, then Steam ARM64 on the Fedora root, then the x86_64 client under FEX: when the chosen one cannot start on this Mac the next one is used, and Settings says which); start that Steam when the launcher opens; ask before stopping; the application backend: native windows or a VNC desktop, and its resolution (Lightning JIT and Apple Hypervisor are not listed: neither exists in this tree, and an option that can never be chosen is not shown; they would appear by themselves if they ever became usable. No choice uses a virtual machine); the source folder the launcher runs scripts from |
| **Entrada** | Controllers, one page per player (below) |
| **Sistema** | Language and time zone of Linux programs; V-Sync (AUTO / ON / OFF); **DRAM** and **VRAM** limits; Proton synchronization (AUTO / default / MSync / fsync / esync: MSync is disabled; fsync and esync are experimental, only an explicit choice turns them on, and each needs a Proton that has it); what to do when a setting cannot work for an app (AUTO, ESTRICTO, PREGUNTAR) |
| **Procesador** | FEX options: on-disk translation cache (experimental), x86 memory-ordering emulation (TSO full/fast/off), multiblock, self-modifying code detection, reduced-precision x87; the Proton and runtime tools installed |
| **Gráficos** | The graphics engine: AUTO (MoltenVK), Vulkan (MoltenVK), Vulkan (KosmicKrisp: experimental, offered only when Homebrew's Mesa is installed and the installed Vulkan shim can load it) or OpenGL (WineD3D: experimental, Wine's own Direct3D on OpenGL from Mesa's Zink over MoltenVK; offered when Zink is in the x86 root and the native X server was built with direct GLX by `scripts/setup.sh`). Shader cache, anisotropic filtering, **edge smoothing (MSAA 2x/4x/8x, forced by DXVK on Direct3D 9 games; Direct3D 10/11/12 games choose it in-game)**, frame-rate limit, DXVK HUD, Metal HUD |
| **Runtime** | Read-only: every backend and capability, its state and reason, and what was detected on this Mac |
| **Sonido** | Sound on/off and volume |
| **Atajos** | Keyboard shortcuts: screenshot (F8 by default, saved to `~/Pictures/SteamARM`), stop the running program, toggle the Metal HUD. Shortcuts while a game has focus need the Accessibility permission, and screenshots need Screen Recording |
| **Registros** | Proton log, `WINEDEBUG`, DXVK and VKD3D-Proton log levels; opens the logs folder (`~/SteamARM-roots/logs`) |
| **Depuración** | Runtime fault reports, runtime tracing, Vulkan debug, extra environment variables |

Options with no working backend on this stack are greyed out, with the
reason: FSR scaling (no installed Proton has its full-screen upscaler any
more, and gamescope does not exist on macOS; a lower in-game resolution is
the way), and MSync (a patch of macOS Wine, not in Linux Proton).

Each app's **Ajustes…** has its own display, vsync, synchronization and
graphics choices over the global ones, the built-in Steam entries included
(their choices are kept in the launcher's settings and also reach the games
that Steam starts).

Native Linux games that draw with OpenGL use Mesa's software renderer
unless told otherwise. Untested with games: their **Launch options** in
Steam can ask for the GPU through Mesa's Zink (OpenGL 4.5 on MoltenVK):
`GALLIUM_DRIVER=zink MESA_EXTENSION_OVERRIDE="+GL_ARB_vertex_type_2_10_10_10_rev +GL_ARB_texture_buffer_object_rgb32" %command%`.
Set them per game, not for all of Steam: the Steam client's own helpers
would use them too (it starts with them since the Vulkan thunk fix in
`benchmarks/stage39-wined3d-gl45.txt`, but nothing more was tried).

### Memory: DRAM and VRAM

Apple Silicon shares memory between the CPU and GPU, so both limits come
out of the same RAM. The maximum you can pick leaves room for macOS:
total − 2 GB up to 8 GB, total − 4 GB above that. So 8 GB → 6, 16 GB → 12,
32 GB → 28, 64 GB → 60. **Automático** uses that maximum.

- **DRAM** is the ceiling for everything SteamARM runs (Steam and the game
  together). The memory guard stops the Linux programs before the Mac runs
  out of memory, instead of letting macOS freeze.
- **VRAM** is the video memory reported to games: the Vulkan heap size seen
  by DXVK/VKD3D-Proton, and DXVK's `dxgi.maxDeviceMemory`.

## Controllers (Entrada)

Plug in or pair a controller with the Mac as usual (USB or Bluetooth). Then
in **Configuración → Entrada**:

1. **Jugador**: player 1–4.
2. **Dispositivo**: the physical controller for this player (the refresh
   button re-scans). With no controller chosen, the first unassigned one is
   used.
3. **Tipo de mando**: **what the game sees**. It is the identity (USB
   vendor/product IDs, name and button layout) the controller service
   presents to Linux and Proton, so games show the matching button prompts.
   It does not have to match the physical controller: a DualSense can be
   presented as an Xbox controller, and the other way round.

| Type | Game sees |
|---|---|
| Xbox 360 | 045e:028e |
| Xbox One | 045e:02ea |
| Xbox Series X\|S (default) | 045e:0b12 |
| Xbox Elite Series 2 | 045e:0b00, 4 paddles |
| DualShock 3 (PS3) | 054c:0268 |
| DualShock 4 (PS4) | 054c:09cc |
| DualSense (PS5) | 054c:0ce6 |
| DualSense Edge (PS5) | 054c:0df2 |
| Steam Controller (2015) | 28de:1102, trackpads, 2 grips |
| **Steam Controller (2026)** | 28de:1302, symmetric sticks, 2 trackpads, 4 rear grips (L4 R4 L5 R5), quick access button |
| Nintendo Switch Pro | 057e:2009 |

The drawing in the middle shows the chosen type. Pressed buttons light up
live, and the two stick positions are shown below it. **Click a button on
the drawing (or in the side lists) and press the control you want for it**
to remap it. The side panels mirror Ryujinx's:

- **Sticks**: invert X/Y, rotate 90°, dead zone and range.
- **Triggers**: threshold (for types with digital triggers).
- **Vibración**: on/off and strength, plus a test button.
- **LED**: colour (DualShock 4 / DualSense).

**Perfil** saves and loads named mappings. **Importar de Ryujinx** reads a
Ryujinx input configuration.

Good to know:

- **Xbox Series X|S** is the most compatible choice. Every Windows game
  supports XInput.
- With the **Nintendo Switch Pro** type, games use Nintendo's labels: the
  bottom face button is **B** and the right one is **A** (as with a real Pro
  Controller on Linux). With a non-Nintendo controller that means A and B
  appear swapped.
- The **Steam Controller** types present Valve's layouts. The trackpads'
  clicks and the rear grips are delivered; finger positions on the trackpads
  are not (no source for them on the Mac side yet).
- Motion sensors (gyro) are not delivered to games yet.

How it works: `steamarm-inputd` (a small native program, started with Steam)
reads the controllers through SDL and publishes each player as a Linux
`/dev/input/eventN` device that the runtime shows to Linux programs.
Proton's input layer (winebus/SDL) turns that into XInput/DirectInput/HID
for the game. Details: [tools/inputd/PROTOCOL.md](../tools/inputd/PROTOCOL.md).

## Sound

Linux programs play through a PulseAudio server running on the Mac, which
outputs to the current macOS output device (it follows the device you
choose in Control Center). Set the volume or mute in **Sonido**.

## Command line

Everything the launcher does is a script in the source folder
(`~/Library/Application Support/SteamARM/src` for the downloaded app, or
your checkout):

| Command | |
|---|---|
| `scripts/run-steam.sh` / `--stop` | start / stop Steam |
| `scripts/run-app.sh` | what the launcher runs for any program |
| `scripts/safeguard.sh status` | the memory guard |
| `scripts/audio.sh status` | sound |
| `scripts/input.sh status` | the controller service and its devices |
| `scripts/setup.sh <step>` | re-run one setup step (`--list`) |
| `tests/elf/run.sh`, `tests/win/run.sh`, `tests/win/run_steam_path.sh` | self-tests |
