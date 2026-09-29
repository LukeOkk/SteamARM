# Steam Frame snapshot of 2026-09-29

**Status (2026-09-29): web research record.** This page records what web
research found about Steam Frame releases, channels, recovery images, root
image formats, base package trees and `holo-core-aarch64-preview`. Updated
2026-09-29 after `benchmarks/stage21-native-arm64-client.txt`.

- A cloud session did the research. A second pass then tried to refute every
  claim. For the release and image part, 20 claims stood, 9 were refuted and
  are dropped or corrected here, and 6 were relabelled. For the Holo Core
  part, the corrections are applied too.
- Blocked from the cloud session: Valve's store, help and partner sites, the
  `steamos.cloud` hosts, `www.collabora.com` and several press sites (§5). So
  the research read no Valve page body, channel file or image directly.
- The research ran nothing on a Mac. Every MEASURED row below comes from the
  owner's Mac, as recorded in the files of §1. stage21 ran on the M4 under
  macOS 27.
- Claims that touch SteamARM were checked against this repository at
  `4c5efd5`, and again at `dfab6e2`, after the merge of main that brought
  stage21.
- This page was split out of `docs/STEAM_FRAME_REFERENCE.md` and
  `docs/HOLO_CORE_ARM64_AUDIT.md`. Those two files now hold the Mac's
  measured records, in Spanish.

**Evidence labels:**

- **UPSTREAM DOCUMENTED**: Valve says so in its own documentation.
- **OWNER-PROVIDED**: reported by the project owner, not re-checked here.
- **MEASURED**: observed on the owner's Mac.
- **COMMUNITY OBSERVATION**: seen in public reports, not confirmed by Valve.
- **VERIFIED IN SOURCE**: read in a source tree. The entry gives the repo URL
  and the commit or path.
- **OBSERVED EXTERNAL METADATA**: seen in SteamDB or a similar tracker.
- **COMMUNITY REFERENCE**: seen only in press or in search snippets.
- **HYPOTHESIS**: inferred, not tested.
- **UNKNOWN**: not established.

PROPRIETARY_DO_NOT_REDISTRIBUTE below marks Valve files that SteamARM may use
locally on the user's Mac but never ships.

## 1. The Mac's records this page was checked against

Where the web research and these records meet, the Mac's MEASURED value
decides.

| record | what the owner's Mac measured | file |
|---|---|---|
| recovery image | sha256 of the `.img.bz2` and of the raw image; `bzip2 -tv` ok; the GPT layout and each partition's filesystem. Nothing was booted, mounted writable or run. | `docs/STEAM_FRAME_REFERENCE.md` |
| Valve's recovery index | read on 2026-09-28: the newest full recovery image is 0.3.0, build 20260922.5153644; none is 0.4.1 or later | `docs/STEAM_FRAME_REFERENCE.md` |
| 0.4.1 announcement | read as 2026-09-25 | `docs/STEAM_FRAME_REFERENCE.md` |
| root filesystem | `btrfs check` clean; `btrfs restore` reports `ZSTD frame incomplete` for about 115,000 files under btrfs-progs 6.17.1, 6.6.3 and 7.1; the package inventory is pending | `docs/STEAM_FRAME_ROOTFS_AUDIT.md` |
| `holo-core-aarch64-preview` | a clone at `67f0d559`, its README, and the package index | `docs/HOLO_CORE_ARM64_AUDIT.md` |
| Valve's native arm64 client | fetched from `steam_client_linuxarm64` (version 1788652215); it starts under lxrun, updates itself, loads `steamui.so` and `steamclient.so`, then aborts with `free(): invalid pointer` | `benchmarks/stage21-native-arm64-client.txt` |

## 2. Releases, channels and images

| fact | value | label |
|---|---|---|
| SteamOS 0.4.1 Beta announcement | Valve page https://store.steampowered.com/news/app/4165890/view/674006995886409325, titled "Steam Frame - SteamOS 0.4.1 (Beta) for Steam Frame". The research saw only the title, in search results. | UPSTREAM DOCUMENTED (URL and title only) |
| 0.4.1 Beta date, as read on the owner's Mac | 2026-09-25, from Valve's announcement https://steamcommunity.com/ogg/4165890/announcements/detail/674006995886409326 | UPSTREAM DOCUMENTED, read on the owner's Mac (`docs/STEAM_FRAME_REFERENCE.md`, "Versión y fuente") |
| 0.4.1 Beta date in the press | 2026-09-26 | COMMUNITY REFERENCE ([mixed-news.com](https://mixed-news.com/en/steamos-0-4-1-steam-frame-proximity-sensor-streaming-keyboard/), [gamingonlinux.com](https://www.gamingonlinux.com/2026/09/steamos-0-4-1-beta-for-steam-frame-fixes-some-nuisance-bugs/), search snippets only) |
| SteamOS 0.3.0 Stable announcement | https://store.steampowered.com/news/app/4165890/view/711161056325533925 and https://steamcommunity.com/games/4165890/announcements/detail/711161056325533926; bodies not read | UPSTREAM DOCUMENTED (URLs only) |
| Channel files | one per channel at `holo-atomupd.steamos.cloud/meta/holo/steamos/aarch64/vr/<channel>.json`. Each candidate has an `image` record (release `holo`, product `steamos`, arch `aarch64`, variant `vr`, `buildid`, `version`, `branch`), an `update_path` `vr/<buildid>/<name>.raucb` and a `chunks_store_path` (`.castr`). Valve's endpoint was not reached. | VERIFIED IN SOURCE (community code: [deckard-rootfs.yml](https://github.com/NyanActions/absolute-test/blob/main/.github/workflows/deckard-rootfs.yml)) |
| Channel `stable` | 0.3.0, 20260922.6101926, bundle `deckard-stable-20260922.6101926-0.3.0.raucb` | OBSERVED EXTERNAL METADATA ([tracker releases](https://github.com/NyanActions/absolute-test/releases)) |
| Channel `galileo` | 0.3.0, 20260922.6101926 | OBSERVED EXTERNAL METADATA ([tracker releases](https://github.com/NyanActions/absolute-test/releases)) |
| Channel `rc` | 0.3.0, 20260928.6143559; before that 20260924.6090401 | OBSERVED EXTERNAL METADATA ([tracker releases](https://github.com/NyanActions/absolute-test/releases)) |
| Channel `beta` | 0.4.1, 20260925.6191901, branch `beta`, bundle `holo-images.steamos.cloud/vr/20260925.6191901/deckard-20260925.6191901-0.4.1.raucb` | OBSERVED EXTERNAL METADATA ([tag vr-beta-0.4.1-20260925.6191901](https://github.com/NyanActions/absolute-test/releases/tag/vr-beta-0.4.1-20260925.6191901)) |
| Channel `bc` | 0.4.2, 20260928.6175029, branch `bc`; before that 0.4.1, 20260925.6191901 | OBSERVED EXTERNAL METADATA ([tracker releases](https://github.com/NyanActions/absolute-test/releases)) |
| Channels `pc` and `preview` | 0.4.1, 20260925.6191901, branch `beta`, in the 2026-09-29 run; before that 0.5.0, 20260925.6175226, branch `preview` | OBSERVED EXTERNAL METADATA ([tracker releases](https://github.com/NyanActions/absolute-test/releases), [page 2](https://github.com/NyanActions/absolute-test/releases?page=2)) |
| Channel `main` | 0.5.0, 20260928.6175419, branch `main` (tag `vr-main-0.5.0-20260928.6175419`, 2026-09-29); before that 0.5.0, 20260925.6175226 | OBSERVED EXTERNAL METADATA ([tracker tags](https://github.com/NyanActions/absolute-test/tags)) |
| Device reports | a Frame owner ran BUILD_ID 20260925.6191901 with beta client 1790377368 on 2026-09-28, and reports 0.3.0 as build 20260922.6101926 with kernel 6.18 | COMMUNITY OBSERVATION ([how-the-frame-works.md](https://github.com/saphid/frame-control/blob/main/docs/how-the-frame-works.md), [open-questions.md](https://github.com/saphid/frame-control/blob/main/docs/open-questions.md)) |
| Frame kernel page size | not recorded in any source found (4K, 16K or 64K) | UNKNOWN ([open-questions.md](https://github.com/saphid/frame-control/blob/main/docs/open-questions.md)) |
| Recovery directory | https://steamdeck-images.steamos.cloud/recovery/, a plain directory listing. Valve's SteamOS download page does not link it; that page leads to the Deck repair FAQ https://help.steampowered.com/en/faqs/view/65B4-2AA3-5F37-4227. | COMMUNITY OBSERVATION ([recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md), lines 10-15) |
| Frame recovery files, as listed on 2026-09-27 | only 0.3.0, build 20260922.5153644, all dated 2026-09-22, about 3.8 GiB each: `steamframe-oobe-repair-20260922.5153644-0.3.0.img.bz2` and `.img.zip`, `steamframe-oobe-repair-qdl-20260922.5153644-0.3.0.tar.gz` and `.zip` | COMMUNITY OBSERVATION ([recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md)) |
| Frame recovery files, as read from the owner's Mac on 2026-09-28 | newest full recovery image `steamframe-oobe-repair-20260922.5153644-0.3.0.img.bz2` (3.8 GiB, 2026-09-22 22:57 UTC), with the `.zip` and QDL files; no 0.4.1 or later. This matches the community list above. | MEASURED (`docs/STEAM_FRAME_REFERENCE.md`, "Versión y fuente") |
| Newer Frame recovery image on 2026-09-29 | not checked: the host is blocked from the cloud session, and web search surfaced no newer `steamframe-oobe-repair` name | UNKNOWN |
| Valve checksum for the recovery files | none in the index | MEASURED (`docs/STEAM_FRAME_REFERENCE.md`, "Artefacto local y hashes"); a community report says the same (COMMUNITY OBSERVATION, [recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md)) |
| `.img.bz2` sha256 | `3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676`. The community value of 2026-09-26 matches the Mac's measurement. | MEASURED on the owner's Mac (`docs/STEAM_FRAME_REFERENCE.md`, "Artefacto local y hashes") |
| Raw image sha256 | `081a38c051e99c09db6ae91be994b67ef3347ea71f330d803ab861f0ba8cc654`, 7,516,192,768 bytes | MEASURED (`docs/STEAM_FRAME_REFERENCE.md`, "Artefacto local y hashes") |
| QDL `.tar.gz` sha256 | `d3323bfa8efe9ece1954948421cdf5f705e8942eb50c960e2916d935d1b850ab`; not measured on the Mac | COMMUNITY OBSERVATION ([recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md)) |
| Recovery image layout | GPT, 512-byte sectors, first partition at sector 34 (not MiB-aligned): `esp` 256 MiB FAT32, `efi-A` 64 MiB FAT32, `rootfs-A` 5 GiB btrfs from sector 655394 (4,475,207,680 of 5,368,709,120 bytes used), `var-A` 256 MiB ext4, `home` 100 MiB ext4. The community layout matches the Mac's measurement. | MEASURED on the owner's Mac (`docs/STEAM_FRAME_REFERENCE.md`, "GPT y filesystems") |
| Recovery root `os-release` | `ID=steamos`, `ID_LIKE=arch`, `VERSION_CODENAME=holo`, variant `vr`. The system reports build 20260922.5152327, not the 5153644 of the file name. The Mac has not read it yet: `btrfs restore` leaves many data files empty (`docs/STEAM_FRAME_ROOTFS_AUDIT.md`). | COMMUNITY OBSERVATION ([recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md)) |
| Valve documentation of Frame recovery | not established | UNKNOWN |
| Valve FAQ pages on recovery | "SteamOS Recovery and Troubleshooting", https://help.steampowered.com/en/faqs/view/1B71-EDF2-EB6D-2BB3, and a Steam Frame troubleshooting FAQ, https://help.steampowered.com/en/faqs/view/2668-0B6E-79E0-A85B. Seen in search results; content not read. | COMMUNITY REFERENCE |
| Update bundle format | a RAUC bundle (`.raucb`): a squashfs holding `manifest.raucm` and `rootfs.img.caibx`, a casync index. The chunks come from the bundle's `.castr` store and from `vr/chunks.castr`. | VERIFIED IN SOURCE (community code: [deckard-rootfs.yml](https://github.com/NyanActions/absolute-test/blob/main/.github/workflows/deckard-rootfs.yml); [make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh) lines 110-146) |
| 0.4.1 and 0.5.0 bundles are fetchable | community tools fetched them on 2026-09-28 and 2026-09-29 (tracker runs, SteamOS-ARM-Handhelds builds). Not reachable from the cloud session. | COMMUNITY OBSERVATION ([tracker releases](https://github.com/NyanActions/absolute-test/releases), [make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh)) |
| `holo-images` and `steamdeck-images` | serve the same `/vr/` bundles through a redirect | HYPOTHESIS (a redirect was seen only from `holo-packages` to `steamdeck-packages`: [base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |
| `rootfs.img` from a bundle: partition table | none. Community code loop-mounts the rebuilt file with `mount -o loop,ro`, with no offset and no `-P`. | VERIFIED IN SOURCE (community code: [make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh) line 142; the tracker runs `file rootfs.img` after `desync verify-index`, [deckard-rootfs.yml](https://github.com/NyanActions/absolute-test/blob/main/.github/workflows/deckard-rootfs.yml) line 190) |
| `rootfs.img` from a bundle: filesystem | btrfs | HYPOTHESIS (inferred: the same script's rsync excludes `btrfs.*` xattrs, line 143, and the recovery image's `rootfs-A` is btrfs) |
| `archlinux-deckard/archlinux` | Valve and Collabora's deckard aarch64 Arch tree on `holo-packages.steamos.cloud`. Snapshots `mash-20260202` and `mash-20260305` (`core.db` last modified 2026-09-15), and a hotfix layer at `archlinux-deckard-hotfixes/<branch>/`. | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |
| Where that tree's sources live | frame-developer-tools maps the base repositories to a private `potato/mash/monorepo` and the hotfix layer to a private `deckard/deckardos/holo` | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |
| `holo-packages.steamos.cloud` | 302-redirects to `steamdeck-packages.steamos.cloud` | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |
| Shipped Frame root (0.3.0 on a device; the 0.5.0 bundle) | glibc 2.39, Qt 6.8.0, Plasma 6.2.5, KF6 extra 6.1.0 | COMMUNITY OBSERVATION ([how-the-frame-works.md](https://github.com/saphid/frame-control/blob/main/docs/how-the-frame-works.md) line 57; SteamOS-ARM-Handhelds [apply-overlays.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/apply-overlays.sh) line 662, [install-plasma-extras.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/install-plasma-extras.sh) lines 32 and 192, [build-gamescope-in-rootfs.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/build-gamescope-in-rootfs.sh) line 2) |
| `archlinux-deckard` `mash-20260305` | glibc 2.43+r5, qt6-base 6.10.2 | COMMUNITY OBSERVATION ([manifest.lock](https://github.com/Nova-Deck/os-build/blob/main/rootfs/manifest.lock) lines 92 and 372) |
| `holo-core-aarch64-preview` `mash-20251118.3` | glibc 2.42+r33, qt6-base 6.10.0 | COMMUNITY OBSERVATION (the same `manifest.lock` at commit `7c0f32c^`) |
| "The shipped Frame images are built from `mash-20260305` plus the hotfix layer" | contradicted by the three rows above | HYPOTHESIS (contradicted) |
| The image's package database | `/usr/lib/holo/pacmandb`; `pacman.conf` names `[extra]` and `[deckard-arch-hotfixes]` | COMMUNITY OBSERVATION ([HOW-IT-WORKS.md](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/docs/HOW-IT-WORKS.md)) |
| SteamOS-ARM-Handhelds | https://github.com/hashtagbasit/SteamOS-ARM-Handhelds, the renamed SteamOS-ARM-SM8650 (the old URL redirects). First commit 2026-09-24; HEAD `67614d4` of 2026-09-29. Releases v1.0 (2026-09-24), v1.2 (2026-09-27, SM8650 stable) and v1.3-beta3 (2026-09-29, SM8550). | VERIFIED IN SOURCE ([repository](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds)) |
| Its fork | https://github.com/lavachemist/SteamOS-ARM-Handhelds, "forked from hashtagbasit/SteamOS-ARM-Handhelds". The fork's old SM8650 URL redirects there. HEAD `80e2867`, 10 commits, the last on 2026-09-27. | VERIFIED IN SOURCE ([old URL](https://github.com/lavachemist/SteamOS-ARM-SM8650)) |
| Devices | SM8650: KONKR Pocket FIT, AYANEO Pocket S2 and S2 Pro. SM8550: 12 devices in the README, 13 DTBs in `external-and-mods/kernel-sm8550/soc.env` (one device has a top-dpad variant). | VERIFIED IN SOURCE ([README.md](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/README.md)) |
| Licence | its scripts and overlays: GPL-2.0. `external-and-mods/` keeps upstream licences (kernel GPL-2.0, gamescope BSD-2-Clause, Decky plugins GPL-3.0+). The bundled ARM64 lsfg-vk 2.0 layer: CC BY-NC-ND 4.0. It states that it does not redistribute the SteamOS root or the Steam client; both are downloaded at build time. | VERIFIED IN SOURCE ([LICENSE](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/LICENSE), [CREDITS.md](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/CREDITS.md)) |
| Its base | Valve's 0.5.0 update bundle 20260925.6175226 from `steamdeck-images.steamos.cloud/vr/` (20260921.6090922, also 0.5.0, before v1.2), rebuilt as in the "Update bundle format" row. Not the recovery image. | VERIFIED IN SOURCE ([make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh) lines 110-146, [extract_rootfs.py](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/extract_rootfs.py)) |
| Kept from Valve | the aarch64 userspace, the gamescope session, Plasma 6.2.5 on Qt 6.8, glibc 2.39 | COMMUNITY OBSERVATION ([apply-overlays.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/apply-overlays.sh), [install-plasma-extras.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/install-plasma-extras.sh), [HOW-IT-WORKS.md](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/docs/HOW-IT-WORKS.md)) |
| Valve's Mesa (`deckard-mesa-linux-aarch64`, `deckard-mesa-linux-x86_64` for FEX guests, `deckard-mesa-android-aarch64`) | kept only on SM8650, whose Adreno 750 is the Frame's GPU. SM8550 builds delete it and install the project's own A740 Turnip for aarch64, x86-64/i386 and Android. | VERIFIED IN SOURCE ([apply-overlays.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/apply-overlays.sh) lines 715-740; [make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh) lines 38-41) |
| Replaced | kernel, DTB, firmware, ABL bootloader and initramfs (ROCKNIX, Linux 7.1.2; the kernel needs binder for Lepton, and NTSYNC); gamescope (a patched MSM build); audio UCM; controllers through InputPlumber | VERIFIED IN SOURCE ([apply-overlays.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/apply-overlays.sh), [kernel-common/steamos.config](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/external-and-mods/kernel-common/steamos.config), [kernel-sm8650/soc.env](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/external-and-mods/kernel-sm8650/soc.env)) |
| Masked Frame-only units | `steamvr*`, `deckard-{audio-setup,fan-control,fpga,fpga-resume,led-control,typec-logger,charger,power-monitor,boot-images}`, `set-wifi-mac-address`, `iwd`, `adbd`, `usb-gadget`, `usb-ncm*`, `steamos-boot`, `efi.mount`, `esp.mount`, `systemd-repart` | VERIFIED IN SOURCE ([apply-overlays.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/apply-overlays.sh), about lines 245-272) |
| The image's Steam client | `/usr/lib/steam/steam.tar.zst` is incomplete: "version 0", no package zips, a spinner. The port installs the full ARM64 client (`bins_linuxarm64_linuxarm64.zip` and `steamui_websrc_all.zip`) and rewrites `VARIANT_ID=vr` to `steamdeck`, because the Frame client "reads VARIANT_ID=vr and Gamepad UI then throws". PROPRIETARY_DO_NOT_REDISTRIBUTE. | COMMUNITY OBSERVATION ([install-complete-steam-client.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/install-complete-steam-client.sh), [apply-overlays.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/scripts/apply-overlays.sh) line 294) |
| ARM64 client manifest, steamdeck publicbeta line | https://client-update.steamstatic.com/steam_client_steamdeck_publicbeta_linuxarm64 returned HTTP 200 on 2026-08-07 and resolved `bins_linuxarm64_linuxarm64.zip.0f238017c65e844f71581d1fae8fb42f410e1032`. Current state not checked. | COMMUNITY REFERENCE ([05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md), snapshot of 2026-08-07) |
| ARM64 client manifest, stable line | https://client-update.steamstatic.com/steam_client_linuxarm64, version 1788652215, 35 zips, about 1.0 GB. Entry point `steamrtarm64/steam`, an aarch64 PIE that needs only glibc. PROPRIETARY_DO_NOT_REDISTRIBUTE. | MEASURED on the owner's Mac (`benchmarks/stage21-native-arm64-client.txt`, "Setup") |
| SteamRT3C ARM64 pointer | https://repo.steampowered.com/steamrt3c/images/latest-public-beta.txt gave `3c.0.20260714.251839` on 2026-08-07. Current state not checked. | COMMUNITY REFERENCE (the same document) |
| Runtime for native ARM64 games on the Frame | Steam Linux Runtime 3.0 ARM64 (Sniper). The same pages link `gitlab.steamos.cloud/frame-public/frame-developer-tools`. | UPSTREAM DOCUMENTED (Steamworks docs read through the mirror [SteamworksDocumentation](https://github.com/SteamTracking/SteamworksDocumentation/blob/master/docs/steamhardware/steamframe/loadgames.html), HEAD `11906ca`) |

**Channels.** The channel rows come from a community tracker that copies
Valve's channel files into GitHub releases and tags
(https://github.com/NyanActions/absolute-test). For each channel, its
workflow takes the candidate with the highest build ID (VERIFIED IN SOURCE,
[deckard-rootfs.yml](https://github.com/NyanActions/absolute-test/blob/main/.github/workflows/deckard-rootfs.yml)
lines 70-78). So the 2026-09-29 run shows that 20260925.6191901 became a
candidate in the `pc` and `preview` files (OBSERVED EXTERNAL METADATA). That a
device on those channels would install it is a HYPOTHESIS. Of the channels
that carried 0.5.0, only `main` moved, to 20260928.6175419 (OBSERVED EXTERNAL
METADATA). No Valve announcement of 0.4.2 or 0.5.0 was seen from the cloud
session; whether one exists is UNKNOWN.

**Build IDs.** Build 20260925.6191901 is 0.4.1 in Valve's beta channel file,
as the tracker mirrors it (OBSERVED EXTERNAL METADATA).
`docs/STEAM_FRAME_REFERENCE.md` keeps that build ID as community-observed.
The 0.4.1 announcement has two dates. The owner's Mac read 2026-09-25 on
Valve's page (UPSTREAM DOCUMENTED, read on the Mac). The press says
2026-09-26 (COMMUNITY REFERENCE). The Mac's reading comes from Valve's own
page, so it is the stronger of the two. The build ID is dated 20260925.
Version 0.3.0 appears with three build IDs: 20260922.6101926 in the stable
channel and on a device, 20260922.5153644 in the recovery file name, and
20260922.5152327 in the recovery root (rows above).

**Recovery images.** The owner's Mac read Valve's recovery index on
2026-09-28. The newest full recovery image was 0.3.0, build 20260922.5153644,
and none was 0.4.1 or later (MEASURED). A community check of 2026-09-27
agrees (COMMUNITY OBSERVATION). The 2026-09-29 state is UNKNOWN. A newer
root does not need a newer recovery image: community tools fetched the 0.4.1
and 0.5.0 update bundles (COMMUNITY OBSERVATION). The Mac's sha256 of the
`.img.bz2` equals the community value. Neither is a Valve checksum.

**Root image format.** An update bundle carries the root as a casync index.
SteamOS-ARM-Handhelds unsquashes the bundle and rebuilds `rootfs.img` from
`rootfs.img.caibx` against the chunk stores. It then checks the result
against the sha256 in `manifest.raucm` (VERIFIED IN SOURCE,
[make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh)
lines 110-146). The rebuilt file has no partition table (VERIFIED IN SOURCE,
community code). That it is btrfs is still inferred (HYPOTHESIS). The bundle
is Valve's root filesystem, with its Steam client bootstrap:
PROPRIETARY_DO_NOT_REDISTRIBUTE.

**Base package tree.** Known: `archlinux-deckard/archlinux` is Valve and
Collabora's deckard aarch64 Arch tree, with snapshots up to `mash-20260305`
and a hotfix layer (COMMUNITY OBSERVATION). Contradicted: that the shipped
Frame images 0.3.0 to 0.5.0 are built from it. They ship glibc 2.39 and Qt
6.8.0. `mash-20260305` has glibc 2.43 and qt6-base 6.10.2, and the preview
has glibc 2.42 and qt6-base 6.10.0 (COMMUNITY OBSERVATION). At most,
`archlinux-deckard` is a future base (HYPOTHESIS). UNKNOWN: which published
tree, if any, the shipped images are built from. Also UNKNOWN: the server
URLs behind the image's `[extra]` and `[deckard-arch-hotfixes]`. The image
inventory lists them once it runs; on the Mac it is still pending
(`docs/STEAM_FRAME_ROOTFS_AUDIT.md`).

**holo-core-aarch64-preview.** See §3.

**Community handheld ports.** SteamOS-ARM-Handhelds rebuilds Valve's Frame
root for SM8650 and SM8550 handhelds (VERIFIED IN SOURCE). It keeps Valve's
userspace and replaces the boot chain, the kernel and the device glue
(VERIFIED IN SOURCE). Valve's Mesa is device-specific: the project keeps it
only where the GPU is the Frame's Adreno 750 (VERIFIED IN SOURCE). The
project builds in an arm64 Linux VM, Colima on a Mac (VERIFIED IN SOURCE,
[repository](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds)).

**ARM64 client manifests.** The steamdeck publicbeta manifest and the
SteamRT3C rows are a community snapshot of 2026-08-07 (COMMUNITY REFERENCE).
They were not re-fetched. The stable `steam_client_linuxarm64` manifest was
fetched on the owner's Mac for stage21 (MEASURED). The client they point to
is PROPRIETARY_DO_NOT_REDISTRIBUTE.

## 3. holo-core-aarch64-preview

The cloud session could not reach `gitlab.steamos.cloud`,
`steamdeck-packages.steamos.cloud` or `www.collabora.com`. The owner's Mac
cloned the source repository and read the package index on 2026-09-28
(`docs/HOLO_CORE_ARM64_AUDIT.md`). Where that reading confirms a research
claim, the claim is upgraded below.

### 3.1 Where it lives and what it holds

| fact | value | label |
|---|---|---|
| Source repository | https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview, cloned on the owner's Mac at `67f0d559c82cdc5c94317bad53ae45409720ef59` ("readme: fix name ordering", 2026-06-10) | VERIFIED IN SOURCE (the clone, `docs/HOLO_CORE_ARM64_AUDIT.md`). LWN quoting Collabora names the same URL (COMMUNITY REFERENCE, [LWN mirror](https://github.com/isdg/feed/blob/main/feeds/sites/lwn/2026-07-17-building-an-arch-linux-aarch64-port-for-holo-core-collabora-0d23cb9f.md)). |
| What the source holds | package recipes, not an installed userspace or a full Steam Frame OS | VERIFIED IN SOURCE (the clone, `docs/HOLO_CORE_ARM64_AUDIT.md`) |
| Recipe paths | PKGBUILDs and patches, at paths such as `extra-aarch64/g/gamescope/3.16.17-1` and `extra-aarch64/r/rauc/1.14-1` | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md), [snapshot.pin](https://github.com/Nova-Deck/os-build/blob/main/build/snapshot.pin), [holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build), [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md)) |
| Arch base and status | the Arch state of 2025-11-18, plus ARM64 changes and licence fixes. The README calls it a technology preview, not a stable system. | VERIFIED IN SOURCE (README at `67f0d559`, `docs/HOLO_CORE_ARM64_AUDIT.md`). The community saw the same date in Nova-Deck's history, `gitref mash-squashed_2025-11-18.3` (COMMUNITY OBSERVATION, [base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md) at commit `7c0f32c^`). |
| Arch "state" commit | `97c0a0b47d15` | COMMUNITY REFERENCE (a search snippet attributed to [Collabora's post](https://www.collabora.com/news-and-blog/news-and-events/building-an-arch-linux-aarch64-port-for-holo-core.html), which was not read). `docs/HOLO_CORE_ARM64_AUDIT.md` does not record a commit. |
| Containers | the README offers `base` and `base-devel` images for building | VERIFIED IN SOURCE (`docs/HOLO_CORE_ARM64_AUDIT.md`). The image names `registry.gitlab.steamos.cloud/holo/holo-core-aarch64-preview/base` and `.../base-devel`: COMMUNITY OBSERVATION ([holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build), [base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)). SteamARM does not use them: no VM, no guest kernel, no container runtime (`docs/HOLO_CORE_ARM64_AUDIT.md`). |
| Binary package repository | the Mac read `https://holo-packages.steamos.cloud/holo-core-aarch64-preview/`: ARM64 `core` and `extra`; newest revision `mash-20251118.3` (tree listed 2026-07-10); 258 entries in `core` and 4,312 in `extra`, counting databases and `FILES` metadata | MEASURED (`docs/HOLO_CORE_ARM64_AUDIT.md`, "Fuentes y snapshot") |
| The same repository through `steamdeck-packages` | https://steamdeck-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/ | COMMUNITY REFERENCE (the same LWN item) |
| `holo-packages.steamos.cloud` | 302-redirects to `steamdeck-packages.steamos.cloud` | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |
| What a snapshot holds | `core`, `extra` (with `-debug`), `extra-archive`, `multilib`, `sources/` tarballs, `pacman.conf` and `system.rootfs.zst` (367.1 MiB, 384,971,555 bytes, sha256 `7e3fb88454e1ac633b7488abb72d3ca0cc7d2578a38146fdd7d58b50fcbd60bf`). 4560 package files in all. | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md), [snapshot.pin](https://github.com/Nova-Deck/os-build/blob/main/build/snapshot.pin), [holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build), [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md)) |
| Signatures | the repository is unsigned (`SigLevel Optional`) | COMMUNITY OBSERVATION (same sources). The Mac found no published `.sig` for `btrfs-progs` or for `core.db` (404): MEASURED (`docs/HOLO_CORE_ARM64_AUDIT.md`). |
| Scope | no Steam, SteamVR or Frame UI. Community projects build packages from it with `makepkg` in `base-devel`. | COMMUNITY OBSERVATION ([holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build), [base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md), [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md)) |
| `os-release` | "Holo core Aarch64 port (preview)" | COMMUNITY OBSERVATION ([rootfs/conf/os-release](https://github.com/Nova-Deck/os-build/blob/main/rootfs/conf/os-release); [recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md) line 103) |
| Versions | glibc 2.42 (2.42+r33), qt6-base 6.10.0, Linux 6.17.8, Mesa 25.2.7 | COMMUNITY OBSERVATION ([manifest.lock](https://github.com/Nova-Deck/os-build/blob/main/rootfs/manifest.lock) at commit `7c0f32c^`; [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md), snapshot of 2026-08-07) |
| Frozen | its `mash-20251118.3` `core.db` and `extra.db` are byte-identical to `holo-packages.steamos.cloud/archlinux-deckard/archlinux/mash-20251118.3` (checked 2026-09-23) | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |

### 3.2 What LWN says, and the dates

- LWN published an item on 2026-07-17 at 17:03:50 UTC,
  https://lwn.net/Articles/1083392/ (COMMUNITY REFERENCE, read through the
  [LWN mirror](https://github.com/isdg/feed/blob/main/feeds/sites/lwn/2026-07-17-building-an-arch-linux-aarch64-port-for-holo-core-collabora-0d23cb9f.md)).
- It quotes a Collabora post on building an Arch Linux AArch64 port for Holo
  Core,
  https://www.collabora.com/news-and-blog/news-and-events/building-an-arch-linux-aarch64-port-for-holo-core.html
  (COMMUNITY REFERENCE; the post itself was not read).
- It names the GitLab source, the package repository and a container image
  (COMMUNITY REFERENCE).
- In the quote, Collabora says that "the infrastructure ... is capable of
  building from first principles up until a point-in-time snapshot, the next
  step is to build this into a system which can track Arch Linux"
  (COMMUNITY REFERENCE).
- The date of Collabora's post is UNKNOWN. It is on or before 2026-07-17,
  since LWN quoted it that day. Some press dates it 2026-07-18 (COMMUNITY
  REFERENCE).
- The repository was public earlier. On 2026-06-11 at 00:15:22 UTC, an
  r/linux_gaming post titled "SteamOS arm64 build repo now public on gitlab"
  linked it (COMMUNITY REFERENCE,
  https://www.reddit.com/r/linux_gaming/comments/1u2jjvh/steamos_arm64_build_repo_now_public_on_gitlab,
  seen in https://github.com/rumca-js/RSS-Link-Database-2026).
- The commit the Mac cloned, `67f0d559`, is dated 2026-06-10, one day before
  that post (VERIFIED IN SOURCE, `docs/HOLO_CORE_ARM64_AUDIT.md`).
- Correction: the research first gave 2026-07-17 as the date of Collabora's
  post and of the announcement. 2026-07-17 is LWN's date.

### 3.3 The preview against the shipped Frame root

- A pacman repository tree exists under the name, at
  `holo-core-aarch64-preview/` on Valve's package host (MEASURED: the Mac
  read its index; COMMUNITY REFERENCE, LWN).
- It is not one `[holo-core-aarch64-preview]` repository. It is a directory
  of dated snapshots. `mash-20251118.3` holds its own `core`, `extra` and
  `multilib` repositories, with databases such as `core.db` and `extra.db`
  (COMMUNITY OBSERVATION,
  [base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)).
  The Mac saw `core` and `extra` in the index (MEASURED).
- The same name is also a GitLab repository of package recipes (VERIFIED IN
  SOURCE, the Mac's clone).
- A community port reports that the Frame image's `pacman.conf` names
  `[extra]` and `[deckard-arch-hotfixes]` (COMMUNITY OBSERVATION,
  [HOW-IT-WORKS.md](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/docs/HOW-IT-WORKS.md)).
  The Mac has not read that file yet: the image's data files do not extract
  (`docs/STEAM_FRAME_ROOTFS_AUDIT.md`).
- The shipped Frame root has glibc 2.39 and Qt 6.8.0. That matches neither
  the preview (glibc 2.42, qt6-base 6.10.0) nor `archlinux-deckard`
  `mash-20260305` (glibc 2.43, qt6-base 6.10.2) (COMMUNITY OBSERVATION;
  sources in §2). So the preview is not what the Frame ships (COMMUNITY
  OBSERVATION). Which tree the shipped images come from is UNKNOWN.
- `docs/HOLO_CORE_ARM64_AUDIT.md` draws the same line from the Mac's side:
  the recovery image is a device disk, the source repository is a recipe
  tree, and packages must be compared before any version or file is taken.

## 4. What this means for SteamARM

- **The ARM64 base.** stage21 ran Valve's native client in a Fedora 43
  aarch64 root built by `scripts/mkarmroot.sh`, and took the client from
  Valve's manifest, not from an image (MEASURED,
  `benchmarks/stage21-native-arm64-client.txt`). The appendix of
  `docs/CURRENT_STEAM_ENVIRONMENT.md` tags that root REPLACE_WITH_HOLO.
  HYPOTHESIS: the preview and the `archlinux-deckard` snapshots would not
  change the client's source. They hold no Steam client, and they are not
  what Valve ships (§2, §3). They matter only if SteamARM needs a root it
  can rebuild and ship. The preview's repository is unsigned (COMMUNITY OBSERVATION; no
  `.sig` on the Mac, MEASURED). A root built from it would need a sha256
  pinned per package, as `scripts/mkroot-rpm.lock` and
  `scripts/mkarmroot.lock` do for Fedora (VERIFIED IN SOURCE).
- **glibc 2.39.** HYPOTHESIS: an aarch64 program taken from SteamARM's
  Fedora 43 roots (glibc 2.42: `scripts/mkroot-rpm.lock`,
  `scripts/mkarmroot.lock`), from the preview (2.42) or from `mash-20260305`
  (2.43) may fail to load in the Frame root (2.39) when it needs newer glibc
  symbol versions. Valve's client ran on the Fedora 43 root up to its abort
  (MEASURED, stage21). FEX-emu is outside this: it runs with its own loader
  and libraries from `/usr/lib/lxrt-emu` (VERIFIED IN SOURCE,
  `scripts/build-fex-host.sh`, `do_emu`). The Vulkan shim is linked with
  `-nostdlib` (VERIFIED IN SOURCE, `Makefile:98`), so it should name no
  glibc version (HYPOTHESIS).
- **A newer root without a newer recovery image.** HYPOTHESIS: SteamARM can
  build a 0.4.1 or 0.5.0 root on the Mac from Valve's update bundle, the way
  SteamOS-ARM-Handhelds does on Linux. `scripts/steamframe-image.py` already
  reads an image with no partition table: `partitions()` returns the whole
  image as partition 0 (VERIFIED IN SOURCE). Its test images are bare btrfs
  files (VERIFIED IN SOURCE, `tests/steamframe_image/run.sh:83-93`), and
  `benchmarks/stage19` §1 records that suite passing. So no new mode is
  needed if `rootfs.img` is btrfs (HYPOTHESIS until `info` runs on one).
  SteamOS-ARM-Handhelds does the unsquash and casync steps in an arm64 Linux
  VM (VERIFIED IN SOURCE). Whether those tools run on macOS directly is
  UNKNOWN. The result is Valve's root: use it locally, never ship it
  (PROPRIETARY_DO_NOT_REDISTRIBUTE).
- **The image's Steam client.** The Frame root reportedly holds only an
  incomplete bootstrap, `/usr/lib/steam/steam.tar.zst` (COMMUNITY
  OBSERVATION). That bears on open question 1 of
  `docs/CURRENT_STEAM_ENVIRONMENT.md` §9. The runnable ARM64 client can be
  downloaded on the user's Mac from Valve's `linuxarm64` client manifest:
  stage21 did so, and the client then updated itself (659 MB) (MEASURED). No
  script in the repository does it yet: `scripts/install-steam.sh` still
  fetches only the x86 bootstrap (VERIFIED IN SOURCE). The client is
  PROPRIETARY_DO_NOT_REDISTRIBUTE. `inventory` settles what the image holds.
- **`VARIANT_ID`.** HYPOTHESIS: a root derived from the Frame image needs
  `VARIANT_ID=steamdeck` instead of `vr`, as SteamOS-ARM-Handhelds sets it
  (COMMUNITY OBSERVATION, §2). Change it in the derived root, not in the
  extracted copy. HYPOTHESIS: the client should not be started with the
  Frame's VR options (`-deckard`, `-vrgamepadui`).
- **Frame-only services.** The units the port masks are Frame hardware
  services (VERIFIED IN SOURCE, §2). SteamARM starts no init system:
  `scripts/run-app.sh` starts each program directly
  (`docs/CURRENT_STEAM_ENVIRONMENT.md` §2). So these units never start under
  lxrun. HYPOTHESIS: tag them HARDWARE_SPECIFIC_IGNORE in the audit.
- **Valve's Mesa.** It is device-specific: the port keeps it only where the
  GPU is the Frame's Adreno 750 (VERIFIED IN SOURCE). HYPOTHESIS: none of it
  is usable on the Mac, including the FEX guest copy in
  `/usr/share/guestos/fex-mesa`. Native ARM64 Vulkan goes to SteamARM's
  shim, and x86 games keep SteamARM's x86 rootfs and Vulkan thunks
  (`docs/CURRENT_STEAM_ENVIRONMENT.md` §3 and §5.4). HYPOTHESIS: tag it
  HARDWARE_SPECIFIC_IGNORE.
- **Native ARM64 games.** On the Frame they run under Steam Linux Runtime
  3.0 ARM64 (UPSTREAM DOCUMENTED). HYPOTHESIS: its pressure-vessel emits a
  bwrap plan that `runtime/mounts.c` can interpret. That interpreter was
  checked only against an x86 SLR plan (`benchmarks/stage6-bwrap-plan.txt`).
  It fails with ENOSYS on `--unshare-*`, `--cap-add`, `--cap-drop`, `--uid`,
  `--gid`, `--seccomp` and `--add-seccomp-fd`, and with EINVAL on any option
  it does not know (VERIFIED IN SOURCE, `runtime/mounts.c:456-474`).
- **Proton.** Nothing here changes `benchmarks/stage19` §2. Unmodified
  Valve Proton ARM64 cannot start in an unentitled native arm64 process on
  macOS: Wine needs a mapping at 0x7ffe0000, and macOS reserves the low
  4 GiB (VERIFIED IN SOURCE; MEASURED in `benchmarks/stage18`). A probe
  linked with `-pagezero_size 0x1000` shrank `__PAGEZERO` to 0x4000, but
  `MAP_FIXED` at 0x7ffe0000 still killed it (MEASURED, stage18 follow-up of
  2026-09-28). stage21 keeps Proton ARM64 as a separate problem.
- **Page size.** The Frame kernel's page size is UNKNOWN. lxrun reports
  16 KiB by default and 4 KiB with `LXRT_GUEST_PAGE=4096` (VERIFIED IN
  SOURCE, `runtime/stack.c:21-34, 134`, `runtime/lxrt.h:19`). It now loads
  executables and `ld.so` aligned to 4 KiB, and keeps their protections per
  4 KiB through `subpage.c` (VERIFIED IN SOURCE,
  `runtime/elf.c:147-157, 209-225`). stage21 loaded Valve's 4 KiB-aligned
  client this way (MEASURED). HYPOTHESIS: a 4 KiB-aligned Frame root would
  load the same way. `docs/STEAM_FRAME_IMAGE.md` still describes the older
  16 KiB rule. The "Loading under lxrun" section of `inventory` answers
  whether the root fits.
- **Metadata only.** SteamARM reads community trackers and releases as
  metadata. It takes no Valve file from them
  (PROPRIETARY_DO_NOT_REDISTRIBUTE).

About `holo-core-aarch64-preview`:

- HYPOTHESIS: the preview is a weaker base than the Frame root for the
  client. It is frozen at the 2025-11-18 Arch state (VERIFIED IN SOURCE, the
  Mac's clone) and has no Steam client (COMMUNITY OBSERVATION).
  `docs/HOLO_CORE_ARM64_AUDIT.md` still names Holo Core as the ARM64
  userspace candidate. HYPOTHESIS: as a replacement for the Fedora 43 roots
  it keeps the same glibc level, 2.42.
- HYPOTHESIS: a package comparison of the preview against the Frame image
  will show most versions differ, because the preview is newer for glibc
  and Qt. It then shows how far the published base is from the shipped one.
- HYPOTHESIS: a binary built against the preview (glibc 2.42) may fail to
  load in the Frame root (glibc 2.39) when it needs newer glibc symbol
  versions.
- `docs/HOLO_CORE_ARM64_AUDIT.md` makes the signature point from the Mac's
  side: a stored sha256 and licence are not a verified signature.

Next steps, on the Mac:

1. Fetch the channel files, which are metadata:
   `curl -s https://holo-atomupd.steamos.cloud/meta/holo/steamos/aarch64/vr/beta.json`,
   then `stable`, `bc`, `pc`, `preview` and `main`. Record the build IDs,
   with the date, beside the Mac's other records in
   `docs/STEAM_FRAME_REFERENCE.md`.
2. Done: Valve's 0.4.1 announcement date, read as 2026-09-25
   (`docs/STEAM_FRAME_REFERENCE.md`). Still to read: the FAQs
   1B71-EDF2-EB6D-2BB3 and 2668-0B6E-79E0-A85B for Frame recovery.
3. Done on 2026-09-28: the recovery index held nothing newer than
   20260922.5153644 (`docs/STEAM_FRAME_REFERENCE.md`). List it again before
   the next audit.
4. Done: the `.img.bz2` and raw sha256 and the partition layout
   (`docs/STEAM_FRAME_REFERENCE.md`); the `.img.bz2` hash equals the
   community value. Pending: the `BUILD_ID` (the community saw 5152327) and
   the package inventory. `btrfs restore` leaves many data files empty
   (`docs/STEAM_FRAME_ROOTFS_AUDIT.md`). `scripts/steamframe-image.py`
   reads btrfs without btrfs-progs (`benchmarks/stage19` §1). Whether its
   `extract` or `inventory` was tried on this image is not recorded
   (UNKNOWN); try it.
5. Once the inventory exists, run `compare` against the preview's `core.db`
   and `extra.db`, and against `archlinux-deckard/archlinux/mash-20260305`.
   `compare` takes several databases, local or https (VERIFIED IN SOURCE,
   `scripts/steamframe-image.py`: `read_repo_db` and the `compare` parser).
   It shows which tree the image is closest to.
6. Rebuild one 0.5.0 `rootfs.img` from its bundle, locally. Run `info` and
   `diagnose` on it. Check whether it has the `zstd frame incomplete`
   extents too.
7. Read a preview snapshot's `pacman.conf` for the exact database URLs.
8. Look for the Arch "state" commit `97c0a0b47d15` in the clone at
   `67f0d559`. The README there gives the 2025-11-18 state
   (`docs/HOLO_CORE_ARM64_AUDIT.md`), but no commit is recorded.

## 5. Could not be checked from here

The cloud session could not reach these hosts:

- `store.steampowered.com` (the 0.4.1 and 0.3.0 news pages): blocked by the
  egress proxy, not by an anti-bot wall;
- `help.steampowered.com` (the FAQ pages);
- `partner.steamgames.com`, read instead through the mirror
  https://github.com/SteamTracking/SteamworksDocumentation (HEAD `11906ca`,
  2026-09-28);
- `steamdeck-images.steamos.cloud` (`/recovery/` and `/vr/`);
- `steamdeck-packages.steamos.cloud` (the preview's package repository);
- `holo-atomupd.steamos.cloud` (the channel files);
- `gitlab.steamos.cloud` (holo-core-aarch64-preview and its README,
  frame-developer-tools);
- `www.collabora.com` (the post), `www.gamingonlinux.com`, `mixed-news.com`,
  `x.com`, `tildes.net` and `www.hackster.io`;
- `web.archive.org`, which the fetch tool refuses.

Not attempted: `holo-images.steamos.cloud`, `holo-packages.steamos.cloud`
and `registry.gitlab.steamos.cloud` (HYPOTHESIS: blocked the same way). The
steamdeck publicbeta client manifest and the SteamRT3C pointer were not
re-fetched. GitHub API access covered only this repository. Other GitHub
repositories were read with git clone, `raw.githubusercontent.com` and
fetches of github.com pages.

This is not evidence that anything is missing there. The owner's Mac
reached some of these hosts later: the recovery index, Valve's 0.4.1
announcement, the preview's GitLab repository and its package index (§1).
The next steps above say what to re-check on the Mac.
