# ARM64-first audit: where each route stands

Numbers for the plan in `docs/ARM64_FIRST_MIGRATION.md`. They come from a
read-only audit on the Mac (2026-09-29, tree at `e4047ef`), the benchmark
record, and read-only checks made while writing this page. Nothing here was
produced by starting Steam.

Labels: MEASURED (the command or file, and its result), VERIFIED IN SOURCE
(file:line at `dbd1657`), HYPOTHESIS, UNKNOWN. Logs are in
`~/SteamARM-roots/logs/`.

## Host

| item | value | label |
|---|---|---|
| Mac | Apple M4, 16 GiB, macOS 27.0 (26A428) | MEASURED (`sw_vers`, `sysctl`) |
| MoltenVK | 1.4.2 (`/opt/homebrew/lib/libMoltenVK.dylib` → `Cellar/molten-vk/1.4.2`), 431 exported `vk*` symbols | MEASURED (`readlink`, `nm -gU`) |
| KosmicKrisp | Mesa 26.2.3 installed, conformant Vulkan 1.4 on the M4; 3 exported `vk*` symbols (ICD entry points only); not used by the shim | MEASURED (`vulkaninfo` with `VK_DRIVER_FILES`, `nm -gU`) |
| x18 kept for a pre-macOS-13 SDK binary | UNKNOWN on the M4 (preserved on a CI M1 runner, macOS 15.7.9) | stage 20 §3 |

## Processes per route

| metric | x86 route (working) | native arm64 route (stage 21) |
|---|---|---|
| client guest images run by FEX | 36 of 36 image loads are `/tmp/lxrt-root/usr/bin/FEX-gb` in `steam-20260929-053409.log` and `steam-20260929-052749.log`; 35 of 35 in `steam-023204.log`. A lower bound: the bwrap-interpreted `FEX-emu` execs (webhelper container, games) are not in these logs (0 `lxrt-emu` lines). MEASURED | 0 FEX lines, 0 x86 exec lines in `native-arm64-steam-nss-20260929.log` and `native-arm64-steam-cef-software-20260928.log`. MEASURED |
| aarch64 images the client started | 0 | `steamrtarm64/steam` ×2, `steamsysinfo`, `vulkandriverquery`, `gldriverquery`, `sh` ×25, `bash` ×13, coreutils ×14, `lspci` ×2; `steamwebhelper` and `taskset` in `Steam/logs/steamwebhelper.log`. All aarch64 PIE. MEASURED |
| live guest processes at the library view | UNKNOWN under lxrun (11 in the retired VM, `stage6-steam-gap.txt:4-6`). Measure with Steam at the library view: `ps -axo command= \| grep '[b]uild/lxrun' \| grep -v -e Xvnc -e FEXServer \| wc -l` | no library view yet |
| login window after start | 85-86 s (stage 18, stage 21); 120 s on a clean install (stage 17). MEASURED | none: aborts before its window. MEASURED |

## x86 code that is not a game payload

x86 route, all required today (VERIFIED IN SOURCE and MEASURED;
`docs/CURRENT_STEAM_ENVIRONMENT.md` §6): the i386 `ubuntu12_32/steam` with
`steamui.so` and scout libraries; the x86-64 `steamwebhelper`; x86-64 bash
for `steam.sh`, `setup.sh`, `steam-runtime-check-requirements` and the
launcher service; amd64 `lsof` and libtirpc; i386 GTK 2 and libXtst; x86-64
pressure-vessel tools for the webhelper container; FEXServer on the client
path; the HideHypervisorBit config for `steam`
(`scripts/install-steamroot-gfx.sh:99-110`).

Native route: none ran. The install still carries x86 files (MEASURED,
`file(1)`): `steamrt64` 779 x86-64 + 536 i386, `steamrt32` 27 i386,
`ubuntu12_32` 51 i386 + 1 x86-64, `ubuntu12_64` 38 x86-64, `linux64` 2,
`linux32` 3, `bin` 1. HYPOTHESIS: they serve x86 games.

## Rewrite coverage (native route)

Details in `docs/ARM64_REWRITE_COVERAGE.md`.

| scope | result | label |
|---|---|---|
| images loaded at exec, newest run | 0 unsupported, 0 unreachable, 0 `svc`/TLS poisoned; `steam` x18 2840/2840, `steamwebhelper` x18 1442/1442 | MEASURED |
| libraries loaded later, newest traced run (2026-09-28) | libcef 5 x18 sites poisoned per webhelper process (3 `exclusive`, 2 `sysreg`); libgallium 1 and libLLVM 2 (`writes sp`) | MEASURED |
| the same for the 2026-09-29 run | not traced | UNKNOWN |
| poison instruction in those runs | `svc #1`, not a trap; `brk #1` since `dbd1657` | VERIFIED IN SOURCE |
| whether any poisoned site executes | | UNKNOWN (a traced run after `dbd1657` answers it) |

## Missing ARM64 root pieces

| piece | Fedora armroot at the audit | Steam Frame root | label |
|---|---|---|---|
| `steam-runtime-launcher-service` | absent ("not found" ×3) | absent | MEASURED |
| aarch64 `lsof` | absent | present | MEASURED (`ls`) |
| `C.UTF-8` / glibc locales | absent (41 + 17 setlocale warnings in 3 logs) | present | MEASURED |
| X locale data | absent | present | MEASURED |
| GTK 3 | absent; whether needed UNKNOWN | present | MEASURED |
| `libnssckbi.so` | absent | present | MEASURED |
| D-Bus system bus socket | absent (`cef_log.txt`, 2026-09-28 17:17:18) | dbus 1.14.10 installed; nothing in `scripts/` or `runtime/` starts a daemon | MEASURED / VERIFIED IN SOURCE |
| shared-library closure of the client and libcef | 137 objects, none missing | not checked | MEASURED (read-only ELF walk) |

The Fedora root gained X locale data, glibc locales, `libnssckbi.so` and
`libSDL3.so.0` at 06:08 on 2026-09-29, after the audit, from work in
progress elsewhere (MEASURED `ls`). Its effect is not measured here.

## ARM64 tools: installed versus runnable

| tool | appid, build | runnable on macOS |
|---|---|---|
| Steam Linux Runtime 4.0 Arm64 | 4185400, 24599775 | UNKNOWN: never started (`docs/STEAM_RUNTIME_4_ARM64.md` §5.3) |
| Proton Experimental ARM64 | 4427310, 25551720 | not run; HYPOTHESIS: no, for the same reasons as Proton 11.0 ARM64 |
| Proton 11.0 ARM64 | 4628740, 25118360 | no: `wine cmd` exits in 2 s (MEASURED, stage 18) |

Installed: 3 (MEASURED appmanifests). Runnable: 0. Showing a tool in Steam's
menu does not make it runnable.

## Launcher wiring

VERIFIED IN SOURCE at `dbd1657`; a launcher change for this is in progress
elsewhere and not counted here.

- No aarch64 Steam `AppEntry`; the launch path keys on `ubuntu12_32/steam`
  (`scripts/run-app.sh:41`, `launcher/LauncherModel.swift:290`).
- `scripts/run-native.sh` defaults to `/tmp/lxrt-arm64root`, which does not
  exist (MEASURED `ls`), and never sets `LXRT_GUEST_PAGE`.
- `/tmp/lxrt-armroot` is a hand-made link; no script creates it.
- `LinuxBaseEnvironment`, `SessionMachine`, `LaunchPlanner` and
  `RuntimeCapabilities` are not used outside `launcher/ApplicationCore.swift`.

## UNKNOWNs and how to measure each

| unknown | command or check |
|---|---|
| live FEX process count, x86 route | the `ps` line above, with Steam at the library view |
| x18 preservation on the M4 | `tests/x18_preserve/run.sh` |
| dlopen rewrite coverage now, and whether poisoned sites run | native client with `LXRT_X18_ALL_TEXT=libcef.so LXRT_TRACE=1 LXRT_TRACE_MATCH=steamwebhelper LXRT_TRACE_FILE=<file>`; `grep -E 'sub-page code at|mapped code at|SIGTRAP' <file>` |
| why the webhelper stops before CEF init | the standalone run in `docs/STEAMWEBHELPER_BRINGUP.md` |
| the abort's cause | E1, E1b, E2 in `docs/STEAM_ARM64_BRINGUP.md` |
| whether the client runs on the Frame root | steps 6-8 of `docs/ARM64_FIRST_MIGRATION.md` |
| which Proton `proton_experimental` resolves to under the arm64 client | `compat_log.txt` and Settings → Compatibility once the UI is up |
| SLR 4 arm64 under lxrun | `docs/STEAM_RUNTIME_4_ARM64.md` §5.3 steps 5-6 |
| esync under Proton 10.0 across processes | a probe with `WINEDEBUG=+esync`, with and without `PROTON_NO_ESYNC=1` |
