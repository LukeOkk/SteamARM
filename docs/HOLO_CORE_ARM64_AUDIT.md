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
  revision was `mash-20251118.3` (tree listed 2026-07-10). Index count: 258
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

Still open: a package-by-package comparison between the image and the Holo
repository (`steamframe-image.py compare <inventory.json> <core.db>
<extra.db>`), and whether a root built from Holo packages alone can carry the
native client. `docs/ARM64_FIRST_MIGRATION.md` takes the image's root as the
target; Holo packages would matter for a root SteamARM could build without
the image.

## Web research of 2026-09-29

What the web research of 2026-09-29 found on `holo-core-aarch64-preview` is
in section 3 of `STEAM_FRAME_SNAPSHOT_2026-09-29.md`. It is web research
with evidence labels, not measurements on the Mac. Where the clone and the
package index recorded here confirm a fact, that page says so and cites
them.
