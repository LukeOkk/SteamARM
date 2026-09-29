# Android userspace on lxrun, with no VM

Status 2026-09-29, stage 26 (`benchmarks/stage25-android-userspace.txt`,
`benchmarks/stage25-binder.txt`, `benchmarks/stage25-art-x86-fex.txt`,
`benchmarks/stage26-android-properties.txt`).
The owner's goal is Android on the Mac: install APKs and, later, the Google
Play Store, with **zero VM** (`AGENTS.md`). This page says what of Android
runs today, how to run it, what stops Java on arm64, how Java runs on the
x86_64 build under FEX, and what the next layers need.

Labels: MEASURED, VERIFIED IN SOURCE, UPSTREAM DOCUMENTED, HYPOTHESIS,
UNKNOWN. Everything MEASURED here is on a Mac mini M4, macOS 27.0.

## In one paragraph

Android's native userspace runs under lxrun as ordinary macOS processes:
bionic's `linker64`, `toybox`, `mksh`, `getprop` and ART's `dexdump` from
an unmodified Android 11 arm64 image (MEASURED). ART itself does not start,
and cannot under the current design: ART keeps object references in 32 bits,
so its heap has to be mapped below 4 GiB, and macOS maps nothing below
4 GiB in an arm64 process (MEASURED; VERIFIED IN SOURCE). Every Java process
of Android (zygote, system_server, every app, the Play Store) needs ART.
Binder, the other wall, is down: lxrun now provides `/dev/binder`,
`/dev/hwbinder` and `/dev/vndbinder` in userspace, and Android's own
`servicemanager` runs as the context manager for Android's own `service`
and `dumpsys` clients and for a native service in another process
(MEASURED, "Binder" below). System properties are there too: a property
service built from the image's own files serves `/dev/__properties__` and
`setprop`, so `hwservicemanager` announces itself, `lshal` lists it, and
HIDL HALs of the image register with it (MEASURED, "Properties" below).
The ART heap is what stands between this and an APK.

The x86_64 build of the same Android does get past that wall, zero-VM:
under SteamARM's FEX, whose 64-bit "low window" gives an x86-64 process
guest addresses below 4 GiB, x86-64 ART maps its heap and boot image there
and runs Java, interpreted and JIT-compiled, with the Mac JVM's results
(MEASURED, section "x86_64 Android under FEX"). That makes Java/Kotlin apps
and apps with x86-64 native code possible now; apps whose native code is
arm64-v8a only still need the native arm64 path, because the x86 image has
no arm64 native bridge.

## What runs today (MEASURED)

| program | result |
|---|---|
| `/system/bin/linker64 /system/bin/toybox ls /` | the root's 36 entries |
| `toybox echo`, `uname -a`, `id`, `cat`, `grep -E`, `seq`, `paste`, `date`, `wc` | correct output (`id`: "bad uid 501", the Mac's uid is not an Android one) |
| `toybox sha256sum` / `md5sum` of `framework.jar` (27 MB) | equal to the Mac's `shasum` / `md5`; libcrypto's FIPS self-test passes |
| `/system/bin/sh` (mksh): arithmetic, loops, `$(...)`, pipes | correct, with fork and exec |
| `getprop ro.build.version.sdk`, `getprop` | "30"; the image's 224 properties (its `.prop` files, as init loads them) |
| `setprop` in one process, `getprop` or `__system_property_wait` in another | the new value; the waiter wakes (a futex wake from the property service); `ro.*` written once, types checked, `ctl.*` refused, `persist.*` kept |
| `/apex/com.android.art/bin/dexdump -d hello.dex` | the D8-built classes and their bytecode |
| `servicemanager`, then `service list`, `service check`, `service call`, `dumpsys -l` | the context manager on the userspace `/dev/binder`; "Found 1 services: 0 manager: [android.os.IServiceManager]" |
| a native service (another process) registering with `servicemanager` | listed by `service list` with its interface name, called by `service call` with an int and with a file descriptor, removed when it dies |
| `vndservicemanager /dev/vndbinder`, `vndservice list` | the vendor context, "Found 1 services" |
| `hwservicemanager`, then `lshal list` | context manager on `/dev/hwbinder`, sets `hwservicemanager.ready`; `lshal` lists its 5 interfaces (exit 72: no binder debug logs for its thread/client columns) |
| HIDL HALs: `android.hidl.allocator@1.0-service`, `android.hardware.configstore@1.1-service`, `android.hardware.memtrack@1.0-service` | registered with `hwservicemanager`, listed by `lshal` |
| `dalvikvm64 -cp hello.dex Hello` (and `-Xint`), `dex2oat64` | abort: "Could not find contiguous low-memory space." |

Start-up costs (10 runs, median): `toybox true` 58 ms, `mksh -c true`
41 ms, a `$(toybox echo x)` fork and exec 90 ms, `sha256sum` of 27 MB
74 ms (stage 25). At stage 26 `toybox true` measured 38.6 ms with the
property areas and 38.7 ms without: properties cost nothing at start-up.

## How to run it

```sh
scripts/mkandroidroot.sh          # download, sha256-check, extract (once)
make lxrt                         # or: rm -f build/lxrun && make lxrt LXRT_KEEP_X18=1
LXRT_ROOT=/Volumes/SteamARMAndroid/root LXRT_GUEST_PAGE=4096 \
    build/lxrun /system/bin/toybox uname -a
tests/android/run.sh              # the battery: 29 passed, 1 expected failure
python3 tests/android/logd.py /Volumes/SteamARMAndroid/root   # "logcat", see below
```

- `LXRT_GUEST_PAGE=4096` is required: Android 11's jemalloc refuses a
  16 KiB page ("Unsupported system page size", MEASURED), and bionic's
  linker maps at its compiled-in 4 KiB `PAGE_SIZE` anyway. Almost the whole
  image is 4 KiB-aligned (2,163 of 2,241 aarch64 ELF files), served by
  `runtime/subpage.c`.
- Both lxrun builds work: the default (current SDK) and `LXRT_KEEP_X18=1`
  (12.3 SDK, the kernel keeps x18). The x18 rewriter handles bionic's
  shadow call stack either way (28 sites outside the VNDK v28 prebuilts,
  all rewritten). JIT code that allocates x18 would need the second build
  (`docs/X18_VIRTUALIZATION.md`); ART's JIT reserves x18 (HYPOTHESIS, not
  measured: ART never started).
- Give guests `env -i` plus `init.environ.rc`'s variables
  (`tests/android/run.sh` shows the list): the Mac's environment means
  nothing to Android.
- Android logs go to logd's socket, not to stderr. `tests/android/logd.py`
  binds `<root>/dev/socket/logdw` on the Mac and prints each record; ART's
  abort messages above came from it.
- Properties need nothing: the first bionic process starts the property
  service for its root ("Properties" below). `LXRT_PROPERTY_DIR` picks a
  private property state (the tests use one), `LXRT_PROPERTY_SERVICE=0`
  switches it off.

## The root (for the next agent)

| what | where |
|---|---|
| downloads, read-only (0444) | `~/SteamARM-roots/android/images/`: the two Waydroid zips and the OTA indexes; `extracted/system.img`, `extracted/vendor.img` (raw ext4) |
| the volume | `~/SteamARM-roots/android.sparsebundle`, Case-sensitive APFS, attached at `/Volumes/SteamARMAndroid` (`hdiutil attach -nobrowse -mountpoint /Volumes/SteamARMAndroid ~/SteamARM-roots/android.sparsebundle`; the script does it) |
| the root | `/Volumes/SteamARMAndroid/root`: system.img's tree (system-as-root), vendor.img's under `/vendor`, `/apex/<name>` APFS clones of `/system/apex/<dir>`, `/data` (kept by `--rebuild`) |
| provenance | `/Volumes/SteamARMAndroid/root/.steamarm-androidroot` |
| tools | `~/SteamARM-roots/android/tools/r8-9.4.27.jar` (D8) |

The image: Waydroid LineageOS 18.1 (Android 11, API 30), arm64,
`lineage-18.1-20250628` VANILLA system and MAINLINE vendor, from
Waydroid's official index (`ota.waydro.id`) and its SourceForge host;
sha256 pinned in `scripts/mkandroidroot.sh`. Licences: AOSP/LineageOS,
mostly Apache-2.0, per-component in `NOTICE.xml.gz`. VANILLA has no Google
apps. Nothing from it is committed or bundled.

It is the same Android version and the same LineageOS 18.1 base as
Lepton's legacy `image/` (VERIFIED IN SOURCE, `docs/LEPTON_REUSE_ANALYSIS.md`
4.5), so what is learnt here carries over to a Lepton-derived root.

## What lxrun needed (stage 25)

| change | why | where |
|---|---|---|
| mremap of 4 KiB pages inside 16 KiB host pages | bionic's CFI shadow moves a copy into a 4 KiB slice with `MREMAP_FIXED`; every program CHECK-failed in `~ShadowWrite` | `runtime/dispatch.c` `do_mremap_subpage` |
| `PR_SET_TAGGED_ADDR_CTRL` is EINVAL | bionic tags heap pointers (0xb4...) when the call succeeds; Darwin's `read(2)` returned EFAULT into them and mksh read no script | `runtime/dispatch.c` prctl |
| BoringSSL FIPS module's TPIDR_EL0 reads kept byte-for-byte | its integrity self-test hashes its own code; the rewritten reads changed the hash and every program linking libcrypto aborted | `runtime/tls.c`, `runtime/elfsect.c`, `runtime/rewrite.c` |
| msync | ART probes free memory with it | `runtime/dispatch.c` |
| `rt_tgsigqueueinfo`, `rt_sigqueueinfo` | bionic's `abort()` raises SIGABRT with it | `runtime/dispatch.c` |
| clone without `CLONE_SETTLS` inherits the thread pointer; a thread without `CLONE_FILES` is refused | bionic's crash handler "pseudothread" | `runtime/thread.c` |
| the binder devices, in userspace | every Android daemon and client ("Binder" below) | `runtime/binder.c`, `runtime/binder_hub.c` |
| `getrlimit`, `setrlimit` (163, 164) | bionic's Parcel reads RLIMIT_NOFILE ("Unable to getrlimit: Function not implemented") | `runtime/dispatch.c` |
| `dup3`: O_CLOEXEC applied, other flags EINVAL, a bad oldfd leaves newfd alone | found by the binder review; all guests | `runtime/dispatch.c` |

The FIPS case deserves a note. The module's code is left exactly as the
file has it, and the self-test passes because it hashes the original bytes.
The alternative, recomputing the expected hash after rewriting, would make a
self-test pass on code it exists to reject; SteamARM does not do that. The
price: each of the module's stack-protector reads returns Darwin's
TPIDR_EL0, the load through it faults, and the fault path substitutes the
guest's thread pointer: 557 faults per toybox start, about 11 us each.

## The ART heap wall

MEASURED: `dalvikvm64` (with or without `-Xint`) and `dex2oat64` abort in
6 s with

```
Could not create image space with image file '/apex/com.android.art/javalib/boot.art:...'
  Failed to mmap at expected address, mapped at 0x158000000
Could not find contiguous low-memory space.
heap.cc:493] Check failed: non_moving_space_mem_map.IsValid() Failed anonymous mmap(0x0, 67108864, ...): Out of memory.
```

Why (VERIFIED IN SOURCE, `LineageOS/android_art` lineage-18.1 `7f2b489`,
the ART of this image): a heap reference is `uint32_t`, compressed by
truncating the pointer and decompressed by zero extension
(`runtime/mirror/object_reference.h:39-47,104`); every heap space is mapped
with `low_4gb = true` (`runtime/gc/heap.cc:757-769`), and so is the boot
image, at `ART_BASE_ADDRESS` 0x70000000 plus a delta
(`runtime/gc/space/image_space.cc`).

Why macOS cannot give it (MEASURED, stage 2 and again here with the 12.3
SDK): an arm64 process has a 4 GiB `__PAGEZERO`; `mmap(MAP_FIXED)` and
`mach_vm_allocate(VM_FLAGS_FIXED)` below 4 GiB fail, a smaller
`-pagezero_size` is killed at exec. FEX works around it for 32-bit x86
guests because it translates every guest address; native arm64 code
dereferences the zero-extended reference directly, so there is no place to
add an offset.

What could get past it, zero-VM, in order of promise:

1. **An ART built with the heap above 4 GiB (HYPOTHESIS, the recommended
   next step).** Map every heap space and the boot image in [4 GiB, 8 GiB),
   keep references 32-bit (the low half of the address, as now), and
   decompress with the high bit added back (null stays 0; the page at
   exactly 4 GiB stays unused). ART already has a hook at every reference
   load and store for heap poisoning (VERIFIED IN SOURCE:
   `compiler/utils/arm64/assembler_arm64.cc:161-183`,
   `runtime/arch/arm64/asm_support_arm64.S:73-84`, mterp's
   `UNPOISON_HEAP_REF`), which marks the sites; the change is that the hook
   has to produce a 64-bit pointer (`orr x, x, #1<<32` behind a null check)
   instead of negating a W register. Plus `MemMap`'s low-4 GiB allocator,
   the image's relocation and `PtrCompression`. The boot image and every
   odex must then be recompiled by that ART's `dex2oat` (the AOT code in
   the image decompresses the old way). Source: Apache-2.0; built outside
   the Mac or cross-built, then used as a replacement `com.android.art`.
   Size: a real ART fork; UNKNOWN how many sites bypass the hooks.
2. **Trap and fix every heap access (HYPOTHESIS, not viable).** The
   `runtime/tls.c` trick at the scale of every object access: millions of
   faults per second at about 11 us each, and pointer comparisons against
   space bounds would still be wrong.
3. **An arm64-to-arm64 translator with an address offset (HYPOTHESIS).** A
   FEX-like JIT for arm64 guests. Correct in principle, large, and slow.
4. A VM: excluded by `AGENTS.md`.

Until one of these lands, nothing that runs Java can start in the arm64
root: no zygote, no system_server, no APK, no Play Store. Native daemons and
tools can. The x86_64 root below is the way around it that exists today.

## x86_64 Android under FEX (Java runs)

Stage 25, `benchmarks/stage25-art-x86-fex.txt`. Waydroid's x86_64 build of
the same LineageOS 18.1 (20250628, VANILLA), run by SteamARM's FEX under
lxrun. No VM.

Why it works (MEASURED): FEX translates every guest address. SteamARM's FEX
has a 64-bit low window (guest 0 < x < 4 GiB at host 0x8000000000 + x,
`benchmarks/stage7-guest-base.txt`), normally only for ET_EXEC and wine
processes; `FEX_LOWWINDOW=1` gives it to Android's PIE programs. x86-64 ART
asks for its heap with `MAP_32BIT` (VERIFIED IN SOURCE: ART's low-4 GiB
allocator is for aarch64 only, `libartbase/base/mem_map.h:32-39`), which
FEX's 32-bit allocator serves inside the window. Objects then sit at guest
0x12c03c50, ART's own preferred heap base (`tests/android/java/HeapRef.java`
reads the 32-bit references back). With `FEX_LOWWINDOW=0` the same run fails
exactly like native arm64 ART.

| program (x86-64, under FEX) | result (MEASURED) |
|---|---|
| `toybox ls /`, `uname -m`, `id`, `sha256sum framework.jar` | correct (`x86_64`; `uid=501(system_steamarm)`; the Mac's `shasum`) |
| `sh` (mksh): arithmetic, pipes, `$(toybox ...)` | correct |
| `linkerconfig --target /linkerconfig` (static ET_EXEC) | writes `ld.config.txt` (the legacy layout), then aborts on `VENDOR_VNDK_VERSION` (no property service) |
| `dalvikvm64 -cp hello.dex Hello` | "Hello from Java on ART, no VM", `java.vm.name=Dalvik`, `os.arch=x86_64`, 0.98 s, 222 MB peak RSS |
| `dalvikvm64 -Xint Loop 2000000 2` | the Mac JVM's result, 156 ms/round (HotSpot's interpreter: 46) |
| `dalvikvm64 Loop 20000000 5` (JIT) | the Mac JVM's result, 77 ms/round after warm-up (HotSpot C2: 62.5) |
| a JIT code cache of 128 KiB, collected and reused (`gen_jitchurn.py`) | the Mac JVM's checksum, 1.9 s |
| `dex2oat64 --compiler-filter=speed hello.dex` | an odex in 0.85 s; its AOT code runs, 80 ms/round from the first round |

Start-up of a small program: 0.12 s and 35 MB peak RSS (native arm64
toybox: 0.03-0.06 s, 19 MB). A second FEXServer for this root: 10 MB.

How to run it:

```sh
scripts/mkandroidroot.sh --arch x86_64        # download, sha256, extract, emulator side
make lxrt
scripts/run-android-x86.sh /system/bin/toybox uname -a
scripts/run-android-x86.sh /apex/com.android.art/bin/dalvikvm64 -cp /data/local/tmp/hello.dex Hello
scripts/run-android-x86.sh --server-stop       # its FEXServer (it also leaves after 60 s idle)
ANDROID_ROOT_DIR=/nonexistent tests/android/run.sh   # the x86_64 section only
```

- The root is `/Volumes/SteamARMAndroid/root-x86_64`, next to the arm64 one,
  with the aarch64 side of the emulator in `/usr/lib/lxrt-emu` (FEX-emu,
  FEXServer, their glibc; `--emu` reinstalls it after a FEX rebuild). The
  runtime runs every x86 ELF under `/usr/lib/lxrt-emu/FEX`.
- `scripts/run-android-x86.sh` gives `env -i`, `init.environ.rc`'s variables,
  `FEX_ROOTFS=/`, `FEX_LOWWINDOW=1`, and a FEXServer of the root's own (own
  socket name and lock): a FEX client takes its rootfs from its server, and
  the shared server serves the Ubuntu rootfs. On first use it runs the
  image's own `linkerconfig`.
- The FEX in that root carries three patches this stage added
  (`scripts/build-fex-host.sh` applies them; the shared roots were not
  reinstalled): a free `mmap` hint below 4 GiB is honoured in the window
  (ART's boot image reservation), SMC tracking of a shared view made
  writable while another view is executable (ART's dual-mapped JIT code
  cache: without it, stale code after code cache collections), and a 16 KiB
  interrupt fault page (without it FEX never delivered a signal it had
  deferred: mksh hung in `rt_sigsuspend` after its child's SIGCHLD). The
  last two affect every FEX guest on SteamARM.
- Runtime changes the same work needed: `getrlimit`/`setrlimit` (163/164),
  `/proc/self/stat`'s startstack, `RLIM_INFINITY` in Linux's encoding,
  `madvise` below 4 GiB under a guest base, AF_UNIX datagrams up to Linux's
  size (liblog's records), `getgroups`; `/system/etc/passwd` gets a
  `system_steamarm` entry for the Mac's uid (ART's `System` class needs
  `getpwuid`).

CPU features (MEASURED): FEX reports SSSE3, SSE4.1/4.2, POPCNT, AVX, AVX2,
FMA, BMI1/2, AES, SHA and no AVX-512. linker64, libc, libart,
libart-compiler, libandroid_runtime and boot.oat use SSSE3/SSE4/POPCNT and
no AVX; ART's JIT targets "ssse3,sse4.1,sse4.2,-avx,-avx2,popcnt". AVX code
is in 24 libraries (codecs, libcrypto, libjpeg) and AVX-512 in 10 (libhwui,
the vendor Mesa/LLVM); HYPOTHESIS: behind runtime CPU dispatch, so not taken
with FEX's CPUID, not exercised here.

The trade-off, honestly:

- Possible now, zero-VM: Java/Kotlin apps, and apps that ship x86-64 native
  libraries (of the 10 F-Droid samples of stage 25, every package with
  native code also ships x86_64 except one armeabi-only game). The image's
  1,217 i386 files could run under FEX's 32-bit mode too (UNTESTED).
- Not possible on this path: apps whose native code is arm64-v8a only. The
  x86 image has no native bridge (`ro.dalvik.vm.native.bridge=0`); the
  arm64-to-x86 translators that exist, Intel's Houdini and Google's
  libndk_translation, are proprietary, absent from the image, and SteamARM
  must not bundle them. On an arm64 Mac that would be arm64 code translated
  to x86 and back again anyway.
- Costs: every Android process is a FEX process (~0.1 s and ~15 MB more to
  start), ART's JIT output is translated a second time (1.23x HotSpot C2 on
  the measured loop), the interpreter is 3.4x HotSpot's.
- ARM64-first is still the direction: arm64 apps need the native root and
  the ART heap work above. This path runs Java today and lets binder,
  properties, init and zygote be built against a working ART meanwhile.

Not done here: zygote, system_server, binder, properties, init, graphics,
input, audio. `dalvikvm64` and `dex2oat64` only.

## Binder (stage 25, `benchmarks/stage25-binder.txt`)

### What works (MEASURED)

Android 11's `servicemanager` opens `/dev/binder`, maps its buffer,
becomes the context manager and serves from its Looper (epoll on the
binder descriptor). Against it:

- `service list` prints "Found 1 services: 0 manager:
  [android.os.IServiceManager]"; `service check manager` finds it;
  `service call manager 4 i32 15` returns the list parcel; `service call
  manager 2 s16 manager` returns a strong handle object.
- A native service in a third process (`tests/android/binder_service.c`:
  raw ioctls, no libbinder, built with glibc because there is no NDK here)
  registers as `steamarm.test` with `addService`. `service list` shows
  "steamarm.test: [steamarm.test.IEcho]" (an INTERFACE_TRANSACTION to it);
  `service call steamarm.test 1 i32 41` returns 42; `service call
  steamarm.test 2 fd /system/etc/hosts` hands it a descriptor that it
  reads (56 bytes, the same count and byte sum as the file on the Mac);
  `dumpsys -l` lists both services. When the service exits,
  servicemanager's death notification removes it: the next `service list`
  finds one service.
- Real libbinder daemons serve from libbinder's own thread pool
  (`startThreadPool`, `joinThreadPool`): `idmap2d` registers "idmap" and
  answers `service call idmap 1 s16 /product/overlay/Test.apk i32 0` with
  "/data/resource-cache/product@overlay@Test.apk@idmap"; `credstore`
  registers "android.security.identity" and answers with an AIDL
  service-specific error ("Error creating default store": there is no
  identity-credential HAL).
- `vndservicemanager /dev/vndbinder` with `vndservice list`: a separate
  context with its own context manager.
- `hwservicemanager` becomes the context manager of `/dev/hwbinder`. At
  stage 25 it could not set `hwservicemanager.ready` ("Failed to set ...
  (error 2). HAL services will not start!") and `lshal` spun at 100% CPU
  with no syscalls: libbase's WaitForPropertyCreation loops while the
  property is missing, and bionic's `__system_property_wait` answers
  "true" at once when there are no property areas (VERIFIED IN SOURCE at
  stage 26, `benchmarks/stage26-android-properties.txt`). With the
  property service (stage 26, "Properties" below) it sets the property,
  `lshal` lists its interfaces, and HIDL HALs register with it.
- Costs: a sync round trip between two lxrun processes is 36-38 us to a
  looper parked in BINDER_WRITE_READ (libbinder's thread pool) and 46-55 us
  to a server that polls as servicemanager does (5,000 calls each). The
  same three-process relay over plain Unix sockets costs 8-9 us on this
  Mac; the rest is, as a HYPOTHESIS not yet profiled, the extra system
  calls per hop (header and payload reads, the hub's poll over every
  channel): room to optimise. `service list`
  takes 60 ms, as long as `toybox true` (58 ms), so process start-up, not
  binder, is the cost. servicemanager is the context manager 95 ms after
  launch. The hub process holds about 2 MB.

### How it is built

Guest processes are separate macOS processes, so binder is inter-process
as on Linux, and the driver's state needs a home every process can reach.
It is a host process, the **hub** (`runtime/binder_hub.c`, run as `lxrun
--binder-hub <dir>`), one per user, started by the first guest that opens a
binder device and gone 10 s after the last one closes
(`LXRT_BINDER_HUB_IDLE`). The directory is `/tmp/lxrt-binder-<uid>`
(`LXRT_BINDER_DIR` overrides it; the tests use private ones); it must be
the user's own and closed to others (mode 0700), or no binder device is
offered. It keeps
what the kernel driver keeps, under the kernel's names: procs (one per open
of a device, so a process with `/dev/binder` and `/dev/hwbinder` has two),
threads, nodes, refs and descriptors, transaction stacks, work lists,
death notifications, and each proc's receive buffer with its allocator.
It is single-threaded: each message is processed to completion, which gives
the kernel's locking for free. A thread whose BINDER_WRITE_READ would sleep
is "parked": its answer is not sent until work arrives for it.

The guest side (`runtime/binder.c`):

| guest call | what happens |
|---|---|
| `open` of `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` (and `/dev/binderfs/*`) | a Unix stream connection to the hub and a hello (pid, euid, context). The guest's descriptor is that connection, the process channel, whatever the root has under `/dev` |
| `poll`, `epoll` on it | work for a polling thread makes the hub write one doorbell byte. Every answer says how many it has written and whether one should stay unread, and the runtime reads up to that count, so readability follows `binder_poll` instead of drifting (0 wakeups with nothing to read in about 20,000, MEASURED). `epoll_ctl` and `ppoll` mark the calling thread as a poller (`LOOPER_STATE_POLL`) |
| `ioctl(BINDER_WRITE_READ)` | on the calling thread's own channel (a socketpair per guest thread and open, handed to the hub): the write bytes plus, per BC_TRANSACTION(_SG) or BC_REPLY(_SG), what the kernel would copy from the sender (data, offsets, scatter-gather buffers) and the descriptors named by FD and FDA objects (SCM_RIGHTS). The answer carries the BR_* bytes for the read buffer, the descriptors to install, where in the receive buffer their numbers go, and FDA descriptors to close |
| other ioctls | BINDER_VERSION (8), SET_MAX_THREADS, SET_CONTEXT_MGR(_EXT), THREAD_EXIT, GET_NODE_DEBUG_INFO and GET_NODE_INFO_FOR_REF go to the hub; anything else (BINDER_FREEZE, ...) is EINVAL, as on a kernel without it |
| `mmap` | the receive buffer: a file mapped read-only for the guest (PROT_WRITE is EPERM and a second mapping EBUSY, as on Linux) and read-write in the hub, which copies each incoming transaction into it and hands out offsets, as the kernel does. The runtime keeps a read-write view only to write the numbers of the descriptors it installed (Linux's fd fixups) |
| `munmap` of it | binder_vma_close: the hub allocates nothing more there, and a transaction to the process gets BR_DEAD_REPLY |
| `read`, `write` | EINVAL: binder has neither |
| `access`, `stat`, `fstat` | the devices exist, as character devices. libbinder's `initWithDriver` falls back to `/dev/binder` when `access()` fails, which would put vndservicemanager on the wrong context (MEASURED: "Binder driver /dev/binder is unavailable. Using /dev/binder instead.") |
| a signal while blocked | the hub is told (BH_CANCEL) and answers -EINTR unless it already had the real answer; libbinder retries, and SA_RESTART restarts the ioctl with the consumed counts, as Linux's restart does |
| `fork` | the child's binder is refused (EINVAL) and has no buffer (VM_INHERIT_NONE, Linux's VM_DONTCOPY); the parent's thread channels are closed in the child |
| thread exit, `close` | the thread's channels close and the hub releases its binder threads. Once the last descriptor and the mapping are gone, the proc is released (binder_release): its nodes die and death notifications go out |

Ported from the semantics of `drivers/android/binder.c` (Linux 5.x),
checked afterwards against Linux v5.10's source function by function
(VERIFIED IN SOURCE, the list is in the stage record), and tested across
processes by `tests/elf/binder_ipc.c`, with raw ioctls against the Linux
UAPI header, so the numbers are the kernel's and not a copy of the
runtime's:

- the node reference protocol (BR_INCREFS, BR_ACQUIRE, BR_RELEASE,
  BR_DECREFS and their DONEs, strong use only through strong refs);
- handle 0 and the context manager;
- the deferred TRANSACTION_COMPLETE of sync calls;
- the transaction stack: a call back into a thread that is waiting on you
  goes to that thread;
- oneway calls delivered one at a time per node, until the previous buffer
  is freed;
- BR_SPAWN_LOOPER and BC_REGISTER_LOOPER, two loopers serving at once;
- FD, FDA and PTR objects with parent fixups and their validation, and FDA
  descriptors closed when the buffer is freed;
- return errors and reply errors, BR_DEAD_REPLY for a dead target or an
  unmapped buffer, BR_FAILED_REPLY for a bad handle;
- TF_CLEAR_BUF: the buffer is zeroed when freed;
- death notifications: request, clear and BC_DEAD_BINDER_DONE.

Deviations, stated rather than hidden:

- No priorities or scheduling policy: a node's min_priority is kept, not
  applied.
- No SELinux. A node that asks for the sender's security context
  (FLAT_BINDER_FLAG_TXN_SECURITY_CTX; servicemanager does) receives the
  label the sender's runtime reports: `LXRT_BINDER_SECCTX`, default
  `u:r:unlabeled:s0`. This image's servicemanager has its SELinux checks
  compiled out (it imports no `getcon` or `selinux_check_access`,
  MEASURED with `strings`), like Lepton's (`docs/LEPTON_REUSE_ANALYSIS.md`
  3.6).
- The sender's euid is the Mac's uid (501) for every process: Android has
  no per-app uids here yet. Lepton sends a uid in the transaction flags
  for the same reason (3.6).
- Poll readiness is kept for the thread that registered the descriptor
  (`epoll_ctl`, `ppoll`); Linux evaluates it for the thread that waits.
  In libbinder's pattern the two are the same thread (`setupPolling`, then
  `Looper::addFd`, then `pollOnce`).
- A binder descriptor does not survive `execve` (Linux's cannot be mapped
  again after exec either).
- A signal handler that calls binder while its own thread is inside a
  binder call gets EAGAIN instead of a nested call.
- Starting the hub makes a short-lived child of the guest process
  (`posix_spawn`; it forks the real hub and exits), so a guest with a
  SIGCHLD handler sees one SIGCHLD it did not cause.
- Everything goes through sockets: 46-55 us per sync round trip, where
  the kernel driver is usually quoted in the tens of microseconds on
  phones (not measured here).

What servicemanager needed from lxrun besides binder, both MEASURED:
`getrlimit` and `setrlimit` (bionic's Parcel reads RLIMIT_NOFILE, and
logged "Unable to getrlimit: Function not implemented" before) and
`access()` of the device. LineageOS 18.1's libbinder writes Android 12's
'SYST' header into every interface token (MEASURED: "Expecting header
0x53595354" when it is missing), which matters to anything that builds
parcels by hand. Its dumpsys prints the service list only when there is
more than one service (VERIFIED IN SOURCE, `cmds/dumpsys/dumpsys.cpp`,
`if (N > 1)`): an empty `dumpsys -l` with servicemanager alone is right.

### How to run it

```sh
tests/elf/run.sh                  # BINDER_IPC and BINDER_POOL (needs the Fedora sysroot)
tests/android/run.sh              # servicemanager, service, dumpsys, vndservicemanager, hwservicemanager
LXRT_BINDER_LOG=1 ...             # the hub logs every transaction to <dir>/hub.log
```

## Properties (stage 26, `benchmarks/stage26-android-properties.txt`)

### What works (MEASURED)

- Every bionic process of the root finds `/dev/__properties__`:
  `property_info` and one 128 KiB area per SELinux context (160 in this
  image) plus `properties_serial`, in bionic's own formats, built from the
  image's `property_contexts` (plat, system_ext, vendor) and `.prop` files
  in init's order. `getprop` lists 224 properties; every `ro.build.*` value
  of `/system/build.prop` is there. `property_info` is byte-identical to
  what AOSP's own serializer (built on the Mac from system_core) makes of
  the same files, and 2,854 lookups agree.
- `ro.build.fingerprint` and `ro.product.{brand,device,manufacturer,model,
  name}` are derived from the image's own values exactly as init derives
  them (this image sets none). Nothing certification-related is invented
  or changed; the build is what it says: userdebug, test-keys.
- `setprop` in one guest, `getprop` in another. `ro.*` is written once:
  `setprop ro.build.version.sdk 99` fails (0xb, READ_ONLY) and it stays
  30. Types from `property_contexts` are checked (`service.adb.tcp.port`
  is an int: "abc" refused). `ctl.*` is refused and logged: there is no
  init to start or stop anything. `persist.*` goes to init's protobuf file
  and comes back on a fresh boot.
- `__system_property_wait` across processes: a bionic program linked
  against the image's libc (`tests/android/props_wait.c`) wakes when
  another guest creates, then changes, the property. 1000 of 1000 changes
  seen; the waiter wakes 18.6 us (median) after the setter sends, back to
  back, 100 us with 5 ms between sets.
- `hwservicemanager` sets `hwservicemanager.ready`; `lshal list` shows its
  5 interfaces; `android.hidl.allocator@1.0-service`,
  `android.hardware.configstore@1.1-service` and
  `android.hardware.memtrack@1.0-service` register with it. `lshal` exits
  72 because its thread and client columns read
  `/dev/binderfs/binder_logs/proc/<pid>`, the binder driver's debug state,
  which the userspace driver does not publish.
- Costs: none at start-up (`toybox true` 38.6 ms with, 38.7 ms without);
  `getprop` of one value 28 ms, `setprop` 28.5 ms (process start-up); the
  service boots in 9-13 ms and adopts existing areas in 7 ms.

### How it is built

Android's property state lives in memory that init writes and every
process maps read-only; here the writer is a host process, the **property
service** (`runtime/propsvc.c`, `lxrun --property-service <dir> <root>`),
one per Android root, and the guest side is `runtime/props.c`.

| piece | what | where |
|---|---|---|
| directory | `LXRT_PROPERTY_DIR`, else `/tmp/lxrt-props-<uid>-<hash of the root's real path>`; the user's own, mode 0700, or no property service is offered | `runtime/props.c` |
| guest paths | `/dev/__properties__[/...]` and `/dev/socket/property_service` lead into it; everything else under `/dev/socket` stays in the root | `runtime/dispatch.c` `translate_one` |
| start | the first process that finds no `property_info` starts the service (its first process boots, binds the socket, forks the service and exits, so the areas exist when the spawner's `waitpid` returns); a `connect()` to the socket that finds nobody listening starts it and retries once | `runtime/props.c`, `runtime/socket.c` |
| ownership | bionic maps an area only if `fstat` says uid 0, gid 0 (`prop_area.cpp` `map_fd_ro`); `fstat`, `stat` and `statx` of the directory and its files answer that | `runtime/props.c` `lxrt_props_fix_stat` |
| boot | property_info (libpropertyinfoserializer's format), the areas, then init's values: `LXRT_PROPERTY_BOOTARGS` as the kernel command line (`androidboot.x=y`, empty by default), `ExportKernelBootProps`' defaults, the `.prop` files, the derived properties, `ro.property_service.version=2`, `/data/local.prop` (debuggable build), the persistent ones, `ro.persistent_properties.ready`; built in `__properties__.new` and renamed into place | `runtime/propsvc.c` |
| writes | the service's own `MAP_SHARED` mapping of the files; bionic's `Update` step for step (dirty bit, backup area, release fences, serial), then `__ulock_wake(UL_COMPARE_AND_WAIT_SHARED)` on the property's serial and on the global one: the queue a guest's `FUTEX_WAIT` in a shared mapping parks on (`runtime/futex_ops.c`); two unrelated processes meet there (MEASURED) | `runtime/propsvc.c` |
| setprop | `PROP_MSG_SETPROP2` and the old `PROP_MSG_SETPROP`; legal name and value, the type check, `ro.*` once, `persist.*` stored, `ctl.*` refused; the peer must be this user | `runtime/propsvc.c` |
| persistent file | `LXRT_PROPERTY_PERSIST`, else the root's `/data/property/persistent_properties` (init's path and protobuf format) | `runtime/propsvc.c` |
| lifetime | leaves after `LXRT_PROPERTY_IDLE` seconds without a setprop (default 60, 0 never) or when its directory goes; the areas stay readable, and a service started again adopts them. Removing the directory resets ("reboots") the root's properties | `runtime/propsvc.c` |

Deviations, stated rather than hidden:

- No SELinux: init's MAC checks (`CheckMacPerms`, `CanReadProperty`) are
  not applied, and every area is readable by every guest.
- No init: `ctl.*` is refused (`PROP_ERROR_HANDLE_CONTROL_MESSAGE`), no
  "on property:" trigger runs, `sys.powerctl` is only logged,
  `selinux.restorecon_recursive` is set at once (there are no labels to
  restore), no `vendor_load_properties` hook.
- Every guest is the Mac user, as for binder: `ro.*` is write-once by the
  protocol, not by permissions (a guest could `chmod` an area file).
- The property state outlives the processes that used it (it is files):
  a later run of the same root sees the earlier run's non-persistent
  values until the directory is removed. On Android a reboot clears them.
- `ro.hardware` is "unknown" (init's default with no
  `androidboot.hardware`); libhardware still finds the image's
  `*.waydroid.so` modules through `ro.board.platform`.

### How to run it

```sh
tests/android/run.sh                                   # properties, setprop, wait, hwservicemanager, lshal, a HAL
LXRT_PROPERTY_DIR=/tmp/my-props ... build/lxrun ...    # a private property state (mode 0700)
cat /tmp/lxrt-props-<uid>-<hash>/service.log           # every set, refusal and start
LXRT_PROPERTY_SERVICE=0 ...                            # no properties (stage 25 behaviour)
```

### Around binder and properties, before anything Java could run even with ART fixed

- **init.** Lepton runs Android's own init in a container with SELinux,
  ueventd and device-mapper removed (`docs/LEPTON_REUSE_ANALYSIS.md` 3.7);
  here init would have to run as an ordinary process with no mount or pid
  namespace: a plan interpreter in the spirit of `runtime/mounts.c`, or a
  hand-written start order (logd, servicemanager, hwservicemanager,
  vndservicemanager, the HALs, then zygote). The property service has the
  state init's `.rc` files act on, but no trigger hook yet ("on
  property:...", `onrestart setprop hwservicemanager.ready false`). This is
  now the first wall for native Android services.
- **Shared memory across processes.** Parcels carry ashmem or memfd
  descriptors; binder passes them (FD objects), but lxrun's memfd seals
  hold only inside one process (`docs/LEPTON_REUSE_ANALYSIS.md` 7). HALs
  hand such buffers to their clients (the ashmem allocator above
  registers, but nothing has allocated through it yet). libcutils picks
  ashmem or memfd from `ro.vndk.version`, which is now set (30); which path
  it takes here is HYPOTHESIS until measured.
- **logd.** `tests/android/logd.py` stands in for its write socket; the
  real logd needs its sockets and `/proc/kmsg` (optional).
- **Linker namespaces.** `linkerconfig` is ET_EXEC and cannot run under
  lxrun (links inside `__PAGEZERO`); the linker then uses its default
  namespace (MEASURED enough for the programs above). ART and apps want
  `/linkerconfig/ld.config.txt`: generate it on the host (the tool is open
  source) or write one. The x86_64 image's linkerconfig runs under FEX and
  writes the main file before it stops on the missing properties (MEASURED;
  `libandroid_runtime.so` needs it to find `libstatssocket.so`).
- **Graphics, input, audio**: `docs/LEPTON_REUSE_ANALYSIS.md` 4.

## APKs and the Google Play Store

- An APK is a zip: its dex runs on ART (blocked above in the arm64 root;
  running in the x86_64 root under FEX) and its `lib/<abi>/*.so` load into
  an ART process of that ABI. `lib/arm64-v8a` needs the arm64 root;
  `lib/x86_64` (and, untested, `lib/x86`) runs in the x86_64 root;
  `armeabi-v7a`-only apps cannot run on Apple silicon (no AArch32; 1,684
  32-bit arm files in the arm64 image are unusable for the same reason) and
  have no translator in the x86 image either.
- Google Play Store and Google Play services are proprietary. SteamARM will
  never commit or bundle them; if it ever fetches them, it does so on the
  owner's Mac from Google's official source, and only if that is legal. The
  Play Store also expects a device registered as certified and Play
  Integrity verdicts; SteamARM must not falsify certification or bypass
  Play Integrity or SafetyNet, so apps that require them will refuse to run
  (UNKNOWN which ones). None of this is reachable before the ART heap wall
  and binder.

## Order of work

1. The ART heap (above): without it, nothing Java runs in the arm64 root.
   Start with a feasibility count of the reference sites outside the
   poisoning hooks in ART's source. In the x86_64 root ART already runs Java
   under FEX (stage 25, above).
2. Binder: done in userspace (stage 25, above). Next on it: ashmem/memfd
   sharing across processes, the binder debug logs `lshal` reads, the cost
   per call, and x86-64 guests reaching it through FEX (UNTESTED).
3. Properties: done (stage 26, above). The next wall for native Android
   services is init: a start order for the daemons and HALs, restarts, and
   the property triggers of the `.rc` files, on top of this property
   service (also what linkerconfig's `VENDOR_VNDK_VERSION` abort asks for);
   then shared memory between a HAL and its clients.
4. Then zygote, system_server, `pm install` and a window (Lepton's graphics
   analysis, 4): first in the x86_64 root for Java and x86-64 apps, then
   with the rebuilt ART for arm64-v8a APKs.

## Tests and records

- `tests/android/run.sh`: the programs above against the arm64 root (ART
  an expected failure with the heap reason); binder with servicemanager,
  `service`, `dumpsys`, a native service and vndservicemanager; properties
  (getprop against the image's files, setprop across processes, `ro.*`,
  types, `ctl.*`, `persist.*` across a fresh boot,
  `__system_property_wait` in a program linked against the image's libc
  (`tests/android/props_wait.c`), the service leaving when idle and
  adopting its areas again), hwservicemanager with `lshal` and a HIDL HAL;
  then an x86_64 section against `root-x86_64` under FEX: bionic programs,
  two freestanding x86-64 probes (`x86_lowwin.c`: `MAP_32BIT`,
  `MADV_DONTNEED` zeroing, low hints, startstack, `RLIM_INFINITY`;
  `x86_dualview.c`: code rewritten through a dual-mapped memfd), and
  `dalvikvm64` against the Mac's JVM. Each section skips without its root.
- `tests/elf/run.sh` BINDER_IPC and BINDER_POOL (`tests/elf/binder_ipc.c`):
  the binder driver between lxrun processes with raw ioctls.
- `tests/elf/run.sh` ANDROID_BIONIC_RT and "kept TLS reads"
  (`tests/elf/android_bionic_rt.c`): the runtime changes, with no Android
  root needed.
- `benchmarks/stage25-android-userspace.txt`, `benchmarks/stage25-binder.txt`,
  `benchmarks/stage25-art-x86-fex.txt` and
  `benchmarks/stage26-android-properties.txt`: every run, before and after.
