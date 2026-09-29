# Applications, base environments and sessions

This page covers how the launcher turns "the user clicked an app" into
processes, what exists today, and the order in which it changes. The target
is set by the ZERO-VM mission: one managed session at a time, ARM64-first,
and Steam as one application among others.

**Status, 2026-09-29 (`b3f64c8`).** "Today" below is the 2026-09-28 audit,
kept as written. The table after it and the next steps are updated: the
built-in entries come from `scripts/builtin-apps.json`, the launcher runs
sessions through `SessionMachine`, and Valve's native arm64 client has two
experimental entries that reach its sign-in window from the launcher
(MEASURED, `benchmarks/stage23-native-arm64-jit.txt` L1-L10 and the
post-merge check in `benchmarks/README.md`).

## Today (VERIFIED IN SOURCE, 2026-09-28 audit)

- **The launch primitive.** `LauncherModel.launch` runs
  `scripts/run-app.sh <id>` (`launcher/LauncherModel.swift:190-224`). The app
  never spawns a Linux process itself. "Detener" runs the same script with
  `--stop`. Until the session wrapper below, that was `kill -9` on every guest
  `lxrun` (`scripts/run-app.sh`, `stop_guests`).
- **`run-app.sh`.** It resolves the entry: Steam from a dict inside the
  script, everything else from `$STATE/launcher/apps.json`. It merges the
  entry's env with `settings-env.py`, starts the X server, PulseAudio and
  `steamarm-inputd`, then starts the program. **Until this change, every
  program went through `scripts/run-fex.sh`, whatever its ISA.**
- **Tracking.**
  - A pidfile, polled every 2 s (`kill(pid,0)` or a `ps` line containing
    `build/lxrun`).
  - Until the session wrapper below: no process group, no `waitpid` and no
    exit status; "it crashed" was inferred from "it disappeared less than
    15 s after starting".
- **One app at a time.** Enforced twice: the Swift phase guard, and
  `run-app.sh` exiting 3 while any guest process exists.
- **`AppEntry` (`launcher/Models.swift`) is today's ApplicationDefinition.**
  - It had no ISA field.
  - It has a raw root path and one FEX-only field (`fexRootfs`).
  - The "Add app" flow recognised programs by file-name suffix, and checked
    only the 4 ELF magic bytes as a fallback.
- **Steam is hard-wired in three places.** These must become one:
  - `AppEntry.steam` in Swift. It is stale: `run-app.sh` never reads it.
  - `run-app.sh`: the Steam dict, the `ubuntu12_32/steam` adoption pattern,
    and the `steamui.so` check.
  - The install chain: `install-steam.sh` (i386 bootstrap), and
    `install-steamroot-gfx.sh`, which hides the hypervisor bit from FEX.

## What changed (the application-core commit, updated to `b3f64c8`)

| target abstraction | where | state |
|---|---|---|
| Program ISA from its ELF, never its name | `ELFInspector` (`launcher/ApplicationCore.swift`); "Add app" stores it in `AppEntry.architecture` | done, tested |
| ApplicationDefinition carries the ISA | `AppEntry.architecture` (`aarch64` / `x86_64` / `i386`; nil = x86_64, so existing `apps.json` files keep working) | done |
| LinuxBaseEnvironment | `LinuxBaseEnvironment` with three built-ins: `arm64` (`/tmp/lxrt-arm64root`, the Steam Frame root, no translator), `armroot` (`/tmp/lxrt-armroot`, the Fedora armroot, no translator, transitional; `1de9fd1`) and `legacy-x86` (`/tmp/lxrt-steamroot`, FEX, `transitional`) | done; the launcher reads it for an entry's "Entorno base" and for why an ARM64 entry is unavailable (`launcher/LauncherModel.swift:255-277`) |
| ARM64-first launch plan | `LaunchPlanner`: aarch64 runs natively only, never through FEX; x86_64/i386 run through FEX only; `usesVirtualMachine` is always false | done, tested |
| Runner per ISA | `run-app.sh` sends aarch64 entries to the new `scripts/run-native.sh` (`build/lxrun <program>`, no `FEX_*` variables). It writes `running.arch` = `<arch> <translator>`, and refuses an aarch64 entry in the x86 root or an x86 entry in the ARM64 base, as `LaunchPlanner` does | done, dry-run tested |
| Session state machine | `SessionMachine`: `idle → starting → running → stopping → cleanup → idle`, `starting → failed → cleanup → idle`, `running → crashed / exited → cleanup → idle`. The lock is taken before any process exists | done, tested; `LauncherModel` uses it instead of its own phase since `774f590` |
| Backends, session mode and capabilities | `ApplicationCore.swift`: `PresentationMode` (native windows / VNC) is separate from `ExecutionBackend` (lxrun / Lightning JIT / Apple Hypervisor). `ApplicationBackendPreset` maps the four presets onto them. `SessionVirtualizationMode` is `VM — Apple Hypervisor` only for that backend, and no fallback ever returns it. `RuntimeCapabilities.current` states what exists today, with its evidence: native windows ready; VNC experimental; lxrun ready; Lightning JIT and Apple Hypervisor unavailable (neither exists in the tree); esync experimental; fsync unsupported (no `futex_waitv`); MSync unavailable; MoltenVK ready; KosmicKrisp unavailable and WineD3D/OpenGL unsupported. `RuntimeCapabilities.detect(from:)` refines that table with what `scripts/compat-status.py` finds on the Mac: KosmicKrisp becomes experimental when it is installed, on macOS 26 or later, loads as an ICD and the installed shim reads `STEAMARM_VK_ICD`; esync is experimental only with an esync Proton (`1de9fd1`) | done, tested; the Settings window and its read-only Runtime page read it, and disabled options show their reason (`774f590`) |
| One process group and an exit status per session | `scripts/session.py run` (started by `run-app.sh`) puts the program and what it forks in a group of its own (`running.pgid`). A process that starts its own session (setsid: wineserver, daemons) leaves that group; `--stop`'s `kill -9` of leftover guests still catches it. How the program ended goes to `running.status`: `N` for an exit code, `N signal S` for a signal. Leftovers get 5 s, then SIGTERM, then SIGKILL. `--stop` signals the group first (`session.py stop`). FEXServer and `safeguard.sh` start in their own sessions (`session.py detach`), so a stop never takes them down. The program gets the caller's environment and signal dispositions, not Python's: no coerced `LC_CTYPE`, default SIGPIPE, and nohup's ignored SIGHUP kept. The launcher reports the status ("terminó con el código N / la señal S") and keeps the 15 s guess only for a run it adopted without a wrapper | done; `tests/launcher/session.sh` 24/24 on Linux; macOS CI 23/23 + 1 skip (`benchmarks/stage20-ci-macos-runner.txt`) |

Steam, Heroic and Prism are marked `x86_64`, so they behave exactly as
before. Steam's entry is labelled TRANSITIONAL_COMPATIBILITY.

Tests (they build and run on Linux too):

```sh
make test-launcher-core
```

It runs (Makefile target `test-launcher-core`):

- `launcher/tests/ApplicationCoreTests.swift`: ELF detection, launch plans,
  session transitions, backends and capabilities, detection, fallback
  policy, the library;
- `tests/test_settings_env.py`, `tests/test_compat_status.py`: the settings
  → environment translation and the compatibility inventory;
- `tests/launcher/run_app_dispatch.sh`: `run-app.sh --dry-run` for
  aarch64/x86_64/i386/Steam/invalid entries, the root rule and the built-in
  entries (25 passed at `9468028`);
- `tests/launcher/session.sh`: `scripts/session.py` with real processes
  (about 20 s);
- `tests/launcher/safeguard.sh`: the memory guard with fake guests;
- `tests/launcher/guest_env.sh`: a root's `.lxrt-guest-env` and
  `scripts/mkframeroot.sh` on a fake extraction.

The same target runs on every PR in `.github/workflows/macos.yml`, on a
macOS runner, after `make all`.

## Next steps, in order (all need the Mac)

The launcher's part of `docs/ARM64_FIRST_MIGRATION.md` (step 10 there).
Status at `b3f64c8` is given per step; nothing is claimed as done unless it
says so.

1. **Build and smoke-test.** PARTLY DONE: `make test-launcher-core` passes
   on the Mac (stage 23; the post-merge check), and both ARM64 Steam
   entries were started and stopped from the launcher (below). A
   `running.arch = "aarch64 none"` check is not recorded. Still to do: add
   a small aarch64 program (for example the `hello_dyn` sample from
   `scripts/mkroot-rpm.sh`) with "Añadir app", launch it from the launcher,
   and check its log and `running.arch = "aarch64 none"`.
2. **The ARM64 roots.** DONE. `/tmp/lxrt-arm64root` is the root derived
   from the Steam Frame image by `scripts/mkframeroot.sh` (an APFS clone on
   the SteamFrameRoot volume; `$STATE/arm64root` points at it while the
   volume is attached, MEASURED in `benchmarks/stage23-frame-root.txt`).
   `/tmp/lxrt-armroot` is the Fedora armroot of `scripts/mkarmroot.sh`. At
   the audit that link was made by hand; since `e8114fa`
   `scripts/env-links.sh` links both when their roots exist. An aarch64
   entry whose root or program is missing is shown as unavailable with the
   reason, not as failing (`launcher/LauncherModel.swift:255-277`).
3. **Exit status and a process group per session.** Done in
   `scripts/session.py` (see the table above). On the Mac,
   `tests/launcher/session.sh` passes (23/0 in stage 23's
   `make test-launcher-core`), and a stop of the native arm64 client
   through the launcher left 0 steam, steamwebhelper or other guest
   processes (MEASURED, stage 23 L1-L10 and the post-merge check). The same
   check for the x86 client is not recorded. `safeguard.sh` could later
   kill the recorded group before every `lxrun`.
4. **Wire `SessionMachine` into `LauncherModel`.** DONE in `774f590`
   (VERIFIED IN SOURCE, built; the lock is taken before `run-app.sh` starts
   anything, and the exit status comes from `running.status`). The running
   panel shows the architecture and translator (from `running.arch`, else
   the entry), the base environment and "Modo de sesión: ZERO-VM".
5. **"Añadir app" per ISA.** DONE in `774f590` (VERIFIED IN SOURCE, built,
   not exercised from the UI): the add flow reads the ELF before moving the
   program; aarch64 programs go to the Fedora armroot's `opt/apps` with
   root `/tmp/lxrt-armroot`, the rest to the x86 root as before
   (`launcher/AddAppView.swift:35-42`).
6. **One Steam definition.** DONE in `e8114fa`: `scripts/builtin-apps.json`
   holds the built-in entries and both `run-app.sh` and the launcher read
   it. `AppEntry.steam` stays in `launcher/Models.swift` only as the
   fallback when that file cannot be read.
7. **Steam ARM64 as its own entry.** DONE: `steam-arm64` ("Steam ARM64
   (experimental)", Fedora armroot, `e8114fa`) and `steam-arm64-frame`
   ("Steam ARM64 · Steam Frame (experimental)", Steam Frame root,
   `9468028`), both `architecture: aarch64` with `LXRT_GUEST_PAGE=4096`,
   `HOME_IN_GUEST=/tmp/armhome` and `LXRT_X18_ALL_TEXT=libcef.so` in the
   entry's environment. From the launcher the client reaches its sign-in
   window: 9 of 10 start/stop cycles in stage 23, and 15-16 s (Fedora
   root) and 21 s (Frame root) in the post-merge check (MEASURED). Sign-in
   was not attempted. The x86 client remains the default,
   TRANSITIONAL_COMPATIBILITY, until the ARM64 one passes the acceptance
   test (open, UI, close, library, open again).

## Virtual machines: not an option

ZERO-VM is mandatory (`AGENTS.md`). The VM path was removed on 2026-09-27
(`docs/history/`). In the model (VERIFIED IN SOURCE,
`launcher/ApplicationCore.swift`):

- `ExecutionBackend.appleHypervisorLegacy` exists only so the launcher can
  show Apple Hypervisor as **unavailable, with that reason** (`:528`, at
  `b3f64c8`). No runner starts a VM, and `usesVirtualMachine` is false for
  every other backend (`:319`).
- No fallback ever returns it: the execution fallback goes from Lightning
  JIT to lxrun and otherwise to nothing (`:624-628`), and the graphics
  fallback order never involves a VM (`:617-623`).
- `SessionVirtualizationMode` reads "ZERO-VM" for every session that can
  run.

A failure on the ZERO-VM path is fixed on that path, or reported as
unsupported. It is never answered by proposing or starting a VM.

## Open design questions

- **Which Proton an ARM64 Steam client picks.** Under a native ARM64 client,
  Steam sees an arm64 host and prefers Proton ARM64 and Steam Linux Runtime 4
  ARM64. Unmodified, those cannot start on macOS (benchmarks/stage19 §2). The
  current trick (FEX's HideHypervisorBit) only works while the client runs
  under FEX. The ARM64 client needs another way to run x86 Proton through FEX
  for game payloads: a compatibility-tool shim, or a patched ARM64 Proton
  (Madeira-style, stage19 §2).
- **ISA of scripts.** `steam.sh`, AppImage `AppRun` and Heroic's wrapper only
  reveal their ISA through the ELF they exec. Today such an entry stays
  x86_64 unless its definition says otherwise.
- **Settings scoped to the payload.** `settings-env.py` still exports `FEX_*`
  for every app. `run-app.sh` now drops them for aarch64 entries; the Settings
  page should say that the Processor section applies to x86 programs.
