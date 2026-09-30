#!/usr/bin/env python3
"""Count, in an Android 11 ART source tree, the places where a 32-bit heap
reference becomes a pointer or back (docs/ART_HEAP_ABOVE_4GB.md).

  patches/art-heap-base/count_sites.py ART_DIR            the summary table
  patches/art-heap-base/count_sites.py --list ART_DIR     plus file:line of every hit
  patches/art-heap-base/count_sites.py --json ART_DIR     machine-readable

ART_DIR is a checkout of LineageOS android_art (lineage-18.1) or AOSP
platform/art (android11-release). Tests are left out (paths under test/ and
files named *_test.*). Pure Python 3.8+, standard library only.

Every category is a regular expression over source lines (a few are small
per-block scans of the assembly interpreter), so the counts are exact for
what the pattern says and approximate for what the category means; the
--list output is what the study reviewed by hand. The "action" column says
what the recommended design (docs/ART_HEAP_ABOVE_4GB.md, design B1) does:

  EDIT      the line (or the helper it names) is changed by that design
  CENTRAL   no edit: it follows one central change (the reference types,
            MemMap's window)
  CONSUMER  no edit under B1, because registers then hold full pointers; a
            design that adds the base at every dereference edits each one
  NONE      stays as is: a 32-bit store of the low half is the compression
  LAYOUT    what 64-bit references (design C) would have to change
  EVIDENCE  comments and asserts that state the below-4 GiB assumption
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

# ------------------------------------------------------------------ files

RUNTIME_CXX = ["runtime/**/*.cc", "runtime/**/*.h", "libartbase/**/*.cc",
               "libartbase/**/*.h", "openjdkjvmti/**/*.cc", "openjdkjvmti/**/*.h",
               "openjdkjvm/**/*.cc", "adbconnection/**/*.cc", "perfetto_hprof/**/*.cc"]
ALL_CXX = RUNTIME_CXX + ["compiler/**/*.cc", "compiler/**/*.h", "dex2oat/**/*.cc",
                         "dex2oat/**/*.h", "libdexfile/**/*.cc", "libdexfile/**/*.h"]
A64_COMPILER = ["compiler/optimizing/code_generator_arm64.cc",
                "compiler/optimizing/code_generator_arm64.h",
                "compiler/optimizing/code_generator_vector_arm64.cc",
                "compiler/optimizing/intrinsics_arm64.cc",
                "compiler/optimizing/intrinsics_arm64.h",
                "compiler/optimizing/common_arm64.h",
                "compiler/optimizing/instruction_simplifier_arm64.cc",
                "compiler/utils/arm64/assembler_arm64.cc",
                "compiler/utils/arm64/assembler_arm64.h"]
A64_ASM = ["runtime/arch/arm64/*.S"]
MTERP = ["runtime/interpreter/mterp/arm64/*.S"]
DEX2OAT = ["dex2oat/**/*.cc", "dex2oat/**/*.h"]


def is_test(path):
    base = os.path.basename(path)
    return ("/test/" in "/" + path or base.endswith(("_test.cc", "_test.h"))
            or "_test_" in base or base.startswith("test_") or "/gtest" in path)


def expand(root, globs):
    """Files under root matching any glob (** matches any depth)."""
    out = set()
    for g in globs:
        if "**" in g:
            top, pat = g.split("/**/", 1)
            for d, _, files in os.walk(os.path.join(root, top)):
                for f in files:
                    if fnmatch.fnmatch(f, pat):
                        out.add(os.path.relpath(os.path.join(d, f), root))
        else:
            d = os.path.join(root, os.path.dirname(g))
            if os.path.isdir(d):
                for f in os.listdir(d):
                    if fnmatch.fnmatch(f, os.path.basename(g)):
                        out.add(os.path.relpath(os.path.join(d, f), root))
    return sorted(p for p in out if not is_test(p))


def tree_id(root):
    """The tree's directory name and git commit (no absolute paths in records)."""
    try:
        sha = subprocess.run(["git", "-C", root, "rev-parse", "--short=12", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        sha = "not a git checkout"
    return f"{os.path.basename(root.rstrip('/'))} ({sha})"


def read_lines(root, rel, cache={}):
    if rel not in cache:
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
            cache[rel] = f.read().split("\n")
    return cache[rel]


# ------------------------------------------------------------- categories
# (id, group, action, description, globs, regex, exclude-regex or None,
#  only-these-files or None, skip-these-files or None)

C = []


def cat(cid, group, action, what, globs, rx, exclude=None, only=None, skip=None,
        within=None):
    """within: a regex for a function header; only lines from that header to
    the next line that is exactly "}" count."""
    C.append(dict(id=cid, group=group, action=action, what=what, globs=globs,
                  rx=re.compile(rx), exclude=re.compile(exclude) if exclude else None,
                  only=only, skip=skip or [], within=re.compile(within) if within else None))


def is_comment(line):
    """A whole-line comment: // ..., a block-comment continuation (* ...), or
    a /* ... line with no code after its */ (so /*low_4gb=*/ true, counts)."""
    s = line.strip()
    if s.startswith("//") or (s.startswith("*") and not s.startswith("*/")):
        return True
    if s.startswith("/*"):
        end = s.find("*/")
        return end < 0 or not s[end + 2:].strip()
    return False


# --- the runtime (C++)
cat("R1", "runtime C++", "EDIT",
    "central encode/decode of a reference (PtrCompression, ObjPtr debug encoding, "
    "LockWord forwarding address, ClassTable slot)",
    RUNTIME_CXX,
    r"static (uint32_t|MirrorType\*) (Compress|Decompress)\(MirrorType\*|static MirrorType\* Decompress\(|"
    r"OBJPTR_INLINE MirrorType\* PtrUnchecked|ObjPtr<MirrorType>::Encode\(MirrorType|"
    r"static LockWord FromForwardingAddress|LockWord::ForwardingAddress\(\) const \{|"
    r"TableSlot::ExtractPtr\(uint32_t|TableSlot::Encode\(ObjPtr")
cat("R2", "runtime C++", "CENTRAL",
    "typed 32-bit references: HeapReference, CompressedReference, StackReference, GcRoot",
    ALL_CXX, r"\b(HeapReference|CompressedReference|StackReference|GcRoot)<")
cat("R3", "runtime C++", "CENTRAL", "ObjPtr<> (a full pointer in release builds)",
    ALL_CXX, r"\bObjPtr<")
cat("R4", "runtime C++", "EDIT",
    "uint32 (vreg, argument slot) turned into an object pointer outside the typed references",
    RUNTIME_CXX,
    r"reinterpret_cast32<\s*mirror::\w+\s*\*\s*>|"
    r"reinterpret_cast<mirror::\w+\*>\((value|val)\)|"
    r"reinterpret_cast<mirror::\w+\*>\(static_cast<uintptr_t>\(\*val\)\)",
    skip=["runtime/gc/space/image_space.cc"])
cat("R5", "runtime C++", "EDIT",
    "object pointer truncated to uint32 outside the typed references (the truncation is the "
    "compression; debug checks refuse a high base)",
    RUNTIME_CXX,
    r"reinterpret_cast32<u?int32_t>\(|PointerToLowMemUInt32\(|"
    r"(static_cast|dchecked_integral_cast)<u?int32_t>\(\s*reinterpret_cast<uintptr_t>",
    exclude=r"static inline uint32_t PointerToLowMemUInt32|inline Dest reinterpret_cast32",
    skip=["runtime/gc/space/image_space.cc", "runtime/imtable.h",
          "runtime/imt_conflict_table.h", "runtime/mirror/dex_cache-inl.h",
          "runtime/mirror/object.h", "libartbase/base/casts.h"])
cat("R5b", "runtime C++", "NONE",
    "32-bit native pointers of 32-bit images (PointerSize::k32 paths, not taken by a 64-bit runtime)",
    ["runtime/imtable.h", "runtime/imt_conflict_table.h", "runtime/mirror/dex_cache-inl.h",
     "runtime/mirror/object.h"],
    r"reinterpret_cast32<")
cat("R6", "runtime C++", "CENTRAL",
    "heap, image, JIT and LOS mappings requested in the low 4 GiB (low_4gb = true): served by "
    "MemMap's window instead",
    RUNTIME_CXX, r"low_4gb\s*=\s*\*/\s*true|/\*\s*low_4gb\s*\*/\s*true|low_4gb=\*/ true")
cat("R7", "runtime C++", "EDIT",
    "explicit below-4 GiB bounds and width checks on addresses",
    RUNTIME_CXX,
    r"\b4U? ?\* ?GB\b|>>\s*32\)\s*!=\s*0|DCHECK_LE\((ref|intp), 0xFFFFFFFFU\)|"
    r"DCHECK_LT\(ptr_out, std::numeric_limits<uint32_t>::max\(\)\)|\bLOW_MEM_START\b|\bMAP_32BIT\b",
    skip=["libartbase/base/mem_map_fuchsia.cc"])
cat("R8", "runtime C++", "EDIT",
    "32-bit address fields read back as pointers (ImageHeader, Heap boot image range)",
    ["runtime/image.h", "runtime/gc/heap.h"],
    r"reinterpret_cast<uint8_t\*>\((image_begin_|oat_file_begin_|oat_data_begin_|oat_data_end_|"
    r"oat_file_end_)\)|- boot_images_start_address_")
cat("R9", "runtime C++", "EDIT",
    "boot image relocation done in 32-bit address arithmetic (reinterpret_cast32)",
    ["runtime/gc/space/image_space.cc"], r"reinterpret_cast32<")
cat("R10", "runtime C++", "EDIT",
    "boot image base address used by the runtime (ART_BASE_ADDRESS: reservation, relocation delta)",
    RUNTIME_CXX, r"\bART_BASE_ADDRESS\b")
cat("R10b", "runtime C++", "NONE",
    "boot image base address in the build (the image keeps its low-half layout; the runtime adds "
    "the base when it maps it)",
    ["build/*.go", "build/*.mk"], r"LIBART_IMG_\w+_BASE_ADDRESS")
cat("R11", "runtime C++", "CENTRAL",
    "JNI: references reach native code only as indirect references or handles",
    RUNTIME_CXX, r"\bsoa\.Decode<|\bDecodeJObject\(|\bAddLocalReference<")

# --- the arm64 optimizing compiler
cat("C1", "arm64 compiler", "EDIT",
    "heap-reference load hooks (Unpoison): become 'decompress' in the hook",
    A64_COMPILER, r"MaybeUnpoisonHeapReference\(|[^e]UnpoisonHeapReference\(|__ neg\(ref_reg",
    exclude=r"void Arm64Assembler::|void (Maybe)?UnpoisonHeapReference\(|^\s*UnpoisonHeapReference\(reg\);")
cat("C2", "arm64 compiler", "NONE",
    "heap-reference store hooks (Poison): the 32-bit store of the low half is the compression",
    A64_COMPILER, r"MaybePoisonHeapReference\(|[^n]PoisonHeapReference\(",
    exclude=r"void Arm64Assembler::|void (Maybe)?PoisonHeapReference\(|^\s*PoisonHeapReference\(reg\);")
cat("C3", "arm64 compiler", "EDIT",
    "GC root loads (ArtMethod declaring class, .bss, JIT roots): one helper, decompress at its end",
    A64_COMPILER, r"GenerateGcRootFieldLoad\(",
    exclude=r"void CodeGeneratorARM64::|^\s*void Generate")
cat("C4", "arm64 compiler", "EDIT",
    "Baker read-barrier reference loads (field, array, acquire, CAS old value): decompress after "
    "the return label",
    A64_COMPILER,
    r"Generate(Field|Array)LoadWithBakerReadBarrier\(|GenerateUnsafeCasOldValueMovWithBakerReadBarrier\(",
    exclude=r"void CodeGeneratorARM64::|^\s*void Generate")
cat("C5", "arm64 compiler", "EDIT",
    "Baker read-barrier thunk kinds (the GC-root thunk dereferences the loaded root)",
    ["compiler/optimizing/code_generator_arm64.cc"], r"case BakerReadBarrierKind::k\w+: \{|"
    r"case BakerReadBarrierKind::kField:$",
    within=r"^void CodeGeneratorARM64::CompileBakerReadBarrierThunk\(")
cat("C6", "arm64 compiler", "EDIT",
    "32-bit literals and table slots holding addresses (boot image objects, JIT root slots, "
    ".data.bimg.rel.ro, .bss)",
    A64_COMPILER,
    r"Ldr\([^;]*Deduplicate(BootImageAddress|JitString|JitClass)Literal\(|"
    r"EmitLdrOffsetPlaceholder\(\w+, (\w+\.W\(\)|root_reg|WRegisterFrom)|"
    r"= dchecked_integral_cast<uint32_t>\(address\)")
cat("C7", "arm64 compiler", "EDIT",
    "Thread fields holding objects read or cleared as 32 bits (exception, peer)",
    A64_COMPILER, r"GetExceptionTlsAddress\(\)\)|PeerOffset<")
cat("C8", "arm64 compiler", "EDIT",
    "address arithmetic on a W reference register (32-bit add/shift of obj, array, card index)",
    A64_COMPILER,
    r"AcquireSameSizeAs\((obj|array|base)\)|__ Add\(base, obj,|__ Add\(temp, base, offset\.W\(\)\)|"
    r"__ Lsr\(temp, object, gc::accounting::CardTable::kCardShift\)|"
    r"void InstructionCodeGeneratorARM64::VisitIntermediateAddress\(HIntermediateAddress|"
    r"TryExtract(Vec)?ArrayAccessAddress\(",
    exclude=r"Register new_base = temps\.AcquireSameSizeAs\(base\)")
cat("C9", "arm64 compiler", "EDIT",
    "moves and selects of reference registers (W moves drop the base; 32-bit spill slots reload)",
    A64_COMPILER,
    r"void CodeGeneratorARM64::MoveLocation\(|void ParallelMoveResolverARM64::EmitMove\(|"
    r"void InstructionCodeGeneratorARM64::VisitSelect\(")
cat("C10", "arm64 compiler", "CONSUMER",
    "dereferences through a reference register (HeapOperand)", A64_COMPILER,
    r"\bHeapOperand(From)?\(", exclude=r"inline vixl::aarch64::MemOperand HeapOperand")
cat("C11", "arm64 compiler", "CONSUMER",
    "read-barrier slow paths and mark entrypoints (the runtime gets full pointers)", A64_COMPILER,
    r"Generate(ReadBarrierSlow|ReadBarrierForRootSlow)\(|MaybeGenerateReadBarrierSlow\(|"
    r"ReadBarrierMarkEntryPointsOffset")
cat("C12", "arm64 compiler", "EVIDENCE",
    "register-width decision points (WRegisterFrom, RegisterFrom(..., kReference))", A64_COMPILER,
    r"\bWRegisterFrom\(|RegisterFrom\([^)]*kReference")
cat("C13", "arm64 compiler", "EVIDENCE",
    "comments stating that a reference is 32-bit and zero-extended", A64_COMPILER,
    r"(?i)zero-extend|zero extend|must be 32bit|fit in a W register|Heap reference = 32b")

# --- hand-written arm64 assembly of the runtime
cat("A1", "arm64 runtime asm", "EDIT",
    "UNPOISON_HEAP_REF after a reference load (the macro becomes 'decompress')", A64_ASM,
    r"^\s*UNPOISON_HEAP_REF\s")
cat("A2", "arm64 runtime asm", "NONE", "POISON_HEAP_REF before a 32-bit reference store",
    A64_ASM, r"^\s*POISON_HEAP_REF\s")
cat("A3", "arm64 runtime asm", "EDIT",
    "32-bit loads of reference fields (class, component type, declaring class, READ_BARRIER)",
    A64_ASM,
    r"ld[rp]\s+\\?w\w+,.*\[.*(OBJECT_CLASS_OFFSET|COMPONENT_TYPE_OFFSET|DECLARING_CLASS_OFFSET|"
    r"CLASS_DEX_CACHE_OFFSET|#\\offset\])")
cat("A4", "arm64 runtime asm", "EDIT", "forwarding address decoded from a lock word",
    A64_ASM, r"LOCK_WORD_STATE_FORWARDING_ADDRESS_SHIFT")
cat("A5", "arm64 runtime asm", "EDIT", "a returned reference moved as a W register",
    A64_ASM, r"mov\s+\\?w\w+,\s*w0\b")
cat("A6", "arm64 runtime asm", "EDIT",
    "invoke stubs load reference arguments (and 'this') as W registers", A64_ASM,
    r"Everything else takes one vReg|Load \"this\" parameter")
cat("A7", "arm64 runtime asm", "CONSUMER", "card marking from the object's full address",
    A64_ASM, r"CARD_TABLE_CARD_SHIFT")
cat("A8", "arm64 runtime asm", "EVIDENCE",
    "comments stating 'compress/uncompress = do nothing, already zero-extended'", A64_ASM,
    r"(?i)zero-extend|uncompress|\"Compress\"|Heap reference = 32b")

# --- the arm64 assembly interpreter (mterp); block scans below
cat("M1", "arm64 mterp", "EDIT", "UNPOISON_HEAP_REF after a reference load", MTERP,
    r"^\s*UNPOISON_HEAP_REF\s")
cat("M2", "arm64 mterp", "NONE", "SET_VREG_OBJECT (a 32-bit vreg store)", MTERP,
    r"^\s*SET_VREG_OBJECT\s")
cat("M5", "arm64 mterp", "EDIT", "Thread::exception_ written from a register", MTERP,
    r"str\s+x\d+,\s*\[xSELF, #THREAD_EXCEPTION_OFFSET\]")

# --- JNI stubs
cat("J1", "arm64 JNI stubs", "EDIT",
    "JNI macro assembler reference operations (LoadRef decompresses; handle-scope helpers)",
    ["compiler/utils/arm64/jni_macro_assembler_arm64.cc"],
    r"void Arm64JNIMacroAssembler::(LoadRef|CopyRef|CreateHandleScopeEntry|"
    r"LoadReferenceFromHandleScope|StoreRef)\(")
cat("J2", "arm64 JNI stubs", "CONSUMER",
    "JNI stub generator calls to those operations (ISA-independent)",
    ["compiler/jni/quick/jni_compiler.cc"],
    r"(->|__ )(LoadRef|StoreRef|CopyRef|CreateHandleScopeEntry|LoadReferenceFromHandleScope)\(")

# --- dex2oat and the oat/image formats
cat("I1", "dex2oat, oat, image", "EDIT",
    "dex2oat's 32-bit address conversions (image writer, oat writer)", DEX2OAT,
    r"PointerToLowMemUInt32\(|reinterpret_cast32<|"
    r"dchecked_integral_cast<uint32_t>\(\s*reinterpret_cast<uintptr_t>")
cat("I2", "dex2oat, oat, image", "CENTRAL",
    ".data.bimg.rel.ro filled with 32-bit boot image addresses at load (compressed values)",
    ["runtime/oat_file.cc"], r"GetBootImagesStartAddress\(\)|boot_image_begin \+")

# --- what 64-bit references would touch
cat("L1", "64-bit references (design C)", "LAYOUT",
    "sizes of the reference types used in layout code", ALL_CXX,
    r"\bkHeapReferenceSize\b|sizeof\((mirror::)?(HeapReference|StackReference|CompressedReference|"
    r"GcRoot)<")
cat("L2", "64-bit references (design C)", "LAYOUT",
    "object layout constants shared with assembly (ASM_DEFINE)",
    ["tools/cpp-define-generator/*.def"], r"^ASM_DEFINE\(")
cat("L3", "64-bit references (design C)", "LAYOUT",
    "DataType::Type::kReference in the optimizing compiler, all back ends",
    ["compiler/optimizing/*.cc", "compiler/optimizing/*.h"], r"DataType::Type::kReference")
cat("L4", "64-bit references (design C)", "LAYOUT",
    "kVRegSize (a reference is one 32-bit dex register)", ALL_CXX, r"\bkVRegSize\b")


# ------------------------------------------------------------------ scans

def scan_lines(root, c):
    hits = []
    files = c["only"] if c["only"] else expand(root, c["globs"])
    for rel in files:
        if rel in c["skip"] or not os.path.exists(os.path.join(root, rel)):
            continue
        inside = c["within"] is None
        for i, line in enumerate(read_lines(root, rel), 1):
            if c["within"] is not None:
                if c["within"].search(line):
                    inside = True
                elif inside and line == "}":
                    inside = False
            if not inside:
                continue
            if c["action"] != "EVIDENCE" and is_comment(line):
                continue
            if c["rx"].search(line) and not (c["exclude"] and c["exclude"].search(line)):
                if c["id"] in ("A1", "M1") and line.lstrip().startswith(".macro"):
                    continue
                hits.append((rel, i, line.strip()))
    return hits


GET_VREG = re.compile(r"^\s*GET_VREG\s+w(\d+),")
WRITES = re.compile(r"^\s*(?:[a-z][a-z0-9.]*)\s+[wx](\d+)\b")
DEREF = re.compile(r"\[x(\d+)\b")


def scan_mterp(root):
    """M3: a register loaded with GET_VREG (an object's compressed reference)
    and then used as the base of a memory operand before being overwritten.
    M4: such a register in x0-x3 when a C++ helper is called (bl)."""
    derefs, calls = [], []
    for rel in expand(root, MTERP):
        live = {}
        for i, line in enumerate(read_lines(root, rel), 1):
            s = line.split("//")[0]
            if s.startswith("%def ") or re.match(r"^\s*\.macro\b", s):
                live = {}
                continue
            m = GET_VREG.match(s)
            if m:
                live[m.group(1)] = i
                continue
            for r in DEREF.findall(s):
                if r in live:
                    derefs.append((rel, i, line.strip()))
                    break
            if re.match(r"^\s*bl\s", s):
                # Helpers that take no object (suspend checks, the branch
                # helpers of control_flow.S) are left out.
                if (any(r in live for r in ("0", "1", "2", "3"))
                        and not re.search(r"Suspend|\$func|MterpLog", s)):
                    calls.append((rel, i, line.strip()))
                live = {}
                continue
            w = WRITES.match(s)
            if w and not re.match(r"^\s*(str|stp|strb|strh|cmp|cbz|cbnz|tst|b\.)", s):
                live.pop(w.group(1), None)
    return derefs, calls


def scan_intrinsics(root):
    """C14: intrinsic code generators whose body handles references."""
    rel = "compiler/optimizing/intrinsics_arm64.cc"
    if not os.path.exists(os.path.join(root, rel)):
        return []
    lines = read_lines(root, rel)
    rx = re.compile(r"kReference|HeapReference|PoisonHeapReference|ReadBarrier|ClassOffset|"
                    r"MarkGCCard|LoadBootImageAddress|PeerOffset|\bHeapOperand")
    # Bodies of static helpers (GenUnsafeGet, GenCas, ...), so that a
    # generator that only calls one is judged by what the helper does.
    helpers, cur, buf = {}, None, []
    for line in lines:
        m = re.match(r"^static \w[\w:<>*& ]* (Gen\w+|Generate\w+)\(", line)
        if m:
            cur, buf = m.group(1), []
        if cur:
            buf.append(line)
            if line == "}":
                helpers[cur] = "\n".join(buf)
                cur = None
    out, i = [], 0
    while i < len(lines):
        m = re.match(r"^void IntrinsicCodeGeneratorARM64::(Visit\w+)\(", lines[i])
        if not m:
            i += 1
            continue
        start = i
        while i < len(lines) and lines[i] != "}":
            i += 1
        body = "\n".join(lines[start:i + 1])
        called = [helpers[h] for h in re.findall(r"\b(Gen\w+|Generate\w+)\(", body) if h in helpers]
        # A shared helper counts only for the generator that hands it a reference.
        if rx.search(body) or ("kReference" in body and any(rx.search(h) for h in called)):
            out.append((rel, start + 1, m.group(1)))
        i += 1
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("art_dir")
    ap.add_argument("--list", action="store_true",
                    help="print every hit (CENTRAL and LAYOUT categories only with --all)")
    ap.add_argument("--all", action="store_true", help="with --list: every category")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    root = os.path.abspath(a.art_dir)
    if not os.path.isfile(os.path.join(root, "runtime/mirror/object_reference.h")):
        sys.exit(f"{root}: not an ART source tree (no runtime/mirror/object_reference.h)")

    results = []
    for c in C:
        hits = scan_lines(root, c)
        results.append(dict(id=c["id"], group=c["group"], action=c["action"], what=c["what"],
                            count=len(hits), hits=hits))
    derefs, calls = scan_mterp(root)
    results.append(dict(id="M3", group="arm64 mterp", action="EDIT",
                        what="a vreg object (GET_VREG w) used as a memory base (heuristic)",
                        count=len(derefs), hits=derefs))
    results.append(dict(id="M4", group="arm64 mterp", action="EDIT",
                        what="a vreg object passed to a C++ helper in x0-x3 (heuristic)",
                        count=len(calls), hits=calls))
    intr = scan_intrinsics(root)
    results.append(dict(id="C14", group="arm64 compiler", action="EDIT",
                        what="intrinsic generators whose body handles references (review each)",
                        count=len(intr), hits=[(r, ln, n) for r, ln, n in intr]))
    order = {c["id"]: n for n, c in enumerate(C)}
    order.update({"M3": order["M2"] + 0.1, "M4": order["M2"] + 0.2, "C14": order["C13"] + 0.1})
    results.sort(key=lambda r: order[r["id"]])

    if a.json:
        json.dump(dict(art_tree=tree_id(root), categories=results), sys.stdout, indent=1)
        print()
        return
    print(f"ART tree: {tree_id(root)}\n")
    print("| id | group | action | count | what |")
    print("|---|---|---|---:|---|")
    for r in results:
        print(f"| {r['id']} | {r['group']} | {r['action']} | {r['count']} | {r['what']} |")
    tot = {}
    for r in results:
        tot[r["action"]] = tot.get(r["action"], 0) + r["count"]
    print("\nlines per action: " + ", ".join(f"{k} {v}" for k, v in sorted(tot.items())))
    if a.list:
        for r in results:
            if r["action"] in ("CENTRAL", "LAYOUT") and not a.all:
                continue
            print(f"\n## {r['id']} ({r['action']}) {r['what']}: {r['count']}")
            for rel, ln, text in r["hits"]:
                print(f"{rel}:{ln}: {text[:160]}")


if __name__ == "__main__":
    main()
