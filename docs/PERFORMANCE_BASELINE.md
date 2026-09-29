# Performance baseline (ZERO-VM)

What has been measured on the Mac with no virtual machine, and what has
not. Every number below is MEASURED and comes from a file in `benchmarks/`.
The VM-era baseline, with its guest measurements, is kept in
`docs/history/PERFORMANCE_BASELINE.md` and is not repeated here.

**No game has been measured.** The probes below clear a window or run a
synthetic loop. They show that a path works and what it costs; they do not
predict game frame rates.

## Host

Apple M4 (4 performance + 6 efficiency cores), 16 GB, macOS 27.0 (26A428).
Display 1920×1080 at 165 Hz, 1× (stage 4, stage 12). MoltenVK 1.4.2.

## Method

For any comparison (`benchmarks/README.md`, "Method"): fix resolution,
settings, FPS cap, V-Sync, display, game version, scene and duration; report
median, 1% low, 0.1% low and the histogram, never the average alone.

## System calls

`benchmarks/stage1-syscall-cost.txt` (`benchmarks/syscall_cost.c`, native
Mach-O, M4, macOS 27):

| mechanism | ns per call |
|---|---:|
| indirect function call (floor) | 0.8 |
| Darwin system call, `svc #0x80` | 71.5 |
| trapping a Linux `svc` with a signal handler | 2789.0 |
| trapping it with a Mach exception handler | 6111.4 |
| **rewritten `svc` → dispatcher → Darwin system call** | **69.3** |
| rewritten `svc`, answered in user space | 0.9 |

An earlier run of the same program, quoted in `benchmarks/README.md`, gave
68.1, 114.2, 2794.2 and 6317.0 ns for the same rows. Either way a rewritten
system call costs about what a native one does, and trapping costs 40-90
times more. This is why lxrun rewrites every `svc` (`docs/ARM64_REWRITE_COVERAGE.md`).

## CPU translation

`benchmarks/stage5-x86-throughput.txt` (`benchmarks/x86_bench.c`: sieve,
FNV-1a, block copy; single-threaded, no I/O). Total ms, three runs:

| program | runs |
|---|---|
| aarch64 ELF under lxrun | 300, 278, 278 |
| the same source as x86-64, through FEX under lxrun | 291, 286, 280 |

FEX costs about 1.0× on this workload. It is one synthetic program; about
30-40 ms of each run is start-up rewriting. The same file also has VM rows;
they are history.

## Vulkan

| measurement | result | record |
|---|---|---|
| one call through the x86-64 Vulkan thunk | 17 ns | stage 14 |
| x86-64 and i386 GPU submit + readback through the thunks | ok | stage 16 (`tests/elf/run_vk_device.sh`) |

## Presentation

All FIFO; the display refreshes every 6.06 ms.

| path | frames | median | 1% low | max | record |
|---|---:|---:|---:|---:|---|
| aarch64, runtime's own NSWindow + CAMetalLayer, 1280×720 | 210 | 8.620 ms | 11.645 ms | 13.201 ms | stage 4 (a shorter run: 6.030 ms) |
| aarch64, X window on Xvnc (layer not shown), 800×600 | 90 | 8.075 ms | 9.39 ms | | stage 12 |
| x86-64 through the thunk, same | 90 | 8.139 ms | 9.68 ms | | stage 12 |
| hosted in the native X server (CALayerHost), no interaction | 3000 | 6.056 ms (165.1 fps) | 6.798 ms | 7.196 ms | stage 12 |
| the same, with window moves, unmap/map, new windows | | 6.056 ms | ~21.5 ms | ~424 ms | stage 12 |

The X server's CPU use while presenting: 0.0 % (stage 12). The spikes under
interaction are not investigated.

## Direct3D probes

Windows programs that create a device and clear the window, through
Proton's DXVK and VKD3D-Proton, MoltenVK and the native X server. The frame
rates sit at the 165 Hz display.

| probe | fps | record |
|---|---|---|
| D3D11 64-bit, 600 / 3000 frames | 161.6 / 164.4 | stage 14 |
| D3D12 64-bit, 600 / 4000 frames | 161.9 / 164.6 | stage 14 |
| D3D9, D3D11, D3D12, 64- and 32-bit, Proton directly | 161.7-163.4 | stage 16 |
| the same through Steam's container (pressure-vessel) | 159.9-161.7 | stage 16 |
| D3D9/11/12, 64- and 32-bit, after the stage 21 runtime changes | ~158-161 | stage 21 |

## Steam

| measurement | result | record |
|---|---|---|
| x86 client: start to login window | 85-86 s | stage 18, stage 21 |
| x86 client on a clean install: second start to "Sign in to Steam" | 120 s | stage 17 |
| `scripts/setup.sh` on a clean checkout (Homebrew formulae already installed) | 6.5 min; build tree 1.8 GB, roots 8.4 GB | stage 17 |
| pressure-vessel start-up to the webhelper, after the fd-link fix | 142 s → 27 s | stage 8 |
| native arm64 webhelper start, libcef mapped from file instead of copied | 2.52 s → 1.49 s | stage 21 |
| guest memory with Steam running, 16 GB Mac | 2.7 GB | commit `e4047ef` (logs of 2026-09-29 05:30:20) |

## Not measured

- Any game: frame rate, frame-time tails, load times, shader stutter.
- The native arm64 client: it has no window yet.
- IMMEDIATE present mode, and the V-Sync setting's effect.
- esync against wineserver sync; fsync does not exist here (no
  `futex_waitv`).
- KosmicKrisp against MoltenVK (the shim cannot load KosmicKrisp yet).
- FEX's translation cache on disk (a setting exists).
- Retina (2×) displays.
