# Holo Core ARM64 audit

First checked 2026-09-28. Holo Core is a candidate ARM64 userspace. No
kernel and no virtualisation mechanism is part of the final runtime.

## Sources and snapshot

- Official source: <https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview>
- Official package repository: <https://holo-packages.steamos.cloud/holo-core-aarch64-preview/>
- Revision cloned for reading: `67f0d559c82cdc5c94317bad53ae45409720ef59`
  (commit `readme: fix name ordering`, 2026-06-10).
- The source README names the base as Arch's state of 2025-11-18, plus ARM64
  changes and licence fixes, and calls it a technology preview, not a stable
  system with guarantees.
- The package index shows ARM64 `core` and `extra`; the newest visible
  revision was `mash-20251118.3` (tree listed 2026-07-10). Rechecked on
  2026-09-29: still `mash-20251118.3`, and `mash-20251118/` redirects there
  (`benchmarks/stage24-holo-vs-frame.txt`). Index count: 258
  entries in `core` and 4,312 in `extra`, including databases and `FILES`
  metadata; these are not signature counts. The `.sig` files for
  `btrfs-progs` and `core.db` were not published (404). Recording SHA-256
  and licence is not the same as verifying a signature. Check the newest
  state at each audit.
- The README offers `base` and `base-devel` container images for building.
  This project does not use them: official packages would be downloaded and
  extracted as userspace files. Zero VM, zero guest kernel, zero container
  runtime.

## Fit with ZERO-VM

**Candidate:** the Arch ARM64 filesystem, glibc and its loader, libraries,
and the packages FEX, the ARM64 Steam client (when it applies) and Linux →
Darwin dispatch need.

**Leave out of a minimal root:** kernel and initramfs, the boot chain,
systemd services that need a full kernel, Steam Frame firmware and drivers,
VR and the dashboard, Snapdragon device support, and any bootable image.
Keep licence files and package manifests.

The source repository is a set of package recipes, not an installed
userspace and not a complete Steam Frame OS. Compare its package DB and
binaries with the recovery image before taking versions or files. Do not
assume Proton ARM64, upstream FEX, gamescope or `SteamLinuxRuntime_4-arm64`
are part of the Holo repository.

## Minimal bootstrap, still to resolve

Resolve dependencies from Holo's package database; do not copy an ad hoc
list without package metadata. First set to evaluate: `filesystem`, `glibc`,
`gcc-libs`, `bash`, `coreutils`, certificates, `zlib`/`zstd`, and
`libx11`/XCB only if the smoke test needs them. Keep manifests, checksums
and licences; verify signatures when the vendor publishes them. The root
must run a trivial AArch64 binary, load glibc/pthread and `dlopen`, and run
FEX for x86-64 and i386 under `lxrun`.

## Compared with the Steam Frame image

The Steam Frame recovery image is a device disk: FAT32 ESPs, a btrfs
`rootfs-A`, ext4 `var-A` and `home`. Holo Core is a generic Arch ARM64
repository.

Since 2026-09-29 the image's root is extracted and inventoried (MEASURED,
`docs/STEAM_FRAME_INVENTORY.md`): SteamOS 0.3.0 `holo`, 965 packages,
glibc 2.39. Its pacman configuration points at private per-device mirrors on
`holo-packages.steamos.cloud`, not at `holo-core-aarch64-preview` (MEASURED,
`etc/pacman.conf`; the URLs are not published). It contains no FEX, Proton
or Steam Linux Runtime (MEASURED). Steam installs those as tools: the x86
client, once it took the host for arm64, installed Proton (ARM64), Steam
Linux Runtime 4.0 Arm64 and Valve's FEX (MEASURED,
`benchmarks/stage15-steam-proton-path.txt:24-31`).

## Package comparison (2026-09-29)

The package-by-package comparison is in `docs/HOLO_CORE_VS_STEAM_FRAME.md`
(record: `benchmarks/stage24-holo-vs-frame.txt`). It compares the image's
965 packages with `mash-20251118.3` `core` + `extra` (4,560 packages; the
databases' URLs, dates and sha256 are recorded there). Results (MEASURED):

- **Versions.** 81 packages have the same version and 806 differ: Holo is
  newer for 764, the Frame for 42 (PipeWire 1.6.8, gamescope 3.16.28,
  kernel 6.18 headers, wayland 1.26). 78 are only in the Frame and 3,673
  only in Holo.
- **The Frame is not built from the preview.** Its set is older: glibc 2.39
  against 2.42, nss 3.99 against 3.117. Every aarch64 package in it was
  built in 2025-05 or later, 574 of them on 2025-10-16. This answers the
  open question in section 3.3 of `STEAM_FRAME_SNAPSHOT_2026-09-29.md`.
- **Frame-only packages.** Hardware and boot (20), SteamOS services (19),
  the Steam client and SteamVR (6), Valve's Mesa builds (7), the Android
  side (5), renamed packages (4, e.g. `sdl2` → `sdl2-compat`), and 16 Arch
  packages Holo does not carry. **GTK 2** is one of those 16.
- **The client's libraries.** Holo ships 50 of the 51 sonames the native
  client's Fedora root is seeded with. The missing one is
  `libgtk-x11-2.0.so.0`, which the client's `steamui.so` and `vgui2_s.so`
  name in `DT_NEEDED`. So a root built from Holo packages alone cannot
  carry the native client's UI.
- **Where Holo is better placed.** Its `mesa` 25.2.7 ships `swrast_dri.so`
  and `libGLX_indirect.so.0`; the Frame's Qualcomm Mesa has neither. Its
  `gawk` is PIE; the Frame's is `ET_EXEC`, which lxrun refuses.
- **Neither ships FEX**, and there is no Steam package in Holo.

**Recommended base** (reasons and the measurements in
`docs/HOLO_CORE_VS_STEAM_FRAME.md`, "Which base"):

1. The Fedora armroot stays the native client's default. It reaches the
   sign-in window 5-7 s sooner than the Frame root in stage 23, and it is
   built from public, pinned packages.
2. The Frame-derived root is the reference root, for checking the client
   against Valve's own userspace.
3. Holo packages are not a base. They lack GTK 2, carry no Steam pieces,
   are unsigned, and the preview has been frozen since 2026-07-10.

`steamframe-image.py compare` now orders versions with pacman's `vercmp`,
links renamed packages through PROVIDES/REPLACES, accepts an extracted root
and records each database's sha256 (`tests/steamframe_image/compare.sh`).
Re-run it for each new Frame image or Holo snapshot.

## Web research of 2026-09-29

What the web research of 2026-09-29 found on `holo-core-aarch64-preview` is
in section 3 of `STEAM_FRAME_SNAPSHOT_2026-09-29.md`. It is web research
with evidence labels, not measurements on the Mac. Where the clone and the
package index recorded here confirm a fact, that page says so and cites
them.
