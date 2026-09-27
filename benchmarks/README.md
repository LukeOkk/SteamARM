# benchmarks

MIGRATION_PLAN Stage 0 (a test harness) and Stage 1 (the one measurement that
decides the architecture).

## Stage 1 — cost of intercepting one Linux syscall on Darwin

```
clang -O2 -arch arm64 -o build/syscall_cost benchmarks/syscall_cost.c
./build/syscall_cost
```

Raw output of the run this document is based on: `stage1-syscall-cost.txt`
(Apple M4, macOS 27, 2026-09-23).

## Stage 5 — running x86-64 programs

`scripts/run-fex.sh [--trace] <program> [args]` runs an x86-64 Linux program
through FEX under the runtime (starts FEXServer if needed; env and paths
documented in the script). `benchmarks/x86_bench.c` is the throughput
benchmark behind `stage5-x86-throughput.txt`.

## Stage 5 — does a W^X flip inside a signal handler take effect?

```
clang -O1 -o build/wx_in_handler benchmarks/wx_in_handler.c
codesign -s - --entitlements resources/SteamARM.entitlements build/wx_in_handler
./build/wx_in_handler
```

(`resources/SteamARM.entitlements` was the VM app's and has been deleted;
`resources/lxrt.entitlements` is the runtime's.)

Decides how FEX's JIT can write to `MAP_JIT` memory on Apple Silicon. Result
(`stage5-jit.txt`, REFINEMENT): a `pthread_jit_write_protect_np` issued inside
a handler is honoured for the handler's own stores and can be flipped back
before returning — but a flip meant to *persist* past the return is not, so the
fault-driven design is dead and the guest asks for each flip itself (private
syscall `0x4C580020`; proven end to end by `tests/elf/jit_wx.c`).

### Finding 1 — `svc` cannot be trapped. This is the decisive one.

`svc #0` with `x16 = 20` returned our own pid. **Darwin ignores the SVC
immediate and dispatches on `x16`.**

A Linux aarch64 binary issues `svc #0` with the syscall number in `x8`, and
`x16` holding whatever the compiler last left there. On Darwin that does not
fault — it *executes the Darwin syscall whose number happens to be in `x16`*,
with Linux arguments in `x0`–`x5`. Silently. Sometimes destructively.

Consequences, and they are not negotiable:

- There is no "trap the guest syscall and service it" design. The mechanism
  does not exist on this platform.
- The loader **must rewrite every `svc` site** it can find statically.
- Code that generates `svc` at runtime — which is exactly what FEX's JIT does
  for the x86 path Steam depends on — needs a different mechanism: FEX must be
  built to emit a call into the runtime instead of an `svc`.

### Finding 2 — signals do fire, but as SIGTRAP, and they are slow

An out-of-range syscall number (`x16 = 0x00ffffff`) raises a catchable
**SIGTRAP**, not SIGSYS. The handler can step `pc` past the instruction and
resume. So a trap-based fallback is *possible* for deliberately-poisoned
numbers — it is just far too expensive to use as the main path.

### Measured

| mechanism | ns/op | vs. native syscall |
|---|---:|---:|
| indirect function call (floor) | 1.3 | — |
| Darwin syscall, `svc #0x80` | 114.2 | 1× |
| Darwin syscall, `svc #0` | 75.8 | same path (immediate ignored) |
| in-process signal interception | **2794.2** | **~25–37×** |
| Mach exception interception | **6317.0** | **~55–83×** |
| rewritten `svc` → dispatcher → real syscall | **68.1** | **~1×** |
| rewritten `svc`, serviced in userspace | **0.9** | — |

### What this means for the plan

The two trap-based mechanisms cost **2.8 µs and 6.3 µs** per syscall. At a
modest 50 000 syscalls/second — ordinary for a game plus a compositor plus
Steam — that is **0.14 s and 0.32 s of pure overhead per wall-clock second**.
Trap-based interception is not viable, and Finding 1 says it was never
available anyway.

The rewriting path costs **68 ns** when it ends in a real kernel call — i.e.
*less* than the native `svc #0x80` it replaces, because a direct branch is
cheaper than a trap — and **0.9 ns** when the runtime can answer without the
kernel (a cached value, a clock read, an uncontended futex).

Compare against what it replaces. In the current architecture a guest syscall
does **not** cause a VM exit at all: the guest kernel services it in-guest. VM
exits happen for MMIO and virtio doorbells, not for `read()`. So the honest
comparison is:

- **VM today:** guest syscall ≈ a native Linux syscall inside the guest, zero
  host involvement. VM exits are a *separate* cost, paid per virtio interaction
  — see the VM exit accounting below.
- **ZERO-VM with rewriting:** ≈ 68 ns, and better than that for anything the
  runtime can answer itself.
- **ZERO-VM with trapping:** 2.8–6.3 µs. Dead on arrival.

**The Stage 1 exit criterion is met, and the answer is conditional:** ZERO-VM
is viable *only* with instruction rewriting, and rewriting is fast enough to be
a non-issue. The risk moves off performance entirely and onto correctness —
finding every `svc` site, and getting FEX to stop emitting them.

## Stage 0 — frame-time capture

> Note (2026-09-27): the VM app, `frametimes.py`, `capture.sh` and the
> `runs/vm-baseline-*` captures have been deleted with the VM.

The app writes one 16-byte record per presented frame when `STEAMARM_TRACE`
names a file:

```
STEAMARM_TRACE=/tmp/frames.bin /Applications/SteamARM.app/Contents/MacOS/SteamARM
benchmarks/frametimes.py /tmp/frames.bin --label vm-baseline --json vm.json
```

`frametimes.py` reports median, **1% low, 0.1% low**, standard deviation and a
histogram. Never compare on average FPS alone — the user's report that
CrossOver *feels* smoother at a similar frame rate is a claim about the tail,
and only the tail statistics can confirm or refute it.

`capture.sh <label> [seconds]` runs the whole cycle: start with tracing and VM
exit accounting on, wait, shut the guest down gracefully, analyse.

## VM exit accounting

libkrun is patched (`src/hvf/src/lib.rs`, `mod vmexit_stats`) to count exits
and charge time to guest and host, printing every 200 000 exits when
`LIBKRUN_VMEXIT_STATS` is set:

```
[vmexit] 200000 exits | host 2.41 us/exit | guest 18.77 us/exit | host share 11.4% | ec=0x24:… 
```

`ec` is the AArch64 exception class: `0x24` data abort (MMIO), `0x18` system
register trap, `0x3f` a non-exception exit (vtimer or cancel). The host share
is the fraction of vcpu wall time spent outside the guest — the part ZERO-VM
would delete.

## Method

Fix resolution, settings, FPS cap, vsync, display, game version, scene and
duration across every compared run. Report median, 1% low, 0.1% low and the
histogram. Compare four ways where possible: VM baseline, ZERO-VM, CrossOver,
and a native macOS build where one exists.
