# The Steam Frame image as the ARM64 base

The plan is to take the ARM64 userspace for the native Steam client, and
later for everything else, from Valve's own Steam Frame recovery image, not
from packages collected one missing library at a time. The image is only a
source of files. Its kernel is never booted, and no VM is involved.

`scripts/steamframe-image.py` reads the image on the Mac directly: GPT,
btrfs, zstd/zlib/lzo extents. It needs no mount and no btrfs-progs, and it
opens the image read-only.

## Why not `btrfs restore`

On the owner's Mac, `btrfs restore` (btrfs-progs, three versions up to v7.1)
stopped on `steamframe-oobe-repair-20260922.5153644-0.3.0.img` with
`zstd frame incomplete`. `btrfs check` found nothing wrong (MEASURED
2026-09-28).

That message comes from btrfs-progs' `decompress_zstd()`. It requires the
zstd frame to end inside a buffer the size of the extent's `ram_bytes`. The
Linux kernel does not: it stops once it has `ram_bytes` and ignores the rest
of the frame. `tests/steamframe_image/run.sh` builds that case: three extents
whose frames decode to 16 KiB more than `ram_bytes`.

- btrfs-progs 7.1 `restore` fails with exactly this message.
- `steamframe-image.py extract` recovers every byte.

MEASURED on synthetic images, Linux, 2026-09-28: 12/12 checks pass.

`diagnose` classifies every compressed extent:

- `frame-larger-than-ram`: the case above. Harmless for this tool.
- `frame-incomplete-input` or `short-output`: the data really is damaged or
  truncated. A partial `.img.bz2` download or decompression gives this;
  `info` then marks the partition `TRUNCATED`.

On the Steam Frame image this is now MEASURED (2026-09-29, on the Mac): of
214,760 zstd extents, 111,347 are `frame-larger-than-ram`, 103,413 are `ok`
and 0 are damaged. The btrfs-progs failure is the case above, not a damaged
image. `extract` then finished with exit 0: 180,797 files (8.6 GiB), 12,672
directories, 30,033 symlinks, 4,276 hard links. Results:
`docs/STEAM_FRAME_INVENTORY.md`.

## Steps (on the Mac)

```sh
IMG=~/Downloads/steamframe-oobe-repair-20260922.5153644-0.3.0.img
chmod a-w "$IMG"                                   # keep the reference read-only
pip3 install zstandard                             # or: brew install zstd
python3 scripts/steamframe-image.py info "$IMG" --hash   # partitions, btrfs, subvolumes, sha256
python3 scripts/steamframe-image.py diagnose "$IMG" --json ~/SteamARM-roots/logs/steamframe-diagnose.json

# A case-sensitive volume keeps names that differ only in case (Linux trees have them).
hdiutil create -size 40g -type SPARSEBUNDLE -fs 'Case-sensitive APFS' \
    -volname SteamFrameRoot ~/SteamARM-roots/steamframe-root.sparsebundle
hdiutil attach ~/SteamARM-roots/steamframe-root.sparsebundle
python3 scripts/steamframe-image.py extract "$IMG" /Volumes/SteamFrameRoot/rootfs
python3 scripts/steamframe-image.py inventory /Volumes/SteamFrameRoot/rootfs \
    --json ~/SteamARM-roots/logs/steamframe-inventory.json --md ~/SteamARM-roots/logs/steamframe-inventory.md
```

Notes:

- **Several btrfs partitions (A/B roots).** `extract` takes the one named
  like a root, or the largest. Pass `--part N` to choose another.
- **Default subvolume.** `extract` takes the filesystem's default subvolume,
  as the kernel mounts it. Pass `--subvol-id` to choose another.
- **Resuming.** `--resume` skips files already extracted with the same size
  and mtime, so an interrupted run can continue.
- **Absolute symlinks** become relative. lxrun resolves guest paths on the
  host, so an absolute link would point into the Mac's own `/usr`;
  `scripts/mkroot-rpm.sh` does the same. The original targets are kept in
  `.steamarm-manifest.json`.
- **What macOS cannot hold in files.** Ownership, xattrs (for example
  `security.capability`), device nodes and fifos also go to the manifest.

## What the inventory answers

The inventory answers the questions `STEAM_FRAME_ROOTFS_AUDIT.md` asks of
the image. The answers for the 0.3.0 image are in
`docs/STEAM_FRAME_INVENTORY.md`. It redacts private per-device pacman
mirror URLs by itself (`tests/steamframe_image/redact.sh`); check any other
output for them before sharing it.

- `os-release` and `BUILD_ID`;
- the pacman database (`usr/lib/holo/pacmandb`), with version, arch and
  license per package;
- ELF files by machine, and their `PT_INTERP`;
- every non-AArch64 ELF (x86 remnants, or an x86 rootfs for FEX);
- FEX binaries and the `binfmt.d` rules that dispatch x86 to it;
- the Steam client, steamwebhelper/CEF, Proton, Steam Linux Runtime and
  gamescope;
- Vulkan ICDs, where Qualcomm's Turnip/freedreno must be replaced by
  SteamARM's MoltenVK shim;
- NSS: whether `libsoftokn3.so` and `libfreeblpriv3.so` sit next to
  `libnss3.so`. In the 0.3.0 image they do (MEASURED). That the
  steamwebhelper NSS FATAL does not occur with this base stays a HYPOTHESIS
  until the client runs on it;
- systemd units for Steam, FEX, Lepton and gamescope;
- what `lxrun` can load:
  - aarch64 `ET_EXEC` files, which `runtime/elf.c` refuses;
  - executables and `ld.so` whose segments are aligned below the 16 KiB host
    page. Since stage 21 `runtime/elf.c` loads them through
    `runtime/subpage.c` when they are 4 KiB-aligned, and refuses anything
    below 4 KiB. glibc also needs `LXRT_GUEST_PAGE=4096` to dlopen 4 KiB-aligned
    libraries (MEASURED, `benchmarks/stage21-native-arm64-client.txt`);
  - libraries in the same case, which go through `runtime/subpage.c`;
  - the page size the image's kernel was built for, from its config or its
    `Image` header: 4 KiB for the 0.3.0 image (MEASURED).

## Licence

The image contains proprietary Valve software: the Steam client, and
possibly Lepton and hardware services. Extract it only from the copy the
owner downloaded, and use it locally. Never ship the extracted tree in the
SteamARM `.dmg`. The release must download or extract it on the user's Mac,
as it does today with Steam.

## Next step: using it as `LXRT_ROOT`

After extraction, the tree is a complete ARM64 userspace. To use it as the
native Steam client's root:

- keep the extracted copy untouched, and derive the root from it as an APFS
  clone on the same volume, linked where the launcher looks for the ARM64
  base (`/tmp/lxrt-arm64root`, which is also the `LXRT_ROOT` default of
  `scripts/run-native.sh`). `scripts/mkframeroot.sh` does this (stage 23):

  ```sh
  scripts/mkframeroot.sh      # /Volumes/SteamFrameRoot/rootfs -> .../arm64root,
                              # linked as ~/SteamARM-roots/arm64root
  scripts/env-links.sh        # links /tmp/lxrt-arm64root while the volume is attached
  ```

  It adds only `etc/resolv.conf`, the host's `etc/localtime` and
  `.lxrt-guest-env` (indirect GLX: the image's Mesa has no software driver),
  keeps `tmp/` (the client's home) across rebuilds, and can seed a client
  home with `--home-from DIR`. Valve's native arm64 client reaches its
  login window on it (MEASURED, `benchmarks/stage23-frame-root.txt`).
- SteamARM's Vulkan shim (`build/libvulkan.so.1`) as the root's
  `libvulkan.so.1`: the image has only the freedreno ICD. The image's
  `vulkaninfo` lists the Apple M4 through the shim on `LD_LIBRARY_PATH`
  (MEASURED, stage 23 V1). It is not needed for the login window and not
  installed yet;
- moving Qualcomm-only ICDs and services aside: not needed so far, not done.

The image has no FEX (no FEX binaries, no `binfmt.d` rules; MEASURED), so
x86 payloads keep using SteamARM's own FEX. The order of the remaining
steps is in `docs/ARM64_FIRST_MIGRATION.md`.
