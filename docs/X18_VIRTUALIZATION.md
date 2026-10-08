# x18 on macOS, and how lxrun virtualises it

Linux aarch64 code treats x18 as an ordinary scratch register. macOS
reserves it, and zeroes it whenever the thread takes an exception. This page
covers what lxrun does about that, what it refuses, and what is still
unknown. Line numbers are at `b3f64c8` (updated 2026-09-29 after the
planner changes `4534360`, `926b89a` and `c86b633`, the report change
`416d7b4` and stage 22) unless a section names another commit. Stage 28
(2026-09-30, `benchmarks/stage28-keep-x18.txt`): lxrun is linked as SDK 12.3
by default so the kernel keeps x18 for JIT code, the rewriter stays (a
forked child loses the kernel's x18), and `br x18` no longer uses x16.

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

`blr x18` / `ret x18` go through x16 instead, which the procedure call
standard lets die at a call or a return, and a `blr` puts the guest return
address in x30 (VERIFIED IN SOURCE, `runtime/x18.c`, `lxrt_x18_plan`).

`br x18` did the same up to `7492467`, and that was the cause of V8's
TurboFan crash (stage 24): TurboFan dispatches a jump table with
`adr x18; add x18, x18, x0, lsl #2; br x18` while w16 is live, and read the
target address back as w16 (MEASURED by bisecting Heroic's rewritten sites
with the keep-x18 build, stage 28). Since `cc54e05` the `br x18` trampoline
keeps every general register (`plan_br`): it saves x16/x17, leaves
{its last word, the target} 32 bytes below sp, restores them, and ends with
`mrs x18, tpidrro_el0; and x18, x18, #~7; ldr x18, [x18, #slot]; br x18`.
Where the kernel zeroes x18 in the middle of that tail, `runtime/signal.c`
recovers: a fault at the `ldr` restarts the trampoline from its first word;
a branch to 0 resumes at the marked target when the mark names a real
trampoline and equals the virtual x18; a guest signal arriving on the tail
restarts the trampoline before its frame covers the mark (MEASURED:
X18_JUMP_TABLE forces both cases; `LXRT_X18_STATS=1` counts them at exit,
5-12 restarts per 48M dispatches on the current-SDK build). Residual
(HYPOTHESIS, not observed): a guest's own jump to address 0 in the same frame
right after such a dispatch, with the virtual x18 unchanged, would be taken
for the second case.

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
  `runtime/elfsect.c:136-137`). It is not a usable control. With the
  SDK-12.3 build (stage 28) it runs single processes correctly, but not
  forked children: Steam's and Heroic's forked GPU processes died.

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
- MEASURED on the M4 under macOS 27.0 (26A428), stage 24
  (`benchmarks/stage24-minecraft-prism.txt`): the same. The SDK 27.0 build
  sees x18 changed by preemption in all 8 threads and zeroed after a signal
  handler, `sched_yield` and `usleep`; the SDK 12.3 build ("LC_BUILD_VERSION
  sdk 12.3") keeps `0x5a18c0de12345678` through 36,000-42,000 preemption
  loops per thread, the signal handler, `sched_yield` and `usleep`.
- Stage 24 added `make lxrt LXRT_KEEP_X18=1` to link lxrun that way.
  MEASURED with it: `tests/elf/run.sh` 69/0, its two `LXRT_NO_X18` controls
  now see x18 survive without the rewriter (they check the build and expect
  that); and the JIT code this runtime never rewrites becomes correct: Linux
  HotSpot's C1/C2 (0 wrong of 800 in 9 runs, against 474-741 wrong and
  crashes with the current-SDK build) and llvmpipe's LLVM JIT (5 of 5 runs of
  3000 frames, against 0 of 10).
- **Since stage 28 it is the default** (`LXRT_KEEP_X18 ?= 1` in the
  `Makefile`; `make lxrt LXRT_KEEP_X18=0` links against the current SDK, and
  switching rebuilds lxrun). MEASURED, `benchmarks/stage28-keep-x18.txt`: both
  builds pass tests/elf (83/0 against 82/0 + the X18_JIT expected failure),
  run_i386 18/0, test-arm64 4/0, run_vk_device 2/0, the Windows probes 11/0
  at 150-162 fps, tests/android with 0 failures (62/0 without its boot
  section on both, 64/0 with it), the native arm64 Steam client's sign-in
  window at 15 s, Heroic's phase_b with and without `--no-opt`, and
  the TurboFan reproducer 10/10 (failures seen on the way were the
  environment, each passing on a re-run: the record says which). If the
  12.3 link fails, the Makefile links
  against the current SDK and says so; every build prints the SDK it
  recorded; CI warns when it is 13 or later.
- **A forked child loses it** (MEASURED on macOS 27, stage 28): the kernel
  sets the flag at exec from the image, and a child of `fork()` without exec
  -- its threads and its own children too -- has x18 zeroed again, on either
  build. `tests/x18_preserve/run.sh` now reports the fork child.
- **So the rewriter stays.** Guests fork without exec all the time
  (Chromium's zygotes, Android's zygote, shells). With the keep build and
  `LXRT_NO_X18=1` the native Steam client never showed its sign-in window
  and Heroic exited at start: both lost their forked GPU process ("GPU
  process isn't usable. Goodbye.") (MEASURED, stage 28). The runtime does
  not detect the kernel's behaviour and does not skip the pass; what the
  kernel's x18 adds is correctness for code the runtime never rewrote, in
  the process lxrun was exec'd as.
- Guest signal handlers used to leave the hardware x18 set to the virtual
  one on return, which broke a JIT's live x18 at its first signal with the
  keep build (MEASURED: generated code lost x18 after 1 signal at
  `7492467`). Since `cc54e05` an x18 the handler left alone goes back into
  the hardware register as it was (`runtime/signal.c`), and without the pass
  the frame carries the hardware x18 (X18_JIT: kept through 100-122
  handlers).
- It would also remove one of the two ARM64 Proton blockers (the Windows TEB
  lives in x18) for a process that does not fork; the `0x7ffe0000`
  low-address one stays (stage 19 §2).

## Native Wine ARM64 (2026-10-08)

The native Proton tool (tools/steamarm-native-proton) runs Wine with the
virtualisation off (LXRT_NO_X18=1). There x18 is the TEB, in Wine's PE code
and in FEX's ARM64EC JIT output; the kernel keeps it for the process lxrun was
exec'd as (the SDK 12.3 link, above), Wine's processes are exec'd, and Wine
already assumes that the Linux side may clobber x18 and puts the TEB back on
every return to Windows code. With the virtualisation on, every x18 use in
Wine's PE code and in the JIT's output was a trampoline: FINAL FANTASY VII
REMAKE reached its engine in 3 minutes, 5 s without (MEASURED,
benchmarks/stage62). STEAMARM_X18_VIRT=1 brings the virtualisation back for
that path.

## JIT output

The rewriter works on images as they are loaded, and code generated at run
time is only scanned for `svc`, TPIDR_EL0 and the trapped system registers
when it becomes executable (`runtime/jit.c`, `runtime/wxsplit.c`), never for
x18. JITs that avoid x18 are safe: FEX (patched), V8 (x18 is not
allocatable: stage 23). JITs that allocate it are not, where the kernel
zeroes x18: Linux aarch64 HotSpot (`R18_RESERVED` is defined only for macOS
and Windows builds, UPSTREAM DOCUMENTED in openjdk/jdk21u) and Mesa's
llvmpipe (LLVM's aarch64-linux target). MEASURED in stage 24 with the
current-SDK build: HotSpot computed wrong results and died at
`ldr w4, [x18, #256]` with x18 = 0; llvmpipe died at `stp xzr, xzr, [x18]`
with x18 = 0 (the fault report prints the instruction and x18 for code
outside the image since stage 24). Both are correct with the SDK-12.3 build
(the default since stage 28, above) in the process lxrun was exec'd as; in a
forked child they are not, on either build (X18_JIT, MEASURED).

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
- `LXRT_X18_STATS=1` prints, at exit, how often the `br x18` trampoline's
  tail was restarted after a fault, recovered from a branch to 0, and
  restarted at a signal delivery (`runtime/signal.c`, since `cc54e05`).
- Every lxrun build prints the SDK it recorded: "lxrun: LC_BUILD_VERSION
  sdk 12.3: the kernel keeps x18 for JIT code" (`Makefile`).

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
| `tests/x18_preserve/run.sh` | whether macOS itself keeps x18 for a pre-13 SDK binary, and in a fork child of it (it does not: stage 28) |
| `tests/elf/run.sh` (X18_JUMP_TABLE) | a `br x18` jump table keeps x16/x17 live across the branch, 16 preempted threads x 3M dispatches; both recoveries of the trampoline's tail forced from generated code |
| `tests/elf/run.sh` (X18_JIT) | generated code (never rewritten) keeps x18 across 1 ms guest signal handlers with the SDK-12.3 build, and loses it in a fork child; an expected failure with the current-SDK build |
| `tests/heroic/turbofan.sh` | V8's TurboFan in Heroic's Electron binary run as Node (stage 24's reproducer; 0 of 3 before `cc54e05`, 10 of 10 after) |

The FDE filter, a large `sp` offset and a poisoned site trapping had no
test at `dbd1657`; they have one since `9e7f4b9`. MEASURED at `5d9760a`:
`tests/elf/run.sh` 64 passed, 0 failed, 0 expected failures. MEASURED in
stage 28: 83 passed, 0 failed with the default (SDK 12.3) build; 82 passed,
0 failed, 1 expected failure (X18_JIT) with `LXRT_KEEP_X18=0`.
