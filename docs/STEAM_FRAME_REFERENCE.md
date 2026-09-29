# Steam Frame reference snapshot

This file records which upstream Steam Frame snapshot the ARM64 base is built
from, and how sure we are of each fact.

Labels:

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

Update this file whenever a new image is audited. Never overwrite an entry:
add a new dated block.

## Snapshot of 2026-09-28

| fact | value | label |
|---|---|---|
| Latest Beta release | SteamOS 0.4.1 Beta, announced 2026-09-25, device setting System → System Update Channel → Beta | OWNER-PROVIDED (the owner cites Valve's announcement) |
| Latest full recovery image in Valve's public recovery directory | SteamOS 0.3.0, build 20260922.5153644, Steam Frame ARM64, `.img.bz2`, about 3.8 GiB | OWNER-PROVIDED |
| QDL recovery of the same build | `steamframe-oobe-repair-qdl`, 20260922.5153644, 0.3.0, tar.gz / zip | OWNER-PROVIDED |
| Build 20260925.6191901 = SteamOS 0.4.1 | device report | COMMUNITY OBSERVATION |
| Image on the owner's Mac | `~/Downloads/steamframe-oobe-repair-20260922.5153644-0.3.0.img` (decompressed) | MEASURED |
| `btrfs check` of the image | clean | MEASURED |
| `btrfs restore` of the image | stops with `zstd frame incomplete` (three btrfs-progs versions up to v7.1) | MEASURED |
| Image sha256 | *to record:* `scripts/steamframe-image.py info IMG --hash` | pending |
| Partition layout, `os-release`, packages | *to record:* `info`, then `inventory` (docs/STEAM_FRAME_IMAGE.md) | pending |

Build 0.3.0 is the newest *full recovery image* in that directory. It is not
necessarily the newest SteamOS for the Frame; 0.4.1 exists on the Beta
channel.

### What this environment could not check (2026-09-28)

The cloud session that wrote this file cannot reach these sites; its network
policy blocks them:

- `steamdeck-images.steamos.cloud` (recovery directory);
- `steamdeck-packages.steamos.cloud` (package repositories);
- `gitlab.steamos.cloud`;
- `repo.steampowered.com`;
- `store.steampowered.com` and `partner.steamgames.com`.

This is not evidence that anything is missing there. Re-check these on the
Mac:

- the recovery directory, for a newer Steam Frame image;
- the Beta and Stable channels;
- the exact build IDs.

## Snapshot of 2026-09-29

Status (2026-09-29): this block records web research done one day after the
2026-09-28 block. A cloud session did the research. A second pass then tried
to refute every claim: 20 stood, 9 were refuted and are dropped or corrected
here, and 6 were relabelled. Valve's store, help and partner sites, the
steamos.cloud hosts and several press sites were blocked (list below). So no
Valve page body, channel file or image was read directly. Claims that touch
SteamARM were checked against this repository at `4c5efd5`. Nothing here was
run on a Mac. The 2026-09-28 block stays as written; where the two differ,
this block says so.

PROPRIETARY_DO_NOT_REDISTRIBUTE below marks Valve files that SteamARM may use
locally on the user's Mac but never ships.

| fact | value | label |
|---|---|---|
| SteamOS 0.4.1 Beta announcement | Valve page https://store.steampowered.com/news/app/4165890/view/674006995886409325, titled "Steam Frame - SteamOS 0.4.1 (Beta) for Steam Frame". Only the title was seen, in search results. | UPSTREAM DOCUMENTED (URL and title only) |
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
| Newer Frame recovery image on 2026-09-29 | not checked: the host is blocked, and web search surfaced no newer `steamframe-oobe-repair` name | UNKNOWN |
| Recovery checksums | Valve publishes none. Community sha256 of 2026-09-26: `.img.bz2` `3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676`; qdl `.tar.gz` `d3323bfa8efe9ece1954948421cdf5f705e8942eb50c960e2916d935d1b850ab` | COMMUNITY OBSERVATION ([recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md)) |
| Recovery image layout | GPT, 512-byte sectors, first partition at sector 34 (not MiB-aligned): `esp` 256 MiB, `efi-A` 64 MiB, `rootfs-A` 5120 MiB btrfs from sector 655394, `var-A` 256 MiB, `home` 100 MiB | COMMUNITY OBSERVATION ([recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md)) |
| Recovery root `os-release` | `ID=steamos`, `ID_LIKE=arch`, `VERSION_CODENAME=holo`, variant `vr`. The system reports build 20260922.5152327, not the 5153644 of the file name. | COMMUNITY OBSERVATION ([recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md)) |
| Valve documentation of Frame recovery | not established | UNKNOWN |
| Valve FAQ pages on recovery | "SteamOS Recovery and Troubleshooting", https://help.steampowered.com/en/faqs/view/1B71-EDF2-EB6D-2BB3, and a Steam Frame troubleshooting FAQ, https://help.steampowered.com/en/faqs/view/2668-0B6E-79E0-A85B. Seen in search results; content not read. | COMMUNITY REFERENCE |
| Update bundle format | a RAUC bundle (`.raucb`): a squashfs holding `manifest.raucm` and `rootfs.img.caibx`, a casync index. The chunks come from the bundle's `.castr` store and from `vr/chunks.castr`. | VERIFIED IN SOURCE (community code: [deckard-rootfs.yml](https://github.com/NyanActions/absolute-test/blob/main/.github/workflows/deckard-rootfs.yml); [make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh) lines 110-146) |
| 0.4.1 and 0.5.0 bundles are fetchable | community tools fetched them on 2026-09-28 and 2026-09-29 (tracker runs, SteamOS-ARM-Handhelds builds). Not reachable from here. | COMMUNITY OBSERVATION ([tracker releases](https://github.com/NyanActions/absolute-test/releases), [make-steamos-sm8650.sh](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/make-steamos-sm8650.sh)) |
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
| `holo-core-aarch64-preview` source and packages | source at https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview; packages at https://steamdeck-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/; a container image | COMMUNITY REFERENCE (LWN quoting Collabora, read through the [LWN mirror](https://github.com/isdg/feed/blob/main/feeds/sites/lwn/2026-07-17-building-an-arch-linux-aarch64-port-for-holo-core-collabora-0d23cb9f.md)) |
| `holo-core-aarch64-preview` state | frozen at a 2025-11-18 Arch snapshot; unsigned (`SigLevel Optional`); no Steam, SteamVR or Frame UI. Its `core.db` and `extra.db` are byte-identical to `archlinux-deckard/archlinux/mash-20251118.3` (checked 2026-09-23). | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md), [holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build)) |
| `holo-core-aarch64-preview` dates | LWN item: 2026-07-17 (https://lwn.net/Articles/1083392/). The GitLab repository was linked in an r/linux_gaming post on 2026-06-11 (https://www.reddit.com/r/linux_gaming/comments/1u2jjvh/steamos_arm64_build_repo_now_public_on_gitlab). | COMMUNITY REFERENCE ([LWN mirror](https://github.com/isdg/feed/blob/main/feeds/sites/lwn/2026-07-17-building-an-arch-linux-aarch64-port-for-holo-core-collabora-0d23cb9f.md); [RSS-Link-Database-2026](https://github.com/rumca-js/RSS-Link-Database-2026)) |
| Date of Collabora's post | not read; on or before 2026-07-17, since LWN quoted it that day | UNKNOWN |
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
| ARM64 client manifest | https://client-update.steamstatic.com/steam_client_steamdeck_publicbeta_linuxarm64 returned HTTP 200 on 2026-08-07 and resolved `bins_linuxarm64_linuxarm64.zip.0f238017c65e844f71581d1fae8fb42f410e1032`. Current state not checked. | COMMUNITY REFERENCE ([05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md), snapshot of 2026-08-07) |
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
METADATA). No Valve announcement of
0.4.2 or 0.5.0 was seen from here; whether one exists is UNKNOWN.

**Build IDs.** Build 20260925.6191901 = 0.4.1 is better supported than on
2026-09-28: it is in Valve's beta channel file as the tracker mirrors it
(OBSERVED EXTERNAL METADATA). The 2026-09-28 row, labelled COMMUNITY
OBSERVATION, stays as written. The day of the 0.4.1 announcement is UNKNOWN
until Valve's page is read. The press says 2026-09-26, the 2026-09-28 block
says 2026-09-25, and the build ID is dated 20260925. Version 0.3.0 appears
with three build IDs: 20260922.6101926 in the stable channel and on a device,
20260922.5153644 in the recovery file name, and 20260922.5152327 in the
recovery root (rows above).

**Recovery images.** The last check found was made on 2026-09-27. It lists
0.3.0 20260922.5153644 as the newest Frame recovery image (COMMUNITY
OBSERVATION).
The 2026-09-29 state is UNKNOWN. A newer root does not need a newer recovery
image: community tools fetched the 0.4.1 and 0.5.0 update bundles (COMMUNITY
OBSERVATION). The community sha256 values are a cross-check, not a Valve
checksum.

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
URLs behind the image's `[extra]` and `[deckard-arch-hotfixes]`. `inventory`
lists them.

**holo-core-aarch64-preview.** It is a frozen 2025-11-18 Arch snapshot for
aarch64, with no Steam, SteamVR or Frame UI (COMMUNITY OBSERVATION). It is
not the shipped Frame base (versions above). Its dated announcement is the
LWN item of 2026-07-17 (COMMUNITY REFERENCE). The date of Collabora's own
post is UNKNOWN. Details are in `docs/HOLO_CORE_ARM64_AUDIT.md`, "Update
2026-09-29".

**Community handheld ports.** SteamOS-ARM-Handhelds rebuilds Valve's Frame
root for SM8650 and SM8550 handhelds (VERIFIED IN SOURCE). It keeps Valve's
userspace and replaces the boot chain, the kernel and the device glue
(VERIFIED IN SOURCE). Valve's Mesa is device-specific: the project keeps it
only where the GPU is the Frame's Adreno 750 (VERIFIED IN SOURCE). The
project builds in an arm64 Linux VM, Colima on a Mac (VERIFIED IN SOURCE,
[repository](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds)).

**ARM64 client manifest.** The manifest and SteamRT3C rows are a snapshot of
2026-08-07 from a community research document (COMMUNITY REFERENCE). They
were not re-fetched. The client they point to is
PROPRIETARY_DO_NOT_REDISTRIBUTE.

### What this means for SteamARM

- **The Frame root stays the ARM64 base.** HYPOTHESIS: the preview and the
  `archlinux-deckard` snapshots are weaker bases for the native client. They
  hold no Steam client, and they are not what Valve ships (rows above). Use
  them only if SteamARM needs a root it can rebuild and ship. The preview's
  repository is unsigned (COMMUNITY OBSERVATION). A root built from it would
  need a sha256 pinned per package, as `scripts/mkroot-rpm.lock` does for
  Fedora (VERIFIED IN SOURCE).
- **glibc 2.39.** HYPOTHESIS: an aarch64 program taken from SteamARM's
  Fedora 43 root (glibc 2.42, `scripts/mkroot-rpm.lock`), from the preview
  (2.42) or from `mash-20260305` (2.43) may fail to load in the Frame root
  (2.39) when it needs newer glibc symbol versions. FEX-emu is outside this:
  it runs with its own loader and libraries from `/usr/lib/lxrt-emu`
  (VERIFIED IN SOURCE, `scripts/build-fex-host.sh`, `do_emu`). The Vulkan
  shim is linked with `-nostdlib` (VERIFIED IN SOURCE, `Makefile:98`), so it
  should name no glibc version (HYPOTHESIS).
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
  `docs/CURRENT_STEAM_ENVIRONMENT.md` §9. HYPOTHESIS: the runnable ARM64
  client has to be downloaded on the user's Mac from Valve's `linuxarm64`
  client manifest, as `scripts/install-steam.sh` downloads the x86 bootstrap
  today. The client is PROPRIETARY_DO_NOT_REDISTRIBUTE. `inventory` settles
  what the image holds.
- **`VARIANT_ID`.** HYPOTHESIS: the derived root needs `VARIANT_ID=steamdeck`
  instead of `vr`, as SteamOS-ARM-Handhelds sets it (COMMUNITY OBSERVATION,
  rows above). Change it in the derived root, not in the extracted copy.
  HYPOTHESIS: the client should not be started with the Frame's VR options
  (`-deckard`, `-vrgamepadui`).
- **Frame-only services.** The units the port masks are Frame hardware
  services (VERIFIED IN SOURCE, rows above). SteamARM starts no init system:
  `scripts/run-app.sh` starts each program directly
  (`docs/CURRENT_STEAM_ENVIRONMENT.md` §2). So these units never start under
  lxrun. HYPOTHESIS: tag them HARDWARE_SPECIFIC_IGNORE in the audit table.
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
  4 GiB (VERIFIED IN SOURCE; MEASURED in `benchmarks/stage18`).
- **Page size.** The Frame kernel's page size is UNKNOWN. lxrun reports
  16 KiB (`AT_PAGESZ` is `LXRT_HOST_PAGE`, VERIFIED IN SOURCE,
  `runtime/stack.c:113`, `runtime/lxrt.h:19`). It refuses executables and
  `ld.so` aligned below that (`docs/STEAM_FRAME_IMAGE.md`). The "Loading
  under lxrun" section of `inventory` answers whether the root fits.
- **Metadata only.** SteamARM reads community trackers and releases as
  metadata. It takes no Valve file from them
  (PROPRIETARY_DO_NOT_REDISTRIBUTE).
- **Order of trust.** HYPOTHESIS: Valve's channel files and `/vr/` bundles
  belong with item 2 (Valve's images) once they are read directly. The list
  below is unchanged. The project in its item 8, SteamOS-ARM-SM8650, is now
  SteamOS-ARM-Handhelds (VERIFIED IN SOURCE).

Next steps, on the Mac:

1. Fetch the channel files, which are metadata:
   `curl -s https://holo-atomupd.steamos.cloud/meta/holo/steamos/aarch64/vr/beta.json`,
   then `stable`, `bc`, `pc`, `preview` and `main`. Record the build IDs in a
   new dated block.
2. Read Valve's 0.4.1 announcement for its date. Read the FAQs
   1B71-EDF2-EB6D-2BB3 and 2668-0B6E-79E0-A85B for Frame recovery.
3. List https://steamdeck-images.steamos.cloud/recovery/ for a Frame image
   newer than 20260922.5153644.
4. Run `info --hash` and `inventory` on the 0.3.0 image. Record, in a new
   dated block, what the 2026-09-28 block left pending: the sha256, the
   `BUILD_ID` (the community saw 5152327) and the partition layout. Compare
   the downloaded `.img.bz2` with the community sha256 as a cross-check only.
5. Run `compare` on that inventory against the preview's `core.db` and
   `extra.db`, and against `archlinux-deckard/archlinux/mash-20260305`. It
   shows which tree the image is closest to.
6. Rebuild one 0.5.0 `rootfs.img` from its bundle, locally. Run `info` and
   `diagnose` on it. Check whether it has the `zstd frame incomplete`
   extents too.

### What this environment could not check (2026-09-29)

The cloud session behind this block could not reach these hosts:

- `store.steampowered.com` (the 0.4.1 and 0.3.0 news pages): blocked by the
  egress proxy, not by an anti-bot wall;
- `help.steampowered.com` (the FAQ pages);
- `partner.steamgames.com`, read instead through the mirror
  https://github.com/SteamTracking/SteamworksDocumentation (HEAD `11906ca`,
  2026-09-28);
- `steamdeck-images.steamos.cloud` (`/recovery/` and `/vr/`);
- `steamdeck-packages.steamos.cloud`;
- `holo-atomupd.steamos.cloud` (the channel files);
- `gitlab.steamos.cloud` (holo-core-aarch64-preview, frame-developer-tools);
- `www.collabora.com`, `www.gamingonlinux.com`, `mixed-news.com`, `x.com`,
  `tildes.net` and `www.hackster.io`;
- `web.archive.org`, which the fetch tool refuses.

Not attempted: `holo-images.steamos.cloud`, `holo-packages.steamos.cloud`
and `registry.gitlab.steamos.cloud` (HYPOTHESIS: blocked the same way). The
ARM64 client manifest and the SteamRT3C pointer were not re-fetched. GitHub
API access covered only this repository. Other GitHub repositories were read
with git clone, `raw.githubusercontent.com` and fetches of github.com pages.

This is not evidence that anything is missing there. The next steps above
say what to re-check on the Mac.

## Order of trust

1. Valve documentation and Steamworks.
2. Valve's recovery images.
3. Valve's steamos.cloud GitLab and package repositories.
4. Valve's GitHub repositories.
5. Steam Runtime repository and images.
6. Upstream FEX, Proton and gamescope.
7. SteamDB, labelled OBSERVED EXTERNAL METADATA.
8. Community projects such as SteamOS-ARM-SM8650, labelled COMMUNITY REFERENCE.
9. Forums.

## When a newer recovery image appears

1. Keep the old image and its inventory.
2. Extract and inventory the new one next to it
   (`scripts/steamframe-image.py extract` and `inventory --json`).
3. Diff the two package lists. Record what changed:
   - packages added and removed;
   - FEX, Proton, Steam Linux Runtime, gamescope, Mesa/Vulkan and Lepton
     versions;
   - filesystem layout.
4. Only then move the baseline, so that earlier benchmarks can still be
   reproduced.
