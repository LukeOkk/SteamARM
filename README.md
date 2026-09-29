# SteamARM

The Linux Steam client, and the Windows games Steam runs through Proton, on
an Apple Silicon Mac, **without a virtual machine**.

```
Steam (x86 / i386 Linux)  ─┐
Proton / Wine (x86)        ├─ FEX (x86 → ARM64 JIT) ─┐
DXVK / VKD3D-Proton        ┘                         ├─ lxrun (Linux on Darwin) ─ macOS
aarch64 Linux programs, no FEX ──────────────────────┘
  (experimental: Valve's native arm64 Steam client, up to its sign-in window)

Vulkan: x86 through FEX's thunks, aarch64 directly ─ Vulkan shim ─ MoltenVK ─ Metal
                                         (or KosmicKrisp, experimental)
X11 windows ─ native X server (XQuartz, rootless) + quartz-wm ─ real macOS windows
```

> **Experimental.** Steam itself works (store, library, downloads, login).
> Windows programs run with Direct3D 9/11/12, sound and controllers in the
> test probes, including through Steam's own Proton launch path. **Real games
> are not verified yet.** Some will not start, and some will be slow.

*Guía en español: [docs/es/GUIA.md](docs/es/GUIA.md).*

## Download

**[Latest release](https://github.com/LukeOkk/SteamARM/releases/latest)**:
`SteamARM-<version>-macOS-arm64.dmg`. Drag SteamARM to Applications, open
it, and click **Instalar**. Full instructions, including the first open of
an app that isn't notarized, are in [docs/INSTALL.md](docs/INSTALL.md).

Requirements: Apple Silicon, macOS 14+, Xcode Command Line Tools, Homebrew,
about 25 GB free. The first install takes 20–60 minutes: it builds the
runtime and FEX and downloads the Linux packages and Steam.

## What you get

- **SteamARM.app**: starts Steam and other Linux apps in native macOS
  windows. Its settings are laid out like the Ryujinx emulator's: system,
  CPU/FEX, graphics, sound, DRAM/VRAM limits sized to your Mac, shortcuts,
  logs.
- **Controllers**: Xbox (360, One, Series, Elite 2), PlayStation (DualShock
  3/4, DualSense, DualSense Edge), Nintendo Switch Pro, and the Steam
  Controller (2015 and **2026**). Each type is drawn with its real layout,
  every button can be remapped, and games see the identity you pick. See
  [docs/USAGE.md](docs/USAGE.md#controllers-entrada).
- **Sound** through the Mac's current output.
- **A memory guard** that stops runaway programs before macOS runs out of
  memory.

## Documentation

| | |
|---|---|
| [INSTALL.md](docs/INSTALL.md) | download, first open, setup, update, uninstall, build from source |
| [USAGE.md](docs/USAGE.md) | the launcher, every setting, controllers, sound, command line |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | how it works without a VM |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | common problems and logs |
| [benchmarks/](benchmarks/) | what was measured at each stage, including what does not work |

Research notes on moving to Valve's ARM64 Steam client. They are not user
guides, and nothing in them is ready for players yet. Start with the plan,
[ARM64_FIRST_MIGRATION.md](docs/ARM64_FIRST_MIGRATION.md), and its numbers,
[ARM64_FIRST_AUDIT.md](docs/ARM64_FIRST_AUDIT.md).

- The native client:
  [STEAM_ARM64_BRINGUP.md](docs/STEAM_ARM64_BRINGUP.md),
  [STEAMWEBHELPER_BRINGUP.md](docs/STEAMWEBHELPER_BRINGUP.md),
  [ARM64_REWRITE_COVERAGE.md](docs/ARM64_REWRITE_COVERAGE.md),
  [X18_VIRTUALIZATION.md](docs/X18_VIRTUALIZATION.md).
- Today's environment and launcher:
  [CURRENT_STEAM_ENVIRONMENT.md](docs/CURRENT_STEAM_ENVIRONMENT.md)
  (audited), [APPLICATION_MANAGER.md](docs/APPLICATION_MANAGER.md).
- Graphics, windows, speed:
  [GRAPHICS_BACKEND_ARCHITECTURE.md](docs/GRAPHICS_BACKEND_ARCHITECTURE.md),
  [WINDOWING_AND_PRESENTATION.md](docs/WINDOWING_AND_PRESENTATION.md),
  [PERFORMANCE_BASELINE.md](docs/PERFORMANCE_BASELINE.md).
- The Steam Frame image and Holo:
  [STEAM_FRAME_IMAGE.md](docs/STEAM_FRAME_IMAGE.md),
  [STEAM_FRAME_INVENTORY.md](docs/STEAM_FRAME_INVENTORY.md),
  [STEAM_FRAME_ROOTFS_AUDIT.md](docs/STEAM_FRAME_ROOTFS_AUDIT.md),
  [STEAM_FRAME_REFERENCE.md](docs/STEAM_FRAME_REFERENCE.md),
  [HOLO_CORE_ARM64_AUDIT.md](docs/HOLO_CORE_ARM64_AUDIT.md),
  [STEAM_FRAME_SNAPSHOT_2026-09-29.md](docs/STEAM_FRAME_SNAPSHOT_2026-09-29.md)
  (web research on Frame releases and Holo Core, checked against the Mac's
  records).
- Valve's ARM64 tools:
  [STEAM_FRAME_COMPAT_TOOLS.md](docs/STEAM_FRAME_COMPAT_TOOLS.md),
  [STEAM_RUNTIME_4_ARM64.md](docs/STEAM_RUNTIME_4_ARM64.md),
  [LEPTON_REUSE_ANALYSIS.md](docs/LEPTON_REUSE_ANALYSIS.md).

## Build from source

```sh
git clone https://github.com/LukeOkk/SteamARM.git && cd SteamARM
scripts/setup.sh            # builds everything and installs Steam; resumable
open build/SteamARM.app     # or: scripts/run-steam.sh
scripts/make-release.sh     # the downloadable .dmg, in build/release/
```

Tests: `tests/elf/run.sh`, `tests/elf/run_i386.sh`,
`tests/elf/run_vk_device.sh`, `tests/win/run.sh`,
`tests/win/run_steam_path.sh`, `tests/x18_preserve/run.sh`,
`tests/steamframe_image/run.sh` (Linux), `tests/steamframe_image/redact.sh`,
`tests/audio/run.sh`, `tests/launcher/safeguard.sh`, `make test-launcher-core`,
`make test-arm64`, `python3 -m unittest tests/test_docs_records.py` (the
benchmark index and the docs against the records and the x18 planner).

The Steam Frame recovery image as the ARM64 base (read on the Mac, no VM, no
mount): [docs/STEAM_FRAME_IMAGE.md](docs/STEAM_FRAME_IMAGE.md).

## Status

Working (measured, see `benchmarks/`):

- Steam client;
- x86-64 and i386 Linux programs;
- Proton/Wine;
- D3D9/D3D11 (DXVK) and D3D12 (VKD3D-Proton) at ~160 fps in the test
  probes, in both 64- and 32-bit Windows programs;
- sound and XInput controllers (with rumble) inside Steam's container;
- an install from scratch up to Steam's sign-in window.

Experimental (measured, not ready for players):

- Valve's native arm64 Steam client under lxrun, with no FEX, reaches its
  "Sign in to Steam" window with V8's JIT on, on a Fedora aarch64 root and
  on a root derived from the Steam Frame image; the launcher has it as
  **Steam ARM64 (experimental)** and **Steam ARM64 · Steam Frame
  (experimental)** (`benchmarks/stage22-native-arm64-bringup.txt`,
  `benchmarks/stage23-native-arm64-jit.txt`,
  `benchmarks/stage23-frame-root.txt`,
  [docs/STEAM_ARM64_BRINGUP.md](docs/STEAM_ARM64_BRINGUP.md)). Sign-in was
  not attempted; the library, downloads and games under it are not
  verified. Its root and client are not set up by the installer.
- KosmicKrisp (Mesa's Vulkan driver for Metal) in place of MoltenVK:
  D3D11/D3D12 probes at 158/154 fps (`benchmarks/stage22-kosmickrisp.txt`);
  not tried through Steam's launch path or with games.

Not done yet:

- **Games are not verified.**
- Proton ARM64 does not run on macOS yet. The working route is the x86
  Steam client with x86 Proton.
- Controllers were tested with scripted input, not a physical pad.
- Some Vulkan extensions are missing from the 32-bit thunks.
- MoltenVK gaps are worked around, not solved: geometry shaders and
  transform feedback.
- Motion sensors and trackpad finger positions are not delivered to games.
- The app isn't notarized; the first open needs **Open Anyway**.

## License and credits

SteamARM's own code is under the [MIT license](LICENSE). It builds on, and
patches, other projects under their own licenses (see [NOTICE](NOTICE)):

- [FEX-Emu](https://github.com/FEX-Emu/FEX)
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK)
- [XQuartz](https://www.xquartz.org) and quartz-wm
- [SDL](https://libsdl.org)
- [PulseAudio](https://www.freedesktop.org/wiki/Software/PulseAudio/)
- Valve's Steam client, Proton, DXVK and VKD3D-Proton, which it downloads,
  not bundles

SteamARM is not affiliated with or endorsed by Valve, Apple, Microsoft,
Sony or Nintendo. Steam, Xbox, PlayStation, DualShock, DualSense and
Nintendo Switch are trademarks of their owners. Controller names identify
compatibility only.
