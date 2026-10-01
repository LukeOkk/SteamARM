# ARM64-first: the migration plan

**Target.** Everything ARM64 except game payloads. Valve's native arm64
Steam client runs directly on lxrun, with no FEX. FEX runs only x86 and i386
game code. No virtual machine at any point (`AGENTS.md`).

**Today (2026-09-29, after stage 23 and the post-merge check).** The x86
client under FEX is the default launcher entry and the only route that
reaches Steam's main window and runs games (MEASURED, stages 8-21; in the
post-merge check 4 of 5 starts reached the main window in 88-93 s, and 1
died with a host SIGTRAP in `pthread_jit_write_protect_np`, cause UNKNOWN,
`b3f64c8`). Valve's native arm64 client runs with no FEX and reaches its
"Sign in to Steam" window with V8's JIT on, on the Fedora armroot and on
the Steam Frame root, from scripts and from the launcher's two
experimental entries (MEASURED, stages 22-23; launcher: 15-16 s and 21 s in
the post-merge check). Sign-in was not attempted: the library, downloads,
games and Proton under that client are not verified, and Proton ARM64
does not run on macOS.

This page replaces five next-step lists that disagreed on the root:
`docs/CURRENT_STEAM_ENVIRONMENT.md` §10, `docs/APPLICATION_MANAGER.md`
"Next steps", `docs/STEAM_FRAME_COMPAT_TOOLS.md` §7.4,
`docs/STEAM_RUNTIME_4_ARM64.md` §5.3 and `docs/LEPTON_REUSE_ANALYSIS.md`
"Next steps". Those lists stay as the detailed notes for their area; the
order is here.

Labels: MEASURED, VERIFIED IN SOURCE, UPSTREAM DOCUMENTED, HYPOTHESIS,
UNKNOWN. Status per step: DONE (with evidence), PARTLY DONE (what is done
has evidence; the rest is open), IN PROGRESS (being worked on elsewhere,
not claimed here), OPEN.

## The root decision

| root | what it is | role |
|---|---|---|
| Fedora 43 aarch64 armroot (`scripts/mkarmroot.sh`, `/tmp/lxrt-armroot`) | the shared-library closure of what the client links (156 root packages in `scripts/mkarmroot.lock`); stages 21-23 ran on it | **DEFAULT for the native client** (was TRANSITIONAL). Both roots reach the sign-in window, and this one does it 5-7 s sooner, with 1 webhelper start instead of 2 and about 0.25 GB less (MEASURED, `benchmarks/stage23-frame-root.txt`). It is built from public, pinned packages with no device image. The Holo comparison found no better base (`docs/HOLO_CORE_VS_STEAM_FRAME.md`). The launcher still labels this base `transicional` and calls the Frame/Holo root the target (`launcher/ApplicationCore.swift:83-108`); changing that label is a separate change |
| Steam Frame root (extracted from `steamframe-oobe-repair-20260922.5153644-0.3.0.img`) | Valve's own aarch64 userspace for the client: SteamOS 0.3.0 "holo", 965 packages, glibc 2.39 (`docs/STEAM_FRAME_INVENTORY.md`) | **REFERENCE** (was TARGET). The second experimental entry, to check the client against what Valve ships: the `steamdeck_stable` branch and the SteamOS services. Owner-only; nothing from it is redistributed. Not built from `holo-core-aarch64-preview`: 81 of 887 shared package names have the same version (MEASURED, `benchmarks/stage24-holo-vs-frame.txt`) |
| `holo-core-aarch64-preview` packages (`mash-20251118.3`) | a public, unsigned, Arch-based ARM64 package repository hosted by Valve; frozen since 2026-07-10 | **NOT ADOPTED.** It has no GTK 2, which the client's `steamui.so` and `vgui2_s.so` need, and no Steam pieces (MEASURED, stage 24). It remains a public source of Arch-layout packages. Its Mesa has `swrast_dri.so` and `libGLX_indirect.so.0`, and its `gawk` is PIE |
| Fedora 43 `lxrt-root` (`scripts/mkroot-rpm.sh`) | FEX, FEXServer, test programs | KEEP for FEX and `tests/elf` until another root is proven for them |
| x86 Steam root (`steamroot`) | the x86 client and x86 Proton's world | KEEP for game payloads; its client-only parts are REMOVE_LATER |

Rules for the Frame root (`docs/STEAM_FRAME_IMAGE.md`, licence section):
extract it only from the owner's own copy of the image, use it on that Mac,
never put extracted files in the repository or the `.dmg`. Derive the
runtime root as an APFS clone; never modify the extracted copy.

## Steps, in order

Each step has an exit test. Steps 1-4 are independent of each other; from
step 5 on, each step depends on the ones before it unless its row says
otherwise.

| # | step | exit test | status |
|---|---|---|---|
| 1 | Runtime correctness for native code: poisoned rewrite sites trap | a poisoned site is `brk #1` | DONE, `dbd1657` (was `svc #1`; `tests/elf/run.sh` 37/37, MEASURED) |
| 2 | The memory guard stops guests only on real pressure | Steam survives normal memory use on a 16 GB Mac | DONE in source, `e4047ef`: `tests/launcher/safeguard.sh` 7/7 (MEASURED on the Mac, 2026-09-29, fake guests). With the new rule running, `safeguard.log` has no STOP or KILL line from 05:36 to 10:55 on 2026-09-29, across stages 22-23 and the post-merge Steam starts (MEASURED, read-only). A long Steam session with a game is not recorded |
| 3 | Regression tests for the stage 21 runtime changes (4 KiB ELF, AT_PAGESZ, FDE filter, large `sp` offsets, trampoline range, poison) | each has a test in `tests/elf` | DONE: `subpage4k` and `dlopen_self4k` (4 KiB ELF), `auxv_layout` with `libpg4k` (AT_PAGESZ), `x18_fde`, `x18_spoff`, `big_text` and `pool_near` (trampoline range), `x18_poison` (`9e7f4b9`, `8ff648e`, `bf3eff5`, `11854bd`). `tests/elf/run.sh` 64 passed, 0 failed, 0 expected failures at `5d9760a` (MEASURED) |
| 4 | Extract and inventory the Frame root | `extract` exit 0; inventory written | DONE, 2026-09-29 (MEASURED: 180,797 files, 965 packages; `docs/STEAM_FRAME_INVENTORY.md`) |
| 5 | Confirm the abort's cause on the Fedora root (E1, E1b, E2 in `docs/STEAM_ARM64_BRINGUP.md`) | E2: no `free(): invalid pointer` after the webhelper timeout | DONE for the mechanism, 2026-09-29: the E1, E1b and E1c probes confirmed it, and the root's seeds gained X locale data and glibc locales (MEASURED, `benchmarks/stage22-native-arm64-bringup.txt`). The exit test as written cannot run any more: on the rebuilt root the webhelper does not time out (E2: 27 of 27 launches reached BrowserReady), so the rescue dialog and the abort are not reached |
| 6 | Derive the ARM64 base from the Frame root and link it: `cp -cR` the extraction, point `~/SteamARM-roots/arm64root` at the clone, `scripts/env-links.sh` links `/tmp/lxrt-arm64root` | `LXRT_ROOT=/tmp/lxrt-arm64root build/lxrun /usr/bin/bash -c 'echo ok'` prints `ok` | DONE, 2026-09-29: `scripts/mkframeroot.sh` (APFS clone plus `etc/resolv.conf`, `etc/localtime`, `.lxrt-guest-env`); the exit test prints `ok` (MEASURED, `benchmarks/stage23-frame-root.txt`) |
| 7 | Adapt the derived root: move the Qualcomm Vulkan ICD aside, add SteamARM's shim with an ICD JSON, keep `tmp/` as state | `vulkaninfo` from the root lists Apple M4 through the shim | OPEN. The exit test passes with the shim on `LD_LIBRARY_PATH` (Apple M4, MoltenVK 1.4.2; MEASURED, `benchmarks/stage23-frame-root.txt` V1), but the shim is not installed in the root: the client reached its login window without it, and the client with a Vulkan device is untested. The freedreno ICD stays |
| 8 | Run the client on the derived root: the Frame image carries its own bootstrap (`usr/lib/steam/steam.tar.zst`, `steamrtarm64/` built 2026-09-15), or use the stage 21 download | the client reaches its login window | DONE, 2026-09-29, with the stage 21 download (copied from the Fedora root's home; the client moved itself to its `steamdeck_stable` build): the login window in 5 of 5 runs, GL through indirect GLX (MEASURED, `benchmarks/stage23-frame-root.txt`). The image's own bootstrap was not tried |
| 9 | The webhelper to BrowserReady and a stable UI (`docs/STEAMWEBHELPER_BRINGUP.md`) | BrowserReady, a network process that stays up, the Steam window | PARTLY DONE: BrowserReady in 7-9 s with V8's JIT on, and the sign-in window up for whole 180 s runs with a live sign-in QR code, so the network process reaches Valve's servers (MEASURED, stage 22 E18, stage 23 E5/E7). The main Steam window needs a sign-in, which was not attempted: OPEN |
| 10 | Launcher: a Steam ARM64 entry (`architecture: aarch64`, its root, `LXRT_GUEST_PAGE=4096`, its `HOME`), a persistent `/tmp` link, keys for `steamrtarm64/steam` beside `ubuntu12_32/steam` | the launcher starts and stops it; `running.arch` = `aarch64 none` | PARTLY DONE: two entries in `scripts/builtin-apps.json`, `steam-arm64` (Fedora armroot) and `steam-arm64-frame` (Steam Frame root); `scripts/env-links.sh` links `/tmp/lxrt-armroot` and `/tmp/lxrt-arm64root`. The launcher starts and stops it: the window in 9 of 10 cycles (stage 23 L1-L10), and in the post-merge check at 15-16 s (Fedora root) and 21 s (Frame root), with 0 guests left after **Detener** (MEASURED). `running.arch` is written by every `run-app.sh` launch (`"<arch> <translator>"`), and since 2026-10-01 a native client started outside the launcher (`scripts/run-steam-arm64.sh`) is adopted too: its root, read from its environment, picks the entry (`/tmp/lxrt-armroot` steam-arm64, `/tmp/lxrt-arm64root` steam-arm64-frame) and `running.arch` gets `aarch64 none` (MEASURED: adopted as steam-arm64; the Frame entry refused it as another program; `scripts/run-app.sh` `outside_client`, `launcher/ApplicationCore.swift` `OutsideClient`, launcher test). DONE |
| 11 | Acceptance of the ARM64 client | open, UI, close, library, open again | OPEN |
| 12 | SLR 4 arm64's pressure-vessel under lxrun (`docs/STEAM_RUNTIME_4_ARM64.md` §5.3 steps 5-6) | `pressure-vessel-wrap --version` and `_v2-entry-point --verb=run -- true` | OPEN |
| 13 | Route Windows games to x86 Proton through SteamARM's FEX under the arm64 client (`docs/STEAM_FRAME_COMPAT_TOOLS.md` §7.3), and keep Valve's FEX from being used | one Windows probe renders when launched by the arm64 client | PARTLY DONE, stage 24 (`docs/FEX_GAME_BOUNDARY.md`): the compatibility tool "SteamARM: Proton x86 via FEX (experimental)" (`scripts/install-fex-proton-tool.sh`) runs the D3D11 and D3D12 probes through x86 Proton Experimental and SLR 4 at about 160 fps, as the x86 client's path does, when invoked the way the client invokes a tool from the ARM64 root, on both roots; both clients register it at start (MEASURED, `benchmarks/stage24-fex-game-boundary.txt`). Open: a launch from the client's UI by the user, a real game, launches that do not exit (Xalia), and Proton 10.0 / sniper, which does not start under FEX on either path |
| 14 | Retire the x86 client's parts (`docs/CURRENT_STEAM_ENVIRONMENT.md` §6: i386 client, x86 webhelper, x86 bash, amd64 `lsof`, HideHypervisorBit) | the x86 client is no longer started | OPEN; only after 11 and 13 |

Independent of that order:

- **Holo Core vs the Frame (the choice of base).** DONE, 2026-09-29
  (`docs/HOLO_CORE_VS_STEAM_FRAME.md`, `benchmarks/stage24-holo-vs-frame.txt`).
  The Frame's 965 packages were compared with the preview's 4,560
  (MEASURED): 81 at the same version, 806 different (Holo newer for 764), 78
  only in the Frame, 3,673 only in Holo. Holo lacks only
  `libgtk-x11-2.0.so.0` of the client's 51 seed sonames. The root table
  above follows from it. Still UNKNOWN: the client on a root built from Holo
  packages; none was built.

- **Apps other than Steam: Heroic Games Launcher.** PARTLY DONE (stage 24):
  its linux-arm64 build runs natively in the Fedora ARM64 root and the
  launcher installs it that way instead of the x64 build under FEX; window,
  Settings, clean close and reopen in 7 of 7 cycles (MEASURED,
  `benchmarks/stage24-heroic.txt`). OPEN: store sign-in, downloads and games;
  Amazon's nile (non-PIE); V8's TurboFan under lxrun
  (`docs/HEROIC_INTEGRATION.md`).

- **Graphics: KosmicKrisp.** PARTLY DONE. The shim loads it in ICD mode
  when `STEAMARM_VK_ICD=kosmickrisp` (`4ccb914`), and the launcher sets that
  variable when **Gráficos → Motor** is KosmicKrisp, which it offers only
  when KosmicKrisp is detected and the installed shim reads the variable
  (`7ad4919`, `774f590`). MEASURED with probes
  (`benchmarks/stage22-kosmickrisp.txt`): aarch64, x86-64 and i386 guests
  create a device, submit, read back and present; the D3D11 and D3D12
  probes run at 158.1 and 154.1 fps once the shim reports
  `fillModeNonSolid` (`8a716c9`). OPEN: Steam's launch path
  (pressure-vessel passing the variable to a game), D3D9, 32-bit D3D and
  any game on it (`docs/GRAPHICS_BACKEND_ARCHITECTURE.md`).
- **x18 on the M4.** `tests/x18_preserve/run.sh` under macOS 27
  (`docs/X18_VIRTUALIZATION.md`). OPEN.
- **Android (Lepton).** `docs/LEPTON_REUSE_ANALYSIS.md` steps 1-3
  (alignment, one bionic program under lxrun). Steps 1-2 done on a
  Waydroid LineageOS 18.1 root (stage 25, MEASURED): 4 KiB-aligned, and
  bionic's linker, toybox and mksh run. ART does not: its heap must be
  mapped below 4 GiB, which macOS forbids
  (`docs/ANDROID_RUNTIME_ARCHITECTURE.md`); binder is the other open layer.
  The x86_64 build of the same image runs Java on x86-64 ART under FEX's
  low window (stage 25, MEASURED): a zero-VM path for Java and x86-64 apps,
  not for arm64-v8a-only ones (no native bridge in the x86 image).
  The Frame root has `usr/share/guestos/android` (57 MB) and podman 5.5.2
  (MEASURED).
  The component-by-component plan and its six stages are in
  `docs/ANDROID_ZERO_VM_FEASIBILITY.md`; Play Store in
  `docs/PLAY_STORE_RESEARCH.md` (stage 25, research only).

## Blockers

| blocker | state | where |
|---|---|---|
| Sign-in and everything after it under the native client | not attempted; the next step (steps 9 and 11) | `docs/STEAM_ARM64_BRINGUP.md` |
| A webhelper zygote dies with SIGBUS in a host `memset` at start | 1 of 10 launcher cycles in stage 23 (L2), no window in that cycle; 0 of 7 direct runs. Cause UNKNOWN; the fault report now names the host frames (`b3f64c8`) | `docs/STEAMWEBHELPER_BRINGUP.md` |
| An arm64 client prefers Proton ARM64 and SLR 4 arm64 | Proton ARM64 cannot start on macOS: `0x7ffe0000` and the low 4 GiB (MEASURED, stage 18; VERIFIED IN SOURCE, stage 19 §2), and x18 for the TEB (UNKNOWN on the M4, see above). The way around it is the stage 24 tool, which a title has to be set to (step 13) | `docs/STEAM_FRAME_COMPAT_TOOLS.md` §7, `docs/FEX_GAME_BOUNDARY.md` |
| 45 aarch64 `ET_EXEC` files in the Frame root (busybox, gawk, gcc, SteamVR's `XRService`...) | lxrun refuses `ET_EXEC` (`runtime/elf.c`). `awk` is a link to `gawk`, and the bootstrap's `steam.sh` names `awk` twice (MEASURED). Whether that path runs at start-up: UNKNOWN. The client started directly (`steamrtarm64/steam`, not `steam.sh`) reaches its sign-in window on that root | `docs/STEAM_FRAME_INVENTORY.md` |

Not an ARM64 blocker, but on the route everything still depends on: 1 of 5
x86 Steam starts in the post-merge check died with a host SIGTRAP at
`pthread_jit_write_protect_np`+388 in libsystem_pthread; cause UNKNOWN
(`b3f64c8`, which makes the next report name the host callers).

Resolved since stage 21 (MEASURED unless marked):

| was a blocker | what happened |
|---|---|
| Main client aborts with `free(): invalid pointer` | The mechanism was confirmed by probe (a Valve `vgui2_s` bug, reached when the webhelper times out on a root without X locale data), and the root gained X locale data and locales. The abort path is no longer reached (stage 22 E1-E2; step 5) |
| The webhelper never initialises (2026-09-29, 02:15) | Not reproduced after the root rebuild: 27 of 27 launches reached BrowserReady (stage 22 E2). Which change removed it is UNKNOWN |
| Poisoned libcef x18 sites killed the browser and every renderer | Rewritten (`4534360`, `c86b633`, `926b89a`; stage 22 E4-E9) |
| V8 JIT under lxrun (a HYPOTHESIS that it could not work) | Refuted: RWX pages split W^X per 16 KiB page and scanned before they run, and the host handler kept when the zygote sets SIG_DFL (stage 23); `--jitless` is no longer needed |
| The Frame root's client exited at `glXChooseVisual failed` | Indirect GLX through the X server, a `resolv.conf`, via `scripts/mkframeroot.sh` (stage 23 Frame root) |

## Measured status

`docs/ARM64_FIRST_AUDIT.md` has the numbers: processes per route, rewrite
coverage, missing root pieces, installed versus runnable tools, and the
UNKNOWNs with the command that measures each.
