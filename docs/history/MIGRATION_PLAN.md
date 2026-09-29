# MIGRATION_PLAN

> **SUPERSEDED — VM-era.** The VM → ZERO-VM migration this plan describes
> happened: the VM was deleted on 2026-09-27 (item 9 below). Its exit-criteria
> status is dated 2026-09-26; later stages (15-21) moved several criteria.
> The current plan is `docs/ARM64_FIRST_MIGRATION.md`. Kept as history.

**Status: PLANNED.** Stages are ordered so that each one can *fail cheaply* and
kill the plan early if the thesis is wrong. Nothing is deleted until a tested
replacement exists.

---

> **STATUS, end of 2026-09-25.** Proven, no Linux kernel and no hypervisor
> involved: Linux aarch64 ELFs (static, dynamic, threads, signals, Vulkan via
> MoltenVK, a rendered window); x86-64 through FEX — static, and dynamic from
> FEX's own Ubuntu rootfs (`uname`, `ls`, `bash` with a forked subshell,
> `env`); FEX's translation overhead isolated at 1.0× against native aarch64
> under the runtime, while the VM path costs 4.3–4.5× on the same work
> (`benchmarks/stage5-x86-throughput.txt`). The test suite is 29 checks against
> the same binaries on a real Linux kernel, all green (fake bwrap included), plus 91+ syscalls
> including epoll/eventfd, SysV IPC, memfd, mremap, timerfd, signalfd, inotify,
> ioctl(tty). **Not reached:** the VM is still the build toolchain and the
> shipping path; Steam has not been started under the runtime; the two Stage 6
> decisions (a fake `bwrap` for pressure-vessel's mount plan; a guest address
> base in FEX for i386, which is the Steam client) are taken but not built;
> Stage 0's in-game frame-time captures are still owed; Stage 9 (`ZERO_VM=1`,
> then default, then delete) is not begun. Late addition: the fake `bwrap`
> is built (`runtime/mounts.c`, 11/11 against the real bwrap on Linux). **`steamwebhelper` (CEF) starts** under FEX under the runtime — three
> processes, alive at the 120 s cap, eight divergences fixed on the way
> (`benchmarks/stage5-fex.txt`); one FEX-internal fault remains, and the
> pressure-vessel chain is gated by the sub-4 GiB wall. The brief's completion criterion —
> the VM gone and Steam running without it — is therefore **not met**.
>
> **Later, 2026-09-25 (evening).** Root cause of every "garbage pointer inside
> FEX's IR compiler" crash found and measured: **Darwin zeroes x18 on every
> exception return, including preemption** (`benchmarks/stage5-x18.txt`), and
> Linux code — FEX's C++ (2271 sites), FEX's own JIT output (x18 is in its
> 64-bit dynamic register pool, verified in source), glibc (326), libstdc++
> (251), ld.so, libgcc_s, libm — uses x18 as a temporary. Fix in three parts,
> in progress: `patches/fex-lxrt-x18.patch` (JIT pool) + FEX rebuilt with
> `-ffixed-x18` (building on the guest), and x18 virtualisation in the
> runtime's rewriter for every other Linux image (`runtime/x18.c`, being
> written; integration into `runtime/rewrite.c` pending). Also fixed today:
> the host alt stack was installed on AppKit's thread instead of the guest
> thread, and Darwin's `fork()` child starts with no alt stack at all
> (`benchmarks/stage3-signals.txt`); `MAP_FIXED_NOREPLACE` now succeeds at
> 4 KiB granularity (`benchmarks/stage5-subpage.txt`). Open and UNKNOWN: a
> kernel-originated SIGKILL (exit reason namespace FOUNDATION, code 1) of the
> root FEX process under tracing. The brief was re-issued with a Milestone 0
> — audit FEX upstream before more code — `FEX_REUSE_ANALYSIS.md` is being
> produced from the exact guest commit (FEX-2609-113-g08f451d3b).
>
> **Night of 2026-09-25 — results.** x18 fixed end to end: FEX rebuilt with
> `-ffixed-x18` and the JIT-pool patch (0 x18 sites left in the binary),
> `runtime/x18.c` virtualises x18 for every other Linux image (3066/3067
> census sites planned, 0 false positives/negatives; `tests/elf/x18_test`
> 26/26 under the runtime, 24/26 without the pass). The FEX fork-loop stress
> passes **6/6 for dash and 6/6 for bash** (was 2/6). The suite is **32/32**:
> the five Vulkan checks that had regressed were the `libvulkan.so.1` shim
> being mapped through the sub-page path without rewriting (a live `svc` →
> SIGSYS), fixed by rewriting code mapped that way. Milestone 0 of the
> re-issued brief is delivered: `FEX_REUSE_ANALYSIS.md` (76 findings over
> the exact guest commit, verified against the cited lines). The single next
> milestone is **Milestone 4 — FEX i386 ZERO-VM**, gated on a guest address
> base inside FEX; the completion criterion (VM gone, Steam running) is
> still **not met**.
>
> **2026-09-26, Milestone 4 under way.** Measured that the low 4 GiB cannot be
> reclaimed at run time either (`benchmarks/stage7-guest-base.txt`), designed
> and applied the guest-address-base patch to FEX
> (`patches/fex-lxrt-guest-base.patch`, 26 files, three choke points) plus the
> runtime's pointer-argument translation (`runtime/gbase.c`). First i386
> probe runs are in progress; the unpatched baseline dies by jetsam after an
> endless allocation loop in FEX's 32-bit start-up. With the corrected build the
> base is live and the guest loads, but i386 does not run yet: FEX's fixed
> RWX mappings fail because Darwin rejects MAP_JIT+MAP_FIXED, and FEX's own
> SIGSEGV handler then derefs a near-null pointer
> (`benchmarks/stage7-guest-base.txt`). Runtime-side fixed-RWX handling is the
> next step. Milestone 4 exit criterion NOT met.
>
> **2026-09-26, Milestone 4 MET.** Fixed the MAP_JIT+MAP_FIXED rejection in
> the runtime (munmap-then-MAP_JIT-at-hint, verified 8/8), then fixed a chain
> of un-based guest-memory accesses one rebuild at a time (32-bit guests were
> using FEX's dead-code passthrough allocator instead of the real bitset one;
> `GuestMprotect`, the vsyscall stub, `AT_PLATFORM`/`AT_RANDOM`/`AT_EXECFN`,
> `CheckRangeExecutable`, and the VDSO symbol parser all needed `G2H()`).
> Root cause of the last SIGSEGV: `ContextImpl::GuestCodeBase()`'s
> `Config.Is64BitMode ? 0 : Config.GuestBase` silently truncated the 64-bit
> base to 16 bits — both operands are `Config::Getter` objects, and the
> ternary's common-type deduction picked the class type instead of the
> built-in `uint64_t`. A debug print proved it: `Config.GuestBase` held the
> correct 0x7000004000, the ternary's result was 0x4000. Fixed by reading
> each operand into a plain local first. **A static i386 ELF now runs end to
> end under `FEX_GUESTBASE=1` and exits cleanly** (8/8 checks: `write`,
> `mmap2`, `.data` access at the announced base, `brk` + heap store,
> `munmap`, a `MAP_FIXED_NOREPLACE` mapping at a chosen low address; exits
> via `exit_group` with status 0, confirmed by `proc_pidinfo` exit reason).
> Next: a dynamic i386 ELF from the rootfs, then `ubuntu12_32/steam` itself
> (Milestone 7). The brief's overall completion criterion (VM gone, Steam
> running without it) is still NOT met.
>
> **2026-09-26 (later): dynamic i386 and x86-64 ET_EXEC run.** A dynamically
> linked i386 program (real `ld-linux.so.2` + `libc.so.6`) runs end to end
> after three more fixes (`PopTwo`, non-rep `MOVS` write-back, and one choke
> point that bases every pointer argument of every 32-bit syscall handler).
> Then the next Steam blocker was measured: the whole Steam runtime's tooling
> (`pressure-vessel-wrap`, `srt-bwrap`, `steam-runtime-*`, the runtime's
> `bash`, the rootfs `python3`) is x86-64 ET_EXEC below 4 GiB. A 64-bit "low
> window" now maps just that range at a power-of-two base, with a flag-free
> three-instruction translation in the JIT that is idempotent on host
> addresses. All five runtime tools and `python3` run under it
> (`benchmarks/stage7-guest-base.txt`). Next: pressure-vessel's container
> launch (`srt-bwrap` needs namespaces; the runtime has a fake-bwrap plan)
> and `steam.sh` start-up.

## Stage 0 — Instrumentation and a test harness (blocks everything) — **DONE, except the two play-session captures**

Built (VM era; deleted on 2026-09-27, ZERO-VM exit criterion 9):
- `src/display.m` writes one record per presented frame under `STEAMARM_TRACE`.
- `benchmarks/frametimes.py` — median / 1% low / 0.1% low / stddev / histogram.
- `benchmarks/capture.sh` — start with tracing on, wait, shut the guest down
  gracefully, analyse.
- libkrun `mod vmexit_stats` — exit rate, per-exit host cost, exception-class
  breakdown, under `LIBKRUN_VMEXIT_STATS`.

Recorded: a VM baseline for the Steam UI at idle (PERFORMANCE_BASELINE §2.1),
VM exit accounting (§2.2), guest syscall rate and mix (§2.3).

**Still open:** the same capture *during a game*, and the CrossOver comparison.
Both need a human playing a fixed scene; the harness for them exists and runs.

**Not built:** `tests/`. Deferred until Stage 2 has something to assert on —
there is no capability to test while the runtime does not exist.

## Stage 1 — Measure the one number that decides the architecture — **DONE**

`benchmarks/syscall_cost.c`, 481 lines, no product code. Results in
PERFORMANCE_BASELINE §2.4 and `benchmarks/README.md`.

**The exit criterion is met, and the answer changed the plan.**

1. **`svc` cannot be trapped on Darwin.** `svc #0` with `x16 = 20` returned our
   pid: Darwin ignores the SVC immediate and dispatches on `x16`. A Linux
   `svc #0` does not fault — it runs whatever Darwin syscall `x16` holds, with
   Linux arguments, silently. Trap-based interception is not a design option;
   it is not available at all.
2. **The trap-based mechanisms that do exist are far too slow anyway:** 2.79 µs
   via signal, 6.32 µs via Mach exception, against 114 ns for a native syscall.
3. **Rewriting is both mandatory and cheap:** a rewritten site that branches
   into the runtime dispatcher costs **68 ns** end to end including the real
   Darwin syscall — *less* than the `svc` it replaces — and **0.9 ns** when the
   runtime answers without the kernel.
4. **The VM exit it would replace is not the competition.** A guest syscall
   causes no VM exit at all; the guest kernel services it. VM exits are a
   separate, virtio-shaped cost: ~1100/s, ~10 µs host time each, ~1% of a core.

**Consequences now folded into Stage 2 and Stage 5:**

- The ELF loader must rewrite every statically discoverable `svc` site.
- FEX must be built to emit a **call into the runtime** instead of an `svc` for
  guest syscalls. Its JIT generates code at runtime, so the loader cannot reach
  it. This is a new, non-optional work item on the x86 critical path — and
  since Steam itself is x86 (ARCHITECTURE_CURRENT §1), it blocks Steam.
- `clock_gettime` is **70% of all guest syscalls** (§2.3) and a runtime that
  answers it in userspace turns the single commonest call into 0.9 ns. That is
  the first dispatcher entry worth writing.

## Stage 2 — Minimal Linux aarch64 ELF on Darwin — **DONE**

`runtime/` exists and runs. See `runtime/README.md`.

1. ~~static, freestanding aarch64 ELF that issues `write(1,...)` and `exit`~~
   **DONE.** `./build/lxrun build/hello-linux` prints from a Linux binary with
   no kernel and no hypervisor. `tests/elf/run.sh`: 6 tests, all passing.
2. ~~then a static glibc binary~~ **DONE.** A real Fedora glibc 2.42
   static-PIE binary runs under `runtime/` with working malloc, stdio,
   `snprintf` and `clock_gettime`. Two things had to be fixed, both instructive:
   - `HWCAP_CPUID` must **not** be advertised in the auxiliary vector. Linux
     emulates `mrs` reads of `MIDR_EL1`/`ID_AA64*`; Darwin does not, and glibc
     took SIGILL on `mrs x0, midr_el1` before its first syscall. Claiming a
     capability the runtime does not provide fails later and less clearly than
     not claiming it.
   - `fstat`, `fstatat`, `prlimit64`, `getrandom` and `set_robust_list` are
     needed by glibc start-up. `rseq` is the only call it still misses, and
     glibc is designed to fall back from its `-ENOSYS`.
3. ~~then dynamic + `ld-linux-aarch64.so.1`~~ **DONE.** A dynamically linked
   Fedora binary runs through the guest's own `ld.so`, which maps `libc.so.6`
   itself — and the runtime rewrites all 415 of its syscall sites as the
   mapping comes in. `PT_INTERP`, `AT_BASE`, `mmap`/`mprotect` rewriting hooks
   and an `LXRT_ROOT` guest filesystem are all in place
   (`benchmarks/stage2-dynamic.txt`).

**Added by Stage 1:** an `svc` rewriting pass in the loader, plus the
dispatcher measured in `benchmarks/syscall_cost.c`. Rewriting is not an
optimisation here — Stage 1 finding (1) means an un-rewritten `svc` executes an
arbitrary Darwin syscall rather than faulting, so a missed site is silent
corruption, not a crash. The loader must be able to prove it found them all, or
poison what it could not reach (an unmapped `x16` value traps as SIGTRAP, which
is slow but safe) rather than letting it run.

**The expected first wall was not the wall.** 4 KiB alignment never appeared:
all 3548 `PT_LOAD`s surveyed across a Fedora aarch64 system, plus FEX and
Proton's ARM64 wine, use `p_align 0x10000` — a clean multiple of Darwin's
16 KiB page (`benchmarks/stage2-elf-alignment.txt`).

**The real wall is `__PAGEZERO`.** Darwin reserves the low 4 GiB of every arm64
process and will not give it up: every `-pagezero_size` below the default is
SIGKILLed at exec, and `mmap`/`mach_vm_allocate` both refuse that range at
runtime (`benchmarks/stage2-pagezero.txt`). Linux ET_EXEC images link at
0x200000, inside it, with no relocations to move them. **Non-PIE Linux images
cannot be loaded in-process, and this is a hard cannot.** Cost measured: 96.7%
of `/usr/bin`, 100% of `/usr/lib64`, 100% of FEX and 94.4% of Steam's own
binaries are PIE, so the exclusion is narrow — it is the GCC toolchain
(`benchmarks/stage2-elf-survey.txt`).

Second surprise, in the good direction: the `svc` scan agrees **exactly** with a
full disassembly on real glibc (415 sites) and `ld-linux` (54), with no false
positives (`benchmarks/stage2-rewrite-validation.txt`). And `clock_gettime` —
70.6% of all guest syscalls — costs **12.6 ns** end to end through the rewritten
path.

**Exit criterion: met.** `hello` runs, a static glibc binary runs, a dynamic
glibc binary runs through its own `ld.so`, `tests/elf/` green at 11/11.

The one non-obvious cost, worth carrying into Stage 5: a trampoline must sit
within `b` range (+/-128 MiB) of the code it serves, and the guest's loader
reserves large spans it then fills in. Placing a pool adjacent to a mapping put
it inside such a span and the loader mapped over the live trampolines, which
surfaced as a SIGSEGV deep inside libc. Pools are now placed distant-first and
every one is tracked, with a test asserting none is overwritten.

## Stage 3 — Threads, TLS, futex, signals

The order matters: nothing above `pthread_create` works until TLS does.

**TLS: DONE, and Stage 2's answer was wrong.** Stage 2 reported that
`TPIDR_EL0` was free for the guest. It is not: Darwin preserves it across a
syscall but clobbers it on any context switch, so a guest that gets descheduled
loses its thread pointer — silently. Measured and corrected in
`benchmarks/stage3-tls.txt`.

The fix reuses the `svc` mechanism: every `mrs Xt, TPIDR_EL0` and
`msr TPIDR_EL0, Xt` is rewritten to a trampoline backed by a Darwin TSD slot
(3 instructions, no scratch register, 2.34 ns). That is 1440 sites in glibc
against 415 syscall sites — TLS reads outnumber syscalls 3.5 to 1. Proven both
ways on a real Fedora glibc binary: preserved across 300 context switches with
rewriting, SIGSEGV inside libc without it.

- ~~TLS with two thread pointers coexisting~~ **done**, including
  `clone(CLONE_SETTLS)` per thread;
- ~~`clone`/`clone3` mapped onto Darwin threads owned by the runtime~~ **done**;
- ~~`futex` on `__ulock_wait/wake`~~ **done for WAIT/WAKE**. Requeue, WAKE_OP
  and the PI futexes have no Darwin primitive and return `-ENOSYS` rather than
  a wrong answer; robust lists are accepted and ignored. Revisit when something
  real needs them;
- ~~`rt_sigaction` / `sigaltstack` / `ucontext_t`~~ **done.** Real delivery:
  signal-number translation both ways, a Linux `siginfo_t` and `ucontext_t`
  built on the guest stack, `sigaltstack`, `rt_sigprocmask`, `setitimer`,
  `ppoll`, `rt_sigsuspend`, and a return path through the runtime because `svc`
  cannot be used for `rt_sigreturn`. A real glibc binary passes all four checks
  — raise, context preserved across the handler, blocking, asynchronous
  `SIGALRM` — matching Linux (`benchmarks/stage3-signals.txt`).

  Three deviations are documented rather than hidden: `uc_mcontext.pc` is a
  host address when a signal arrives inside a syscall (matters for Stage 5,
  FEX inspects it); `x16` is not restored by `rt_sigreturn`; realtime signals
  have no Darwin carrier, which blocks glibc's thread cancellation.

**Exit criterion: met.** A real Fedora glibc binary with 4 pthreads,
a mutex, a condition variable, per-thread `__thread` state and `pthread_join`
runs correctly 15 times out of 15, at **55.8 ms against 56 ms** for the same
binary on a real Linux kernel — no measurable difference
(`benchmarks/stage3-threads.txt`).

Two silent bugs found on the way, both worth carrying forward:
**Darwin and Linux errno numbers differ** (EAGAIN/EDEADLK are swapped, ENOSYS is
78 vs 38), which killed glibc's `clone3` → `clone` fallback; and **clone's child
inherits the entire register file**, not just the syscall-ABI-preserved half.

## Stage 4 — Vulkan without a VM — **step 1 DONE, and the overhead is zero**

1. ~~an ELF Vulkan app reaches MoltenVK in-process~~ **DONE.** A Fedora glibc
   static-PIE binary opens MoltenVK through a private dlopen/dlsym syscall pair,
   creates an instance and enumerates the GPU as **`Apple M4`** — not
   `Virtio-GPU Venus (Apple M4)`. Measured: a direct guest→host call costs
   **0.8 ns**, a real `vkGetPhysicalDeviceProperties` **11.6 ns**
   (`benchmarks/stage4-vulkan.txt`).

   Option (1) from ARCHITECTURE_TARGET §3.4 is confirmed and options (2) and (3)
   are unnecessary: no ELF build of MoltenVK, and above all **no RPC helper**.
   There is no boundary to pay for — one address space, one calling convention.

2. ~~an ELF `libvulkan.so.1` shim~~ **DONE.** 431 entry points, each a
   four-instruction signature-agnostic tail call into MoltenVK, generated by
   `shim/gen.py` and built on the host with clang+lld. A plain Fedora binary
   compiled with `-lvulkan`, resolved by the guest's own `ld.so`, creates an
   instance and enumerates `Apple M4` knowing nothing about the runtime. The
   thunk costs **0.4 ns** (`benchmarks/stage4-shim.txt`);
3. ~~presentation: a surface backed by a `CAMetalLayer` the runtime owns~~
   **DONE.** `runtime/window.m` owns an `NSWindow` + `CAMetalLayer`; a private
   syscall hands the layer to the guest, which passes it to
   `vkCreateMetalSurfaceEXT`. A Fedora binary builds a 3-image swapchain and
   runs acquire/clear/present at a **8.62 ms median (116 fps)**, 1% low
   11.6 ms. **The three full-frame copies per displayed frame are now zero** —
   the swapchain images are the layer's drawables
   (`benchmarks/stage4-present.txt`).
4. ~~`vulkaninfo`~~ **DONE.** Fedora's own unmodified `vulkaninfo` runs to
   completion — 1199 lines, 296 extensions, `DRIVER_ID_MOLTENVK`. It dlopens
   `libvulkan.so.1`, so it reaches the shim with no cooperation of its own. It
   also confirms MoltenVK's feature gaps are unchanged, which is why the DXVK
   patch still applies (`benchmarks/stage4-vulkaninfo.txt`);
5. ~~triangle → a real render pass with shaders~~ **DONE.** A Fedora binary
   builds image views, a render pass, framebuffers, two SPIR-V shader modules
   and a graphics pipeline, and draws a triangle at a 6.84 ms median on a
   165 Hz display. **Pipeline compilation cost 352.5 ms cold and ~2 ms warm** —
   Metal has an on-disk shader cache. PERFORMANCE_BASELINE §2.5 listed
   shader-compilation stalls as unmeasured; these are the first numbers, and
   they confirm the other half of that entry too: removing the VM did nothing
   for it (`benchmarks/stage4-triangle.txt`);
6. a pipeline cache, then DXVK — **next**.

**Stage 4 exit criterion: substantially met.** The whole chain runs with no
Linux kernel and no hypervisor: ELF loading → syscall and TLS rewriting →
threads → signals → dynamic linking → an ELF Vulkan loader → MoltenVK → Metal
→ a window on screen. What is not yet proven is a *real* frame: no vertex
buffers, no descriptor sets, no textures, and DXVK has not been run.

**Exit criterion:** `vkcube` renders through the runtime with no VM, and the
overhead versus native is quantified. Step 1 already quantifies the *boundary*
at 0.8 ns; what remains to measure is the whole pipeline.

## Stage 5 — FEX for x86-64, on the runtime — **exit criterion met (2026-09-25): x86-64 runs, FEX overhead isolated at 1.0x; the VM path is 4.5x slower on the same work**

**FEX executes under the runtime.** 625663 words scanned, 242 `svc` sites, 40
TLS sites and 1 `CTR_EL0` site rewritten; it links through the guest's own
`ld.so`, runs its start-up, parses its config, and stops inside
`FEXServerClient` — its own code reaching its own requirements
(`benchmarks/stage5-fex.txt`).

Two findings gate the rest, and one of them is the first thing this
architecture does not simply absorb:

1. **`mrs CTR_EL0` traps on Darwin**, for native Mach-O code too. Every icache
   flush reads it, and a JIT does so constantly. Now rewritten to a synthesised
   constant built from `hw.cachelinesize`. The cache instructions themselves
   (`dc cvau`, `ic ivau`, `dsb`, `isb`) are allowed
   (`benchmarks/stage5-sysreg.txt`).
2. **RWX is refused; W^X works.** `mmap` and `mprotect` both reject
   write+execute, but a write → `mprotect RX` → run → `mprotect RW` cycle runs
   correctly 200 times with freshly generated code. A JIT that supports W^X
   works; one that insists on RWX does not. FEX's mode is untested.

**Five walls cleared, one of them (JIT memory, item 4) ending in the project's first — and so far only — patch to FEX.** It now starts
FEXServer, connects to it, detects the host correctly, sets up its signal
handlers and its memory, and gets past everything previously reported as
blocking. It currently stops blocked after installing four signal handlers;
where exactly is not yet diagnosed.

1. **The ID registers.** Darwin traps every `ID_AA64*` and `MIDR_EL1` read at
   EL0; FEX reads eleven of them. Now rewritten to constants built from the
   real feature set macOS publishes via sysctl — 23 sites in FEX. Pointer
   authentication and SME are reported absent on purpose
   (`benchmarks/stage5-idregs.txt`).
2. **The 128 TiB reservation was the runtime's bug, not FEX's.** FEX probes the
   host's VA width and accepts `EEXIST` as proof a width exists; the runtime
   returned `EEXIST` for every `MAP_FIXED_NOREPLACE` failure, including
   "address does not exist". FEX concluded 57-bit addressing and tried to fence
   off `[2^47, 2^48)`. With `EEXIST` and `ENOMEM` distinguished, it detects 47
   bits and skips it. `benchmarks/stage5-va.txt` is corrected.
3. **`si_addr` was missing from the signal frame.** FEX's SIGSEGV handler maps
   pages on demand; with a zero fault address it could fix nothing, returned,
   faulted at the same pc, and looped 1321 times until the stack was gone.
4. **JIT memory.** Apple Silicon gives no process write+execute on one page —
   `MAP_JIT` makes it a per-thread switch, verified. FEX has no W^X mode. The
   first design drove the switch from the faults; **that design is dead,
   measured**: a `pthread_jit_write_protect_np` issued inside a signal handler
   does not persist past the handler's return, so the retried store faults
   forever (`benchmarks/stage5-jit.txt`, CORRECTION). What does work, also
   measured (`benchmarks/wx_in_handler.c`): a flip requested by the guest on
   its own stack, and a symmetric flip-write-flip inside a handler. So **the
   guest asks**: private syscall `0x4C580020` (x8), `(enable, addr, len)` —
   0 opens the thread for writing, 1 closes it for execution and rescans
   exactly the range the guest says it emitted (where `svc` in JIT output gets
   rewritten — the code no load-time pass can see), 2 rescans and stays
   writable for nested scopes. This is the project's first patch to FEX and it
   is the "small patch" rung: a 104-line header (`patches/LxrtJit.h`) and 34
   lines of scopes at the eight sites that write into executable memory (two
   code-buffer `memcpy`s, four block-link stores, the dispatcher emit, the
   SIGBUS backpatcher — `patches/fex-lxrt-wx.patch`). On a real Linux kernel
   the syscall number returns `-ENOSYS`, the scope latches to a no-op, and the
   same binary runs both places. Proven without FEX by `tests/elf/jit_wx.c`
   (check #17): 5/5 native and 5/5 under the runtime, with the JIT-emitted
   `svc` rewritten on the execute flip.
5. **Realtime signals.** Linux has 32, Darwin none, and FEX uses them. They are
   now carried: the number is queued against the target thread and `SIGEMT`
   wakes it — the one Darwin signal no Linux signal maps onto, so it cannot
   collide with a guest handler.
6. **4 KiB guest pages on 16 KiB host pages — the wall this document predicted
   in PERFORMANCE_BASELINE §5.** Stage 2 found aarch64 binaries all use
   `p_align 0x10000` and concluded x86 would be fine "because under FEX guest
   addresses are not host addresses". That was wrong: FEX maps the guest's
   segments at their own addresses with the host's `mmap`, and
   `mmap(0x108259000, 0x1000, R|X)` is `EINVAL` on Darwin. `runtime/subpage.c`
   backs the enclosing host pages anonymously, `pread`s file content into
   place, and gives each host page the union of the guest protections inside
   it. FEX now maps the x86-64 ELF (`benchmarks/stage5-subpage.txt`).

Also fixed for FEX: `personality`, `getcpu`, and `/proc` openable as a
directory.

**Where it is now:** the patched FEX runs an x86-64 static ELF to completion
under the runtime — `hello from x86-64, translated by FEX, on macOS, with no
VM`, exit 0, 7 W^X flips, 269 syscalls, 10–40 ms wall against 10–16 ms for
native FEX inside the VM (`benchmarks/stage5-fex.txt`). The last wall after
W^X was a 4 KiB guard page inside a 16 KiB host page (FEX's temporary code
buffer), fixed in `runtime/subpage.c` by adopting untracked neighbours before a
sub-page `mprotect`. Not yet proven: dynamic x86-64 through FEX's rootfs,
threads under FEX, translation throughput on a real workload.

**Blocker A (resolved) — Linux abstract AF_UNIX sockets.** FEX's first connect is
to `\0 501.FEXServer.Socket`. Darwin has no filesystem-free socket namespace.
It is no longer refused: abstract names are materialised as real sockets under
`$TMPDIR/lxrt-abstract-<uid>/`, because both ends are guest code going through
the same translation. What is lost is auto-cleanup on last close and isolation
from anything else using that directory (`benchmarks/stage5-process.txt`).

**Blocker B (resolved) — FEX spawns FEXServer as a separate process.** The runtime
now has one: `fork` is Darwin's `fork`, and `execve` re-executes the runtime on
the target binary, so the child gets a fresh rewriting pass
(`benchmarks/stage5-process.txt`). The decision this forced still stands and is
not undone by having implemented it: **a second process is a second runtime**,
and none of what the graphics bridge gains from a single address space — the
0.8 ns direct call above all — carries across that boundary.

Also implemented on the way, because FEX needed them: `mkdirat` (it called it
40335 times against `-ENOSYS`), `getcwd` (Linux returns a length, not a
pointer), `prctl`, and the socket family with full `sockaddr` translation.
**And a latent bug of the same class as errno was found and fixed: `open()`
flags differ between Linux and Darwin** — Linux `O_APPEND` (0x400) reads as
Darwin `O_TRUNC|O_EXCL`, which truncates the file it was meant to append to.
Every path now goes through `runtime/fsflags.c`.

---

### Original plan for this stage

FEX is aarch64 and runs on Stage 2–3. Then:

1. **patch FEX to emit a call into the runtime dispatcher instead of `svc`** —
   Stage 1 finding (1); its JIT generates syscall sites at runtime that the
   loader's rewriting pass can never see;
2. a static x86-64 Linux binary;
3. dynamic + glibc;
4. threads, signals, JIT memory (`MAP_JIT`, `pthread_jit_write_protect_np`).

Item 1 is on the critical path for Steam, which is i386/x86-64
(ARCHITECTURE_CURRENT §1). It is also the largest fork risk in the project:
per the minimise-forks rule, try FEX's existing syscall-handler abstraction
before patching its code emitter.

**Exit criterion:** an x86-64 Linux binary runs, FEX overhead isolated from
runtime overhead. **Met** (`benchmarks/stage5-x86-throughput.txt`): the same
freestanding program four ways — native aarch64 in the VM 1185–1256 ms,
x86-64/FEX in the VM 1304–1338 ms, native aarch64 under the runtime 278–300 ms,
x86-64/FEX under the runtime 280–291 ms. FEX costs 1.0× on macOS; the VM
costs 4.3–4.5× on both. Dynamic x86-64 through FEX's rootfs also runs
(`uname -m` → `x86_64`, `benchmarks/stage5-fex.txt`). Still owed: threads and
fork/exec under FEX.

## Stage 6 — Steam

Steam is i386 + x86-64 (ARCHITECTURE_CURRENT §1), so it depends on Stage 5.
Order: start → login/UI → networking → library → download → launch a game.

~~Investigate what pressure-vessel actually requires before implementing any
namespace support.~~ Investigated. **THE PRESSURE-VESSEL DECISION (2026-09-25):**

*Evidence* (`benchmarks/stage6-bwrap-plan.txt`, from Steam's own logs on the
guest):

1. **The Steam UI does not need bwrap.** On the guest itself `srt-bwrap` fails
   every single time — `bwrap: Unexpected capabilities but not setuid, old file
   caps config?` — and `steamwebhelper.sh` falls back to `exec ./steamwebhelper`
   with the sniper runtime on `LD_LIBRARY_PATH`. `steamwebhelper_sniper_wrap.sh`
   on this install is four lines and execs the helper directly. What the ftrace
   saw as "the UI goes through pressure-vessel" is `pv-adverb`, which sets up
   environment and locks and needs no namespace. So the client's own path is
   the fallback path, already proven on Linux, and the runtime only has to let
   bwrap fail *cleanly* (it does: `unshare` is `-ENOSYS`).
2. **A game launch asks bwrap for a mount plan, not for isolation.** The full
   plan pressure-vessel-wrap generated for a real launch is 47 options plus two
   probe invocations: `--ro-bind` ×19, `--bind` ×18, `--symlink` ×4,
   `--ro-bind-data` ×3, `--tmpfs` ×2, `--dev-bind` ×1, and in the probes
   `--dir`, `--proc`, `--setenv`, `--chdir`, `--new-session`, `--perms`,
   `--level-prefix`, `--not-a-security-boundary`. **No `--unshare-*`, no
   `--cap-*`, no `--uid/--gid`, no seccomp fd.** Every destination is a path
   (`/run/host/*`, `/run/pressure-vessel/*`, `/usr`, `/etc`, `~`, `/tmp/.X11-unix`,
   `/run/user/1000/*`, `/dev/snd`). The only namespace involved is the mount
   namespace bwrap creates for itself to realise those binds.

*Decision:* **(a)+(c) — a fake `bwrap` that interprets the plan as a
per-process path-translation table inside the runtime.** Not (b): bypassing
pressure-vessel loses the Steam Runtime sysroot that Proton is built against.
Not real namespaces: they cannot exist on Darwin and, per the evidence, are
not what is being asked for.

*Shape of the work* (Stage 6, after the fd families):
- `--ro-bind SRC DST` / `--bind` / `--dev-bind` / `--ro-bind-try` → a prefix
  entry `{DST → SRC, ro}` in the child's translation table, consulted by the
  same `translate()` that already serves `LXRT_ROOT` and `/proc`. Longest
  prefix wins, exactly bwrap's later-mount-shadows-earlier order.
- `--symlink TARGET DST` → a synthetic symlink entry (readlink answers TARGET).
- `--tmpfs DST` / `--dir DST` → a fresh host temp directory mapped at DST.
- `--proc DST` → procfs.c, already synthetic. `--dev DST` → passthrough.
- `--ro-bind-data FD DST` → materialise the fd's bytes into a temp file at DST
  (pressure-vessel feeds it `font-dirs.xml` and friends).
- `--setenv`, `--chdir`, `--new-session`, `--perms`, `--level-prefix`,
  `--not-a-security-boundary`, `--sync-fd`, `--info-fd` → trivial or ignored.
- `--unshare-*`, `--cap-*`, `--uid`, `--gid`, `--seccomp`, `--add-seccomp-fd`
  → refused loudly. The evidence says they are never asked for; if a future
  pressure-vessel asks, the refusal must say so rather than pretend.
- The table is inherited across `fork`/`execve` (process.c) — bwrap's child
  and grandchildren all see the same view.
- pressure-vessel hands the plan over as `bwrap --args FD` (its own strings:
  `"bwrap --args %d = ..."`, `"Error replacing self with bwrap"`), so the
  interpreter reads NUL-separated options from that fd. **Built 2026-09-25**
  as `runtime/mounts.c`: execve of anything named `bwrap`/`srt-bwrap` is
  intercepted, the plan materialised into a fresh sandbox root (dirs,
  symlinks, data files) plus a bind table carried to children in
  `LXRT_MOUNTS`, consulted by `translate()` before the root prefix.

**THE OTHER STAGE 6 DECISION — GUEST ADDRESSES BELOW 4 GiB (2026-09-25):**
FEX runs guests at their own addresses, and Darwin's `__PAGEZERO` makes the
low 4 GiB of every arm64 process unmappable (`benchmarks/stage2-pagezero.txt`).
Measured consequence (`benchmarks/stage5-fex.txt`): a non-PIE x86-64 ELF
(`python3.12`, linked at 0x400000) fails to load with ENOMEM. The Steam client
itself (`ubuntu12_32/steam`) is a 32-bit PIE — PIE does not help a 32-bit
guest, whose whole address space is below 4 GiB by construction — as is every
32-bit Windows game under Wine. Census on the guest (`file` over Steam's
tree): the client i386 PIE, `steamwebhelper` x86-64 PIE, both `steamclient.so`
shared objects, and 16 x86-64 non-PIE executables among the tools — the ones that matter being `steam-launch-wrapper` (both copies), `hardwareupdater`, `disk-free` and `gldriverquery`; `steam-launch-wrapper` is on every game's launch path.
**And the whole pressure-vessel chain is non-PIE** (measured on the copied
tree with an ELF-header parse): `pressure-vessel-wrap`, `srt-bwrap`,
`steam-runtime-launcher-service` and `x86_64-linux-gnu-exec` are all
`ET_EXEC` linked at `0x400000`. The `steamwebhelper.sh` → `_v2-entry-point`
→ `run` → `pressure-vessel-wrap` chain therefore dies at the first re-exec
(`E MapFile: Some elf mapping failed, 12`) even though `steamwebhelper` itself
is PIE. **The guest address base is on the critical path for every Steam
launch, 64-bit included** — not only for the i386 client. 64-bit PIE
and 64-bit Windows images (base 0x140000000) are unaffected — `steamwebhelper`,
`uname`, `ls`, `bash`, `env` all run today.

Options, evidence-weighted:
1. **A guest address base in FEX** — map the guest at `host = guest + BASE`
   (BASE ≥ 4 GiB, held in a reserved register), add it at every emitted
   memory access (ARM's `[Xn, Xm]` addressing makes the add free where it can
   be folded), and adjust every pointer crossing the syscall/signal boundary
   (FEX already marshals those). This is FEX's 32-bit-guest address-space
   machinery generalised by one constant; a real patch, centralised in the
   JIT's load/store emission and the syscall layer — sized on the source:
   `GenerateMemOperand` has 15 call sites in `MemoryOps.cpp` and 2 in
   `VectorOps.cpp`, plus 41 other `MemOperand` uses. **Recommended**, because it
   is the only option that also gives i386, and i386 is the Steam client.
2. Run only 64-bit PIE code and treat the i386 client as out of scope — not
   viable: there is no 64-bit Steam client for Linux.
3. Relocate ET_EXEC images — impossible; they carry absolute addresses.

Not started here. It is the largest single FEX change in the plan and must
be a fork-level patch with its own benchmark (`x86_bench` with BASE vs
without) before it is adopted.

*Patch shape, sized on FEX's source (2026-09-25):* every guest memory
operand is built in two functions of `FEXCore/Source/Interface/Core/JIT/
MemoryOps.cpp` — `GenerateMemOperand` (an addressing mode `[Base, Offset]`,
15 call sites) and `ApplyMemOperand` (an `add` into a temporary, for
instructions without a suitable mode), plus 2 sites in `VectorOps.cpp` and
41 other `MemOperand` uses. The decisive fact is an ARM64 addressing mode:
`ldr x0, [xBASE, wADDR, uxtw]` folds a 64-bit base plus a zero-extended
32-bit index into one instruction. **For a 32-bit guest — the Steam client —
the base is free**: keep the guest's 32-bit addresses as they are, reserve
one register for `BASE = 0x1_0000_0000` (the first byte above `__PAGEZERO`),
and every access becomes `[xBASE, wADDR, uxtw]`; FEX's existing 32-bit mode
already keeps guest pointers in 32 bits. For 64-bit non-PIE images (the five
Steam tools) it costs one `add` per access, which is why 64-bit stays
unbased by default and only images that need it get the base. Outside the
JIT: the ELF loader maps at `vaddr + BASE`; `mmap`/`mremap`/`brk` results
and every pointer FEX's syscall layer marshals (it already copies structures
for 32-bit guests) subtract/add BASE; `si_addr` in signal frames likewise.
The first benchmark is `x86_bench` built `-m32`, run with BASE against
native FEX in the VM.

*What this does not give:* isolation. Nothing here protects the host from the
game; bwrap's `--not-a-security-boundary` flag in the real plan says
pressure-vessel already knows that.

**Exit criterion:** Steam reaches the library view with no VM.

## Stage 7 — Proton, DXVK, VKD3D

Minimal wine binary → simple Windows app → graphical app → DXVK test →
VKD3D test. Keep bug attribution separate: runtime / FEX / wine / Vulkan /
MoltenVK.

The DXVK feature patch from today applies unchanged.

## Stage 8 — Real games

Small game → D3D11 → D3D12 → CPU-heavy → GPU-heavy. Correctness before tuning.

## FINAL ZERO-VM EXIT CRITERIA (set by the project owner, 2026-09-26)

The project is NOT finished while any mandatory dependency on the VM remains.
It is complete only when all of these hold:

1. Steam runs without the VM.
2. FEX x86-64 runs without the VM.
3. FEX i386 runs without the VM.
4. Linux ARM64 runs through the Darwin runtime without the VM.
5. Proton/Wine runs without the VM.
6. DXVK/VKD3D → Vulkan → MoltenVK → Metal works without the VM.
7. Real games work without the VM.
8. FEX and every needed component can be BUILT without the VM.
9. Removed from the project: libkrun, kernel/initramfs, qcow2, virtio*, Venus,
   virglrenderer, gvproxy, virtualisation-specific patches, and every
   VM-only script/configuration.
   Status (2026-09-27): MET. Deleted from the project (not archived):
   src/ (the libkrun front-end: virtio-gpu/input,
   gvproxy networking), guest/, payload/ (kernel Image/vmlinuz, initramfs),
   scripts/{build-virtgpu.sh,patch-virtgpu.py,extract-kernel.sh,
   mkaarch64extras.sh}, tests/elf/mkroot.sh, resources/{SteamARM.entitlements,
   Info.plist}, benchmarks/{capture.sh,frametimes.py,runs/vm-baseline-*},
   build/{SteamARM,obj}. Makefile: the VM app targets are gone and
   `all` is `lxrt shim launcher`. The Venus ICD (virtio_icd.json) is deleted
   by scripts/fetch-x86-rootfs.sh's fixups and was deleted from the live
   roots. What still names libkrun/virtio/Venus/qcow2/initramfs is historical
   documentation only (the ARCHITECTURE_*/PERFORMANCE_BASELINE/this plan,
   benchmarks/*.txt measurement notes) plus that ICD-deleting fixup.
10. A clean Mac can build → install → launch Steam → launch a game without
    creating, starting or depending on any VM.

Only then: ZERO-VM = COMPLETED.

Status against the list (2026-09-26, MEASURED unless marked):
- 2, 3, 4: met (tests/elf/run.sh 32/32; i386/x86-64 probes; FEX under lxrun).
- 1: partial — client starts, updates, spawns helpers and steamwebhelper
  (Chromium) inside pressure-vessel; the UI has not drawn yet.
- 5, 6, 7: not started (Stage 7/8).
- 8: mostly met — everything the runtime needs can be built or fetched on
  the Mac with no VM (benchmarks/stage9-vmfree-build.txt, MEASURED):
  FEX/FEXServer/FEXGetConfig from source (scripts/build-fex-host.sh, 102 s,
  bit-identical Release outputs; the installed default since 2026-09-26);
  the aarch64 root with Xvnc/Mesa/xkb/bash and the test programs from 71
  pinned Fedora 43 RPMs (scripts/mkroot-rpm.sh + .lock, 100 s; 331 files
  byte-identical to the VM-made root, the rest newer package builds; ELF
  suite 32/32 on it); the x86 rootfs from FEX's own image index
  (scripts/fetch-x86-rootfs.sh, 19 s; all 39,553 entries identical).
  Still open: the live roots in ~/SteamARM-roots are the VM-made ones (not
  swapped while Steam runs from them), and the Steam install inside the
  Steam root was copied from the VM (Steam's bootstrap re-fetches it).
- 9: met (2026-09-27), see the status under item 9.
- 10: not done.

## Stage 9 — `ZERO_VM=1`, then default, then delete

Add a build configuration that compiles the runtime with **no** libkrun, no
kernel payload, no virtio. CI must keep it buildable; a change that reintroduces
a mandatory VM dependency is an architectural regression unless it is explicitly
a development tool.

Only once ZERO-VM passes the Stage 0 benchmarks does the VM code get removed.

---

## Milestone 1 — **DELIVERED** (2026-09-23), one item outstanding

**"Measure before rearchitecting."** Stage 0 + Stage 1 together.

| Deliverable | Status |
|---|---|
| `benchmarks/` producing median / 1% / 0.1% frame times | done |
| VM baseline for a fixed scene | done for the Steam UI at idle; **the in-game capture needs a play session** |
| CrossOver baseline for the same scene | **outstanding — needs a play session** |
| syscall-interception microbenchmark vs. a VM exit | done |
| `PERFORMANCE_BASELINE.md` with real numbers | done |

It paid for itself: Stage 1 found that the mechanism the target architecture
assumed — trapping `svc` — **does not exist on Darwin**, at a cost of one
afternoon rather than after months of loader work.

## Milestone 2 — proposed

**"Prove the rewriting path on a real binary."** Stage 2, steps 1–2, plus the
dispatcher entries for the head of the measured syscall distribution
(`clock_gettime`, `ppoll`, `futex`, `read`, `write`, `exit_group`).

Deliverables:
1. `runtime/elf` mapping a static aarch64 `PT_LOAD` set on 16 KiB pages — this
   is where the 4 KiB/16 KiB conflict (PERFORMANCE_BASELINE §5) is settled or
   the plan dies;
2. the `svc` rewriting pass, with a report of sites found and sites poisoned;
3. the dispatcher from `benchmarks/syscall_cost.c` promoted to product code;
4. a static Linux `hello` running with no VM, under `tests/elf/`.

Entry condition: none. Exit condition: a Linux aarch64 binary prints to stdout
on macOS with no kernel, no hypervisor, and no `svc` reaching XNU unrewritten.

## Risk register

| Risk | Stage | If it happens |
|---|---|---|
| 4 KiB ELF segments unmappable on 16 KiB pages | 2 | ZERO-VM for arbitrary Linux binaries is blocked; report it |
| Syscall interception costlier than a VM exit | 1 | Re-examine; the graphics argument may still carry the plan |
| MoltenVK cannot be loaded in-process from an ELF consumer | 4 | Falls back to RPC, which negates much of the benefit |
| FEX JIT incompatible with macOS W^X | 5 | ~~x86 support blocked → Steam blocked~~ **Mitigated**: guest-driven flip via syscall `0x4C580020`, 34-line FEX patch, mechanism proven by `tests/elf/jit_wx.c` |
| Proton ARM64 stays broken | 7 | Fall back to x86-64 Proton under FEX, as today |
