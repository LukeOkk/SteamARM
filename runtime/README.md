# lxrt — a Linux aarch64 runtime on Darwin

MIGRATION_PLAN Stage 2. Runs a Linux aarch64 ELF inside a macOS process: no
Linux kernel, no hypervisor, no guest.

```
make lxrt
./build/lxrun build/hello-linux
hello from a Linux ELF, no VM
```

```
$ ./build/lxrun /tmp/lxrt-samples/hello_glibc
glibc static-pie alive: argc=1 argv0=... monotonic=127603.480354125
malloc+stdio+snprintf ok, strlen=29
```

```
$ scripts/mkroot-rpm.sh <root> <samples>    # populate a Linux tree, no VM
$ LXRT_ROOT=/tmp/lxrt-root ./build/lxrun /tmp/lxrt-samples/hello_dyn
[lxrt] PT_INTERP /lib/ld-linux-aarch64.so.1 -> /tmp/lxrt-root/lib/...
[lxrt] mapped code at 0x1060d0000: 415 svc sites, 415 rewritten, 0 poisoned
glibc static-pie alive: ...
malloc+stdio+snprintf ok, strlen=27
```

```
./tests/elf/run.sh      # 11 tests
```

## What exists

| file | does |
|---|---|
| `elf.c` | maps `PT_LOAD` segments, refuses what it cannot honour |
| `rewrite.c` | rewrites every `svc #0` into a branch to a per-site trampoline |
| `trampoline.S` | the trampoline template, and the thunk that enters the guest |
| `dispatch.c` | the Linux syscall table |
| `tls.c` | the guest thread pointer, in a Darwin TSD slot |
| `thread.c` | `clone`/`clone3`, `futex`, thread exit |
| `signal.c` | signal numbers, the Linux frame, delivery and return |
| `errno_map.c` | Darwin errno → Linux errno |
| `fsflags.c` | `open()` and `*at()` flag values |
| `socket.c` | `sockaddr` layout, address families, `SOCK_*` flags |
| `sysreg.c` | `CTR_EL0`, which Darwin traps even for its own code |
| `window.m` | the `NSWindow` + `CAMetalLayer` the guest presents into |
| `include/lxrt_host.h` | the host bridge, for guest code to include |
| `stack.c` | argc/argv/envp and the auxiliary vector, as glibc expects them |
| `main.c` | `lxrun`, with `--trace` and `--dry-run` |

Guest absolute paths resolve inside `LXRT_ROOT` when it is set;
`scripts/mkroot-rpm.sh` populates one from pinned Fedora RPMs, with no VM (the
VM-era `tests/elf/mkroot.sh` has been deleted).

## What gets rewritten, and why

Two instruction kinds, for the same underlying reason: Darwin gives no way to
intercept either, and leaving either alone fails *silently*.

| instruction | problem | replacement | cost |
|---|---|---|---|
| `svc #0` | Darwin ignores the SVC immediate and dispatches on `x16`, so a Linux syscall runs an arbitrary Darwin one | branch to a trampoline that saves x0–x30 and calls the dispatcher | 68 ns |
| `mrs Xt, CTR_EL0` | Darwin traps it at EL0, for its own code too; every icache flush reads it | branch to two instructions that materialise a synthesised value | — |
| `mrs Xt, TPIDR_EL0` | Darwin clobbers `TPIDR_EL0` on every context switch, so the guest loses its thread pointer | branch to a 3-instruction read of a Darwin TSD slot | 2.34 ns |
| `msr TPIDR_EL0, Xt` | same | branch to a store into that slot | — |

Counted on the real binaries: glibc has **415** syscall sites and **1440** TLS
reads. The thread pointer is written once and read constantly, which is why the
read trampoline uses no scratch register and no stack.

## The host bridge

A guest ELF cannot `dlopen` a Mach-O dylib — its own `ld.so` has never heard of
one. Two private syscalls (`0x4C580001` dlopen, `0x4C580002` dlsym) hand it a
**real host function pointer**, which it then calls with an ordinary `blr`.

There is no marshalling, no ring buffer and no helper process, because AAPCS64
is the same calling convention on both sides and there is one address space.
Measured: **0.8 ns** for a direct guest→host call, **11.6 ns** for a real
`vkGetPhysicalDeviceProperties` on MoltenVK. A Linux ELF reports the GPU as
`Apple M4` — not `Virtio-GPU Venus (Apple M4)`
(`benchmarks/stage4-vulkan.txt`).

This is what replaces the VM's graphics bridge, where every Vulkan call was
serialised by Mesa venus across a virtio ring into a second process.

### `libvulkan.so.1`, for consumers that do not know about any of this

DXVK links the Vulkan loader by SONAME and calls the API; it will never invoke
the bridge itself. `make shim` generates an ordinary Linux aarch64 shared
object — 431 entry points, each a four-instruction tail call into MoltenVK,
built on the host with clang+lld and no cross toolchain. A tail call is
signature-agnostic: arguments stay where the caller put them, so one thunk
shape serves every prototype.

A plain Fedora binary compiled with `-lvulkan`, resolved by the guest's own
`ld.so`, creates an instance and enumerates `Apple M4`. The thunk costs
**0.4 ns** on top of the direct call.

### Presentation

The runtime owns an `NSWindow` and a `CAMetalLayer`; a private syscall hands
the layer to the guest, which passes it to `vkCreateMetalSurfaceEXT`. A guest
builds a swapchain on it and presents — **at 165.8 fps on a 165 Hz display,
i.e. vsync-bound** (`benchmarks/stage4-present.txt`).

The three full-frame copies the VM needed per displayed frame are gone: the
swapchain images are the layer's drawables. AppKit owns the main thread, so
the guest runs on a second one and the main thread stays in a run loop — which
is why every signal is blocked there and `tgkill` is thread-directed.

## Why rewriting, and not trapping

Stage 1 measured that **`svc` cannot be trapped on Darwin**: XNU ignores the SVC
immediate and dispatches on `x16`, so a Linux `svc #0` does not fault — it runs
whatever Darwin syscall `x16` happens to hold. There is no exception to catch.
Rewriting is the only mechanism, and at **68 ns** it is cheaper than the `svc`
it replaces. See `benchmarks/README.md`.

Because a missed site is *silent misbehaviour* rather than a crash, a site the
rewriter cannot reach is poisoned with `brk #1` instead of being left alone.

## Measured

- `clock_gettime` through the full path — rewritten site, trampoline, C
  dispatcher, `mach_absolute_time`, back — **12.6 ns**. It is 70.6% of every
  syscall the real guest makes.
- glibc 2.x contains **415** syscall sites, `ld-linux-aarch64.so.1` **54**,
  `libvulkan.so.1` and `/usr/bin/true` **zero**. Userspace goes through libc;
  the rewriting surface is small and concentrated.
- The linear scan agrees **exactly** with a full disassembly on all four
  (`benchmarks/stage2-rewrite-validation.txt`).
- A real **Fedora glibc 2.42 static-PIE binary** reaches `main` with working
  malloc, stdio, `snprintf` and `clock_gettime`. `rseq` is the only syscall its
  start-up still misses, and glibc falls back from that by design.
- **Guest TLS survives context switches.** Darwin does not use `TPIDR_EL0` for
  its own thread pointer — but it does not preserve it either: a syscall keeps
  it, any deschedule clobbers it. Rewriting every access to a Darwin TSD slot
  fixes it, proven both ways on a real glibc binary that sleeps 300 times
  (`benchmarks/stage3-tls.txt`). `LXRT_NO_TLS_REWRITE=1` reproduces the failure
  on demand.

## Hard limitations, stated rather than worked around

- **Non-PIE (ET_EXEC) images cannot be loaded.** They link at 0x200000, inside
  Darwin's 4 GiB `__PAGEZERO`, which cannot be shrunk (every `-pagezero_size`
  below the default is SIGKILLed at exec) nor mapped over (`mmap` and
  `mach_vm_allocate` both refuse). An ET_EXEC image has no relocations to move
  it. Measured in `benchmarks/stage2-pagezero.txt`; the cost is quantified in
  `stage2-elf-survey.txt` — 96.7% of `/usr/bin`, 100% of `/usr/lib64`, 100% of
  FEX and 94.4% of Steam's own binaries are PIE, so the exclusion is real but
  narrow.
- **The `svc` scan is a linear pattern match**, structurally unable to tell
  code from data. Validated, not proven.
- **Dynamic linking works, with one sharp edge.** A trampoline must be within
  `b` range (±128 MiB) of its site, and the guest's `ld.so` reserves whole
  spans it then fills in with `MAP_FIXED`. A pool placed adjacent to a mapping
  landed inside such a span and was mapped over, surfacing as a SIGSEGV deep
  inside libc. Pools are now placed distant-first and tracked; a guest mapping
  that lands on one is reported, and a test asserts it never happens.
- **Threads and signals work.** `clone`/`clone3`, `futex` WAIT/WAKE,
  per-thread TLS and the `CLONE_CHILD_CLEARTID` wake that `pthread_join` needs
  are implemented: 4 pthreads with a mutex and a condition variable run
  correctly, at the same speed as on a real Linux kernel
  (`benchmarks/stage3-threads.txt`). Signals are delivered for real, with
  number translation, a Linux frame on the guest stack and a return path
  through the runtime (`benchmarks/stage3-signals.txt`).
- **Signal deviations, documented not hidden:** `uc_mcontext.pc` is a host
  address when the signal arrives inside a syscall; `x16` is not restored by
  `rt_sigreturn`. Realtime signals (32–64) have no Darwin number of their own:
  they are carried on Darwin's `SIGEMT`, which Linux's numbering leaves
  unclaimed, with their Linux mask bits kept per thread (`signal.c:84-100`).
  One aimed at a thread of another process goes through that process's
  mailbox in `/tmp/lxrt-sig` (`signal.c:1296-1308`).
- **Futex requeue and `WAKE_OP` are emulated; PI futexes are not.** Darwin
  has no requeue, so `FUTEX_REQUEUE`/`FUTEX_CMP_REQUEUE` wake the waiters
  Linux would have moved, never fewer (`futex_ops.c:10-15`, `:897-906`), and
  `FUTEX_WAKE_OP` is implemented (`futex_ops.c:907-908`). The PI operations
  return `-ENOSYS` rather than a wrong answer (`futex_ops.c:759-779`, `:912-918`).
- **`fork` is Darwin's `fork`.** `clone` without `CLONE_VM` or `CLONE_THREAD`
  becomes one (`thread.c:467-470`, `process.c:58-80`), and `execve`
  re-executes `lxrun` with the new image (`process.c:122-133`).
- **Three number spaces differ between Linux and Darwin, and all three are
  silent when wrong:** errno values, `open()`/`*at()` flags, and `sockaddr`
  layout with the address families. Linux `O_APPEND` passed through reads as
  Darwin `O_TRUNC|O_EXCL` — it truncates the file it was meant to append to.
  Each has its own translation module; nothing is passed through.
- **Never return a host errno.** Darwin and Linux disagree on the numbers —
  `EAGAIN`/`EDEADLK` are swapped, `ENOSYS` is 78 against 38 — so every return
  path goes through `errno_map.c`. Getting this wrong killed glibc's
  `clone3` → `clone` fallback and surfaced as
  `pthread_create: Remote address changed`.
- **Do not advertise capabilities the runtime lacks.** `HWCAP_CPUID` in the
  auxiliary vector means "the kernel emulates `mrs` of the ID registers".
  Darwin does not, and glibc took SIGILL on `mrs x0, midr_el1` before its first
  syscall. `stack.c` now leaves it out, with a comment saying why.
- **The syscall table is the measured head of the distribution**, not Linux.
  Anything else returns `-ENOSYS` and is recorded so a failing run names the
  call it lacked.
