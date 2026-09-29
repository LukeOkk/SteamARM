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
| images lxrun maps itself (the program, ld.so) | always, one `svc`/`tls` line and one `x18` line, with poisoned, unsupported and unreachable counts | `runtime/main.c:207-216` |
| code ld.so maps later (16 KiB-aligned libraries) | only with `LXRT_TRACE=1`; includes unsupported and unreachable | `runtime/dispatch.c:617-633` |
| code on the sub-page path (4 KiB-aligned libraries: libcef, the client's own) | only with `LXRT_TRACE=1`; `svc` poisoned, but **x18 found and rewritten only** | `runtime/dispatch.c:1210-1214` |

The last row is a diagnostic gap: libcef's refused x18 sites show only as
found minus rewritten, and their reasons only in the per-site trace lines
(`runtime/rewrite.c:486-490`).

`build/lxrun --dry-run <image>` prints the first row for one image without
running it (`runtime/main.c:269`).

## Measured coverage (native arm64 client, audit of 2026-09-29)

Logs in `~/SteamARM-roots/logs/`, read on the Mac. Code line numbers on
this page are at `dbd1657`.

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
`LXRT_X18_ALL_TEXT` on for libcef). Per webhelper process (MEASURED):

| library | x18 found | rewritten | poisoned | reasons |
|---|---:|---:|---:|---|
| `libcef.so` (sub-page, `0xa2a4000` bytes) | 76,048 | 76,043 | 5 | 3 `exclusive` (`88dffe52`, `88dffe12`: LDAR/LDAXR w18), 2 `sysreg` (`d53b4212`, `d51b4212`: MRS/MSR NZCV with x18) |
| `libgallium-25.3.6.so` | 6,178 | 6,177 | 1 | `writes sp` (`9100025f`) |
| `libLLVM.so.21.1` | 3,858 | 3,856 | 2 | `writes sp` |

- The sub-page total over the trace is 373,644 found and 373,624 rewritten:
  4 webhelper processes × 5 libcef sites.
- Across all 15 native logs and traces with mapped-code lines: `svc`
  poisoned 0, x18 unreachable 0.
- Earlier runs, before the large-`sp`-offset fix, had 77-78 unsupported x18
  sites (`native-arm64-steam-stack-pad-20260928.log.gz`,
  `native-arm64-steam-chunk-rewrite-20260928-2.log`).
- The 2026-09-29 run was not traced, so its dlopen counts are UNKNOWN.
- All these runs predate `dbd1657`: their poisoned words were `svc #1`.

## What is not known

| question | how to measure |
|---|---|
| Are the 8 poisoned sites (libcef 5, libgallium 1, libLLVM 2) ever executed? | Run the native client after `dbd1657` with `LXRT_X18_ALL_TEXT=libcef.so LXRT_TRACE=1 LXRT_TRACE_MATCH=steamwebhelper LXRT_TRACE_FILE=<file>`; a SIGTRAP at one of those addresses answers yes |
| dlopen coverage of the current client build | the same traced run; `grep -E 'mapped code at|sub-page code at' <file>` |
| Coverage for the Steam Frame root's own libraries | the same, once the client runs on that root (`docs/ARM64_FIRST_MIGRATION.md`) |
| `svc` sites in data (literal pools) | the loader warns on `hvc`/`smc` encodings in an image (`runtime/main.c:217-221`); the FDE filter applies to x18 only |

## Why the gaps matter

- libcef's 5 sites sit in its atomics and flag handling. If one runs, the
  webhelper now stops with SIGTRAP instead of running a random Darwin call
  (HYPOTHESIS: it would show as a webhelper crash at startup or under load).
- libgallium and libLLVM are Mesa's software renderer in the webhelper's
  GPU process (`docs/STEAMWEBHELPER_BRINGUP.md`). The `writes sp` form
  needs a planner change: the trampoline spills its scratch registers on
  `sp` (`runtime/x18.c`).
