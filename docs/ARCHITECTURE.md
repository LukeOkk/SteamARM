# How SteamARM works

On today's working route, Steam for Linux, Proton and the games are
unmodified x86 Linux programs. aarch64 Linux programs run too, directly, with
no translator; Valve's native arm64 Steam client is being brought up that way
(experimental: it does not reach its window yet,
`docs/STEAM_ARM64_BRINGUP.md`). Instead of running a Linux kernel in a
virtual machine, SteamARM runs these programs as ordinary macOS processes and
translates what they ask the kernel for.

```
 Windows game (x86 / x86-64)
   Proton / Wine ── DXVK (D3D9/10/11) · VKD3D-Proton (D3D12) → Vulkan
 Steam client, Steam Linux Runtime (pressure-vessel container)
 ───────────────────────────────── x86 Linux user space
 FEX  (x86 → ARM64 JIT, an aarch64 Linux program)
 ───────────────────────────────── aarch64 Linux user space
 aarch64 programs run here directly (scripts/run-native.sh), e.g. the
 experimental native arm64 Steam client (steamrtarm64/steam)
 lxrun (runtime/): Linux syscalls on Darwin, in the same process
 ───────────────────────────────── macOS
 Vulkan: x86 via thunks, aarch64 directly → Vulkan shim (shim/) → MoltenVK → Metal
 X11 → native X server (XQuartz, rootless) + quartz-wm → macOS windows
 PulseAudio (Homebrew) → CoreAudio · steamarm-inputd (SDL) → controllers
```

## lxrun: Linux programs on Darwin (`runtime/`)

`lxrun` loads Linux aarch64 ELF programs into a macOS process and runs them
natively on the CPU. Darwin cannot trap a Linux `svc` (it runs the Darwin
call named by x16), so every image is scanned when it is mapped and each
`svc` is rewritten to call into the runtime, which implements the Linux
system call interface on top of Darwin. The same pass redirects the
instructions Darwin breaks for Linux code (`docs/ARM64_REWRITE_COVERAGE.md`):

- `TPIDR_EL0` reads and writes: the guest's thread pointer lives in a TSD
  slot, since Darwin clobbers the register;
- system registers Darwin traps (`CTR_EL0`, the ID registers);
- every instruction that names **x18**, which Darwin zeroes on each
  exception: it is kept per thread in a TSD slot
  (`docs/X18_VIRTUALIZATION.md`). Since stage 21 only `.eh_frame` function
  ranges are touched when a file has them, so constant tables in `.text`
  stay intact (libcef needs `LXRT_X18_ALL_TEXT` to lift that).

A site the rewriter refuses, or cannot reach, is poisoned with `brk #1` and
traps. Up to 0.3.4 the poison word was really `svc #1`, a live Darwin call;
`dbd1657` fixed it.

The system call layer covers files, processes (`fork`/`execve`/`clone`),
threads, signals, `mmap` with 4 KiB pages on a 16 KiB-page kernel, futexes,
epoll, eventfd, timerfd, inotify, sockets, `/proc`, `/sys` and `/dev`. On top
of that come the pieces the Linux tools expect:

- **bwrap emulation** for Steam's pressure-vessel containers. Mount
  namespaces become a per-process table of bind mounts.
- **Cross-process signals to threads** (Darwin cannot signal another
  process's thread): a mailbox in `/tmp/lxrt-sig` plus a carrier signal.
- **Shared-memory views** kept coherent when a 4 KiB page lives inside a
  16 KiB host page (Wine's clock page).
- **O_PATH descriptors for sockets**, **netlink uevent sockets** (libudev,
  SDL) and **`/dev/input/eventN`** (controllers).
- **4 KiB-aligned ELF images** (Valve's native client, libcef) inside
  16 KiB host pages, through `runtime/subpage.c`. With
  `LXRT_GUEST_PAGE=4096` the guest sees AT_PAGESZ 4096, so glibc dlopens
  such libraries (stage 21).
- **One process group per launcher session**, with an exit status
  (`scripts/session.py`, `docs/APPLICATION_MANAGER.md`).

Each quirk found along the way is documented next to its code and in
`benchmarks/stage*.txt`.

## FEX (`patches/`)

[FEX-Emu](https://github.com/FEX-Emu/FEX) translates x86 and x86-64 code to
ARM64. It is itself a Linux aarch64 program, run by `lxrun`. SteamARM
patches it for Darwin's constraints:

- macOS reserves the low 4 GiB of every process, so the guest address space
  is placed at a base with a low window for 32-bit programs;
- the x18 register is reserved on macOS: FEX's JIT does not use it and FEX
  is built with `-ffixed-x18` (every other image goes through the runtime's
  x18 rewriting);
- the JIT follows the W^X (write-xor-execute) rules;
- memory ordering: FEX emulates x86 ordering in software. Apple silicon's
  hardware TSO mode cannot be switched on from user space, so the runtime
  refuses FEX's request for it (`PR_SET_MEM_MODEL`). When the request
  seemed to succeed, the Steam client's lock-free lists corrupted themselves
  (benchmarks/stage8-steam-zero-vm.txt).

**Thunks** let x86 programs call the host's native libraries instead of
emulating them. SteamARM uses them for Vulkan, in 64-bit and 32-bit, with
the generator extended to cover the whole 32-bit Vulkan API.

## Graphics

DXVK and VKD3D-Proton turn Direct3D into Vulkan inside the game. The Vulkan
calls cross the thunks into `shim/libvulkan.so.1`, a Linux Vulkan driver
that forwards to MoltenVK (Vulkan on Metal). It adds X11 presentation
(windows of the native X server) and pointer translation for 32-bit
programs. It also caps the video memory reported to games (the VRAM
setting). Some Vulkan features Metal lacks (geometry shaders, transform
feedback) are only partly covered. Details, and the backends the launcher
knows (KosmicKrisp is installed on the Mac but not usable by the shim yet):
`docs/GRAPHICS_BACKEND_ARCHITECTURE.md`.

## Windows

Linux programs draw with X11. SteamARM builds XQuartz's X server in rootless
mode, with quartz-wm as the window manager, so every X11 window is a normal
macOS window. Vulkan frames skip the X server: the Metal layer is hosted over
the X window across processes (CALayerHost), with no copy. A VNC desktop
mode (Xvnc) exists as a fallback; it cannot show that Metal layer, so games
cannot present there (`docs/WINDOWING_AND_PRESENTATION.md`).

## Sound and controllers

- **Sound:** a PulseAudio server on the Mac outputs to CoreAudio. Its
  socket lives inside the Steam root, and pressure-vessel shares it with
  games like on Linux.
- **Controllers:** `steamarm-inputd` reads real controllers with SDL2 and
  publishes each player as a socket plus a capability description
  (`tools/inputd/PROTOCOL.md`). The runtime turns them into
  `/dev/input/eventN` evdev devices with the identity of the chosen type.

## Memory safety net

Emulation can use a lot of memory quickly. `scripts/safeguard.sh` watches
the Linux processes and the kernel's VM object count. It stops them before
the Mac runs out of memory, and the DRAM setting sets its ceiling. Since
`e4047ef` it acts on memory pressure only when macOS reports critical
pressure for 2 checks, or free memory stays under 12 % for 3 checks; it
stops the largest guest first and the rest only if the pressure lasts 5 s
more, and it records why. Up to 0.3.4 one reading under 35 % free stopped
everything, which closed Steam on a 16 GB Mac under normal use.

## Target: ARM64-first

Everything ARM64 except game payloads: Valve's native arm64 client on lxrun,
with FEX only for x86 and i386 game code, and never a virtual machine. The
ordered plan and its status are in `docs/ARM64_FIRST_MIGRATION.md`; the
VM-era target design is kept in `docs/history/ARCHITECTURE_TARGET.md`.

## Source map

| Path | |
|---|---|
| `runtime/` | lxrun |
| `shim/` | Linux Vulkan driver over MoltenVK |
| `patches/` | FEX, thunk generator, XQuartz, quartz-wm changes |
| `launcher/` | SteamARM.app (SwiftUI) |
| `tools/inputd/` | controller service |
| `scripts/` | setup, start/stop, sound, controllers, memory guard, release |
| `tests/` | Linux (aarch64, i386, x86-64) and Windows probes |
| `benchmarks/` | measurements and findings at each stage |
| `docs/history/` | earlier design documents (the VM-based design it replaced) |
