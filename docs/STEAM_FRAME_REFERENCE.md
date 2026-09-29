# Steam Frame: official reference

Snapshot checked on 2026-09-28, to audit Valve's ARM64 stack without booting
the image's kernel.

## Version and source

- Steamworks describes Steam Frame as a Snapdragon 8 Gen 3 ARM64 device
  running an Arch-based SteamOS, and documents Windows x86 games through
  Proton + FEX and Android through Lepton:
  <https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility>
- Valve announced SteamOS 0.4.1 Beta for Steam Frame on 2026-09-25:
  <https://steamcommunity.com/ogg/4165890/announcements/detail/674006995886409326>
- On 2026-09-28 the official recovery index still listed, as the newest full
  recovery, `steamframe-oobe-repair-20260922.5153644-0.3.0.img.bz2` (3.8 GiB,
  2026-09-22 22:57 UTC), with `.zip` and QDL variants:
  <https://steamdeck-images.steamos.cloud/recovery/>
- The index had no 0.4.1 or newer recovery. The identifier
  `20260925.6191901` stays community-observed, not an official snapshot
  confirmed by this check.

## Local copy and hashes

The copy the owner received has the same name as the index entry.
`bzip2 -tv` ended with `ok`. Valve publishes no hash in the index; the
SHA-256 values below are local measurements, not vendor checksums.

| artefact | size | SHA-256 | permissions |
|---|---:|---|---|
| compressed archive in `~/Downloads/` | 3.8 GiB | `3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676` | read-only |
| raw image in `~/SteamARM-roots/reference/` | 7,516,192,768 bytes | `081a38c051e99c09db6ae91be994b67ef3347ea71f330d803ab861f0ba8cc654` | read-only |

`steamframe-image.py info --hash` gave the same size and SHA-256 for the raw
image on 2026-09-29 (MEASURED).

## GPT and filesystems

Raw image: GUID partition table, 512-byte sectors. Inspected with
`gpt -r show`, `hdiutil imageinfo` and an attach with `-readonly -nomount`;
`diskutil info` confirmed `Media Read-Only: Yes`.

| partition | start (sector) | sectors | size | type observed |
|---|---:|---:|---:|---|
| `esp` | 34 | 524,288 | 256 MiB | FAT32 |
| `efi-A` | 524,322 | 131,072 | 64 MiB | FAT32 |
| `rootfs-A` | 655,394 | 10,485,760 | 5 GiB | btrfs, label `rootfs-A`, 4,475,207,680 of 5,368,709,120 bytes used; zstd-compressed, sectorsize 4096 |
| `var-A` | 11,141,154 | 524,288 | 256 MiB | ext4 |
| `home` | 11,665,442 | 204,800 | 100 MiB | ext4 |

The image was not booted, its kernel was not loaded, no filesystem was
mounted writable, and no code from the root was run. `btrfs check`
validated the filesystem and its checksums. `btrfs restore` failed on the
zstd extents; `scripts/steamframe-image.py` read them instead, and on
2026-09-29 extracted and inventoried the whole root (MEASURED). Details:
`docs/STEAM_FRAME_ROOTFS_AUDIT.md`; results: `docs/STEAM_FRAME_INVENTORY.md`.

The root's own `BUILD_ID` is `20260922.5152327`, not the file name's
`5153644` (MEASURED, `etc/os-release`).

## What it is a reference for

Steam Frame is the primary reference for a real Linux ARM64 layout and for
Valve's architecture decisions. Generic Arch ARM64 packages have to be told
apart from firmware, kernel, drivers, device services and proprietary layers
specific to Snapdragon and the Frame (`docs/STEAM_FRAME_INVENTORY.md`,
"Reuse classification"). A file present in the recovery image is not
automatically redistributable, nor needed under ZERO-VM.

## Web research of 2026-09-29

The web research of 2026-09-29 on channels, recovery images, update bundles
and package trees is in `STEAM_FRAME_SNAPSHOT_2026-09-29.md`. It is web
research with evidence labels, not measurements on the Mac. Where it agrees
with the measurements here, that page says so and cites them.
