# Android userspace on lxrun, with no VM

Status 2026-09-29, stage 25 (`benchmarks/stage25-android-userspace.txt`,
`benchmarks/stage25-binder.txt`).
The owner's goal is Android on the Mac: install APKs and, later, the Google
Play Store, with **zero VM** (`AGENTS.md`). This page says what of Android
runs today, how to run it, what stops Java, and what the next layers need.

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
(MEASURED, "Binder" below). The ART heap is what stands between this and an
APK.

## What runs today (MEASURED)

| program | result |
|---|---|
| `/system/bin/linker64 /system/bin/toybox ls /` | the root's 36 entries |
| `toybox echo`, `uname -a`, `id`, `cat`, `grep -E`, `seq`, `paste`, `date`, `wc` | correct output (`id`: "bad uid 501", the Mac's uid is not an Android one) |
| `toybox sha256sum` / `md5sum` of `framework.jar` (27 MB) | equal to the Mac's `shasum` / `md5`; libcrypto's FIPS self-test passes |
| `/system/bin/sh` (mksh): arithmetic, loops, `$(...)`, pipes | correct, with fork and exec |
| `getprop ro.build.version.sdk` | empty, exit 0 (no property service) |
| `/apex/com.android.art/bin/dexdump -d hello.dex` | the D8-built classes and their bytecode |
| `servicemanager`, then `service list`, `service check`, `service call`, `dumpsys -l` | the context manager on the userspace `/dev/binder`; "Found 1 services: 0 manager: [android.os.IServiceManager]" |
| a native service (another process) registering with `servicemanager` | listed by `service list` with its interface name, called by `service call` with an int and with a file descriptor, removed when it dies |
| `vndservicemanager /dev/vndbinder`, `vndservice list` | the vendor context, "Found 1 services" |
| `hwservicemanager` | context manager on `/dev/hwbinder`; cannot set `hwservicemanager.ready` (no property service), so HIDL clients (`lshal`) never start (see "Binder") |
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

Until one of these lands, nothing that runs Java can start: no zygote, no
system_server, no APK, no Play Store. Native daemons and tools can.

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
- `hwservicemanager` becomes the context manager of `/dev/hwbinder`, but
  cannot set `hwservicemanager.ready` ("Failed to set ... (error 2). HAL
  services will not start!": there is no property service). `lshal` never
  opens `/dev/hwbinder`; it spins at 100% CPU with no syscalls. That
  libhidl waits for that property rests on the string "Waited for
  hwservicemanager.ready for a second" in `libhidlbase.so`; where exactly
  it spins is HYPOTHESIS. Properties come next (below), not binder.
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
(`LXRT_BINDER_DIR` overrides it; the tests use private ones). It keeps
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
tests/android/run.sh              # servicemanager, service, dumpsys, vndservicemanager
LXRT_BINDER_LOG=1 ...             # the hub logs every transaction to <dir>/hub.log
```

### Around binder, before anything Java could run even with ART fixed

- **Properties.** bionic reads `/dev/__properties__` (an mmapped trie and
  per-context areas written by init's property service). Absent today,
  every read is empty (MEASURED). Options: generate the areas offline from
  the image's `*.prop` and `property_contexts` files with a host tool, or
  run init's property service; `setprop` needs a writer
  (`/dev/socket/property_service`). This is now the first wall for native
  services: hwservicemanager cannot announce `hwservicemanager.ready`, so
  no HIDL client or HAL starts (MEASURED, "Binder" above).
- **Shared memory across processes.** Parcels carry ashmem or memfd
  descriptors; binder passes them (FD objects), but lxrun's memfd seals
  hold only inside one process (`docs/LEPTON_REUSE_ANALYSIS.md` 7), and
  libcutils decides between ashmem and memfd from a property: `service`
  logs "ashmem: memfd: ro.vndk.version not defined or invalid ()"
  (MEASURED).
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
  source) or write one.
- **Graphics, input, audio**: `docs/LEPTON_REUSE_ANALYSIS.md` 4.

## APKs and the Google Play Store

- An APK is a zip: its dex runs on ART (blocked above) and its
  `lib/arm64-v8a/*.so` load into an ART process (blocked with it).
  `armeabi-v7a`-only apps cannot run on Apple silicon (no AArch32; 1,684
  32-bit arm files in this image are unusable for the same reason). x86 ABIs
  would need FEX inside Android: analysis only, not planned.
- Google Play Store and Google Play services are proprietary. SteamARM will
  never commit or bundle them; if it ever fetches them, it does so on the
  owner's Mac from Google's official source, and only if that is legal. The
  Play Store also expects a device registered as certified and Play
  Integrity verdicts; SteamARM must not falsify certification or bypass
  Play Integrity or SafetyNet, so apps that require them will refuse to run
  (UNKNOWN which ones). None of this is reachable before the ART heap wall
  and binder.

## Order of work

1. The ART heap (above): without it, nothing Java runs. Start with a
   feasibility count of the reference sites outside the poisoning hooks in
   ART's source.
2. Binder: done in userspace (stage 25, above). Next on it: libbinder's own
   thread pool in a real Android daemon (tested so far with raw ioctls),
   ashmem/memfd sharing across processes, and the cost per call.
3. Properties and a minimal init sequence: now the first wall for native
   Android services (hwservicemanager.ready, HIDL, `setprop`).
4. Then zygote with the rebuilt ART, system_server, `pm install` of an
   arm64-v8a APK, and a window (Lepton's graphics analysis, 4).

## Tests and records

- `tests/android/run.sh`: the programs above against the root; ART is an
  expected failure with the heap reason; binder with servicemanager,
  `service`, `dumpsys`, a native service and vndservicemanager.
- `tests/elf/run.sh` BINDER_IPC and BINDER_POOL (`tests/elf/binder_ipc.c`):
  the binder driver between lxrun processes with raw ioctls.
- `tests/elf/run.sh` ANDROID_BIONIC_RT and "kept TLS reads"
  (`tests/elf/android_bionic_rt.c`): the runtime changes, with no Android
  root needed.
- `benchmarks/stage25-android-userspace.txt`: every run, before and after.
- `benchmarks/stage25-binder.txt`: the binder runs and costs.
