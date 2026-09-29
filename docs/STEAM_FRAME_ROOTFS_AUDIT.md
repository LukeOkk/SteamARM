# Steam Frame root filesystem audit

Started 2026-09-28. Source: the official Steam Frame recovery image
`20260922.5153644-0.3.0`. The image is only read, never booted: no kernel,
no VM, no `chroot`, and no code from the root is run.

Labels: MEASURED, VERIFIED IN SOURCE, UPSTREAM DOCUMENTED, HYPOTHESIS,
UNKNOWN.

## Extraction: done (2026-09-29)

**First attempt, 2026-09-28 (MEASURED, superseded).** The raw image and the
compressed archive have hashes and read-only permissions recorded in
`docs/STEAM_FRAME_REFERENCE.md`. The GPT names `rootfs-A` as a 5 GiB btrfs,
4,475,207,680 bytes used; the FAT32 and ext4 partitions were identified by
their signatures. The disk was attached with `hdiutil -readonly -nomount`.
`btrfs check --readonly --check-data-csum` under lxrun read the whole
filesystem and reported no metadata or data-checksum errors. `btrfs restore`
(btrfs-progs 6.17.1 aarch64, 6.6.3 x86-64 and 7.1 x86-64) then failed with
"zstd frame incomplete" for about 115,000 files and left them empty, so that
tree could not be used. Logs and tools are outside the repository, in
`~/SteamARM-roots/logs/` and `~/SteamARM-roots/reference/tools/btrfs-progs/`.

**Cause, 2026-09-29 (MEASURED).** `scripts/steamframe-image.py diagnose`
classified all 214,760 zstd extents: 111,347 decode to more bytes than the
extent's `ram_bytes` (the kernel stops there; btrfs-progs calls it "frame
incomplete"), 103,413 decode exactly, 0 are damaged. The image is intact.

**Extraction, 2026-09-29 (MEASURED).** `steamframe-image.py extract` wrote
the default subvolume of `rootfs-A` to a case-sensitive APFS sparsebundle
(`/Volumes/SteamFrameRoot/rootfs`) with exit 0: 180,797 files (8.6 GiB),
12,672 directories, 30,033 symlinks, 4,276 hard links. Ownership, xattrs and
absolute symlink targets are in the tree's `.steamarm-manifest.json`, since
APFS cannot hold all of them.

## Inventory: done

The full results are in `docs/STEAM_FRAME_INVENTORY.md`. Against the list
this audit started with:

| asked for | found (MEASURED unless marked) |
|---|---|
| `/etc/os-release` | SteamOS `0.3.0`, codename `holo`, variant `vr`, `BUILD_ID=20260922.5152327` |
| package DB | `usr/lib/holo/pacmandb/local`, 965 packages with version, arch and licence |
| loader, glibc, ARM64 libraries | glibc 2.39-2, gcc-libs 15.1.1; 7,280 aarch64 ELF files; glibc and ld.so are 64 KiB-aligned PIE |
| Steam ARM64 and x86/i386 payloads | `steam` 1.0.0.85-2 (bootstrap script) and `deckard-steam-rel` with `usr/lib/steam/steam.tar.zst` (700 MB): `steamrtarm64/` (native client, CEF), `linuxarm64/`, `androidarm64/`, and the x86 `steamrt64/`, `ubuntu12_32/`, `ubuntu12_64/`; 894 x86-64 and 509 i386 ELF files in the root, most in `usr/share/guestos/fex-mesa` |
| FEX | none: no FEX binaries, no `binfmt.d` rules. Steam installs its own FEX as a tool (MEASURED with the x86 client, stage 15); that the Frame gets it the same way is a HYPOTHESIS |
| Proton ARM64/x86, Wine ARM64EC, tool manifests, Steam Linux Runtime ARM64 | none in the root. Steam installs Proton (ARM64) and SLR 4.0 Arm64 as tools (MEASURED, stage 15) |
| gamescope | 3.16.28-1 |
| Vulkan loader and ICDs | vulkan-icd-loader 1.4.309; the only ICD is Qualcomm's freedreno (hardware-specific); KosmicKrisp: absent (it is a macOS driver) |
| Mesa | `deckard-mesa-*` 26.3.0_devel, aarch64 and an x86_64 guest Mesa for FEX |
| reusable Arch ARM64 packages versus device files | classified in `docs/STEAM_FRAME_INVENTORY.md` ("Reuse classification") |
| kernel page size | 4 KiB (`boot/Image` header) |

Each file that might be reused still needs its package, licence,
architecture, Linux dependency and ZERO-VM destination before it is used.
Being present in the recovery image does not make a file redistributable or
necessary. The pacman mirrors in the image are private per-device URLs and
are never published.

## Next

- Derive a root from the extraction and run the native client on it:
  `docs/ARM64_FIRST_MIGRATION.md`, steps 6-8. Not done: it writes under
  `~/SteamARM-roots` and needs the owner's go-ahead.
- Compare the package set with `holo-core-aarch64-preview`
  (`docs/HOLO_CORE_ARM64_AUDIT.md`): `steamframe-image.py compare`. Not run.
