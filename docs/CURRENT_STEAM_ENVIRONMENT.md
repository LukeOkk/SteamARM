# The Steam environment today, classified

This audit covers what runs Steam on SteamARM today, and what happens to each
piece under the ARM64-first plan. That plan: everything ARM64 except the game
payloads, and FEX only for x86/i386 game code.

**How it was made (2026-09-28, tree at `748bc97`):**

- Five readers each went through one area: `runtime/`, `scripts/` and the
  `Makefile`, `launcher/`, graphics/input/audio, and `benchmarks/` plus
  `docs/`. Every claim came with a `path:line`.
- Each area was then checked by a reviewer asked to refute it, and a last
  reviewer looked for what all of them missed. Their corrections are applied
  here. They also found four bugs, now fixed:
  - `safeguard.sh` started FEXServer for ARM64 sessions;
  - `audio.sh` had no socket in the ARM64 root;
  - `run-app.sh` accepted aarch64 entries in the x86 root;
  - `docs/ARCHITECTURE.md` described the memory-ordering (TSO) setting
    backwards.
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
  - Two measurements disagree on how far the x86 client goes. Commit
    `06cbc41` (later, when an "ARM64 Protons" toggle was tried) says that with
    the leaves visible the x86 client registers no ARM64 Proton, and only
    Valve's native arm64 client lists them. Both are in the record; the
    native client's behaviour is what matters from here on.
- **How many client processes FEX translates today:**
  - Every guest process in Steam's tree except FEXServer. Xvnc is excluded too
    (and is aarch64 anyway), but it only runs in VNC mode.
  - MEASURED 2026-09-25, in the retired Fedora VM: Steam's tree had 11
    processes, all under FEX through binfmt. Among them were the client, six
    web helpers and the launcher service (`stage6-steam-gap.txt:4-6`).
  - Every client image lxrun loaded in the newest x86 Steam logs was FEX:
    36 of 36 in `steam-20260929-053409.log` and `steam-20260929-052749.log`
    (MEASURED, audit 2026-09-29). A lower bound: the bwrap-interpreted
    `FEX-emu` execs (webhelper container, games) are not logged there.
  - Under lxrun, without the VM, the live process count has not been
    counted: UNKNOWN.
    From the launch chain, at least 7 at the library view: bash, the client,
    the launcher service, pv-adverb, the webhelper browser, its zygote and one
    renderer. HYPOTHESIS: 8-12 with the GPU, network and utility processes.
    To measure on the Mac, with Steam at the library view:
    `ps -axo command= | grep '[b]uild/lxrun' | grep -v -e Xvnc -e FEXServer | wc -l`
    (the same filter the launcher uses, `launcher/Models.swift`, `Shell.guestProcesses`).
  - Target under ARM64-first: 0 client processes. Only game payloads run under
    FEX.

## 2. Process tree today

Steam started from the launcher. Every guest process is a separate
`build/lxrun` Darwin process (`fork` is a Darwin fork, and `execve` re-execs
`lxrun`, `runtime/process.c:13-27`). An x86 program's own execs go through
FEX re-executing itself (`process.c:92-113`). When an aarch64 guest, or the
bwrap interpreter, execs an x86 or i386 ELF, `lxrun` hands it to
`/usr/lib/lxrt-emu/FEX`, the way binfmt_misc does on Linux
(`runtime/main.c:387-420`).

```
SteamARM.app                                         Mach-O arm64
└─ /bin/bash scripts/run-app.sh steam                macOS
   ├─ scripts/run-x11-native.sh start :2
   │    X11.bin (patched XQuartz, rootless) + quartz-wm   Mach-O arm64
   ├─ scripts/audio.sh start: pulseaudio (Homebrew)   Mach-O arm64
   ├─ scripts/input.sh start: steamarm-inputd         Mach-O arm64
   └─ scripts/run-fex.sh  (safeguard.sh watchdog: macOS script)
      └─ lxrun FEX-gb /bin/bash …/Steam/steam.sh -noverifyfiles   (run-fex.sh:73)
         x86-64 bash under FEX, LXRT_ROOT=/tmp/lxrt-steamroot, FEX_ROOTFS=/
         └─ ubuntu12_32/steam                         i386 under FEX (guest base)
            ├─ steam-runtime-check-requirements, launcher service, lsof (amd64)
            ├─ pressure-vessel-wrap = pv-adverb       x86-64 ET_EXEC (low window); one PID:
            │                                          the bwrap exec is interpreted, then replaced
            │  └─ steamwebhelper + zygote, renderers, GPU and network processes
            │                                          x86-64 CEF under FEX
            └─ game: reaper → SteamLinuxRuntime_4/_v2-entry-point
               → pressure-vessel (bwrap interpreted by runtime/mounts.c) → pv-adverb
               → proton (python3) → wine/wineserver (x86-64, or i386 for 32-bit)
               → DXVK / VKD3D-Proton → libvulkan-guest.so (x86 thunk)
               → libvulkan-host.so (aarch64) → shim libvulkan.so.1 → MoltenVK → Metal

launchd
└─ lxrun FEXServer --foreground --persistent=0       aarch64 ELF; exists only for x86
     (started from a subshell, so pv-adverb never waits on it; run-fex.sh:68-85)
```

Two FEX binaries run in one Steam session:

- `FEX-gb`, from the aarch64 test root (`/tmp/lxrt-root/usr/bin`), runs the
  client tree. Each x86 exec there re-executes the same image (`process.c:92-113`).
- `FEX-emu` (`/usr/lib/lxrt-emu/FEX` in the Steam root, relinked with its own
  loader path, `build-fex-host.sh:288-305`) runs whatever the bwrap interpreter
  execs: the steamwebhelper container and every game.

Sources:

- `scripts/run-app.sh` (launch block);
- `scripts/run-fex.sh:28-101`;
- `benchmarks/stage15-steam-proton-path.txt:8-12`;
- `benchmarks/stage10-vulkan-thunks.txt:17-27`.

`scripts/run-steam.sh` is the command-line twin of the same chain.

## 3. Roots, homes and fixed paths

| path | what it is | ISA | tag |
|---|---|---|---|
| `~/SteamARM-roots/lxrt-root` → `/tmp/lxrt-root` | Fedora 43 aarch64 root, built from 66 root RPMs (plus 5 build-only) pinned in `scripts/mkroot-rpm.lock` (`stage9-vmfree-build.txt:427-429`). It holds glibc, bash, the X client libraries, Xvnc + Mesa swrast, and FEX + FEXServer. | aarch64 | REPLACE_WITH_HOLO (keep it until the Holo tree is inventoried: FEX, FEXServer and `tests/elf` run from it) |
| `~/SteamARM-roots/x86-rootfs` | FEX's Ubuntu 24.04 x86-64 image from rootfs.fex-emu.gg (`scripts/fetch-x86-rootfs.sh`). Adds amd64 `lsof` + libtirpc for the client's port check, and i386 GTK2/libXtst (who added them, and why, is not recorded: `stage9-vmfree-build.txt:535-536`). | x86-64 + i386 | GUEST_X86_REQUIRED / GAME_PAYLOAD_EXCEPTION (the client-only additions are REMOVE_LATER) |
| `~/SteamARM-roots/steamroot` → `/tmp/lxrt-steamroot` | The x86-64 tree cloned as `/`, plus the aarch64 emulator side in `/usr/lib/lxrt-emu` (`scripts/mksteamroot.sh:5-10`). The script's stated reason for an x86 `/` (a comment, not measured): pressure-vessel inspects the host through directory file descriptors, which FEX's path-based rootfs overlay does not cover. | mixed | REPLACE_WITH_HOLO / REPLACE_WITH_ARM64 |
| `steamroot/tmp/fexhome` (guest `/tmp/fexhome`) | Guest `HOME` (`run-fex.sh:34`; passwd user `steam`, `mksteamroot.sh:61-64`). Holds the Steam install, `.fex-emu`, and FEX's rootfs clone `RootFS/Ubuntu_24_04`. | data | KEEP the convention. This path is baked into Steam's configuration and symlinks (`scripts/env-links.sh:2-4`). |
| `/usr/lib/lxrt-emu` (inside the Steam root) | The only host directory visible inside pressure-vessel containers. Holds `FEX-emu`, the thunks, the shim and the aarch64 X libraries (`runtime/mounts.c:283-312`). | aarch64 | KEEP for game payloads. The X libraries are REPLACE_WITH_HOLO. |
| `~/SteamARM-roots/samples` → `/tmp/lxrt-samples` | aarch64 test programs | aarch64 | KEEP |
| `/tmp/lxrt-arm64root` | The ARM64 base the launcher and `scripts/run-native.sh` expect. `scripts/env-links.sh:16-18` links it when `$STEAMARM_STATE/arm64root` exists; that does not exist on the owner's Mac (MEASURED `ls`), so neither does the link. It is meant to point at a root derived from the Steam Frame image (`docs/ARM64_FIRST_MIGRATION.md`, the target). | aarch64 | new, REPLACE_WITH_HOLO target |
| `$STEAMARM_STATE/armroot` → `/tmp/lxrt-armroot` | Fedora 43 aarch64 root built by `scripts/mkarmroot.sh` (150 root packages) for Valve's native client; stage 21 ran from it. The `/tmp` link was made by hand: no script creates it (MEASURED). | aarch64 | TRANSITIONAL; REPLACE_WITH_HOLO |
| `steamframe-oobe-repair-20260922.5153644-0.3.0.img` (on the owner's Mac) | Valve's Steam Frame recovery image: the source of the ARM64 userspace. It is read with `scripts/steamframe-image.py` (read-only, never booted, never bundled). Extracted and inventoried on 2026-09-29 (MEASURED): SteamOS 0.3.0, 965 packages, glibc 2.39, a Steam bootstrap with the native client; `docs/STEAM_FRAME_INVENTORY.md`. | data (aarch64 contents) | STEAM_RUNTIME_OWNED; PROPRIETARY_DO_NOT_REDISTRIBUTE |
| `$STEAMARM_STATE/launcher/` | `apps.json`, `settings.json`, `running.{pid,id,display,arch}` | data | KEEP |
| `/tmp/lxrt-shm-<uid>`, `/tmp/lxrt-sig`, `/tmp/lxrt-input` | Host directories behind `/dev/shm`, the cross-process signal mailboxes and the evdev sockets | host | KEEP |

## 4. Lifecycle, in one paragraph

This is VERIFIED IN SOURCE; details in `docs/APPLICATION_MANAGER.md`.

- The launcher runs `scripts/run-app.sh <id>`. The script starts X, PulseAudio
  and inputd, then starts the program with `nohup`.
- Tracking: a pidfile polled every 2 s, plus "some `build/lxrun` process
  still exists". Since `scripts/session.py`, the pidfile names the leader of
  the app's own process group, and the program's exit status lands in
  `running.status`; before it, a crash was guessed as "gone within 15 s".
- "Detener" signals that group (SIGTERM, then SIGKILL), then does `kill -9`
  on any guest `lxrun` still there.
- Only one app runs at a time.
- Since `748bc97`, entries marked `aarch64` go to `scripts/run-native.sh`
  (lxrun, no FEX). Steam, Heroic and Prism are still marked `x86_64`, so in
  practice everything still goes through FEX. `run-app.sh` refuses an
  aarch64 entry whose root is the x86 Steam root, and an x86 entry whose root
  is the ARM64 base, as `LaunchPlanner` does. "Añadir app" still installs
  every program under the Steam root, so an aarch64 program added that way is
  refused until the add flow installs into the ARM64 base.
- Nothing stops the host side after the app exits: X11.bin and quartz-wm,
  PulseAudio, `steamarm-inputd`, FEXServer and the 1 Hz `safeguard.sh` loop
  keep running (no caller of their `stop`). This is harmless, but it is not
  "back to idle".
- The launcher's FEX settings (Procesador) reach the Steam client too, since
  the client runs under FEX: today they tune the Steam UI as well as games.

## 5. Components

### 5.1 Runtime (`runtime/`, host Mach-O arm64)

| component | tag | x86 debt | note |
|---|---|---|---|
| Core: ELF loader, section-aware rewriting and trampolines, x18 virtualisation, TLS in a TSD slot, synthetic system registers, the Linux-shaped stack, vDSO. Files: `main.c`, `elf.c`, `elfsect.c`, `rewrite.c`, `trampoline.S`, `x18.c`, `tls.c`, `sysreg.c`, `stack.c`, `vdso*`. | KEEP | NOT_APPLICABLE | Any aarch64 Linux code needs these, including an ARM64 client, SLR and CEF. `elf.c` refuses ET_EXEC; aarch64 images surveyed so far are PIE (`stage2-elf-survey.txt:8-13`). |
| The Linux API on Darwin: `dispatch.c`, `thread.c`, `futex_ops.c`, `epoll_eventfd.c`, `inotify.c`, `timerfd_signalfd.c`, `fileops2.c`, `socket.c`, `signal.c`, `sysv_ipc.c`, `process.c`, `procfs.c`, `procpid.c`, `proc_ext.c`, `sysfs.c`, `privmap.c`, `jit.c` | KEEP | GAME_PAYLOAD_EXCEPTION where it serves FEX | Generic. Its FEX-specific paths stay for x86 games under an ARM64 client: the mmap/mprotect/brk/prctl cases, the binfmt hand-off, FEX's `/proc/self/exe` re-exec, the `/proc/cpuinfo` untranslate, SIGBUS left to FEX's unaligned-atomic backpatching. One rule is client-only and wrong for native code: RWX mprotect granted as RW (`dispatch.c:1308-1320`, §7). |
| `mounts.c` (pressure-vessel's bwrap plan interpreted in-process) | KEEP | GAME_PAYLOAD_EXCEPTION (emulator binds only) | HYPOTHESIS: SLR Arm64 uses the same pressure-vessel. The plan it was checked against came from x86 SLR 3 (`stage6-bwrap-plan.txt`). The "emulator binds" (`mounts.c:283-312`) are FEX-specific. |
| `subpage.c` (4 KiB guest pages inside 16 KiB host pages), `shmirror.c` | KEEP | GAME_PAYLOAD_EXCEPTION | x86 images use `p_align` 0x1000; aarch64 ones measured before stage 21 use 0x10000 (`stage2-elf-alignment.txt:10-29`). The shim itself is linked with 4 KiB pages (`dispatch.c:1193-1195`). Valve's native arm64 client ships 4 KiB-aligned images too, and since stage 21 `elf.c` loads them through `subpage.c` (MEASURED, `stage21-native-arm64-client.txt`). So `subpage.c` now serves the ARM64 client as well. |
| `gbase.c` (32-bit guest base; pointer rebasing for about 120 syscalls; fix-up of low-pointer faults) | KEEP | GAME_PAYLOAD_EXCEPTION | Host code, not guest code. Serves i386 guests and the 64-bit low window of x86-64 Wine and ET_EXEC tools. The fix-up exists for DXVK's 64-bit `vkCreateInstance` (`gbase.c:212-279`). |
| `evdev.c`, `window.m`, `remote_layer.m` | KEEP | NOT_APPLICABLE | `window.m` has two parts. Every process uses its lazy main-thread pump (`lxrt_window_pump`). Its NSWindow + CAMetalLayer path (`0x4C580010`/`11`) is used only by `tests/elf/vk_present.c` and `vk_triangle.c`; that part is a REMOVE_LATER candidate once those tests present through X. |
| `resources/lxrt.entitlements` (allow-jit), Makefile lxrt section, `tests/elf/`, `tests/x18_check.*`, `tests/x18_preserve/` | KEEP | NOT_APPLICABLE (`tests/elf` i386/Vulkan-thunk suites: GAME_PAYLOAD_EXCEPTION) | Without the entitlement, MAP_JIT is refused. |

### 5.2 Build and install (`Makefile`, `scripts/`)

| component | tag | x86 debt | note |
|---|---|---|---|
| `Makefile` (lxrun, vdso, shim, inputd, launcher) | KEEP | NOT_APPLICABLE | Everything it builds is arm64. |
| Homebrew formulae (`setup.sh:33-34`) | KEEP | NOT_APPLICABLE, except `mingw-w64`: GAME_PAYLOAD_EXCEPTION | `mingw-w64` only builds the Windows test probes in `tests/win`. |
| `scripts/setup.sh` (ten steps) | KEEP | TEMPORARY_X86_DEPENDENCY | Its `steam` step is x86: `install-steam.sh`, then the thunks, then `install-steamroot-gfx.sh`. |
| `scripts/mkroot-rpm.sh` + `.lock` | KEEP | NOT_APPLICABLE | The same unpacking technique (no scriptlets, relative symlinks) would work for Holo packages. |
| The Fedora sysroot `sysroot-f43` + `toolchain-aarch64-linux-fedora.cmake` | KEEP | GAME_PAYLOAD_EXCEPTION | Everything built against them serves x86 payloads: FEX, the thunk host libraries and `thunkgen` (`build-fex-host.sh:249-259`, `build-fex-thunks.sh:267-282`). lxrun, the shim and the launcher use Homebrew clang without it. |
| Fedora root: Xvnc + closure (about 40 packages), xkbcomp/xkeyboard-config, Mesa swrast | REMOVE_LATER | NOT_APPLICABLE | Used only by the VNC display mode, which cannot show the Metal layer (`stage12-native-present.txt:37`). |
| `scripts/build-fex-host.sh` (FEX `08f451d3b` + 8 patches in its `PATCHES` array, `:79`, `arch-prctl` the newest; `-ffixed-x18`; relinked as FEX-emu) | KEEP | GAME_PAYLOAD_EXCEPTION | The header comment (`:17-19`) still names only 4 of them. |
| `scripts/build-fex-thunks.sh` + the Ubuntu 24.04 x86-64/i386 dev sysroots | KEEP | GAME_PAYLOAD_EXCEPTION | Builds Vulkan thunks only (64-bit and 32-bit). No other library is thunked. |
| `scripts/fetch-x86-rootfs.sh` | GUEST_X86_REQUIRED | GAME_PAYLOAD_EXCEPTION | It also disables 8 Mesa ICD manifests so the x86 Vulkan loader sees only lavapipe (bypassed by the thunk overlay). |
| `scripts/mksteamroot.sh` | REPLACE_WITH_HOLO | REPLACE_WITH_ARM64 | Exists for the x86-as-`/` design. |
| `scripts/install-steam.sh` (`steam_latest.deb` → `bootstraplinux_ubuntu12_32.tar.xz`) | REPLACE_WITH_HOLO | TEMPORARY_X86_DEPENDENCY | No arm64 bootstrap URL exists anywhere in the repo. Stage 21 downloaded the ARM64 client from Valve's `steam_client_linuxarm64` manifest by hand; the Steam Frame image also carries a bootstrap (`usr/lib/steam/steam.tar.zst`, MEASURED). No script does either yet. |
| `scripts/install-steamroot-gfx.sh`: install of shim, thunks and X libraries | KEEP | GAME_PAYLOAD_EXCEPTION | |
| `scripts/install-steamroot-gfx.sh:99-110`: FEX `AppConfig` for `steam` (HideHypervisorBit) and for `steamwebhelper` (Vulkan thunks off) | REMOVE_LATER | TEMPORARY_X86_DEPENDENCY | Keyed on the x86 client's executable names. |
| `scripts/run-app.sh`, `scripts/settings-env.py` | KEEP | TEMPORARY_X86_DEPENDENCY | `settings-env.py` exports `FEX_*` and the Proton knobs to every app. `run-app.sh` now drops `FEX_*` for aarch64 entries. |
| `scripts/run-fex.sh` | KEEP | GAME_PAYLOAD_EXCEPTION | For x86 payloads. `scripts/run-native.sh` is the aarch64 counterpart. |
| `scripts/run-steam.sh` | REMOVE_LATER | TEMPORARY_X86_DEPENDENCY | A command-line duplicate of `run-app.sh steam`. |
| `scripts/run-x11-native.sh`, `audio.sh`, `input.sh`, `safeguard.sh`, `env-links.sh`, `make-release.sh`, `cdp.py`, `default-output.py`, `make-icon.py`, `make-icns.sh` | KEEP | NOT_APPLICABLE | `safeguard.sh` also restarts FEXServer when it is missing. Until this audit it did so for any guest, so a pure aarch64 session got a FEXServer and a FEX `/bin/true` every 5 s. It now does so only while an x86 guest runs (`has_x86_guest`). |
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
| Proton (ARM64) | aarch64 | STEAM_RUNTIME_OWNED | Hidden today. MEASURED: Proton 11.0 (ARM64) `wine cmd` under lxrun cannot reserve its low ranges and exits in 2 s (`stage18-settings-audio-controllers.txt:136-141`). The reason is verified in xnu and Wine (`stage19` §2). A patched Wine might work. |

### 5.4 Graphics, presentation, FEX patches

| component | tag | x86 debt | note |
|---|---|---|---|
| Shim `build/libvulkan.so.1`: `vulkan_shim.{c,S}`, `gen.py`, `wsi.c`, `features.c`, `fallback.c`, `memcap.c` | KEEP | NOT_APPLICABLE | Not built with `-ffixed-x18` (`Makefile:96-102`), so it relies on the runtime's x18 rewriter like any other guest library. An aarch64 caller links the shim directly, with no thunk (MEASURED, `stage4-shim.txt:29-32`; `tests/elf/run.sh`, check 12). The feature spoofs work around MoltenVK gaps and do not depend on the ISA. MoltenVK is loaded by absolute path, with no ICD JSON (`shim/gen.py:75-78`). |
| `shim/gen_rebase.py` → `vk_rebase.c` | GUEST_X86_REQUIRED | GAME_PAYLOAD_EXCEPTION | With no guest base these wrappers only forward, so they cost nothing for aarch64 callers. |
| `shim/map32.c` | GUEST_I386_REQUIRED | GAME_PAYLOAD_EXCEPTION | Passes calls through unless FEX reports a 32-bit guest base. |
| FEX Vulkan thunks: guest `libvulkan-guest.so` (x86-64 and i386), host `libvulkan-host.so` (aarch64), `thunkgen` | GUEST_X86_REQUIRED / GUEST_I386_REQUIRED | GAME_PAYLOAD_EXCEPTION | |
| x86 rootfs Vulkan loader + lavapipe | GUEST_X86_REQUIRED | GAME_PAYLOAD_EXCEPTION | The thunk overlay bypasses it (`stage10-vulkan-thunks.txt:11-12`). |
| FEX patches: `guest-base`, `thunk-args`, `wx` + `LxrtJit.h`, `x18`, `thunkgen-macos`, `shebang` | KEEP | GAME_PAYLOAD_EXCEPTION | `guest-base` also forces the low window for executables whose names start with `wine` (`:1179-1180`). `shebang` is for Proton's own `#!/usr/bin/env python3` launcher under FEX inside the container (`stage15-steam-proton-path.txt:59-62`). |
| FEX patches: `guest-reserve`, `thunkgen-32bit`, `thunks-guestbase32` | GUEST_I386_REQUIRED | GAME_PAYLOAD_EXCEPTION | |
| `patches/fex-lxrt-hide-hypervisor.patch` | REMOVE_LATER | TEMPORARY_X86_DEPENDENCY | Its only purpose is to steer the x86 client's choice of Proton and SLR. |
| XQuartz 21.1.24 + `xquartz-remote-layer`, `-signals-to-server-thread`, `-log-file-env` patches; quartz-wm + `quartz-wm-picture`; `run-x11-native.sh` | KEEP | NOT_APPLICABLE | Cross-process CALayerHost presentation, no per-frame copy (MEASURED, `stage12-native-present.txt`). |
| aarch64 X client libraries copied into `/usr/lib/lxrt-emu` (libX11, libxcb, libXau, libXdmcp from the Fedora root) | KEEP | GAME_PAYLOAD_EXCEPTION | Their one consumer once Xvnc is gone is the aarch64 Vulkan host thunk, which loads them for x86 payloads (`install-steamroot-gfx.sh`). They can later come from the Holo root instead (REPLACE_WITH_HOLO); UNKNOWN whether Holo's libxcb ABI matches what `wsi.c` declares by hand. |
| Xvnc mode (display `:1`, Screen Sharing) | REMOVE_LATER | NOT_APPLICABLE | Games cannot present through it. |

### 5.5 Input, audio, sync

| component | tag | x86 debt | note |
|---|---|---|---|
| `steamarm-inputd` (SDL, IOKit) + `runtime/evdev.c` + `tools/inputd/PROTOCOL.md` | KEEP | NOT_APPLICABLE | |
| PulseAudio (Homebrew), socket at `<root>/tmp/pulse/native`; `runtime/pathfd.c` stand-in for O_PATH | KEEP | NOT_APPLICABLE | The socket lives inside the guest root so pressure-vessel can bind it. Until this audit, `run-app.sh` started `audio.sh` without `LXRT_ROOT`, so the socket was always in the Steam root and an aarch64 entry had none. Now there is one server, with a socket in each root that asks for one (`tests/audio/run.sh`). |
| eventfd, futex WAIT/WAKE/BITSET/REQUEUE | KEEP | NOT_APPLICABLE | The esync substrate. |
| `futex_waitv` (fsync), ntsync | missing | — | `futex_waitv` (449) is **not implemented**. It appears only in the guest-base pointer table (`runtime/gbase.c:102`); `dispatch.c` has no case for it, so it returns ENOSYS. Wine's fsync needs it: Proton's `do_fsync` probes exactly that call (MEASURED, disassembly of `ntdll.so` in Proton 10.0 and Experimental), so fsync turns itself off and the launcher's fsync toggle (`PROTON_NO_FSYNC`, `settings-env.py:71-74`) changes nothing today. Proton Experimental has no esync compiled in; its effective sync is then wineserver (HYPOTHESIS). ntsync (`/dev/ntsync`) does not exist either. |

### 5.6 Launcher (`launcher/`, Mach-O arm64)

| component | tag | x86 debt | note |
|---|---|---|---|
| `SteamARMApp`, `HomeView`, `Settings*`, `Hotkeys`, `Controllers*`, `SDLShim.h`, `Info.plist.in`, tests | KEEP | NOT_APPLICABLE | |
| `LauncherModel.swift` | KEEP | TEMPORARY_X86_DEPENDENCY | Adopts a running Steam by the ps substring `ubuntu12_32/steam ` (`:290`). `needsSetup` checks for the x86 `steam.sh`. |
| `Models.swift` | KEEP | TEMPORARY_X86_DEPENDENCY | Has a stale copy of Steam's definition in `AppEntry.steam` (`:76-79`), which `run-app.sh` never reads. `Paths.guestRoot` = `/tmp/lxrt-steamroot` for every app. |
| `Installers.swift` | KEEP | REPLACE_WITH_ARM64 | Heroic and Prism deliberately pick x86-64 assets. Whether usable aarch64 builds exist is UNKNOWN. |
| `AddAppView.swift` | KEEP | TEMPORARY_X86_DEPENDENCY | Since `748bc97` it records the ISA from `e_machine` (`ELFInspector`). It still installs into `Paths.appsRoot`, inside the x86 Steam root. |
| `ApplicationCore.swift` (new) | KEEP | NOT_APPLICABLE | Holds `LinuxBaseEnvironment`, `LaunchPlanner` and `SessionMachine`. |
| `SPEC.md` | KEEP | TEMPORARY_X86_DEPENDENCY | Its launch description (every app through `run-fex.sh`) predates `748bc97`; `docs/APPLICATION_MANAGER.md` is the current one. |
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
| i386 client `ubuntu12_32/steam` + `steamui.so` + scout i386 libraries | Valve's Linux client | Valve's native arm64 client (`steamrtarm64/steam`), downloaded from Valve or taken from the owner's Steam Frame image; never shipped by SteamARM |
| x86-64 `steamwebhelper` (CEF) | Steam's UI | the arm64 `steamrtarm64/steamwebhelper` and `libcef.so` that come with that client |
| x86-64 bash running `steam.sh`, `setup.sh`, `steam-runtime-check-requirements`, the launcher service | the bootstrap scripts | the ARM64 root's aarch64 shell and tools |
| amd64 `lsof` + libtirpc (`fetch-x86-rootfs.sh:40-47`) | the client checks its websocket peer with `lsof -i TCP@127.0.0.1:<port>` (MEASURED, `stage8-steam-zero-vm.txt:296-311`) | an aarch64 `lsof`. HYPOTHESIS: the ARM64 client runs the same check. |
| i386 GTK2 + libXtst (`fetch-x86-rootfs.sh:35-39`) | presumably the i386 client; no record of who added them (`stage9-vmfree-build.txt:535-536`) | nothing |
| x86-64 pressure-vessel tools for **steamwebhelper's** container | the client starts its UI through pressure-vessel (MEASURED, `stage6-steam-gap.txt:57-58`) | SLR Arm64 for the client. The x86 SLR stays for x86 Proton. |
| FEXServer started on the **client's** launch path (`run-fex.sh:68-85`) | FEX needs it | started on the first x86 payload instead (TEMPORARY until the client is ARM64) |
| Xvnc's `xkbcomp` running an x86 bash (`run-steam.sh:64-72`) | VNC mode | removed with VNC mode |

**No THUNK_CANDIDATE is in use today.** Only Vulkan is thunked. Under x86
Proton, the game payload loads x86 X11/xcb, PulseAudio and SDL libraries from
the FEX rootfs.

What the pinned FEX (`08f451d3b`) can forward, VERIFIED IN SOURCE in its
`Data/ThunksDB.json`:

| library | thunk in FEX | THUNK_CANDIDATE for SteamARM? |
|---|---|---|
| Vulkan | yes | in use |
| GL | yes | HYPOTHESIS: only with an aarch64 GLX on the host side, which macOS lacks (Zink over the shim would be one) |
| asound (ALSA) | yes | low value: Wine talks to PulseAudio here (`PULSE_SERVER`) |
| drm, wayland-client, cuda | yes | not applicable on macOS |
| X11/xcb, libpulse | no | would need new thunk definitions. HYPOTHESIS: they would cut FEX time in Wine's display and audio paths. Not measured, and not a priority while real games are unverified. |
| SDL2 | source only (`ThunkLibs/libSDL2`), not in ThunksDB | same as X11 |

## 7. Where Steam is hard-wired, and what the ARM64 client changes

The launch and stop paths recognise Steam through the i386 client:

- the pattern `build/lxrun .*ubuntu12_32/steam ` in `run-app.sh:41`,
  `run-steam.sh:76`, `install-steamroot-gfx.sh:38` and
  `LauncherModel.swift:290`;
- the `steamui.so` test that decides `-noverifyfiles` (`run-app.sh:94-98`);
- the command `/bin/bash /tmp/fexhome/.local/share/Steam/steam.sh`
  (`run-app.sh:81`, `Models.swift:78`);
- removal of `tmp/fexhome/.steam/steam.pid` on stop (`run-app.sh:62`);
- `needsSetup` (`LauncherModel.swift:91`).

(Line numbers at `dbd1657`.) All of these must also key on the ARM64
client's paths, which are now known (MEASURED, stage 21 and its logs):
`steamrtarm64/steam` and `steamrtarm64/steamwebhelper` under the client's
`.local/share/Steam`.

Behaviours of the Steam client that the runtime handles specially
(VERIFIED IN SOURCE; each was MEASURED when it was added):

| special case | where | needed by an ARM64 client? |
|---|---|---|
| `/dev/shm` → `/tmp/lxrt-shm-<uid>` | `dispatch.c:384-398` | yes (Darwin) |
| `set_robust_list` kept for the webhelper's shared-memory check | `dispatch.c:2202-2212` | yes |
| `PR_SET_MM_MAP` rewrites `/proc/<pid>/cmdline` (the client checks its UI's peer process by it) | `dispatch.c:2640-2652` | yes |
| SA_RESTART emulation (webhelper "Error 4 locking shared memory file") | `dispatch.c:3292-3320` | yes |
| SysV IPC with a 10.8 MB segment above Darwin's shmmax | `sysv_ipc.c` | yes (`libtier0_s.so`) |
| SEQPACKET → DGRAM for Chromium IPC | `socket.c:267-273` | yes |
| netlink uevent socket offered only inside bwrap containers (outside one, the client's window never appeared) | `socket.c:282-321` | UNKNOWN: tuned against the x86 client's udev threads |
| `/proc/<pid>/fd`, `/proc/net/tcp` for `lsof` | `procpid.c` | yes, with an aarch64 `lsof` |
| `max_user_namespaces = 0`, so CEF starts with `--no-sandbox` | `proc_ext.c:1172-1182` | yes |
| mprotect RWX outside MAP_JIT granted as **RW** (x86 V8 under FEX never executes that memory) | `dispatch.c:1308-1320` | **no**, see below |
| `PR_SET_MEM_MODEL` → EINVAL, so FEX keeps TSO | `dispatch.c:2653-2663` | no for the client; yes for FEX payloads |
| `/proc/cpuinfo` untranslate, so FEX recognises it | `procfs.c:451-457` | no for the client; yes for FEX payloads |
| AppKit started lazily; `OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES` on every exec | `window.m:104-135`, `process.c:138-161` | yes. UNKNOWN whether an ARM64 webhelper that loads the shim directly hits the ObjC fork kill that `steamwebhelper.json` avoids today. |
| AT_PAGESZ 16384, or 4096 with `LXRT_GUEST_PAGE=4096` | `stack.c:21-34`, `stack.c:134` | not as 16384: glibc refuses to dlopen the client's 4 KiB-aligned `steamui.so` on a 16 KiB system, and the client runs with `LXRT_GUEST_PAGE=4096` (MEASURED, `stage21-native-arm64-client.txt`). The Steam Frame kernel is built for 4 KiB pages (MEASURED, `docs/STEAM_FRAME_INVENTORY.md`). |

**A native ARM64 V8 has no JIT under lxrun (HYPOTHESIS, from measured parts).**

- Apple Silicon gives no memory that is writable and executable at once. A
  MAP_JIT region is either one or the other, per thread (MEASURED,
  `stage5-jit.txt`).
- The flip cannot be done behind the guest's back. A
  `pthread_jit_write_protect_np` issued inside a signal handler does not
  survive the handler's return (MEASURED, `stage5-jit.txt:28-36`,
  `runtime/jit.c:1-15`). FEX works because it was patched to ask for the flip
  itself (private syscall `0x4C580020`).
- MAP_JIT cannot be applied to an existing range either: MAP_JIT together
  with MAP_FIXED returns EINVAL (`dispatch.c:977-982`).
- So with today's runtime an unpatched ARM64 V8 fails in every case:
  - without the rule, at its RWX mprotect (EACCES);
  - with the rule, when it executes its code (a fault).
- This does not stop the network process, which runs no JS; the NSS FATAL
  comes first. It stops the renderers.
- Update, stage 23 (`benchmarks/stage23-native-arm64-jit.txt`): none of
  the ways below was needed. The runtime now keeps each 16 KiB page of a
  native guest's RWX range either read-write or read-execute and flips it on
  faults, scanning a page read-only before it becomes executable
  (`runtime/wxsplit.c`); with the runtime's SIGSEGV/SIGBUS handler kept when
  the zygote resets them to SIG_DFL, the unpatched V8 JITs and the login
  window comes (MEASURED). What follows is the analysis as it stood.
- The likely ways out:
  - run steamwebhelper with `--js-flags=--jitless` (no JIT and no
    WebAssembly; slower JS), set in the derived root's `steamwebhelper.sh`;
  - a CEF whose V8 asks for the flip, as FEX does;
  - store emulation in the runtime. Keep the thread in execute mode, and on
    each faulting store to a JIT page flip to write mode inside the handler,
    do the store there, flip back and step past it. Stores made inside the
    handler do land (MEASURED, `stage5-jit.txt:57-75`); the cost is one fault
    per store instruction (HYPOTHESIS: acceptable for UI JS).
- Whichever way is chosen, the RW rule should apply only to FEX guests.

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
| `svc` rewriting, TPIDR_EL0 in a TSD slot, x18 virtualisation, sysreg rewriting | runtime core | **yes**. Prebuilt ARM64 libraries use x18 (libcef `blr x18`, `stage19` §3a). `tests/x18_preserve` checks whether a macOS SDK < 13 binary keeps x18 (`stage19` §3a). Since stage 21 the x18 pass covers only `.eh_frame` function ranges (`runtime/elfsect.c`), because rewriting the client's OpenSSL constant tables in `.text` broke every TLS handshake. libcef's whole text is still rewritten, with `LXRT_X18_ALL_TEXT=libcef.so`, which no script sets yet (MEASURED, `stage21-native-arm64-client.txt`; `docs/X18_VIRTUALIZATION.md`). |
| Guest-driven W^X flip (`0x4C580020`), RWX → MAP_JIT | `jit.c`, FEX `wx` patch | the mechanism, yes; an unpatched ARM64 V8 cannot use it (§7) |
| Sub-page (4 KiB on 16 KiB) | `subpage.c` | **yes**. Valve's native client ships 4 KiB-aligned images, and they load through `subpage.c` (MEASURED, `stage21-native-arm64-client.txt`). |
| Guest base / low window, pointer rebasing, 32-bit thunks, `map32` | `gbase.c`, FEX patches, shim | no for the client; yes for x86/i386 games |
| bwrap interpreter | `mounts.c` | yes (HYPOTHESIS: SLR Arm64 is pressure-vessel too) |
| HideHypervisorBit for `steam` | `install-steamroot-gfx.sh:104-105` + patch | no (not under FEX); the problem it solved turns around (§7) |
| No Vulkan thunks in `steamwebhelper` | `install-steamroot-gfx.sh:106-110` | UNKNOWN (see the ObjC fork row in §7) |
| `-noverifyfiles` only once `steamui.so` exists | `run-app.sh:94-98` | UNKNOWN: depends on whether the image's client is complete |
| FEXServer `--persistent=0`, restarted by `safeguard.sh` | `run-fex.sh`, `safeguard.sh:93-100` | no for the client; yes for games |
| PulseAudio socket inside the root, O_PATH stand-in | `audio.sh`, `pathfd.c` | yes |
| passwd/group/machine-id, empty `/dev/input` | `mksteamroot.sh:56-82` | yes, in the ARM64 root |
| Replace files by rename, never in place | `Makefile:56-60` | yes |
| Memory and process watchdog | `safeguard.sh` | yes |

## 9. Open questions

1. Does the Steam Frame image hold a complete, runnable ARM64 client? If so,
   at what path, and does its `steamwebhelper` get past BrowserReady and the
   NSS network-process FATAL under lxrun? This is next on the Mac, using
   `docs/STEAM_FRAME_IMAGE.md`.
   Partly answered by stage 21 (MEASURED): the client was downloaded from
   Valve's `steam_client_linuxarm64` manifest instead, into the root that
   `scripts/mkarmroot.sh` builds. It starts, updates itself, loads
   `steamui.so` and `steamclient.so`, then aborts with `free(): invalid
   pointer` before its window. The abort's cause is a HIGH-CONFIDENCE
   HYPOTHESIS: a `vgui2_s` bug reached after the webhelper times out, on a
   root without X locale data (`docs/STEAM_ARM64_BRINGUP.md`).
   The image does hold the client, as a bootstrap tarball
   (`usr/lib/steam/steam.tar.zst` with `steamrtarm64/`, MEASURED 2026-09-29,
   `docs/STEAM_FRAME_INVENTORY.md`). It has not been run from there.
2. Are all ELFs in that tree PIE and 16 KiB-aligned (or more), and what page
   size is its kernel built for? The "Loading under lxrun" section of
   `steamframe-image.py inventory` answers this.
   Answered (MEASURED): the client's libraries are 4 KiB-aligned and load
   with `LXRT_GUEST_PAGE=4096` (stage 21). In the Frame root, 6,718 aarch64
   files use 64 KiB segments, 25 programs and 21 libraries use 4 KiB, 45 are
   `ET_EXEC`, and the kernel uses 4 KiB pages (inventory, 2026-09-29).
3. Does SLR Arm64's pressure-vessel emit the bwrap plan `mounts.c` handles?
   Does it expect Valve's FEX, or binfmt, for x86 games?
4. How is x86 Proton offered to an ARM64 client (§7)?
5. fsync needs `futex_waitv`, which lxrun lacks (§5.5). Implement it (a wait
   on several futexes at once), or say in the launcher that only esync applies.
6. MoltenVK has no version pin: it comes from Homebrew and is measured only on
   1.4.2.
7. Real games are not verified (`README.md`). Known defects that remain open:
   - "Abort trap: 6" after the `d3d11_32` probe, in 2 of about 25 runs
     (`stage17-clean-install.txt:51-54`);
   - "corrupted double-linked list" when a container exits (`stage15-steam-proton-path.txt:96-98`).

## 10. What this changes next

The ordered plan now lives in `docs/ARM64_FIRST_MIGRATION.md`, with the
Fedora armroot as the transitional root and the Steam Frame root as the
target. Its numbers are in `docs/ARM64_FIRST_AUDIT.md`. The plan this
section held first (extract the Frame root, run its client, re-key the
launcher, classify Holo) is folded into it: the extraction and inventory
are done (MEASURED, 2026-09-29), and stage 21 reached the client by
downloading it instead.

---

## Appendix: observations on the Mac (2026-09-28, local session)

The notes below were written during local sessions on the owner's Mac and
are kept verbatim (Spanish). Where they disagree with the audit above, the
MEASURED label decides.

## Entorno Steam actual: auditoría de migración

Fecha de observación: 2026-09-28. Objetivo: fijar la línea base antes de
migrar el userspace ARM64 a Holo Core. Todo el camino sigue siendo macOS
nativo y ZERO-VM: no hay kernel Linux invitado ni hipervisor.

### Lifecycle observado

La biblioteca SwiftUI invoca `scripts/run-app.sh <id>` desde
`launcher/LauncherModel.swift`. La app integrada Steam resuelve a
`steam.sh`; una app Windows resuelve Proton instalado mediante
`scripts/proton-command.py`, que asigna un prefijo separado bajo
`$STEAMARM_STATE/steamroot/tmp/fexhome/.steamarm/prefixes/<id>`.

`scripts/run-app.sh` prepara pantalla, sonido, entrada y entorno por app;
`scripts/run-fex.sh` inicia `build/lxrun` con el FEX ARM64 adecuado. `lxrun`
es proceso Mach-O de macOS: carga ELF Linux ARM64 y traduce sus syscalls a
Darwin/XNU. FEX ejecuta el código x86/x86-64/i386 en el mismo proceso/runtime;
no existe arranque de kernel ni guest. FEXServer persiste entre procesos
FEX. `--stop` detiene apps guest; X y FEXServer tienen lifecycle separado.

Presentación principal: X11 rootless nativo en `:2`, cuyas ventanas aparecen
como ventanas macOS. Xvnc/Screen Sharing es ruta alternativa de diagnóstico.
Audio cruza PulseAudio a CoreAudio; `steamarm-inputd` y el runtime exponen
mandos como evdev. Vulkan cruza FEX thunks y `shim/libvulkan.so.1` hasta
MoltenVK 1.4.2.

### Roots y componentes

| Ubicación lógica | Construcción/contenido actual | Clasificación |
|---|---|---|
| `$STEAMARM_STATE/lxrt-root` (`/tmp/lxrt-root`) | Root ARM64 mínimo derivado de RPM Fedora 43; FEX, FEXServer, loader/glibc y utilidades | `REPLACE_WITH_HOLO` |
| `$STEAMARM_STATE/armroot` | Segundo root Fedora 43, ampliado con GTK/X11/audio para el experimento de Steam ARM64 directo | `REPLACE_WITH_HOLO`, experimento `REMOVE_LATER` tras migración validada |
| `$STEAMARM_STATE/x86-rootfs` | Ubuntu 24.04.4 LTS descargado de FEX; contiene ABI x86-64 e i386 y fixups locales | `GUEST_X86_REQUIRED`, conservar hasta probar sustituto |
| `$STEAMARM_STATE/steamroot` (`/tmp/lxrt-steamroot`) | Ubuntu x86-64 fusionado con utilidades y bibliotecas ARM64; Steam, herramientas Proton y runtimes instalados en `$HOME` guest | Mixto: x86 `GUEST_X86_REQUIRED`; capa ARM Fedora `REPLACE_WITH_HOLO` |
| `$STEAMARM_BUILD` | FEX compilado en macOS para ELF Linux ARM64, thunks, toolchains y cachés | `KEEP` durante migración; revisar al cerrar build reproducible |

El root Ubuntu contiene el Steam client `ubuntu12_32/steam` como ELF i386.
Steam administra varias instalaciones Proton y Steam Linux Runtime. Las
instalaciones observadas incluyen Proton Experimental/10/Hotfix x86,
Proton 11 ARM64 y Experimental ARM64; también existen
`SteamLinuxRuntime_4`, `SteamLinuxRuntime_4-arm64`, `sniper` y `soldier`.
Esto prueba presencia de payloads descargados, no que la ruta ARM64 ya pueda
ejecutarse desde el cliente x86 bajo FEX.

El inventario del launcher busca manifiestos Valve en `steamapps/common` y
herramientas comunitarias en `compatibilitytools.d`. Sólo muestra tools
instalados; Bannerlator aparece cuando se instala allí. Proton 10 bajo Sniper
quedó detenido en `wineboot` y agotó el timeout de 180 s el 2026-09-28. Los
probes de cuadro limpio D3D11/D3D12 con Proton Experimental sí presentaron
imagen el 2026-09-28: 157.3 y 158.8 fps (600 cuadros cada uno), 2/2. No son
FPS de juego. Persisten avisos del selector FS de Wine; VKD3D/MoltenVK también
informó un compute shader sustituido por uno vacío durante D3D12. Log:
`~/SteamARM-roots/logs/proton-graphics-retest-20260928.log`.

### Clasificación de dependencias

| Clase | Elementos y criterio |
|---|---|
| `KEEP` | `runtime/` y `build/lxrun`; supervisor SwiftUI; FEX x86 adaptado a guest-base/x18/W^X; pantalla nativa; audio/mandos; MoltenVK 1.4.2. Mantener la ruta funcional mientras se migra. |
| `REPLACE_WITH_HOLO` | Fedora ARM64 de `mkroot-rpm.sh` y `mkarmroot.sh`; libraries/loader ARM64 de los dos roots. Sustituir sólo después de validar loader, glibc, pthread, `dlopen`, FEX y fallback. |
| `GUEST_X86_REQUIRED` | Steam x86/x86-64, Ubuntu x86-64 base, Proton x86 y ejecutables/juegos Windows x86/x86-64. FEX debe seguir traduciendo este código. |
| `GUEST_I386_REQUIRED` | Steam client i386, partes i386 de Proton/Wine y juegos/overlay que aún las requieran. Conservar FEX 32-bit y Vulkan thunk 32-bit. |
| `THUNK_CANDIDATE` | Vulkan guest-to-host hacia el shim/MoltenVK; medir por API, extensión y ABI antes de ampliar a OpenGL u otras bibliotecas. |
| `STEAM_RUNTIME_OWNED` | `SteamLinuxRuntime_4`, `_4-arm64`, `sniper`, `soldier` y manifests/payloads administrados por Steam. El launcher los detecta; no debe reemplazar sus archivos. |
| `REMOVE_LATER` | Constructor Fedora ARM y su root alternativo, sólo tras migrar Holo y demostrar fallback. No borrar aún. VNC queda como diagnóstico, no base gráfica. |
| `UNKNOWN` | Paridad de packages Holo frente a Steam Frame, ABI completa de SLR ARM64, semánticas pressure-vessel que sobreviven sin namespaces y necesidades i386 por juego. Resolver mediante auditoría binaria y smoke tests. |

### Proton ARM64: límite actual

Los tool-manifests ARM64 están instalados, pero `scripts/proton-command.py`
los rechaza deliberadamente; aparecen en el inventario con estado no compatible.
El Steam client en esta ruta es x86/i386 bajo FEX; Proton ARM64 requiere
procesos ELF Linux ARM64 y el runtime aún no puede reservar las direcciones
bajas que Wine necesita. Además, Darwin borra `x18` entre excepciones, mientras
ARM64EC usa ese registro para el TEB. El probe documentado en
`benchmarks/stage18-settings-audio-controllers.txt` falló en 2 s. Ocultar el
CPUID de FEX es necesario para Steam/pressure-vessel x86; mostrarlo hace que
Steam seleccione ARM64 y rompe el camino x86 medido. El selector sólo habilita
tools x86 que están instalados. Una prueba local el 2026-09-28 redujo
`__PAGEZERO` a 16 KiB, pero `MAP_FIXED` en `0x7ffe0000` terminó en `SIGKILL`
(exit 137); cambiar sólo el tamaño del segmento no elimina el bloqueo de baja
dirección.

La corrección de `arch_prctl(ARCH_GET_FS/GS)` de esta auditoría ya está
versionada como `patches/fex-lxrt-arch-prctl.patch` y se instala en ambos
roots mediante `scripts/install-fex-host.sh`.

### Puntos de entrada de auditoría

- `docs/ARCHITECTURE.md`: arquitectura y límites de `lxrun`/FEX.
- `scripts/run-app.sh`, `scripts/run-fex.sh`, `scripts/run-steam.sh`: lifecycle.
- `scripts/mkroot-rpm.sh`, `scripts/mkarmroot.sh`, `scripts/mksteamroot.sh`:
  creación de roots actuales.
- `scripts/fetch-x86-rootfs.sh`: Ubuntu/FEX x86 root y fixups.
- `scripts/proton-command.py`: Proton por app, selección de SLR y prefijos.
- `scripts/build-fex-host.sh`, `scripts/install-fex-host.sh`: build e instalación
  reproducibles de FEX con rollback binario.
- `scripts/install-steamroot-gfx.sh`: Vulkan thunks/shim y MoltenVK.
