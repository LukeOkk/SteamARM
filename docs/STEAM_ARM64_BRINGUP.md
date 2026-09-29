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
The working route for games is still the x86 client under FEX. The sections
below are the earlier bring-up (`benchmarks/stage21-native-arm64-client.txt`),
the Mac's logs as read by the 2026-09-29 audit, and the analysis of the
abort, kept as written.

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
- **Launcher.** The built-in entry still uses the Fedora root. To move it,
  change only its `"root"` to `/tmp/lxrt-arm64root`; the GL variables come
  from the root's file.

## How it runs today

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
- **Link.** `/tmp/lxrt-armroot` → `~/SteamARM-roots/armroot` was made by hand
  (MEASURED `ls`). No script creates it, so it is gone after a reboot.
- **Launcher.** Not wired at `dbd1657`: there is no aarch64 Steam entry, and
  `scripts/run-native.sh` defaults to `/tmp/lxrt-arm64root` (which does not
  exist) and never sets `LXRT_GUEST_PAGE` (VERIFIED IN SOURCE,
  `scripts/run-native.sh:19-29`). A launcher entry is being worked on
  separately; it is not done here.

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

## Where it stops

Newest run: `native-arm64-steam-nss-20260929.log` (02:14-02:17).

- The webhelper was launched 13 times, 10 s apart, from 02:15:08 to
  02:17:10. Each printed "Disabling sandbox due to a previous crash in
  CefInitialize". None reached BrowserReady; `steamui_html.txt` ends with
  "Timed out waiting for webhelper init" (MEASURED). The webhelper is the open
  blocker: `docs/STEAMWEBHELPER_BRINGUP.md`.
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

### Experiments that would confirm it (not run)

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

The Fedora root is being changed by work in progress elsewhere: X locale
data, glibc locales, the `libnssckbi.so` link and `libSDL3.so.0` appeared in
it at 06:08 on 2026-09-29 (MEASURED `ls`, after the audit). What that does
to the abort is what E2 measures; it is not measured here.

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

- Why the 2026-09-29 webhelpers never reach CEF initialisation
  (`docs/STEAMWEBHELPER_BRINGUP.md`).
- Whether `LXRT_X18_ALL_TEXT` or other `LXRT_*` variables were set for the
  2026-09-29 run. The log records no environment.
- Which exact `build/lxrun` produced that log: it was rebuilt at 02:21,
  after the run. Nothing memory-related changed in `runtime/` between the
  WIP commit `4181429` and `e4047ef` (MEASURED `git diff`).
- The library bases above are reconstructed. A `vmmap` of a live native
  client, or abort reports that name mapped files, would make them MEASURED.
