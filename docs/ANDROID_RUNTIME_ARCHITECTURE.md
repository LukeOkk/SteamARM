# Android userspace on lxrun, with no VM

Status 2026-09-29, stage 27 (`benchmarks/stage25-android-userspace.txt`,
`benchmarks/stage25-binder.txt`, `benchmarks/stage25-art-x86-fex.txt`,
`benchmarks/stage26-android-properties.txt`,
`benchmarks/stage27-android-framework.txt`,
`benchmarks/stage27-android-display.txt`).
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

In that x86_64 root the framework now starts (stage 27, "Boot" below):
binder and properties work for x86-64 guests through FEX unchanged, a
minimal init (`scripts/android-boot.py`) runs the image's own .rc files for
a headless profile of 23 services, zygote64 preloads its 12,100 classes and
forks system_server, which runs as uid 1000 under its seccomp policy and
starts its bootstrap services up to LightsService. There it waits for
SurfaceFlinger, which waits for a display composer; the image's only one
is Waydroid's Wayland client, and there is no display here, so the
framework's Watchdog restarts system_server every 96 s and
PackageManagerService (after the display) never starts (MEASURED).
Android also has a screen now: in the x86_64 root, Android's own
SurfaceFlinger renders the LineageOS boot animation with SwiftShader and
presents it through Waydroid's hwcomposer to Weston running under lxrun,
whose X11 window on SteamARM's X server is a macOS window, at 57 frames/s
(MEASURED, section "Display"). Stage 28 removed the intermittent SIGCHLD
hang of x86-64 shells under FEX (0 hangs in 720 runs; two causes, one in
the runtime, one in FEX), made the image's i386 programs run under FEX's
32-bit mode (zygote_secondary, dex2oat32, dalvikvm32, the 32-bit audio
HAL; 32-bit bionic's 16-bit thread ids are still a limit), and gave each
Android stack its own `/dev/input` for the composer's input FIFOs (section
"Reliability and input").

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

Stage 25 ran `dalvikvm64` and `dex2oat64` only; stage 27 added binder,
properties, an init, zygote64 and system_server (below). Graphics, input
and audio are not done.
Not done here: zygote, system_server, binder, properties, init, graphics,
input, audio. `dalvikvm64` and `dex2oat64` only. (Since then: binder,
properties and the display stack run in this root too, section "Display".)

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
  for the same reason (3.6). A process started with `LXRT_BINDER_UID=<n>`
  presents uid n instead, as init's `user` line would give it (stage 27:
  SurfaceFlinger as 1000, bootanimation as 1003; section "Display").
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
  ueventd and device-mapper removed (`docs/LEPTON_REUSE_ANALYSIS.md` 3.7).
  Here it is `scripts/android-boot.py` (stage 27, "Boot" above): the
  image's .rc files, init's triggers and restart rules, a profile of
  services, and the property service's `LXRT_PROPERTY_INIT` hook for
  ctl.* and "on property:" triggers.
- **Shared memory across processes.** Parcels carry ashmem or memfd
  descriptors; binder passes them (FD objects), but lxrun's memfd seals
  hold only inside one process (`docs/LEPTON_REUSE_ANALYSIS.md` 7). Stage
  27: the graphics allocator's buffers reach SurfaceFlinger and the
  composer through hwbinder and are mapped by all three, with
  `sys.use_memfd=true` (there is no `/dev/ashmem`, so the frames that
  arrive can only have come through memfd; not traced per call).
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

## Identities (stage 27, `runtime/android_ids.c`)

Android's init starts each daemon as root and drops it to the user, groups
and capabilities its .rc file names; the zygote does the same for
system_server and every app, and treats each failure as fatal. A Mac
process cannot become another user, so the runtime keeps per-process
virtual credentials for Android guests, switched on by `LXRT_ANDROID_IDS`
(`ruid[,euid,suid]:rgid[,egid,sgid]:groups:eff,prm,inh,bnd,amb[:knb<hex>]`, or
`root`), which the boot script sets per service. Lepton gets the same with
a seccomp profile that answers success and a bionic fake-uid patch
(`docs/LEPTON_REUSE_ANALYSIS.md` 3.3).

| guest call | with Android ids (Linux's rules, MEASURED by tests/elf ANDROID_IDS) |
|---|---|
| get/set[res]uid, get/set[res]gid, setfs*, get/setgroups | the virtual ids; CAP_SETUID/CAP_SETGID decide; saved ids as kernel/sys.c moves them; groups kept sorted |
| capget, capset, prctl capability options | kernel/capability.c's subset rules; keepcaps and the effective-set fixups on a uid change; bounding set 0..40 then EINVAL; ambient; securebits; no_new_privs |
| execve, fork | the next image gets them, recomputed as for a file without capabilities (root: bounding and inheritable; others: the ambient set); the bind table goes along |
| chown, fchown, fchownat | a change Linux would allow reports success; the file stays the Mac user's |
| unshare, mount, umount2, setns | namespaces and propagation changes succeed for CAP_SYS_ADMIN (one view); `MS_BIND` is an entry in the per-process bind table (`runtime/mounts.c`); other mounts EPERM |
| SO_PEERCRED, SCM_CREDENTIALS, capget of a pid, binder sender euid | another process's virtual ids (effective for SO_PEERCRED, real for SCM_CREDENTIALS) and capabilities, from `/tmp/lxrt-shm-<uid>/android-ids.v2` (pid, checked against its start time) |

Without the variable nothing changes. What the kernel would enforce is not
enforced: files are all the Mac user's (stat shows 501) and any process can
signal any other.

## Boot: init, zygote64, system_server (stage 27, `benchmarks/stage27-android-framework.txt`)

### What runs (MEASURED, x86_64 root under FEX)

| piece | result |
|---|---|
| binder from x86-64 guests | servicemanager, service list/call, dumpsys, vndservicemanager, hwservicemanager, lshal, an x86-64 HIDL HAL, and an x86-64 native service (linked against the image's bionic) that receives a descriptor and whose death is noticed: no change to the driver was needed |
| properties from x86-64 guests | getprop, setprop, `__system_property_wait` (FUTEX_WAIT through FEX, woken by the service) |
| `scripts/android-boot.py` | parses 93 .rc files (91 services, 184 actions), runs init's boot and property triggers in 1.8-2.1 s, starts 23 services with their .rc command lines, sockets, environment and Android ids, restarts them as init does |
| HALs and daemons | suspend, configstore, memtrack, ashmem allocator, keymaster, graphics allocator, light, gatekeeper, health, vibrator, power register with hwservicemanager; keystore, gatekeeperd, installd, credstore, idmap2d, audioserver run; SurfaceFlinger renders with SwiftShader (GLES 3.0) |
| zygote64 | preloads 12,100 classes in 1.1-1.9 s and listens on its init socket |
| system_server | forked and specialized (uid 1000, capabilities, Android's x86_64 seccomp policy under FEX's emulation, `/mnt/user/0` bound on `/storage`); its classpath compiled by installd with dex2oat64; "Entered the Android system server!" 30 s after the boot starts (20 s of it waiting for the secondary zygote, which is i386); bootstrap services up to LightsService |
| the wall | LightsService -> SurfaceControl -> SurfaceFlinger -> IComposer: `hwcomposer.waydroid` "Couldn't open Wayland display."; the Watchdog kills system_server after 60 s. With a headless weston (exploratory) the composer connects and then waits on a futex; IComposer is never registered |

### What it took

- Runtime: Android ids (above); an AF_UNIX name read within the length the
  kernel returned (a host-bound init socket came back with stack bytes
  after its name and the zygote refused to fork); `/proc/self/fd` without
  the runtime's descriptors (eventfd pipe ends) and FEX's (opened before
  the guest's first instruction), since the zygote aborts on any
  descriptor it does not know; `/proc/self/attr/*` and `/proc/thread-self`
  (keystore aborts without a context); SO_PEERCRED that no longer reports
  a stale sender; the property service handing ctl.* and property changes
  to an init (`LXRT_PROPERTY_INIT`).
- FEX: `patches/fex-lxrt-seccomp-cap-sys-admin.patch` (a filter may be
  installed with no_new_privs OR CAP_SYS_ADMIN, as on Linux), and
  `FEX_NEEDSSECCOMP=1` for the zygote and its children: without seccomp
  emulation FEX answers EINVAL and the zygote child aborts.
- The boot script: init's `setpgid` instead of a new session (the zygote's
  first call is `setpgid(0, 0)`), `/dev/null` stdio for the zygote, init's
  mkdirs under `/data`, `/mnt` and `/storage`, Waydroid's `mount_all`
  (`ro.crypto.state=unencrypted`), and host properties as Waydroid's and
  Lepton's hosts set them: `bpf.progs_loaded=1`, `ro.cold_boot_done=true`,
  `sys.use_memfd=true`, `ro.hardware.egl=swiftshader`,
  `ro.hardware.gralloc=default`, `dalvik.vm.dex2oat64.enabled=true`.

### Left out, and why (MEASURED unless marked)

netd exits at once (no `NETLINK_KOBJECT_UEVENT`, route netlink, iptables
or BPF) and its `onrestart restart zygote` would kill the zygote every 5 s;
the audio HAL, dex2oat32 and zygote_secondary are i386 and died in FEX's
32-bit mode ("NoExec instruction in entry block"; a JIT fault at address 0)
-- audioserver waited for the audio HAL forever (stage 28 runs all three:
"Reliability and input" below); statsd cannot link (the
root's `/linkerconfig` was written before there were properties and has
no namespace for its APEX); ueventd, logd (`tests/android/logd.py` stands
in), lmkd, vold, apexd (flattened APEXes), tombstoned, bootanim and the
camera, media, drm, wifi and tracing daemons are not started.
## Display (stage 27, `benchmarks/stage27-android-display.txt`)

### What works (MEASURED)

Android's own SurfaceFlinger presents its boot animation in a macOS window,
with no VM. The chain, every piece an ordinary macOS process:

```
bootanimation (x86-64, FEX)       draws the LineageOS animation with GLES
  | BufferQueue over binder        (SwiftShader: GLES 3.0 on the CPU)
SurfaceFlinger (x86-64, FEX)      composites the layers, GLES again
  | HWC2 over hwbinder, gralloc    (composer@2.1 + HWC2On1Adapter)
  | buffers (memfd) passed as fds
hwcomposer.waydroid (x86-64, FEX) copies the frame into a memfd wl_shm
  | Wayland over a Unix socket,    buffer (R and B swapped), commits it
  | the memfd passed by SCM_RIGHTS
Weston 14 (aarch64, native lxrun)  composites with pixman, X11 backend
  | X11 + MIT-SHM
XQuartz :2 (SteamARM-X11.app)     a rootless X server: a macOS window
```

| step | result (MEASURED) |
|---|---|
| Weston under lxrun (Fedora 43's 14.0.2, `scripts/mkwestonroot.sh`), X11 backend on :2, pixman renderer, desktop shell | an 800x600 X window "Weston Compositor - screen0" on :2: a macOS window with Weston's panel and background; `wayland-info` lists 20 globals (16 headless with the kiosk shell) |
| `weston-simple-shm` | its rings in the window (`xwd` of the window, not a screen capture), 56.2 frame callbacks/s; Weston 1.8% of a core, XQuartz 20% more than idle |
| `tests/android/wl_shm_client.c`, aarch64 | 57.4 frames/s, 300 frames |
| the same client built for x86-64, run under FEX from the x86_64 Android root | 57.5 frames/s into the aarch64 Weston; `xwd` of the window: 37,440 + 37,440 + 38,400 + 38,411 pixels of its four colours, exactly the client's 480x320 buffer less the moving band. An FEX guest and a native guest share buffers through a memfd passed over the socket |
| `servicemanager`, `service list` of the x86_64 image, under FEX | "Found 1 services": binder reaches x86-64 guests through FEX |
| hwservicemanager, allocator@2.0, vendor.waydroid.task@1.0, configstore@1.1 (x86_64, FEX) | registered; `lshal` lists them |
| composer@2.1 with hwcomposer.waydroid | connects to Weston through `waydroid.xdg_runtime_dir`/`waydroid.wayland_display`, creates its "Waydroid" window, registers IComposer and Waydroid's display, window and clipboard HALs; its EGL worker initialises SwiftShader |
| SurfaceFlinger (x86_64, FEX) | RenderEngine on "Google SwiftShader, OpenGL ES 3.0 SwiftShader 4.1.0.7", display 0 800x568 (Weston's maximised window), 48 shaders in 128 ms, registers "SurfaceFlinger" |
| bootanimation (x86_64, FEX) | "Enter boot animation"; the LineageOS animation (teal arc and circle) in the macOS window, frames changing between screenshots; SurfaceFlinger 57 page flips/s, hwcomposer 56.8 buffer commits/s |
| start-up | composer ready 0.5 s after launch, SurfaceFlinger 0.9 s after that, the boot animation layer 2.4 s after that; `scripts/run-android-display.sh start` (binder, properties, the HALs, SurfaceFlinger) 4.9 s |
| cost, boot animation running | SurfaceFlinger 40-48% of one core, bootanimation 35-38%, composer 4-5%, Weston 4-5%, XQuartz 23-30%, the HALs and managers ~0% |

### What it needed

| change | why | where |
|---|---|---|
| mremap of a MAP_SHARED file mapping maps more of the file | every libwayland-server compositor grows a client's wl_shm pool with `mremap(MREMAP_MAYMOVE)`; lxrun refused, and Weston quit at once: "wl_shm_pool#3: error 2: failed mremap" for its own desktop shell | `runtime/mremap.c` (a Mach fileport of each shared file mapping, `remap_shared_file`), `runtime/dispatch.c`; `tests/elf/shm_mremap.c` |
| the syscall trampoline keeps v0-v31, FPSR and NZCV | Linux preserves them across `svc`; the dispatcher (Darwin C) clobbers them. A raw client kept its msghdr template in q2 across syscalls and sent `iovlen` 0 (EMSGSIZE) after a few calls; a counted `subs; svc; b.ne` loop exits early. Any inline-syscall code (glibc's INTERNAL_SYSCALL) is exposed. 18.8 -> 23.5 ns per getpid | `runtime/trampoline.S`; `tests/elf/simd_syscall.c` |
| `LXRT_BINDER_UID` | every guest is the Mac user (501); SurfaceFlinger admits only AID_GRAPHICS/AID_SYSTEM without system_server's permission service, and bootanimation waited forever ("Waiting to check permission android.permission.ACCESS_SURFACE_FLINGER from uid=501"). The uid binder peers see, as init's `user` line gives it (surfaceflinger 1000, bootanimation 1003); grants nothing a guest of the same Mac user could not already claim | `runtime/binder.c`; `tests/android/run.sh` |
| the socket in `/dev/shm` | the runtime maps `/dev/shm` to one host directory per user for every root, so one `XDG_RUNTIME_DIR` works from the Fedora root, the Android root and FEX | `scripts/run-weston.sh` |
| Waydroid's host properties | what Waydroid's container manager writes into `/vendor/waydroid.prop` (here a dummy): `ro.hardware.gralloc=default`, `ro.hardware.egl=swiftshader`, `waydroid.xdg_runtime_dir`, `waydroid.wayland_display`, `waydroid.active_apps=Waydroid`, plus `sys.use_memfd=true` (there is no `/dev/ashmem`); set with the image's `setprop` | `scripts/run-android-display.sh` |

### How hwcomposer.waydroid connects (VERIFIED IN SOURCE)

Waydroid's `android_hardware_waydroid` `3b2f29d` (branch lineage-18.1, the
last hwcomposer commit before the image's 2025-06-28 build) and Lepton's pin
`896f652` (`gitlab.steamos.cloud/frame-public/android_hardware_waydroid`)
connect the same way:

- `hwc_open` sets `XDG_RUNTIME_DIR` from `waydroid.xdg_runtime_dir`
  (default `/run/user/1000`) and `WAYLAND_DISPLAY` from
  `waydroid.wayland_display` (default `wayland-0`), then
  `wl_display_connect(NULL)` (`hwcomposer/hwcomposer.cpp:1260-1268`,
  `wayland-hwc.cpp:1953`; Lepton's `hwcomposer.cpp:755-759`). Lepton points
  them at gamescope's socket (`docs/LEPTON_REUSE_ANALYSIS.md` 4.2).
- Globals it binds (`wayland-hwc.cpp:1817-1899`): wl_compositor (<= 5),
  wl_subcompositor, xdg_wm_base (v1), wl_shell, wl_seat, wl_shm, wl_output,
  wp_presentation, wp_viewporter; android_wlegl (gralloc "android") or
  zwp_linux_dmabuf_v1 v3 (gbm, minigbm); optional zwp_tablet_manager_v2,
  pointer constraints, relative pointer, idle inhibit, fractional scale,
  wl_data_device_manager, gtk_shell1 (Lepton adds mir_shell_v1). Nothing
  Waydroid-specific on the Wayland side: its own protocol file
  (`wayland-android.xml`, android_wlegl) is only for gralloc "android".
  Weston offers every one it needs (not dmabuf with the pixman renderer,
  which gralloc "default" does not use).
- Gralloc "default": the composer locks each layer's buffer for CPU reads
  and copies it, swapping red and blue, into a `wl_shm` buffer in a memfd
  of its own (`hwcomposer.cpp:154-183, 187-262`,
  `wayland-hwc.cpp:260-293`). One copy per frame in the composer.
- `create_display` ends with `IWaydroidTask::getService()`
  (`wayland-hwc.cpp:1971`): the task HAL is declared in the vendor VINTF
  manifest, so libhidl waits for it; the image's
  `/system/bin/hw/vendor.waydroid.task@1.0-service` provides it.
- `waydroid.active_apps=Waydroid` shows the whole Android screen in one
  window (`hwcomposer.cpp:494-540`); per-app windows are its multi-window
  mode (`persist.waydroid.multi_windows`), not tried.
- There is no sw_sync device here, so `sw_sync_timeline_create()`
  (`hwcomposer.cpp:1246`) can only fail and every retire fence is -1
  (HYPOTHESIS from the source, not traced); SurfaceFlinger runs without a
  sync framework anyway (`ro.surface_flinger.running_without_sync_framework=true`
  in the image) and presented regardless (MEASURED).
- Input: it `mkfifo`s `/dev/input/wl_{pointer,keyboard,touch,tablet}_events`
  and writes `input_event` records for Android's EventHub
  (`wayland-hwc.cpp:1292-1323, 694, 1809`). The runtime maps `/dev/input`
  to one host directory for every guest (`runtime/evdev.c`), so the FIFOs
  land in `/tmp/lxrt-input`; with no InputFlinger reading them the
  composer logs "Failed to open pipe to InputFlinger" (ENXIO) when the
  pointer enters the window. Input is the next step, not done here.

Log lines that look like failures and are not (MEASURED, explained):
"FMQ: grantorIdx must be less than 3" is libfmq mapping the optional
event-flag word of a queue that has none (LineageOS `android_system_libfmq`
`19c77ec`, `include/fmq/MessageQueue.h:622, 1204-1207`); libprocessgroup's
cgroup warnings (no cgroups here); "Unable to set property ctl.start to
bootanim" (no init: the property service refuses `ctl.*`).

### The GL path

- Today (MEASURED): SwiftShader, in the image as
  `/vendor/lib64/egl/lib*_swiftshader.so`, renders GLES on the CPU for both
  SurfaceFlinger and apps; under FEX that is x86-64 SwiftShader JIT output
  translated again. It costs about 40% of a core for SurfaceFlinger and 35%
  for the boot animation at 800x568 and 57 frames/s.
- Also in the image, untested: Mesa's `libEGL_mesa.so` (needs a DRM device
  and gbm: there is none) and Mesa's lavapipe `vulkan.lvp.so` (Vulkan on the
  CPU, `ro.hardware.vulkan=lvp`, HYPOTHESIS: usable with gralloc "default").
  The Waydroid image has no ANGLE; Lepton's API 30 image adds it
  (`docs/LEPTON_REUSE_ANALYSIS.md` 0.2).
- Later, the GPU (HYPOTHESIS): a bionic build of SteamARM's Vulkan shim as
  Android's Vulkan HAL (`vulkan.steamarm.so`, MoltenVK or KosmicKrisp
  underneath, with `VK_ANDROID_native_buffer`), then GLES on top through
  ANGLE's Vulkan back end or Zink, as Lepton does over Turnip
  (`docs/ANDROID_ZERO_VM_FEASIBILITY.md` 3.11). The copies change too: a
  GPU gralloc would hand the compositor dmabuf-like buffers, which lxrun
  does not have; IOSurface or a remote CAMetalLayer per window
  (`runtime/remote_layer.m`) would replace wl_shm and the X11 hop.

### Costs of this path, and what could replace parts of it

Per frame, with gralloc "default": SurfaceFlinger composites on the CPU into
a gralloc buffer; the composer copies it into its wl_shm buffer (the
composer process uses 0.7 ms of CPU per 800x568 frame, 4% of a core at 57
frames/s); Weston composites it
with pixman into its X11 image; the X server copies that into the macOS
window. Four CPU passes over the frame between the app and the screen, and
three processes in the chain besides Android's. It is the correct first
step because every piece is unmodified (Android's HALs, Weston, XQuartz).
An `hwcomposer.steamarm` that is itself an X11 client (MIT-SHM) or presents
into a CAMetalLayer would remove Weston and one or two copies
(`docs/ANDROID_ZERO_VM_FEASIBILITY.md` 3.10); Waydroid's composer is
Apache-2.0 and its HWC1 structure is the reference.

### How to run it

```sh
make lxrt
scripts/android-boot.py --seconds 120                  # the x86_64 root; state in /tmp/lxrt-android-<uid>-<hash>
scripts/android-boot.py --state /tmp/b --persist /tmp/b/persistent_properties --seconds 60
tail -f /tmp/b/logcat.txt                              # logcat (logd.py); /tmp/b/init.log, /tmp/b/svc/<name>.log
scripts/android-boot.py --also vendor.hwcomposer-2-1   # a service outside the profile
scripts/android-boot.py --trace zygote                 # LXRT_TRACE for one service
```

scripts/mkwestonroot.sh                      # the Weston root (once, reproducible from scripts/mkwestonroot.lock)
scripts/run-x11-native.sh start              # SteamARM's X server on :2 (if not running)
scripts/run-weston.sh start                  # Weston on :2; --headless, --kiosk, --size WxH
scripts/run-weston.sh client /usr/bin/weston-simple-shm
scripts/run-android-display.sh start         # the x86_64 root's display stack and bootanimation
scripts/run-android-display.sh stop
scripts/run-weston.sh stop
```

- One display stack per Android root at a time: the binder hub and the
  property service are private (`ANDROID_DISPLAY_DIR`), but
  `<root>/dev/socket/logdw` (`ANDROID_DISPLAY_LOGD=1`) is the root's.
- The root's path must be short: FEX's server socket
  `<root>/data/local/tmp/steamarm-android-<hash>.FEXServer.Socket` has to
  fit Darwin's 104-byte `sun_path`; a 44-byte root path failed with
  "Failed to create FEXServer socket: error 22" (MEASURED).
- `weston-terminal` does not start: it needs a pseudo-terminal
  (`/dev/ptmx`), which the runtime does not pass through (VERIFIED IN
  SOURCE, `runtime/dispatch.c` `translate_one`).

## Reliability and input (stage 28, `benchmarks/stage28-android-reliability.txt`)

### The `sh -c 'x=$(toybox ...)'` hang: its root cause (MEASURED)

x86-64 Android's `mksh` blocks SIGCHLD around `fork` and then waits for the
child's SIGCHLD in `rt_sigsuspend` with SIGCHLD let through. The
intermittent hang of stage 27 (about 1 run in 40 of
`sh -c 'x=$(toybox echo x); y=$(toybox seq 3 | toybox wc -l); echo got $x $y'`
under FEX) was that SIGCHLD never reaching the guest. It was lost in two
independent places, and each one alone still hangs:

1. **XNU leaves a process-directed signal on the host main thread.** XNU
   binds a process-directed signal (a child's SIGCHLD, a `kill` of the
   process) to one thread when it is posted: a thread that does not block
   it or waits for it in `sigwait`, and when every thread blocks it, the
   process's first thread, where it stays (MEASURED on macOS 27,
   `tests/elf/sig_stranded.c`). Linux keeps it in the process's shared
   pending set for whichever thread unblocks it first. Under lxrun the
   first thread is the host main thread, which blocks every signal for
   good, so a SIGCHLD that came while the guest blocked it -- or while the
   runtime was in one of its all-signals-blocked sections, such as the
   thread-table lock every `rt_sigprocmask` takes -- was never delivered.
   Fix: `runtime/signal.c` `lxrt_signal_rescue_stranded`. The main thread
   looks at its own pending set on every turn of its loop (at most 20 ms
   apart, `runtime/window.m`), takes each signal that has the runtime's
   handler by unblocking it for a moment, and `host_handler`, seeing a
   non-guest thread, hands it with `pthread_kill` to a guest thread whose
   Linux mask lets it through, else to the first guest thread, where it
   stays pending until unblocked. `runtime/thread.c` notes each guest
   thread's Linux mask as `rt_sigprocmask`, `rt_sigsuspend` and a handler's
   return change it. A signal at SIG_DFL is left alone (taking it would act
   on it while the guest blocks it).
2. **FEX keeps it in `PendingSignals`, and `sigsuspend` did not look
   there.** FEX updates its emulated mask before the host mask
   (`GuestSigProcMask`), and a guest handler runs with a host mask narrower
   than the guest's; a signal that lands in between is taken from the host
   and only recorded in `PendingSignals`, to be raised again when the guest
   unmasks it with `rt_sigprocmask`. `GuestSigSuspend` never checked it
   before sleeping in the host `sigsuspend` (an instrumented FEX logged
   "SIGCHLD -> pending", then the `sigsuspend` that never woke). Fix:
   `patches/fex-lxrt-sigsuspend-pending.patch` raises each pending signal
   the suspend mask lets through again (`tgkill` to itself, blocked on the
   host), so the host `sigsuspend` delivers it and returns as Linux's does;
   `GuestSigTimedWait` does the same for the signals of its set.

| lxrun | FEX in the root | runs | hung (15 s deadline) |
|---|---|---|---|
| main (7492467) | main's: no `lxrt-sigsuspend-pending` | 240 | 3 |
| stage 28 | main's | 240 | 1 |
| main | stage 28's | 240 | 6 |
| stage 28 | stage 28's | 720 | **0** |

Each run in a process group of its own with a 15 s deadline
(`tests/android/run.sh`'s `dlrun`), other agents' guests running on the
same Mac. The previous session measured 12, 4, 3 and 0 hangs in 300 runs
each. `tests/elf/sig_stranded.c`: 0 ok, 3 mal on main's lxrun; 3 ok now.
`tests/android/run.sh` runs the command 40 times (`ANDROID_SIG_RUNS`): 40
of 40 in each of the record's six suite runs that got there, 240 more
with no hang.

Left as it is (VERIFIED IN SOURCE, not measured): a stranded signal that no
guest thread accepts goes to the first guest thread. A program whose
*other* thread later waits for it in `sigwait`/`sigtimedwait` (a dedicated
signal thread, as ART's signal catcher) would not get it, and a thread
already waiting in the runtime's `rt_sigtimedwait` is not noted as
accepting its set. Linux would hand it to whichever thread wants it first.
XNU itself picks a thread that waits in `sigwait` when the signal is
posted, so this needs the signal to come while that thread is busy.

Also found (VERIFIED IN SOURCE, MEASURED): FEXServer started with
`--foreground`, as `scripts/run-android-x86.sh` and `scripts/run-fex.sh`
start it, never exits when idle -- FEX's `ProcessPipe.cpp` waits with no
timeout when Foreground is set, whatever `--persistent` says. Three servers
of this stage's loops were still there after 7-10 idle minutes. Every one
left counts toward `scripts/safeguard.sh`'s 80 lxrun processes, past which
it kills every guest on the Mac; `scripts/run-android-x86.sh --server-stop`
stops a root's.

### i386 Android under FEX's 32-bit mode

The x86_64 image's 32-bit programs (`app_process32` as zygote_secondary,
`dex2oat32`, `dalvikvm32`, the 32-bit HALs) died at once in FEX's 32-bit
mode at stage 27. What each one needed, measured one after the other:

| what failed | where it is fixed |
|---|---|
| bionic makes every syscall through AT_SYSINFO; FEX's fallback vsyscall page (there is no VDSO thunk for bionic) was not tracked as executable: "NoExec instruction in entry block" | `patches/fex-lxrt-i386-bionic.patch` (ELFCodeLoader) |
| `FUTEX_CMP_REQUEUE`'s count was given the guest base like a pointer and arrived negative: EINVAL, ART "futex requeue failed" | the patch (x32 futex) |
| `rt_sigtimedwait` with a NULL siginfo (`sigwait`, ART's signal catcher) asserted; an error was copied out as a siginfo | the patch (x32 signals) |
| FEX's 32-bit allocator ignored free mmap hints: ART's boot image did not map at its address, then an imageless start (6 s for a hello world, 1.6 s with the image) | the patch (MemAllocator32Bit: a free hint is taken; a shared mapping only when the 16 KiB host pages it touches are free as a whole, else the runtime would place a private copy) |
| x32 `setsockopt`/`getsockopt` used the guest's `optval` without the base (the zygote's SO_RCVTIMEO on every connection) | the patch (x32 socket) |
| `msync` of a 32-bit guest's address (dex2oat32's vdex), SO_DOMAIN/SO_PROTOCOL, SO_RCVTIMEO_NEW/SNDTIMEO_NEW, MSG_TRUNC/MSG_CTRUNC echoed back by XNU, an empty SCM_RIGHTS | `runtime/dispatch.c`, `runtime/socket.c` |
| binder from a 32-bit guest: its addresses (the ioctl argument, the read/write buffers, a transaction's data and offsets, PTR buffers) are its own, below 4 GiB | `runtime/gbase.c`, `runtime/binder.c` (based where read or written; the hub is given the guest's view of the receive buffer) |

With them (MEASURED, `tests/android/run.sh` and
`tests/android/i386_bionic.c`, one check per fix): an i386 program linked
against the image's 32-bit bionic passes all 8 checks; `dalvikvm32` runs
Java; `dex2oat32` compiles a dex into an i386 odex that `dalvikvm32` runs;
an i386 native service registers with servicemanager, answers an x86-64
client and reads a descriptor passed to it; `android-boot.py` starts
zygote_secondary when the root's FEX carries the patch (it carries the
string `lxrt-i386-bionic`), system_server connects to it and no longer
waits 20 s for it. The 32-bit audio HAL registers IDevicesFactory and
IEffectsFactory 4.0 and audioserver starts AudioFlinger, with
`android-boot.py --linkerconfig` (init's `update_linker_config` with the
property service up: the root's legacy `/linkerconfig` has no VNDK
namespace for `android.hardware.audio@4.0.so`; opt-in because it rewrites
the root's file). Re-run for the record: system_server entered 8 s after
the boot started (stage 27: 30 s, 20 of them waiting for the secondary
zygote).

Two limits of i386 bionic under lxrun remain (MEASURED,
`tests/android/i386_rmutex.c`), both from 32-bit bionic keeping thread ids
in 16 bits (a 4-byte `pthread_mutex_t`):

- **The main thread's tid is the Mac's pid**, up to 99999, and 32-bit
  bionic refuses to start above 65535 ("Limited by the size of
  pthread_mutex_t, 32 bit bionic libc only accepts pid <= 65535, but
  current pid is 84368"). With the Mac's pid counter in the upper third of
  its range every i386 program aborts at start (and under FEX the abort
  then hung until the test's deadline); after it wraps they run again.
  `tests/android/run.sh` names these runs as expected failures.
- **Every other thread's tid is 200000 + pid * 10000 + n**
  (`runtime/thread.c`, chosen so tids never meet a Darwin pid), so a
  second thread does not own its own mutexes: relocking a recursive mutex
  gives EBUSY, unlocking an error-checking one EPERM, and a `printf` from
  such a thread (stdio's recursive lock) never returns. zygote_secondary,
  dalvikvm32 and dex2oat32 got through their runs, but any 32-bit Android
  code that uses a recursive mutex off the main thread will hang. The fix
  is tids (and a main-thread pid) below 65536 for 32-bit bionic guests,
  unique across processes -- a system-wide id allocator, not done yet.
  i386 glibc (the Steam client) has no such limit.

The same FEX serves every FEX guest, so the Steam/Proton route was run
against it: `tests/elf/run_i386.sh`, `tests/win/run.sh` (Proton D3D9/11/12,
64- and 32-bit, frame rates within ~1 fps of the installed FEX in alternating runs)
and `tests/elf/run_vk_device.sh` gave the same results as before.

### Input: a /dev/input per Android stack

The runtime maps a guest's `/dev/input` to one host directory,
`/tmp/lxrt-input`, where `steamarm-inputd` publishes the controllers
(`runtime/evdev.c`). Waydroid's composer makes its input FIFOs there
(`/dev/input/wl_pointer_events`, `wl_keyboard_events`, ...), for
InputFlinger's EventHub to read: in the shared directory they sat next to
the controllers, and two Android roots would have met in one FIFO.
`LXRT_INPUT_DIR=<absolute host directory>` now gives a guest, and what it
runs, a `/dev/input` of its own; unset, it is `/tmp/lxrt-input` as before,
so the Steam controller path does not change.
`scripts/run-android-display.sh` and `scripts/android-boot.py` give their
stack `<state>/input`, and the display stack's stop removes the FIFOs.

MEASURED: with the real composer (x86-64 under FEX) its FIFOs appear in the
stack's own directory, and 30 synthetic pointer motions sent to Weston's X
window come out of `wl_pointer_events` as 150 records (ABS X/Y, REL X/Y,
SYN) to an EventHub-style reader (`tests/android/input_fifo.c`,
`tests/android/xsend_motion.py`); a guest with another `LXRT_INPUT_DIR`
does not see the FIFO. InputFlinger itself has not read them:
system_server stops at the display in these boots.

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
2. Binder: done in userspace (stage 25), and x86-64 guests reach it
   through FEX (stage 27). Next on it: ashmem/memfd sharing across
   processes, the binder debug logs `lshal` reads, the cost per call.
3. Properties: done (stage 26), for x86-64 guests too (stage 27).
4. init, zygote64, system_server: running up to LightsService in the
   x86_64 root (stage 27). The next wall is a display composer for
   SurfaceFlinger: Waydroid's with a working Wayland compositor, or one of
   SteamARM's. After it, in the order system_server meets them:
   PackageManagerService (`pm list packages`, `pm install`), then netd.
   audioserver's i386 audio HAL runs since stage 28 (with
   `--linkerconfig`), and so do zygote_secondary and dex2oat32.
5. Then a window, input and audio (Lepton's graphics analysis, 4), and the
   rebuilt ART for arm64-v8a APKs in the native root.
2. Binder: done in userspace (stage 25, above). Next on it: ashmem/memfd
   sharing across processes, the binder debug logs `lshal` reads, the cost
   per call, and x86-64 guests reaching it through FEX (UNTESTED).
3. Properties: done (stage 26, above). The next wall for native Android
   services is init: a start order for the daemons and HALs, restarts, and
   the property triggers of the `.rc` files, on top of this property
   service (also what linkerconfig's `VENDOR_VNDK_VERSION` abort asks for);
   then shared memory between a HAL and its clients.
4. Then zygote, system_server, `pm install` and a window: the window's
   path exists (stage 27, "Display": SurfaceFlinger through Waydroid's
   hwcomposer into Weston, a macOS window); input (the composer's FIFOs to
   InputFlinger) and a GPU path are next. First in the x86_64 root for
   Java and x86-64 apps, then with the rebuilt ART for arm64-v8a APKs.

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
  root needed; ANDROID_IDS (`tests/elf/android_ids.c`): Android ids, with
  and without `LXRT_ANDROID_IDS`.
- `tests/android/run.sh`, x86_64 section (stage 27): getprop, setprop and
  `__system_property_wait` from x86-64 bionic; servicemanager, an x86-64
  native service with a descriptor, dumpsys, vndservicemanager,
  hwservicemanager, lshal and a HAL; `x86_initsock.c` (an init socket's
  name, `/proc/self/fd`, `attr/current`); `android-boot.py` to
  system_server, with the SurfaceFlinger wall an expected failure.
- `benchmarks/stage25-android-userspace.txt`, `benchmarks/stage25-binder.txt`,
  `benchmarks/stage25-art-x86-fex.txt`,
  `benchmarks/stage26-android-properties.txt` and
  `benchmarks/stage27-android-framework.txt`: every run, before and after.
  root needed.
- `tests/android/run.sh`, display section: Weston (headless) with an
  aarch64 and an x86-64 (FEX) wl_shm client; with an X server on :2, the
  x86-64 client's pixels read back from Weston's X window with `xwd`
  (`tests/android/xwd_colors.py`), then SurfaceFlinger's boot animation in
  it and its page-flip rate (`ANDROID_DISPLAY_SF=0` skips that part).
  `LXRT_BINDER_UID` in the binder section.
- `tests/elf/run.sh` SIMD_SYSCALL (`tests/elf/simd_syscall.c`) and
  SHM_MREMAP (`tests/elf/shm_mremap.c`): the two runtime fixes of stage 27.
- `tests/android/run.sh`, stage 28: the x86_64 `$(toybox ...)` command 40
  times (`ANDROID_SIG_RUNS`); i386 (`tests/android/i386_bionic.c`,
  `dalvikvm32`, `dex2oat32` and its odex, an i386 binder service, the boot's
  zygote_secondary), each an expected failure on a root whose FEX lacks
  `lxrt-i386-bionic` or when the Mac's pid is above 65535; the 16-bit tid
  limit (`tests/android/i386_rmutex.c`, an expected failure until it is
  fixed); `LXRT_INPUT_DIR` (`tests/android/input_fifo.c`
  between two x86-64 guests, and through the composer with an X server).
  `tests/elf/run.sh` SIG_STRANDED (`tests/elf/sig_stranded.c`) and
  LXRT_INPUT_DIR.
- `benchmarks/stage25-android-userspace.txt`, `benchmarks/stage25-binder.txt`,
  `benchmarks/stage25-art-x86-fex.txt`,
  `benchmarks/stage26-android-properties.txt`,
  `benchmarks/stage27-android-display.txt` and
  `benchmarks/stage28-android-reliability.txt`: every run, before and after.
