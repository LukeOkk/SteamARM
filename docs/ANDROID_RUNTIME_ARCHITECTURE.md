# Android userspace on lxrun, with no VM

Status 2026-09-29, stage 25 (`benchmarks/stage25-android-userspace.txt`,
`benchmarks/stage25-art-x86-fex.txt`).
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
That wall comes before binder. Both have to fall before an APK runs.

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
| `getprop ro.build.version.sdk` | empty, exit 0 (no property service) |
| `/apex/com.android.art/bin/dexdump -d hello.dex` | the D8-built classes and their bytecode |
| `servicemanager`, `service list` | "Binder driver '/dev/binder' could not be opened. Terminating." |
| `dalvikvm64 -cp hello.dex Hello` (and `-Xint`), `dex2oat64` | abort: "Could not find contiguous low-memory space." |

Start-up costs (10 runs, median): `toybox true` 58 ms, `mksh -c true`
41 ms, a `$(toybox echo x)` fork and exec 90 ms, `sha256sum` of 27 MB
74 ms.

## How to run it

```sh
scripts/mkandroidroot.sh          # download, sha256-check, extract (once)
make lxrt                         # or: rm -f build/lxrun && make lxrt LXRT_KEEP_X18=1
LXRT_ROOT=/Volumes/SteamARMAndroid/root LXRT_GUEST_PAGE=4096 \
    build/lxrun /system/bin/toybox uname -a
tests/android/run.sh              # the battery: 7 passed, 1 expected failure
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

## The next layer: binder (for the next agent)

What Android's first binder process does today (MEASURED): `servicemanager`
opens `/dev/binder`, gets ENOENT, logs "Binder driver '/dev/binder' could not
be opened. Terminating." and aborts (exit status 1 under lxrun: the crash
handler cannot make its pseudothread).

What libbinder asks of the driver (VERIFIED IN SOURCE,
`LineageOS/android_frameworks_native` lineage-18.1 `103c04d`):

- `open("/dev/binder" | "/dev/hwbinder" | "/dev/vndbinder", O_RDWR|O_CLOEXEC)`
  (`libs/binder/ProcessState.cpp:351`); `servicemanager` takes the driver
  path as its argument (`cmds/servicemanager/main.cpp:121`).
- ioctls: `BINDER_VERSION`, `BINDER_SET_MAX_THREADS` (15),
  `BINDER_SET_CONTEXT_MGR_EXT` then `BINDER_SET_CONTEXT_MGR`,
  `BINDER_WRITE_READ` (all traffic), `BINDER_THREAD_EXIT`,
  `BINDER_GET_NODE_DEBUG_INFO`, `BINDER_GET_NODE_INFO_FOR_REF`,
  `BINDER_FREEZE` and `BINDER_GET_FROZEN_INFO`
  (`ProcessState.cpp:145-367`, `IPCThreadState.cpp:980-1349`).
- `mmap(NULL, 1 MiB - 2 pages, PROT_READ, MAP_PRIVATE|MAP_NORESERVE, fd, 0)`
  (`ProcessState.cpp:43,400`): the driver copies each incoming transaction
  into that buffer and hands the reader offsets into it.

What lxrun would have to provide (HYPOTHESIS, design notes only):

- Guest processes are separate macOS processes (fork is a real fork), so
  binder is inter-process: a host "binder hub" process that owns nodes,
  references, handles, death notifications and per-thread work queues, and
  one channel per guest process to it (a Unix socket for commands, and the
  1 MiB receive buffer as shared memory the hub writes into and the guest
  maps read-only at its `mmap` of the binder fd). `BINDER_WRITE_READ` blocks
  in the guest's thread on that socket.
- Object translation in flight: `BINDER_TYPE_BINDER`/`WEAK_BINDER` to handles
  and back, `BINDER_TYPE_FD` (descriptors passed with SCM_RIGHTS through the
  hub and installed in the receiver), `BINDER_TYPE_PTR` scatter-gather
  buffers (hwbinder), sender pid and euid (Lepton patches libbinder to send a
  fake uid instead of trusting the kernel's: `docs/LEPTON_REUSE_ANALYSIS.md`
  3.6).
- Three contexts: binder, hwbinder, vndbinder, each with its own context
  manager.
- Memory sharing between Android processes goes through ashmem or memfd with
  `mmap(MAP_SHARED)` across processes: lxrun's memfd seals hold only inside
  one process (`docs/LEPTON_REUSE_ANALYSIS.md` 7).

Around binder, before anything Java could run even with ART fixed:

- **Properties.** bionic reads `/dev/__properties__` (an mmapped trie and
  per-context areas written by init's property service). Absent today,
  every read is empty (MEASURED). Options: generate the areas offline from
  the image's `*.prop` and `property_contexts` files with a host tool, or
  run init's property service; `setprop` needs a writer
  (`/dev/socket/property_service`).
- **init.** Lepton runs Android's own init in a container with SELinux,
  ueventd and device-mapper removed (`docs/LEPTON_REUSE_ANALYSIS.md` 3.7);
  here init would have to run as an ordinary process with no mount or pid
  namespace: a plan interpreter in the spirit of `runtime/mounts.c`, or a
  hand-written start order (logd, servicemanager, hwservicemanager,
  vndservicemanager, then zygote).
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
   poisoning hooks in ART's source.
2. Binder, in parallel (native services can be tested without ART:
   `servicemanager` and `service list`, a native client and server). The
   x86_64 root runs the same libbinder under FEX, with a working ART, so
   binder can be tested against Java there too (x86-64 guests reach the
   runtime's ioctls through FEX; UNTESTED).
3. Properties and a minimal init sequence (also what linkerconfig's
   `VENDOR_VNDK_VERSION` abort asks for).
4. Then zygote, system_server, `pm install` and a window (Lepton's graphics
   analysis, 4): first in the x86_64 root for Java and x86-64 apps, then
   with the rebuilt ART for arm64-v8a APKs.

## Tests and records

- `tests/android/run.sh`: the programs above against the arm64 root (ART
  an expected failure with the heap reason), then an x86_64 section against
  `root-x86_64` under FEX: bionic programs, two freestanding x86-64 probes
  (`x86_lowwin.c`: `MAP_32BIT`, `MADV_DONTNEED` zeroing, low hints,
  startstack, `RLIM_INFINITY`; `x86_dualview.c`: code rewritten through a
  dual-mapped memfd), and `dalvikvm64` against the Mac's JVM. Each section
  skips without its root.
- `tests/elf/run.sh` ANDROID_BIONIC_RT and "kept TLS reads"
  (`tests/elf/android_bionic_rt.c`): the runtime changes, with no Android
  root needed.
- `benchmarks/stage25-android-userspace.txt` and
  `benchmarks/stage25-art-x86-fex.txt`: every run, before and after.
