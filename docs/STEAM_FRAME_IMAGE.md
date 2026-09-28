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

Whether the Steam Frame image fails for this reason is a HYPOTHESIS until
`diagnose` runs on it. `diagnose` classifies every compressed extent:

- `frame-larger-than-ram`: the case above. Harmless for this tool.
- `frame-incomplete-input` or `short-output`: the data really is damaged or
  truncated. A partial `.img.bz2` download or decompression gives this;
  `info` then marks the partition `TRUNCATED`.

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
    --json ~/SteamARM-roots/logs/steamframe-inventory.json --md docs/STEAM_FRAME_INVENTORY.md
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
the image:

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
- NSS: `libsoftokn3.so` and `libfreeblpriv3.so` sit next to `libnss3.so`, so
  the steamwebhelper FATAL is gone with this base;
- systemd units for Steam, FEX, Lepton and gamescope.

## Licence

The image contains proprietary Valve software: the Steam client, and
possibly Lepton and hardware services. Extract it only from the copy the
owner downloaded, and use it locally. Never ship the extracted tree in the
SteamARM `.dmg`. The release must download or extract it on the user's Mac,
as it does today with Steam.

## Next step: using it as `LXRT_ROOT`

After extraction, the tree is a complete ARM64 userspace. To use it as the
native Steam client's root:

- point `LXRT_ROOT` at it, or bind it where the ARM64 client's root lives
  today;
- add SteamARM's `libvulkan.so.1` shim and its ICD JSON;
- move Qualcomm-only ICDs and services aside in the derived root, not in the
  extracted copy;
- keep `tmp/` as runtime state.

This has to be measured on the Mac with lxrun (steamwebhelper past
BrowserReady, network process alive, Steam UI, Settings → Compatibility).
It cannot be done from a machine without lxrun.
