# How SteamARM works

Steam for Linux, Proton and the games are unmodified x86 Linux programs.
Instead of running a Linux kernel in a virtual machine, SteamARM runs those
programs as ordinary macOS processes and translates what they ask the kernel
for.

```
 Windows game (x86 / x86-64)
   Proton / Wine ── DXVK (D3D9/10/11) · VKD3D-Proton (D3D12) → Vulkan
 Steam client, Steam Linux Runtime (pressure-vessel container)
 ───────────────────────────────── x86 Linux user space
 FEX  (x86 → ARM64 JIT, an aarch64 Linux program)
 ───────────────────────────────── aarch64 Linux user space
 lxrun (runtime/): Linux syscalls on Darwin, in the same process
 ───────────────────────────────── macOS
 Vulkan thunks → Vulkan shim (shim/) → MoltenVK → Metal
 X11 → native X server (XQuartz, rootless) + quartz-wm → macOS windows
 PulseAudio (Homebrew) → CoreAudio · steamarm-inputd (SDL) → controllers
```

## lxrun: Linux programs on Darwin (`runtime/`)

`lxrun` loads Linux aarch64 ELF programs into a macOS process and runs them
natively on the CPU. Their `svc` system calls are rewritten to call into the
runtime, which implements the Linux system call interface on top of Darwin.
That covers files, processes (`fork`/`execve`/`clone`), threads, signals,
`mmap` with 4 KiB pages on a 16 KiB-page kernel, futexes, epoll, eventfd,
timerfd, inotify, sockets, `/proc`, `/sys` and `/dev`. On top of that come the
pieces the Linux tools expect:

- **bwrap emulation** for Steam's pressure-vessel containers. Mount
  namespaces become a per-process table of bind mounts.
- **Cross-process signals to threads** (Darwin cannot signal another
  process's thread): a mailbox in `/tmp/lxrt-sig` plus a carrier signal.
- **Shared-memory views** kept coherent when a 4 KiB page lives inside a
  16 KiB host page (Wine's clock page).
- **O_PATH descriptors for sockets**, **netlink uevent sockets** (libudev,
  SDL) and **`/dev/input/eventN`** (controllers).

Each quirk found along the way is documented next to its code and in
`benchmarks/stage*.txt`.

## FEX (`patches/`)

[FEX-Emu](https://github.com/FEX-Emu/FEX) translates x86 and x86-64 code to
ARM64. It is itself a Linux aarch64 program, run by `lxrun`. SteamARM
patches it for Darwin's constraints:

- macOS reserves the low 4 GiB of every process, so the guest address space
  is placed at a base with a low window for 32-bit programs;
- the x18 register is reserved on macOS;
- the JIT follows the W^X (write-xor-execute) rules;
- memory ordering: macOS runs with the x86-compatible TSO mode.

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
feedback) are only partly covered.

## Windows

Linux programs draw with X11. SteamARM builds XQuartz's X server in rootless
mode, with quartz-wm as the window manager, so every X11 window is a normal
macOS window. A VNC desktop mode (Xvnc) exists as a fallback.

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
the Mac runs out of memory, and the DRAM setting sets its ceiling.

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
