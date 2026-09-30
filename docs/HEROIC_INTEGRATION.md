# Heroic Games Launcher on SteamARM

**Status, 2026-09-29 (stage 24).** Heroic 2.22.3 runs as a **native
linux-arm64 program** under lxrun, from the Fedora ARM64 root: no FEX, no
VM. Its window opens in about 4.5 s, its pages work, and it closes cleanly
and opens again (MEASURED, `benchmarks/stage24-heroic.txt`). Signing in to a
store, downloads and games are **not verified** (no account was used), and
Amazon Games cannot work yet. The launcher installs it as **Heroic Games
Launcher (ARM64, experimental)**; the x64 build under FEX is no longer
installed.

**Stage 28 (2026-09-30).** V8's TurboFan runs: its crash was the runtime's
`br x18` trampoline clobbering a jump table's live x16, now fixed, and the
entry no longer passes `--js-flags=--no-opt` (MEASURED,
`benchmarks/stage28-keep-x18.txt`).

Labels: MEASURED, VERIFIED IN SOURCE, UPSTREAM DOCUMENTED, HYPOTHESIS,
UNKNOWN.

## Why ARM64, and where the build comes from

| fact | label |
|---|---|
| Heroic's releases ship Linux builds for x64 only (v2.22.3: deb, pacman, tar.xz, AppImage, rpm); macOS and Windows have arm64 ones | UPSTREAM DOCUMENTED |
| Flathub's `com.heroicgameslauncher.hgl` is `only-arches: x86_64` and wraps the x64 AppImage | UPSTREAM DOCUMENTED |
| Heroic's CI builds a linux arm64 AppImage (`build-base.yml`: `pnpm dist:linux AppImage --x64 --arm64`), kept only as a 14-day workflow artifact; the release workflow publishes x64 | UPSTREAM DOCUMENTED |
| Heroic is Electron (43.1.1 pinned by its pnpm-lock) with no native Node module | UPSTREAM DOCUMENTED; MEASURED (no `.node` in `app.asar`) |
| Its helpers have linux arm64 releases: legendary 0.21.1, gogdl v1.3.0, nile v1.2.0, comet v0.2.0; vulkan-helper arm64 is in Heroic's repository | UPSTREAM DOCUMENTED (`meta/downloadHelperBinaries.ts`) |

So an ARM64 build is obtainable now, and ARM64-first says use it.
`scripts/install-heroic-arm64.sh` assembles it the way electron-builder does,
from official release files only, each checked against a pinned sha256:

1. Electron 43.1.1 linux-arm64, with `electron` renamed `heroic`;
2. `resources/` of the official Heroic 2.22.3 linux-x64 tarball, whose
   `app.asar` header is re-pointed from the x64 helpers to arm64 ones
   (`scripts/heroic-asar.py`; the archive's data is copied unchanged);
3. the arm64 helpers from their own releases.

MEASURED: Heroic built from its source tag with its own toolchain
(`electron-builder --linux dir --arm64`) and the script's output are the same
tree (`diff -r`: no difference; `app.asar` byte-identical, sha256
`86782b62…`). The x64 and arm64 `app.asar` differ only in the five helpers.
Files, URLs, sha256 and licences: `benchmarks/stage24-heroic.txt` §2
(Heroic and its Python helpers GPL-3.0, Electron MIT, comet Apache-2.0).
SteamARM downloads them; it does not bundle them.

## What the root needs

Electron's binary links `libgtk-3.so.0`; Chromium dlopens libsecret and
libnotify; legendary and gogdl are Python zipapps (`#!/usr/bin/env python3`,
cpython 3.14 modules). `scripts/mkarmroot.sh` now seeds `libgtk-3.so.0`,
`libsecret-1.so.0`, `libnotify.so.4` and `python3` (Fedora 43: gtk3 3.24.52,
python3 3.14.7): 24 packages more, all additions, same repodata (MEASURED).
A rebuild keeps `opt/apps` and `tmp/`, so an installed Heroic and its home
survive it. A root built before this lacks them: the installer refuses and
the launcher shows the entry as unavailable, with the reason (VERIFIED IN
SOURCE, `HeroicARM64.missing`). The Steam Frame root already has all four.

The Electron images are aligned to 64 KiB, so Heroic runs with the host's
16 KiB page (no `LXRT_GUEST_PAGE=4096`).

## What the runtime needed

Six fixes, each found on a Heroic run and each covered by
`tests/elf/electron_runtime.c` (29 checks; at `611d71d` the same checks
crash, hang or fail in every section):

| fix | symptom before it |
|---|---|
| `prlimit64` writes the old limit through Darwin's `getrlimit`, so a read-only buffer is `EFAULT` | Chromium's `ProtectedMemory` check faulted inside the runtime |
| an `mprotect` length that is not a multiple of 4 KiB covers its last page (16 KiB for a 16 KiB guest) | the tail of Chromium's `protected_memory` stayed writable; a `CHECK` ended the browser |
| SysV `IPC_RMID` of an attached segment is deferred to the last detach, exec or exit | cairo's MIT-SHM attach was refused by the X server (BadAccess) and GDK exited |
| `execve` of a missing file fails with `ENOENT` before the runtime replaces itself | `env python3` died on the first PATH entry: legendary and gogdl could not start |
| a `dup` of an epoll descriptor is the same epoll instance | comet (tokio) could not build its runtime |
| `SOCK_SEQPACKET` end-of-file: blocked `recvmsg`/`ppoll` wait in slices; `ECONNRESET` reads as 0 | Chromium's zygotes never saw the browser exit and stayed behind |

`LXRT_X18_ALL_TEXT=/opt/apps/heroic/`: Chromium is built without unwind
tables, so most functions have no `.eh_frame` FDE and the default x18 pass
missed them (ICU crashed on `ldrb w3, [x18], #1`). Over the whole text:
78,584 x18 sites, all rewritten; 78,573 of them inside functions known to
Electron's symbols, none in V8's embedded builtins (MEASURED).

## How it runs

The launcher entry (`HeroicARM64` in `launcher/ApplicationCore.swift`):

```
root     /tmp/lxrt-armroot                       (aarch64, no translator)
command  /opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic
         --no-sandbox --disable-gpu
env      HOME_IN_GUEST=/tmp/heroichome  LXRT_X18_ALL_TEXT=/opt/apps/heroic/
```

- `--no-sandbox`: Chromium's sandbox needs namespaces and seccomp.
- `--disable-gpu`: its GPU process cannot initialise GL here (ANGLE over
  indirect GLX) and exited twice per start before falling back to software;
  with the switch the window came at 4.0-4.8 s instead of 6.9-7.2 s
  (MEASURED).
- No V8 flags since stage 28. Up to then the entry passed
  `--js-flags=--no-opt` (TurboFan off): under lxrun TurboFan crashed on a
  null node input in `GraphReducer::ReduceNode` and other graph checks
  (`CFGBuilder::ConnectBlocks`, `EscapeAnalysisReducer`), with the same
  binary run as plain Node too (stage 24, cause unknown then). Stage 28 found
  the cause by bisecting the rewritten x18 sites with the SDK-12.3 lxrun,
  which keeps the unrewritten ones correct: TurboFan dispatches a jump table
  with `adr x18; add x18, x18, x0, lsl #2; br x18` while w16 is live, and
  the `br x18` trampoline carried the target in x16. The trampoline now
  keeps every general register (`runtime/x18.c`, `plan_br`). MEASURED with
  both lxrun builds: the Node reproducer (`tests/heroic/turbofan.sh`) 10 of
  10 (0 of 3 before), `tests/heroic/phase_b.py` without the flag 2 of 2, and
  3 of 3 plus a Quit cycle with the default build. An entry installed before
  stage 28 keeps the flag until Heroic is installed again (the launcher
  writes the command at install time); it only turns TurboFan off.

From a shell, without the launcher:

```sh
scripts/install-heroic-arm64.sh                   # into $STEAMARM_STATE/armroot/opt/apps/heroic
tests/heroic/phase_b.py --cycles 2                # start, window, Settings, close, again
tests/heroic/turbofan.sh 5                        # V8's TurboFan, the binary as Node
```

## Measured (stage 24)

| step | result |
|---|---|
| start → main window mapped and viewable on `:2` | 4.0-4.8 s (7 cycles) |
| interaction: its Settings link | `#/settings/general` with General, Game Defaults, Advanced, System Information, Log |
| close: SIGTERM to its main process | every process gone in 0.4-0.5 s, exit status 0, no leftovers |
| close: its own Quit (sidebar, then QUIT) | the same, 0.5 s |
| open again | the same window, 4.5 s |
| memory | about 1.7 GB resident over 6 processes (main, 3 zygotes, network, renderer) |
| helpers | legendary 0.21.1, gogdl 1.3.0, comet 0.2.0, vulkan-helper (Vulkan 1.4 through the shim) run; nile does not |
| the launcher's session path (`session.py run`/`stop`) | window at 3.9-4.2 s; **Detener** ends it with SIGKILL after the grace period (see below), 0 processes left |

Stage 28, without `--no-opt` (TurboFan on), `tests/heroic/phase_b.py`:
window at 4.2-10.7 s over 8 cycles on both lxrun builds (4.2-5.0 s with the
default build and a quiet Mac), Settings route, every process gone
0.4-0.8 s after SIGTERM with status 0, and Heroic's own Quit 0.5 s
(MEASURED, `benchmarks/stage28-keep-x18.txt`).

## Limits and open items

- **Store sign-in, libraries, downloads, games: not verified.** Games are
  Windows (and some Linux x86) payloads; running them from the arm64 Heroic
  would need its Wine/Proton to go through FEX, which is not wired.
- **Amazon Games: unavailable.** `nile_linux_arm64` is a non-PIE (`ET_EXEC`)
  binary linked at `0x401d5c`, inside Darwin's `__PAGEZERO`; lxrun refuses it
  (MEASURED). Needs a PIE build upstream, or nile run from its Python source
  with the root's python3 (not tried).
- TurboFan: on since stage 28 (above). Whether Steam's CEF 126 had hit the
  same `br x18` bug was not measured; its libcef's `br x18` sites get the
  new trampoline too.
- **Detener is not graceful.** `session.py stop` signals the whole process
  group at once; Heroic's browser process then does not exit (MEASURED:
  still alive 15 s after), and the stop's SIGKILL ends it. SIGTERM to the
  main process alone, or Heroic's own Quit, ends it cleanly. A stop that
  signals the program first and the group after would fix it; not changed
  here, since every app's stop goes through it.
- No D-Bus session or system bus in the root: Heroic logs "Failed to connect
  to the bus". What that disables (Secret Service, the tray icon) was not
  measured.
- Chromium's X11 software presenter asks for a 4.9 MB SysV segment, above
  macOS's `kern.sysv.shmmax` (4 MiB): the runtime serves it from POSIX
  memory, which the X server cannot attach, and Chromium falls back to
  plain image transfers. The window draws (MEASURED); the cost is UNKNOWN.

## ARM64 plan

1. Done: native arm64 Heroic from official release files, installed by the
   launcher into the ARM64 root (this page).
2. Ask upstream to publish the linux arm64 build its CI already makes (a
   release asset instead of a 14-day artifact); the installer then takes it
   directly and `scripts/heroic-asar.py` goes away.
3. Done (stage 28): TurboFan's crash found in the runtime and fixed;
   `--no-opt` dropped.
4. nile: a PIE build upstream, or its Python source in the root.
5. A program-first stop in `scripts/session.py`, for every Chromium-based
   app.
6. Games from Heroic: Wine/Proton through FEX for the x86 payload, as for
   Steam (`docs/ARM64_FIRST_MIGRATION.md`, step 13).
