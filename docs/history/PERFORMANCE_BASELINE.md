# PERFORMANCE_BASELINE

> **SUPERSEDED — VM-era.** These are the VM baseline and the first
> ZERO-VM measurements, taken while the VM still existed (8 vCPU guest). The
> "Status: CURRENT" line below is kept as it was written. The ZERO-VM
> baseline is `docs/PERFORMANCE_BASELINE.md`.

**Status: CURRENT.** Milestone 1 (Stage 0 + Stage 1) has been executed and
most of what §2 listed as unmeasured is now measured. The brief's hypothesis —
"the VM is the bottleneck" — is **refuted in its naive form and replaced by a
sharper one**: see §2 and §3.

Host: Apple M4, 4P + 6E cores, 16 GB, macOS 27. Guest: 8 vCPU, 8192 MiB.

---

## 1. Measured

### Presentation path (`src/display.m`, counters in-tree)

| Condition | Frames | Time per present | Effective rate |
|---|---|---|---|
| Steam UI, vsync on | 600 | 0.90–1.24 ms | 60 fps (vsync-locked) |
| Steam UI, vsync off | 600 | 0.70–0.94 ms | 88–160 fps |
| vkcube fullscreen | 600 | — | **165 fps** |

`displaySyncEnabled = NO` + `maximumDrawableCount = 3` lifted the host from a
hard 60 to 88–160 fps.

**Conclusion: presentation is not the bottleneck.** At ~0.8 ms it could sustain
>1000 fps. This is a measured fact and it *weakens* the naive form of the
"VM overhead" hypothesis — the part of the VM everyone assumes is slow is not.

### Game

`Schedule I` (Unity 2022.3, D3D11), launched from Steam, in-game:

```
DXVK v1.10.3+
Virtio-GPU Venus (Apple M4)
Driver: 26.1.6      Vulkan: 1.1.357
FPS: 58.7
```

58.7 fps is **against a 60 Hz vsync**, so it is a floor, not a ceiling. No
uncapped in-game number has been captured yet.

### Guest GPU identity

```
deviceName = Virtio-GPU Venus (Apple M4)
driverName = venus
apiVersion = 1.4.307 (aarch64 clients) / 1.3.357 (x86-64 clients under FEX)
```

### Known per-frame cost in the display path

Three full-frame copies at 1080p ≈ 24 MB/frame:
guest RAM → `host_backing` → display frame → `MTLTexture`.
At 60 fps that is ~1.4 GB/s of memcpy. Its measured cost is inside the
0.7–1.24 ms above, so it is real but small relative to a 16.7 ms frame.

---

## 2. Measured in Milestone 1 (2026-09-23)

> Note (2026-09-27): the VM harness (src/, benchmarks/capture.sh,
> benchmarks/frametimes.py, benchmarks/runs/vm-baseline-*) has been deleted;
> the numbers below remain the VM baseline.

Harness: `benchmarks/` in-tree. Frame trace written by `src/display.m` under
`STEAMARM_TRACE`; VM exit accounting by `mod vmexit_stats` in libkrun's
`src/hvf/src/lib.rs` under `LIBKRUN_VMEXIT_STATS`.

### 2.1 Frame-time distribution — VM baseline, Steam UI idle

`benchmarks/runs/vm-baseline-01`, 2335 frames over 37.4 s of steady state
(45 s of boot and Steam start-up discarded):

| | |
|---|---:|
| median frame time | **16.25 ms** (61.5 fps) |
| 1% low | **25.06 ms** (39.9 fps) |
| 0.1% low | **25.70 ms** (38.9 fps) |
| mean / stddev | 16.04 / **4.42 ms** |
| min / max | 1.07 / 25.87 ms |
| present cost, median | 1.13 ms (**6.9%** of the frame) |

The median sits on a 60 Hz beat even though the host lifted its own vsync
(`displaySyncEnabled = NO`). The cap is now the **guest compositor**, not the
host presentation path.

The tail is the interesting part: the worst 1% of frames take 25 ms against a
16 ms median, a 54% excursion, with a 4.4 ms standard deviation on an idle UI.
That is measurable judder before a game is even running.

### 2.2 VM exit rate and cost

```
[vmexit] 44856 exits, 1272/s | host 9.60 us/exit | ec=0x24:44831 ec=0x18:16 ec=0x17:9
```

- **~1000–1270 exits/second** at idle.
- **99.94% are `ec=0x24`, data aborts** — i.e. MMIO, which here means virtio
  doorbells. 16 system-register traps and 9 others in the whole run.
- **~10–16 µs of host time per exit.** Measured directly against wall clock:
  **4.1 ms of every wall-clock second**, falling as the idle window widens
  (1272 exits/s early in the run, 258/s once Steam settles). Under 1% of one
  core either way.

Caveat, stated because the raw counter invites the wrong reading: the
"guest-in-run" figure is every vcpu's time inside `hv_vcpu_run`, and 8 vcpus
park there for whole seconds when idle. It is not time doing useful work, and
no guest-versus-host *share* should be computed from it.

### 2.3 Guest syscall rate and mix

ftrace `raw_syscalls:sys_enter` histogram, whole guest, two 10 s windows:
42153 and 43189 hits → **~4300 syscalls/second at idle**, 68 distinct calls.

| id | name | share |
|---:|---|---:|
| 113 | `clock_gettime` | **70.6%** |
| 212 | `recvmsg` | 6.3% |
| 73 | `ppoll` | 5.7% |
| 98 | `futex` | 4.4% |
| 178 | `gettid` | 1.6% |
| 63 | `read` | 1.4% |
| 22 | `epoll_pwait` | 1.0% |
| 172 | `getpid` | 0.9% |

Nine calls cover 92% of everything. Full table in
`benchmarks/stage1-syscall-mix.txt`.

`clock_gettime` dominating at 70% is a FEX artefact: on native Linux the vDSO
answers it without entering the kernel, but the x86 client under FEX cannot use
the guest vDSO, so every one is a real syscall.

### 2.4 Cost of intercepting one Linux syscall on Darwin — the Stage 1 number

Full method and output in `benchmarks/README.md` and
`benchmarks/stage1-syscall-cost.txt`.

**Finding first, because it outranks the timings: `svc` cannot be trapped on
Darwin.** `svc #0` with `x16 = 20` returned our own pid — Darwin ignores the
SVC immediate and dispatches on `x16`. A Linux binary's `svc #0` therefore does
not fault; it executes whatever Darwin syscall happens to be in `x16`, with
Linux arguments. Silently.

| mechanism | ns/op |
|---|---:|
| indirect function call (floor) | 1.3 |
| Darwin syscall, `svc #0x80` | 114.2 |
| in-process signal interception (SIGTRAP) | **2794.2** |
| Mach exception interception | **6317.0** |
| rewritten `svc` → dispatcher → real syscall | **68.1** |
| rewritten `svc`, answered in userspace | **0.9** |

Applied to the measured 4300 syscalls/s, per second of one core:

| mechanism | overhead |
|---|---:|
| trap via signal | 12.0 ms (1.2%) |
| trap via Mach exception | 27.2 ms (2.7%) |
| rewritten svc | 0.29 ms (0.03%) |
| rewritten, with `clock_gettime` served in userspace | 0.09 ms (0.01%) |

### 2.5 Still not measured

| Metric | Why it matters | Status |
|---|---|---|
| Frame times *during a game*, uncapped | The baseline that matters; §2.1 is an idle UI | needs a play session |
| Syscall rate during a game | §2.3 is a floor, not a load figure | needs a play session |
| CrossOver on the same scene | The actual comparison the project is judged against | needs a play session |
| Input latency, end to end | Part of "feels worse" | not measured |
| FEX translation overhead, isolated | Identical in both architectures | not measured |
| Shader compilation stalls | Unaffected by VM/no-VM | not measured |

`benchmarks/capture.sh <label> <seconds>` runs the whole cycle for the first
three; they need a human at the controls playing a fixed scene.

## 3. Honest read of the hypothesis

The brief says the VM introduces structural overhead. Partly true, partly not,
on the evidence so far:

**Supports it:** three full-frame copies per displayed frame; a software
compositor in the guest; every Vulkan call crossing a virtio ring into a
second renderer process' address space; 8 vCPU contending with the host's own
render thread on 10 cores.

**Argues against it:** presentation costs 0.8 ms, not 8 ms. Venus is a
*protocol pass-through*, not emulation. FEX cost is identical in both designs.
DXVK, MoltenVK and Metal are the same code either way.

**Measured verdict (§2):** the naive form of the hypothesis is refuted.
Presentation costs 1.13 ms, 6.9% of the frame. VM exits cost ~12 ms per
wall-clock second, about 1% of one core. Neither is where a frame budget goes.

What the numbers *do* support is the variance claim: a 4.42 ms standard
deviation and a 1% low 54% worse than the median, on an **idle UI**. Something
in the guest→host path is jittering. That is the measurement worth chasing, and
it now has a harness pointed at it.

## 4. Where time actually went in this project so far

Not performance — **correctness walls**, all in the graphics translation layer,
none caused by virtualisation speed:

- MoltenVK missing `VK_KHR_external_memory_fd` → no Vulkan device at all;
- missing sync-fd semaphores → no `VK_KHR_swapchain` → no game could present;
- `VkDeviceGroupDeviceCreateInfo` with untranslated guest handles → SIGSEGV in
  MoltenVK that killed the host app;
- `blob_size 16384` vs `allocation_size 1280` → `vkMapMemory` returned null →
  guest segfault;
- six DXVK-required Vulkan features Metal does not have.

Every one of these except the last is an artefact of the *VM's* graphics
bridge. **That is the strongest real argument for ZERO-VM in this document**,
and it is an argument about fragility and engineering cost, not about FPS.

## 5. The 4 KiB / 16 KiB page conflict (measured, and it will recur)

- `hv_vm_map` on Apple Silicon requires 16 KiB granularity.
- FEX segfaults on a 16 KiB-page guest (verified: FEX `08f451d`).
- Resolved by aligning only the virtio-gpu host-visible blob allocator.

ZERO-VM does not inherit `hv_vm_map`, and **it did not inherit the harder half
either**. Measured in Stage 2: every aarch64 Linux binary uses `p_align
0x10000`, a clean multiple of 16 KiB, so segments map without straddling
(`benchmarks/stage2-elf-alignment.txt`). The prediction in this section was
wrong, and the real wall turned out to be Darwin's `__PAGEZERO`
(`benchmarks/stage2-pagezero.txt`). x86-64 Linux binaries do use `p_align
0x1000`, which matters for Stage 5 — but under FEX guest addresses are not host
addresses.

## 6. Method for future comparisons

Fix resolution, settings, FPS cap, vsync, display, game version, scene and
duration. Report median frame time, 1% low, 0.1% low and a frame-time
histogram — **never average FPS alone**. Compare four ways: VM baseline,
ZERO-VM, CrossOver, and a native macOS build where one exists.

## Addendum (2026-09-25) — CPU throughput, VM vs runtime

`benchmarks/stage5-x86-throughput.txt`: one freestanding program (sieve, FNV,
block copy), built for x86-64 and aarch64, run native and through FEX, inside
the VM and under the runtime. FEX's translation costs 1.0–1.1×. The VM costs
4.3–4.5× on the same hardware for the same work, memory-bound phases worst
(sieve 7–8×). §5's page-size conflict, measured.


## Addendum, 2026-09-25 — correctness before speed

The sporadic crashes that made FEX-under-runtime numbers untrustworthy (IR
compiler faults at small bogus addresses) had one cause: Darwin zeroes x18 on
every exception return, 444 times in 1.5 s of preemption alone
(`benchmarks/stage5-x18.txt`). With the fix in place the fork-loop stress passes
6/6 for dash and bash and the suite is 32/32, so the x86-64 throughput figures
in `benchmarks/stage5-x86-throughput.txt` (FEX under the runtime 280–291 ms vs
1304–1338 ms in the VM on the same work) stand on a stable base. Cost of the
x18 trampolines and of one fewer JIT register: not yet measured — the next
throughput run must include `LXRT_NO_X18=1` as the control. `clock_gettime`
under the runtime for an aarch64 guest: 14.7 ns/call (suite); the FEX thunk
path for x86 guests is not measured yet (see `FEX_REUSE_ANALYSIS.md` §12).
