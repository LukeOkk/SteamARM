# ART with its heap above 4 GiB: prototype patch series

**Everything here is HYPOTHESIS and UNTESTED.** No patch was compiled in an
Android tree, nothing was run. What was checked (record:
`benchmarks/stage28-art-heap-sites.txt`): the series applies with `git am`
to LineageOS `android_art` lineage-18.1 `7f2b48980eaa` and to AOSP
`platform/art` android11-release `34f62af4d642`; the new arm64 assembly
macros assemble (one `ORR` per decompression); the new constants in
`libartbase/base/globals.h` pass a syntax check. The study, the counts and
the go/no-go are in `docs/ART_HEAP_ABOVE_4GB.md`.

## What the series does

Android 11's ART keeps heap references in 32 bits and uses them as
addresses, so its heap must be below 4 GiB, where an arm64 macOS process
can map nothing (`docs/ANDROID_RUNTIME_ARCHITECTURE.md`, "The ART heap
wall"). The series moves every object, the boot image and the JIT data into
a 4 GiB window at a fixed base `B` (`ART_HEAP_REFERENCE_BASE`, proposed
`0x4000000000`, 256 GiB) and keeps references 32-bit:

- in memory (heap fields, GC roots, dex registers, 32-bit stack slots,
  .bss, .data.bimg.rel.ro) a reference is the low 32 bits of its address;
  a 32-bit store of the pointer is the compression;
- in C++ and in compiled code's core registers it is the full pointer:
  a load decompresses with `x | B` (null stays 0);
- `B` is a compile-time constant, a single bit, so arm64 code applies it
  with one `ORR` immediate and needs no reserved register.

With `ART_HEAP_REFERENCE_BASE` unset every patch compiles to upstream
behaviour (base 0).

| patch | what it changes | needed for |
|---|---|---|
| 0001 build, libartbase | `ART_HEAP_REFERENCE_BASE` (environment) defines `kHeapReferenceBase` (device runtime, C++ and assembly) and `kTargetHeapReferenceBase` (arm64 code generator, host and device); `CompressHeapAddress`/`DecompressHeapAddress`/`IsInHeapReferenceWindow`; `PointerToLowMemUInt32` means "compressed address in the window", `LowMemUInt32ToPointer` is its inverse | all |
| 0002 runtime | the central encodings: `PtrCompression` (every `HeapReference`, `CompressedReference`, `StackReference`, `GcRoot`), `ObjPtr`'s debug encoding, the lock word's forwarding address, the class table slot; `ReferenceFromVRegValue`/`VRegValueFromReference` | phase 1 |
| 0003 runtime | the 19 casts between a 32-bit vreg or argument value and an object pointer (stack walking, deoptimization, monitors, interpreter argument arrays, string builder, dex2oat), `Heap::IsBootImageAddress`; app images off (their relocation is not ported) | phase 1 |
| 0004 libartbase, runtime | MemMap's "low 4 GiB" allocator scans `[B + 64 KiB, B + 4 GiB)`; the card table covers the window; the sentinel fault page is not a low-4 GiB request | phase 1 |
| 0005 runtime, dex2oat | the boot image in the window: reservation at `B + ART_BASE_ADDRESS`, ImageHeader address getters, relocation in 32-bit arithmetic plus `B`, so an upstream boot image relocates as it is; oat files record the base their code needs (`heap-reference-base`), and code built for another base is refused unless the runtime is interpret-only (`-Xint`) | phase 1 |
| 0006 interpreter | the arm64 assembly interpreter (mterp) is not ported: `CanUseMterp()` is false with a base, the switch interpreter runs | phase 1 |
| 0007 arm64 entrypoints | `DECOMPRESS_HEAP_REF[_NOT_NULL]` macros; invoke stubs decompress `this` and `L` arguments; `art_quick_aput_obj`'s read barrier, array allocation, IMT conflict trampoline, `READ_BARRIER_MARK_REG`, the mark introspection entrypoint (its field slow path moves out of line to keep the 256-byte budget) | phase 1 (invoke stubs), phase 3 (the rest) |
| 0008 compiler | arm64 code keeps full pointers in registers: the load hook `MaybeUnpoisonHeapReference` also decompresses; Baker read-barrier loads, GC roots and the GC-root thunk decompress; `.data.bimg.rel.ro`, JIT literals and root slots; exception/peer as 64 bits; 64-bit address arithmetic on references (array elements, far fields, UnsafeGet, vectors, card marking); register moves and selects of references keep 64 bits, spill reloads decompress; `HIntermediateAddress` not extracted | phase 3 |
| 0009 JNI stubs | `LoadRef` decompresses; `LoadReferenceFromHandleScope` reads 32 bits | phase 3 |

Phase 1 is the smallest thing that can prove the design on the Mac:
0001-0007 (0007 for its invoke stubs) with the upstream boot image, `-Xint`
and the switch interpreter. Phase 3 adds the compiler (0008, 0009), a boot
image and odex files recompiled by the patched `dex2oat64`, and the JIT.
(Phase 2, porting mterp, is optional: about 20 sites, see the study.)

`count_sites.py` is the scripted count of the study
(`python3 patches/art-heap-base/count_sites.py [--list] ART_DIR`).

## Build (on an x86-64 Linux host; not on the Mac)

ART 11 has no unbundled build; it builds inside a whole Android 11 tree.
Requirements (UPSTREAM DOCUMENTED, https://source.android.com/docs/setup/start/requirements:
x86-64 Linux, "at least 400 GB" of disk and "at least 64 GB" of RAM for
current AOSP; Android 11 builds commonly succeed with 16-32 GB of RAM and
swap, HYPOTHESIS). For the ART module alone, HYPOTHESIS: about 150-250 GB
of disk (checkout plus the host and arm64 intermediates) and 32 GB of RAM;
Soong still parses every `Android.bp` of the tree. Macs are not supported
build hosts ("Android OS development on macOS isn't supported as of June
22, 2021", same page), and SteamARM runs no VM, so this is a separate Linux
machine or CI.

```sh
# 1. The tree the Waydroid image was built from (LineageOS 18.1).
mkdir lineage-18.1 && cd lineage-18.1
repo init -u https://github.com/LineageOS/android.git -b lineage-18.1 --depth=1
repo sync -c --no-tags -j8
# 2. The series (art/ is LineageOS/android_art; 7f2b489 is the image's ART).
cd art && git checkout 7f2b48980eaa && git am /path/to/SteamARM/patches/art-heap-base/0*.patch && cd ..
# 3. Configure and build the ART module, flattened (a directory, as the
#    Waydroid image has it).
source build/envsetup.sh
lunch aosp_arm64-userdebug        # or the Waydroid target, if its device tree is synced
export ART_HEAP_REFERENCE_BASE=0x4000000000
export ART_USE_CXX_INTERPRETER=true   # optional: mterp is off anyway with a base
export TARGET_FLATTEN_APEX=true
m com.android.art.release
# Result: $OUT/system/apex/com.android.art.release/{bin,lib64,javalib,...}
```

`ART_HEAP_REFERENCE_BASE` is read by Soong (`build/art.go`); export it
before the first `m`, and a change makes Soong regenerate its build files.
Leave it unset to check that the series builds to upstream behaviour first
(`m test-art-host-gtest` exercises the host side).

## Install into SteamARM's arm64 root (proposed, not implemented)

`scripts/mkandroidroot.sh` copies each flattened APEX of `/system/apex` to
`/apex/<name>` (APFS clones; `com.android.art.release` becomes
`/apex/com.android.art`). A rebuilt ART module goes over both copies. The
proposal is an option, `scripts/mkandroidroot.sh --art-apex DIR`, run after
the root exists, that does what these commands do by hand:

```sh
ROOT=/Volumes/SteamARMAndroid/root
NEW=$HOME/SteamARM-roots/android/art-heap-base/com.android.art.release   # copied from $OUT
for d in "$ROOT/system/apex/com.android.art.release" "$ROOT/apex/com.android.art"; do
    rsync -a --delete "$NEW/bin/" "$d/bin/"
    rsync -a --delete "$NEW/lib64/" "$d/lib64/"
    # Phase 1 keeps the image's javalib/ (upstream boot image, relocated
    # into the window, interpreted). Phase 3 also copies javalib/ from a
    # build with dexpreopt, or lets the runtime compile a boot image.
done
rm -rf "$ROOT/data/dalvik-cache/arm64"/*       # odex files of the old ART
shasum -a 256 "$NEW/lib64/libart.so" >> "$ROOT/.steamarm-androidroot"
```

The option would also record the series' commit and refuse a directory
without `lib64/libart.so`. Nothing from the build is committed to SteamARM.

## Run (phase 1 test plan, nothing measured)

```sh
LXRT_ROOT=/Volumes/SteamARMAndroid/root LXRT_GUEST_PAGE=4096 \
    build/lxrun /apex/com.android.art/bin/dalvikvm64 -Xint -Xusejit:false \
    -cp /data/local/tmp/hello.dex Hello
```

with `env -i` and `init.environ.rc`'s variables as `tests/android/run.sh`
gives them. Expected, if the design holds: "Hello from Java on ART, no VM"
instead of stage 25's "Could not find contiguous low-memory space"; ART's
spaces in `/proc/self/maps` between 0x4000010000 and 0x4100000000. The
next checks, in order, are in `docs/ART_HEAP_ABOVE_4GB.md` ("Test plan").
