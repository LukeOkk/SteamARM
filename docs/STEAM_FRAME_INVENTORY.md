# Steam Frame image: inventory

What Valve's Steam Frame recovery image contains, read on the owner's Mac
with `scripts/steamframe-image.py` on 2026-09-29. Nothing was booted,
mounted with write access or executed from the image. How to reproduce it is
in `docs/STEAM_FRAME_IMAGE.md`; how it feeds the plan is in
`docs/ARM64_FIRST_MIGRATION.md`.

Labels: MEASURED (a command and its result), VERIFIED IN SOURCE, UPSTREAM
DOCUMENTED, HYPOTHESIS, UNKNOWN.

**Privacy.** The image's `etc/pacman.conf` points every repository at a
private per-device mirror. Those URLs are redacted here, and
`steamframe-image.py inventory` redacts them itself since `b875959`. The
bootstrap's `package/beta` value is not recorded either.

**Licence.** The image holds proprietary Valve software. Nothing extracted
from it goes into this repository or the SteamARM `.dmg`. A root derived
from it lives only on the Mac of the person who downloaded the image.

## The image (MEASURED)

| | |
|---|---|
| file | `steamframe-oobe-repair-20260922.5153644-0.3.0.img` |
| size | 7,516,192,768 bytes |
| SHA-256 | `081a38c051e99c09db6ae91be994b67ef3347ea71f330d803ab861f0ba8cc654` (the same as `docs/STEAM_FRAME_REFERENCE.md`) |
| partitions | 1 `esp` vfat 256 MiB; 2 `efi-A` vfat 64 MiB; 3 `rootfs-A` btrfs 5 GiB; 4 `var-A` ext4 256 MiB; 5 `home` ext4 100 MiB |
| `rootfs-A` | label `rootfs-A`, generation 13, sectorsize 4096, nodesize 4096, crc32c, 4.2 GiB used; features include `COMPRESS_ZSTD` and `NO_HOLES`; one subvolume (5, the default) |

Commands: `steamframe-image.py info IMG --hash`, `diagnose IMG --json F`,
`extract IMG /Volumes/SteamFrameRoot/rootfs` (a case-sensitive APFS
sparsebundle), `inventory ROOT --json F --md F`.

## Why `btrfs restore` failed

`diagnose` over `rootfs-A` (MEASURED):

| file extents | count |
|---|---:|
| zstd | 214,760 |
| uncompressed | 46,107 |

| zstd extents | count |
|---|---:|
| `frame-larger-than-ram`: the frame decodes to more than the extent's `ram_bytes`; the kernel stops at `ram_bytes`, btrfs-progs reports "zstd frame incomplete" | 111,347 |
| `ok` | 103,413 |
| damaged (`frame-incomplete-input`, `short-output`, decode errors) | 0 |

So the btrfs-progs failure is the `frame-larger-than-ram` case, not a damaged
image. This was a HYPOTHESIS in `docs/STEAM_FRAME_IMAGE.md`; it is now
MEASURED. The first examples are small inline extents whose frames declare
4096 bytes of content for 32-2847 bytes of data.

## Extraction (MEASURED)

`extract` finished with exit 0: 180,797 files (8.6 GiB), 12,672
directories, 30,033 symlinks, 4,276 hard links, 0 special files (these would
go to the manifest). 111,347 extents were decoded the kernel's way.

## What the root is (MEASURED)

- `os-release`: SteamOS, `ID=steamos`, `ID_LIKE=arch`, `VERSION_ID=0.3.0`,
  `VERSION_CODENAME=holo`, `VARIANT_ID=vr`, `BUILD_ID=20260922.5152327`. The
  build ID differs from the image file name's `5153644`.
- 965 pacman packages (`usr/lib/holo/pacmandb/local`); 214,783 files walked.
- ELF files by machine: aarch64 7,280; x86-64 894; i386 509.
- Kernel `boot/Image-6.18.0-gfbdbca41fd45`: 4 KiB pages. The client's
  `LXRT_GUEST_PAGE=4096` matches the page size it is built for.

### Key packages

| package | version | why it matters |
|---|---|---|
| glibc | 2.39-2 | the client needs at most `GLIBC_2.29` (MEASURED on its binaries) |
| holo-glibc-locales | 2.39-2 | `usr/lib/locale/C.utf8` and others; missing in the stage 21 Fedora root |
| gcc-libs | 15.1.1 | |
| libx11 | 1.8.9 | with `usr/share/X11/locale`; its absence is in the abort chain (`docs/STEAM_ARM64_BRINGUP.md`) |
| nss | 3.99-1 | `libsoftokn3.so` and `libfreeblpriv3.so` next to `libnss3.so`, and `libnssckbi.so` |
| lsof | 4.99.3 | aarch64 |
| gtk3, gtk2 | 3.24.41, 2.24.33 | |
| dbus | 1.14.10 | |
| sdl2 | 2.30.2 | no system `libSDL3.so.0`; the client ships its own |
| steam | 1.0.0.85-2 | the bootstrap script `usr/bin/steam` → `usr/lib/steam/bin_steam.sh` |
| deckard-steam-rel | r1789506314 | `usr/lib/steam/steam.tar.zst` (below) and Frame launch scripts |
| gamescope | 3.16.28-1 | |
| steamos-manager | 26.4.1-2 | |
| vulkan-icd-loader | 1.4.309 | |
| deckard-mesa-linux-aarch64 | 26.3.0_devel | the only Vulkan ICD: freedreno (Qualcomm Adreno), API 1.4.359 |
| deckard-mesa-linux-x86_64 (+ deps) | 26.3.0_devel | an x86-64/i386 Mesa and userland under `usr/share/guestos/fex-mesa` (961 MB): the x86 side for FEX, with x86 freedreno ICD manifests |
| podman, crun, conmon, passt | 5.5.2, 1.14.4, 2.1.10, 2026_06_11 | with `usr/lib/systemd/user/podman.service.d/lepton-podman-timeout.conf`. HYPOTHESIS from the file name: Lepton (Android) runs in podman. `usr/share/guestos/android` is 57 MB |
| pipewire | 1.6.8 | |
| android-platform-tools | 34.0.5 | |
| linux-618-deckard | 6.18.0 | the device kernel |

### The Steam client inside

`usr/lib/steam/steam.tar.zst`, 700,296,701 bytes, owned by
`deckard-steam-rel` (MEASURED, pacman file list, `tar -t`):

- 14,472 entries; `steamrtarm64/builddate.txt` says 2026-09-15 19:45:26 UTC.
- `steamrtarm64/` (171 entries): `steam` (10,130,496 B), `steamui.so`,
  `vgui2_s.so`, `steamclient.so`, `steamwebhelper`, `libcef.so`
  (218,132,264 B), `cefsimple`, `libSDL3.so.0`, `vulkandriverquery`,
  `gldriverquery` and the rest of the native client.
- `linuxarm64/steamclient.so` (46,427,648 B) and 4 other aarch64 files;
  `androidarm64/` (5 files).
- x86 parts as in the downloaded client: `steamrt64/` (x86-64, including
  pressure-vessel and the x86 `srt-launcher-service`), `ubuntu12_32/`,
  `ubuntu12_64/`, `steamrt32/`.

HYPOTHESIS: this is the same client stage 21 downloaded from Valve's
`steam_client_linuxarm64` manifest, at another build. Not compared.

`usr/share/deckard/steam_launch_wrapper_env_defaults.txt` limits the VRAM
reported "to all games that use thunking" to 4096 MB, for a Qualcomm device
ID (`0x5143:...`) (MEASURED, file content). HYPOTHESIS: on the Frame, x86 games reach the
aarch64 Vulkan driver through FEX's thunks, as they reach the shim here.

### What is not in it

- No FEX: no `FEXInterpreter`, `FEXServer`, `libFEXCore` or FEX loader, and
  no `binfmt.d` rules (MEASURED: empty component and binfmt sections).
  Steam installs its own FEX as a tool (MEASURED with the x86 client,
  `benchmarks/stage15-steam-proton-path.txt:24-31`); that the Frame gets it
  that way is a HYPOTHESIS.
- No Proton and no Steam Linux Runtime in the root; only two Proton tuning
  files (`15-proton-nice.conf`, `51-proton-max-maps.conf`).
- No `steam-runtime-launcher-service` outside the bootstrap's x86
  `steamrt64/`.

### What lxrun can load (MEASURED)

- Smallest `p_align` per aarch64 ELF: `0x10000` 6,718 files, `0x4000` 6,
  `0x1000` 45, `0x8` 1. glibc's `ld-linux-aarch64.so.1`, `libc.so.6`,
  `libX11.so.6` and `bash` are PIE with `0x10000` segments (`llvm-readelf`),
  so the base loads without the sub-page path.
- 45 aarch64 `ET_EXEC` files, which `runtime/elf.c` refuses: `busybox`,
  `gawk` (and so `awk`), `gcc`/`g++`/`cpp`, `catatonit`, SteamVR's
  `XRService` and `overlay-merge`, and others.
- 25 executables and 21 libraries aligned to 4 KiB, below the 16 KiB host
  page: they load through `runtime/subpage.c`, and glibc needs
  `LXRT_GUEST_PAGE=4096`. Most are SteamVR programs.
- 3 files use Android's `/system/bin/linker64`.

## Reuse classification

| category | meaning |
|---|---|
| REUSE_BINARY_IF_LICENSE_ALLOWS | open-source userspace SteamARM may run from the owner's local copy. If SteamARM ever ships such a file, it comes from the upstream package with its licence and source offer, never from the image |
| REFERENCE_ONLY | read to learn how Valve does it; nothing is copied |
| HARDWARE_SPECIFIC_IGNORE | tied to the Frame's Qualcomm hardware or boot chain; moved aside in a derived root |
| PROPRIETARY_DO_NOT_REDISTRIBUTE | Valve's proprietary software: used only from the owner's copy, or downloaded from Valve on the user's Mac |

| component | category |
|---|---|
| glibc, holo-glibc-locales, gcc-libs, libx11 (+ locale data), nss, lsof, gtk3, dbus, sdl2, bash, coreutils and the rest of the generic Arch userspace | REUSE_BINARY_IF_LICENSE_ALLOWS (local ARM64 base) |
| vulkan-icd-loader | REUSE_BINARY_IF_LICENSE_ALLOWS; HYPOTHESIS: usable with an ICD JSON for SteamARM's shim |
| Steam client (`steam` bootstrap, `deckard-steam-rel`'s `steam.tar.zst`: client, steamwebhelper, libcef, steamclient) | PROPRIETARY_DO_NOT_REDISTRIBUTE |
| SteamVR (`deckard-steamvr-rel`, `opt/steamvr`) | PROPRIETARY_DO_NOT_REDISTRIBUTE; not needed |
| `usr/share/guestos/android`, the androidarm64 Steam libraries | PROPRIETARY_DO_NOT_REDISTRIBUTE (`docs/LEPTON_REUSE_ANALYSIS.md`) |
| freedreno ICD and `deckard-mesa-*` (aarch64 and the x86 `fex-mesa` tree) | HARDWARE_SPECIFIC_IGNORE for the driver; REFERENCE_ONLY for the FEX/thunk layout |
| kernel `linux-618-deckard`, firmware, u-boot, `deckard-hw-support`, `-charger`, `-eeprom`, `-fpga`, `-led-control`, `-power-monitor`, `-typec-logger`, device trees | HARDWARE_SPECIFIC_IGNORE |
| gamescope, steamos-manager, Frame launch scripts (`usr/share/deckard/*`), systemd units, pipewire setup | REFERENCE_ONLY |
| podman, crun, conmon, passt, `lepton-podman-timeout.conf` | REFERENCE_ONLY (how Lepton is hosted) |
| pacman mirror URLs | never published |

## Open

| question | how to answer |
|---|---|
| How the image's packages compare with `holo-core-aarch64-preview` | `steamframe-image.py compare <inventory.json> <that repository's core.db and extra.db>` |
| Whether the client runs on this root under lxrun | `docs/ARM64_FIRST_MIGRATION.md` steps 6-8 |
| Whether the bootstrap's client is the stage 21 build | compare `steamrtarm64/steam` hashes after extracting the tarball locally |

---

The generated inventory follows as `inventory --md` wrote it on 2026-09-29.
The five mirror lines are shown as the script prints them since `b875959`
(MEASURED: `pacman_repos()` over the extracted root); the run itself predates
that commit and was redacted by hand.

## Inventory (generated by scripts/steamframe-image.py inventory)

| | |
|---|---|
| NAME | `SteamOS` |
| ID | `steamos` |
| ID_LIKE | `arch` |
| VERSION_ID | `0.3.0` |
| BUILD_ID | `20260922.5152327` |
| VARIANT_ID | `vr` |
| VERSION_CODENAME | `holo` |
| files | 214783 |
| pacman db | `usr/lib/holo/pacmandb/local` (965 packages) |

### ELF files by machine

| machine | files |
|---|---|
| aarch64 | 7280 |
| x86-64 | 894 |
| i386 | 509 |
| 0 | 338 |
| 247 | 9 |
| 164 | 8 |
| 94 | 6 |
| arm | 3 |
| 1 | 2 |

### Program interpreters

| machine | PT_INTERP | files |
|---|---|---|
| aarch64 | `/lib/ld-linux-aarch64.so.1` | 2464 |
| x86-64 | `/lib64/ld-linux-x86-64.so.2` | 299 |
| aarch64 | `/usr/lib/ld-linux-aarch64.so.1` | 16 |
| x86-64 | `/usr/lib/ld-linux-x86-64.so.2` | 16 |
| i386 | `/lib/ld-linux.so.2` | 12 |
| i386 | `/usr/lib32/ld-linux.so.2` | 7 |
| aarch64 | `/system/bin/linker64` | 3 |

### Loading under lxrun (aarch64 ELF)

Smallest PT_LOAD p_align per file: 0x8 1, 0x1000 45, 0x4000 6, 0x10000 6718

- ET_EXEC (lxrun refuses them, runtime/elf.c): 45
  - `opt/steamvr/drivers/cv/bin/linuxarm64/XRService`
  - `opt/steamvr/tools/overlay-merge`
  - `usr/bin/aarch64-unknown-linux-gnu-c++`
  - `usr/bin/aarch64-unknown-linux-gnu-g++`
  - `usr/bin/aarch64-unknown-linux-gnu-gcc`
  - `usr/bin/aarch64-unknown-linux-gnu-gcc-15.1.1`
  - `usr/bin/aarch64-unknown-linux-gnu-gcc-ar`
  - `usr/bin/aarch64-unknown-linux-gnu-gcc-nm`
  - `usr/bin/aarch64-unknown-linux-gnu-gcc-ranlib`
  - `usr/bin/busybox`
  - `usr/bin/c++`
  - `usr/bin/catatonit`
  - `usr/bin/cpp`
  - `usr/bin/g++`
  - `usr/bin/gawk`
  - `usr/bin/gawk-5.3.0`
  - `usr/bin/gcc`
  - `usr/bin/gcc-ar`
  - `usr/bin/gcc-nm`
  - `usr/bin/gcc-ranlib`
- executables (PT_INTERP) and ld.so below the 16 KiB host page (runtime/subpage.c path; refused below 4 KiB): 25
  - `opt/steamvr/bin/linuxarm64/hellovr_sdl` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/helloxr` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/openvr_api_canary` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/overlay_viewer` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/proxmicmute` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/systemlayer` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/v4l2cam` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrcmd` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrcompositor` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrcompositor-launcher` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrdashboard` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrmonitor` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrpathreg` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrprismhost` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrserver` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrstartup` p_align 0x1000
  - `opt/steamvr/bin/linuxarm64/vrurlhandler` p_align 0x1000
  - `opt/steamvr/bin/vrwebhelper/linuxarm64/vrwebhelper` p_align 0x1000
  - `opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking` p_align 0x1000
  - `opt/steamvr/tools/imageoverlay/bin/linuxarm64/imageoverlay` p_align 0x1000
- libraries below the 16 KiB host page (runtime/subpage.c path; glibc needs LXRT_GUEST_PAGE=4096): 21
- below 4 KiB (runtime/elf.c refuses the programs among them): 1
- kernel `boot/Image` (Image header (raw)): page size 4K
- kernel `boot/Image-6.18.0-gfbdbca41fd45` (Image header (raw)): page size 4K

### Components found (paths, first 12 each)

- **audio** (400): `usr/bin/pipewire`, `usr/bin/pipewire-aes67`, `usr/bin/pipewire-avb`, `usr/bin/pipewire-pulse`, `usr/bin/wireplumber`, `usr/include/KPipeWire/kpipewire_export.h`, `usr/include/KPipeWire/kpipewire_version.h`, `usr/include/KPipeWire/kpipewiredmabuf_export.h`, `usr/include/KPipeWire/pipewirebaseencodedstream.h`, `usr/include/KPipeWire/pipewireencodedstream.h`, `usr/include/KPipeWire/pipewirerecord.h`, `usr/include/KPipeWire/pipewiresourceitem.h`
- **gamescope** (61): `usr/bin/gamescope`, `usr/bin/gamescope-type`, `usr/bin/gamescopectl`, `usr/bin/gamescopereaper`, `usr/bin/gamescopestream`, `usr/bin/start-gamescope-session`, `usr/lib/holo/pacmandb/local/gamescope-3.16.28-1/desc`, `usr/lib/holo/pacmandb/local/gamescope-3.16.28-1/files`, `usr/lib/holo/pacmandb/local/gamescope-3.16.28-1/install`, `usr/lib/holo/pacmandb/local/gamescope-3.16.28-1/mtree`, `usr/lib/holo/pacmandb/local/xdg-desktop-portal-gamescope-0.1.23.bfbf0e3-1/desc`, `usr/lib/holo/pacmandb/local/xdg-desktop-portal-gamescope-0.1.23.bfbf0e3-1/files`
- **lepton** (1): `usr/lib/systemd/user/podman.service.d/lepton-podman-timeout.conf`
- **mesa** (146): `usr/include/X11/dri/xf86dri.h`, `usr/include/X11/dri/xf86driproto.h`, `usr/include/X11/dri/xf86dristr.h`, `usr/lib/dri/apple_dri.so`, `usr/lib/dri/armada-drm_dri.so`, `usr/lib/dri/exynos_dri.so`, `usr/lib/dri/gm12u320_dri.so`, `usr/lib/dri/hdlcd_dri.so`, `usr/lib/dri/hx8357d_dri.so`, `usr/lib/dri/ili9163_dri.so`, `usr/lib/dri/ili9225_dri.so`, `usr/lib/dri/ili9341_dri.so`
- **nss** (5): `usr/lib/libfreeblpriv3.so`, `usr/lib/libnspr4.so`, `usr/lib/libnss3.so`, `usr/lib/libnssckbi.so`, `usr/lib/libsoftokn3.so`
- **proton** (3): `etc/security/limits.d/15-proton-nice.conf`, `usr/lib/sysctl.d/51-proton-max-maps.conf`, `usr/share/kf6/searchproviders/protondb.desktop`
- **qualcomm/hardware** (400): `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/apq8016-sbc-usb-host.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/apq8016-sbc.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/apq8016-schneider-hmibsc.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/apq8039-t2.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/apq8096-db820c.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/hamoa-iot-evk.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/ipq5018-rdp432-c2.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/ipq5018-tplink-archer-ax55-v1.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/ipq5332-rdp441.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/ipq5332-rdp442.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/ipq5332-rdp468.dtb`, `boot/dtbs/6.18.0-gfbdbca41fd45/qcom/ipq5332-rdp474.dtb`
- **steam-client** (59): `usr/bin/steam`, `usr/bin/steam-http-loader`, `usr/bin/steam_notif_daemon`, `usr/bin/steamcl-install`, `usr/bin/steamos-add-to-steam`, `usr/bin/steamos-atomupd-client`, `usr/bin/steamos-atomupd-mkmanifest`, `usr/bin/steamos-boot-install`, `usr/bin/steamos-bootconf`, `usr/bin/steamos-bootdebug`, `usr/bin/steamos-chroot`, `usr/bin/steamos-devmode`
- **steamwebhelper/cef** (1): `opt/steamvr/bin/vrwebhelper/linuxarm64/libcef.so`
- **vulkan-icd** (3): `usr/share/guestos/fex-mesa/usr/share/vulkan/icd.d/freedreno_icd.x86.json`, `usr/share/guestos/fex-mesa/usr/share/vulkan/icd.d/freedreno_icd.x86_64.json`, `usr/share/vulkan/icd.d/freedreno_icd.aarch64.json`
- **x11** (23): `etc/xdg/Xwayland-session.d/00-at-spi`, `etc/xdg/Xwayland-session.d/10-ibus-x11`, `usr/bin/Xwayland`, `usr/lib/libX11.so`, `usr/lib/libX11.so.6`, `usr/lib/libX11.so.6.4.0`, `usr/lib/libxcb.so`, `usr/lib/libxcb.so.1`, `usr/lib/libxcb.so.1.1.0`, `usr/share/applications/org.freedesktop.Xwayland.desktop`, `usr/share/guestos/fex-mesa/usr/lib/libX11.so`, `usr/share/guestos/fex-mesa/usr/lib/libX11.so.6`

### Key libraries

- `libc.so.6`: `usr/lib/libc.so.6`, `usr/share/guestos/fex-mesa/usr/lib32/libc.so.6`, `usr/share/guestos/fex-mesa/usr/lib/libc.so.6`
- `libc.so.6.debug`: `usr/lib/debug/usr/lib/libc.so.6.debug`
- `libfreeblpriv3.so`: `usr/lib/libfreeblpriv3.so`
- `libnss3.so`: `usr/lib/libnss3.so`
- `libsoftokn3.so`: `usr/lib/libsoftokn3.so`
- `libstdc++.so.6.0.34`: `usr/lib/libstdc++.so.6.0.34`, `usr/share/guestos/fex-mesa/usr/lib32/libstdc++.so.6.0.34`, `usr/share/guestos/fex-mesa/usr/lib/libstdc++.so.6.0.34`
- `libvulkan.so.1.4.309`: `usr/lib/libvulkan.so.1.4.309`
- `libvulkan.so.1.4.321`: `usr/share/guestos/fex-mesa/usr/lib32/libvulkan.so.1.4.321`, `usr/share/guestos/fex-mesa/usr/lib/libvulkan.so.1.4.321`

### Vulkan ICDs

- `usr/share/vulkan/icd.d/freedreno_icd.aarch64.json` -> `/usr/lib/libvulkan_freedreno.so` (API 1.4.359)

### binfmt_misc (how x86 is dispatched)


### Non-AArch64 ELF files: 1769

- `usr/share/guestos/fex-mesa/usr/bin/` 313
- `usr/share/guestos/fex-mesa/usr/lib32/gconv/` 253
- `usr/share/guestos/fex-mesa/usr/lib/gconv/` 253
- `usr/share/guestos/fex-mesa/usr/lib/` 216
- `usr/share/guestos/fex-mesa/usr/lib32/` 182
- `usr/lib/guile/3.0/ccache/ice-9/` 73
- `usr/share/guestos/fex-mesa/usr/lib32/security/` 42
- `usr/share/guestos/fex-mesa/usr/lib/security/` 42
- `usr/share/guestos/fex-mesa/usr/lib/bash/` 40
- `usr/lib/guile/3.0/ccache/srfi/` 36
- `usr/lib/guile/3.0/ccache/language/cps/` 35
- `usr/lib/guile/3.0/ccache/scripts/` 20
- `usr/lib/guile/3.0/ccache/rnrs/` 17
- `usr/lib/guile/3.0/ccache/scheme/` 16
- `usr/lib/guile/3.0/ccache/language/tree-il/` 15
- `usr/lib/guile/3.0/ccache/system/vm/` 15
- `usr/lib/guile/3.0/ccache/system/base/` 10
- `usr/lib/udev/rc_keymaps/protocols/` 9
- `usr/lib/guile/3.0/ccache/oop/goops/` 8
- `usr/lib/guile/3.0/ccache/language/elisp/` 8
- `usr/lib/guile/3.0/ccache/language/ecmascript/` 8
- `usr/share/guestos/fex-mesa/usr/lib/sasl2/` 8
- `usr/lib/guile/3.0/ccache/sxml/` 7
- `usr/lib/guile/3.0/ccache/system/repl/` 7
- `usr/lib/guile/3.0/ccache/texinfo/` 7

### pacman repositories (etc/pacman.conf)

- `[deckard-arch-hotfixes-release-0.3]`: `https://holo-packages.steamos.cloud/<private path redacted>`
- `[core]`: `https://holo-packages.steamos.cloud/<private path redacted>`
- `[extra]`: `https://holo-packages.steamos.cloud/<private path redacted>`
- `[core-debug]`: `https://holo-packages.steamos.cloud/<private path redacted>`
- `[extra-debug]`: `https://holo-packages.steamos.cloud/<private path redacted>`

### Packages (name, version, arch, license)

| package | version | arch | license |
|---|---|---|---|
| a52dec | 0.8.0-2 | aarch64 | GPL |
| aalib | 1.4rc5-18 | aarch64 | LGPL-2.0-or-later |
| abseil-cpp | 20250814.1-1 | aarch64 | Apache-2.0 |
| accounts-qml-module | 0.7-6 | aarch64 | LGPL |
| accountsservice | 23.13.9-2 | aarch64 | GPL-3.0-or-later |
| acl | 2.3.2-1 | aarch64 | LGPL |
| adobe-source-code-pro-fonts | 2.042u+1.062i+1.026vf-1 | any | custom |
| adwaita-cursors | 46.0-1 | any | CC-BY-SA-3.0 LGPL-3.0-only |
| adwaita-icon-theme | 46.0-1 | any | CC-BY-SA-3.0 LGPL-3.0-only |
| alsa-card-profiles | 1:1.6.8-1.2 | aarch64 | LGPL-2.1-or-later |
| alsa-lib | 1.2.11-1 | aarch64 | LGPL-2.1-or-later |
| alsa-plugins | 1:1.2.12-4 | aarch64 | GPL-2.0-or-later LGPL-2.1-or-later |
| alsa-topology-conf | 1.2.5.1-3 | any | BSD |
| alsa-ucm-conf | 1.2.11-1 | any | BSD-3-Clause |
| alsa-utils | 1.2.11-1 | aarch64 | GPL-2.0-or-later |
| aml | 0.3.0-1 | aarch64 | custom:ISC |
| android-boringssl | 14.0.0.r45-2 | aarch64 | Apache-2.0 BSD-3-clause Expat ISC OpenSS |
| android-platform-tools | 34.0.5+g7f0f5ecc8f13-3 | aarch64 | Apache-2.0 PSF public-domain Khronos |
| anthy | 9100h-6 | aarch64 | LGPL GPL |
| aom | 3.9.0-1 | aarch64 | BSD-3-Clause |
| apparmor | 3.1.7-2 | aarch64 | GPL-2.0-only LGPL-2.0-only LGPL-2.1-only |
| appstream | 1.1.1-1 | aarch64 | LGPL-2.1-or-later |
| appstream-qt | 1.1.1-1 | aarch64 | LGPL-2.1-or-later |
| archlinux-appstream-data | 20240415-1 | any | GPL |
| archlinux-keyring | 20240313-1 | any | GPL-3.0-or-later |
| argon2 | 20190702-5 | aarch64 | Apache custom:CC0 |
| aribb24 | 1.0.3-3 | aarch64 | LGPL3 |
| aspell | 0.60.8.1-2 | aarch64 | LGPL-2.0-or-later AND LGPL-2.1-only AND  |
| aspell-en | 2020.12.07-1 | aarch64 | custom |
| at-spi2-core | 2.52.0-1 | aarch64 | LGPL-2.1-or-later |
| atomupd-daemon | 0.20260810.0-1 | aarch64 | MIT |
| attica | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| attr | 2.5.2-1 | aarch64 | LGPL |
| audit | 4.0.1-3 | aarch64 | GPL-2.0-or-later LGPL-2.0-or-later |
| autoconf | 2.72-1 | any | GPL2 GPL3 custom |
| autoconf-archive | 1:2023.02.20-2 | any | GPL3 |
| automake | 1.16.5-2 | any | GPL |
| avahi | 1:0.8+r194+g3f79789-2 | aarch64 | LGPL |
| baloo | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| baloo-widgets | 24.08.0-1 | aarch64 | LGPL-2.0-or-later |
| base | 3-2 | any | GPL |
| base-devel | 1-1 | any | GPL |
| bash | 5.3.3-3 | aarch64 | GPL-3.0-or-later |
| bash-completion | 2.11-3 | any | GPL2 |
| bc | 1.07.1-4 | aarch64 | GPL |
| binutils | 2.42-3 | aarch64 | GPL-2.0-or-later GPL-3.0-or-later LGPL-2 |
| bison | 3.8.2-6 | aarch64 | GPL3 |
| bluez | 5.79-1.3 | aarch64 | GPL-2.0-only |
| bluez-deprecated-tools | 5.79-1.3 | aarch64 | GPL-2.0-only |
| bluez-libs | 5.79-1.3 | aarch64 | LGPL-2.1-only |
| bluez-utils | 5.79-1.3 | aarch64 | GPL-2.0-only |
| boost-libs | 1.83.0-6 | aarch64 | custom |
| bpf | 6.18.0+gfbdbca41fd45-1 | aarch64 | GPL2 |
| breakpad | v2024.02.16-1 | aarch64 | BSD |
| breeze | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| breeze-icons | 6.14.0-1.1 | aarch64 | GPL-3.0-or-later LGPL-2.1-only |
| brltty | 6.6-7 | aarch64 | LGPL-2.1-or-later |
| brotli | 1.1.0-2 | aarch64 | MIT |
| btrfs-progs | 6.8-3 | aarch64 | GPL-2.0-only |
| bubblewrap | 0.9.0-1 | aarch64 | LGPL-2.0-or-later |
| busybox | 1.37.0-1.2 | aarch64 | GPL-2.0-only |
| bzip2 | 1.0.8-6 | aarch64 | BSD |
| ca-certificates | 20220905-1 | any | GPL |
| ca-certificates-mozilla | 3.99-1 | aarch64 | MPL-2.0 |
| ca-certificates-utils | 20220905-1 | any | GPL |
| cairo | 1.18.0-2 | aarch64 | LGPL-2.1-only OR MPL-1.1 |
| cantarell-fonts | 1:0.303.1-1 | any | custom:SIL |
| capstone | 5.0.1-3 | aarch64 | BSD |
| catatonit | 0.2.0-3 | aarch64 | GPL-2.0-or-later |
| cdparanoia | 10.2-9 | aarch64 | GPL |
| cifs-utils | 7.0-3 | aarch64 | GPL |
| cjson | 1.7.17-1 | aarch64 | MIT |
| clang | 19.1.7-1.1 | aarch64 | Apache-2.0 WITH LLVM-exception |
| clang-libs | 19.1.7-1.1 | aarch64 | Apache-2.0 WITH LLVM-exception |
| cmake | 3.29.2-1 | aarch64 | custom |
| compiler-rt | 19.1.7-1.1 | aarch64 | Apache-2.0 WITH LLVM-exception |
| confuse | 3.3-4 | aarch64 | ISC |
| conmon | 1:2.1.10-1 | aarch64 | APACHE |
| containers-common | 1:0.58.2-1 | any | Apache-2.0 |
| convertlit | 1.8-11 | aarch64 | GPL |
| coreutils | 9.5-1 | aarch64 | GPL-3.0-or-later GFDL-1.3-or-later |
| cppdap | 1.58.0-1 | aarch64 | Apache |
| criu | 3.18-3 | aarch64 | GPL2 LGPL2.1 |
| crun | 1.14.4-2 | aarch64 | LGPL |
| cryptsetup | 2.7.2-1 | aarch64 | GPL |
| curl | 8.7.1-5 | aarch64 | MIT |
| dav1d | 1.4.1-1 | aarch64 | BSD |
| db5.3 | 5.3.28-4 | aarch64 | custom:sleepycat |
| dbus | 1.14.10-2 | aarch64 | AFL-2.1 OR GPL-2.0-or-later |
| dbus-broker | 36-2 | aarch64 | Apache-2.0 |
| dbus-broker-units | 36-2 | aarch64 | Apache-2.0 |
| dbus-python | 1.3.2-3 | aarch64 | MIT |
| dconf | 0.40.0-2 | aarch64 | LGPL |
| ddcutil | 2.1.4-1 | aarch64 | GPL2 |
| debugedit | 5.0-5 | aarch64 | LGPL2.1 |
| debuginfod | 0.191-1 | aarch64 | GPL-3.0-or-later |
| deckard-audio-config | 20260914.1-1 | any | MIT |
| deckard-boot-images | 20260317.2-1 | aarch64 | Proprietary OFL-1.1 |
| deckard-charger | 20260908.2-1 | aarch64 | Proprietary |
| deckard-eeprom | 20260121.1-3 | aarch64 | Proprietary |
| deckard-fan-control | 20260324.1-2 | any | MIT |
| deckard-firewall | 0.1-1 | any | MIT |
| deckard-fpga | 20250924.1-1 | aarch64 | Proprietary |
| deckard-hw-support | 20260911.1-1 | aarch64 | Proprietary |
| deckard-led-control | 20260909.1-1 | aarch64 | Proprietary |
| deckard-mesa-android-aarch64 | 26.3.0_devel+git8aa73b4b-1 | aarch64 | custom |
| deckard-mesa-linux-aarch64 | 26.3.0_devel+git8aa73b4b-1 | aarch64 | custom |
| deckard-mesa-linux-deps-x86_64 | 20260820-1 | aarch64 | Various |
| deckard-mesa-linux-x86_64 | 26.3.0_devel+git8aa73b4b-1 | aarch64 | custom |
| deckard-power-monitor | 20260814.2-1 | aarch64 | Proprietary |
| deckard-steam-rel | r1789506314+d1110443-1 | aarch64 | LicenseRef-steam-subscriber-agreement |
| deckard-steamvr-rel | r25358740+28b72a4f-1 | aarch64 | LicenseRef-steam-subscriber-agreement |
| deckard-steamvr-session | 1.6.1-8 | aarch64 | MIT |
| deckard-typec-logger | 20250728.3-1 | aarch64 | Proprietary |
| deckard-uboot | 20260908.1-2 | aarch64 | GPL-2.0+ BSD OFL-1.1 |
| deckard-uboot-splctl | 20260128.3-1 | aarch64 | GPL-2.0+ |
| deckard-vulkan-layers-android-aarch64 | 20260914.1-1 | aarch64 | Proprietary MIT |
| deckard-vulkan-layers-linux-aarch64 | 20260914.1-1 | aarch64 | Proprietary MIT |
| default-cursors | 2-1 | any | LGPL3 |
| desktop-file-utils | 0.27-1 | aarch64 | GPL |
| desync | 1.0.0-1.2 | aarch64 | BSD-3-Clause |
| device-mapper | 2.03.23-3 | aarch64 | GPL2 LGPL2.1 |
| dhclient | 4.4.3.P1-2 | aarch64 | custom:isc-dhcp |
| dhcpcd | 10.0.6-1 | aarch64 | BSD-2-Clause |
| diffutils | 3.10-1 | aarch64 | GPL3 |
| discount | 3.0.0.d-1 | aarch64 | BSD-3-Clause |
| discover | 6.4.3-1.1 | aarch64 | LGPL-2.0-or-later |
| dnsmasq | 2.90-1 | aarch64 | GPL |
| dolphin | 24.08.0-1 | aarch64 | LGPL-2.0-or-later |
| dosfstools | 4.2-3 | aarch64 | GPL3 |
| dotconf | 1.4.1-1 | aarch64 | LGPL2.1 |
| double-conversion | 3.3.0-1 | aarch64 | BSD |
| duktape | 2.7.0-6 | aarch64 | MIT |
| e2fsprogs | 1.47.3-1 | aarch64 | GPL LGPL MIT |
| ebook-tools | 0.2.2-7 | aarch64 | custom |
| editorconfig-core-c | 0.12.7-1 | aarch64 | BSD |
| efibootmgr | 18-3 | aarch64 | GPL-2.0-or-later |
| efivar | 39-1 | aarch64 | LGPL-2.1-or-later |
| elfutils | 0.191-1 | aarch64 | GPL-3.0-or-later |
| ell | 0.64-2 | aarch64 | LGPL-2.1-or-later |
| enchant | 2.6.5-1 | aarch64 | LGPL |
| evtest | 1.35-1 | aarch64 | GPL |
| exiv2 | 0.28.2-3 | aarch64 | GPL2 |
| expat | 2.6.2-1 | aarch64 | MIT |
| f3 | 9.0-1.1 | aarch64 | GPL3 |
| faad2 | 2.11.1-1 | aarch64 | GPL-2.0-or-later |
| fakeroot | 1.34-1 | aarch64 | GPL |
| ffmpeg | 2:7.0-1.1 | aarch64 | GPL-3.0-only |
| ffmpeg4.4 | 4.4.4-5 | aarch64 | GPL3 |
| fftw | 3.3.10-6 | aarch64 | GPL-2.0-or-later |
| file | 5.45-1 | aarch64 | custom |
| filesystem | 2021.12.07-1.26 | aarch64 | GPL |
| findutils | 4.9.0-3 | aarch64 | GPL3 |
| firewalld | 2.1.2-2 | any | GPL-2.0-or-later |
| flac | 1.4.3-1 | aarch64 | BSD GPL |
| flatpak | 1:1.15.8-1 | aarch64 | LGPL-2.1-or-later |
| flex | 2.6.4-5 | aarch64 | custom |
| fmt | 10.2.0-1 | aarch64 | MIT |
| fontconfig | 2:2.15.0-2 | aarch64 | custom |
| frameworkintegration | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| freeglut | 3.4.0-1 | aarch64 | MIT |
| freerdp | 2:3.17.2-5 | aarch64 | Apache-2.0 |
| freetype2 | 2.13.2-1 | aarch64 | GPL |
| fribidi | 1.0.13-2 | aarch64 | LGPL |
| fuse-common | 3.16.2-1 | aarch64 | GPL2 |
| fuse2 | 2.9.9-4 | aarch64 | GPL2 |
| fuse3 | 3.16.2-1 | aarch64 | GPL2 |
| gamescope | 3.16.28-1 | aarch64 | MIT |
| gawk | 5.3.0-1 | aarch64 | GPL |
| gc | 8.2.6-1 | aarch64 | LicenseRef-GC MIT |
| gcc | 15.1.1+r500+gb1b8d8ce3eea-1.2 | aarch64 | GPL-3.0-with-GCC-exception GFDL-1.3-or-l |
| gcc-libs | 15.1.1+r500+gb1b8d8ce3eea-1.2 | aarch64 | GPL-3.0-with-GCC-exception GFDL-1.3-or-l |
| gdb | 14.2-2 | aarch64 | GPL3 |
| gdb-common | 14.2-2 | aarch64 | GPL3 |
| gdbm | 1.23-2 | aarch64 | GPL3 |
| gdk-pixbuf2 | 2.42.11-2 | aarch64 | LGPL-2.0-or-later |
| geoclue | 2.7.1-2 | aarch64 | LGPL-2.1-or-later GPL-2.0-or-later |
| gettext | 0.22.4-1 | aarch64 | GPL-2.0-only LGPL-2.0-only GFDL-1.2-only |
| gfxreconstruct | 1.4.350.1-6 | aarch64 | MIT |
| giflib | 5.2.2-1 | aarch64 | MIT |
| git | 2.44.0-1 | aarch64 | GPL2 |
| glew | 2.2.0-6 | aarch64 | BSD MIT GPL |
| glfw | 3.4-2 | aarch64 | custom:ZLIB |
| glib-networking | 1:2.80.0-1 | aarch64 | LGPL-2.1-or-later |
| glib2 | 2.84.3-3 | aarch64 | LGPL-2.1-or-later |
| glibc | 2.39-2 | aarch64 | GPL-2.0-or-later LGPL-2.1-or-later |
| glibc-debug | 2.39-2 | aarch64 | GPL-2.0-or-later LGPL-2.1-or-later |
| glslang | 14.0.0-2 | aarch64 | BSD-3-Clause |
| glu | 9.0.3-1 | aarch64 | LGPL |
| gmp | 6.3.0-2 | aarch64 | GPL-2.0-or-later LGPL-3.0-or-later |
| gnu-free-fonts | 20120503-8 | any | GPL3 |
| gnupg | 2.4.5-1 | aarch64 | BSD-2-Clause BSD-3-Clause BSD-4-Clause C |
| gnutls | 3.8.5-1 | aarch64 | GPL-3.0-or-later AND LGPL-2.1-or-later |
| gobject-introspection-runtime | 1.80.1-3 | aarch64 | GPL-2.0-or-later LGPL-2.0-or-later |
| gperftools | 2.15-1 | aarch64 | BSD |
| gpgme | 1.23.2-4 | aarch64 | LGPL |
| gpm | 1.20.7.r38.ge82d1a6-5 | aarch64 | GPL |
| gptfdisk | 1.0.10-2 | aarch64 | GPL2 |
| gpu-trace | 2.14-1.3 | any | MIT |
| graphene | 1.10.8-1 | aarch64 | MIT |
| graphite | 1:1.3.14-3 | aarch64 | LGPL GPL custom |
| grep | 3.11-1 | aarch64 | GPL3 |
| groff | 1.23.0-5 | aarch64 | GPL |
| grub | 2:2.12-1 | aarch64 | GPL-3.0-or-later |
| gsettings-desktop-schemas | 47.1-1.1 | any | LGPL-2.1-or-later |
| gsettings-system-schemas | 47.1-1.1 | any | LGPL-2.1-or-later |
| gsm | 1.0.22-1 | aarch64 | custom |
| gssdp | 1.6.3-1 | aarch64 | LGPL |
| gst-plugins-bad-libs | 1.24.2-2 | aarch64 | LGPL-2.1-or-later |
| gst-plugins-base | 1.24.2-2 | aarch64 | LGPL-2.1-or-later |
| gst-plugins-base-libs | 1.24.2-2 | aarch64 | LGPL-2.1-or-later |
| gst-plugins-good | 1.24.2-2 | aarch64 | LGPL-2.1-or-later |
| gstreamer | 1.24.2-2 | aarch64 | LGPL-2.1-or-later |
| gtk-update-icon-cache | 1:4.14.3-1 | aarch64 | LGPL-2.1-or-later |
| gtk2 | 2.24.33-3 | aarch64 | LGPL |
| gtk3 | 1:3.24.41-1 | aarch64 | LGPL-2.0-only |
| gtk4 | 1:4.14.3-1 | aarch64 | LGPL-2.1-or-later |
| guile | 3.0.9-1 | aarch64 | GPL |
| gupnp | 1:1.6.6-1 | aarch64 | LGPL |
| gupnp-igd | 1.6.0-1 | aarch64 | LGPL |
| gzip | 1.13-2 | aarch64 | GPL-3.0-or-later |
| harfbuzz | 8.4.0-1 | aarch64 | MIT |
| haveged | 1.9.18-2 | aarch64 | GPL |
| hicolor-icon-theme | 0.17-3 | any | GPL2 |
| hidapi | 0.14.0-2 | aarch64 | GPL3 BSD custom |
| highway | 1.1.0-1 | aarch64 | Apache-2.0 BSD-3-Clause |
| holo-glibc-locales | 2.39-2 | aarch64 | GPL LGPL |
| holo-zram-swap | 0.3-1 | any | LGPL2.1 |
| hostapd | 2.11-5 | aarch64 | BSD-3-Clause |
| htop | 3.3.0-4 | aarch64 | GPL |
| http-parser | 2.9.4-2 | aarch64 | MIT |
| hunspell | 1.7.2-1 | aarch64 | GPL LGPL MPL |
| hwdata | 0.381-1 | any | GPL-2.0-or-later |
| hwloc | 2.10.0-1 | aarch64 | BSD |
| i2c-tools | 4.3-6 | aarch64 | GPL |
| iana-etc | 20240412-1 | any | custom:none |
| ibus | 1.5.32-2.4 | aarch64 | LGPL-2.1-or-later |
| ibus-anthy | 1.5.14-4.4 | aarch64 | LGPL |
| ibus-hangul | 1.5.5-5 | aarch64 | GPL |
| ibus-pinyin | 1.5.1-2.3 | aarch64 | GPL |
| ibus-table | 1.17.4-2 | any | LGPL |
| ibus-table-cangjie-lite | 1.8.8-2.4 | any | GPL3 |
| icu | 74.2-2 | aarch64 | LicenseRef-Unicode-3.0 BSD-2-Clause BSD- |
| iftop | 1.0pre4-5 | aarch64 | GPL |
| imath | 3.1.11-2 | aarch64 | BSD-3-Clause |
| imlib2 | 1.12.2-2 | aarch64 | BSD |
| inetutils | 2.5-1 | aarch64 | GPL-3.0-or-later |
| inotify-tools | 4.23.9.0-1 | aarch64 | GPL |
| iproute2 | 6.8.0-1 | aarch64 | GPL2 |
| iptables | 1:1.8.10-1 | aarch64 | GPL2 |
| iputils | 20240117-1 | aarch64 | BSD-3-Clause GPL-2.0-or-later |
| iso-codes | 4.16.0-1 | any | LGPL |
| iw | 6.7-1 | aarch64 | GPL |
| iwd | 2.17-2 | aarch64 | LGPL-2.1-or-later |
| jansson | 2.14-3 | aarch64 | MIT |
| jbigkit | 2.1-7 | aarch64 | GPL-2.0-or-later |
| jq | 1.7.1-1 | aarch64 | MIT |
| json-c | 0.17-1 | aarch64 | MIT |
| json-glib | 1.8.0-1 | aarch64 | GPL |
| jsoncpp | 1.9.5-2 | aarch64 | MIT custom:Public_Domain |
| kaccounts-integration | 24.02.2-1 | aarch64 | GPL-2.0-or-later |
| kactivitymanagerd | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| karchive | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kauth | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kbd | 2.6.4-1 | aarch64 | GPL |
| kbookmarks | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kcmutils | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kcodecs | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kcolorscheme | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kcompletion | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kconfig | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kconfigwidgets | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kcoreaddons | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kcrash | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kdbusaddons | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kde-cli-tools | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| kdeclarative | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kdecoration | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| kded | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kdesu | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kdialog | 24.02.2-1 | aarch64 | LGPL-2.0-or-later |
| kdnssd | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kdsoap-qt6 | 2.2.0-1 | aarch64 | GPL3 LGPL custom |
| kdsoap-ws-discovery-client | 0.4.0-1 | aarch64 | CC0-1.0 GPL-3.0-or-later |
| keyutils | 1.6.3-2 | aarch64 | GPL2 LGPL2.1 |
| kfilemetadata | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kglobalaccel | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kglobalacceld | 6.4.5-1 | aarch64 | LGPL-2.0-or-later |
| kguiaddons | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kholidays | 1:6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| ki18n | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kiconthemes | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kidletime | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kio | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kio-extras | 24.12.2-1.1 | aarch64 | LGPL-2.0-or-later |
| kio-fuse | 5.1.0-3 | aarch64 | GPL |
| kirigami | 6.14.1-1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kirigami-addons | 1.7.0-1 | aarch64 | GPL-2.0-or-later LGPL-2.1-or-later |
| kitemmodels | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kitemviews | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kjobwidgets | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kmenuedit | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| kmod | 32-1 | aarch64 | GPL2 |
| knewstuff | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| knotifications | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| knotifyconfig | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| konsole | 24.02.2-1 | aarch64 | GPL-2.0-or-later LGPL-2.0-or-later |
| kpackage | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kparts | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kpipewire | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| kpty | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kquickcharts | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| krb5 | 1.21.2-2 | aarch64 | custom |
| krunner | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kscreenlocker | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| kservice | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kstatusnotifieritem | 6.14.0-1.1 | aarch64 | LGPL-2.0-or-later |
| ksvg | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| ksystemstats | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| ktexteditor | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| ktextwidgets | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kunitconversion | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kuserfeedback | 6.14.0-1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kwallet | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kwayland | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| kwidgetsaddons | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kwin | 6.2.5-3.1 | aarch64 | LGPL-2.0-or-later |
| kwindowsystem | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| kxmlgui | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| l-smash | 2.14.5-3 | aarch64 | custom |
| lame | 3.100-4 | aarch64 | LGPL |
| layer-shell-qt | 6.2.5-2 | aarch64 | GPL-2.0-or-later LGPL-2.0-or-later |
| lcms2 | 2.16-1 | aarch64 | MIT |
| ldb | 2:2.9.0-3 | aarch64 | GPL-3.0-or-later |
| less | 1:643-2 | aarch64 | GPL-3.0-or-later |
| libaccounts-glib | 1.27-2 | aarch64 | LGPL-2.1-or-later |
| libaccounts-qt | 1.17-1 | aarch64 | LGPL-2.1-or-later |
| libadwaita | 1:1.5.0-1 | aarch64 | LGPL-2.1-or-later |
| libao | 1.2.2-6 | aarch64 | GPL2 |
| libarchive | 3.7.3-3 | aarch64 | BSD |
| libass | 0.17.1-4 | aarch64 | BSD |
| libassuan | 2.5.7-2 | aarch64 | FSFULLR GPL-2.0-or-later LGPL-2.1-or-lat |
| libasyncns | 1:0.8+r3+g68cd5af-2 | aarch64 | LGPL |
| libatasmart | 0.19-6 | aarch64 | LGPL |
| libavc1394 | 0.5.4-6 | aarch64 | LGPL |
| libavif | 1.0.4-3 | aarch64 | LicenseRef-libavif |
| libb2 | 0.98.1-2 | aarch64 | custom:CC0 |
| libblockdev | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libblockdev-crypto | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libblockdev-fs | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libblockdev-loop | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libblockdev-mdraid | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libblockdev-nvme | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libblockdev-part | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libblockdev-swap | 3.1.1-2 | aarch64 | LGPL-2.1-or-later |
| libbluray | 1.3.4-1 | aarch64 | LGPL2.1 |
| libbpf | 1.5.1-1 | aarch64 | LGPL-2.1-only OR BSD-2-Clause |
| libbs2b | 3.1.0-8 | aarch64 | custom:MIT |
| libbsd | 0.12.2-1 | aarch64 | custom |
| libbytesize | 2.8-3 | aarch64 | LGPL |
| libcaca | 0.99.beta20-4 | aarch64 | WTFPL |
| libcanberra | 1:0.30+r2+gc0620e4-3 | aarch64 | LGPL |
| libcap | 2.69-4 | aarch64 | BSD-3-Clause OR GPL-2.0-only |
| libcap-ng | 0.8.5-2 | aarch64 | GPL-2.0-or-later LGPL-2.1-or-later |
| libcloudproviders | 0.3.6-1 | aarch64 | LGPL-3.0-or-later |
| libcolord | 1.4.7-2 | aarch64 | GPL-2.0-or-later |
| libconfig | 1.7.3-2 | aarch64 | LGPL2.1 |
| libcups | 1:2.4.8-1 | aarch64 | Apache-2.0 WITH LLVM-exception AND BSD-3 |
| libdaemon | 0.14-5 | aarch64 | LGPL |
| libdatrie | 0.2.13-4 | aarch64 | LGPL |
| libdbusmenu-glib | 16.04.0.r498-2 | aarch64 | GPL3 LGPL2.1 LGPL3 |
| libdbusmenu-gtk3 | 16.04.0.r498-2 | aarch64 | GPL3 LGPL2.1 LGPL3 |
| libdca | 0.0.7-2 | aarch64 | GPL |
| libdecor | 0.2.2-1 | aarch64 | MIT |
| libdeflate | 1.20-1 | aarch64 | MIT |
| libdisplay-info | 0.1.1-3 | aarch64 | MIT |
| libdmtx | 0.7.7-1 | aarch64 | GPL2 |
| libdovi | 3.3.0-1 | aarch64 | MIT |
| libdrm | 2.4.134-1 | aarch64 | MIT |
| libdv | 1.0.0-11 | aarch64 | LGPL |
| libdvbpsi | 1:1.3.3-3 | aarch64 | LGPL2.1 |
| libebml | 1.4.5-1 | aarch64 | LGPL2.1 |
| libebur128 | 1.2.6-1 | aarch64 | MIT |
| libedit | 20230828_3.1-1 | aarch64 | BSD |
| libei | 1.4.1-3 | aarch64 | MIT |
| libelf | 0.191-1 | aarch64 | GPL-2.0-or-later OR LGPL-3.0-or-later |
| libepoxy | 1.5.10-2 | aarch64 | MIT |
| libevdev | 1.13.1-1 | aarch64 | custom:MIT |
| libevent | 2.1.12-4 | aarch64 | BSD |
| libfdk-aac | 2.0.3-1 | aarch64 | custom |
| libffi | 3.4.6-1 | aarch64 | MIT |
| libfontenc | 1.1.8-1 | aarch64 | MIT |
| libfreeaptx | 0.1.1-1 | aarch64 | LGPL |
| libftdi | 1.5-6 | aarch64 | GPL2 LGPL2.1 |
| libfyaml | 0.9-1 | aarch64 | MIT |
| libgcrypt | 1.10.3-1 | aarch64 | LGPL |
| libgirepository | 1.80.1-3 | aarch64 | GPL-2.0-or-later LGPL-2.0-or-later |
| libgit2 | 1:1.7.2-1 | aarch64 | LicenseRef-GPL-2.0-only-with-linking-exc |
| libglvnd | 1.7.0-1 | aarch64 | custom:BSD-like |
| libgpg-error | 1.49-1 | aarch64 | LGPL-2.1-or-later BSD-3-Clause OR LGPL-2 |
| libgpiod | 2.1.2-3 | aarch64 | LGPL2.1 |
| libgudev | 238-1 | aarch64 | LGPL2.1 |
| libhangul | 0.1.0-5 | aarch64 | LGPL |
| libibus | 1.5.32-2.4 | aarch64 | LGPL-2.1-or-later |
| libice | 1.1.1-2 | aarch64 | custom |
| libidn | 1.42-1 | aarch64 | GPL3 LGPL |
| libidn2 | 2.3.7-1 | aarch64 | GPL2 LGPL3 |
| libiec61883 | 1.2.0-7 | aarch64 | LGPL |
| libimobiledevice | 1.3.0-13 | aarch64 | LGPL-2.1-or-later |
| libimobiledevice-glue | 1.2.0-1 | aarch64 | LGPL-2.1-or-later |
| libinih | 58-1 | aarch64 | BSD |
| libinput | 1.26.1-1.1 | aarch64 | MIT |
| libisl | 0.26-1 | aarch64 | MIT |
| libjpeg-turbo | 3.0.2-2 | aarch64 | BSD-3-Clause IJG |
| libjxl | 0.10.2-1 | aarch64 | BSD-3-Clause |
| libkexiv2 | 24.08.3-1.1 | aarch64 | GPL-2.0-or-later LGPL-2.0-or-later |
| libksba | 1.6.6-1 | aarch64 | GPL-2.0-only GPL-3.0-only LGPL-3.0-only |
| libkscreen | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| libksysguard | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| liblc3 | 1.1.1-1 | aarch64 | Apache-2.0 |
| libldac | 2.0.2.3-1 | aarch64 | Apache |
| libldap | 2.6.7-2 | aarch64 | custom |
| liblouis | 3.29.0-3 | aarch64 | GPL-3.0-or-later AND LGPL-2.1-or-later |
| libmad | 0.15.1b-10 | aarch64 | GPL |
| libmalcontent | 0.12.0-1 | aarch64 | LGPL-2.1-or-later |
| libmatroska | 1.7.1-1 | aarch64 | LGPL2.1 |
| libmd | 1.1.0-1 | aarch64 | BSD |
| libmicrohttpd | 1.0.1-1 | aarch64 | LGPL |
| libmm-glib | 1.22.0-1 | aarch64 | GPL2 LGPL2.1 |
| libmnl | 1.0.5-2 | aarch64 | LGPL2.1 |
| libmodplug | 0.8.9.0-5 | aarch64 | custom |
| libmpc | 1.3.1-1 | aarch64 | LGPL |
| libmpcdec | 1:0.1+r475-4 | aarch64 | BSD LGPL |
| libmpeg2 | 0.5.1-10 | aarch64 | GPL-2.0-or-later |
| libmtp | 1.1.21-1 | aarch64 | LGPL |
| libmysofa | 1.3.2-1 | aarch64 | BSD |
| libndp | 1.8-1 | aarch64 | LGPL |
| libnet | 2:1.3-1 | aarch64 | BSD |
| libnetfilter_conntrack | 1.0.9-2 | aarch64 | GPL |
| libnewt | 0.52.24-2 | aarch64 | GPL |
| libnfnetlink | 1.0.2-2 | aarch64 | GPL |
| libnftnl | 1.2.6-1 | aarch64 | GPL2 |
| libnghttp2 | 1.61.0-1 | aarch64 | MIT |
| libnghttp3 | 1.2.0-1 | aarch64 | MIT |
| libnice | 0.1.22-1 | aarch64 | MPL-1.1 OR LGPL-2.1-only |
| libnl | 3.9.0-1 | aarch64 | GPL |
| libnm | 1.52.1-1.1 | aarch64 | LGPL-2.1-or-later |
| libnotify | 0.8.3-1 | aarch64 | LGPL |
| libnsl | 2.0.1-1 | aarch64 | LGPL-2.1-only |
| libnvme | 1.8-2 | aarch64 | LGPL2.1 |
| libogg | 1.3.5-1 | aarch64 | BSD |
| libopenmpt | 0.7.6-2 | aarch64 | BSD-3-Clause |
| libp11-kit | 0.25.3-1 | aarch64 | BSD |
| libpcap | 1.10.4-1 | aarch64 | BSD |
| libpciaccess | 0.18.1-2 | aarch64 | LicenseRef-libpciaccess |
| libpfm | 4.13.0+r83+g91970fe-2 | aarch64 | MIT |
| libpgm | 5.3.128-3 | aarch64 | LGPL |
| libpipeline | 1.5.7-2 | aarch64 | GPL-3.0-or-later |
| libpipewire | 1:1.6.8-1.2 | aarch64 | MIT |
| libplacebo | 6.338.2-6 | aarch64 | LGPL-2.1-or-later |
| libplasma | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| libplist | 2.4.0-2 | aarch64 | LGPL-2.1-or-later |
| libpng | 1.6.43-1 | aarch64 | custom |
| libproxy | 0.5.5-1 | aarch64 | LGPL-2.1-or-later |
| libpsl | 0.21.2-1 | aarch64 | MIT |
| libpulse | 17.0-3 | aarch64 | LGPL |
| libqaccessibilityclient-qt6 | 0.6.0-1 | aarch64 | LGPL2.1 |
| libqalculate | 5.0.0-2 | aarch64 | GPL-2.0-only |
| libraw1394 | 2.1.2-3 | aarch64 | LGPL2.1 |
| librsvg | 2:2.58.0-1 | aarch64 | LGPL-2.1-or-later |
| libsamplerate | 0.2.2-2 | aarch64 | BSD |
| libsasl | 2.1.28-4 | aarch64 | custom |
| libseccomp | 2.5.5-3 | aarch64 | LGPL2.1 |
| libsecret | 0.21.4-1 | aarch64 | LGPL-2.1-or-later |
| libshout | 1:2.4.6-2 | aarch64 | LGPL |
| libsm | 1.2.4-1 | aarch64 | custom |
| libsndfile | 1.2.2-2 | aarch64 | LGPL-2.1-or-later |
| libsodium | 1.0.19-3 | aarch64 | custom:ISC |
| libsoup | 2.74.3-1 | aarch64 | LGPL |
| libsoup3 | 3.4.4-1 | aarch64 | LGPL |
| libsoxr | 0.1.3-3 | aarch64 | GPL |
| libspeechd | 0.11.5-2 | aarch64 | GPL2 FDL |
| libssh | 0.10.6-2 | aarch64 | LGPL |
| libssh2 | 1.11.0-1 | aarch64 | BSD |
| libstemmer | 2.2.0-2 | aarch64 | BSD |
| libsysprof-capture | 46.0-1 | aarch64 | BSD-2-Clause-Patent |
| libtar | 1.2.20-7 | aarch64 | BSD |
| libtasn1 | 4.19.0-1 | aarch64 | GPL3 LGPL |
| libteam | 1.32-1 | aarch64 | LGPL |
| libthai | 0.1.29-3 | aarch64 | LGPL |
| libtheora | 1.1.1-6 | aarch64 | BSD |
| libtiff | 4.6.0-4 | aarch64 | custom |
| libtirpc | 1.3.4-1 | aarch64 | BSD |
| libtommath | 1.3.0-1 | aarch64 | custom |
| libtool | 2.5.4+r1+gbaa1fe41-3.1 | aarch64 | LGPL-2.0-or-later WITH Libtool-exception |
| libtraceevent | 1:1.8.2-1 | aarch64 | GPL-2.0-only LGPL-2.1-only |
| libtracefs | 1.8.3-1 | aarch64 | GPL-2.0-only LGPL-2.1-only |
| libunibreak | 6.1-1 | aarch64 | custom:zlib/libpng |
| libunistring | 1.2-1 | aarch64 | GPL |
| libunwind | 1.8.1-2 | aarch64 | GPL |
| libupnp | 1.14.19-2 | aarch64 | BSD-3-Clause |
| liburing | 2.5-1 | aarch64 | (GPL-2.0-only WITH Linux-syscall-note) O |
| libusb | 1.0.27-1 | aarch64 | LGPL-2.1-or-later |
| libusbmuxd | 2.1.0-1 | aarch64 | GPL-2.0-or-later AND LGPL-2.1-or-later |
| libutempter | 1.2.1-4 | aarch64 | LGPL |
| libuv | 1.48.0-2 | aarch64 | custom |
| libva | 2.21.0-1 | aarch64 | MIT |
| libvdpau | 1.5-2 | aarch64 | custom |
| libverto | 0.3.2-5 | aarch64 | MIT |
| libvorbis | 1.3.7-3 | aarch64 | BSD |
| libvpl | 2.10.2-1 | aarch64 | MIT |
| libvpx | 1.14.0-1 | aarch64 | custom:BSD |
| libwacom | 2.11.0-1 | aarch64 | MIT |
| libwbclient | 4.20.0-3 | aarch64 | GPL-3.0-or-later |
| libwebp | 1.4.0-1 | aarch64 | BSD-3-Clause |
| libwireplumber | 0.5.14-1.3 | aarch64 | MIT |
| libwnck3 | 43.3-1 | aarch64 | LGPL-2.0-or-later |
| libx11 | 1.8.9-1 | aarch64 | MIT AND X11 |
| libxau | 1.0.11-2 | aarch64 | custom |
| libxaw | 1.0.16-1 | aarch64 | MIT-open-group X11 HPND HPND-sell-varian |
| libxcb | 1.17.0-1 | aarch64 | X11 |
| libxcomposite | 0.4.6-1 | aarch64 | custom |
| libxcrypt | 4.4.36-1 | aarch64 | LGPL |
| libxcrypt-compat | 4.4.36-1 | aarch64 | LGPL |
| libxcursor | 1.2.2-1 | aarch64 | HPND-sell-variant |
| libxcvt | 0.1.2-1 | aarch64 | custom |
| libxdamage | 1.1.6-1 | aarch64 | custom |
| libxdmcp | 1.1.5-1 | aarch64 | MIT-open-group |
| libxext | 1.3.6-1 | aarch64 | LicenseRef-libxext |
| libxfixes | 6.0.1-1 | aarch64 | custom |
| libxfont2 | 2.0.6-2 | aarch64 | custom |
| libxft | 2.3.8-1 | aarch64 | custom |
| libxi | 1.8.1-1 | aarch64 | custom |
| libxinerama | 1.1.5-1 | aarch64 | custom |
| libxkbcommon | 1.13.2-1 | aarch64 | MIT |
| libxkbcommon-x11 | 1.13.2-1 | aarch64 | MIT |
| libxkbfile | 1.1.3-1 | aarch64 | LicenseRef-libxkbfile |
| libxml2 | 2.12.6-2 | aarch64 | MIT |
| libxmlb | 0.3.19-1 | aarch64 | LGPL |
| libxmu | 1.2.1-1 | aarch64 | MIT-open-group AND SMLNJ AND X11 AND ISC |
| libxpm | 3.5.17-1 | aarch64 | custom |
| libxrandr | 1.5.4-1 | aarch64 | custom |
| libxrender | 0.9.11-1 | aarch64 | custom |
| libxres | 1.2.2-1 | aarch64 | custom |
| libxshmfence | 1.3.2-1 | aarch64 | GPL |
| libxslt | 1.1.39-2 | aarch64 | custom:MIT |
| libxt | 1.3.0-1 | aarch64 | custom |
| libxtst | 1.2.4-1 | aarch64 | custom |
| libxv | 1.0.12-1 | aarch64 | custom |
| libxxf86vm | 1.1.5-1 | aarch64 | custom |
| libyaml | 0.2.5-2 | aarch64 | MIT |
| libyuv | r2426+464c51a0-1 | aarch64 | custom |
| libzip | 1.10.1-1 | aarch64 | BSD |
| licenses | 20240206-1 | any | LicenseRef-None |
| lilv | 0.24.24-2 | aarch64 | 0BSD OR ISC ISC |
| linux-618-deckard | 6.18.0+gfbdbca41fd45-1 | aarch64 | GPL-2.0-only |
| linux-618-v4l2loopback-deckard | 20251112.1+gfbdbca41fd45-1 | aarch64 | Proprietary |
| linux-api-headers | 6.18.0+gfbdbca41fd45-1 | aarch64 | GPL-2.0 |
| linux-firmware-deckard | 20260801.1+gita0b66dde-1 | any | custom |
| lld | 19.1.7-1.1 | aarch64 | Apache-2.0 WITH LLVM-exception |
| llvm-libs | 19.1.7-1.1 | aarch64 | Apache-2.0 WITH LLVM-exception |
| lm_sensors | 1:3.6.0.r41.g31d1f125-2 | aarch64 | GPL LGPL |
| lmdb | 0.9.32-1 | aarch64 | custom:OpenLDAP |
| lsb-release | 2.0.r55.a25a4fc-1.1 | any | GPL-2.0-or-later |
| lsof | 4.99.3-2 | aarch64 | custom |
| lua | 5.4.6-3 | aarch64 | MIT |
| luajit | 2.1.1713773202-1 | aarch64 | MIT |
| luit | 20240910-1.1 | aarch64 | MIT |
| lv2 | 1.18.10-1 | aarch64 | ISC |
| lxterminal | 0.4.0-1 | aarch64 | GPL2 |
| lz4 | 1:1.9.4-3 | aarch64 | GPL2 |
| lzo | 2.10-5 | aarch64 | GPL |
| m4 | 1.4.19-3 | aarch64 | GPL3 |
| make | 4.4.1-2 | aarch64 | GPL3 |
| man-db | 2.12.1-1 | aarch64 | GPL-2.0-or-later AND GPL-3.0-or-later |
| mangohud | 20260828.1-1 | aarch64 | MIT |
| md4c | 0.5.2-1 | aarch64 | MIT |
| mdadm | 4.3-2 | aarch64 | GPL-2.0-or-later |
| media-player-info | 24-2 | any | BSD |
| mesa-utils | 9.0.0-4 | aarch64 | MIT |
| meson | 1.4.0-4 | any | Apache-2.0 |
| milou | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| minizip | 1:1.3.1-1 | aarch64 | Zlib |
| mkinitcpio | 39.2-3 | any | GPL-2.0-only |
| mkinitcpio-busybox | 1.36.1-1.2 | aarch64 | GPL |
| mobile-broadband-provider-info | 20230416-1 | any | custom |
| mpdecimal | 4.0.0-2 | aarch64 | BSD |
| mpfr | 4.2.1-2 | aarch64 | GPL-3.0-or-later LGPL-3.0-or-later |
| mpg123 | 1.32.5-1 | aarch64 | LGPL2.1 |
| mtdev | 1.1.6-2 | aarch64 | custom:MIT |
| nano | 8.6-1 | aarch64 | GPL-3.0-or-later |
| ncurses | 6.4_20230520-2 | aarch64 | MIT-open-group |
| neatvnc | 0.9.5-2 | aarch64 | custom:ISC |
| net-tools | 2.10-2 | aarch64 | GPL2 |
| netavark | 1.10.3-1 | aarch64 | Apache-2.0 |
| nettle | 3.9.1-1 | aarch64 | GPL2 |
| networkmanager | 1.52.1-1.1 | aarch64 | LGPL-2.1-or-later GPL-2.0-or-later |
| nftables | 1:1.0.9-2 | aarch64 | GPL2 |
| ninja | 1.12.0-2 | aarch64 | Apache-2.0 |
| noto-fonts | 1:24.4.1-1 | any | custom:SIL |
| noto-fonts-cjk | 20230817-1 | any | custom:SIL |
| noto-fonts-emoji | 1:2.042-1 | any | custom:OFL |
| npth | 1.7-1 | aarch64 | LGPL-2.1-or-later |
| nspr | 4.35-2 | aarch64 | MPL-2.0 |
| nss | 3.99-1 | aarch64 | MPL-2.0 |
| numactl | 2.0.18-1 | aarch64 | GPL-2.0-only LGPL-2.1-only |
| nvtop | 3.3.2-1 | aarch64 | GPL3 |
| ocean-sound-theme | 6.2.5-1.1 | any | LGPL-2.0-or-later |
| ocl-icd | 2.3.2-1 | aarch64 | BSD |
| onetbb | 2021.12.0-2 | aarch64 | Apache |
| oniguruma | 6.9.9-1 | aarch64 | BSD |
| openal | 1.23.1-1 | aarch64 | LGPL |
| openbox | 3.6.1-10 | aarch64 | GPL |
| opencore-amr | 0.1.6-1 | aarch64 | Apache |
| openexr | 3.2.4-1 | aarch64 | BSD-3-Clause |
| openjpeg2 | 2.5.2-1 | aarch64 | BSD-2-Clause MIT |
| openocd | 0.12.0-1 | aarch64 | GPL |
| openssh | 9.7p1-1 | aarch64 | BSD-2-Clause BSD-3-Clause ISC LicenseRef |
| openssl | 3.2.1-1 | aarch64 | Apache-2.0 |
| opus | 1.5.2-1 | aarch64 | BSD-3-Clause |
| orc | 0.4.38-1 | aarch64 | BSD-3-Clause |
| orca | 48.6-1.3 | any | LGPL-2.1-or-later |
| ostree | 2024.5-1 | aarch64 | LGPL-2.0-or-later |
| p11-kit | 0.25.3-1 | aarch64 | BSD |
| pacman | 6.1.0-3 | aarch64 | GPL-2.0-or-later |
| pacman-mirrorlist | 20231001-1 | any | GPL |
| pacparser | 1.4.2-1 | aarch64 | LGPL |
| pam | 1.6.1-2 | aarch64 | GPL-2.0-only |
| pambase | 20230918-1 | any | GPL-3.0-or-later |
| pango | 1:1.52.2-1 | aarch64 | LGPL-2.1-or-later |
| parted | 3.6-1 | aarch64 | GPL3 |
| passt | 2026_06_11.a9c61ff-4 | aarch64 | BSD-3-Clause GPL-2.0-or-later |
| patch | 2.7.6-10 | aarch64 | GPL |
| patchelf | 0.18.0-2 | aarch64 | GPL3 |
| pax-utils | 1.3.7-1 | aarch64 | GPL2 |
| pciutils | 3.12.0-1 | aarch64 | GPL2 |
| pcre | 8.45-4 | aarch64 | BSD |
| pcre2 | 10.43-3 | aarch64 | BSD-3-Clause |
| pcsclite | 2.1.0-2 | aarch64 | BSD |
| perf | 6.18.0+gfbdbca41fd45-1 | aarch64 | GPL2 |
| perl | 5.38.2-1 | aarch64 | GPL PerlArtistic |
| perl-error | 0.17029-5 | any | PerlArtistic GPL |
| perl-mailtools | 2.21-7 | any | PerlArtistic GPL |
| perl-timedate | 2.33-5 | any | PerlArtistic |
| phonon-qt6 | 4.12.0-4 | aarch64 | LGPL |
| phonon-qt6-vlc | 0.12.0-2 | aarch64 | LGPL |
| pidbridge | 0.1.0-1 | aarch64 | MIT |
| pigz | 2.8-1 | aarch64 | custom |
| pinentry | 1.3.0-1 | aarch64 | GPL |
| pipewire | 1:1.6.8-1.2 | aarch64 | MIT LGPL-2.1-or-later |
| pipewire-alsa | 1:1.6.8-1.2 | aarch64 | MIT |
| pipewire-audio | 1:1.6.8-1.2 | aarch64 | MIT |
| pipewire-jack | 1:1.6.8-1.2 | aarch64 | MIT GPL-2.0-only LGPL-2.1-or-later |
| pipewire-pulse | 1:1.6.8-1.2 | aarch64 | MIT |
| pixman | 0.46.4-1 | aarch64 | MIT |
| pkgconf | 2.1.1-1 | aarch64 | ISC |
| plasma-activities | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| plasma-activities-stats | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| plasma-desktop | 6.2.5-2 | aarch64 | LGPL-2.0-or-later |
| plasma-integration | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| plasma-pa | 6.0.4-1 | aarch64 | LGPL-2.0-or-later |
| plasma-wayland-protocols | 1.19.0-1.1 | any | LGPL-2.0-or-later |
| plasma-workspace | 6.2.5-2 | aarch64 | LGPL-2.0-or-later |
| plasma5support | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| plocate | 1.1.22-1 | aarch64 | GPL2 |
| podman | 5.5.2-1 | aarch64 | Apache-2.0 |
| polkit | 124-2 | aarch64 | LGPL-2.0-or-later |
| polkit-kde-agent | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| polkit-qt6 | 0.200.0-1 | aarch64 | BSD-3-Clause GPL-2.0-or-later LGPL-2.0-o |
| poppler | 24.03.0-1 | aarch64 | GPL-2.0-only GPL-3.0-or-later MIT HPND-s |
| poppler-qt6 | 24.03.0-1 | aarch64 | GPL-2.0-only GPL-3.0-or-later MIT HPND-s |
| popt | 1.19-1 | aarch64 | custom |
| portaudio | 1:19.7.0-2 | aarch64 | MIT |
| powerdevil | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| prison | 6.14.0-1.1 | aarch64 | MIT |
| procps-ng | 4.0.4-3 | aarch64 | GPL LGPL |
| protobuf | 25.3-5 | aarch64 | BSD |
| protobuf-c | 1.5.0-2 | aarch64 | BSD |
| psmisc | 23.7-1 | aarch64 | GPL |
| purpose | 6.14.0-1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| python | 3.12.3-1 | aarch64 | PSF-2.0 |
| python-aiohttp | 3.9.3-2 | aarch64 | Apache |
| python-aiosignal | 1.3.1-6 | any | Apache |
| python-anyio | 4.3.0-3 | any | MIT |
| python-attrs | 23.2.0-3 | any | MIT |
| python-cairo | 1.26.0-2 | aarch64 | LGPL2.1 MPL |
| python-capng | 0.8.5-2 | aarch64 | GPL-2.0-or-later LGPL-2.1-or-later |
| python-certifi | 2024.02.02-2 | any | MPL-2.0 |
| python-cffi | 1.16.0-2 | aarch64 | MIT |
| python-cryptography | 42.0.5-2 | aarch64 | Apache |
| python-dbus-next | 0.2.3-5 | any | MIT |
| python-frozenlist | 1.4.1-2 | aarch64 | Apache |
| python-gobject | 3.48.2-1 | aarch64 | LGPL-2.1-or-later |
| python-h11 | 0.14.0-3 | any | MIT |
| python-httpcore | 1.0.2-3 | any | BSD |
| python-httpx | 0.26.0-3 | any | BSD |
| python-idna | 3.6-2 | any | BSD |
| python-inotify-simple | 1.3.5-6 | any | BSD |
| python-mako | 1.3.3-2 | any | MIT |
| python-markupsafe | 2.1.5-2 | aarch64 | BSD-3-Clause |
| python-minidump | 0.0.24-2 | any | MIT |
| python-multidict | 6.0.5-2 | aarch64 | Apache-2.0 |
| python-packaging | 23.2-3 | any | Apache |
| python-pip | 24.0-2 | any | MIT |
| python-protobuf | 25.3-5 | aarch64 | BSD |
| python-psutil | 5.9.8-4 | aarch64 | BSD-3-Clause |
| python-pyalsa | 1.2.7-5 | aarch64 | LGPL-2.1-or-later |
| python-pycparser | 2.22-2 | any | BSD |
| python-pyelftools | 0.31-2 | any | custom:Public Domain |
| python-pyenchant | 3.2.2-3 | any | LGPL |
| python-pyxdg | 0.28-3 | any | LGPL |
| python-semantic-version | 2.10.0-5 | any | BSD |
| python-setproctitle | 1.3.3-2 | aarch64 | BSD |
| python-sniffio | 1.3.1-3 | any | MIT |
| python-tqdm | 4.66.2-2 | any | MIT MPL-2.0 |
| python-typing_extensions | 4.11.0-1 | any | Python-2.0.1 |
| python-wheel | 0.43.0-4 | any | MIT |
| python-yaml | 6.0.1-3 | aarch64 | MIT |
| python-yarl | 1.9.4-2 | aarch64 | Apache-2.0 |
| pyzy | 1.1-2.5 | aarch64 | LGPL |
| qca-qt6 | 2.3.8-3 | aarch64 | LGPL-2.1-or-later |
| qcoro | 0.11.0-1.1 | aarch64 | MIT |
| qqc2-breeze-style | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| qqc2-desktop-style | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| qrencode | 4.1.1-2 | aarch64 | GPL |
| qt5-base | 5.15.16+kde+r130-3 | aarch64 | GPL3 LGPL3 FDL custom |
| qt5-svg | 5.15.16+kde+r5-3 | aarch64 | GPL3 LGPL3 FDL custom |
| qt5-translations | 5.15.16-3 | any | GPL3 LGPL3 FDL custom |
| qt5-x11extras | 5.15.16-3 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-5compat | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-base | 6.8.0-2.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-declarative | 6.8.0-1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-multimedia | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-multimedia-ffmpeg | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-positioning | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-sensors | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-shadertools | 6.8.0-1.1 | aarch64 | GPL3 |
| qt6-speech | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-svg | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-tools | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-translations | 6.8.0-1.1 | any | GPL3 LGPL3 FDL custom |
| qt6-virtualkeyboard | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-wayland | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-webchannel | 6.7.0-1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-webengine | 6.7.0-1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-websockets | 6.8.0-1.1 | aarch64 | GPL3 LGPL3 FDL custom |
| qt6-webview | 6.8.0-1 | aarch64 | GPL3 LGPL3 FDL custom |
| rauc | 1.14-1.1 | aarch64 | LGPL-2.1-or-later |
| rav1e | 0.7.1-1 | aarch64 | custom:BSD |
| readline | 8.3.001-1 | aarch64 | GPL-3.0-or-later |
| renderdoc | 1.45+git7fa6d123-1 | aarch64 | MIT |
| renderdoc-android-apks | 1.45+git7fa6d123-1 | aarch64 | MIT |
| rhash | 1.4.4-1 | aarch64 | BSD |
| ripgrep | 14.1.0-1 | aarch64 | MIT custom |
| ripgrep-all | 0.10.6-3 | aarch64 | AGPL3 |
| rsync | 3.3.0-1 | aarch64 | GPL3 |
| rtkit | 0.14-2 | aarch64 | GPL-3.0-or-later AND MIT |
| rubberband | 3.3.0-1 | aarch64 | GPL2 |
| run-parts | 5.17-1 | aarch64 | GPL |
| sbc | 2.0-1 | aarch64 | GPL LGPL |
| screen | 5.0.0-2.1 | aarch64 | GPL-3.0-or-later |
| scx-scheds | 1.1.2.linux.steamos.3-1 | aarch64 | GPL-2.0-only |
| sddm | 0.21.0-8 | aarch64 | GPL-2.0-or-later |
| sdl2 | 2.30.2-1 | aarch64 | MIT |
| sdl2_ttf | 2.22.0-1 | aarch64 | MIT |
| seatd | 0.8.0-1 | aarch64 | MIT |
| sed | 4.9-3 | aarch64 | GPL3 |
| serd | 0.32.2-1 | aarch64 | 0BSD OR ISC BSD-3-Clause ISC |
| shaderc | 2023.8-1 | aarch64 | Apache |
| shadow | 4.15.1-2 | aarch64 | BSD-3-Clause |
| shared-mime-info | 2.4-1 | aarch64 | GPL2 |
| signon-kwallet-extension | 24.02.2-1 | aarch64 | LGPL-2.0-or-later |
| signon-plugin-oauth2 | 0.25-3 | aarch64 | LGPL |
| signon-ui | 0.17+20231016-2 | aarch64 | GPL |
| signond | 8.61-3 | aarch64 | LGPL |
| slang | 2.3.3-2 | aarch64 | GPL |
| smartmontools | 7.4-1 | aarch64 | GPL |
| smbclient | 4.20.0-3 | aarch64 | GPL-3.0-or-later |
| snappy | 1.1.10-1 | aarch64 | BSD |
| socat | 1.8.0.0-1 | aarch64 | GPL-2.0-only |
| softapmanager | 1.2.23-1 | aarch64 | Proprietary |
| solid | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| sonnet | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| sord | 0.16.16-1 | aarch64 | ISC |
| sound-theme-freedesktop | 0.8-5 | any | custom |
| source-highlight | 3.1.9-12 | aarch64 | GPL |
| spandsp | 0.0.6-5 | aarch64 | LGPL2.1 |
| spdlog | 1.13.0-1 | aarch64 | MIT |
| speech-dispatcher | 0.11.5-2 | aarch64 | GPL2 FDL |
| speex | 1.2.1-1 | aarch64 | BSD |
| speexdsp | 1.2.1-1 | aarch64 | BSD |
| spirv-tools | 1:1.4.309.0-4 | aarch64 | Apache-2.0 |
| sqlite | 3.45.3-1 | aarch64 | LicenseRef-Sqlite |
| squashfs-tools | 4.6.1-1 | aarch64 | GPL2 |
| squashfuse | 0.5.2-1 | aarch64 | LicenseRef-squashfuse |
| sratom | 0.6.16-1 | aarch64 | ISC |
| srt | 1.5.3-1 | aarch64 | MPL2 |
| sshpass | 1.10-1 | aarch64 | GPL |
| startup-notification | 0.12-8 | aarch64 | LGPL |
| steam | 1.0.0.85-2 | aarch64 | LicenseRef-steam-subscriber-agreement |
| steam-devices | 1.0.0.85-2 | aarch64 | MIT |
| steam-im-modules | jupiter.20240131-3 | aarch64 | GPL3 |
| steam_notif_daemon | v1.0.1-3 | aarch64 | MIT |
| steamdeck-kde-presets | 3.8.3-7 | any | GPL2 |
| steamos-atomupd-client | 0.20260331.1-1 | any | LGPL2.1 |
| steamos-customizations-deckard | 20260823.1-2 | any | LGPLv2+ |
| steamos-devkit-service | 0.20250916.0-3 | any | LGPL-2.1+ |
| steamos-efi | 20260122.1-5 | aarch64 | GPL2 |
| steamos-log-submitter | 0.8.3+g50d60e44a4a5-1 | any | LGPL2.1 |
| steamos-manager | 26.4.1-2 | aarch64 | MIT |
| steamos-passwd | 1.0-1 | any | LGPL2.1 |
| steamos-powerbuttond | 3.2+git4403691c-2 | aarch64 | BSD |
| steamos-reset | 20241008.1-4 | aarch64 | GPL |
| steamos-systemreport | 0.21-1 | any | LGPL2.1 |
| strace | 6.16-1 | aarch64 | LGPL-2.1-or-later |
| sudo | 1.9.15.p5-1 | aarch64 | custom |
| svt-av1 | 2.0.0-1 | aarch64 | BSD custom: Alliance for Open Media Pate |
| syndication | 6.14.0-1.1 | aarch64 | LGPL-2.0-only LGPL-3.0-only |
| syntax-highlighting | 6.14.0-1.1 | aarch64 | MIT |
| systemd | 257.7-2.2 | aarch64 | LGPL-2.1-or-later CC0-1.0 GPL-2.0-or-lat |
| systemd-libs | 257.7-2.2 | aarch64 | LGPL-2.1-or-later CC0-1.0 GPL-2.0-or-lat |
| systemd-resolvconf | 257.7-2.2 | aarch64 | LGPL-2.1-or-later |
| systemd-sysvcompat | 257.7-2.2 | aarch64 | LGPL-2.1-or-later |
| systemsettings | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| taglib | 2.0.1-1 | aarch64 | LGPL-2.1-only MPL-1.1 |
| talloc | 2.4.2-2 | aarch64 | GPL-3.0-or-later |
| tar | 1.35-2 | aarch64 | GPL3 |
| tcpdump | 4.99.4-1 | aarch64 | BSD |
| tdb | 1.4.10-3 | aarch64 | GPL-3.0-or-later |
| tevent | 1:0.16.1-3 | aarch64 | GPL-3.0-or-later |
| texinfo | 7.1-2 | aarch64 | GPL3 |
| tmux | 3.4-6 | aarch64 | BSD |
| tpm2-tss | 4.0.1-1 | aarch64 | BSD |
| trace-cmd | 3.2-3 | aarch64 | GPL-2.0-only LGPL-2.1-only |
| tracker3 | 3.7.2-1 | aarch64 | LGPL-2.1-or-later |
| tslib | 1.23-1 | aarch64 | LGPL-2.1-only |
| ttf-hack | 3.003-6 | any | custom:ttf-hack |
| twolame | 0.4.0-3 | aarch64 | LGPL2.1 |
| tzdata | 2024a-1 | aarch64 | LicenseRef-tz |
| uboot-tools | 20260908.1-2 | aarch64 | GPL |
| udisks2 | 2.10.1-4 | aarch64 | GPL-2.0-or-later LGPL-2.0-or-later |
| ufs-utils | 7.14.12-1 | aarch64 | GPL2 |
| unzip | 6.0-20 | aarch64 | custom |
| upower | 1.90.4-1 | aarch64 | GPL-2.0-or-later |
| usb-gadget-tools | 20260331.1-1 | any | MIT |
| usbutils | 017-1 | aarch64 | GPL-2.0-only GPL-2.0-or-later GPL-3.0-on |
| util-linux | 2.40-3 | aarch64 | BSD-2-Clause BSD-3-Clause BSD-4-Clause-U |
| util-linux-libs | 2.40-3 | aarch64 | BSD-2-Clause BSD-3-Clause BSD-4-Clause-U |
| v4l-utils | 1.33.0-1 | aarch64 | LGPL |
| valgrind | 3.25.0-1.1 | aarch64 | GPL-2.0-or-later |
| vapoursynth | R66-2 | aarch64 | LGPL2.1 custom:OFL |
| vi-vim-symlink | 1-2 | any | CDDL |
| vid.stab | 1.1.1-1 | aarch64 | GPL |
| vim | 9.1.0378-1 | aarch64 | custom:vim |
| vim-runtime | 9.1.0378-1 | aarch64 | custom:vim |
| vlc | 3.0.20-8 | aarch64 | GPL-2.0-or-later LGPL-2.1-or-later |
| vmaf | 3.0.0-1 | aarch64 | BSD |
| volume_key | 0.3.12-9 | aarch64 | GPL2 |
| vte-common | 0.76.1-1 | aarch64 | LGPL-3.0-or-later GPL-3.0-or-later |
| vte3 | 0.76.1-1 | aarch64 | LGPL-3.0-or-later GPL-3.0-or-later |
| vulkan-extra-layers | 1.4.309.0-4 | aarch64 | Apache-2.0 |
| vulkan-extra-tools | 1.4.309.0-4 | aarch64 | Apache-2.0 |
| vulkan-headers | 1:1.4.309.0-4 | any | Apache-2.0 OR MIT |
| vulkan-icd-loader | 1.4.309.0-4 | aarch64 | Apache-2.0 |
| vulkan-tools | 1.4.309.0-4 | aarch64 | custom |
| wavpack | 5.7.0-1 | aarch64 | BSD |
| wayland | 1.26.0-1 | aarch64 | MIT |
| wayland-protocols | 1.49-1 | any | MIT |
| webrtc-audio-processing | 2.1-6 | aarch64 | BSD-3-Clause |
| wget | 1.24.5-1 | aarch64 | GPL3 |
| which | 2.21-6 | aarch64 | GPL3 |
| wireguard-tools | 1.0.20210914-2 | aarch64 | GPL |
| wireless-regdb | 2026.02.04-1 | any | LicenseRef-custom |
| wireless_tools | 30.pre9-3 | aarch64 | GPL |
| wireplumber | 0.5.14-1.3 | aarch64 | MIT |
| wpa_supplicant | 2:2.10-9 | aarch64 | GPL |
| x264 | 3:0.164.r3108.31e19f9-1 | aarch64 | GPL |
| x265 | 3.5-3 | aarch64 | GPL |
| xbitmaps | 1.1.3-1 | any | custom |
| xcb-proto | 1.17.0-2 | any | X11-distribute-modifications-variant |
| xcb-util | 0.4.1-1 | aarch64 | custom |
| xcb-util-cursor | 0.1.5-1 | aarch64 | custom:MIT |
| xcb-util-errors | 1.0.1-1 | aarch64 | MIT |
| xcb-util-image | 0.4.1-2 | aarch64 | custom |
| xcb-util-keysyms | 0.4.1-4 | aarch64 | custom |
| xcb-util-renderutil | 0.3.10-1 | aarch64 | custom |
| xcb-util-wm | 0.4.2-1 | aarch64 | custom |
| xdg-dbus-proxy | 0.1.5-1 | aarch64 | LGPL |
| xdg-desktop-portal | 1.18.4-1 | aarch64 | LGPL-2.1-or-later |
| xdg-desktop-portal-gamescope | 0.1.23.bfbf0e3-1 | aarch64 | BSD-3-Clause |
| xdg-desktop-portal-holo | 0.1.14.bb73298-2 | aarch64 | LGPL-2.1-or-later |
| xdg-desktop-portal-kde | 6.2.5-1.1 | aarch64 | LGPL-2.0-or-later |
| xdg-user-dirs | 0.18-1 | aarch64 | GPL |
| xdg-utils | 1.2.1-1 | any | MIT |
| xf86-input-libinput | 1.4.0-1 | aarch64 | custom:MIT |
| xf86-video-vesa | 2.6.0-2 | aarch64 | MIT |
| xkeyboard-config | 2.41-1 | any | LicenseRef-xkeyboard-config |
| xorg-bdftopcf | 1.1.1-1 | aarch64 | custom |
| xorg-docs | 1.7.3-2 | any | MIT AND X11 AND BSD-2-Clause |
| xorg-font-util | 1.4.1-1 | aarch64 | custom |
| xorg-fonts-100dpi | 1.0.4-2 | any | custom |
| xorg-fonts-75dpi | 1.0.4-1 | any | custom |
| xorg-fonts-alias-100dpi | 1.0.5-1 | any | custom |
| xorg-fonts-alias-75dpi | 1.0.5-1 | any | custom |
| xorg-fonts-encodings | 1.1.0-1 | any | LicenseRef-xorg-fonts-encodings |
| xorg-iceauth | 1.0.10-1 | aarch64 | MIT-open-group |
| xorg-mkfontscale | 1.2.3-1 | aarch64 | MIT-open-group X11 MIT HPND-sell-variant |
| xorg-server | 21.1.13-1 | aarch64 | LicenseRef-Adobe-Display-PostScript BSD- |
| xorg-server-common | 21.1.13-1 | aarch64 | LicenseRef-Adobe-Display-PostScript BSD- |
| xorg-server-devel | 21.1.13-1 | aarch64 | LicenseRef-Adobe-Display-PostScript BSD- |
| xorg-server-xephyr | 21.1.13-1 | aarch64 | LicenseRef-Adobe-Display-PostScript BSD- |
| xorg-server-xnest | 21.1.13-1 | aarch64 | LicenseRef-Adobe-Display-PostScript BSD- |
| xorg-server-xvfb | 21.1.13-1 | aarch64 | MIT GPL-2.0-only |
| xorg-sessreg | 1.1.3-1 | aarch64 | custom |
| xorg-setxkbmap | 1.3.4-1 | aarch64 | custom |
| xorg-smproxy | 1.0.7-1 | aarch64 | custom |
| xorg-util-macros | 1.20.1-1 | any | HPND-sell-variant AND MIT |
| xorg-x11perf | 1.6.2-1 | aarch64 | custom |
| xorg-xauth | 1.1.3-1 | aarch64 | MIT-open-group |
| xorg-xbacklight | 1.2.3-3 | aarch64 | custom |
| xorg-xcmsdb | 1.0.6-1 | aarch64 | custom |
| xorg-xcursorgen | 1.0.8-1 | aarch64 | custom |
| xorg-xdpyinfo | 1.3.4-1 | aarch64 | custom |
| xorg-xdriinfo | 1.0.7-1 | aarch64 | custom |
| xorg-xev | 1.2.6-1 | aarch64 | MIT |
| xorg-xgamma | 1.0.7-1 | aarch64 | custom |
| xorg-xhost | 1.0.9-1 | aarch64 | custom |
| xorg-xinput | 1.6.4-1 | aarch64 | custom |
| xorg-xkbcomp | 1.4.7-1 | aarch64 | LicenseRef-xkbcomp |
| xorg-xkbevd | 1.1.5-1 | aarch64 | custom |
| xorg-xkbutils | 1.0.6-1 | aarch64 | LicenseRef-xkbutils |
| xorg-xkill | 1.0.6-1 | aarch64 | custom |
| xorg-xlsatoms | 1.1.4-1 | aarch64 | custom |
| xorg-xlsclients | 1.1.5-1 | aarch64 | custom |
| xorg-xmessage | 1.0.7-1 | aarch64 | X11 |
| xorg-xmodmap | 1.0.11-1 | aarch64 | custom |
| xorg-xpr | 1.2.0-1 | aarch64 | MIT |
| xorg-xprop | 1.2.7-1 | aarch64 | LicenseRef-xprop |
| xorg-xrandr | 1.5.2-1 | aarch64 | custom |
| xorg-xrdb | 1.2.2-1 | aarch64 | custom |
| xorg-xrefresh | 1.1.0-1 | aarch64 | MIT |
| xorg-xset | 1.2.5-1 | aarch64 | custom |
| xorg-xsetroot | 1.1.3-1 | aarch64 | custom |
| xorg-xvinfo | 1.1.5-1 | aarch64 | custom |
| xorg-xwayland | 24.1.9-1.2 | aarch64 | LicenseRef-Adobe-Display-PostScript BSD- |
| xorg-xwd | 1.0.9-1 | aarch64 | custom |
| xorg-xwininfo | 1.1.6-1 | aarch64 | custom |
| xorg-xwud | 1.0.6-1 | aarch64 | custom |
| xorgproto | 2024.1-2 | any | BSD-2-Clause HPND HPND-sell-variant ICU  |
| xorgxrdp-glamor | 0.10.5-1 | aarch64 | X11 |
| xrdp | 0.10.5-2 | aarch64 | Apache-2.0 |
| xterm | 396-2.1 | aarch64 | custom |
| xvidcore | 1.3.7-2 | aarch64 | GPL |
| xxhash | 0.8.2-1 | aarch64 | GPL2 BSD |
| xz | 5.6.1-3 | aarch64 | GPL LGPL custom |
| yajl | 2.1.0-6 | aarch64 | ISC |
| zenity | 4.0.1-1 | aarch64 | LGPL-2.1-or-later |
| zeromq | 4.3.5-2 | aarch64 | MPL2 |
| zimg | 3.0.5-1 | aarch64 | custom:WTFPL |
| zip | 3.0-11 | aarch64 | custom |
| zix | 0.4.2-2 | aarch64 | 0BSD OR ISC |
| zlib | 1:1.3.1-1 | aarch64 | Zlib |
| zram-generator | 1.2.1-1 | aarch64 | MIT |
| zstd | 1.5.5-1 | aarch64 | BSD GPL2 |
| zxing-cpp | 2.2.1-1 | aarch64 | Apache |
