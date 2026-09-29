# The FEX game boundary: x86 Proton under the native arm64 Steam client

The ARM64-first target is "Steam ARM64, FEX only for x86 game payloads"
(`docs/ARM64_FIRST_MIGRATION.md`, step 13). This page is that boundary as
built on 2026-09-29 (stage 24): a Steam compatibility tool for Valve's native
arm64 client, **SteamARM: Proton x86 via FEX (experimental)**, that hands a
Windows game to the x86 Proton and x86 Steam Linux Runtime which SteamARM's
x86 Steam root already has, run by SteamARM's FEX. No virtual machine is
involved at any point.

Labels: MEASURED (a command and its result, `benchmarks/stage24-fex-game-boundary.txt`),
VERIFIED IN SOURCE (file:line or function), UPSTREAM DOCUMENTED, HYPOTHESIS,
UNKNOWN.

**Status.**

- MEASURED: invoked the way Steam launches a game with a tool (the
  client's own reaper, as the x86 client does in stage 15; the command line
  of the tool's `toolmanifest.vdf`; Steam's `STEAM_COMPAT_*` environment;
  the game's directory as working directory), from an aarch64 shell under
  lxrun in the ARM64 root, the tool runs tests/win's D3D11 and D3D12 probes through
  x86 Proton Experimental and Steam Linux Runtime 4 at the same frame rate
  as the x86 client's own launch path (about 160 fps each), on the Fedora
  armroot and on the Steam Frame root. The tone probe's stream reaches
  PulseAudio.
- MEASURED: both native clients register the tool when they start, before
  any sign-in (`compat_log.txt`: "Registering tool steamarm-fex-proton").
- MEASURED, open: 10 of 29 probe launches through the tool rendered but
  did not exit (Xalia, Proton's gamepad-navigation helper, at 100 % CPU);
  1 of 10 on the x86 client's own path did not end either. See
  [Exit](#exit-a-session-that-does-not-end).
- Not done by this task: the tool in Settings > Compatibility, choosing it
  for a game and starting a game from the client's UI, which need a sign-in
  (not attempted here). No real game was run. The logs of a signed-in
  session outside this task show the client mapping a title to the tool and
  running the tool itself, into x86 Proton (MEASURED from those logs; see
  [A signed-in client](#a-signed-in-client-seen-in-its-logs)).
- Proton 10.0 (its runtime is Steam Linux Runtime 3.0, "sniper") does not
  start a probe under SteamARM's FEX, through the tool or through the x86
  client's own path (MEASURED). Only Proton Experimental is offered.

## The chain

```
 Steam client, steamrtarm64/steam                         aarch64, ARM64 root, no FEX
  └ steamrtarm64/reaper SteamLaunch AppId=<id> --         aarch64 (the client's reaper)
     └ compatibilitytools.d/steamarm-fex-proton/
       steamarm-fex-proton waitforexitandrun <game.exe>   aarch64 bash (the root's /bin/bash)
        │  checks the x86 side (one child with a bind table of its own)
        │  exec with LXRT_ROOT = the x86 Steam root, LXRT_MOUNTS = the client's paths
 ───────┼──────────────────────────────── the boundary: everything below is x86 code under FEX
        └ FEX-gb /bin/bash SteamLinuxRuntime_4/_v2-entry-point --verb=waitforexitandrun --
           └ pressure-vessel (its bwrap plan interpreted by runtime/mounts.c) → pv-adverb
              └ Proton - Experimental/proton waitforexitandrun <game.exe>   (Python)
                 └ wine → steam.exe → the game; DXVK / VKD3D-Proton
                    → Vulkan thunks → shim/libvulkan.so.1 → MoltenVK → Metal
```

The x86 client's own path, for comparison (stage 15,
`tests/win/run_steam_path.sh`): `reaper → SteamLinuxRuntime_4/_v2-entry-point
→ pressure-vessel → proton → wine`, all under FEX, started by an x86 client
that itself runs under FEX. The tool keeps everything from `_v2-entry-point`
down exactly as the x86 client builds it, and puts an ARM64 client and an
aarch64 script above it.

## What is ARM64 and what is x86

| part | ISA | runs as | class | evidence |
|---|---|---|---|---|
| Steam client, webhelper | aarch64 | directly under lxrun | ARM64 | MEASURED (stages 22-23) |
| reaper (`steamrtarm64/reaper`) | aarch64 | directly under lxrun | ARM64 | MEASURED: `[lxrt] /tmp/lxrt-armroot/tmp/armhome/.local/share/Steam/steamrtarm64/reaper: PIE` |
| the tool script and its check shell | aarch64 bash of the ARM64 root | directly under lxrun | ARM64 | MEASURED: `[lxrt] /tmp/lxrt-armroot/bin/bash: PIE` |
| lxrun | arm64 Mach-O | macOS process | ARM64 | |
| FEX, FEXServer | aarch64 Linux | under lxrun | ARM64 (the translator) | MEASURED: `/tmp/lxrt-root/usr/bin/FEX-gb` outside the container, `/usr/lib/lxrt-emu/FEX` inside |
| Steam Linux Runtime 4 (pressure-vessel, the container's libraries) | x86-64 | under FEX | TRANSITIONAL: SLR 4 arm64's pressure-vessel under lxrun is step 12, not done | MEASURED |
| Proton Experimental (script, Wine, DXVK, VKD3D-Proton) | x86-64 (Python and Wine under FEX) | under FEX | TRANSITIONAL: Proton ARM64 cannot start on macOS (0x7ffe0000 in the low 4 GiB, `benchmarks/stage18-settings-audio-controllers.txt`, stage 19 §2) | MEASURED |
| the game | x86-64 / i386 | under FEX | game payload | MEASURED with probes only |
| Vulkan below the thunks: shim, MoltenVK | aarch64 Linux / arm64 Mach-O | host side of the thunk | ARM64 | stage 10 |

In one D3D11 launch through the tool, lxrun started 3 aarch64 programs of the
ARM64 root (the test's launch shell, the reaper, the tool), 1 check shell
whose trace is discarded, and 53 x86 programs through FEX: 24 outside the
container (`FEX-gb`) and 29 inside it (`/usr/lib/lxrt-emu/FEX`) (MEASURED,
the `[lxrt] ...: PIE` lines of the run's log).

## How the ARM64 root reaches the x86 world

The client runs with `LXRT_ROOT=/tmp/lxrt-armroot` (or `/tmp/lxrt-arm64root`);
x86 Proton, its runtime and FEX's x86 root file system live in the x86 Steam
root (`/tmp/lxrt-steamroot`, `~/SteamARM-roots/steamroot`). Nothing is copied
between them and the x86 root is not modified.

1. **The root switch.** A guest's `execve` re-runs lxrun on the program with
   the environment the guest passes (`lxrt_execve`, `runtime/process.c`), and
   the new runtime resolves the program in the `LXRT_ROOT` of that
   environment, falling back to the host path (`resolve_program`,
   `runtime/main.c`) (VERIFIED IN SOURCE). So the tool exports
   `LXRT_ROOT=/tmp/lxrt-steamroot` and execs `/tmp/lxrt-root/usr/bin/FEX-gb`,
   exactly what `scripts/run-fex.sh` does for the x86 client's games, with
   `FEX_ROOTFS=/` (the x86 root is `/`) and `HOME=/tmp/fexhome` (FEX's
   configuration: the Vulkan thunks, `HideHypervisorBit`'s per-program
   settings). MEASURED: `uname -m` answers `aarch64` in the ARM64 root and
   `x86_64` after the switch.
2. **The client's paths.** The game, its prefix (`compatdata`) and its shader
   cache are in the arm64 client's library, `/tmp/armhome/...`, a path that
   exists only in the ARM64 root. The tool passes a bind table,
   `LXRT_MOUNTS`, that every lxrun process reads at start-up
   (`lxrt_mounts_load_env`, `runtime/mounts.c`: records
   `dst \x1e src \x1e ro`, separated by `\x1f`) and that the bwrap
   interpreter carries into the container: pressure-vessel's own binds of
   `STEAM_COMPAT_*` paths resolve through it (`guest_to_host`), and the table
   is handed on with the sandbox (`serialize`) (VERIFIED IN SOURCE). What is
   bound, each to `<ARM64 root><path>`: the client's `HOME`, then
   `STEAM_COMPAT_CLIENT_INSTALL_PATH`, each of `STEAM_COMPAT_LIBRARY_PATHS`,
   `STEAM_COMPAT_INSTALL_PATH`, `STEAM_COMPAT_DATA_PATH`,
   `STEAM_COMPAT_SHADER_PATH` and the working directory, when not already
   under a bound path. Never bound: `/`, `/tmp`, `/usr`, `/lib*`, `/bin`,
   `/sbin`, `/etc`, `/proc`, `/sys`, `/dev`, `/run`, `/var`, `/opt`, `/home`,
   `/root` and the x86 side's own home, which would hide the x86 root's
   files. MEASURED: `Proton: Upgrading prefix from None to 11.0-100
   (/tmp/armhome/.local/share/Steam/steamapps/compatdata/999999/)`, and the
   probe's `probe-result.txt` written in the game's directory in the ARM64
   root.
3. **Checks before the switch.** The ARM64 root cannot see host paths. The
   tool starts one child `/bin/bash` with a bind table of its own
   (`/run/steamarm-x86` → the x86 root, `/run/steamarm-fex` → FEX's
   directory, `/run/steamarm-fexserver` → `/tmp/lxrt-abstract-<uid>`) and
   tests: Proton's `proton` and `files/bin/wine`, no `files/bin-arm64`, the
   runtime's `_v2-entry-point`, FEX, and FEXServer's socket. A missing piece
   ends the launch with its reason and an exit status (4 missing, 5 no
   FEXServer, 6 refused). MEASURED: a missing FEX and a missing runtime
   gave exit 4 and their messages in 1 s from the real ARM64 root; the
   tool's own work costs about 50 ms (0.09 s against 0.04 s for
   `/bin/true` under `scripts/run-native.sh`).
4. **The environment.**

   | variable | to the x86 side | why |
   |---|---|---|
   | `LXRT_ROOT`, `LXRT_MOUNTS` | the x86 root; the client's paths | above |
   | `FEX_ROOTFS`, `HOME`, `TMPDIR`, `FEX_GUESTBASE`, `FEX_SMCCHECKS`, `FEX_OUTPUTLOG`, `PATH`, `OBJC_DISABLE_INITIALIZE_FORK_SAFETY` | as `scripts/run-fex.sh` sets them | the x86 client's games get the same |
   | `STEAM_COMPAT_TOOL_PATHS` | x86 Proton and runtime | what the x86 client passes; the tool's own directory holds only an aarch64 script |
   | `STEAM_COMPAT_CLIENT_INSTALL_PATH`, `_DATA_PATH`, `_INSTALL_PATH`, `_LIBRARY_PATHS`, `_SHADER_PATH`, `SteamAppId`, `SteamGameId`, `STEAMARM_RUN_ID` | kept | Proton's contract; `STEAMARM_RUN_ID` lets `scripts/run-steam-arm64.sh` find the session's processes |
   | `DISPLAY`, `PULSE_SERVER` | kept; default `:2` and `unix:/tmp/pulse/native` | the X server is the host's; the PulseAudio socket lives in the x86 root (`scripts/audio.sh`) |
   | `LXRT_GUEST_PAGE`, `LXRT_X18_ALL_TEXT`, `LD_LIBRARY_PATH`, `STEAM_RUNTIME_LIBRARY_PATH` | removed | the client's settings for itself (4 KiB guest pages, libcef's x18 pass, aarch64 libraries) |
   | `STEAM_COMPAT_EMULATOR`, `PRESSURE_VESSEL_EMULATOR`, `STEAM_COMPAT_MACHINE_ARCHITECTURE`, `STEAM_COMPAT_GRAPHICS_PROVIDER` | removed | they would send pressure-vessel to Valve's FEX (3127680), which lacks SteamARM's patches (`docs/STEAM_FRAME_COMPAT_TOOLS.md` §7.3) |
   | `LD_PRELOAD` | removed unless `STEAMARM_FEX_KEEP_LD_PRELOAD=1` | the client's overlay libraries; whether they are x86 is UNKNOWN (no launch from the UI) |
   | the names in the ARM64 root's `.lxrt-guest-env` | removed | that root's own needs: the Steam Frame root's indirect GLX (`LIBGL_ALWAYS_INDIRECT=1` and two more, `scripts/mkframeroot.sh`) is not the x86 root's. MEASURED on the Frame root: "not passing on the ARM64 root's own guest environment: __GLX_VENDOR_LIBRARY_NAME MESA_LOADER_DRIVER_OVERRIDE LIBGL_ALWAYS_INDIRECT" |
5. **FEXServer.** One FEXServer serves every FEX program of the user and
   hands each the rootfs it was started with; started from a game's
   environment it breaks later x86 programs (`scripts/run-fex.sh`). The tool
   therefore requires a running one (`scripts/run-fex.sh` starts it, as does
   the launcher's x86 Steam entry) and refuses to start FEX without its
   socket (exit 5). That refusal was not exercised: the shared FEXServer ran
   throughout (VERIFIED IN SOURCE only).

## Install

```sh
scripts/install-fex-proton-tool.sh                      # the Fedora armroot's client
scripts/install-fex-proton-tool.sh --root arm64root     # the Steam Frame root's client
scripts/install-fex-proton-tool.sh --uninstall [--root ...]
```

It writes only `<root>/tmp/armhome/.local/share/Steam/compatibilitytools.d/steamarm-fex-proton/`:

| file | content |
|---|---|
| `compatibilitytool.vdf` | `steamarm-fex-proton`, `install_path "."`, display name "SteamARM: Proton x86 via FEX (experimental)", `from_oslist "windows"`, `to_oslist "linux"` (Proton's own template's keys, `docs/STEAM_FRAME_COMPAT_TOOLS.md` §2.4) |
| `toolmanifest.vdf` | `version 2`, `commandline "/steamarm-fex-proton %verb%"`, `compatmanager_layer_name "proton"`; no `require_tool_appid`, so the client wraps it in none of its own (arm64) runtimes |
| `steamarm-fex-proton` | the aarch64 script (`tools/steamarm-fex-proton/`) |
| `steamarm-fex-proton.conf` | the x86 root, its home and Steam directory, Proton, the runtime its `toolmanifest.vdf` requires, FEX's directory and binary |

It checks, on the host, that the chosen Proton is installed in the x86 root,
that its Wine loader is x86 (ELF machine 62 or 3), that it is not an ARM64
build, and that the runtime it requires is installed. Only `Proton -
Experimental` is installed without `--unverified`: Proton 10.0 is measured
not to start, and any other Proton is not measured. The client reads
`compatibilitytools.d` when it starts, so a running client needs a restart
(MEASURED at start; whether it rescans later is UNKNOWN).

`tests/launcher/fex_proton_tool.sh` (in `make test-launcher-core`) runs the
installer and the tool's dry run (`STEAMARM_FEX_PROTON_DRYRUN=1`) on fake
roots; `tests/win/run_fex_boundary.sh [d3d11] [d3d12] [tone]` is the
measurement on the Mac, and `tests/win/run_steam_path.sh d3d11 d3d12` the
x86 client's path to compare with.

## Evidence (stage 24)

All MEASURED on the Mac (Mac mini M4, 16 GB, macOS 27.0), with no Steam UI,
no sign-in and no game; details and log lines in
`benchmarks/stage24-fex-game-boundary.txt`.

| launch | root | D3D11 fps (launches) | D3D12 fps (launches) | tone | exited | time to exit |
|---|---|---|---|---|---|---|
| the tool | Fedora armroot | 158.4-160.8 (15) | 159.7-161.1 (6) | stream 2 of 2 | 15 of 23 | 51-57 s (63-67 s while other tests ran) |
| the tool | Steam Frame root | 159.8-160.4 (3) | 152.6-161.3 (3) | | 4 of 6 | 54-56 s |
| the x86 client's path (`tests/win/run_steam_path.sh`) | x86 Steam root | 160.2-160.7 (4), 178.8 (1), no frame (1) | 159.6-161.2 (3) | stream 1 of 1 | 9 of 10 | 48-57 s |

Proton Experimental and Steam Linux Runtime 4 in every row; each probe
presents 600 frames. "The tool" is the client's launch line: its reaper,
the tool, `waitforexitandrun`; 2 of the Fedora launches were without the
reaper. The frame rate through the tool matches the x86 client's path
within about 1 fps; the single 178.8 fps x86 run and the 152.6 fps Frame
run are unexplained. The tool's launches took about 3-5 s longer to exit
(cause UNKNOWN; the tool's own checks are 50 ms of it). "Exited" counts
launches not killed from outside; the ones that did not exit are the next
section.

Registration, with the tool installed and the client started by
`scripts/run-steam-arm64.sh --for 90` (no sign-in):

```
[2026-09-29 14:26:05] Processing local tool list at /tmp/armhome/.local/share/Steam/steamrtarm64/../compatibilitytools.d/steamarm-fex-proton/compatibilitytool.vdf...
[2026-09-29 14:26:05] Registering tool steamarm-fex-proton, AppID 0
[2026-09-29 14:26:05] Loaded manifest for tool steamarm-fex-proton.
```

(Fedora armroot, 4 s after start; the sign-in window at 14:26:12. The Frame
root logged the same three lines at 14:20:19, its sign-in window at
14:20:34.) No "Ignoring tool ... for a different target platform" line,
which the client has for tools it rejects (VERIFIED IN SOURCE: the string in
`steamrtarm64/steamclient.so`). The same `compat_log.txt` still maps AppID 0
(every title without its own mapping) to `proton_experimental` (Fedora root)
and `proton-stable` (Frame root): which builds those names mean under an
arm64 client is UNKNOWN, and the ARM64 Protons it has installed cannot start
on macOS.

### A signed-in client, seen in its logs

From 15:20:35 to 15:22:08 the same day, a session outside this task started
the launcher's **Steam ARM64 · Steam Frame (experimental)** entry, with the
tool installed in that root, and the client got past sign-in
(`compat_log.txt`: "Waiting for compat in post-logon"). This task did not
sign in and started no game; it read the logs afterwards (MEASURED from the
logs):

```
[2026-09-29 15:21:23] Posting queued tool registration callback 0 steamarm-fex-proton
[2026-09-29 15:21:23] Command prefix for tool 0 "SteamARM: Proton x86 via FEX (experimental)" set to: "'/tmp/armhome/.local/share/Steam/compatibilitytools.d/steamarm-fex-proton'/steamarm-fex-proton run ".
[2026-09-29 15:21:23] DEBUG: setting env var: STEAM_COMPAT_FEX_CONFIG = 
[2026-09-29 15:21:49] Mapping AppID 3164500 to tool "steamarm-fex-proton" with priority 250
[2026-09-29 15:22:08] Shutting down: forcing platform override cache flush to disk.
```

The launcher's log of that session has the tool's own line twice
(`steamarm-fex-proton: run via x86 Proton - Experimental +
SteamLinuxRuntime_4 under FEX (root /tmp/lxrt-steamroot); client paths bound
from /tmp/lxrt-arm64root: /tmp/armhome`), started by a `/bin/sh` of the
root, then `FEX-gb`, pressure-vessel and `Proton: Upgrading prefix from None
to 11.0-100 (/tmp/armhome/.local/share/Steam/steamapps/compatdata/0/)`; the
session ended at 15:22:08 with status 137, signal 9. So a signed-in arm64
client knows the tool by its display name, builds its command from
`toolmanifest.vdf`, had a title mapped to it (priority 250; that this is a
choice made in the title's Properties > Compatibility is a HYPOTHESIS), and
ran it with the verb `run` into x86 Proton. Which program that run was for
(the prefix is `compatdata/0`, not the title's) and what came of it are
UNKNOWN: the tool's line did not yet name the program (it does now), and
the session was stopped 3 s after the prefix was made. The client also sets
`STEAM_COMPAT_FEX_CONFIG` (empty) for the tool; the tool passes it on and
SteamARM's FEX does not read it (HYPOTHESIS).

## Exit: a session that does not end

MEASURED, not solved. Of 29 probe launches through the tool with Proton
Experimental that were not killed from outside, all 29 rendered (or played,
for tone), 19 exited in 51-67 s, and 10 did not exit within the 150-300 s
limit. The 10 came in three stretches of time (14:36-15:02, 15:09-15:19 and
15:30-15:35), on both roots, with and without the reaper; the launches
before, between and after them exited, including 4 of 4 from 15:37 to 15:41.
In the 6 that were inspected, what kept the session alive was `wineserver`,
Wine's system programs, `steam.exe` (5 of them) and `xalia.exe`, Proton's
gamepad-navigation helper, which Proton Experimental starts for every title
unless it is on the `noxalia` list (`PROTON_USE_XALIA=1`, VERIFIED IN SOURCE,
Proton's `proton` script); `xalia.exe` was at 100 % CPU. One launch with
`PROTON_USE_XALIA=0` exited normally (52 s). On the x86 client's own path, 9
of 10 launches exited (48-57 s) and 1 (15:23) neither rendered nor exited in
152 s; its 2 launches at 15:35, just after 2 launches through the tool had
not exited, both exited. Switching the tool's `STEAM_COMPAT_CLIENT_INSTALL_PATH`
between the arm64 client's directory and the x86 client's changed nothing
(4 of 4 exited, alternated). Other sessions were starting and stopping
guests on the same Mac throughout (6 of this stage's launches were killed
from outside, rc 137). The cause is UNKNOWN: the tool's path hung more often
(10 of 29 against 1 of 10), but no difference between the two paths has
been shown to cause it. For a game started from Steam this would show as a game still
running after its window closed; Steam's Stop, or `PROTON_USE_XALIA=0
%command%` in the game's launch options, are the workarounds to try (the
second MEASURED once).

## What remains

- **A game from the UI (the user).** With the tool installed, start
  **Steam ARM64 (experimental)** or **Steam ARM64 · Steam Frame
  (experimental)**, sign in, and check: Settings > Compatibility lists
  "SteamARM: Proton x86 via FEX (experimental)"; a Windows title's
  Properties > Compatibility can be forced to it; the title starts, and the
  launcher's log for the client shows `steamarm-fex-proton:
  waitforexitandrun <the game's exe> (app <id>) via x86 Proton -
  Experimental`. The session above shows the mapping and a `run` of the
  tool, not a game started and played. Still UNKNOWN: the client's exact
  launch line for a game (its reaper or not) and whether it sets
  `STEAM_COMPAT_EMULATOR` or an `LD_PRELOAD` for the tool.
- **Steam API in games.** Proton's `lsteamclient` loads
  `STEAM_COMPAT_CLIENT_INSTALL_PATH/linux64/steamclient.so`, which in the
  arm64 client's install is x86-64 (MEASURED, `file`). That it talks to the
  running arm64 client is a HYPOTHESIS; the probes do not use the Steam API.
- **Real games**, 32-bit games (the i386 probes were not run through the
  tool), controllers (the XInput probe was not run: another session's
  `steamarm-inputd` was running), the overlay (dropped).
- **The exit hangs** above.
- **FEXServer** has to be running before a game starts from the arm64 client;
  the launcher's ARM64 entries do not start it.
- **Proton 10.0 / Steam Linux Runtime 3.0 (sniper).** Measured not to start
  a probe, the same way through the tool and on the x86 client's path:
  pressure-vessel builds the sniper container and Proton runs ("Proton:
  Upgrading prefix from None to 10.1000-105"), then Wine does not finish
  starting in 240-300 s, with or without esync. Cause UNKNOWN.
- The launcher's Processor (FEX) settings do not reach these games:
  `scripts/run-app.sh` drops `FEX_*` for aarch64 entries, and the tool sets
  `scripts/run-fex.sh`'s defaults.
- Stopping a client session: the game's processes carry the client's
  `STEAMARM_RUN_ID`, but `scripts/run-steam-arm64.sh` stops only
  descendants and orphans whose command line names `/steamrtarm64/`; an
  orphaned `wineserver` would be missed (VERIFIED IN SOURCE; not measured
  with a game).
