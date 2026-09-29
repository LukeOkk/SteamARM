# x18 on macOS, and how lxrun virtualises it

Linux aarch64 code treats x18 as an ordinary scratch register. macOS
reserves it, and zeroes it whenever the thread takes an exception. This page
covers what lxrun does about that, what it refuses, and what is still
unknown. Line numbers are at `b3f64c8` (updated 2026-09-29 after the
planner changes `4534360`, `926b89a` and `c86b633`, the report change
`416d7b4` and stage 22).

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
return address in x30 (VERIFIED IN SOURCE, `runtime/x18.c:340-354`).

Since stage 21, a load or store of x18 at a large `sp` offset
(`stp x18, x17, [sp, #0x1f8]`) is addressed through a scratch copy of `sp`
instead of being refused (`runtime/x18.c`, `sp_via_s2`; MEASURED in an
isolated test, `benchmarks/stage21-native-arm64-client.txt` change 4).

Since stage 22 the planner also rewrites three forms it used to refuse
(VERIFIED IN SOURCE; each is executed by `tests/elf/run.sh` x18_forms and
planned by `build/x18_check --self-test`):

- LDAR/STLR and the other ordered, non-exclusive members of the
  load/store-exclusive group (bit 23 set): planned like any load or store
  (`c86b633`, `926b89a`; `runtime/x18.c:329-333`).
- MRS/MSR of NZCV, FPCR and FPSR with x18: run as themselves on a scratch
  register, since nothing in the trampoline touches flags or FP state
  (`4534360`, `926b89a`; `plain_sysreg`, `runtime/x18.c:257-263`).
- An instruction that computes `sp` from x18 (`mov sp, x18`, and add, sub,
  and, orr or eor into `sp`): its own trampoline, which never leaves the
  saved scratch pair below the live `sp` (`926b89a`; `plan_writes_sp`,
  `runtime/x18.c:268-322`, called at `:366-368`). The one case it cannot
  do safely, a misaligned `sp` whose new value lands less than 16 bytes
  above the saved pair, executes `brk #1` inside the trampoline at run
  time; it is not refused at load.

Dry run of the planner change (MEASURED, `build/lxrun --dry-run`, commit
`926b89a`): libcef.so with `LXRT_X18_ALL_TEXT=libcef.so` 76,048 found,
76,048 rewritten, 0 unsupported (was 76,043 + 5); libgallium-25.3.6.so
6,178/6,178 (was 6,177 + 1); steamclient.so 3,858/3,858 (was 3,856 + 2).

## What the planner refuses

`lxrt_x18_plan` rejects these forms, and only these (VERIFIED IN SOURCE at
`b3f64c8`: every `reject(p, ...)` call in `runtime/x18.c`):

| reason | form | where |
|---|---|---|
| `exclusive` | a real load/store-exclusive with x18 (LDXR, LDAXR, STXR, STLXR, LDXP, STXP: bit 23 clear). A trampoline between the load-exclusive and its store-exclusive could clear the exclusive monitor on every attempt and livelock the retry loop. None occurs in libcef's full text (`926b89a`) | `runtime/x18.c:333` |
| `casp pair` | CASP with x18 in the pair | `:334` |
| `sysreg` | MRS/MSR with x18 of any system register other than TPIDR_EL0 (emulated through the TSD slot), NZCV, FPCR and FPSR | `:335-337` |
| `imm overflow` | the TSD slot offsets or the site and trampoline alignment do not fit the plan, the compensated `sp` immediate of an add/sub does not fit, or an ADRP target is out of range | `:338-339`, `:404`, `:449` |
| `sp writeback` | a load/store that names x18 and writes back its `sp` base (pre/post-index, pair and SIMD forms) | `:378`, `:380`, `:408` |
| `unknown` | a form the classifier matched that the planner has no case for | `:475` |

There is no `writes sp` reason any more. Up to `926b89a` every instruction
whose destination was `sp` and that named x18 was refused with it.

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
  files (`runtime/elfsect.c:293-297`). libcef needs it: it has x18 uses
  outside its FDEs (stage 21 change 5). `scripts/run-steam-arm64.sh`
  exports `LXRT_X18_ALL_TEXT=libcef.so` unless it is already set, and both
  ARM64 Steam entries in `scripts/builtin-apps.json` set it (VERIFIED IN
  SOURCE).
- `LXRT_NO_X18=1` is a diagnostic. With it the client's real x18 uses
  corrupted its package checksums (MEASURED, recorded at
  `runtime/elfsect.c:136-137`). It is not a usable control.

## Poisoned sites: `brk #1` since `dbd1657`

A refused site (`unsupported`), or one with no reachable trampoline
(`unreachable`), is overwritten with a poison word. The same happens to
unreachable `svc` and TLS sites (`runtime/rewrite.c:528, 536, 588`).

- **Since `dbd1657`** the poison is `brk #1` (`0xD4200020`,
  `runtime/rewrite.c:40-42`). Executing it raises SIGTRAP.
- **Up to 0.3.4** it was `0xD4000021`, which decodes as `svc #1`, not
  `brk #1` (MEASURED with llvm-mc, commit `dbd1657`). Darwin ignores the SVC
  immediate and dispatches on x16 (`benchmarks/stage1-syscall-cost.txt`). A
  poisoned site therefore ran whatever Darwin system call x16 held, silently.
- Runs before `dbd1657` that report poisoned, `unsupported` or
  `unreachable` sites ran with live `svc #1` words there. That includes the
  5 x18 sites per webhelper process in libcef traced on 2026-09-28
  (`docs/ARM64_REWRITE_COVERAGE.md`).
- Poisoned sites do execute. Stage 22, with `lxrun` at `dbd1657`
  (MEASURED, `benchmarks/stage22-native-arm64-bringup.txt`): in E4 the
  webhelper's browser process ran into the `brk #1` on libcef's
  `mrs x18, nzcv` (refused as `sysreg`) 3-4 s after start; in E7, 500 of
  500 renderer deaths were at the poisoned `ldar w18, [x16]` (refused as
  `exclusive`). With `4534360` and `c86b633` rewriting both forms, a 45 s
  zygote trace had no `brk` traps left (E9). Whether libgallium's or
  steamclient.so's refused `mov sp, x18` sites ever ran was not measured;
  since `926b89a` they are rewritten.
- `tests/elf/run.sh` passed 37/37 with the change (MEASURED on the Mac,
  commit `dbd1657`). Since `9e7f4b9`, `x18_poison` checks that a refused
  site traps (rc 133, "SIGTRAP at pc ... insn 0xd4200020").

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
  (`runtime/main.c:251-260`).
- Code that ld.so maps later prints the same counts, and TLS counts, only
  with tracing on (`runtime/dispatch.c:632-640`).
- The sub-page path (4 KiB-aligned libraries, libcef) prints the same
  fields since `416d7b4`: unsupported, unreachable, code windows and TLS
  counts (`runtime/dispatch.c:1221-1229`). Before that it printed found and
  rewritten only, so its poisoned sites showed only as the difference.
- With `LXRT_TRACE=1`, each refused site is named with its reason
  (`runtime/rewrite.c:529-533`).

## Tests

| test | covers |
|---|---|
| `tests/elf/run.sh` (x18_branch) | `br`/`blr`/`ret` through x18: 6 found, 6 rewritten, and the program runs |
| `tests/elf/run.sh` (x18_test) | x18 kept across a syscall, sleep, a signal and preemption, for every instruction form in the census; the same binary with `LXRT_NO_X18=1` must fail |
| `tests/elf/run.sh` (x18_forms) | LDAR/STLR with x18, MRS/MSR of NZCV/FPCR/FPSR, and `sp` computed from x18 (grow, shrink, equal, a switch to another stack, misaligned both ways), executed; `LXRT_NO_X18=1` must fail it |
| `tests/elf/run.sh` (x18_spoff) | `stp`/`ldp`/`str`/`ldur` of x18 at large `sp` offsets, executed |
| `tests/elf/run.sh` (x18_fde) | the FDE filter leaves a `.text` table alone; with `LXRT_X18_ALL_TEXT` the table is rewritten and the program fails |
| `tests/elf/run.sh` (x18_callret) | nested `blr x18`, `br x18`, `ret x18` |
| `tests/elf/run.sh` (x18_poison) | a refused site (`ldaxr x18`) traps with `brk #1` |
| `tests/elf/run.sh` (X18_THREAD_ISOLATION) | nine threads keep their own x18 through 20,000 raw system calls each, with signals |
| `tests/x18_check.sh`; `build/x18_check --self-test` inside `tests/elf/run.sh` | the planner's self-test over saved objdump listings and plan words; runs no guest |
| `tests/x18_preserve/run.sh` | whether macOS itself keeps x18 for a pre-13 SDK binary |

The FDE filter, a large `sp` offset and a poisoned site trapping had no
test at `dbd1657`; they have one since `9e7f4b9`. MEASURED at `5d9760a`:
`tests/elf/run.sh` 64 passed, 0 failed, 0 expected failures.
