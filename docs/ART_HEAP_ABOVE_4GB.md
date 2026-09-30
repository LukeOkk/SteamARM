# ART with its heap above 4 GiB: feasibility study and prototype

Status 2026-09-30, stage 28: research, source reading and a prototype patch
series. Nothing in this page ran ART; no patch was compiled in an Android
tree. The record is `benchmarks/stage28-art-heap-sites.txt`; the patches and
the counting script are in `patches/art-heap-base/`.

Labels as in `docs/ANDROID_RUNTIME_ARCHITECTURE.md`: MEASURED, VERIFIED IN
SOURCE, UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN.

## In one paragraph

Native arm64 ART cannot start on the Mac because its 32-bit heap references
are used directly as addresses, so every object must lie below 4 GiB, where
an arm64 macOS process maps nothing (MEASURED, stage 25). Reading all of
Android 11's ART (LineageOS 18.1 and AOSP android11-release) shows that the
32-bit reference is converted to and from a pointer in one central C++
class for almost all of the runtime (5,212 uses follow it unchanged), and
that the places which bypass it are countable: **232 source lines** in the
runtime, the arm64 compiler, the arm64 assembly and the image/oat code
(VERIFIED IN SOURCE, scripted). The recommended design keeps references
32-bit, puts every object, the boot image and the JIT data in a 4 GiB
window at a fixed base of `0x4000000000`, and decompresses with one or two
instructions (`cbz; orr x, x, #base`) where a reference is loaded, the way
HotSpot handles compressed oops with a non-zero heap base; arm64 code keeps
full pointers in registers and needs no reserved register. The prototype
series is 9 patches, 38 files, +534/-101 lines (HYPOTHESIS, UNTESTED). Its
first stage runs Java with only the runtime rebuilt: the upstream boot
image relocates into the window as it is, and `-Xint` keeps its compiled
code from running. **Verdict: GO for that first stage** (a runtime-only
rebuild, interpreted), **conditional GO for the compiler stage** after it
passes. 64-bit references (design C) are a no-go.

## The wall

MEASURED (stage 25, `benchmarks/stage25-android-userspace.txt`):
`dalvikvm64` and `dex2oat64` of the Waydroid LineageOS 18.1 arm64 image
abort with "Could not find contiguous low-memory space" and
`heap.cc:493 Check failed: non_moving_space_mem_map.IsValid()`; the boot
image "Failed to mmap at expected address, mapped at 0x158000000" (it wants
0x70000000). A plain Mach-O cannot map below 4 GiB (`MAP_FIXED` at
0x10000, 0x70000000, 0xfc000000: ENOMEM; a smaller `-pagezero_size`:
killed at exec).

VERIFIED IN SOURCE (LineageOS `android_art` lineage-18.1 `7f2b48980eaa`,
the image's ART; line numbers are at that commit):

- `runtime/mirror/object_reference.h:36-49`: `PtrCompression::Compress`
  truncates the pointer to 32 bits and `Decompress` zero-extends it.
- `compiler/optimizing/common_arm64.h:76-79`: the arm64 code generator
  holds a `kReference` value in a W register; `HeapOperand` (`:180-185`,
  "A heap reference must be 32bit, so fit in a W register") dereferences
  its X view, which is correct only because a 32-bit write zero-extends.
- `runtime/arch/arm64/quick_entrypoints_arm64.S:1117-1136`: the assembly
  says it in its comments: `"Compress" = do nothing`, `"uncompress" = do
  nothing, already zero-extended`.
- `code_generator_arm64.cc:4245`, before a 32-bit load of a boot image
  `ArtMethod*`: "Boot image is in the low 4GiB and the entry is 32-bit".
- 14 mappings ask MemMap for `low_4gb` memory (heap spaces, boot image,
  large objects, JIT); on arm64 ART serves them with its own allocator that
  probes the low 4 GiB with `msync()` from 64 KiB
  (`libartbase/base/mem_map.cc:97, 1036-1118`).

## Sources

| tree | branch | commit |
|---|---|---|
| LineageOS/android_art | lineage-18.1 | `7f2b48980eaa` (2021-11-03, the image's ART, as stage 25) |
| AOSP platform/art | android11-release | `34f62af4d642` (2020-07-16) |

Shallow clones, read 2026-09-30. The two trees give the same counts except
three `ObjPtr` lines; the series applies to both.

## Where a 32-bit reference becomes a pointer, counted

`patches/art-heap-base/count_sites.py` scans the tree with one pattern per
category (tests excluded; a few assembly categories are small per-handler
scans). Counts are source lines on LineageOS `7f2b489`; every hit of an
EDIT, CONSUMER, NONE or EVIDENCE category is listed in the stage 28 record
and was read. "Action" is what the recommended design does:

- **EDIT**: the line, or the helper it calls, changes;
- **CENTRAL**: no edit, it follows one central change;
- **CONSUMER**: no edit under the recommended design (registers hold full
  pointers), but a design that adds the base at every dereference edits it;
- **NONE**: stays (a 32-bit store of the low half is the compression);
- **LAYOUT**: what 64-bit references would change.

| area | EDIT | CENTRAL | CONSUMER | NONE |
|---|---:|---:|---:|---:|
| runtime and libartbase C++ | 86 | 5,211 | | 19 |
| arm64 optimizing compiler | 93 | | 77 | 6 |
| arm64 runtime assembly (entrypoints) | 17 | | 2 | 5 |
| arm64 assembly interpreter (mterp) | 20 | | | 8 |
| arm64 JNI stubs | 7 | | 16 | |
| dex2oat, oat, image | 9 | 1 | | |
| **total** | **232** | **5,212** | **95** | **38** |

Plus 100 EVIDENCE lines (81 register-width decisions, 19 comments stating
the zero-extension assumption) and 588 LAYOUT lines (below).

### Runtime C++ (86 EDIT)

- **R1, 8: the central encodings.** `PtrCompression::Compress/Decompress`
  (every `HeapReference`, `CompressedReference`, `StackReference`,
  `GcRoot`: 681 lines use those types, 3,940 use `ObjPtr`, all CENTRAL);
  `ObjPtr`'s debug-build encoding, which packs a below-4 GiB pointer
  shifted by 3 with a thread cookie (`runtime/obj_ptr.h:91-98`,
  `obj_ptr-inl.h:54-64`, `DCHECK_LE(ref, 0xFFFFFFFFU)`); the forwarding
  address the moving collector stores in the 32-bit lock word
  (`lock_word.h:151-154`, `lock_word-inl.h:44-47`); the class table slot, a
  32-bit class pointer with hash bits (`class_table-inl.h:124-131`).
- **R4, 15, and R5, 24: casts outside the types.** A dex register or
  argument slot turned into an object with a plain cast (stack walking
  `stack.cc:186, 253`, deoptimization `quick_exception_handler.cc:526`,
  lock owners `monitor.cc:1522`, the unstarted runtime's 8 argument reads,
  the string builder), and objects truncated to 32 bits (interpreter
  argument arrays, hprof object ids, the heap's boot image range; the
  `reinterpret_cast32` and `PointerToLowMemUInt32` ones DCHECK that the
  pointer is below 4 GiB). `stack.cc:255` is an explicit
  `DCHECK_LT(ptr_out, UINT32_MAX)`.
- **R7, 14: explicit bounds.** `LOW_MEM_START`, `4 * GB` in the allocator,
  `MapInternal`'s `(addr >> 32) != 0`, the card table over `[4 KiB, 4 GiB)`
  (`gc/heap.cc:629-631`).
- **R8, 6; R9, 14; R10, 5: the boot image.** `ImageHeader` stores
  addresses as `uint32_t` and returns them zero-extended
  (`image.h:155-195`); relocation runs in 32-bit arithmetic through
  `reinterpret_cast32` (`gc/space/image_space.cc:2669-2745, 2790-2801,
  2989-2991`); the reservation is at `ART_BASE_ADDRESS` (0x70000000) plus a
  random delta (`:2583-2584`). `Heap::IsBootImageAddress` subtracts a
  32-bit start from a 64-bit pointer (`gc/heap.h:714-716`).
- CENTRAL, no edit: the 14 `low_4gb` mappings (R6) once MemMap serves the
  window, and JNI (R11, 576 lines of `Decode<>`/`AddLocalReference<>`):
  native code only ever sees indirect references and handle-scope
  addresses.

### arm64 optimizing compiler (93 EDIT, 77 CONSUMER)

The heap-poisoning hooks mark heap loads and stores, as SteamARM's stage 25
guessed (VERIFIED IN SOURCE: `MaybeUnpoisonHeapReference`, 24 call sites,
C1; stores, 6, C2). They are not enough on their own; what bypasses them:

- **C3, 5: GC roots** (a method's declaring class, `.bss` class and string
  entries, JIT root tables), loaded by `GenerateGcRootFieldLoad`
  (`code_generator_arm64.cc:5980-6051`): "GC roots are not affected by
  heap poisoning".
- **C4, 16, and C5: Baker read barriers.** The field, array, acquire and
  CAS loads (`:6053-6248`) emit their `neg` for poisoning inside a fixed-size
  scope whose layout the thunks and the introspection entrypoint decode;
  the GC-root thunk dereferences the root it was given (`:6502-6527`).
- **C6, 11: 32-bit values that are addresses:** `.data.bimg.rel.ro`
  entries (boot image classes, strings and `ArtMethod*`), JIT literals of
  boot image objects and of JIT root-table slots
  (`DeduplicateBootImageAddressLiteral`, `PatchJitRootUse`, both with a
  checked 32-bit cast).
- **C7, 3: Thread fields that hold objects,** read or cleared as 32 bits:
  `LoadException` loads the low half of `Thread::exception_`,
  `ClearException` stores `wzr` into a 64-bit field (only the low half),
  `Thread.currentThread()` loads the low half of the peer.
- **C8, 15: address arithmetic on a W reference register:** array element
  addresses (`AcquireSameSizeAs(array)` then a 32-bit `Add`), far and
  acquire Baker field bases, `Unsafe.get*`, SIMD addresses, and card marking
  (`MarkGCCard` shifts the W register to index the card table). The
  `HIntermediateAddress` node is an `int32` value that the register
  allocator may spill as 32 bits.
- **C9, 3: moves.** `MoveLocation` moves a reference W to W, and the
  parallel move resolver moves untyped stack slots as 32 bits
  (`:1059-1062, 1400-1551`); `HSelect` of references is a W `csel`.
- **C14, 11: intrinsics** that handle references (Unsafe get/put/CAS,
  `String.equals/compareTo`, `System.arraycopy`, `Integer.valueOf`,
  `Thread.currentThread`); most go through the hooks above.
- CONSUMER, 77: the 61 dereferences through `HeapOperand` and 16
  read-barrier slow paths and mark entrypoints.

### arm64 runtime assembly (17 EDIT)

`runtime/arch/arm64/quick_entrypoints_arm64.S`: the invoke stubs load `this`
and every `L` argument as W registers ("Everything else takes one vReg",
`:546, 679`); `READ_BARRIER` in `art_quick_aput_obj` (`:1059-1103`); the
component type in array allocation (`:1597`); the IMT conflict trampoline
(`:1713-1717`); `READ_BARRIER_MARK_REG` (`:2216-2305`, a W move of the
runtime's result and the forwarding address from the lock word); the mark
introspection entrypoint (`:2408-2560`). Card marking already uses the full
address (`lsr x0, x0, #CARD_TABLE_CARD_SHIFT`, CONSUMER).

### mterp (20 EDIT, heuristic)

The arm64 assembly interpreter loads a dex register with `GET_VREG w` and
uses the X register as a base (11 lines: iget/iput fast paths,
array-length, aget/aput) or passes it to C++ helpers taking a
`mirror::Object*` (6: `artIGetObjectFromMterp`, `artAGetObjectFromMterp`,
`MterpFillArrayData`, `artLockObjectFromCode`, ...); `throw` stores it into
`Thread::exception_` (2). The switch interpreter (C++) needs nothing, and
arm64 has no nterp in Android 11 (`runtime/Android.bp:296-309`,
`nterp_stub.cc`).

### JNI (7 EDIT) and what needs nothing

JNI stubs store reference arguments into the handle scope with a 32-bit
store and pass the slot's address as the `jobject`
(`jni_macro_assembler_arm64.cc:574-600`); a returned reference comes from
`artJniMethodEndWithReference` as a full pointer. `LoadRef` and an unused
`LoadReferenceFromHandleScope` are the 7 lines. Native code is not
affected: it sees handles, and `Get*Critical` pointers point into objects,
which may be anywhere in its address space anyway.

Verified to need nothing: the generic JNI trampoline, the
quick-to-interpreter bridge, the resolution trampoline and proxies read
arguments from spilled registers through `StackReference` (the low half of
a little-endian 64-bit slot is the compressed reference, whether the
register held the compressed value or the full pointer); the GC updates
those slots through the same view, which keeps the base in the upper half;
the GC visits compiled frames' saved registers as `mirror::Object**`
(`thread.cc`, `VisitQuickFramePrecise`), which is right once registers hold
full pointers; the concurrent copying collector's region and card logic
work on addresses in any range.

### 64-bit references would touch (LAYOUT, 588 lines, and more)

96 lines size things with `kHeapReferenceSize` or `sizeof(HeapReference)`;
154 object-layout constants are shared with assembly
(`tools/cpp-define-generator/*.def`); `DataType::Type::kReference` appears
312 times across all optimizing back ends; a reference is one 32-bit dex
register (`kVRegSize`, 26), so shadow frames and the verifier change; the
image format changes; every object header and reference field grows.

## Designs

| | (a) poisoning hooks as add/sub of a base | (b) fixed base, dedicated register | **(B1) fixed base, ORR immediate, full pointers in registers (recommended)** | (B2) base added at each dereference | (c) 64-bit references |
|---|---|---|---|---|---|
| sites | ~30 hook sites; misses the other ~200 (GC roots, literals, moves, arithmetic, casts, image) | as B1 or B2, plus register allocation | 232 lines (EDIT) | 232 minus the load sites, plus every dereference: 61 `HeapOperand` + other X-view addresses, assembly and mterp dereferences, every object argument to the runtime, the GC's register visitor | XL: 588 layout lines, every back end, verifier, image format |
| cost per load | 1-2 | 1 (`add x, xB, w, uxtw`) but no null handling | 1 (non-null) or 2 (`cbz; orr`) | 0 | 0 |
| cost per dereference | 0 | 0 | 0 | 1 `orr` and a temporary | 0 |
| register | Thread load or a reserved one | one callee-save lost (ART already reserves x16, x17, x18, x19 = TR, x20 = MR) | none | none | none |
| memory | same | same | same | same | references double; +10-30% heap (HYPOTHESIS) |
| images | rebuild | rebuild | upstream images relocate for the interpreter; rebuild for compiled code | same as B1 | new format |
| verdict | a mechanism of B1, not a design | not needed: the base is a compile-time single bit | **go** | more sites and a cost on every access | no-go |

Registers, VERIFIED IN SOURCE: `runtime/arch/arm64/registers_arm64.h:63-64`
(`TR = X19`, `MR = X20`), `code_generator_arm64.cc:1288-1304` (lr, tr, mr,
ip0, ip1 and x18 blocked). A base register would cost one of the eight
callee-saves left to the allocator (x21-x28); an immediate costs nothing, because any
single bit from 2^32 to 2^46 is an AArch64 logical immediate (VERIFIED:
assembled, `b25a0042 orr x2, x2, #0x4000000000`, stage 28 record).

(d) Other routes, not chosen: an arm64-to-arm64 translator with an address
offset (FEX-like; large and slow, `docs/ANDROID_RUNTIME_ARCHITECTURE.md`);
trapping every access (millions of faults a second); x86_64 ART under FEX
with an arm64 native bridge (FEX's low window gives guest addresses that
differ from host addresses, so every pointer crossing JNI would need
translation; large, HYPOTHESIS); a VM (excluded, `AGENTS.md`).

## The recommended design (B1)

**The window.** Base `B = 0x4000000000` (256 GiB, bit 38); the window is
`[B, B + 4 GiB)`; nothing is mapped in `[B, B + 64 KiB)`, as the low 64 KiB
upstream. Why there (HYPOTHESIS for Darwin's placement, VERIFIED IN SOURCE
for lxrun's): Darwin fills a process from the bottom (the executable just
above `__PAGEZERO`, the dyld shared cache, malloc; lxrun's comment: a guest
"gets its libraries at ~5 GB", `runtime/dispatch.c:950-951`), lxrun places
hint-less guest mappings top-down under `0x7f0000000000` (`:957`) and FEX's
low window at `0x8000000000` (512 GiB, FEX processes only). 256 GiB is
clear of all of them and below 2^39, so the same build also runs on a
Linux arm64 kernel with 39-bit addresses (a test bed that is not the Mac).
ART's own layout inside the window is upstream's: the boot image at
`B + 0x70000000 +/- 16 MiB`, the heap after it, randomized as upstream.

**Invariants.**

- In memory a reference is the low 32 bits of its address. Every 32-bit
  store of a pointer is already the compression.
- In C++ and in compiled code's core registers a reference is the full
  pointer; null is 0. `PtrCompression::Decompress` returns
  `ref ? (B | ref) : 0`; compiled code decompresses after each load with
  `cbz w, 1f; orr x, x, #B; 1:` (one `orr` when the value cannot be null:
  a class, a `.data.bimg.rel.ro` entry, a JIT literal).
- Decompression is idempotent on a full pointer, and a W view of a full
  pointer is the compressed reference, so comparisons (`cmp w`), null checks
  (`cbz w`) and 32-bit stores are unchanged.
- Implicit null checks still fault in `__PAGEZERO` (null stays 0).

**Baker read barriers without new layouts.** The decompression goes after
the load's return label, where the not-gray path (the LDR), the
introspection entrypoint (a W move) and the forwarding path all arrive, so
the thunks, the introspection offsets and
`BAKER_MARK_INTROSPECTION_*_LDR_OFFSET` stay as they are. The GC-root thunk
decompresses the root before reading its lock word; the introspection
entrypoint keeps the reference compressed in IP0 (its return switch stores
`ref_reg*8` in IP0's high word) and reads the lock word through IP1; its
field slow path moves out of line to keep the 256-byte budget before the
array switch.

**The first stage needs no compiler and no new boot image.**
VERIFIED IN SOURCE: with `-Xint` the runtime gives every non-native boot
image method the interpreter bridge (`class_linker.cc:2118-2130`,
`runtime.cc:1345-1346`, before the heap is created at `:1381`). Native
methods keep the image's compiled JNI stubs, which only store the low
halves of references into the handle scope. The boot image's references
are the low halves of its addresses whatever the base, and its relocation
already runs in 32-bit arithmetic, so an upstream boot image relocates
into the window as it is when the relocation ORs `B` into every result
(patch 0005). With mterp off (patch 0006) the switch interpreter reads dex
registers through `StackReference`. So the first stage is the runtime
(patches 0001-0007) plus the upstream `javalib`, interpreted.

**Compiled code must match.** An oat file records the base its code
decompresses with (`heap-reference-base` in the oat header, written by
dex2oat for arm64 when non-zero); a mismatch refuses the boot image's oat
file and makes an app's oat file unusable, unless the runtime is
interpret-only (patch 0005).

## Risks

1. **A missed site.** The list comes from patterns and reading; the
   compiler's 32-bit address arithmetic is the likeliest place to miss one.
   On the Mac a miss is loud: a zero-extended reference used as an address
   is below 4 GiB, in `__PAGEZERO`, and faults at once with the compressed
   value as the fault address; a 64-bit field cleared with a 32-bit store
   keeps `B` in its upper half and faults in the unmapped
   `[B, B + 64 KiB)`. A silent miss needs a 64-bit comparison between a
   full and a compressed value; compiled code compares references as W
   registers (HYPOTHESIS that none remains).
2. **Debuggers.** JVMTI/JDWP writing a reference into a compiled frame's
   register goes through debugger shadow frames (interpreter), UNKNOWN in
   detail.
3. **App images** are disabled (their relocation compares full addresses
   with 32-bit bounds, `image_space.cc:1385-1560`): app start-up is slower
   until ported.
4. **The window must stay free.** Darwin and lxrun do not map there today
   (HYPOTHESIS for Darwin); a reservation of the window by lxrun at start
   for Android roots is a small runtime change, not made.
5. **Performance**, below. **The JIT** under lxrun (dual-view code cache)
   is a separate open item (`docs/ANDROID_ZERO_VM_FEASIBILITY.md` 3.2).
6. **ABI of the rebuilt module**: built from the same LineageOS 18.1 tree
   as the image (HYPOTHESIS that a tree synced today matches the
   2025-06-28 image closely enough; the ART APEX is self-contained apart
   from bionic).
7. The base is a build constant: all ART binaries, the boot image's code
   and every odex must come from one build.

## Performance (HYPOTHESIS, to be measured)

Per reference load: +1 instruction (non-null) or +2 (`cbz`, well
predicted, and `orr`); per reload of a spilled reference: +2; nothing per
dereference, comparison or store. HotSpot's compressed oops with a non-zero
heap base make the same trade on arm64 (decode after load, `cbz` for
nullable values); from that analogy, not measured here: a few percent on
reference-heavy code and a few percent of code size. `HIntermediateAddress`
is no longer extracted (a small cost in array loops). The first stage runs
only the switch interpreter: much slower than mterp, the JIT or AOT code
(stage 25 measured x86-64 `-Xint` under FEX at 156 ms/round on its `Loop`;
the arm64 switch interpreter is UNKNOWN). Measuring it is part of the test
plan.

## What must be rebuilt or regenerated

- Stage 1: the ART module's native code only (`bin/`, `lib64/` of
  `com.android.art.release`). The upstream boot image and odex files stay;
  their compiled code is not run (`-Xint`).
- Stage 3: the boot image (`/apex/com.android.art/javalib/arm64/boot*`,
  `/system/framework/arm64/boot-framework.*`) and every system and app odex,
  by the patched `dex2oat64`: in the Android build (dexpreopt), or on the
  Mac by that `dex2oat64` itself under lxrun once stage 1 works (it is
  arm64 and needs only the runtime), or by the runtime compiling a boot
  image into `/data/dalvik-cache` when the stored one is refused
  (HYPOTHESIS that Android 11's image generation path still does this).
  Apps: `cmd package compile -m speed -f -a` once PackageManager runs.

## Build and install

`patches/art-heap-base/README.md`: an Android 11 tree on an x86-64 Linux
host (not the Mac), `git am` of the series in `art/`,
`ART_HEAP_REFERENCE_BASE=0x4000000000 TARGET_FLATTEN_APEX=true m
com.android.art.release`, and the proposed `scripts/mkandroidroot.sh
--art-apex DIR` step (not implemented) that copies the module's `bin/` and
`lib64/` over `/system/apex/com.android.art.release` and
`/apex/com.android.art` in the arm64 root and clears its dalvik-cache.

## Test plan

1. Build with the variable unset; ART's host gtests pass (the series is
   inert at base 0).
2. Build with the base; on the Mac, arm64 root, `LXRT_GUEST_PAGE=4096`:
   `dalvikvm64 -Xint -Xusejit:false -cp hello.dex Hello` prints its line;
   `/proc/self/maps` shows ART's spaces in `[0x4000010000, 0x4100000000)`.
3. `tests/android/java/HeapRef.java` (stage 25) reads references back as
   32-bit values; a GC-heavy program (allocation loops, `System.gc()`,
   weak references) exercises the moving collector's forwarding addresses
   and the class table.
4. The same `Loop` as stage 25 with `-Xint`: time per round against the
   x86-64 root under FEX.
5. zygote64 with `dalvik.vm.extra-opts=-Xint` in the arm64 root (after
   porting `scripts/android-boot.py` to it).
6. Stage 3: `dex2oat64` compiles hello.dex; the odex's code runs; a boot
   image compiled with the base; JIT on. Any fault at an address below
   4 GiB is a missed decompression: its PC names the site.
7. Optional, not a VM: the same module on a Linux arm64 machine with a
   39-bit address space, in a Waydroid container, to separate ART bugs
   from lxrun bugs.

## Go / no-go

- **GO, stage 1** (patches 0001-0007, upstream boot image, `-Xint`,
  switch interpreter): +368/-72 lines of C++ and assembly in 31 files, no
  compiler change, no image regeneration, decisive: it shows whether an
  arm64 ART heap can live above 4 GiB at all. No blocker was found in the
  source.
- **Conditional GO, stage 3** (patches 0008-0009, regenerated images, JIT):
  bounded (93 compiler lines, mostly through hooks and a few helpers) but
  only as good as the site list; start it after stage 1 passes, and rely on
  `__PAGEZERO` faults to find what the list missed.
- **Optional, stage 2**: port mterp (20 sites) for interpreter speed.
- **NO-GO**: 64-bit references (design C), trapping, a VM.

Overall: FEASIBLE as a HYPOTHESIS, not demonstrated. The next step is the
stage 1 build on a Linux host and test 2 on the Mac.

## Open questions

- Whether Darwin ever places memory at 256 GiB in a long-running process
  (maps of lxrun guests after hours of use).
- The arm64 switch interpreter's speed under lxrun.
- Which of the upstream boot image's `@FastNative` and `@CriticalNative`
  stubs run with `-Xint` (all are base-agnostic by reading; not run).
- Whether Android 11 regenerates a refused boot image in
  `/data/dalvik-cache` (stage 3 shortcut).
