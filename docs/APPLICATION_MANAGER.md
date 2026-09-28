# Applications, base environments and sessions

This page covers how the launcher turns "the user clicked an app" into
processes, what exists today, and the order in which it changes. The target
is set by the ZERO-VM mission: one managed session at a time, ARM64-first,
and Steam as one application among others.

## Today (VERIFIED IN SOURCE, 2026-09-28 audit)

- **The launch primitive.** `LauncherModel.launch` runs
  `scripts/run-app.sh <id>` (`launcher/LauncherModel.swift:190-224`). The app
  never spawns a Linux process itself. "Detener" runs the same script with
  `--stop`, which does `kill -9` on every guest `lxrun`
  (`scripts/run-app.sh`, `stop_guests`).
- **`run-app.sh`.** It resolves the entry: Steam from a dict inside the
  script, everything else from `$STATE/launcher/apps.json`. It merges the
  entry's env with `settings-env.py`, starts the X server, PulseAudio and
  `steamarm-inputd`, then starts the program. **Until this change, every
  program went through `scripts/run-fex.sh`, whatever its ISA.**
- **Tracking.**
  - A pidfile, polled every 2 s (`kill(pid,0)` or a `ps` line containing
    `build/lxrun`).
  - There is no process group, no `waitpid` and no exit status.
  - "It crashed" is inferred from "it disappeared less than 15 s after
    starting".
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

## What changed (this commit)

| target abstraction | where | state |
|---|---|---|
| Program ISA from its ELF, never its name | `ELFInspector` (`launcher/ApplicationCore.swift`); "Add app" stores it in `AppEntry.architecture` | done, tested |
| ApplicationDefinition carries the ISA | `AppEntry.architecture` (`aarch64` / `x86_64` / `i386`; nil = x86_64, so existing `apps.json` files keep working) | done |
| LinuxBaseEnvironment | `LinuxBaseEnvironment` with two built-ins: `arm64` (`/tmp/lxrt-arm64root`, no translator) and `legacy-x86` (`/tmp/lxrt-steamroot`, FEX, `transitional`) | model done; the launcher does not read it yet |
| ARM64-first launch plan | `LaunchPlanner`: aarch64 runs natively only, never through FEX; x86_64/i386 run through FEX only; `usesVirtualMachine` is always false | done, tested |
| Runner per ISA | `run-app.sh` sends aarch64 entries to the new `scripts/run-native.sh` (`build/lxrun <program>`, no `FEX_*` variables). It writes `running.arch` = `<arch> <translator>` | done, dry-run tested |
| Session state machine | `SessionMachine`: `idle → starting → running → stopping → cleanup → idle`, `starting → failed → cleanup → idle`, `running → crashed / exited → cleanup → idle`. The lock is taken before any process exists | model done, tested; `LauncherModel` still uses its own `Phase` |

Steam, Heroic and Prism are marked `x86_64`, so they behave exactly as
before. Steam's entry is labelled TRANSITIONAL_COMPATIBILITY.

Tests (they build and run on Linux too):

```sh
make test-launcher-core
```

It runs two tests:

- `launcher/tests/ApplicationCoreTests.swift`: ELF detection, launch plans,
  session transitions;
- `tests/launcher/run_app_dispatch.sh`: `run-app.sh --dry-run` for
  aarch64/x86_64/i386/Steam/invalid entries.

## Next steps, in order (all need the Mac)

1. **Build and smoke-test.** Run `make launcher test-launcher-core`. Then add
   an aarch64 test program as an app, for example the `hello_dyn` sample from
   `scripts/mkroot-rpm.sh` copied into the ARM64 root. Launch it from the
   launcher, and check its log and `running.arch = "aarch64 none"`.
2. **The ARM64 root at `/tmp/lxrt-arm64root`.** Link the root extracted from
   the Steam Frame image there (`docs/STEAM_FRAME_IMAGE.md`), from
   `scripts/env-links.sh` like the other two roots. Until then, the launcher
   must show aarch64 entries as unavailable, not failing.
3. **Exit status and a process group per session.** `run-app.sh` should start
   the program under a small wrapper. The wrapper creates a new session
   (`setsid` through python3 or perl; macOS has no `setsid` command) and
   writes `running.status` when the tree ends. Then:
   - "Detener" signals that group instead of every `lxrun`;
   - `safeguard.sh` can target it;
   - `SessionMachine.ended(status:)` gets a real value instead of the 15 s
     guess.
4. **Wire `SessionMachine` into `LauncherModel`.** It replaces `Phase`. Take
   the lock in `launch` before running the script, and release it only after
   cleanup. `RunningView` shows the architecture and translator from
   `running.arch`, and `Session Mode: ZERO-VM`.
5. **One Steam definition.** Move Steam out of the `run-app.sh` dict and
   `AppEntry.steam` into a single JSON (`$STATE/launcher/builtin.json`, or
   `apps.json` with `builtIn: true`) read by both.
6. **Steam ARM64 as its own entry.** Once the client from the Steam Frame
   root reaches a stable UI under lxrun, add it as an entry with
   `architecture: aarch64` and `root: /tmp/lxrt-arm64root`; its command comes
   from the inventory. The x86 client remains as TRANSITIONAL_COMPATIBILITY
   until the ARM64 one passes the acceptance test (open, UI, close, library,
   open again).

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
