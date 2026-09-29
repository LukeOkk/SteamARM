# Holo Core ARM64 audit

Status (2026-09-28): not started. The sources could not be reached from the
environment that wrote this file (see below). This page says what the audit
must produce and how to produce it on the Mac.

Web research on 2026-09-29 found where the preview lives and what it holds:
see "Update 2026-09-29" below.

## What `holo-core-aarch64-preview` is

HYPOTHESIS: the name has the shape of a pacman repository in SteamOS' "Holo"
distribution layer, like the Steam Deck's `holo-*` repositories. If so, it
is not a source tree.

Checked from here:

- `raw.githubusercontent.com/ValveSoftware/holo-core-aarch64-preview` returns
  404 on both `main` and `master`.
- `steamdeck-packages.steamos.cloud` and `gitlab.steamos.cloud` are blocked
  by this environment's network policy.

Neither result says the repository does not exist.

The Steam Frame image settles it. Its `etc/pacman.conf` names the
repositories the device installs from, with their server URLs.
`steamframe-image.py inventory` lists them.

## Update 2026-09-29

Status (2026-09-29): this section records what web research found about
`holo-core-aarch64-preview`. A cloud session did the research. A second pass
then tried to refute every claim, and its corrections are applied here. The
session could not reach `gitlab.steamos.cloud`,
`steamdeck-packages.steamos.cloud` or `www.collabora.com`. So nothing here
was read from Valve's or Collabora's own pages. The labels are the ones
defined at the top of `docs/STEAM_FRAME_REFERENCE.md`. The audit table below
is still empty.

### Where it lives and what it holds

| fact | value | label |
|---|---|---|
| Source repository | https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview | COMMUNITY REFERENCE (LWN quoting Collabora, read through the [LWN mirror](https://github.com/isdg/feed/blob/main/feeds/sites/lwn/2026-07-17-building-an-arch-linux-aarch64-port-for-holo-core-collabora-0d23cb9f.md)) |
| Binary package repository | https://steamdeck-packages.steamos.cloud/holo-core-aarch64-preview/mash-20251118.3/ | COMMUNITY REFERENCE (the same LWN item) |
| Container image | LWN says there is one | COMMUNITY REFERENCE (the same LWN item) |
| `holo-packages.steamos.cloud` | 302-redirects to `steamdeck-packages.steamos.cloud` | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |
| What the source holds | PKGBUILDs and patches, at paths such as `extra-aarch64/g/gamescope/3.16.17-1` and `extra-aarch64/r/rauc/1.14-1` | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md), [snapshot.pin](https://github.com/Nova-Deck/os-build/blob/main/build/snapshot.pin), [holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build), [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md)) |
| What a snapshot holds | `core`, `extra` (with `-debug`), `extra-archive`, `multilib`, `sources/` tarballs, `pacman.conf` and `system.rootfs.zst` (367.1 MiB, 384,971,555 bytes, sha256 `7e3fb88454e1ac633b7488abb72d3ca0cc7d2578a38146fdd7d58b50fcbd60bf`). 4560 package files in all. The repository is unsigned (`SigLevel Optional`). | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md), [snapshot.pin](https://github.com/Nova-Deck/os-build/blob/main/build/snapshot.pin), [holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build), [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md)) |
| Containers, scope, building | images `registry.gitlab.steamos.cloud/holo/holo-core-aarch64-preview/base` and `.../base-devel`. No Steam, SteamVR or Frame UI. Community projects build packages from it with `makepkg` in `base-devel`. | COMMUNITY OBSERVATION ([holo-core-aarch64-preview-build](https://github.com/SolberLight/holo-core-aarch64-preview-build), [base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md), [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md)) |
| `os-release` | "Holo core Aarch64 port (preview)" | COMMUNITY OBSERVATION ([rootfs/conf/os-release](https://github.com/Nova-Deck/os-build/blob/main/rootfs/conf/os-release); [recovery-and-images.md](https://github.com/saphid/frame-control/blob/main/docs/recovery-and-images.md) line 103) |
| Versions | glibc 2.42 (2.42+r33), qt6-base 6.10.0, Linux 6.17.8, Mesa 25.2.7 | COMMUNITY OBSERVATION ([manifest.lock](https://github.com/Nova-Deck/os-build/blob/main/rootfs/manifest.lock) at commit `7c0f32c^`; [05-current-arm64-steam-research.md](https://github.com/xXJSONDeruloXx/steam-android-runtime-research/blob/main/docs/00-start-here/05-current-arm64-steam-research.md), snapshot of 2026-08-07) |
| Arch snapshot date | 2025-11-18 (Nova-Deck history: `gitref mash-squashed_2025-11-18.3`) | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md) at commit `7c0f32c^`) |
| Arch "state" commit | `97c0a0b47d15` | COMMUNITY REFERENCE (a search snippet attributed to [Collabora's post](https://www.collabora.com/news-and-blog/news-and-events/building-an-arch-linux-aarch64-port-for-holo-core.html), which was not read) |
| Frozen | its `mash-20251118.3` `core.db` and `extra.db` are byte-identical to `holo-packages.steamos.cloud/archlinux-deckard/archlinux/mash-20251118.3` (checked 2026-09-23) | COMMUNITY OBSERVATION ([base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)) |

### What LWN says, and the dates

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
- Correction: the research first gave 2026-07-17 as the date of Collabora's
  post and of the announcement. 2026-07-17 is LWN's date.

### The HYPOTHESIS above, against this evidence

The HYPOTHESIS paragraph above stays as written. The new evidence supports
its first sentence in part and contradicts its second.

- Supported in part: a pacman repository tree exists under that name, at
  `steamdeck-packages.steamos.cloud/holo-core-aarch64-preview/` (COMMUNITY
  REFERENCE, LWN; COMMUNITY OBSERVATION,
  [base-pin.md](https://github.com/Nova-Deck/os-build/blob/main/docs/base-pin.md)).
- But it is not one `[holo-core-aarch64-preview]` repository. It is a
  directory that holds a dated snapshot, `mash-20251118.3`, and the snapshot
  holds its own `core`, `extra` and `multilib` repositories (COMMUNITY
  OBSERVATION, same source). The
  databases are `core.db`, `extra.db` and so on, inside a snapshot
  (COMMUNITY OBSERVATION, same source). So the
  `holo-core-aarch64-preview.db` URL in the procedure below probably does
  not exist (HYPOTHESIS).
- Contradicted: "it is not a source tree". The same name is also a GitLab
  repository of PKGBUILDs and patches (COMMUNITY REFERENCE for the location,
  COMMUNITY OBSERVATION for the contents).
- Not tested: the likeness to the Steam Deck's `holo-*` repositories.

The section above says the image's `etc/pacman.conf` settles which
repositories the device uses. A community port reports that it names
`[extra]` and `[deckard-arch-hotfixes]` (COMMUNITY OBSERVATION,
[HOW-IT-WORKS.md](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/docs/HOW-IT-WORKS.md)).
The shipped Frame root has glibc 2.39 and Qt 6.8.0. That matches neither
the preview (glibc 2.42, qt6-base 6.10.0) nor `archlinux-deckard`
`mash-20260305` (glibc 2.43, qt6-base 6.10.2) (COMMUNITY OBSERVATION; sources
in `docs/STEAM_FRAME_REFERENCE.md`, snapshot of 2026-09-29). So the preview
is not what the Frame ships (COMMUNITY OBSERVATION). Which tree the shipped
images come from is UNKNOWN.

### What this means for SteamARM

- HYPOTHESIS: the preview is a weaker ARM64 base than the Frame root. It is
  frozen at 2025-11-18 and has no Steam client. It is worth using only if
  SteamARM needs a root it can rebuild and ship.
- HYPOTHESIS: most rows of the audit table will differ in version, because
  the preview is newer than the shipped image for glibc and Qt. The
  comparison then shows how far the published base is from the shipped one.
- HYPOTHESIS: a binary built against the preview (glibc 2.42) may fail to
  load in the Frame root (glibc 2.39) when it needs newer glibc symbol
  versions.
- The preview is unsigned (COMMUNITY OBSERVATION). If SteamARM ever builds a
  root from it, pin a sha256 per package, as `scripts/mkroot-rpm.lock` does
  for Fedora (VERIFIED IN SOURCE).

Next steps, on the Mac:

1. Read a snapshot's `pacman.conf` for the exact database URLs.
2. Run `compare` on the image inventory with the preview's `core.db` and
   `extra.db`. `compare` takes several databases, local or https (VERIFIED
   IN SOURCE, `scripts/steamframe-image.py`: `read_repo_db` and the
   `compare` parser).
3. Run it again with `archlinux-deckard/archlinux/mash-20260305`, to see
   which tree is closer to the image.
4. Read the GitLab README for the Arch "state" commit and the dates.

### Could not be checked from here

- `gitlab.steamos.cloud` (the source repository and its README): blocked.
- `steamdeck-packages.steamos.cloud` (the package repository): blocked.
- `www.collabora.com` (the post) and `www.hackster.io`: blocked.
- `holo-packages.steamos.cloud` and `registry.gitlab.steamos.cloud`: not
  attempted.

This is not evidence that anything is missing there.

## Procedure (on the Mac)

```sh
python3 scripts/steamframe-image.py inventory /Volumes/SteamFrameRoot/rootfs \
    --json ~/SteamARM-roots/logs/steamframe-inventory.json --md docs/STEAM_FRAME_INVENTORY.md
# "pacman repositories" lists every [repo] with its server URL; the database
# is <server>/<repo>.db, for example:
python3 scripts/steamframe-image.py compare ~/SteamARM-roots/logs/steamframe-inventory.json \
    'https://<server from the inventory>/holo-core-aarch64-preview.db' --md docs/HOLO_CORE_VS_STEAM_FRAME.md
```

`compare` reads the repository database (a compressed tar of `desc` files) and
sets the image's installed package versions beside it. It shows for each
package whether the versions match, differ, or exist on only one side.

## Table this audit must fill (§11.1D)

| component | Holo Core preview version | Steam Frame recovery version | architecture | open source? | relevant to SteamARM? | current SteamARM equivalent | action |
|---|---|---|---|---|---|---|---|

- The version columns come from `compare`.
- The architecture and licence come from the package database (`%ARCH%`,
  `%LICENSE%`).
- The last three columns come from `docs/CURRENT_STEAM_ENVIRONMENT.md`: the
  Fedora 43 aarch64 root, FEX's x86 rootfs and Valve's Steam client.

Classify each component with one of: REFERENCE_ONLY, REUSE_SOURCE,
REUSE_BINARY_IF_LICENSE_ALLOWS, REIMPLEMENT_DARWIN_ADAPTER, FEX_REUSE,
STEAM_RUNTIME_OWNED, HARDWARE_SPECIFIC_IGNORE, PROPRIETARY_DO_NOT_REDISTRIBUTE,
UNKNOWN.

Neither source replaces the other:

- the preview repository is the open, buildable baseline;
- the recovery image is what Valve actually ships.

When they differ, find out what each package is for before picking one.
