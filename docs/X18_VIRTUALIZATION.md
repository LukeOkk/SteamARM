# x18 on macOS, and how lxrun virtualises it

Linux aarch64 code treats x18 as an ordinary scratch register. macOS
reserves it, and zeroes it whenever the thread takes an exception. This page
covers what lxrun does about that, what it refuses, and what is still
unknown. Line numbers are at `dbd1657`.

Labels: MEASURED (a command and its result), VERIFIED IN SOURCE (file:line),
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN.

## The measurement

`benchmarks/stage5-x18.txt` (MEASURED, M4, macOS 27, native probe):

- x18 survives a plain function call. It is zeroed across a raw `svc #0x80`,
  a signal handler, `usleep`, and preemption: 444 losses in a 1.5 s busy loop
  with 16 spinning threads.
- Linux libraries use it: 326 instructions in `libc.so.6`, 251 in
  `libstdc++.so.6`, 27 in `ld-linux-aarch64.so.1`, 2271 in FEX before the
  fix.
- Every "small bogus pointer" crash in FEX's IR compiler recorded in stages
  5 and 6 was x18 read back as 0.

## The fix, in three parts

1. **FEX's JIT** does not allocate x18 (`patches/fex-lxrt-x18.patch`).
2. **FEX's own code** is built with `-ffixed-x18`
   (`scripts/build-fex-host.sh`).
3. **Every other Linux image** (glibc, ld.so, libstdc++, Valve's client,
   libcef) goes through the runtime's rewriter. Each instruction that names
   x18 becomes a branch to a trampoline. The trampoline loads the thread's
   virtual x18 from a TSD slot into a scratch register, runs the instruction
   with that register in place of x18, stores the result back and branches
   home. Preemption cannot touch the slot. The planner is `runtime/x18.c`;
   the integration is `runtime/rewrite.c`.

`br x18` / `blr x18` go through x16 instead, and a `blr` puts the guest
return address in x30 (VERIFIED IN SOURCE, `runtime/x18.c:273-289`).

Since stage 21, a load or store of x18 at a large `sp` offset
(`stp x18, x17, [sp, #0x1f8]`) is addressed through a scratch copy of `sp`
instead of being refused (`runtime/x18.c`, `sp_via_s2`; MEASURED in an
isolated test, `benchmarks/stage21-native-arm64-client.txt` change 4).

## What the planner refuses

`lxrt_x18_plan` rejects these forms (VERIFIED IN SOURCE,
`runtime/x18.c:265-272` and the `imm overflow` / `sp writeback` cases
below it):

| reason | form |
|---|---|
| `exclusive` | load/store-exclusive with x18 (`ldaxr w18`, `ldar w18` class) |
| `casp pair` | CASP with x18 in the pair |
| `writes sp` | an instruction whose destination is `sp` and that also names x18 |
| `sysreg` | MRS/MSR with x18, except the TPIDR_EL0 forms it emulates |
| `imm overflow` | the compensated immediate does not fit |
| `sp writeback` | SIMD load/store with writeback through `sp` |

## Which words are rewritten

- Only words inside the file's executable sections (`in_code`,
  `runtime/rewrite.c`).
- Since stage 21, only words inside an `.eh_frame` FDE range, when the file
  has FDEs (`runtime/elfsect.c:129-139`, `x18_site` in `runtime/rewrite.c`).
  The client's static OpenSSL keeps round constants in `.text`; rewriting
  them broke every TLS handshake ("http error 0"). With the filter the
  client downloads and verifies its 659 MB update (MEASURED, stage 21
  change 3).
- `LXRT_X18_ALL_TEXT=<path substring>` turns the filter off for matching
  files (`runtime/elfsect.c:290-298`). libcef needs it: it has x18 uses
  outside its FDEs (stage 21 change 5). No repository script sets it yet.
- `LXRT_NO_X18=1` is a diagnostic. With it the client's real x18 uses
  corrupted its package checksums (MEASURED, recorded at
  `runtime/elfsect.c:136-137`). It is not a usable control.

## Poisoned sites: `brk #1` since `dbd1657`

A refused site (`unsupported`), or one with no reachable trampoline
(`unreachable`), is overwritten with a poison word. The same happens to
unreachable `svc` and TLS sites (`runtime/rewrite.c:485, 493, 545`).

- **Since `dbd1657`** the poison is `brk #1` (`0xD4200020`,
  `runtime/rewrite.c:40-42`). Executing it raises SIGTRAP.
- **Up to 0.3.4** it was `0xD4000021`, which decodes as `svc #1`, not
  `brk #1` (MEASURED with llvm-mc, commit `dbd1657`). Darwin ignores the SVC
  immediate and dispatches on x16 (`benchmarks/stage1-syscall-cost.txt`). A
  poisoned site therefore ran whatever Darwin system call x16 held, silently.
- Runs before `dbd1657` that report poisoned, `unsupported` or
  `unreachable` sites ran with live `svc #1` words there. That includes the
  5 x18 sites per webhelper process in libcef traced on 2026-09-28
  (`docs/ARM64_REWRITE_COVERAGE.md`). Whether any of them executed is
  UNKNOWN. A run after `dbd1657` would show a SIGTRAP at that address if
  one does.
- `tests/elf/run.sh` passed 37/37 with the change (MEASURED on the Mac,
  commit `dbd1657`).

## Does macOS preserve x18 for old binaries?

xnu keeps x18 for tasks whose binary was built against an SDK older than
macOS 13 (VERIFIED IN SOURCE of xnu, `benchmarks/stage19-steamframe-base-and-arm64-limits.txt` §3a).

- MEASURED on a GitHub macos-15 runner (macOS 15.7.9, Apple M1, virtual):
  `tests/x18_preserve/run.sh` shows x18 zeroed for an SDK 15.5 build and
  preserved across preemption, a signal, `sched_yield` and `usleep` for an
  SDK 12.3 build (`benchmarks/stage20-ci-macos-runner.txt` §3).
- On the M4 under macOS 27: UNKNOWN. Measure with
  `tests/x18_preserve/run.sh`.
- If it holds there, linking lxrun against a pre-13 SDK would keep guest x18
  and make the rewriter optional (HYPOTHESIS). It would also remove one of
  the two ARM64 Proton blockers (the Windows TEB lives in x18); the
  `0x7ffe0000` low-address one stays (stage 19 §2).

## Diagnostics

- Images `lxrun` loads itself print one line each:
  `x18 N found in K code windows, N rewritten, N unsupported, N unreachable`
  (`runtime/main.c:207-216`).
- Code that ld.so maps later prints the same counts, only with tracing on
  (`runtime/dispatch.c:617-633`).
- The sub-page path (4 KiB-aligned libraries, libcef) prints found and
  rewritten only (`runtime/dispatch.c:1210-1214`). Its poisoned sites show
  only as the difference.
- With `LXRT_TRACE=1`, each refused site is named with its reason
  (`runtime/rewrite.c:486-490`).

## Tests

| test | covers |
|---|---|
| `tests/elf/run.sh` (x18_branch) | `br`/`blr`/`ret` through x18: 6 found, 6 rewritten, and the program runs |
| `tests/elf/run.sh` (x18_test) | x18 kept across a syscall, sleep, a signal and preemption, for every instruction form in the census; the same binary with `LXRT_NO_X18=1` must fail |
| `tests/x18_check.sh` | the planner's self-test over saved objdump listings; runs no guest |
| `tests/x18_preserve/run.sh` | whether macOS itself keeps x18 for a pre-13 SDK binary |

Not covered by a test at `dbd1657`: the FDE filter, a large `sp` offset
executed under lxrun, and a poisoned site trapping.
