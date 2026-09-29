# Holo Core preview vs Steam Frame 0.3.0

Checked on 2026-09-29, with no guest and no X: package databases and file
lists only. This page compares the public `holo-core-aarch64-preview`
repository with the root of the Steam Frame 0.3.0 recovery image, and says
which ARM64 base SteamARM should run Valve's native arm64 client on. The
run log is `benchmarks/stage24-holo-vs-frame.txt`.

Labels: MEASURED (a command and its result on the Mac), VERIFIED IN SOURCE,
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN.

**Privacy.** The image's own `etc/pacman.conf` points at private per-device
mirrors. They were not fetched and are not written here. Only the public
preview repository was read.

## Sources (MEASURED)

**Steam Frame.** `steamframe-oobe-repair-20260922.5153644-0.3.0.img`
(SteamOS 0.3.0 `holo`, `BUILD_ID=20260922.5152327`), extracted read-only at
`/Volumes/SteamFrameRoot/rootfs` (`docs/STEAM_FRAME_INVENTORY.md`). Its
pacman database `usr/lib/holo/pacmandb/local` has 965 packages: 863
`aarch64` and 102 `any`.

**Holo Core preview.** `https://holo-packages.steamos.cloud/holo-core-aarch64-preview/`,
read on 2026-09-29 at 16:46 UTC. The directory has three snapshots:

- `mash-20251118.2/` (2026-06-06);
- `mash-20251118.3/` (2026-07-10);
- `mash-20251118/`, which answers with a 307 redirect to `mash-20251118.3/`.

`mash-20251118.3` holds `core`, `extra`, `core-debug` and `extra-debug`,
plus `pacman.conf`, `pacman.mirrorlist` and `system.rootfs.zst`. Its
`pacman.conf` has `[core]` and `[extra]` with `SigLevel = Optional`, and its
mirror list points at `mash-20251118/$repo/os/$arch`.

Databases fetched from
`https://holo-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/<repo>/os/aarch64/<file>`
at 16:47-16:57 UTC:

| file | Last-Modified (GMT) | bytes | packages | sha256 |
|---|---|---:|---:|---|
| `core/.../core.db` (xz) | 2026-07-10 13:47:22 | 43,340 | 254 | `749c5120622a0218a83259e3d3e0d70383e7e29ff710f419f068024a29aa28e5` |
| `extra/.../extra.db` (xz) | 2026-07-10 14:12:40 | 656,616 | 4,306 | `cca1ced1cf1eb64c4b507c64b968c7a696fd4f67a546e3f35403d56d6a8bbe05` |
| `core-debug/.../core-debug.db` | 2026-07-10 14:14:12 | 22,440 | 160 | `c35c462720d124681d8655276ea9101de5e05b3f8b56a9b17f437e99fbd4bd68` |
| `extra-debug/.../extra-debug.db` | 2026-07-10 14:21:54 | 255,544 | 2,012 | `8ee09e166f5f11ae0df06968210cb047a470b509a73b7cf719c802c36d66320a` |
| `core/.../core.files` | 2026-07-10 13:47:24 | 781,992 | | `f22a78846c05bc958c7134f6e6f1158ae99f705e879016071ab97b0c05444f86` |
| `extra/.../extra.files` | 2026-07-10 14:13:10 | 8,108,164 | | `6935528e97068d7b9a913d826cc6ad199183d408b651815855616994f707164d` |

`core.db.sig` and `extra.db.sig` return 404. The repository is unsigned, so
the only integrity check is each package's `SHA256SUM` in a database fetched
over HTTPS. The databases are metadata. Each package's licence is the
`LICENSE` field quoted in the tables below.

Two packages were downloaded to read their ELF headers, and their sha256
matched the database:

- `core/os/aarch64/gawk-5.3.2-1-aarch64.pkg.tar.zst`, sha256
  `e2008e50f4c7828a48e3ad57ebc1c8d72bbe319f4ba02b122f598848738e04b0`,
  GPL-3.0-or-later;
- `extra/os/aarch64/busybox-1.36.1-2.2-aarch64.pkg.tar.zst`, sha256
  `4f00454b717148a7bb865be7ac1845124670234f414ac072215e1751fd01eae2`,
  GPL-2.0-only.

Nothing downloaded is committed or installed.

Command (the `compare` fixes it needed are below):

```sh
scripts/steamframe-image.py compare /Volumes/SteamFrameRoot/rootfs core.db extra.db \
    --md compare.md --json compare.json
```

## Aggregate (MEASURED)

These are the Frame's 965 packages against Holo `core` + `extra` (4,560).
Newer and older are pacman's `vercmp` order.

| | packages |
|---|---:|
| same name, same version | 81 |
| same name, different version | 806 |
| — Holo newer | 764 |
| — Frame newer | 42 |
| — pkgrel only (a rebuild of the same upstream version; 154 Holo newer, 14 Frame newer) | 168 |
| — upstream version | 638 |
| only in the Frame | 78 (5 have a Holo counterpart under another name) |
| only in Holo | 3,673 |

With `core-debug` and `extra-debug` added, `glibc-debug` moves from Frame-only to
"differs" (2.39-2 against 2.42): 81 / 807 / 77 / 5,844.

**The Frame is not built from this repository.** The two share 887 names,
and only 81 of them (9 %) have the same version. Holo is newer for 764 of the
806 that differ. Section 3.3 of `STEAM_FRAME_SNAPSHOT_2026-09-29.md` had
this as a community observation; it is now measured.

**What the Frame's package set is.** Its version and date fields show this
(MEASURED):

- every `aarch64` package was built on 2025-05-07 or later;
- 574 of the 965 packages were built on the same day, 2025-10-16;
- the 72 packages built before 2025 are all `any` (architecture-free), and
  the newest of them is from 2024-04-26.

The versions are glibc 2.39, python 3.12.3, openssl 3.2.1, curl 8.7.1, nss
3.99 and qt6-base 6.8.0. HYPOTHESIS: an Arch package set of about April 2024,
rebuilt for aarch64 from October 2025, plus Valve's newer pieces. Holo's
README names its base as Arch's state of 2025-11-18
(`docs/HOLO_CORE_ARM64_AUDIT.md`). Its `core` was built around 2026-07-03
(median `BUILDDATE`).

**Where the Frame is newer (42).** These are 28 upstream versions:

- the PipeWire stack at 1.6.8: `pipewire`, `-alsa`, `-audio`, `-jack`,
  `-pulse`, `libpipewire` and `alsa-card-profiles`. Holo has 1.4.9;
- `wireplumber` and `libwireplumber` 0.5.14;
- gamescope 3.16.28 (Holo 3.16.17);
- the kernel-derived packages at 6.18: `linux-api-headers`, `perf`, `bpf`;
- wayland 1.26, wayland-protocols 1.49, libdrm 2.4.134, libxkbcommon
  1.13.2 (with `-x11`);
- passt 2026_06_11, rtkit, renderdoc, v4l-utils, wireless-regdb,
  uboot-tools, busybox 1.37.0, desync, nvtop and libtracefs.

The other 14 are Valve rebuilds (pkgrel only), for example `bash`,
`xorg-xwayland` and `ibus`.

**Only in the Frame (78).** The categories are by package name:

| category | n | packages |
|---|---:|---|
| Frame hardware, boot, kernel | 20 | `deckard-*` hardware packages (audio-config, boot-images, charger, eeprom, fan-control, firewall, fpga, hw-support, led-control, power-monitor, typec-logger, uboot, uboot-splctl, vulkan-layers-linux-aarch64), `linux-618-deckard`, `linux-618-v4l2loopback-deckard`, `linux-firmware-deckard`, `softapmanager`, `ufs-utils`, `usb-gadget-tools` |
| SteamOS system services | 19 | `steamos-*` (atomupd-client, customizations-deckard, devkit-service, efi, log-submitter, manager, passwd, powerbuttond, reset, systemreport), `atomupd-daemon`, `holo-zram-swap`, `steamdeck-kde-presets`, `steam_notif_daemon`, `xdg-desktop-portal-gamescope`, `xdg-desktop-portal-holo`, `gpu-trace`, `pidbridge`, `scx-scheds` |
| Steam client, bootstrap, SteamVR | 6 | `steam`, `steam-devices`, `steam-im-modules`, `deckard-steam-rel`, `deckard-steamvr-rel`, `deckard-steamvr-session` |
| Valve's graphics builds and tools | 7 | `deckard-mesa-linux-aarch64`, `deckard-mesa-linux-x86_64`, `deckard-mesa-linux-deps-x86_64`, `vulkan-extra-layers`, `vulkan-extra-tools`, `mangohud`, `gfxreconstruct` |
| Android (Lepton) side | 5 | `android-boringssl`, `android-platform-tools`, `deckard-mesa-android-aarch64`, `deckard-vulkan-layers-android-aarch64`, `renderdoc-android-apks` |
| renamed in Holo | 4 | `sdl2` (Holo: `sdl2-compat`), `holo-glibc-locales` (`glibc-locales`), `dbus-python` (`python-dbus`), `tracker3` (`tinysparql`); also `deckard-mesa-linux-aarch64` (`mesa`), counted above |
| debug | 1 | `glibc-debug` (in `core-debug`) |
| other Arch packages Holo does not carry | 16 | **`gtk2`**, `breakpad`, `clang-libs`, `f3`, `http-parser`, `ibus-anthy`, `ibus-pinyin`, `ibus-table-cangjie-lite`, `orca`, `python-inotify-simple`, `pyzy`, `sddm`, `strace`, `vi-vim-symlink`, `xorgxrdp-glamor`, `xrdp` |

**Only in Holo (3,673).** Most are desktop, development and server packages
a device image leaves out. Worth naming:

- `mesa` 25.2.7 with `vulkan-swrast` (lavapipe), `vulkan-freedreno`,
  `vulkan-asahi` and other drivers;
- `sdl3` 3.2.26 and `sdl2-compat`;
- `glibc-locales`;
- `linux` 6.17.8;
- `openxr`;
- aarch64 `wine` 10.19;
- the `qemu-*` family 10.1.2 (never used: ZERO-VM).

There is no FEX, box64 or Steam package in either repository database
(MEASURED: no such name).

## The client's libraries in Holo (MEASURED)

- **Seed sonames.** `scripts/mkarmroot.sh` seeds the Fedora root with the
  51 sonames the client links. Holo's `core.files` and `extra.files` ship 50
  of them in `usr/lib`. The one missing is `libgtk-x11-2.0.so.0`: Holo has
  no GTK 2.
- **GTK 2 is needed.** `steamrtarm64/steamui.so` and
  `steamrtarm64/vgui2_s.so` of the client in the Fedora root's home (build
  of 2026-09-03) name it in `DT_NEEDED` (read with a Python ELF parser;
  nothing was run). A root built from Holo packages alone cannot load the
  client's UI. The Frame has `gtk2` 2.24.33-3, and Fedora `gtk2`
  2.24.33-23.fc43.
- **Mesa.** Holo's `mesa` 1:25.2.7-1 ships `dri/swrast_dri.so`,
  `kms_swrast_dri.so`, `zink_dri.so`, `libGLX_indirect.so.0` and
  `libGLX_mesa.so.0`, and depends on `llvm-libs`. The Frame's
  `deckard-mesa-linux-aarch64` has no `swrast_dri.so` and no
  `libGLX_indirect.so.0`. That is why the Frame root needs
  `MESA_LOADER_DRIVER_OVERRIDE=swrast` and indirect GLX
  (`benchmarks/stage23-frame-root.txt`). HYPOTHESIS: on Holo's Mesa, GLX
  would fall back to indirect without those variables, as on Arch.
- **awk.** Holo's `gawk` 5.3.2 is PIE (`ET_DYN`, `p_align` 0x10000). The
  Frame's `gawk` 5.3.0 is `ET_EXEC`, which `runtime/elf.c` refuses, and
  `awk` links to it. `busybox` is static `ET_EXEC` in both.
- **Present in both.** `libx11` 1.8.12 ships `usr/share/X11/locale`
  (`C/`, `en_US.UTF-8/`, `locale.dir`, `compose.dir`). `glibc-locales` has
  `en_US.utf8` and `es_ES.utf8`, and `glibc` has `C.utf8`. `nss` 3.117 ships
  `libsoftokn3.so`, `libfreeblpriv3.so` and `libnssckbi.so`. `lsof`
  4.99.5 and `ca-certificates-utils` (`etc/ssl/certs/ca-certificates.crt`)
  are there too. Stage 22 added the same pieces to the Fedora root.

## Key components

Columns:

- **Holo**: `holo-core-aarch64-preview` `mash-20251118.3`.
- **Frame**: the 0.3.0 recovery image.
- **Fedora armroot**: versions from `scripts/mkarmroot.lock`, Fedora 43.
- **Arch**: `aarch64` unless marked `any`; the same on both sides.
- **Open source?**: the `LICENSE` field in the package metadata.
- **ZERO-VM**: whether SteamARM's guest root or runtime needs the component.

All versions are MEASURED from the databases and the lock.

| Component | Holo preview version | Steam Frame recovery version | Architecture | Open source? | Relevant to ZERO-VM? | Current ZERO-VM equivalent | Action |
|---|---|---|---|---|---|---|---|
| glibc | 2.42+r33+gde1fe81f4714-1 | 2.39-2 | aarch64 | yes, GPL-2.0-or-later, LGPL-2.1-or-later | yes: every native aarch64 guest loads it | Fedora armroot glibc 2.42-16.fc43. The Frame root uses its own 2.39. The client needs at most `GLIBC_2.29` (`docs/STEAM_FRAME_INVENTORY.md`) | none; either version works (stage 23: window on both roots) |
| glibc locales | `glibc-locales` 2.42+r33 (C.utf8 is in `glibc`) | `holo-glibc-locales` 2.39-2 (provides and replaces `glibc-locales`) | aarch64 | yes (GPL/LGPL) | yes: a missing locale is in the abort chain (stage 22) | Fedora `glibc-common` + `glibc-langpack-en` 2.42-16; Frame: its own | none |
| gcc-libs | 15.2.1+r301+gf24307422d1d-1 | 15.1.1+r500+gb1b8d8ce3eea-1.2 | aarch64 | yes, GPL-3.0-with-GCC-exception | yes (`libstdc++`, `libgcc_s`) | Fedora `libgcc`/`libstdc++` 15.3.1-1 | none |
| nss (+ nspr) | 3.117-1 (nspr 4.38.2-1) | 3.99-1 (nspr 4.35-2) | aarch64 | yes, MPL-2.0 | yes: the webhelper's NSS needs `libsoftokn3.so`/`libfreeblpriv3.so`; both ship them | Fedora nss 3.129.0-1, nspr 4.39.0-5 | none |
| libX11 + X locale data | `libx11` 1.8.12-1, `usr/share/X11/locale` inside | `libx11` 1.8.9-1, locale data inside | aarch64 | yes, MIT AND X11 | yes: without the locale data `vgui2_s.so` aborts (stage 22) | Fedora `libX11` + `libX11-common` 1.8.13-1 | none |
| mesa | `mesa` 1:25.2.7-1: swrast, kms_swrast, zink, `libGLX_indirect.so.0`, and lavapipe (`vulkan-swrast`) | `deckard-mesa-linux-aarch64` 26.3.0_devel+git8aa73b4b-1: zink and display-controller names only, Vulkan freedreno only | aarch64 | Holo: MIT AND BSD-3-Clause AND SGI-B-2.0. Frame: the field says `custom` (a Mesa git build; its licence text is not checked here) | GL for the client's windows: yes. The Vulkan drivers: no, SteamARM brings its own | Fedora mesa 25.3.6-3 (GLX through `libGLX_system`). Frame root: indirect GLX 1.4 with `MESA_LOADER_DRIVER_OVERRIDE=swrast` (`scripts/mkframeroot.sh`, stage 23) | none now. A Holo-based root would test whether plain indirect GLX works (HYPOTHESIS above) |
| vulkan-icd-loader | 1.4.328.1-1 | 1.4.309.0-4 | aarch64 | yes, Apache-2.0 | the loader: only as a fallback. Vulkan goes to SteamARM's shim | shim `build/libvulkan.so.1` → MoltenVK (or KosmicKrisp), stage 4 and `stage22-kosmickrisp.txt`. In the Frame root only on `LD_LIBRARY_PATH` (stage 23 V1). Fedora `vulkan-loader` 1.4.341.0 | unchanged: migration step 7 |
| sdl2 / sdl3 | `sdl2-compat` 2.32.58-1.2 + `sdl3` 3.2.26-1 | `sdl2` 2.30.2-1 (classic SDL2); no `sdl3` package | aarch64 | yes, Zlib (Frame's sdl2 field: MIT) | the client's `libSDL2-2.0.so.0`: yes. The client ships and loads its own SDL3 3.4.0 (stage 23) | Fedora `sdl2-compat` 2.32.72 + `SDL3` 3.4.16. The host launcher and `steamarm-inputd` use macOS SDL dylibs, not these | none |
| GTK 2 (not in the mission's list; it decides the base) | **absent** | `gtk2` 2.24.33-3 | aarch64 | yes, LGPL | yes: `steamui.so` and `vgui2_s.so` need `libgtk-x11-2.0.so.0` | Fedora `gtk2` 2.24.33-23 | blocks a Holo-only root |
| gamescope | 3.16.17-1 | 3.16.28-1 | aarch64 | yes. Holo: BSD-2-Clause, BSD-3-Clause, LicenseRef-Reshade. Frame: MIT | no: a DRM/KMS and libinput compositor | XQuartz rootless on `:2`, with Vulkan frames hosted over X windows through CALayerHost (stage 11-12) | none (REFERENCE_ONLY) |
| steam bootstrap | absent | `steam` 1.0.0.85-2 (bootstrap scripts) + `deckard-steam-rel` r1789506314+d1110443-1 (`steam.tar.zst`, client built 2026-09-15) | aarch64 | no: LicenseRef-steam-subscriber-agreement | the client: yes. The bootstrap script: not used | the client downloaded in stage 21 (`steam_client_linuxarm64`) in `<root>/tmp/armhome`, started directly as `steamrtarm64/steam`, not through `steam.sh` | the image's bootstrap is still untried (stage 23, open item 5); never redistribute |
| steamos-manager | absent | 26.4.1-2 | aarch64 | yes, MIT | no: a system D-Bus daemon for device settings | none. On the Frame root the client logs "SteamOSManager daemon not present" and carries on (stage 23) | none |
| pipewire | 1:1.4.9-1 | 1:1.6.8-1.2 (Frame newer) | aarch64 | yes, MIT, LGPL-2.1-or-later | the client library `libpipewire-0.3.so.0` only; no PipeWire daemon runs | PulseAudio on the Mac → CoreAudio; guests use `libpulse`. Fedora `pipewire-libs` 1.4.11 | none |
| podman / Lepton pieces | podman 5.6.2-1.2, crun 1.25-1, conmon 1:2.1.13-1, passt 2025_09_19, netavark 1.16.1-1, catatonit 0.2.1-2 | podman 5.5.2-1, crun 1.14.4-2, conmon 1:2.1.10-1, passt 2026_06_11 (Frame newer), netavark 1.10.3-1, catatonit 0.2.0-3; `android-platform-tools` 34.0.5 and `usr/share/guestos/android` (Frame only) | aarch64 | the container tools yes (Apache-2.0, LGPL, GPL). The Android payload: proprietary | no: container runtimes need namespaces, cgroups and netns from a Linux kernel. lxrun emulates only what pressure-vessel's bwrap needs | none. Lepton is OPEN: bionic under lxrun, no container runtime (`docs/LEPTON_REUSE_ANALYSIS.md`) | none (REFERENCE_ONLY) |
| FEX-related | no FEX, box64 or thunk package. `qemu-user` and `qemu-*` 10.1.2 are present | no FEX (MEASURED, inventory). `deckard-mesa-linux-x86_64` and `-deps-x86_64` (the x86 Mesa for FEX thunks under `usr/share/guestos/fex-mesa`) | x86 Mesa built as an `aarch64` package holding x86 files | FEX upstream is MIT. The x86 Mesa field: `custom` / `Various` | FEX: yes, for x86 and i386 game code only | SteamARM's own FEX `FEX-2609-113-g08f451d3b`, patched (`scripts/build-fex-host.sh`, `patches/`), in the Fedora `lxrt-root`, with x86 Vulkan thunked to the shim | keep SteamARM's FEX. Never qemu: `qemu-system` is a VM, and `qemu-user` would only duplicate FEX |
| python | 3.13.7-1 | 3.12.3-1 | aarch64 | yes, PSF-2.0 | no for the client started directly. The `steam` bootstrap package depends on python | the Fedora armroot has no python, and the client reaches its window there (stages 22-23) | none |
| lsof | 4.99.5-2 | 4.99.3-2 | aarch64 | yes (the field says `custom`: lsof's own permissive licence) | yes: the client runs `lsof` to vet WebUI connections (stage 22) | Fedora `lsof` 4.98.0-8; the Frame's own | none |
| ca-certificates | `ca-certificates` 20240618-1, `-mozilla` 3.117-1 | `ca-certificates` 20220905-1, `-mozilla` 3.99-1 | `any` + aarch64 | yes (GPL-2.0-or-later, MPL-2.0) | yes: TLS to Valve's servers | Fedora `ca-certificates` 2026.2.90_v9.0.317-1. On the Frame root the client reached Valve's servers with the 3.99 bundle (a 223,154 KB download and the sign-in QR code, stage 23 F2-F6) | none now; the Frame's trust store is the oldest of the three |

## Which base

Stage 23 measured the Fedora root and the Frame root with the same client
build, on the same Mac and with the same sampler
(`benchmarks/stage23-frame-root.txt`). No Holo root has been built.

| | Fedora armroot (`scripts/mkarmroot.sh`) | Frame-derived root (`scripts/mkframeroot.sh`) | Holo packages |
|---|---|---|---|
| client to its sign-in window | yes. Stage 23 C1/C4: BrowserReady 7-8 s, window 11-12 s; launcher 15-16 s in the post-merge check | yes, 5 of 5 runs. BrowserReady 15-17 s, window 17-19 s; launcher 21 s | UNKNOWN (never built). MEASURED: no GTK 2, so `steamui.so` would not load from Holo alone |
| webhelper launches, renderers | 1, 5 | 2 (a restart at start), 11 | UNKNOWN |
| summed RSS | 2.28-2.33 GB | 2.53-2.66 GB | UNKNOWN |
| workarounds in the root | X locale data and locales in the seeds (stage 22) | `resolv.conf`, indirect GLX with a driver override, and the client moves itself to `steamdeck_stable` | UNKNOWN; HYPOTHESIS: plain indirect GLX (Holo's Mesa has `libGLX_indirect.so.0`) |
| where it comes from | public Fedora 43 RPMs, each checked against the sha256 in `repomd.xml`, pinned in the committed `scripts/mkarmroot.lock` (no signature check; VERIFIED IN SOURCE, `scripts/mkroot-rpm.sh`) | the owner's copy of Valve's recovery image. Proprietary parts: local use only, never shipped | a public, unsigned Valve-hosted preview. Package sha256 only. Frozen since 2026-07-10 |
| versions | newest (glibc 2.42, nss 3.129, mesa 25.3.6) | oldest (glibc 2.39, nss 3.99, CA 2022-09) plus Valve's newer PipeWire, gamescope and kernel | middle (Arch 2025-11-18) |
| like the Frame? | no | it is the Frame | no: 81 of 887 shared names match |
| `ET_EXEC` files lxrun refuses | not surveyed for this root; the client and its libraries load (stages 22-23) | 45 (gawk and so `awk`, busybox, gcc...) | gawk is PIE; busybox static `ET_EXEC`; the rest not surveyed |

**Recommendation.**

1. **The native client's default base stays the Fedora armroot.** It
   reaches the same window 5-7 s sooner, with one webhelper start instead of
   two and about 0.25 GB less. It is built from public, pinned packages on
   any Mac, with no device image. The launcher's `steam-arm64` entry keeps
   `/tmp/lxrt-armroot`. Stage 23 drew the same conclusion.
2. **The Frame-derived root becomes the reference root, not the target.**
   It is Valve's own userspace for this client, and the only root where
   SteamOS behaviour shows: the `steamdeck_stable` branch, SteamOSManager,
   NetworkManager and atomupd. Keep it as the second experimental entry
   (`steam-arm64-frame`), to check the client against what Valve ships. It
   stays owner-only, and nothing from it is redistributed. Whether a
   desktop Mac should run the SteamOS branch at all is a HYPOTHESIS to
   settle before it could become a default.
3. **Holo packages: not a base now.**
   - They lack GTK 2, which the client's UI needs.
   - They carry no Steam client, bootstrap or SteamOS services.
   - They are unsigned, and the preview has not changed since 2026-07-10.
   - They do not reproduce the Frame (9 % of shared names match), so they
     bring neither Valve's tested set nor a maintained distribution.

   They stay a public source of Arch-layout aarch64 packages. Two findings
   would matter to a pacman-based root built without the image: Mesa with
   swrast and `libGLX_indirect.so.0`, and a PIE gawk. That root would need
   GTK 2 from elsewhere and a measured run of the client. It is not
   planned: the Fedora root already covers that role.

What would change this:

- a Frame root that starts as fast as the Fedora root;
- a client build that stops linking GTK 2;
- a Holo snapshot with GTK 2 and signatures;
- a need, after sign-in, for something only the SteamOS branch or the
  Frame root has (library, downloads, Proton routing: migration steps
  11-13).

## `compare` fixes (VERIFIED IN SOURCE, tested)

`scripts/steamframe-image.py compare` gave only "same / differs /
image-only / repo-only" from string equality. The fixes, each covered by
`tests/steamframe_image/compare.sh` (27 checks; python3 only, so it runs on
macOS):

- **Which side is newer.** Versions are ordered with a port of pacman's
  `alpm_pkg_vercmp`/`rpmvercmp`. It is checked against the 46 pairs of
  pacman's `test/util/vercmptest.sh`, in both directions, plus 3 pairs of
  its own. String order gets `1.5b` vs `1.5` and `10` vs `9` wrong.
- **pkgrel-only differences** (rebuilds) are told apart from upstream
  version changes.
- **Renamed packages** are linked through PROVIDES/REPLACES: `sdl2` ↔
  `sdl2-compat`, `holo-glibc-locales` ↔ `glibc-locales`,
  `deckard-mesa-linux-aarch64` ↔ `mesa`. Only packages that are on one
  side only are linked. A versioned REPLACES counts only inside its range:
  KF5's `kio5` replaces `kio<5.111`, not KF6's `kio`. `inventory` now
  records PROVIDES, REPLACES and BUILDDATE.
- **Sources recorded.** The report lists each database's size, package
  count and sha256. A database is named by its base name, or a URL the way
  `redact_url` prints it, so no home path and no private mirror reaches a
  report.
- **More inputs.** An extracted root is accepted in place of an inventory
  JSON. `--json` writes every row, `--states` filters the listed rows, and
  a zstd database decodes with the `zstd` command when the Python module is
  missing.
