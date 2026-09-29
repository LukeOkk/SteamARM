# Steam Frame reference snapshot

This file records which upstream Steam Frame snapshot the ARM64 base is built
from, and how sure we are of each fact.

Labels:

- **UPSTREAM DOCUMENTED**: Valve says so in its own documentation.
- **OWNER-PROVIDED**: reported by the project owner, not re-checked here.
- **MEASURED**: observed on the owner's Mac.
- **COMMUNITY OBSERVATION**: seen in public reports, not confirmed by Valve.

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
