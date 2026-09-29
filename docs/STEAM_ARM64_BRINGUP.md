# Valve's native arm64 Steam client under lxrun

Status on 2026-09-29 (stage 23): the client reaches its "Sign in to Steam"
window with V8's JIT on, from `scripts/run-steam-arm64.sh` and from the
launcher (`scripts/run-app.sh steam-arm64`); login itself was not tried
(MEASURED, `benchmarks/stage23-native-arm64-jit.txt`). Stage 22 reached the
same window only with `--jitless`
(`benchmarks/stage22-native-arm64-bringup.txt`); stage 23 split V8's
read-write-execute code pages W^X per page (`runtime/wxsplit.c`) and kept
the runtime's SIGSEGV/SIGBUS handler when the guest resets them to SIG_DFL.
It also reaches that window on the root derived from the Steam Frame image
(`scripts/mkframeroot.sh`), in 5 of 5 runs, 5-7 s later than on the Fedora
root (MEASURED, `benchmarks/stage23-frame-root.txt`; next section).
The launcher has two built-in entries for it (`scripts/builtin-apps.json`,
`9468028`): "Steam ARM64 (experimental)" (`steam-arm64`, the Fedora
armroot) and "Steam ARM64 · Steam Frame (experimental)"
(`steam-arm64-frame`, the Steam Frame root). In the post-merge check of
2026-09-29 (after `b3f64c8`), started from the launcher, the sign-in window
came at 15-16 s on the Fedora root and 21 s on the Frame root, and
**Detener** left 0 guest processes (MEASURED; launcher logs
`steam-arm64-20260929-100619.log`, `-100747.log` and
`steam-arm64-frame-20260929-100701.log`: 0 FEX lines, every image an
aarch64 PIE). Sign-in, the library, downloads and games under this client
are not verified; Proton ARM64 does not run on macOS. The working route for
games is still the x86 client under FEX. The sections below are the earlier
bring-up (`benchmarks/stage21-native-arm64-client.txt`), the Mac's logs as
read by the 2026-09-29 audit, and the analysis of the abort, kept as
written, with notes where stage 22 answered them.

Labels: MEASURED (a command and its result), VERIFIED IN SOURCE (file:line),
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN. Logs are in
`~/SteamARM-roots/logs/`; code line numbers are at `dbd1657`.

## On the Steam Frame root (stage 23)

```sh
scripts/mkframeroot.sh [--home-from ~/SteamARM-roots/armroot/tmp/armhome]
scripts/env-links.sh                    # /tmp/lxrt-arm64root, while the volume is attached
ARMROOT=~/SteamARM-roots/arm64root ARMROOT_LINK=/tmp/lxrt-arm64root \
    scripts/run-steam-arm64.sh --for 180
tests/arm64/frame_glx.sh                # GLX in that root: an indirect context
```

- **Root.** An APFS clone of the extraction of the Steam Frame 0.3.0 image
  (`/Volumes/SteamFrameRoot/rootfs`, never written) with three additions:
  `etc/resolv.conf`, the host's `etc/localtime`, and `.lxrt-guest-env`
  (MEASURED, `benchmarks/stage23-frame-root.txt`).
- **GL.** The image's Mesa (deckard-mesa) has no software driver and, on
  its own, finds no GLX visual on the Mac's X server: the client exited at
  `glXChooseVisual failed` (stage 22 E19). Indirect GLX through the X
  server works with `__GLX_VENDOR_LIBRARY_NAME=mesa`,
  `MESA_LOADER_DRIVER_OVERRIDE=swrast` and `LIBGL_ALWAYS_INDIRECT=1`,
  which `.lxrt-guest-env` names. `scripts/run-steam-arm64.sh` and
  `scripts/run-native.sh` read that file (`scripts/guest-env.sh`).
- **Network.** The image has no `resolv.conf`; without one the client's
  update check failed ("http error 0") and it exited.
- **Branch.** On this root the client moves itself to the
  `steamdeck_stable` branch: the same version, 1788652215, with other
  binaries (it updated itself once, about 220 MB).
- **Result.** The login window in 5 of 5 runs, from `run-steam-arm64.sh`
  and from the launcher's runner (`run-native.sh`). BrowserReady at 15-17 s
  against 7-8 s on the Fedora root; 2.53-2.66 GB against 2.28-2.33 GB.
  MEASURED: the first webhelper is restarted within a second of its start
  (cause UNKNOWN), and its two zygotes stay alive for the whole session.
  The client also takes its SteamOS code paths (SteamOSManager,
  NetworkManager, atomupd-manager, which crashes without a system D-Bus).
  That these paths and the restart explain the slower start is a
  HYPOTHESIS.
- **Launcher.** `9468028` added a separate built-in entry for this root,
  `steam-arm64-frame` ("Steam ARM64 · Steam Frame (experimental)", root
  `/tmp/lxrt-arm64root`, the same command and environment), and kept
  `steam-arm64` on the Fedora root on purpose, because that root starts
  5-7 s faster. Do not change `steam-arm64`'s root:
  `tests/launcher/run_app_dispatch.sh` expects `/tmp/lxrt-armroot` for it.
  The GL variables come from the root's `.lxrt-guest-env` through
  `scripts/run-native.sh`. The launcher shows the entry as unavailable,
  with the reason, while the SteamFrameRoot volume is not attached or the
  client is missing from `<root>/tmp/armhome`. Before `9468028` the Frame
  root was run through the launcher's runner only (`run-native.sh`, run N1
  of stage 23); the first start through `scripts/run-app.sh
  steam-arm64-frame` is the post-merge check above (window at 21 s).
- **Rebuilds and stops.** `scripts/mkframeroot.sh` and `scripts/mkarmroot.sh`
  resolve SRC/OUT first (symlinks, trailing slashes, relative to the
  caller), refuse a layout where the swap would delete or write something
  else, and refuse a root a guest runs on. `scripts/roots.sh` carries
  `tmp/` (the client home) and `opt/apps` over, puts them back after an
  interrupted swap, and never deletes a leftover that holds a home
  (VERIFIED IN SOURCE; MEASURED on fake roots by
  `tests/launcher/mkarmroot.sh` and `tests/launcher/guest_env.sh`).
  `scripts/run-steam-arm64.sh` refuses `ARMROOT` without `ARMROOT_LINK`
  unless it is `$STEAMARM_STATE/armroot` or `.../arm64root`, and a root or
  link a guest runs on. When it stops, it stops only the orphans that carry
  its `STEAMARM_RUN_ID` (MEASURED with a stand-in for lxrun,
  `tests/launcher/run_steam_arm64.sh`). That the variable reaches the real
  webhelper's zygotes is a HYPOTHESIS: `runtime/process.c` (`lxrt_execve`)
  passes the guest's environment on, and those processes still find their
  files through `LXRT_ROOT`, which travels the same way; no client session
  has been checked yet. Orphans without it are named in the log and left
  running.

## How it runs on the Fedora armroot

From the launcher: **Steam ARM64 (experimental)**. From a shell:

```sh
scripts/run-steam-arm64.sh --for 180       # stops what it started at the end
```

By hand, as stage 21 did (the script adds `LXRT_X18_ALL_TEXT=libcef.so`,
a Linux `PATH` and `TMPDIR`, and tracks the processes it started):

```sh
LXRT_ROOT=/tmp/lxrt-armroot LXRT_GUEST_PAGE=4096 HOME=/tmp/armhome DISPLAY=:2 \
    build/lxrun /tmp/armhome/.local/share/Steam/steamrtarm64/steam
```

- **Client.** Downloaded from Valve's `steam_client_linuxarm64` manifest
  (version 1788652215, 35 zips, about 1.0 GB), then self-updated (MEASURED,
  stage 21). Entry point `steamrtarm64/steam`, an aarch64 PIE. No repository
  script downloads it yet.
- **Root.** `scripts/mkarmroot.sh` builds a Fedora 43 aarch64 root at
  `~/SteamARM-roots/armroot` from `scripts/mkarmroot.lock`: 150 root packages
  plus 5 build-only ones (MEASURED count of the lock; stage 21 says 153).
  Its glibc is 2.42-16. The client's home is `<armroot>/tmp/armhome`.
- **Link.** `/tmp/lxrt-armroot` → `~/SteamARM-roots/armroot`. At the audit
  it was made by hand (MEASURED `ls`). Since `e8114fa`,
  `scripts/env-links.sh` makes it whenever the armroot exists, and
  `scripts/run-steam-arm64.sh` links it too (VERIFIED IN SOURCE).
- **Launcher.** Not wired at `dbd1657`. Since `e8114fa` and `774f590` the
  built-in entry `steam-arm64` runs `steamrtarm64/steam` in
  `/tmp/lxrt-armroot` with `HOME_IN_GUEST=/tmp/armhome`,
  `LXRT_GUEST_PAGE=4096` and `LXRT_X18_ALL_TEXT=libcef.so`, through
  `scripts/run-native.sh`, which gives native guests a Linux `PATH` since
  `ecca552`. Through the launcher's path the window came in 9 of 10
  start/stop cycles in stage 23 (L1-L10; the failed one is the zygote
  SIGBUS of `docs/STEAMWEBHELPER_BRINGUP.md`), and in the post-merge check
  above.

The client needs at most `GLIBC_2.29` (MEASURED 2026-09-29: `llvm-readelf -V`
over `steam`, `steamui.so`, `steamclient.so`, `vgui2_s.so`, `chromehtml.so`,
`steamwebhelper` and `libcef.so`; no `GLIBCXX` or `CXXABI` versions). Both
Fedora's glibc 2.42 and the Steam Frame image's 2.39 satisfy that.

## What the runtime needed (stage 21, MEASURED)

1. 4 KiB-aligned ELF images load through `runtime/subpage.c`.
2. `LXRT_GUEST_PAGE=4096` sets AT_PAGESZ to 4096, so glibc dlopens the
   client's 4 KiB-aligned libraries.
3. The x18 pass covers only `.eh_frame` function ranges; the client's static
   OpenSSL keeps constants in `.text`.
4. x18 loads and stores at large `sp` offsets go through a scratch copy of
   `sp`.
5. Trampoline pools split per reachable range for libcef's ~162 MiB of code;
   `blr x18` rewritten; libcef mapped from its file instead of copied
   (webhelper start 2.52 s → 1.49 s); x18 rewriting over all of libcef's text
   with `LXRT_X18_ALL_TEXT`.
6. Fault reports name the memory region of the pc.

Since then: `dbd1657` makes poisoned rewrite sites trap (`brk #1`); before
it they were live `svc #1` (`docs/X18_VIRTUALIZATION.md`).

## What the install contains

`file(1)` over `armroot/tmp/armhome/.local/share/Steam` (MEASURED, audit):

| directory | aarch64 ELF | x86 ELF |
|---|---:|---|
| `steamrtarm64` | 82 | |
| `linuxarm64` | 5 | |
| `androidarm64` | 5 (incl. `libsteamclient.so`, 37,517,612 B) | |
| `steamrt64` | | 779 x86-64 + 536 i386 |
| `steamrt32` | | 27 i386 |
| `ubuntu12_32` | | 51 i386 + 1 x86-64 |
| `ubuntu12_64` | | 38 x86-64 |
| `linux64`, `linux32`, `bin` | | 2, 3, 1 |

None of the x86 files ran in the native runs: 0 FEX and 0 x86 exec lines
(MEASURED). HYPOTHESIS: they serve x86 games (`steamclient.so`, overlay).

## Where it stopped before stage 22

The audit's newest run, `native-arm64-steam-nss-20260929.log`
(02:14-02:17). None of what follows came back after stage 22's root and
runtime changes (`benchmarks/stage22-native-arm64-bringup.txt`).

- The webhelper was launched 13 times, 10 s apart, from 02:15:08 to
  02:17:10. Each printed "Disabling sandbox due to a previous crash in
  CefInitialize". None reached BrowserReady; `steamui_html.txt` ends with
  "Timed out waiting for webhelper init" (MEASURED). The webhelper was the
  blocker then: `docs/STEAMWEBHELPER_BRINGUP.md`.
- The main process then aborted: `free(): invalid pointer`, then
  `[lxrt] pid 60919 ...: SIGABRT to itself` (MEASURED, log lines 591-592).
- `steam-runtime-launcher-service` was "not found" 3 times (non-fatal).
- `gldriverquery` (pid 60958) aborted in a library constructor: the root's
  `libSDL2` is sdl2-compat, which requires `libSDL3.so.0`, and the root has
  none (MEASURED, strings and DT_NEEDED). Not fatal to the client.

### The abort: root cause

HIGH-CONFIDENCE HYPOTHESIS, from disassembly of the saved binaries. No
memory map was logged, so the library bases are reconstructed.

1. The abort is on the **main thread** of `steamrtarm64/steam`. The frames
   end in `__libc_start_call_main` (MEASURED, frame walker
   `runtime/dispatch.c:1797-1839`).
2. In this glibc (2.42-16), "free(): invalid pointer" has one source: the
   check that the pointer is 16-byte aligned (`libc.so.6` `0xa1b90`). It
   runs before any tcache, arena or TLS access. Heap metadata is not
   involved (VERIFIED IN SOURCE, disassembly).
3. The caller chain: `steamui.so` builds a `CRescueDialog` (RTTI name
   `13CRescueDialog`) → the vgui `Frame` constructor sets its title →
   `vgui2_s.so` `0x980d8`. That function calls
   `XwcTextListToTextProperty(dpy, list, 1, XUTF8StringStyle, &tp)`,
   **ignores its return value**, calls `XSetWMName`, then
   `XFree(tp.value)` (VERIFIED IN SOURCE, disassembly of `vgui2_s.so`).
   `tp` is a stack slot that is never initialised.
4. libX11 1.8.13 returns `-2` (XLocaleNotSupported) **without writing `tp`**
   when `_XlcCurrentLC()` is NULL. That happens when it finds no
   `<XLOCALEDIR or /usr/share/X11/locale>/locale.dir` (VERIFIED IN SOURCE,
   disassembly of `libX11.so.6.4.0`; UPSTREAM DOCUMENTED, libX11
   `lcWrap.c`/`lcPublic.c`/`lcDB.c`).
5. When these runs were made, the Fedora root had no `/usr/share/X11/locale`
   (`libX11-common` is not in `scripts/mkarmroot.lock` at `dbd1657`) and no
   glibc locales (MEASURED `ls`, audit).
   The log agrees: `XOpenIM() failed, LANG = es_MX.UTF-8` (lines 74-75), and
   `setlocale` fell back to "C" (line 10).
6. The frames are identical in all 4 captured runs that reached the
   webhelper timeout, across runtime builds with different x18 results
   (MEASURED: `chunk-rewrite-20260928-2`, `zoneinfo-long-20260928`,
   `with-arm-tools-20260928`, `nss-20260929`).

So: a Valve bug (an unchecked return in `vgui2_s`), reached only on the
rescue-dialog path, which runs because the webhelper never initialises, and
which fires because the root lacks X locale data. Runtime causes (the x18
FDE filter, TLS, madvise, munmap on partial pages, the sub-page path, mremap,
brk, AT_PAGESZ) are ranked LOW: each would corrupt heap memory and give a
different message, not hand `free()` a misaligned pointer.

Earlier notes named the missing `lsof` as a HYPOTHESIS for this abort
(`docs/STEAM_FRAME_COMPAT_TOOLS.md` §7.2). The chain above does not involve
it.

### Experiments that would confirm it

Run in stage 22 (MEASURED, `benchmarks/stage22-native-arm64-bringup.txt`):
E1 on the old root gave `XSupportsLocale` 0, return `-2` and `tp.value`
unchanged, the chain's step 4; E1b with `XLOCALEDIR` set gave
`XSupportsLocale` 1; E1c on the rebuilt root (with `libX11-common` and
glibc locales in its seeds) gave `XSupportsLocale` 1 without it. E2 could
not run as written: on the rebuilt root the webhelper initialised (27 of 27
launches reached BrowserReady), so the timeout, the rescue dialog and the
abort were never reached again. The `free()` fix is therefore shown by the
probes, not by a client run. E3 was not needed. The table is the plan as
it was written.

E1 to E3 write into `~/SteamARM-roots`, so they need the owner's approval.

| id | what | confirms if | refutes if |
|---|---|---|---|
| E1 | a probe outside Steam that calls `XSupportsLocale` and `XwcTextListToTextProperty` with `tp.value` preset to `0x4141414141414141`, in the Fedora root, `DISPLAY` unset | `XSupportsLocale=0`, return `-2`, `tp.value` unchanged | `XSupportsLocale=1` |
| E1b | E1 with `XLOCALEDIR` pointing at `usr/share/X11/locale` from the matching `libX11-common` rpm | `XSupportsLocale=1` | |
| E2 | the stage 21 command with `XLOCALEDIR` set, waiting past the webhelper timeout (about 2 min 40 s) | no `XOpenIM() failed`, no abort after "Timed out waiting for webhelper init" | the same frames |
| E3 | only if E2 still aborts: runs with `LXRT_NO_TOPDOWN=1`, `LXRT_NO_MADVISE=1`, `LXRT_NO_FAST_SUBPAGE=1` one at a time | frames or message change: that mechanism | |

The permanent fix, if E2 confirms: add `libX11-common` (and SDL3, a glibc
langpack and the `libnssckbi.so` link) to the Fedora root's seeds, or move
to the Steam Frame root, which has them (next section). Neither fixes the
webhelper.

## Missing root pieces: Fedora root and Steam Frame root

MEASURED with `ls` on 2026-09-29, read-only. The Fedora column is the root
stage 21 and the audit saw. The Frame root is the extraction at
`/Volumes/SteamFrameRoot/rootfs` (`docs/STEAM_FRAME_INVENTORY.md`).

| piece | Fedora root (stage 21, audit) | Steam Frame root | seen missing in |
|---|---|---|---|
| X locale data (`usr/share/X11/locale/locale.dir`) | absent | present (libx11 1.8.9) | the abort chain above |
| glibc locales (`C.UTF-8`) | absent | present (`usr/lib/locale/C.utf8` and others, holo-glibc-locales 2.39-2) | 41 + 17 setlocale warnings in 3 native logs |
| aarch64 `lsof` | absent | present (4.99.3) | x86 client's port check (stage 8) |
| GTK 3 | absent | present (3.24.41) | whether the client needs it: UNKNOWN |
| `libnssckbi.so` (libcef dlopens it by name) | absent | present | |
| D-Bus system bus socket | absent | package present (dbus 1.14.10); a socket needs a running daemon | `cef_log.txt` 2026-09-28 17:17:18 |
| `libSDL3.so.0` in the system dirs | absent | absent (the client ships its own in `steamrtarm64/`) | `gldriverquery` abort |
| `steam-runtime-launcher-service` | absent | absent from `/usr`; only x86 copies inside the bootstrap's `steamrt64/` | 3 "not found" lines |

Stage 22 added X locale data, glibc locales (C.UTF-8, en_US.UTF-8), the
`libnssckbi.so` link, SDL3 and `lsof` to the Fedora root's seeds
(`scripts/mkarmroot.lock`; MEASURED rebuilds in
`benchmarks/stage22-native-arm64-bringup.txt`). With them the abort path is
no longer reached, and without `lsof` the client had rejected the
webhelper's transport connections (403, E13). `steam-runtime-launcher-
service` is still missing; the client logs it as a possible problem and
carries on.

That the client gets further on the Frame root is a HYPOTHESIS until it is
run there (`docs/ARM64_FIRST_MIGRATION.md`).
Run there on 2026-09-29 (stage 23): it gets as far as on the Fedora root
(the login window), not further, once GLX, DNS and the branch are handled.
The rows still absent there (a system D-Bus socket, `libSDL3.so.0` in the
system directories, `steam-runtime-launcher-service`) did not stop it
(`benchmarks/stage23-frame-root.txt`).

## ARM64 tools the native client installed

Library appmanifests (MEASURED): 4185400 Steam Linux Runtime 4.0 Arm64
(build 24599775), 4427310 Proton Experimental ARM64 (25551720), 4628740
Proton 11.0 ARM64 (25118360). `scripts/install-arm-proton-tools.sh:35-39`
cloned them (VERIFIED IN SOURCE). Runnable on macOS: 0. Proton 11.0 (ARM64)
`wine cmd` exits in 2 s: it cannot reserve its low ranges
(`benchmarks/stage18-settings-audio-controllers.txt:136-141`; reasons in
stage 19 §2). `compat_log.txt` maps AppID 0 to `proton_experimental` at
priority 75 (last entry 2026-09-29 02:15:07); which build that name means
under the arm64 client is UNKNOWN.

## Open questions

- Sign-in, the main Steam window, the library and downloads under the
  native client: not attempted.
- Which Proton the native client resolves `proton_experimental` to, and how
  x86 Proton is to be offered to it (`docs/ARM64_FIRST_MIGRATION.md`,
  step 13). Proton ARM64 cannot start on macOS (stage 18, stage 19 §2).
- Why the Frame root restarts the first webhelper, and why a webhelper
  zygote died once with SIGBUS in a host `memset` at start (1 of 10
  launcher cycles, stage 23 L2): both UNKNOWN
  (`docs/STEAMWEBHELPER_BRINGUP.md`).
- Answered or moot since stage 22: why the early 2026-09-29 webhelpers
  never reached CEF initialisation (not reproduced after the root rebuild;
  cause UNKNOWN), whether `LXRT_X18_ALL_TEXT` was set for that run, and
  which `build/lxrun` produced it.
- The library bases in the abort analysis are reconstructed. The abort is
  no longer reached, so they will stay reconstructed unless it comes back.
