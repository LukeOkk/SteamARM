# The Steam environment today, classified

This audit covers what runs Steam on SteamARM today, and what happens to each
piece under the ARM64-first plan. That plan: everything ARM64 except the game
payloads, and FEX only for x86/i386 game code.

**How it was made (2026-09-28, tree at `748bc97`):**

- Five readers each went through one area: `runtime/`, `scripts/` and the
  `Makefile`, `launcher/`, graphics/input/audio, and `benchmarks/` plus
  `docs/`. Every claim came with a `path:line`.
- Each area is then checked by a reviewer asked to refute it. **Status:
  draft.** That pass was still running when this was committed; the key
  citations were re-read by hand, and the pass's corrections come in a
  follow-up commit.
- Nothing here was run on a Mac; the MEASURED facts come from the benchmark
  record.
- For how a launch works in detail, see `docs/APPLICATION_MANAGER.md`.

**Evidence labels:**

| label | meaning |
|---|---|
| MEASURED | recorded as measured in `benchmarks/` or a commit message |
| VERIFIED IN SOURCE | read in this repository |
| HYPOTHESIS | inferred, not tested |
| UNKNOWN | not established |

**Tags.** Each component gets one primary tag and one x86-debt tag.

| primary tag | meaning |
|---|---|
| KEEP | stays as it is under ARM64-first |
| REPLACE_WITH_HOLO | to be taken from the Steam Frame / Holo aarch64 userspace |
| GUEST_X86_REQUIRED | x86-64 code or data that x86 game payloads need |
| GUEST_I386_REQUIRED | the same, for 32-bit games |
| THUNK_CANDIDATE | an x86 library that could be forwarded to an aarch64 host library |
| STEAM_RUNTIME_OWNED | downloaded and managed by Steam, not by SteamARM |
| REMOVE_LATER | exists only for the x86 client or a retired path |
| UNKNOWN | cannot be classified yet |

| x86-debt tag | meaning |
|---|---|
| NOT_APPLICABLE | no x86 involved |
| GAME_PAYLOAD_EXCEPTION | x86 because games are x86; allowed |
| TEMPORARY_X86_DEPENDENCY | x86 only because the Steam client is x86 today |
| REPLACE_WITH_ARM64 | should become ARM64 |
| UPSTREAM_ARM64_PENDING | waiting on Valve / upstream |

## 1. Summary

- **The whole Steam client runs as x86 code under FEX today** (MEASURED,
  `benchmarks/stage8-steam-zero-vm.txt:4-22`). That includes:
  - `ubuntu12_32/steam`, an i386 PIE;
  - `steam.sh`, run by an x86-64 bash;
  - `steamwebhelper` (CEF, x86-64);
  - the pressure-vessel tools (x86-64, all ET_EXEC, `stage7-guest-base.txt:276-320`);
  - the helper services.
- **Already ARM64:**
  - `lxrun`;
  - the Vulkan shim `libvulkan.so.1`;
  - FEX itself and the host half of the Vulkan thunks;
  - XQuartz and quartz-wm;
  - PulseAudio and `steamarm-inputd`;
  - the launcher;
  - the Fedora 43 aarch64 root.
- **The client already made an ARM64 choice on its own, and we hide it**
  (MEASURED, `stage15-steam-proton-path.txt:24-32`).
  - When the x86 client sees FEX's CPUID leaves, it decides the host is
    arm64. It then installs Proton (ARM64), Steam Linux Runtime 4.0 Arm64 and
    Valve's own FEX.
  - `patches/fex-lxrt-hide-hypervisor.patch` hides the leaves, for the
    `steam` executable only (`scripts/install-steamroot-gfx.sh:100-105`).
  - A native ARM64 client does not run under FEX, so this trick cannot apply
    to it. See §7.
- **How many client processes FEX translates today:**
  - Every guest process in Steam's tree except FEXServer. Xvnc is excluded too
    (and is aarch64 anyway), but it only runs in VNC mode.
  - MEASURED 2026-09-25, in the retired Fedora VM: Steam's tree had 11
    processes, all under FEX through binfmt. Among them were the client, six
    web helpers and the launcher service (`stage6-steam-gap.txt:4-6`).
  - Under lxrun, without the VM, the number has not been counted: UNKNOWN.
    To measure on the Mac, with Steam at the library view:
    `ps -axo command= | grep '[b]uild/lxrun' | grep -v -e Xvnc -e FEXServer | wc -l`
    (the same filter the launcher uses, `launcher/Models.swift`, `Shell.guestProcesses`).
  - Target under ARM64-first: 0 client processes. Only game payloads run under
    FEX.

## 2. Process tree today

Steam started from the launcher. Every guest process is a separate
`build/lxrun` Darwin process (`fork` is a Darwin fork, and `execve` re-execs
`lxrun`, `runtime/process.c:13-27`). When a guest execs an x86 or i386 ELF,
`lxrun` hands it to `/usr/lib/lxrt-emu/FEX` (`runtime/main.c:357-390`).

```
SteamARM.app                                         Mach-O arm64
└─ /bin/bash scripts/run-app.sh steam                macOS
   ├─ scripts/run-x11-native.sh start :2
   │    X11.bin (patched XQuartz, rootless) + quartz-wm   Mach-O arm64
   ├─ scripts/audio.sh start: pulseaudio (Homebrew)   Mach-O arm64
   ├─ scripts/input.sh start: steamarm-inputd         Mach-O arm64
   └─ scripts/run-fex.sh  (safeguard.sh watchdog: macOS script)
      ├─ lxrun FEXServer --persistent=0               aarch64 ELF; exists only for x86
      │    (LXRT_ROOT=/tmp/lxrt-root, run-fex.sh:61-66)
      └─ lxrun FEX-gb /bin/bash …/Steam/steam.sh -noverifyfiles   (run-fex.sh:73)
         x86-64 bash under FEX, LXRT_ROOT=/tmp/lxrt-steamroot, FEX_ROOTFS=/
         └─ ubuntu12_32/steam                         i386 under FEX (guest base)
            ├─ steam-runtime-check-requirements, launcher service, lsof (amd64)
            ├─ pressure-vessel-wrap → pv-adverb       x86-64 ET_EXEC (low window)
            │  └─ steamwebhelper + zygote, renderers, GPU and network processes
            │                                          x86-64 CEF under FEX
            └─ game: reaper → SteamLinuxRuntime_4/_v2-entry-point
               → pressure-vessel (bwrap interpreted by runtime/mounts.c) → pv-adverb
               → proton (python3) → wine/wineserver (x86-64, or i386 for 32-bit)
               → DXVK / VKD3D-Proton → libvulkan-guest.so (x86 thunk)
               → libvulkan-host.so (aarch64) → shim libvulkan.so.1 → MoltenVK → Metal
```

Sources:

- `scripts/run-app.sh` (launch block);
- `scripts/run-fex.sh:31-73`;
- `benchmarks/stage15-steam-proton-path.txt:8-12`;
- `benchmarks/stage10-vulkan-thunks.txt:17-27`.

`scripts/run-steam.sh` is the command-line twin of the same chain.

## 3. Roots, homes and fixed paths

| path | what it is | ISA | tag |
|---|---|---|---|
| `~/SteamARM-roots/lxrt-root` → `/tmp/lxrt-root` | Fedora 43 aarch64 root, built from 66 root RPMs (plus 5 build-only) pinned in `scripts/mkroot-rpm.lock` (`stage9-vmfree-build.txt:427-429`). It holds glibc, bash, the X client libraries, Xvnc + Mesa swrast, and FEX + FEXServer. | aarch64 | REPLACE_WITH_HOLO (keep it until the Holo tree is inventoried: FEX, FEXServer and `tests/elf` run from it) |
| `~/SteamARM-roots/x86-rootfs` | FEX's Ubuntu 24.04 x86-64 image from rootfs.fex-emu.gg (`scripts/fetch-x86-rootfs.sh`). Adds i386 GTK2/libXtst for the client, and amd64 `lsof` + libtirpc for the client's port check. | x86-64 + i386 | GUEST_X86_REQUIRED / GAME_PAYLOAD_EXCEPTION (the client-only additions are REMOVE_LATER) |
| `~/SteamARM-roots/steamroot` → `/tmp/lxrt-steamroot` | The x86-64 tree cloned as `/`, plus the aarch64 emulator side in `/usr/lib/lxrt-emu` (`scripts/mksteamroot.sh:5-10`). `/` is x86 because pressure-vessel inspects the host through file descriptors and captures host libraries. | mixed | REPLACE_WITH_HOLO / REPLACE_WITH_ARM64 |
| `steamroot/tmp/fexhome` (guest `/tmp/fexhome`) | Guest `HOME` (`run-fex.sh:31`; passwd user `steam`, `mksteamroot.sh:61-64`). Holds the Steam install, `.fex-emu`, and FEX's rootfs clone `RootFS/Ubuntu_24_04`. | data | KEEP the convention. This path is baked into Steam's configuration and symlinks (`scripts/env-links.sh:2-4`). |
| `/usr/lib/lxrt-emu` (inside the Steam root) | The only host directory visible inside pressure-vessel containers. Holds `FEX-emu`, the thunks, the shim and the aarch64 X libraries (`runtime/mounts.c:283-312`). | aarch64 | KEEP for game payloads. The X libraries are REPLACE_WITH_HOLO. |
| `~/SteamARM-roots/samples` → `/tmp/lxrt-samples` | aarch64 test programs | aarch64 | KEEP |
| `/tmp/lxrt-arm64root` | The ARM64 base. It is to be linked to the root extracted from the Steam Frame image (`docs/STEAM_FRAME_IMAGE.md`). Nothing creates it yet. | aarch64 | new, REPLACE_WITH_HOLO target |
| `$STEAMARM_STATE/launcher/` | `apps.json`, `settings.json`, `running.{pid,id,display,arch}` | data | KEEP |
| `/tmp/lxrt-shm-<uid>`, `/tmp/lxrt-sig`, `/tmp/lxrt-input` | Host directories behind `/dev/shm`, the cross-process signal mailboxes and the evdev sockets | host | KEEP |

## 4. Lifecycle, in one paragraph

This is VERIFIED IN SOURCE; details in `docs/APPLICATION_MANAGER.md`.

- The launcher runs `scripts/run-app.sh <id>`. The script starts X, PulseAudio
  and inputd, then starts the program with `nohup`.
- Tracking is weak:
  - a pidfile, polled every 2 s;
  - plus "some `build/lxrun` process still exists";
  - no process group, no exit status;
  - a crash is guessed as "gone within 15 s".
- "Detener" is `kill -9` on every guest `lxrun`.
- Only one app runs at a time.
- Since `748bc97`, entries marked `aarch64` go to `scripts/run-native.sh`
  (lxrun, no FEX). Steam, Heroic and Prism are still marked `x86_64`, so in
  practice everything still goes through FEX.

## 5. Components

### 5.1 Runtime (`runtime/`, host Mach-O arm64)

| component | tag | x86 debt | note |
|---|---|---|---|
| Core: ELF loader, section-aware rewriting and trampolines, x18 virtualisation, TLS in a TSD slot, synthetic system registers, the Linux-shaped stack, vDSO. Files: `main.c`, `elf.c`, `elfsect.c`, `rewrite.c`, `trampoline.S`, `x18.c`, `tls.c`, `sysreg.c`, `stack.c`, `vdso*`. | KEEP | NOT_APPLICABLE | Any aarch64 Linux code needs these, including an ARM64 client, SLR and CEF. `elf.c` refuses ET_EXEC; aarch64 images surveyed so far are PIE (`stage2-elf-survey.txt:8-13`). |
| The Linux API on Darwin: `dispatch.c`, `thread.c`, `futex_ops.c`, `epoll_eventfd.c`, `inotify.c`, `timerfd_signalfd.c`, `fileops2.c`, `socket.c`, `signal.c`, `sysv_ipc.c`, `process.c`, `procfs.c`, `procpid.c`, `proc_ext.c`, `sysfs.c`, `privmap.c`, `jit.c` | KEEP | TEMPORARY_X86_DEPENDENCY in places (see §7) | Generic. Its FEX-specific paths are the mmap/mprotect/brk/prctl cases, the binfmt hand-off, `/proc/self/exe`, the `procfs` untranslate and signal diagnostics. |
| `mounts.c` (pressure-vessel's bwrap plan interpreted in-process) | KEEP | TEMPORARY_X86_DEPENDENCY | HYPOTHESIS: SLR Arm64 uses the same pressure-vessel. The plan it was checked against came from x86 SLR 3 (`stage6-bwrap-plan.txt`). The "emulator binds" (`mounts.c:283-312`) are FEX-specific. |
| `subpage.c` (4 KiB guest pages inside 16 KiB host pages), `shmirror.c` | KEEP | GAME_PAYLOAD_EXCEPTION | x86 images use `p_align` 0x1000; aarch64 ones measured so far use 0x10000 (`stage2-elf-alignment.txt:10-29`). The shim itself is linked with 4 KiB pages (`dispatch.c:1188-1190`). Rebuilding it with 16 KiB segments would remove the one ARM64-side user. |
| `gbase.c` (32-bit guest base; pointer rebasing for about 120 syscalls; fix-up of low-pointer faults) | GUEST_I386_REQUIRED | GAME_PAYLOAD_EXCEPTION | Also serves the 64-bit low window for x86-64 ET_EXEC tools and Wine. |
| `evdev.c`, `window.m`, `remote_layer.m` | KEEP | NOT_APPLICABLE | `window.m` (`LXRT_NR_WINDOW`) is used only by `tests/elf/vk_present.c` and `vk_triangle.c`. It is a REMOVE_LATER candidate once those tests present through X. |

### 5.2 Build and install (`Makefile`, `scripts/`)

| component | tag | x86 debt | note |
|---|---|---|---|
| `Makefile` (lxrun, vdso, shim, inputd, launcher) | KEEP | NOT_APPLICABLE | Everything it builds is arm64. |
| `scripts/setup.sh` (ten steps) | KEEP | TEMPORARY_X86_DEPENDENCY | Its `steam` step is x86: `install-steam.sh`, then the thunks, then `install-steamroot-gfx.sh`. |
| `scripts/mkroot-rpm.sh` + `.lock`, `toolchain-aarch64-linux-fedora.cmake`, the Fedora sysroot | KEEP | NOT_APPLICABLE | The same unpacking technique (no scriptlets, relative symlinks) would work for Holo packages. |
| Fedora root: Xvnc + closure, xkbcomp/xkeyboard-config, Mesa swrast | REMOVE_LATER | NOT_APPLICABLE | Used only by the VNC display mode, which cannot show the Metal layer (`stage12-native-present.txt:37`). |
| `scripts/build-fex-host.sh` (FEX `08f451d3b` + 7 patches, `-ffixed-x18`, relinked as FEX-emu) | KEEP | GAME_PAYLOAD_EXCEPTION | |
| `scripts/build-fex-thunks.sh` + the Ubuntu 24.04 x86-64/i386 dev sysroots | KEEP | GAME_PAYLOAD_EXCEPTION | Builds Vulkan thunks only (64-bit and 32-bit). No other library is thunked. |
| `scripts/fetch-x86-rootfs.sh` | GUEST_X86_REQUIRED | GAME_PAYLOAD_EXCEPTION | It also disables 8 Mesa ICD manifests so the x86 Vulkan loader sees only lavapipe (bypassed by the thunk overlay). |
| `scripts/mksteamroot.sh` | REPLACE_WITH_HOLO | REPLACE_WITH_ARM64 | Exists for the x86-as-`/` design. |
| `scripts/install-steam.sh` (`steam_latest.deb` → `bootstraplinux_ubuntu12_32.tar.xz`) | REPLACE_WITH_HOLO | REPLACE_WITH_ARM64 | No arm64 bootstrap URL exists anywhere in the repo. The ARM64 client comes from the Steam Frame image instead. |
| `scripts/install-steamroot-gfx.sh`: install of shim, thunks and X libraries | KEEP | GAME_PAYLOAD_EXCEPTION | |
| `scripts/install-steamroot-gfx.sh:99-110`: FEX `AppConfig` for `steam` (HideHypervisorBit) and for `steamwebhelper` (Vulkan thunks off) | REMOVE_LATER | TEMPORARY_X86_DEPENDENCY | Keyed on the x86 client's executable names. |
| `scripts/run-app.sh`, `scripts/settings-env.py` | KEEP | TEMPORARY_X86_DEPENDENCY | `settings-env.py` exports `FEX_*` and the Proton knobs to every app. `run-app.sh` now drops `FEX_*` for aarch64 entries. |
| `scripts/run-fex.sh` | KEEP | GAME_PAYLOAD_EXCEPTION | For x86 payloads. `scripts/run-native.sh` is the aarch64 counterpart. |
| `scripts/run-steam.sh` | REMOVE_LATER | TEMPORARY_X86_DEPENDENCY | A command-line duplicate of `run-app.sh steam`. |
| `scripts/run-x11-native.sh`, `audio.sh`, `input.sh`, `safeguard.sh`, `env-links.sh`, `make-release.sh`, `cdp.py`, `default-output.py`, `make-icon.py`, `make-icns.sh` | KEEP | NOT_APPLICABLE | `safeguard.sh` also restarts FEXServer (`:85-92`), which only x86 payloads need. |
| `scripts/vnc_auth.py`, `vnc_click.py`, `vnc_snapshot.py` | REMOVE_LATER | NOT_APPLICABLE | VNC mode only. |
| `scripts/steamframe-image.py`, `tests/steamframe_image`, `tests/x18_preserve` | KEEP | NOT_APPLICABLE | The ARM64 base tooling. |

### 5.3 Valve components (downloaded by Steam, or by `install-steam.sh`)

| component | ISA | tag | x86 debt |
|---|---|---|---|
| Steam client: `ubuntu12_32/steam`, `steamui.so`, the scout runtime libraries | i386 | REPLACE_WITH_HOLO | TEMPORARY_X86_DEPENDENCY |
| `steamwebhelper` (CEF) and its libraries | x86-64 | REPLACE_WITH_HOLO | TEMPORARY_X86_DEPENDENCY |
| `steam.sh`, `setup.sh`, `steam-runtime-check-requirements`, the launcher service | x86-64 bash / ELF | REPLACE_WITH_HOLO | TEMPORARY_X86_DEPENDENCY |
| Steam Linux Runtime 4.0 (pressure-vessel, `srt-bwrap`, `pv-adverb`, python3; all ET_EXEC) | x86-64 | STEAM_RUNTIME_OWNED | UPSTREAM_ARM64_PENDING. The x86 SLR is needed for as long as x86 Proton runs inside it. SLR 4.0 Arm64 exists, but has not been measured here. |
| Proton Experimental (Wine, DXVK, VKD3D-Proton) | x86-64 + i386 | STEAM_RUNTIME_OWNED | GAME_PAYLOAD_EXCEPTION |
| Proton (ARM64) | aarch64 | STEAM_RUNTIME_OWNED | Cannot start unmodified on macOS (`stage19` §2). Hidden today. |

### 5.4 Graphics, presentation, FEX patches

| component | tag | x86 debt | note |
|---|---|---|---|
| Shim `build/libvulkan.so.1`: `vulkan_shim.{c,S}`, `gen.py`, `wsi.c`, `features.c`, `fallback.c`, `memcap.c` | KEEP | NOT_APPLICABLE | An aarch64 caller links the shim directly, with no thunk (MEASURED, `stage4-shim.txt:29-32`; `tests/elf/run.sh`, check 12). The feature spoofs work around MoltenVK gaps and do not depend on the ISA. MoltenVK is loaded by absolute path, with no ICD JSON (`shim/gen.py:75-78`). |
| `shim/gen_rebase.py` → `vk_rebase.c` | GUEST_X86_REQUIRED | GAME_PAYLOAD_EXCEPTION | With no guest base these wrappers only forward, so they cost nothing for aarch64 callers. |
| `shim/map32.c` | GUEST_I386_REQUIRED | GAME_PAYLOAD_EXCEPTION | Passes calls through unless FEX reports a 32-bit guest base. |
| FEX Vulkan thunks: guest `libvulkan-guest.so` (x86-64 and i386), host `libvulkan-host.so` (aarch64), `thunkgen` | GUEST_X86_REQUIRED / GUEST_I386_REQUIRED | GAME_PAYLOAD_EXCEPTION | |
| x86 rootfs Vulkan loader + lavapipe | GUEST_X86_REQUIRED | GAME_PAYLOAD_EXCEPTION | The thunk overlay bypasses it (`stage10-vulkan-thunks.txt:11-12`). |
| FEX patches: `guest-base`, `thunk-args`, `wx` + `LxrtJit.h`, `x18`, `thunkgen-macos`, `shebang` | KEEP | GAME_PAYLOAD_EXCEPTION | `guest-base` also forces the low window for executables whose names start with `wine` (`:1179-1180`). `shebang` is TEMPORARY_X86_DEPENDENCY: whether an ARM64 SLR still runs x86 scripts under FEX is UNKNOWN. |
| FEX patches: `guest-reserve`, `thunkgen-32bit`, `thunks-guestbase32` | GUEST_I386_REQUIRED | GAME_PAYLOAD_EXCEPTION | |
| `patches/fex-lxrt-hide-hypervisor.patch` | REMOVE_LATER | TEMPORARY_X86_DEPENDENCY | Its only purpose is to steer the x86 client's choice of Proton and SLR. |
| XQuartz 21.1.24 + `xquartz-remote-layer`, `-signals-to-server-thread`, `-log-file-env` patches; quartz-wm + `quartz-wm-picture`; `run-x11-native.sh` | KEEP | NOT_APPLICABLE | Cross-process CALayerHost presentation, no per-frame copy (MEASURED, `stage12-native-present.txt`). |
| aarch64 X client libraries copied into `/usr/lib/lxrt-emu` (from the Fedora root) | REPLACE_WITH_HOLO | NOT_APPLICABLE | UNKNOWN whether Holo's libxcb ABI matches what `wsi.c` declares by hand. |
| Xvnc mode (display `:1`, Screen Sharing) | REMOVE_LATER | NOT_APPLICABLE | Games cannot present through it. |

### 5.5 Input, audio, sync

| component | tag | x86 debt | note |
|---|---|---|---|
| `steamarm-inputd` (SDL, IOKit) + `runtime/evdev.c` + `tools/inputd/PROTOCOL.md` | KEEP | NOT_APPLICABLE | |
| PulseAudio (Homebrew), socket at `$ROOT/tmp/pulse/native`; `runtime/pathfd.c` stand-in for O_PATH | KEEP | NOT_APPLICABLE | The socket lives inside the Steam root so pressure-vessel can bind it. With two roots, where it lives is an open question. |
| eventfd, futex WAIT/WAKE/BITSET/REQUEUE | KEEP | NOT_APPLICABLE | The esync substrate. |
| `futex_waitv` (fsync), ntsync | UNKNOWN | — | `futex_waitv` (449) is in the guest-base pointer table (`runtime/gbase.c:102`); whether it is implemented was not checked. ntsync does not exist. The esync/fsync toggles only set `PROTON_NO_ESYNC`/`PROTON_NO_FSYNC` (`settings-env.py:71-74`). |

### 5.6 Launcher (`launcher/`, Mach-O arm64)

| component | tag | x86 debt | note |
|---|---|---|---|
| `SteamARMApp`, `HomeView`, `Settings*`, `Hotkeys`, `Controllers*`, `SDLShim.h`, `Info.plist.in`, tests | KEEP | NOT_APPLICABLE | |
| `LauncherModel.swift` | KEEP | TEMPORARY_X86_DEPENDENCY | Adopts a running Steam by the ps substring `ubuntu12_32/steam ` (`:288`). `needsSetup` checks for the x86 `steam.sh`. |
| `Models.swift` | KEEP | TEMPORARY_X86_DEPENDENCY | Has a stale copy of Steam's definition in `AppEntry.steam` (`:72-75`), which `run-app.sh` never reads. `Paths.guestRoot` = `/tmp/lxrt-steamroot` for every app. |
| `Installers.swift` | KEEP | REPLACE_WITH_ARM64 | Heroic and Prism deliberately pick x86-64 assets. Whether usable aarch64 builds exist is UNKNOWN. |
| `AddAppView.swift` | KEEP | NOT_APPLICABLE since `748bc97` | Now records the ISA from `e_machine` (`ELFInspector`). |
| `ApplicationCore.swift` (new) | KEEP | NOT_APPLICABLE | Holds `LinuxBaseEnvironment`, `LaunchPlanner` and `SessionMachine`. |
| `SETTINGS_IMPLEMENTATION.md` | REMOVE_LATER | NOT_APPLICABLE | Stale at `:44` and `:46`. |

### 5.7 Record and docs

`benchmarks/`, `docs/`, `README.md` and `NOTICE` are KEEP.

- `docs/history/` is the retired VM design, kept as history.
- Three references in `benchmarks/README.md` point to deleted files:
  - `resources/SteamARM.entitlements`;
  - `frametimes.py` and `capture.sh`;
  - the libkrun vmexit stats.

  These are REMOVE_LATER.

## 6. x86 code that is not a game payload

Everything below has to go for "everything ARM64 except game payloads".

| x86 piece | why it exists | what replaces it |
|---|---|---|
| i386 client `ubuntu12_32/steam` + `steamui.so` + scout i386 libraries | Valve's Linux client | the ARM64 client from the Steam Frame image |
| x86-64 `steamwebhelper` (CEF) | Steam's UI | ARM64 CEF from the same image |
| x86-64 bash running `steam.sh`, `setup.sh`, `steam-runtime-check-requirements`, the launcher service | the bootstrap scripts | the image's aarch64 shell and tools |
| amd64 `lsof` + libtirpc (`fetch-x86-rootfs.sh:40-47`) | the client checks its websocket peer with `lsof -i TCP@127.0.0.1:<port>` (MEASURED, `stage8-steam-zero-vm.txt:296-300`) | an aarch64 `lsof`. HYPOTHESIS: the ARM64 client runs the same check. |
| i386 GTK2 + libXtst (`fetch-x86-rootfs.sh:35-39`) | the i386 client's UI | nothing |
| x86-64 pressure-vessel tools for **steamwebhelper's** container | the client starts its UI through pressure-vessel (MEASURED, `stage6-steam-gap.txt:57-58`) | SLR Arm64 for the client. The x86 SLR stays for x86 Proton. |
| FEXServer started on the **client's** launch path (`run-fex.sh:61-66`) | FEX needs it | started on the first x86 payload instead (TEMPORARY until the client is ARM64) |
| Xvnc's `xkbcomp` running an x86 bash (`run-steam.sh:62-70`) | VNC mode | removed with VNC mode |

**No THUNK_CANDIDATE is in use today.** Only Vulkan is thunked. Under x86
Proton, the game payload loads x86 X11/xcb, PulseAudio and SDL libraries from
the FEX rootfs.

- HYPOTHESIS: forwarding X11/xcb and libpulse to their aarch64 versions would
  cut FEX time in Wine's display and audio paths.
- UNKNOWN: whether the pinned FEX still ships generators for them. Not
  measured; not a priority while real games are unverified.

## 7. Where Steam is hard-wired, and what the ARM64 client changes

The launch and stop paths recognise Steam through the i386 client:

- the pattern `build/lxrun .*ubuntu12_32/steam ` in `run-app.sh:36`,
  `run-steam.sh`, `install-steamroot-gfx.sh:38` and `LauncherModel.swift:288`;
- the `steamui.so` test that decides `-noverifyfiles` (`run-app.sh:86-90`);
- the command `/bin/bash /tmp/fexhome/.local/share/Steam/steam.sh`
  (`run-app.sh:73`, `Models.swift:74`);
- removal of `tmp/fexhome/.steam/steam.pid` on stop (`run-app.sh:54`);
- `needsSetup` (`LauncherModel.swift:91`).

All of these must key on the ARM64 client's paths once they are known from the
image inventory (UNKNOWN today).

Behaviours of the Steam client that the runtime handles specially
(VERIFIED IN SOURCE; each was MEASURED when it was added):

| special case | where | needed by an ARM64 client? |
|---|---|---|
| `/dev/shm` → `/tmp/lxrt-shm-<uid>` | `dispatch.c:384-398` | yes (Darwin) |
| `set_robust_list` kept for the webhelper's shared-memory check | `dispatch.c:2197-2207` | yes |
| `PR_SET_MM_MAP` rewrites `/proc/<pid>/cmdline` (the client checks its UI's peer process by it) | `dispatch.c:2635-2647` | yes |
| SA_RESTART emulation (webhelper "Error 4 locking shared memory file") | `dispatch.c:3287-3315` | yes |
| SysV IPC with a 10.8 MB segment above Darwin's shmmax | `sysv_ipc.c` | yes (`libtier0_s.so`) |
| SEQPACKET → DGRAM for Chromium IPC | `socket.c:267-273` | yes |
| netlink uevent socket offered only inside bwrap containers (outside one, the client's window never appeared) | `socket.c:282-321` | UNKNOWN: tuned against the x86 client's udev threads |
| `/proc/<pid>/fd`, `/proc/net/tcp` for `lsof` | `procpid.c` | yes, with an aarch64 `lsof` |
| `max_user_namespaces = 0`, so CEF starts with `--no-sandbox` | `proc_ext.c:1171-1181` | yes |
| mprotect RWX outside MAP_JIT granted as **RW** (x86 V8 under FEX never executes that memory) | `dispatch.c:1303-1315` | **no, and harmful**: a native ARM64 V8 executes its own JIT output, so an RWX request turned into RW faults on the first call. HYPOTHESIS: the first ARM64 steamwebhelper run hits this. Ways out: map RWX requests to MAP_JIT and flip W^X on the fault, or start CEF with `--js-flags=--jitless` as a fallback that costs speed. |
| `PR_SET_MEM_MODEL` → EINVAL, so FEX keeps TSO | `dispatch.c:2648-2658` | no for the client; yes for FEX payloads |
| `/proc/cpuinfo` untranslate, so FEX recognises it | `procfs.c:451-457` | no for the client; yes for FEX payloads |
| AppKit started lazily; `OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES` on every exec | `window.m:104-135`, `process.c:138-161` | yes. UNKNOWN whether an ARM64 webhelper that loads the shim directly hits the ObjC fork kill that `steamwebhelper.json` avoids today. |
| AT_PAGESZ 16384 | `stack.c:38-43` | yes. UNKNOWN whether the ARM64 client and CEF accept 16 KiB pages (the Steam Frame kernel's page size is not recorded here). |

**The Proton-selection problem turns around.**

- Today the x86 client is made to believe it runs on x86 so that it offers
  x86 Proton.
- A native ARM64 client knows it runs on arm64. It will offer Proton (ARM64)
  and SLR Arm64, which cannot start unmodified on macOS (`stage19` §2).
- There are two ways out (`docs/APPLICATION_MANAGER.md`, open questions):
  - a compatibility-tool shim that runs x86 Proton through FEX;
  - a patched Wine in the style of Madeira.
- Also UNKNOWN: whether the client then tries to install Valve's own FEX,
  which would conflict with SteamARM's patched one.

## 8. Workarounds and whether they carry over

| workaround | where | ARM64 client |
|---|---|---|
| `svc` rewriting, TPIDR_EL0 in a TSD slot, x18 virtualisation, sysreg rewriting | runtime core | **yes**. Prebuilt ARM64 libraries use x18 (libcef `blr x18`, `stage19` §3a). `tests/x18_preserve` checks whether a macOS SDK < 13 binary keeps x18 (`stage19` §3a). |
| Guest-driven W^X flip (`0x4C580020`), RWX → MAP_JIT | `jit.c`, FEX `wx` patch | the mechanism, yes. UNKNOWN whether ARM64 V8 can use it without patching (the client's JIT does not know the private syscall). |
| Sub-page (4 KiB on 16 KiB) | `subpage.c` | mostly no. HYPOTHESIS: Holo images use 64 KiB alignment, to be checked by `steamframe-image.py inventory`. |
| Guest base / low window, pointer rebasing, 32-bit thunks, `map32` | `gbase.c`, FEX patches, shim | no for the client; yes for x86/i386 games |
| bwrap interpreter | `mounts.c` | yes (HYPOTHESIS: SLR Arm64 is pressure-vessel too) |
| HideHypervisorBit for `steam` | `install-steamroot-gfx.sh:104-105` + patch | no (not under FEX); the problem it solved turns around (§7) |
| No Vulkan thunks in `steamwebhelper` | `install-steamroot-gfx.sh:106-110` | UNKNOWN (see the ObjC fork row in §7) |
| `-noverifyfiles` only once `steamui.so` exists | `run-app.sh:86-90` | UNKNOWN: depends on whether the image's client is complete |
| FEXServer `--persistent=0`, restarted by `safeguard.sh` | `run-fex.sh`, `safeguard.sh:85-92` | no for the client; yes for games |
| PulseAudio socket inside the root, O_PATH stand-in | `audio.sh`, `pathfd.c` | yes |
| passwd/group/machine-id, empty `/dev/input` | `mksteamroot.sh:56-82` | yes, in the ARM64 root |
| Replace files by rename, never in place | `Makefile:56-60` | yes |
| Memory and process watchdog | `safeguard.sh` | yes |

## 9. Open questions

1. Does the Steam Frame image hold a complete, runnable ARM64 client? If so,
   at what path, and does its `steamwebhelper` get past BrowserReady and the
   NSS network-process FATAL under lxrun? This is next on the Mac, using
   `docs/STEAM_FRAME_IMAGE.md`.
2. Are all ELFs in that tree PIE and 64 KiB-aligned? `steamframe-image.py
   inventory` answers this.
3. Does SLR Arm64's pressure-vessel emit the bwrap plan `mounts.c` handles?
   Does it expect Valve's FEX, or binfmt, for x86 games?
4. How is x86 Proton offered to an ARM64 client (§7)?
5. Is `futex_waitv` implemented, and so does the fsync toggle mean anything?
6. MoltenVK has no version pin: it comes from Homebrew and is measured only on
   1.4.2.
7. Real games are not verified (`README.md`). Known defects that remain open:
   - "Abort trap: 6" after the `d3d11_32` probe, in 2 of about 25 runs
     (`stage17-clean-install.txt:51-54`);
   - "corrupted double-linked list" when a container exits (`stage15-steam-proton-path.txt:96-98`).

## 10. What this changes next

In order:

1. On the Mac, extract and inventory the Steam Frame root. Link it to
   `/tmp/lxrt-arm64root`.
2. Run the image's client with `scripts/run-native.sh`, as an `aarch64`
   launcher entry. Expect the RWX→RW rule (§7) to be the first runtime change
   an ARM64 CEF needs.
3. Replace the `ubuntu12_32/steam` pattern, the `steamui.so` test and
   `needsSetup` with keys taken from the inventory.
4. Classify the Holo tree with the same tags, in
   `docs/HOLO_CORE_ARM64_AUDIT.md`.
