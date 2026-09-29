# ARM64-first audit: where each route stands

Numbers for the plan in `docs/ARM64_FIRST_MIGRATION.md`. They come from a
read-only audit on the Mac (2026-09-29, tree at `e4047ef`), the benchmark
record, and read-only checks made while writing this page. Nothing in the
audit itself was produced by starting Steam.

**Updated 2026-09-29 after stages 22-23 and the post-merge check.** The
tables now give today's state; the audit's own values are kept where they
are history, marked "at the audit". New values come from
`benchmarks/stage22-native-arm64-bringup.txt`,
`benchmarks/stage23-native-arm64-jit.txt`,
`benchmarks/stage23-frame-root.txt`, the commits named, and a read-only
reading of the launcher logs of the post-merge check (2026-09-29
10:01-10:27, `~/SteamARM-roots/logs/steam-20260929-10*.log`,
`steam-arm64-20260929-10*.log`, `steam-arm64-frame-20260929-100701.log`).

Labels: MEASURED (the command or file, and its result), VERIFIED IN SOURCE
(file:line at `dbd1657`), HYPOTHESIS, UNKNOWN. Logs are in
`~/SteamARM-roots/logs/`.

## Host

| item | value | label |
|---|---|---|
| Mac | Apple M4, 16 GiB, macOS 27.0 (26A428) | MEASURED (`sw_vers`, `sysctl`) |
| MoltenVK | 1.4.2 (`/opt/homebrew/lib/libMoltenVK.dylib` → `Cellar/molten-vk/1.4.2`), 431 exported `vk*` symbols | MEASURED (`readlink`, `nm -gU`) |
| KosmicKrisp | Mesa 26.2.3 installed, conformant Vulkan 1.4 on the M4; 3 exported `vk*` symbols (ICD entry points only). Not used by the shim at the audit; since `4ccb914` the shim loads it in ICD mode with `STEAMARM_VK_ICD=kosmickrisp` (D3D11/D3D12 probes 158.1/154.1 fps, `benchmarks/stage22-kosmickrisp.txt`) | MEASURED (`vulkaninfo` with `VK_DRIVER_FILES`, `nm -gU`; stage 22) |
| x18 kept for a pre-macOS-13 SDK binary | UNKNOWN on the M4 (preserved on a CI M1 runner, macOS 15.7.9) | stage 20 §3 |

## Processes per route

| metric | x86 route: launcher entry "Steam" (the default and the working route) | native arm64 route: "Steam ARM64 (experimental)" and "Steam ARM64 · Steam Frame (experimental)" |
|---|---|---|
| Steam client processes translated by FEX | all of them. At the audit: 36 of 36 image loads are `/tmp/lxrt-root/usr/bin/FEX-gb` in `steam-20260929-053409.log` and `steam-20260929-052749.log`; 35 of 35 in `steam-023204.log`. Post-merge check: 35 of 35 in each x86 Steam log from `steam-20260929-100948.log` on. A lower bound: the bwrap-interpreted `FEX-emu` execs (webhelper container, games) are not in these logs (0 `lxrt-emu` lines). MEASURED | 0. At the audit: 0 FEX lines, 0 x86 exec lines in `native-arm64-steam-nss-20260929.log` and `native-arm64-steam-cef-software-20260928.log`. Post-merge check, both entries from the launcher: 0 FEX lines; 35 (Fedora root, twice) and 60 (Frame root) image loads, every one an aarch64 PIE. MEASURED |
| aarch64 images the client started | 0 | at the audit: `steamrtarm64/steam` ×2, `steamsysinfo`, `vulkandriverquery`, `gldriverquery`, `sh` ×25, `bash` ×13, coreutils ×14, `lspci` ×2; `steamwebhelper` and `taskset` in `Steam/logs/steamwebhelper.log`. All aarch64 PIE. MEASURED |
| live guest processes | at the library view: UNKNOWN under lxrun (11 in the retired VM, `stage6-steam-gap.txt:4-6`). Measure with Steam at the library view: `ps -axo command= \| grep '[b]uild/lxrun' \| grep -v -e Xvnc -e FEXServer \| wc -l` | at the sign-in window: 8 lxrun processes, 2.28-2.36 GB resident in total on the Fedora root (stage 23 E5, E7, C4); 10 and 2.53-2.66 GB on the Frame root (F4-F6). No library view yet. MEASURED |
| window after start | sign-in window 85-86 s (stage 18, stage 21); 120 s on a clean install (stage 17). Main window (signed in) 88-93 s in 4 of 5 starts of the post-merge check; the fifth died with a host SIGTRAP in `pthread_jit_write_protect_np` (cause UNKNOWN, `b3f64c8`). MEASURED | at the audit: none, it aborted before its window. Now the sign-in window: 10-13 s from `scripts/run-steam-arm64.sh` on the Fedora root (stage 22 E18, stage 23 E5/E7), 17-19 s on the Frame root (F3-F6); from the launcher 17-18 s (stage 23 L3-L10), and 15-16 s (Fedora root) and 21 s (Frame root) in the post-merge check, where **Detener** left 0 guests. Sign-in not attempted. MEASURED |

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
| images loaded at exec, newest run at the audit | 0 unsupported, 0 unreachable, 0 `svc`/TLS poisoned; `steam` x18 2840/2840, `steamwebhelper` x18 1442/1442 | MEASURED |
| libraries loaded later, newest traced run (2026-09-28) | libcef 5 x18 sites poisoned per webhelper process (3 `exclusive`, 2 `sysreg`); libgallium 1 and steamclient.so 2 (`writes sp`). The audit gave the steamclient.so pair to libLLVM, whose report is 34,697 of 34,697 (interleaved trace lines; `926b89a`) | MEASURED |
| the same since `926b89a` | libcef 76,048/76,048, libgallium 6,178/6,178, steamclient.so 3,858/3,858, libLLVM 34,697/34,697: 0 refused (dry run) | MEASURED |
| the same for a run of the current client builds | not traced | UNKNOWN |
| poison instruction | `svc #1` in the audit's runs, not a trap; `brk #1` since `dbd1657` | VERIFIED IN SOURCE |
| whether any poisoned site executes | yes: two of libcef's, in the browser (E4) and in every renderer that died (E7), before they were rewritten | MEASURED (stage 22) |

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

After the audit, stage 22 added X locale data, glibc locales,
`libnssckbi.so`, `libSDL3.so.0` and an aarch64 `lsof` to the Fedora root's
seeds; with them the client reaches its sign-in window, and the abort path
is no longer reached (MEASURED, `benchmarks/stage22-native-arm64-bringup.txt`).
Neither root has a system D-Bus socket, GTK 3 is absent from the Fedora
root, and `steam-runtime-launcher-service` from both: none of them stopped
the sign-in window (MEASURED, stages 22-23). Whether they matter after
sign-in is UNKNOWN.

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

Since then (VERIFIED IN SOURCE at `b3f64c8`):

- `scripts/builtin-apps.json` holds the built-in entries, read by
  `scripts/run-app.sh` and the launcher: `steam` (x86, the default),
  `steam-arm64` (`/tmp/lxrt-armroot`) and `steam-arm64-frame`
  (`/tmp/lxrt-arm64root`), both aarch64 with `LXRT_GUEST_PAGE=4096`
  (`e8114fa`, `9468028`).
- `scripts/env-links.sh` links `/tmp/lxrt-armroot` and
  `/tmp/lxrt-arm64root` when their roots exist; `scripts/run-native.sh`
  gives native guests a Linux `PATH` (`ecca552`).
- `LauncherModel` runs sessions through `SessionMachine` and reads the
  detected `RuntimeCapabilities` (`774f590`).
- Adopting a Steam started outside the launcher still keys on
  `ubuntu12_32/steam` only (`scripts/run-app.sh:42`,
  `launcher/LauncherModel.swift:520`).

## UNKNOWNs and how to measure each

| unknown | command or check |
|---|---|
| live FEX process count, x86 route | the `ps` line above, with Steam at the library view |
| x18 preservation on the M4 | `tests/x18_preserve/run.sh` |
| dlopen rewrite coverage of the current client builds | native client with `LXRT_X18_ALL_TEXT=libcef.so LXRT_TRACE=1 LXRT_TRACE_MATCH=steamwebhelper LXRT_TRACE_FILE=<file>`; `grep -E 'sub-page code at|mapped code at|unsupported|SIGTRAP' <file>` |
| the native client after sign-in (main window, library, downloads) | sign in from either ARM64 entry; step 11 of `docs/ARM64_FIRST_MIGRATION.md` |
| the host SIGTRAP in `pthread_jit_write_protect_np` (1 of 5 x86 starts) and the zygote SIGBUS in host `memset` (1 of 10 native launcher cycles) | the next occurrence's fault report, which names host frames since `b3f64c8` |
| which Proton `proton_experimental` resolves to under the arm64 client | `compat_log.txt` and Settings → Compatibility once signed in |

Answered since the audit: whether poisoned sites run (yes, stage 22 E4/E7),
why the webhelper stopped before CEF init (not reproduced after the root
rebuild; cause UNKNOWN), the abort's cause (confirmed by probe, stage 22
E1/E1b/E1c) and whether the client runs on the Frame root (yes, to its
sign-in window, stage 23).
| SLR 4 arm64 under lxrun | `docs/STEAM_RUNTIME_4_ARM64.md` §5.3 steps 5-6 |
| esync under Proton 10.0 across processes | a probe with `WINEDEBUG=+esync`, with and without `PROTON_NO_ESYNC=1` |
