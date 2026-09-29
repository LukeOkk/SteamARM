# benchmarks

What was measured at each stage of taking SteamARM from a Linux VM to
ZERO-VM, including what failed. Each `stage*.txt` file is the record of its
stage and is not rewritten afterwards; later corrections go in a later file
or are marked inside the old one (for example `stage2-tls.txt`).

Host for every Mac measurement unless a file says otherwise: Apple M4,
16 GB, macOS 27. Files marked **VM-era** were measured inside, or against,
the Fedora VM that was deleted on 2026-09-27; they are history, and
`docs/PERFORMANCE_BASELINE.md` quotes only the ZERO-VM numbers.

## Stage index

| stage | file(s) | date | what it established |
|---|---|---|---|
| 0 | (none left) | 2026-09-23 | frame-time capture for the VM app; its tools were deleted with the VM (see below) |
| 1 | `stage1-syscall-cost.txt` | 09-23 | Darwin ignores the `svc` immediate and runs the call in x16, so Linux `svc` cannot be trapped; a rewritten `svc` costs about as much as a native one, a trap 40-90× more |
| 1 | `stage1-syscall-mix.txt` | 09-23 | **VM-era**: about 4,300 syscalls/s from Steam idle at the library, measured with ftrace in the guest |
| 2 | `stage2-dynamic.txt` | 09-23 | ld.so maps libraries itself, so their `svc` sites must be rewritten at mmap time |
| 2 | `stage2-elf-alignment.txt`, `stage2-elf-survey.txt` | 09-23 | Fedora aarch64 images use `p_align` 0x10000 and are PIE; the PIE-only rule costs little |
| 2 | `stage2-pagezero.txt` | 09-23 | the low 4 GiB (`__PAGEZERO`) cannot be used or reclaimed, so `ET_EXEC` images cannot load |
| 2 | `stage2-rewrite-validation.txt` | 09-23 | a linear scan for `svc #0` matches the disassembler on real libraries |
| 2 | `stage2-tls.txt` | 09-23 | superseded by `stage3-tls.txt` (its conclusion was wrong) |
| 3 | `stage3-tls.txt` | 09-23 | Darwin clobbers `TPIDR_EL0` on context switch: guest TLS moves to a TSD slot |
| 3 | `stage3-threads.txt`, `stage3-signals.txt` | 09-23/24 | glibc threads, futexes and signals work; errno numbers differ between Darwin and Linux |
| 4 | `stage4-vulkan.txt`, `stage4-shim.txt`, `stage4-vulkaninfo.txt`, `stage4-triangle.txt`, `stage4-present.txt` | 09-24 | Vulkan from a Linux ELF to MoltenVK through the shim; unmodified `vulkaninfo`; a drawn triangle; presentation with no copy |
| 5 | `stage5-fex.txt`, `stage5-process.txt`, `stage5-selfread-deadlock.txt`, `stage5-subpage.txt`, `stage5-va.txt`, `stage5-sysreg.txt`, `stage5-idregs.txt`, `stage5-jit.txt`, `stage5-x18.txt` | 09-24/25 | FEX runs under lxrun: a process model, 4 KiB guest pages on 16 KiB, the address space, trapped system registers, W^X flips requested by the guest, and x18 zeroed by Darwin on every exception |
| 5 | `stage5-x86-throughput.txt` | 09-25 | x86-64 through FEX under lxrun runs at 1.0× native aarch64 on `x86_bench.c`; also has **VM-era** rows |
| 6 | `stage6-steam-gap.txt`, `stage6-bwrap-plan.txt`, `stage6-vm-lockups.txt` | 09-23..25 | **VM-era**: what Steam needs (86 distinct syscalls), the bwrap plan pressure-vessel generates, and two guest-kernel lockups |
| 7 | `stage7-guest-base.txt` | 09-25 | a guest address base for 32-bit guests, since the low 4 GiB is unavailable |
| 8 | `stage8-steam-zero-vm.txt` | 09-26 | the x86 Steam client runs with no VM: pressure-vessel, the webhelper, x86 memory ordering kept in software |
| 9 | `stage9-vmfree-build.txt` | 09-26 | FEX builds on the Mac from Fedora RPMs, with no VM |
| 10 | `stage10-vulkan-thunks.txt` | 09-26 | x86-64 Vulkan reaches MoltenVK through FEX's thunks |
| 11 | `stage11-native-x11.txt` | 09-26 | SteamARM's own XQuartz build, rootless on `:2`: X windows are macOS windows |
| 12 | `stage12-native-present.txt` | 09-27 | Vulkan frames hosted over X windows across processes (CALayerHost), no copy; 165 Hz pacing |
| 13 | `stage13-wine-and-host-safety.txt` | 09-27 | Proton's Wine runs `cmd.exe`; two Mac hangs and the memory guard that followed |
| 14 | `stage14-d3d11-d3d12.txt` | 09-27 | D3D11 (DXVK) and D3D12 (VKD3D-Proton) probes at ~162 fps |
| 15 | `stage15-steam-proton-path.txt` | 09-27 | Windows programs launched the way Steam launches them (pressure-vessel, SLR 4) |
| 16 | `stage16-32bit-vulkan.txt` | 09-27 | 32-bit D3D9/11/12 through the i386 Vulkan thunk |
| 17 | `stage17-clean-install.txt` | 09-27 | a clean install up to Steam's sign-in window, with no VM |
| 18 | `stage18-settings-audio-controllers.txt` | 09-27 | settings, sound in Steam's container, controllers; Proton ARM64 measured not viable on macOS |
| 19 | `stage19-steamframe-base-and-arm64-limits.txt` | 09-28 | reading the Steam Frame image without btrfs-progs; the macOS limits on ARM64 Proton (written without a Mac; labelled) |
| 20 | `stage20-ci-macos-runner.txt` | 09-29 | first build and tests on a GitHub macOS runner (a virtual M1, not the M4); x18 preserved for a pre-13 SDK binary there |
| 21 | `stage21-native-arm64-client.txt` | 09-27..29 | Valve's native arm64 client under lxrun: starts, self-updates, loads its UI libraries; no window in that stage (it aborted with `free(): invalid pointer`) |
| 22 | `stage22-native-arm64-bringup.txt` | 09-29 | the native arm64 client from that abort to its "Sign in to Steam" window, with `--jitless`: X locale data and locales in the Fedora root (the abort's mechanism confirmed by probe), `lsof`, libcef's `mrs x18, nzcv` and `ldar w18` rewritten instead of poisoned (and measured trapping before), mmap at 4 KiB file offsets; the Steam Frame root still failed at GLX |
| 22 | `stage22-kosmickrisp.txt` | 09-29 | KosmicKrisp as a second Vulkan driver through the shim (`STEAMARM_VK_ICD`, ICD mode) for aarch64, x86-64 and i386 guests; D3D11/D3D12 probes on it once the shim reports `fillModeNonSolid`; present modes reported and overridable, IMMEDIATE measured |
| 23 | `stage23-native-arm64-jit.txt` | 09-29 | V8's JIT in the native arm64 client: RWX pages split W^X per page and scanned before they execute, the host SIGSEGV/SIGBUS handler kept under a guest SIG_DFL; the login window with the JIT on, also from the launcher |
| 23 | `stage23-runtime-fixes.txt` | 09-29 | the W/X livelock of 4 KiB sub-pages (stores from a split page into itself are emulated) and a relative `/proc/self/exe`; `tests/elf/run.sh` 55/0 with 2 xfail → 63/0 with 0 xfail |
| 23 | `stage23-frame-root.txt` | 09-29 | the native arm64 client on the Steam Frame root (`scripts/mkframeroot.sh`): indirect GLX through the X server, a resolv.conf, the client's own steamdeck_stable branch; the login window 5 of 5 runs, 5-7 s later than on the Fedora root |

## After stage 21 (no stage file)

- `dbd1657`: the rewriter's poison word is now `brk #1`. It was
  `0xD4000021`, which is `svc #1`, so a refused or unreachable site ran a
  Darwin system call instead of trapping. `tests/elf/run.sh` 37/37.
  `docs/X18_VIRTUALIZATION.md`.
- `e4047ef`: on memory pressure the memory guard stops guests only on
  critical pressure (2 checks) or under 12 % free (3 checks), largest guest
  first. The old rule (under 35 % free, once) closed Steam on a 16 GB Mac
  with 2.7 GB of guests. Its other limits (guests over the DRAM setting,
  fseventsd, process count, kernel VM objects and map entries) still stop
  every guest at once. `tests/launcher/safeguard.sh`, 7 checks.
- 2026-09-29: the Steam Frame image diagnosed, extracted and inventoried
  (`docs/STEAM_FRAME_INVENTORY.md`).
- Stage 21's "153 packages" for the Fedora armroot: `scripts/mkarmroot.lock`
  has 150 root and 5 build-only entries (MEASURED count).

## After stage 23 (no stage file)

The post-merge check of 2026-09-29, on the Mac, after the stage 23 branches
were merged (`c636fcc`, `c8bba0f`) and the fixes that followed (`5d9760a`,
`9468028`, `b3f64c8`). MEASURED; the numbers are the ones recorded in those
commit messages and by the check, the launcher logs are
`~/SteamARM-roots/logs/steam-20260929-10*.log`,
`steam-arm64-20260929-10*.log` and `steam-arm64-frame-20260929-100701.log`.

- Tests: `tests/elf/run.sh` 64 passed, 0 failed, 0 expected failures;
  `tests/elf/run_i386.sh` 18 passed, 0 failed (plus its existing
  `smc_subpage` expected failure); `tests/win/run.sh` 11 passed, 0 failed,
  158.7-161.9 fps; `tests/win/run_steam_path.sh` 2/0; `make test-arm64`
  4/0; `tests/elf/run_vk_device.sh` 2/0; `make test-launcher-core` passes
  (`5d9760a`).
- Native arm64 client from the launcher (`scripts/run-app.sh`, V8's JIT
  on): "Steam ARM64 (experimental)" showed the sign-in window at 15-16 s
  (Fedora armroot), "Steam ARM64 · Steam Frame (experimental)" at 21 s
  (Frame root, its first recorded start through `run-app.sh`); **Detener**
  left 0 guest processes. 0 FEX lines in the three logs; every image load
  is an aarch64 PIE (35, 35 and 60). Sign-in not attempted.
- x86 Steam client from the launcher: the main window in 88-93 s in 4 of 5
  starts. The fifth died at start: "SIGTRAP at pc 0x19df7ec58 = outside
  the guest image", which `atos` names `pthread_jit_write_protect_np`+388
  in libsystem_pthread (`steam-20260929-100114.log`, status 133). Cause
  UNKNOWN; since `b3f64c8` such a report names the host pc, lr and up to
  7 callers. Every image load in the x86 logs of the check is `FEX-gb`
  (35 of 35 in each log from `steam-20260929-100948.log` on; 2 of 2 in the
  one that died).

## Tools in this directory

**Stage 1, syscall cost:**

```
clang -O2 -arch arm64 -o build/syscall_cost benchmarks/syscall_cost.c
./build/syscall_cost
```

`stage1-syscall-cost.txt` has the raw output of one run (0.8 ns function
call, 71.5 ns `svc #0x80`, 69.3 ns rewritten `svc`, 0.9 ns answered in user
space, 2789 ns signal trap, 6111 ns Mach exception trap). An earlier run gave
68.1, 114.2, 2794.2 and 6317.0 ns for the rewritten, native, signal and Mach
rows. Two findings decided the design:

1. **`svc` cannot be trapped.** `svc #0` with `x16 = 20` returned the pid:
   Darwin dispatches on x16 and ignores the immediate. A Linux binary's `svc`
   silently runs whatever Darwin call x16 holds. So every `svc` must be
   rewritten at load, and FEX's JIT must call the runtime instead of emitting
   `svc`.
2. **Traps are possible but slow.** An out-of-range number in x16 raises a
   catchable SIGTRAP; at 2.8-6.3 µs per call it is not a usable main path.
   It is why a poisoned site must be a real `brk`, not an `svc`
   (`dbd1657`).

**Stage 5, x86 throughput:** `scripts/run-fex.sh [--trace] <program> [args]`
runs an x86-64 Linux program through FEX under lxrun. `x86_bench.c` is the
freestanding benchmark behind `stage5-x86-throughput.txt`; it builds as
x86-64 and as aarch64 from the same source.

**Stage 5, a W^X flip inside a signal handler:**

```
clang -O1 -o build/wx_in_handler benchmarks/wx_in_handler.c
codesign -s - --entitlements resources/lxrt.entitlements build/wx_in_handler
./build/wx_in_handler
```

Result (`stage5-jit.txt`): a flip made inside a handler holds for the
handler's own stores, but not past its return. So the guest asks for each
flip itself (private syscall `0x4C580020`; `tests/elf/jit_wx.c`).

**Deleted with the VM (2026-09-27):** the VM app's frame-time trace
(`STEAMARM_TRACE`), `frametimes.py`, `capture.sh`, the `runs/vm-baseline-*`
captures, the libkrun VM-exit counters and `resources/SteamARM.entitlements`.
The VM-era results they produced are in `docs/history/PERFORMANCE_BASELINE.md`.

## Method

Fix resolution, settings, FPS cap, V-Sync, display, game version, scene and
duration across every compared run. Report median, 1% low, 0.1% low and the
histogram, never the average alone: smoothness is a claim about the tail.
Compare ZERO-VM against CrossOver and against a native macOS build where one
exists; the VM baseline is history. No game has been measured this way yet
(`docs/PERFORMANCE_BASELINE.md`, "Not measured").
