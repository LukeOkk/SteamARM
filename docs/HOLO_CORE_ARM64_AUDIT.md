# Holo Core ARM64 audit

Status (2026-09-28): not started. The sources could not be reached from the
environment that wrote this file (see below). This page says what the audit
must produce and how to produce it on the Mac.

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
