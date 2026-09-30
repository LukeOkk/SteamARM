# Android apps on a Mac with no VM: feasibility (mission E2)

Status (2026-09-29, stage 25): research only. Nothing in this page has
run Android code under lxrun; no Android program, APK or image was started
on the Mac for it. The record is `benchmarks/stage25-android-research.txt`.
The one MEASURED result that is new here is a native macOS probe of the
memory mapping ART's JIT needs (section 3.2). Everything about Android is
read in source, or taken from upstream documentation, and labelled.

The owner's priority is Android: install APKs and, later, Google Play. The
constraint is `AGENTS.md`: zero VM, no guest kernel, no Apple Hypervisor.
So every Linux kernel service Android expects must come from lxrun, in user
space, on Darwin. This page goes through Android component by component and
says what lxrun has today, what Lepton and Waydroid do, what is missing,
how much work that is and how risky. It ends with a staged roadmap. Play
Store itself is in `docs/PLAY_STORE_RESEARCH.md`; what can be reused from
Valve's Lepton is in `docs/LEPTON_REUSE_ANALYSIS.md`.

**Labels:**

| label | meaning |
|---|---|
| MEASURED | a command was run on the owner's Mac (M4, macOS 27.0) and its output is recorded in `benchmarks/` |
| VERIFIED IN SOURCE | read in a source tree; the repository, commit and file:line are given |
| UPSTREAM DOCUMENTED | the project's own documentation says so; the URL is given |
| COMMUNITY REFERENCE | third-party reports, not confirmed upstream |
| HYPOTHESIS | inferred, not tested |
| UNKNOWN | not established |

**Sources, pinned.** Android is read in LineageOS `lineage-18.1`
(Android 11, API 30), because that is what Lepton's `image/` builds
(`docs/LEPTON_REUSE_ANALYSIS.md` §0) and what Waydroid's 18.1 images are.
Branch heads read on 2026-09-29 through `raw.githubusercontent.com`
(android.googlesource.com answered 503 to the shell):

| repository | commit |
|---|---|
| LineageOS/android_bionic | `b67b03356ceb` |
| LineageOS/android_art | `7f2b48980eaa` |
| LineageOS/android_system_core | `92420679ffbd` |
| LineageOS/android_frameworks_base | `8d14a16b5d75` |
| LineageOS/android_frameworks_native | `103c04dc9ff9` |
| LineageOS/android_build_soong | `91bbda1f3319` |
| llvm/llvm-project (main) | `fce53c80f985` |
| torvalds/linux (master) | `6f8319e3e9a4` |
| Lepton, Valve's GitLab `frame-public/lepton` | `0ea2492` (v3.0.3); mirror commit `6135b53` is its ancestor |
| Lepton submodules (`frame-public`) | `android_hardware_waydroid` `896f652`, `android_device_waydroid_waydroid` `589fd9f` |
| waydroid/android_vendor_waydroid | `e2619850` (Lepton's pin) |

The lxrun side is this repository at `53432eb`; `runtime/` line numbers
are at that commit.

## 1. Summary

1. HYPOTHESIS: Android apps without a VM are possible in principle on
   lxrun, the same way Linux programs are: Android user space is ordinary
   aarch64 Linux ELF code on bionic. Valve's Lepton already shows that
   Android 11 and 14 run in an unprivileged container with SELinux, eBPF,
   uevent, device-mapper and real uid switching patched out
   (`docs/LEPTON_REUSE_ANALYSIS.md` §3.7). What remains are kernel objects
   and a container view that lxrun must provide in user space.
2. The three large pieces, in order of risk:
   - **binder**, a userspace implementation of the kernel driver's
     protocol. Nothing of it exists in lxrun (VERIFIED IN SOURCE, 3.3).
     Every Android service talks through it.
   - **graphics**: a bionic Vulkan HAL over MoltenVK, a gralloc that
     shares buffers between lxrun processes, and a composer that presents
     into a macOS window (3.10-3.11).
   - **init and the container view**: Android's `init` is a static
     executable (HYPOTHESIS: `ET_EXEC`, which lxrun refuses), expects
     `mount(2)`, namespaces, capabilities and cgroups (3.7).
3. MEASURED on the Mac: Darwin refuses an executable mapping of an
   unsigned file (`mmap` of a temporary file with `PROT_EXEC`, shared or
   private, is `EPERM`), but an anonymous region aliased with
   `mach_vm_remap` can be read-execute while the original stays read-write,
   and code written through the writable view runs through the other
   (6 of 6 runs: 3 of the probe as linked, 3 signed with lxrun's
   entitlements). ART's JIT maps a memfd twice in exactly that shape; lxrun
   today turns the executable view into a private copy, so JIT output would
   never become visible there (VERIFIED IN SOURCE, 3.2). Bring-up therefore
   starts with the JIT off.
4. x18: Android reserves x18 for the shadow call stack, and bionic puts a
   pointer in it on every thread (VERIFIED IN SOURCE, 3.1). Darwin zeroes
   it on every exception. lxrun's rewriter covers loaded images today;
   `make lxrt LXRT_KEEP_X18=1` makes the kernel keep it (MEASURED in
   stage 24). Android bring-up should use that build.
5. ABIs: arm64-v8a only, as Lepton (VERIFIED IN SOURCE, Lepton). Apple
   silicon cannot run AArch32, so armeabi-v7a native code has no route
   (HYPOTHESIS: no translator is in scope). x86 and x86_64 Android native
   code would need a native bridge; FEX is not one. That is analysis only
   (section 5). Java-only APKs run whatever ABI list they declare.
6. A different route exists and is noted, not chosen: the Android
   Translation Layer re-implements the Android framework on glibc and GTK4,
   with no binder and no system_server (UPSTREAM DOCUMENTED, section 4). It
   cannot run the Play Store, so it does not serve the owner's goal.

## 2. What Android needs from Linux, and where Lepton and Waydroid get it

| need | Waydroid (LXC) | Lepton (rootless podman) | lxrun today |
|---|---|---|---|
| binder | host kernel `binder_linux` module or binderfs; the host tool creates `anbox-binder`, `anbox-hwbinder`, `anbox-vndbinder` with `BINDER_CTL_ADD` (VERIFIED IN SOURCE, waydroid `c78a305` `tools/helpers/drivers.py:15-99`) | binderfs mounted by the container's own init, then renamed (`binder.rc`, 3.3) | none |
| ashmem | memfd via `sys.use_memfd` (Waydroid patch `system/core/0009`) | the same, set when `/dev/ashmem` is absent | memfd, emulated (3.4) |
| init as PID 1, mounts | LXC; Waydroid patches first-stage init to skip mounts and SELinux (`system/core/0001`, `0002`) | podman `--init=false`, Android `/init` as PID 1 | no PID namespace, no `mount(2)` (3.7) |
| SELinux | off in init, servicemanager, installd, property service (Waydroid and Halium patches) | removed in 18 places (API 34) | not needed |
| uid switching | real uids in the container | one kernel uid; bionic fakes uid/gid; libbinder sends the fake uid in the transaction flags | one Darwin uid; `setuid` to another id is `EPERM` (3.8) |
| display | `hwcomposer.waydroid` presents Android layers as Wayland surfaces | the same, to gamescope's Wayland socket | X11 windows through XQuartz; Vulkan frames as CAMetalLayers hosted in them (3.10) |
| input | the composer writes `struct input_event` into FIFOs that a patched EventHub reads (3.12) | the same | `/dev/input/eventN` from `steamarm-inputd` (3.12) |
| audio | ALSA's `pulse` plugin to the host PulseAudio (3.13) | the same | PulseAudio on CoreAudio (3.13) |
| GPU | Mesa (gbm, zink, freedreno, etc.) on the host DRM device | Turnip over KGSL/msm DRM, Zink for GLES | no DRM device; Vulkan through the shim to MoltenVK (3.11) |
| network | veth into the container | pasta, static `eth0` | host sockets directly; no route netlink (3.14) |

## 3. Component by component

Effort is a rough size for one engineer, a HYPOTHESIS in every row:
S = days, M = one to three weeks, L = one to three months, XL = more.

### 3.1 bionic: linker64 and libc

**What Android does.** Every dynamic executable names
`/system/bin/linker64` as its interpreter (VERIFIED IN SOURCE,
`build/soong` `91bbda1` `cc/binary.go:268-281`). The linker reads its
namespace configuration from `/linkerconfig/ld.config.txt`, or
`/system/etc/ld.config.vndk_lite.txt`, or `LD_CONFIG_FILE`
(`bionic` `b67b033` `linker/linker.cpp:93-98, 3445-3465`). libc maps the
system property areas at start-up (`libc/bionic/libc_init_common.cpp:105`).

**x18 and the shadow call stack.**

- VERIFIED IN SOURCE: bionic allocates a shadow call stack (SCS) for each
  thread and stores its address in x18, "deliberately the only place where
  the address is stored" (`libc/bionic/pthread_create.cpp:109-138`; 8 KiB
  stack in a 16 MiB guard region, `libc/private/bionic_constants.h:23-29`).
  `setjmp`/`longjmp` save the low bits of x18 and rebuild it from the live
  register (`libc/arch-arm64/bionic/setjmp.S:160, 264-265`).
- VERIFIED IN SOURCE: LLVM reserves x18 by default for Android, Darwin,
  Fuchsia, Windows and OHOS targets (`llvm-project` `fce53c8`
  `llvm/lib/TargetParser/AArch64TargetParser.cpp:146-149`). So code the NDK
  compiles for Android does not allocate x18.
- UPSTREAM DOCUMENTED: "SCS assumes that the x18 register is reserved to
  store the address of the ShadowCallStack ... all system libraries are
  compiled to reserve the x18 register"; SCS is enabled per component with
  `sanitize: { scs: true }`
  (https://source.android.com/docs/security/test/shadow-call-stack).
  Which Android 11 components enable it: UNKNOWN (measure it, stage 1).
- ART's compiler blocks x18 (3.2).
- MEASURED (stage 5, `benchmarks/stage5-x18.txt`): Darwin zeroes x18 on
  every exception, signal and preemption. Consequence (HYPOTHESIS): any
  SCS-instrumented function in an Android process would store its return
  address through a null x18 after the first preemption; a `longjmp` would
  rebuild x18 from a zeroed register.
- What lxrun has: every x18 use in a loaded image is rewritten to a
  per-thread virtual x18 (`runtime/x18.c`, `docs/X18_VIRTUALIZATION.md`),
  including bionic's `mov x18, %0`. Each SCS push and pop would then cost a
  trampoline (HYPOTHESIS: measurable on call-heavy code). With
  `make lxrt LXRT_KEEP_X18=1`, lxrun is linked against the macOS 12.3 SDK
  and the kernel keeps x18 across preemption, signals and sleeps on this
  M4 under macOS 27 (MEASURED, `benchmarks/stage24-minecraft-prism.txt`).
  That is why the Android work should use that build: bionic's own use of
  x18 is then native, and the rewriter remains a second line.

**Page size and ELF types.**

- Android 11 predates 16 KiB page support; its images are 4 KiB-aligned
  (HYPOTHESIS; the API 34 Lepton image is VERIFIED IN SOURCE to target
  4 KiB, `docs/LEPTON_REUSE_ANALYSIS.md` §4.4). lxrun loads 4 KiB-aligned
  programs and interpreters through `runtime/subpage.c` since stage 21 and
  reports 4 KiB pages with `LXRT_GUEST_PAGE=4096`
  (`runtime/stack.c:21-34`).
- The cost is stated in `runtime/subpage.c:1-30`: small mappings are
  copied, not shared; write can leak across 4 KiB boundaries inside a
  16 KiB host page. A 4 KiB `PROT_NONE` guard next to a writable page does
  not fault. bionic's thread-stack guards and ART's stack-overflow checks
  rely on such guards (HYPOTHESIS: an overflow becomes a crash, not a
  `StackOverflowError`).
- Static executables: soong links a static bionic executable with
  `-static` and no `-pie` (VERIFIED IN SOURCE, `cc/binary.go:254-266`;
  dynamic ones get `-pie`, `:302`). HYPOTHESIS: `/system/bin/init` is
  therefore `ET_EXEC`, which lxrun refuses (`runtime/elf.c:113-120`; the
  low 4 GiB belong to macOS, `benchmarks/stage2-pagezero.txt`). The linker
  itself is linked `-shared` (`cc/binary.go:256-258`), so it is `ET_DYN`.
  Stage 1 measures both.

| | |
|---|---|
| lxrun today | TLS rewrite for `TPIDR_EL0` (`runtime/tls.c`), x18 (`runtime/x18.c`), 4 KiB guest pages (`runtime/subpage.c`), PIE loading (`runtime/elf.c`), `uname` reports Linux 6.6 (`runtime/dispatch.c:2070-2083`). Tested only with glibc programs. |
| Lepton / Waydroid | run the same bionic on a real kernel; Lepton patches bionic to fake uids |
| effort | S to M: probe `linker64` and `toybox`, fix what `LXRT_REPORT_ENOSYS=1` names |
| risk | M: `ET_EXEC` init; guard pages; property areas missing outside init |

### 3.2 ART: dalvikvm, app_process, dex2oat and the JIT

**The JIT code cache.** VERIFIED IN SOURCE (`art` `7f2b489`):

- A process's JIT cache is one memfd, `memfd_create("jit-cache")`, mapped
  twice: the executable view `kProtRX` over the memfd, and a separate
  never-executable view that is made writable for updates
  (`runtime/jit/jit_memory_region.cc:60-110, 175-215`). "Without using RWX
  page permissions, the JIT can not fallback to single mapping"
  (`:80-83`). For the region an app gets after the zygote fork, RWX is
  allowed except in system_server (`runtime/jit/jit_code_cache.cc:1866`).
- The zygote's JIT cache "requires dual code mappings by design" and a
  sealable memfd (`jit_memory_region.cc:507-531`).
- ART's `memfd_create` returns `ENOSYS` unless `uname` says Linux 3.17 or
  later (`libartbase/base/memfd.cc:57-66`). lxrun reports `6.6.0-lxrt`
  (`runtime/dispatch.c:2077-2079`), so ART will use memfd.

**What lxrun does with that.** VERIFIED IN SOURCE:

- A `PROT_EXEC` file mapping is brought in writable so its `svc` sites can
  be rewritten, and a `MAP_SHARED` one is forced to `MAP_PRIVATE`
  (`runtime/dispatch.c:1064-1079`). ART's executable view of the memfd
  therefore becomes a private snapshot. Code ART later writes through the
  writable view reaches the memfd, not the snapshot (HYPOTHESIS: the JIT's
  first compiled method executes stale bytes).
- A memfd is an unlinked temporary file (`runtime/fex_support.c`); its
  seals are bookkeeping inside one process (`:178-217`), which is enough
  for ART's `F_SEAL_FUTURE_WRITE` probe.

**What Darwin allows.** MEASURED on the Mac (native probe
`benchmarks/dual_view_jit.c`, 3 runs as linked and 3 signed with
`resources/lxrt.entitlements` as `build/lxrun` is, the same result each
time; `benchmarks/stage25-android-research.txt`):

- `mmap(PROT_READ|PROT_EXEC, MAP_SHARED)` of an unlinked temporary file:
  `Operation not permitted`. `MAP_PRIVATE` with `PROT_EXEC`: the same.
- An anonymous region aliased with `mach_vm_remap`, the alias made
  read-execute and the original left read-write: code written through the
  original runs through the alias, and a rewrite of it is seen on the next
  call. lxrun already aliases pages this way for other
  reasons (`LNR_lxrt_alias`, `runtime/dispatch.c:2230-2248`).

**Options.**

1. JIT off for bring-up: `dalvik.vm.usejit=false`, with apps compiled
   ahead of time by `dex2oat64` at install (Lepton compiles
   `speed-profile`, `docs/LEPTON_REUSE_ANALYSIS.md` §4.5). The `.oat`
   files of the boot image and of apps are ELF files mapped executable
   `MAP_PRIVATE`, which lxrun already rewrites and flips (HYPOTHESIS: no
   special case is needed). The Android Translation Layer documents the same fallback
   on Apple silicon with 16 KiB pages: `-Xusejit:false` and the interpreter
   (UPSTREAM DOCUMENTED, ATL `README.md`).
2. Later, a dual view in lxrun: back a memfd that is mapped executable
   with anonymous memory, and map its views as `mach_vm_remap` aliases of
   one object (the MEASURED shape above). lxrun's rule that no executable
   page holds unscanned words would need the alias pair handled the way
   `runtime/wxsplit.c` handles RWX pages: write-protect the writable view
   once a page is scanned, and on a write fault make the executable view
   non-executable until the next fetch rescans it. ART's generated code
   never uses x18 (VERIFIED IN SOURCE, below) and keeps the current thread
   in x19; HYPOTHESIS: it also contains no `svc` and no `TPIDR_EL0` access,
   so a scan finds nothing to rewrite. Effort M.

**ART and x18.** VERIFIED IN SOURCE: ART's arm64 optimizing compiler
blocks x18 as the platform register (`compiler/optimizing/code_generator_arm64.cc:1288-1304`),
the thread register is x19 (`runtime/arch/arm64/registers_arm64.h:63`),
and the quick entrypoints skip x18 when they save and restore registers
(`runtime/arch/arm64/quick_entrypoints_arm64.S:861, 2247, 2366, 2423`).
Unlike Linux HotSpot and llvmpipe (stage 24), ART's JIT output is not
exposed to Darwin's x18 zeroing.

| | |
|---|---|
| lxrun today | fork, threads, signals (ART's implicit null and suspend checks are SIGSEGV-driven), `membarrier`, memfd, madvise; no dual view |
| Lepton / Waydroid | stock ART on a real kernel; Lepton sets `usejit=true` |
| effort | M (JIT off); M more for the dual view |
| risk | M: SIGSEGV-driven checks under lxrun's signal path; 4 KiB guards (3.1); zygote fork cost with a large preloaded heap (UNKNOWN) |

### 3.3 binder, binderfs and the three service managers

**What Android needs.** VERIFIED IN SOURCE:

- AOSP's `init.rc` mounts binderfs at `/dev/binderfs` and symlinks
  `/dev/binder`, `/dev/hwbinder` and `/dev/vndbinder` to it
  (`system_core` `9242067` `rootdir/init.rc:175-189`). A binderfs mount
  creates `binder-control` plus the devices named in the kernel's
  `binder.devices` parameter (`linux` `6f8319e` `drivers/android/binderfs.c:672-685`);
  more are added with `BINDER_CTL_ADD` on `binder-control`
  (UPSTREAM DOCUMENTED, https://docs.kernel.org/admin-guide/binderfs.html).
- libbinder opens the device and maps a receive buffer of
  1 MiB minus two pages, `PROT_READ`, `MAP_PRIVATE` on the binder fd
  (`frameworks_native` `103c04d` `libs/binder/ProcessState.cpp:43, 400`).
- The protocol is ioctls on that fd: `BINDER_WRITE_READ` carrying `BC_*`
  commands and returning `BR_*` replies, `BINDER_VERSION` (protocol 8 on
  64-bit), `BINDER_SET_MAX_THREADS`, `BINDER_SET_CONTEXT_MGR(_EXT)`,
  `BINDER_FREEZE`; objects in a transaction are binder nodes, handles,
  file descriptors (`BINDER_TYPE_FD`, `BINDER_TYPE_FDA`) and
  scatter-gather buffers (`BINDER_TYPE_PTR`, used by hwbinder)
  (`include/uapi/linux/android/binder.h:36-38, 193-195, 267-271`). The
  driver keeps node and handle reference counts, delivers death
  notifications, queues one-way calls per node, grows thread pools with
  `BR_SPAWN_LOOPER`, and reports the caller's pid and euid.
- The Linux driver is `drivers/android/binder.c` (7,186 lines at
  `6f8319e`) plus `binder_alloc.c`. It is GPL-2.0; SteamARM's code is MIT,
  so an implementation must be written from the UAPI header (which carries
  the syscall note) and the documented behaviour, not copied (HYPOTHESIS on
  what counsel would require).
- Three context managers: `servicemanager` on `/dev/binder` (AIDL),
  `hwservicemanager` on `/dev/hwbinder` (HIDL) and `vndservicemanager` on
  `/dev/vndbinder`. `servicemanager` is `critical` in init
  (`cmds/servicemanager/servicemanager.rc`).
- Android 11's libbinder takes the caller's uid from the kernel. Lepton,
  with one kernel uid for all processes, writes the fake uid into the
  transaction flags and reads it back (Lepton patch
  `frameworks/native/1004`, `system/libhwbinder/1000`); Waydroid and Halium
  never set `TXN_SECURITY_CTX` and drop SELinux checks in the service
  managers (`frameworks/native/0002`, `0003`).

**What lxrun has.** VERIFIED IN SOURCE: nothing of binder. `/dev/binder`
resolves inside the guest root, where it is not a device
(`runtime/dispatch.c:412-430`); an unknown `ioctl` is `ENOTTY`
(`runtime/ioctl_tty.c:253-258`); `mount(2)` has no case and returns
`ENOSYS` (`runtime/dispatch.c:3613`). The building blocks exist: AF_UNIX
sockets with `SCM_RIGHTS` and emulated `SO_PEERCRED`
(`runtime/socket.c:568-700`), cross-process futexes keyed by VM object
(`runtime/futex_ops.c:48-60`), shared anonymous memory across fork, SysV
shared memory (`runtime/sysv_ipc.c`) and memfd.

**Design options.** HYPOTHESIS, all of them:

1. A broker. One process (native macOS, or an lxrun guest) holds the
   driver state: nodes, handles, refcounts, per-process transaction queues,
   thread pools, death notifications. In each guest, `open("/dev/binder")`
   (and `hwbinder`, `vndbinder`) gives an AF_UNIX connection to it;
   `mmap` of that fd gives a shared memory region the broker also maps;
   `ioctl(BINDER_WRITE_READ)` sends the write buffer and blocks for the
   read buffer; the broker copies each payload once into the target's
   region, as the kernel does. File descriptors travel with `SCM_RIGHTS`
   and are installed in the target when it reads its `BR_TRANSACTION`.
   The broker sees a peer close and sends the death notifications. It knows
   each peer's pid and can report a per-process virtual uid, which makes
   Lepton's uid-in-flags patch unnecessary.
2. The same state in a shared mapping among lxrun processes, with futex
   wake-ups and no broker round trip. Faster; much harder to keep
   consistent when a process dies holding a lock.
3. RPC binder (binder over sockets) exists in newer libbinder:
   "the Binder protocol has been rewritten to work over sockets"
   (UPSTREAM DOCUMENTED, https://source.android.com/docs/core/virtualization/microdroid).
   It is not in Android 11 (HYPOTHESIS) and covers AIDL sessions set up
   explicitly, not the whole framework. Not a route.

| | |
|---|---|
| lxrun today | none (above) |
| Lepton / Waydroid | the host kernel's driver; binderfs mounted inside the container (Lepton `compat_tool/images/rootfs_overlay/system/etc/init/binder.rc`) |
| effort | L: the largest single piece; thousands of lines and a protocol test suite |
| risk | H: every service depends on it; latency per transaction (UNKNOWN; SurfaceFlinger, the app and system_server exchange many per frame) |

### 3.4 ashmem and memfd

VERIFIED IN SOURCE: libcutils uses memfd instead of `/dev/ashmem` only when
`sys.use_memfd` is true, the vendor VNDK level allows it, and
`F_SEAL_FUTURE_WRITE` can be set (`system_core` `libcutils/ashmem-dev.cpp:98-185`).
Waydroid patches init so the property can be set before boot
(`system/core/0009`); Lepton sets it when `/dev/ashmem` is absent
(`compat_tool/liblepton/properties.sh:19-22`, at Lepton `6135b53`).
The Waydroid gralloc falls back to ashmem buffers
(`hardware/libhardware/0005`).

lxrun (VERIFIED IN SOURCE): memfd is an unlinked temporary file; seals are
in-process bookkeeping only (`runtime/fex_support.c:178-217`); a process
can hold at most 256 memfd objects and the next `memfd_create` is `EMFILE`
(`:219, 353`). A large memfd is written to disk rather than kept in the page
cache (`:151-162`).

| | |
|---|---|
| effort | S |
| risk | M: 256 per process is low for a compositor holding every app's buffers (HYPOTHESIS); disk-backed graphics buffers (HYPOTHESIS: slow) |

### 3.5 System properties

VERIFIED IN SOURCE (`bionic`, `system_core`):

- init creates `/dev/__properties__/` with a `property_info` file and one
  128 KiB area per SELinux context (`init/property_service.cpp:995-999,
  1072`; `libc/system_properties/prop_area.cpp:44, 87`). Every process maps
  them read-only at libc start-up (`libc_init_common.cpp:105`) and waits on
  a property's serial with a futex.
- Writes go to init over the stream socket `/dev/socket/property_service`
  with `PROP_MSG_SETPROP2` (`libc/bionic/system_property_set.cpp:51-66, 284`);
  init checks the caller's credentials.

lxrun: shared file mappings, cross-process futexes and `SO_PEERCRED` exist
(3.3). HYPOTHESIS: a property area mapped `MAP_SHARED` by two lxrun
processes is coherent through the host page cache, and a futex wait in one
is woken by the other, since both map the same VM object. Not tested across
unrelated processes.

Update, stage 26 (MEASURED, `benchmarks/stage26-android-properties.txt`):
done without init, as a host property service (`runtime/propsvc.c`). What
properties need held across unrelated processes: guests read the service's
writes to the shared areas, and a guest's `__system_property_wait` is woken
by the service's process-shared ulock wake on the same file. See
`docs/ANDROID_RUNTIME_ARCHITECTURE.md`, "Properties".

| | |
|---|---|
| effort | S with Android's init; M if SteamARM has to write the property service itself (3.7) |
| risk | L |

### 3.6 logd

VERIFIED IN SOURCE: logd listens on `logd` (stream), `logdr` (seqpacket)
and `logdw` (datagram with credentials) (`system_core` `logd/logd.rc:2-4`).
lxrun turns `SOCK_SEQPACKET` into a Darwin datagram socket
(`runtime/socket.c:268-281`). HYPOTHESIS: a datagram socket cannot
`listen`/`accept`, so `logdr` and `logcat` fail; writers (`logdw`) work.

Option: skip logd and give lxrun a small `logdw` reader that prints to the
session log. Effort S, risk L. A real SEQPACKET listener would matter later
for `lmkd` and `tombstoned` too (Lepton disables both, patches
`system/memory/lmkd/1000`, `system/core/1006`).

### 3.7 init, and what it expects

VERIFIED IN SOURCE (`system_core`):

- First stage mounts `/dev`, `/proc`, `/sys` and selinuxfs and loads the
  SELinux policy; Waydroid's patch skips those outside recovery, where LXC
  has done them, and goes straight to second stage
  (`android_vendor_waydroid` `e2619850` `waydroid-patches/base-patches-30/system/core/0001`).
- SELinux: enforcing unless `androidboot.selinux=permissive`
  (`init/selinux.cpp:92-106`); Waydroid and Lepton patch the SELinux calls
  out of init, the property service and the service managers.
- APEX: init creates bootstrap and default mount namespaces with
  `unshare(CLONE_NEWNS)` and `setns` (`init/mount_namespace.cpp:280-331`).
- Services start with `user`, `group`, capabilities and
  `writepid /dev/cpuset/...` (`init/service.cpp`; e.g. zygote,
  `rootdir/init.zygote64.rc`); cgroups come from `cgroups.json` through
  libprocessgroup (`libprocessgroup/cgroup_map.cpp`).
- ueventd: disabled in both containers (`ro.cold_boot_done=true`; Waydroid
  `system/core/0013`).

lxrun (VERIFIED IN SOURCE): no `mount`, `umount2`, `pivot_root`, `unshare`,
`setns` or `seccomp` case; they return `ENOSYS` (`runtime/dispatch.c:3613`,
the number list at `:66-140`). `clone` without `CLONE_THREAD|CLONE_VM` is a
plain fork and namespace flags are ignored (`runtime/thread.c:467-470`).
`getpid` is the Darwin pid (`runtime/dispatch.c:3378`), so there is no PID 1.
The only mount view is the per-process bind table of the bwrap emulation,
at most 256 entries (`runtime/mounts.c:20-39`). No cgroup filesystem.

**Options.**

1. A SteamARM "Android mode" launcher that does what podman and init do
   for Lepton: build the guest root's `/dev` (socket directory, property
   areas, binder device names that lxrun routes to the broker), run a
   property service, and start `servicemanager`, `hwservicemanager`,
   `vndservicemanager`, `logd`-replacement, the HALs, `surfaceflinger` and
   `zygote` in dependency order. Lepton's `.rc` overlay and property list
   are the reference (MIT, `docs/LEPTON_REUSE_ANALYSIS.md`). Effort M.
2. Android's own init under lxrun: needs a PIE build of init (HYPOTHESIS:
   it is `ET_EXEC` today), Waydroid's container patches, and lxrun answers
   for `mount` (redirect into the bind table), `unshare`/`setns` (no-ops in
   an "Android mode"), cgroup files and `signalfd`. Rebuilding init means
   an AOSP build: "a 64-bit x86 system", 400 GB of disk and 64 GB of RAM,
   and "Android OS development on macOS isn't supported as of June 22, 2021"
   (UPSTREAM DOCUMENTED, https://source.android.com/docs/setup/start/requirements).
   That build cannot run on the owner's Mac without a VM; it would run on a
   Linux x86-64 machine or CI. Effort M plus the build infrastructure.

Recommendation (HYPOTHESIS): option 1 for stages 1-3; decide on option 2
when stage 3 shows which init behaviours the framework really needs.

### 3.8 zygote

VERIFIED IN SOURCE (`frameworks_base` `8d14a16`
`core/jni/com_android_internal_os_Zygote.cpp`): for each app the zygote
forks, then in the child

- calls `setgroups` (`:527-548`), `setresuid`/`setresgid` (`:1694`),
  `capset` twice (`:685, 706`), each fatal on failure;
- `unshare(CLONE_NEWNS)` for storage (`:557`), fatal on failure;
- installs the app seccomp filter unless SELinux is not enforcing
  (`:634-648`); `gIsSecurityEnforced = security_getenforce()` (`:2285`);
- `selinux_android_setcontext` (`:1774`), fatal on failure.

lxrun answers today (VERIFIED IN SOURCE): `setgroups` has no case
(`ENOSYS`); `setuid` to another id is `EPERM`
(`runtime/dispatch.c:3330-3348`); `capset` is `EPERM` unless it asks for
nothing (`:2467-2494`); `unshare` is `ENOSYS`; `prctl` options it does not
know return 0 (`:2943-2985`), and bionic installs seccomp filters with
`prctl(PR_SET_SECCOMP)` (`libc/seccomp/seccomp_policy.cpp:178`), so a
filter would be "accepted" and never applied. HYPOTHESIS: without SELinux,
`security_getenforce` returns -1, which is true in C, so the filter path is
taken and is then silently a no-op; the fatal calls above end the child.

What is needed: an lxrun "Android mode" that answers these as Lepton's
seccomp profile does (success for `setgroups`, `setuid`, `setgid`, the
chown family, `capset`; per-process virtual uid for `getuid`), and treats
`unshare(CLONE_NEWNS)` as success. Lepton's bionic fake-uid patch is the
alternative on the image side. Waydroid's image already changes part of
this path (`frameworks/base/0001-waydroid-disable-SELinux-parts.patch`,
`0047-Zygote-Fix-dropping-capabilities-in-containers.patch`, VERIFIED IN
SOURCE by name; their effect under lxrun is UNKNOWN). Effort M; risk M (fork of a zygote with a
large preloaded heap on Darwin: cost UNKNOWN).

### 3.9 system_server

Android 11's `SystemServer` starts about a hundred services
(`services/java/com/android/server/SystemServer.java`); only four can be
switched off with properties (`config.disable_otadexopt`,
`config.disable_systemtextclassifier`, `config.disable_networktime`,
`config.disable_cameraservice`, `:884-1046`). Lepton comments out many more
in source (patch `frameworks/base/1015-lepton-disable-many-more-services`:
vibrator, consumer IR, VR, accessibility, OEM lock, IpSec, Wi-Fi scanning,
NSD, device storage monitor, broadcast radio, MIDI, twilight, backup,
context hub, print, restrictions, HDMI, TV remote, face) and removes about
ninety LineageOS and AOSP packages from the build
(`image/android_vendor_valve/manifest_scripts/manifests-30/11-removes.xml`).

What system_server needs from below (HYPOTHESIS unless noted): binder (all
of it), the HIDL HALs registered with `hwservicemanager` (health, power,
light, memtrack, keymaster, gatekeeper, allocator, composer; Waydroid
provides software or stub versions, VERIFIED IN SOURCE,
`android_hardware_waydroid` `896f652`), `installd` for app data and dexopt,
`vold` for storage (Lepton patches it to bind-mount instead of FUSE),
SurfaceFlinger with a primary display, and InputFlinger.

A "headless" system_server is not a switch in Android 11. It is a patched
build, or a composer that reports a display and drops frames (Lepton's
`lepton.headless=true` keeps Waydroid's composer and hides the window,
`compat_tool/liblepton/properties.sh:123-137`).

| | |
|---|---|
| effort | L (mostly on binder, HALs and installd underneath) |
| risk | H: unknown number of kernel assumptions surface only at this stage |

### 3.10 SurfaceFlinger, hwcomposer and gralloc

**Waydroid and Lepton.** VERIFIED IN SOURCE (`android_hardware_waydroid`
`896f652`; device tree `589fd9f`):

- The device builds HWC2 with gralloc 4 (`BoardConfig.mk`), the composer
  2.1 service, `hwcomposer.waydroid` and allocators for minigbm, gbm and
  Qualcomm (`device.mk`, "Display").
- `hwcomposer.waydroid` is an HWC1 module (it composes `hwc_layer_1_t`)
  that is a Wayland client. A gbm or minigbm buffer becomes a
  `zwp_linux_dmabuf_v1` buffer (`hwcomposer/gralloc_handler.cpp:175-195`);
  any other gralloc's buffer is copied into a `wl_shm` pool backed by a
  memfd (`:80-110`, chosen at `:404-414`). Gralloc type comes from the
  property (`hwcomposer/wayland-hwc.cpp:1963-1977`).
- `system.prop` runs SurfaceFlinger "without sync framework" and latches
  unsignaled buffers (`ro.surface_flinger.running_without_sync_framework=true`,
  `debug.sf.latch_unsignaled=1`).
- Lepton's default renders SurfaceFlinger itself with Zink over Turnip
  (`docs/LEPTON_REUSE_ANALYSIS.md` §4.1).

**What SteamARM has.** VERIFIED IN SOURCE: X11 windows (XQuartz, rootless,
quartz-wm) and, for Vulkan, a CAMetalLayer created by lxrun
(`runtime/remote_layer.m:1-20`, private syscall `0x4C580012`) whose Core
Animation context id is shown inside the X window through CALayerHost, with
no copy (MEASURED, `benchmarks/stage12-native-present.txt`). SysV shared
memory for MIT-SHM works (`runtime/sysv_ipc.c`, stage 24 Heroic).

**Stage 27 (MEASURED, `benchmarks/stage27-android-display.txt`,
`docs/ANDROID_RUNTIME_ARCHITECTURE.md` "Display"):** Waydroid's own
composer works unchanged against Weston running under lxrun: the x86_64
image's SurfaceFlinger (under FEX, SwiftShader, gralloc "default", memfd
buffers) presents its boot animation through `hwcomposer.waydroid` as
`wl_shm` buffers to Weston, whose X11 window on :2 is a macOS window, at 57
frames/s. So the list below is now an optimisation (fewer processes and
copies), not a prerequisite; the GPU path (3.11) still is one.

**What SteamARM needs.** HYPOTHESIS:

1. A composer HAL ("hwcomposer.steamarm") that is an X11 client: a CPU
   path first (copy SurfaceFlinger's client-composited target into an X
   window with MIT-SHM), then a Metal path (present the target as a Vulkan
   swapchain on a remote CAMetalLayer, exactly as Linux Vulkan games
   present today). Waydroid's HWC1 structure and its multi-window modes
   are the reference (Apache-2.0).
2. A gralloc whose buffers two lxrun processes can map: memfd (ashmem)
   buffers first; IOSurface-backed buffers, shared through a host
   service, when GPU rendering needs zero-copy (UNKNOWN: nothing in lxrun
   passes Mach ports between guests today).
3. SurfaceFlinger with `running_without_sync_framework`, as Waydroid.

| | |
|---|---|
| effort | L |
| risk | M: the CPU path is ordinary code; the GPU path depends on 3.11 |

### 3.11 GLES and Vulkan

VERIFIED IN SOURCE (`frameworks_native` `103c04d`): the Vulkan loader
loads `vulkan.<ro.hardware.vulkan>.so` (or `ro.board.platform`) as a HAL
module with id `HWVULKAN_HARDWARE_MODULE_ID`
(`vulkan/libvulkan/driver.cpp:150-260`) and implements `VK_KHR_swapchain`
itself on top of the driver's `VK_ANDROID_native_buffer`
(`:540-621, 989, 1175-1180`). EGL loads ANGLE first if configured, else
`lib*_<ro.hardware.egl>.so` (`opengl/libs/EGL/Loader.cpp:143-147, 225-279`).
Lepton's API 30 image also carries SwiftShader EGL/GLES, `vulkan.pastel`
and ANGLE (`android_device_waydroid_waydroid` `device.mk`, last package
list); Lepton's CI uses SwiftShader for API 30 and ANGLE with `pastel` for
API 34 (`properties.sh:52-62`).

SteamARM today: `shim/libvulkan.so.1` is a Linux Vulkan driver over
MoltenVK (or KosmicKrisp), linked `-nostdlib` (`Makefile:117-122`), loading
the host library through lxrun (`shim/vulkan_shim.c:545-561`), with X11 WSI
only (`shim/wsi.c:122-130`). Zink over it gave a GL 3.2 core context with
correct pixels in stage 24 (MEASURED, `benchmarks/stage24-minecraft-prism.txt`).
Host glibc Mesa cannot be loaded into a bionic process (initial-exec TLS,
`docs/LEPTON_REUSE_ANALYSIS.md` §4.3).

Paths, in order (HYPOTHESIS):

1. Software first: SwiftShader (in the image) for GLES and
   `vulkan.pastel` for Vulkan. No new driver code; SurfaceFlinger and
   apps render on the CPU. SwiftShader's JIT emits code for the Android
   target, where x18 is reserved (3.1).
2. A bionic build of the shim as `vulkan.steamarm.so`: the HAL module
   struct, the entry points it already has, and `VK_ANDROID_native_buffer`
   (import a gralloc buffer as a MoltenVK image). Then GLES through ANGLE's
   Vulkan back end or Zink built for Android, as Lepton does over Turnip.
   MoltenVK's gaps (geometry shaders, transform feedback: `README.md`)
   carry over.

| | |
|---|---|
| effort | S (software), L (the HAL) |
| risk | M |

### 3.12 Input

VERIFIED IN SOURCE: Waydroid's composer creates FIFOs
`/dev/input/wl_{touch,keyboard,pointer,tablet}_events` and writes
`struct input_event` records into them (`hwcomposer/wayland-hwc.h:75-79`,
`wayland-hwc.cpp:1334-1365, 1853`); its EventHub patches accept those FIFOs
as devices without `EVIOC*` ioctls (`frameworks/native/0004`, `0006`,
`0012`). SteamARM's lxrun serves `/dev/input/eventN` from
`steamarm-inputd` with the `EVIOC*` ioctls, `read` of `input_event` records
and force feedback (`runtime/evdev.c:1-20`). Android's EventHub finds
devices with inotify on `/dev/input` and waits with epoll
(`frameworks_native` `services/inputflinger/reader/EventHub.cpp:72, 297-301`);
lxrun implements both (`runtime/inotify.c`, `runtime/epoll_eventfd.c`).

So controllers can reach Android through the existing evdev path
(HYPOTHESIS: Android's key layout files cover the identities inputd
reports). Pointer, keyboard and touch from the macOS window would come from
the composer's X client, written as `input_event` records into Waydroid's
FIFOs. Effort S-M, risk L.

### 3.13 Audio

VERIFIED IN SOURCE: `audio.primary.waydroid` opens the ALSA PCM `pulse`
(`audio/audio_hw.c:164`), and the image carries the ALSA pulse plugin
(`device.mk`, "Audio HAL"); the PulseAudio socket path is a property
(`waydroid.pulse_runtime_path`, Lepton `properties.sh`). SteamARM runs a
PulseAudio server on CoreAudio with a socket inside the guest root
(`docs/ARCHITECTURE.md`). Reuse as is (HYPOTHESIS). Effort S, risk L.

### 3.14 Networking

Android decides it is online through ConnectivityService, which learns
interfaces from netd, which uses route netlink and runs `iptables-restore`
(HYPOTHESIS for the details in Android 11). Lepton gives Android a static
`eth0` through pasta and a generated `ipconfig.txt`
(`docs/LEPTON_REUSE_ANALYSIS.md` §3.5); Waydroid patches in a fake Wi-Fi
connection for selected apps (`frameworks/base/0041`).

lxrun (VERIFIED IN SOURCE): guest sockets are host sockets on the Mac's own
interfaces; abstract AF_UNIX names live in a directory
(`runtime/socket.c:56-67`); netlink exists only as a silent
`NETLINK_KOBJECT_UEVENT` socket (`:342-360`); there is no `NETLINK_ROUTE`.

HYPOTHESIS: raw sockets work at once; Android-level connectivity (what
Play Store and many apps check) needs either a minimal route-netlink
emulation that shows one `eth0` with the Mac's address, plus netd's
firewall calls failing softly, or a patched ConnectivityService. DNS goes
through netd's `dnsproxyd`. Effort M-L, risk M.

## 4. The route not taken: Android Translation Layer

UPSTREAM DOCUMENTED (https://gitlab.com/android_translation_layer/android_translation_layer,
`README.md` and `doc/Architecture.md`, read 2026-09-29; GPL-3.0,
`LICENSE.txt`): ATL runs an APK directly on desktop Linux. It makes "a
chirurgical cut ... directly between the Apps and the Java APIs provided by
the android frameworks": the app's dex runs on a standalone ART, its native
`.so` files load through a bionic-compatibility layer, and the framework
classes are re-implemented on GTK4. It has no binder, no system_server and
no Android image. On Apple silicon it documents running ART with
`-Xusejit:false` and the interpreter.

HYPOTHESIS: ATL could run under lxrun in the Fedora armroot as a glibc
program, and would reach simple APKs sooner than the full stack. It cannot
run Play Store or Google Play services, which need the real framework, so
it does not meet the owner's goal. It stays a reference.

## 5. ABIs

- arm64-v8a: the target. Lepton's image is arm64 only, 64-bit only, with no
  native bridge (VERIFIED IN SOURCE, `docs/LEPTON_REUSE_ANALYSIS.md` §4.5).
- armeabi-v7a: Apple silicon has no AArch32 execution state (HYPOTHESIS as
  stated here; not measured by this project). No route in scope.
- x86 and x86_64: Android loads such libraries only through a native
  bridge (`ro.dalvik.vm.native.bridge`, e.g. libhoudini or ndk_translation;
  HYPOTHESIS on the mechanism). FEX translates whole Linux processes; it is
  not a native bridge, and running an x86_64 Android image under FEX would
  put bionic, ART and every service under translation. Analysis only; not
  planned.
- Java-only APKs: run on any ABI.

## 6. Which Android root

| candidate | what it is | licence | on the owner's Mac | use |
|---|---|---|---|---|
| Waydroid VANILLA `lineage-18.1-*-waydroid_arm64` | LineageOS 18.1 with Waydroid's patches, no Google apps, published by the Waydroid project on SourceForge through `ota.waydro.id` (MEASURED metadata: 122 arm64 18.1 VANILLA builds, the last `20250628`) | the component licences of LineageOS/AOSP (Apache-2.0, GPL for some parts) | a copy is in `~/SteamARM-roots/android/images` (state, not repo; another session's download, not read here) | stages 1-3 (HYPOTHESIS: its Waydroid patches cover SELinux, uevent and memfd; uid switching is left to lxrun) |
| Lepton `image/` built from source | LineageOS 18.1, Waydroid patches plus Valve's 101 (single uid, no bpf, no ueventd, trimmed services) | GPL-3.0 as a combined image, per Valve (`LICENSES/image.md`) | cannot be built on the Mac (AOSP needs x86-64 Linux, 3.7) | later, built on a Linux machine, if Valve's single-uid patches are needed |
| Valve's `rootfs.tar.zst` from the Steam depot | Lepton's prebuilt image | never shipped by SteamARM (PROPRIETARY_DO_NOT_REDISTRIBUTE by this repo's policy) | only if the owner's Steam installed it | reference, read in place |

## 7. Roadmap

Each stage has an entry test that must pass on the Mac before the next
starts. Until stage 5 passes, the launcher shows Android as unavailable
with the reason (the stage that has not passed), never as a working card.

**Stage 1: bionic and ART, standalone.**
- Measure: `llvm-readelf -hlW` of `system/bin/linker64`, `init`,
  `app_process64`, `toybox`, `dalvikvm64`, `libc.so`, `libart.so` in the
  VANILLA root (ELF type, `p_align`); count SCS prologues
  (`str x30, [x18], #8`) across `system/lib64` to know how much x18 traffic
  there is.
- Run `toybox` and a small NDK program under `build/lxrun` built with
  `LXRT_KEEP_X18=1`, `LXRT_GUEST_PAGE=4096`, `LXRT_REPORT_ENOSYS=1`, with a
  handmade `/dev/__properties__` (or none) and `LD_CONFIG_FILE`.
- Run `dalvikvm64 -Xusejit:false` on a hello-world dex, then with an
  AOT-compiled `.oat`.
- Exit: `toybox`, the NDK program and `dalvikvm64` print their output,
  repeatedly, with no poisoned site hit and no unexplained `ENOSYS`.

**Stage 2: binder and servicemanager.**
- Write the broker (3.3, option 1) and lxrun's `/dev/binder`,
  `/dev/hwbinder`, `/dev/vndbinder` routing, ioctls and `mmap`.
- A protocol test suite first: two processes, transactions, one-way calls,
  fd passing, death notifications, thread pool growth.
- Exit: `servicemanager` starts; `service list` and `service call` from
  another process work; `hwservicemanager` and one HIDL HAL register.

**Stage 3: a minimal headless system_server.**
- The Android-mode launcher (3.7 option 1), property service, logdw
  reader, the Android-mode syscall answers (3.8), `zygote` with the JIT
  off, a null composer that reports one display, SwiftShader.
- Exit: `sys.boot_completed=1`; `pm list packages`; `pm install` of a
  Java-only APK succeeds (installd, dex2oat).

**Stage 4: SurfaceFlinger into a macOS window.**
- The X11 composer (CPU path), memfd gralloc, input FIFOs from the X
  window.
- Exit: the launcher's home activity is drawn in a macOS window and
  responds to the pointer; timing recorded.

**Stage 5: launch an APK activity.**
- `am start` of an installed arm64-v8a APK with native code; audio through
  PulseAudio; a controller through inputd.
- Exit: the app's activity is interactive; SteamARM's launcher gets an
  Android card that launches it for real, with a clean stop.

**Stage 6: Play Store with a user-provided GAPPS image.**
- Only after stage 5, and only as `docs/PLAY_STORE_RESEARCH.md` sets out:
  networking that Android considers online, WebView, the user's own GApps
  or GAPPS image, the user's own registration of the device's GSF ID, and
  the user signing in themselves. SteamARM never bundles Google software,
  never falsifies certification and never bypasses Play Integrity.

## 8. Open questions

- The ELF type of Android's `init` and the `p_align` of the platform
  binaries (stage 1 measures them).
- How many Android 11 components use SCS.
- Binder transaction latency through a broker on Darwin, against what
  SurfaceFlinger and the framework need per frame.
- Whether two lxrun processes mapping the same property file see each
  other's futex wakes. Answered at stage 26 for what properties need: two
  unrelated host processes meet on one wait queue through a shared file,
  and the property service's wake reaches a guest's `FUTEX_WAIT`
  (MEASURED, `benchmarks/stage26-android-properties.txt`). Two guests
  waking each other through a file was not run.
- The cost of forking a zygote with a large preloaded heap under lxrun.
- Whether Mach ports (for IOSurface sharing) can be passed between lxrun
  guests.
