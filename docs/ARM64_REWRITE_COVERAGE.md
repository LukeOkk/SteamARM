# ARM64 rewrite coverage

lxrun cannot trap a Linux `svc` on Darwin: Darwin ignores the immediate and
runs the system call named by x16 (MEASURED, `benchmarks/stage1-syscall-cost.txt`).
So every aarch64 image is scanned when it is mapped, and each site that
must not run as written is replaced by a branch to a trampoline:

| kind | why | where |
|---|---|---|
| `svc #0` | Linux system call | `runtime/rewrite.c`, dispatcher in `runtime/dispatch.c` |
| `mrs`/`msr TPIDR_EL0` | Darwin clobbers TPIDR_EL0 on context switch (`benchmarks/stage3-tls.txt`) | `runtime/tls.c` |
| `mrs CTR_EL0`, ID registers | Darwin traps them at EL0 (`benchmarks/stage5-sysreg.txt`) | `runtime/sysreg.c` |
| any instruction naming x18 | Darwin zeroes x18 (`docs/X18_VIRTUALIZATION.md`) | `runtime/x18.c` |

"Coverage" here means, per image: sites found, sites rewritten, and sites
poisoned (refused as `unsupported`, or with no trampoline in branch range,
`unreachable`). A poisoned site is `brk #1` since `dbd1657`; up to 0.3.4 it
was a live `svc #1` (`docs/X18_VIRTUALIZATION.md`).

Code that FEX generates for x86 programs is not scanned: FEX was patched to
call the runtime instead of emitting `svc` (`benchmarks/stage5-fex.txt`).
Only FEX's own aarch64 image, and the aarch64 libraries it loads, go through
the rewriter.

## Where the numbers come from

| path | printed | source |
|---|---|---|
| images lxrun maps itself (the program, ld.so) | always, one `svc`/`tls` line and one `x18` line, with poisoned, unsupported and unreachable counts | `runtime/main.c:251-260` |
| code ld.so maps later (16 KiB-aligned libraries) | only with `LXRT_TRACE=1`; includes unsupported, unreachable and TLS counts | `runtime/dispatch.c:632-640` |
| code on the sub-page path (4 KiB-aligned libraries: libcef, the client's own) | only with `LXRT_TRACE=1`; the same fields as the row above since `416d7b4` | `runtime/dispatch.c:1221-1229` |

Up to `416d7b4` the last row printed x18 found and rewritten only, so
libcef's refused x18 sites showed only as found minus rewritten (the
2026-09-28 trace below). The reason for each refused site is in the
per-site trace lines (`runtime/rewrite.c:529-533`).

`build/lxrun --dry-run <image>` prints the first row for one image without
running it (`runtime/main.c:515`).

Line numbers on this page are at `b3f64c8` (updated 2026-09-29); the
measured sections below keep the date of the runs they describe.

## Measured coverage (native arm64 client, audit of 2026-09-29)

Logs in `~/SteamARM-roots/logs/`, read on the Mac. These runs predate
`dbd1657` and the planner changes after it; what changed since is in the
next section.

**Images loaded at exec time, newest run** (`native-arm64-steam-nss-20260929.log`,
MEASURED): 120 x18 report lines; 0 `unsupported`, 0 `unreachable`; `svc`
and TLS poisoned 0.

| image | words scanned | svc | tls | x18 |
|---|---:|---|---|---|
| `steamrtarm64/steam` | 2,409,948 | 3977/3977 | 49/49 | 2840/2840 |
| `steamrtarm64/steamwebhelper` (from `Steam/logs/steamwebhelper.log`) | | 1540/1540 | 64/64 | 1442/1442 |
| `ld-linux-aarch64.so.1` | | 54 | 31 (+2 sysreg) | 27 |

**Libraries loaded later**, traced runs only. Newest:
`native-arm64-webhelper-libcef-all-text-20260928.trace` (2026-09-28 17:14,
`LXRT_X18_ALL_TEXT` on for libcef). Per process (MEASURED):

| library | process | x18 found | rewritten | poisoned | reasons |
|---|---|---:|---:|---:|---|
| `libcef.so` (sub-page, `0xa2a4000` bytes) | every webhelper process | 76,048 | 76,043 | 5 | 3 `exclusive` (`88dffe52` ×2: `ldar w18, [x18]`; `88dffe12`: `ldar w18, [x16]`), 2 `sysreg` (`d53b4212`, `d51b4212`: MRS/MSR NZCV with x18) |
| `libgallium-25.3.6.so` | the webhelper's GPU process | 6,178 | 6,177 | 1 | `writes sp` (`9100025f`, `mov sp, x18`) |
| `libLLVM.so.21.1` | the webhelper's GPU process | 34,697 | 34,697 | 0 | |
| `steamclient.so` (`mapped code at 0x7efec733c000`) | the client's main process | 3,858 | 3,856 | 2 | `writes sp` (`9100025f` at `0x7efec8b94284` and `0x7efec8b943e8`) |

- Correction (2026-09-29): the first version of this page, and the audit it
  came from, gave libLLVM "3,858 found, 2 `writes sp`". Those counts are
  steamclient.so's: its report line comes from the main process and sits
  just after the GPU process's libLLVM `openat` in the trace, where the two
  processes' output is interleaved. libLLVM's own sub-page report is
  34,697 found, 34,697 rewritten (MEASURED, the same trace; `926b89a`).
- The sub-page total over the trace is 373,644 found and 373,624 rewritten:
  4 webhelper processes × 5 libcef sites.
- Across all 15 native logs and traces with mapped-code lines: `svc`
  poisoned 0, x18 unreachable 0.
- Earlier runs, before the large-`sp`-offset fix, had 77-78 unsupported x18
  sites (`native-arm64-steam-stack-pad-20260928.log.gz`,
  `native-arm64-steam-chunk-rewrite-20260928-2.log`).
- The 2026-09-29 run was not traced, so its dlopen counts are UNKNOWN.
- All these runs predate `dbd1657`: their poisoned words were `svc #1`.

## Since then (stage 22, 2026-09-29)

- **The poisoned sites run.** With `lxrun` at `dbd1657` (poison `brk #1`),
  the webhelper's browser process trapped on libcef's `mrs x18, nzcv` 3-4 s
  after start (E4), and 500 of 500 renderer deaths were at the poisoned
  `ldar w18, [x16]` (E7) (MEASURED,
  `benchmarks/stage22-native-arm64-bringup.txt`). The client restarted the
  webhelper about every 10 s. Whether libgallium's or steamclient.so's
  refused sites ran was not measured.
- **None of the 8 is refused now.** `4534360` and `c86b633`, then `926b89a`,
  plan LDAR/STLR, MRS/MSR of NZCV/FPCR/FPSR and every instruction that
  computes `sp` from x18 (`docs/X18_VIRTUALIZATION.md`). Dry run of
  `926b89a` (MEASURED, `build/lxrun --dry-run`):

  | library | x18 found | rewritten | unsupported |
  |---|---:|---:|---:|
  | `libcef.so` (`LXRT_X18_ALL_TEXT=libcef.so`) | 76,048 | 76,048 | 0 |
  | `libgallium-25.3.6.so` | 6,178 | 6,178 | 0 |
  | `steamclient.so` | 3,858 | 3,858 | 0 |
  | `libLLVM.so.21.1` | 34,697 | 34,697 | 0 |

  After the NZCV and LDAR changes a 45 s zygote trace had no `brk` traps
  left (stage 22 E9), and the client reaches its sign-in window
  (stages 22-23).
- **The report gap is closed.** The sub-page line prints unsupported and
  unreachable counts since `416d7b4` (table above).

## What is not known

| question | how to measure |
|---|---|
| dlopen coverage of the current client build (stable, and the `steamdeck_stable` build the Frame root's client moved to) | a traced run: `LXRT_X18_ALL_TEXT=libcef.so LXRT_TRACE=1 LXRT_TRACE_MATCH=steamwebhelper LXRT_TRACE_FILE=<file>`, then `grep -E 'mapped code at|sub-page code at|unsupported' <file>` |
| Coverage for the Steam Frame root's own libraries | the same traced run on that root (`scripts/mkframeroot.sh`, `benchmarks/stage23-frame-root.txt`) |
| `svc` sites in data (literal pools) | the loader warns on `hvc`/`smc` encodings in an image (`runtime/main.c:261-265`); the FDE filter applies to x18 only |

## Why the gaps matter

- A site that is still refused (a real load/store-exclusive, CASP, another
  system register, an `sp` writeback) stops its process with SIGTRAP when
  it runs, instead of running a random Darwin call. That is how stage 22
  found libcef's two executed sites (above).
- JIT output is scanned for `svc`, TLS and system-register reads before it
  runs, but not for x18: V8 does not allocate x18, and the x18-naming words
  seen in its code pages were data (MEASURED and UPSTREAM DOCUMENTED,
  `benchmarks/stage23-native-arm64-jit.txt`).
