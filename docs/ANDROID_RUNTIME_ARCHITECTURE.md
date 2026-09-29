# Android userspace on lxrun, with no VM

Status 2026-09-29, stage 25 (`benchmarks/stage25-android-userspace.txt`).
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
That wall comes before binder. Both have to fall before an APK runs.

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

Until one of these lands, nothing that runs Java can start: no zygote, no
system_server, no APK, no Play Store. Native daemons and tools can.

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
2. Binder, in parallel (native services can be tested without ART:
   `servicemanager` and `service list`, a native client and server).
3. Properties and a minimal init sequence.
4. Then zygote with the rebuilt ART, system_server, `pm install` of an
   arm64-v8a APK, and a window (Lepton's graphics analysis, 4).

## Tests and records

- `tests/android/run.sh`: the programs above against the root; ART is an
  expected failure with the heap reason.
- `tests/elf/run.sh` ANDROID_BIONIC_RT and "kept TLS reads"
  (`tests/elf/android_bionic_rt.c`): the runtime changes, with no Android
  root needed.
- `benchmarks/stage25-android-userspace.txt`: every run, before and after.
