# Steam Frame compatibility tools

**Status (2026-09-29): research record.** This page records what is known
about how Valve's native arm64 Steam client offers compatibility tools:
Proton ARM64, FEX, Steam Linux Runtime 4.0 Arm64 and Lepton. The project
owner's goal is to see a Proton ARM64 entry in Steam > Settings >
Compatibility while that client runs under lxrun. Updated 2026-09-29 after
`benchmarks/stage21-native-arm64-client.txt`.

- The content comes from web research by a cloud session on 2026-09-29.
- A second, adversarial pass re-checked every claim. It confirmed 29, refuted
  12 and relabelled 4. The refuted claims are dropped or corrected here, and
  the relabels are applied.
- A few Proton source facts were read again while writing this page. They are
  marked "read while writing" and were not part of the adversarial pass.
- The research ran nothing on a Mac. The owner's Mac then ran Valve's native
  arm64 client under lxrun (MEASURED on the M4 under macOS 27,
  `benchmarks/stage21-native-arm64-client.txt`). It starts, updates itself
  from the `steam_client_linuxarm64` manifest (version 1788652215), and
  loads `steamui.so` and `steamclient.so`. Its main process then aborts with
  `free(): invalid pointer`, before any window. So it has not reached
  Settings > Compatibility, and no ARM64 Proton entry has been seen under
  SteamARM.
- Proton ARM64 cannot start unmodified on macOS (`benchmarks/stage18-settings-audio-controllers.txt`,
  `benchmarks/stage19-steamframe-base-and-arm64-limits.txt` §2). See §7.
- Blocked or unreachable from the cloud session: partner.steamgames.com,
  steamdb.info, store.steampowered.com (search-result titles only),
  gitlab.steamos.cloud, repo.steampowered.com, client-update.steamstatic.com,
  blog.drakulix.de, web.archive.org, lobste.rs, wiki.fex-emu.com,
  gamingonlinux.com, techspot.com, digitalcitizen.life, hwbusters.com, vr.org,
  interfacinglinux.com, and the GitHub REST API for ValveSoftware/Proton and
  SteamTracking. Details are in §8.

**Evidence labels:**

| label | meaning |
|---|---|
| MEASURED | observed by SteamARM on a Mac or a CI runner, as recorded in this repository's `benchmarks/` or `docs/` |
| VERIFIED IN SOURCE | read in a source tree. The repo URL and commit or path are given. Paths without a URL are in this repository. Their line numbers were re-checked at `dfab6e2`, the merge of main that brought stage21. |
| UPSTREAM DOCUMENTED | Valve or upstream documentation says so |
| OBSERVED EXTERNAL METADATA | SteamDB and similar trackers, including SteamTracking's mirror of Valve's client manifests |
| COMMUNITY OBSERVATION | reported by community projects or users, not confirmed by Valve |
| COMMUNITY REFERENCE | seen only in press or in search snippets |
| HYPOTHESIS | inferred, not tested |
| UNKNOWN | not established |

**PROPRIETARY_DO_NOT_REDISTRIBUTE** marks Valve's binaries: the Steam client,
its client runtime package, Valve's Proton, FEX, Steam Linux Runtime and
Lepton depot builds, and the contents of the Steam Frame image. SteamARM may
read them on the owner's Mac, or let Steam download them there. It must never
ship them. This holds where the source is open too: SteamARM may build from
source, but not ship Valve's builds.

## 1. The tools and their app IDs

| tool | app ID | depot | branch or version | where seen | label |
|---|---|---|---|---|---|
| Proton 11.0 (ARM64) | 4628740 | 4628741, installed as `steamapps/common/Proton 11.0 (ARM64)` | Branch `proton_11.0`, tag `proton-11.0-2c`, commit 5b89db94 (2026-09-04). On a Frame the version file read `1788505046 proton-11.0-2c-arm64` (2026-09-28). | Title: https://store.steampowered.com/app/4628740/Proton_110_ARM64/ and https://steamdb.info/app/4628740/depots/ (search-result titles only). Depot: https://github.com/huntergdavis/steamclienttermux/blob/main/config/steam-arm64-compatibilitytools.vdf.in. Branch: https://github.com/ValveSoftware/Proton/tree/proton_11.0. Version file: https://github.com/saphid/frame-control/blob/main/docs/evidence/mods-2026-09-28.md | Title: UPSTREAM DOCUMENTED (store title) and OBSERVED EXTERNAL METADATA (SteamDB title). Depot: COMMUNITY OBSERVATION. Branch: VERIFIED IN SOURCE. Version file: COMMUNITY OBSERVATION. |
| Proton Experimental (ARM64) | 4427310, internal tool name `proton-experimental-arm64` | 4427311, manifest 6971993485335684515, buildid 25502785. These numbers come from a community re-upload dated 2026-09-26. Valve's publish date for that build is UNKNOWN. | Branch `experimental_11.0`, HEAD 22f5f69 (2026-09-24). Which source commit buildid 25502785 was built from is UNKNOWN. | IDs: https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-steam-compat. Depot: https://github.com/The412Banner/winlator-contents/releases/tag/steam-proton-arm64-25502785. Branch: https://github.com/ValveSoftware/Proton/tree/experimental_11.0 | IDs and depot: COMMUNITY OBSERVATION. Branch: VERIFIED IN SOURCE. |
| FEX (Valve's FEX compatibility tool, installdir `FEX-Emu`) | 3127680 (common.type Tool, oslist linux, section_type ownersonly) | 3127681: the guest rootfs, 0 bytes on public, 3,945,426,115 bytes on beta and gamma. 3127682: FEX plus thunks, 24,805,692 bytes on public (buildid 24136555). | Public FEX-2607-76-g37265b1 and beta FEX-2604-97-ga04b024 on 2026-08-05. The current depot build is UNKNOWN. The newest upstream tag is FEX-2609.1. | Depot data: https://github.com/Nova-Deck/os-build/blob/main/.claude/plans/fex-compat-tool.plan.md (a steamcmd dump). Upstream: https://github.com/FEX-Emu/FEX/tree/main/Source/Steam | IDs, depots, versions: COMMUNITY OBSERVATION (2026-08-05). Upstream tag: VERIFIED IN SOURCE. The SteamDB title was not fetched. |
| Steam Linux Runtime 4.0 - Arm64 (`SteamLinuxRuntime_4-arm64`) | 4185400 | 4185401 | UNKNOWN (SteamDB blocked). The x86 counterpart is 4183110. | App ID: https://github.com/ValveSoftware/Proton/blob/experimental_11.0/toolmanifest_arm64.vdf. Title: https://steamdb.info/app/4185400/depots/ (search-result title). Depot: https://github.com/huntergdavis/steamclienttermux/blob/main/config/steam-arm64-compatibilitytools.vdf.in | App ID: VERIFIED IN SOURCE. Title: OBSERVED EXTERNAL METADATA. Depot: COMMUNITY OBSERVATION. |
| Steam Linux Runtime 3.0 (sniper) arm64, legacy | 3810310 | UNKNOWN | UNKNOWN. The first ARM64 Proton manifest required it from 2025-11-12 to 2026-01-26. | https://github.com/ValveSoftware/Proton/commit/b4d640e5 ; https://github.com/ValveSoftware/Proton/commit/0a7e25b6def85d56707099578990c8e61cf4fa5e | App ID and dates: VERIFIED IN SOURCE. That 3810310 is the sniper arm64 runtime: HYPOTHESIS. |
| Lepton | 3029110 | UNKNOWN. About 1 GB. Installs to `steamapps/common/Lepton/lepton`. | Branches public, previous and bleeding-edge. Aliases `lepton-stable` and `fauxdroid`. Source commit 6135b53 (2026-09-16). The Frame runs Android 11 (API 30). | Title: https://steamdb.info/app/3029110/info/ (search-result title). Aliases, branches, size: https://vr.org/articles/valve-lepton-fex-steam-frame-translation-layers-2026 (snippet). Source: https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b. Frame: https://github.com/saphid/frame-control/blob/main/docs/how-the-frame-works.md | Title: OBSERVED EXTERNAL METADATA. Aliases, branches, size: COMMUNITY REFERENCE. Source: VERIFIED IN SOURCE (a third-party mirror). Android version and install path: COMMUNITY OBSERVATION. |
| Lepton Development | 3056000 | UNKNOWN | UNKNOWN | https://store.steampowered.com/app/3056000/Lepton_Development/ (search-result title) | OBSERVED EXTERNAL METADATA |
| Proton 11.0 | 4628710 | 4628711 | Current title "Proton 11.0". During the beta it was "Proton 11.0 (Beta)", alias `proton-11.0-beta`, with an ARM64 branch that carried FEX-2604. | Current title: https://steamdb.info/app/4628710/info/ (search-result title). Beta name, alias, branch, depot: https://vr.org/articles/valve-lepton-fex-steam-frame-translation-layers-2026 (snippet) | Current title: OBSERVED EXTERNAL METADATA. Beta name, alias, ARM64 branch and depot: COMMUNITY REFERENCE. |

Every depot in this table is a Valve build: PROPRIETARY_DO_NOT_REDISTRIBUTE.
The community re-upload of Proton Experimental (ARM64) cited above is used
here only as a source of depot numbers. SteamARM does not use it.

Other identifiers:

| item | value | label | source |
|---|---|---|---|
| Proton Experimental (x86) | app 1493710 | OBSERVED EXTERNAL METADATA (search-result title) | https://steamdb.info/app/1493710/ |
| Steam Linux Runtime 4.0 (x86) | app 4183110 | VERIFIED IN SOURCE | https://github.com/ValveSoftware/Proton/blob/proton_11.0/toolmanifest_x86_64.vdf |
| Steam Linux Runtime 3.0 sniper (x86) | app 1628350, required by x86 Proton until commit 0a7e25b6 | VERIFIED IN SOURCE | https://github.com/ValveSoftware/Proton/commit/0a7e25b6def85d56707099578990c8e61cf4fa5e |
| Steam Frame | app 4165890 | OBSERVED EXTERNAL METADATA | https://store.steampowered.com/news/app/4165890/view/674006995886409325 |
| "Steam Frame ARM64 Compat List" | app 3043620 | OBSERVED EXTERNAL METADATA | https://steamdb.info/app/3043620/info/ |
| Directory `SteamLinuxRuntime_sniper-arm64` | seen installed on x86_64 hosts | COMMUNITY OBSERVATION | https://github.com/ValveSoftware/steam-for-linux/issues/12936 |

Steam Linux Runtime 4.0 - Arm64, in more detail:

- `SteamLinuxRuntime_4-arm64` contains `_v2-entry-point` and
  `pressure-vessel/bin/pressure-vessel-wrap`. Its README reportedly says
  32-bit ARM is unsupported. Its container needs unprivileged user
  namespaces; without them the launch is dropped silently. COMMUNITY
  OBSERVATION: https://github.com/Daaboulex/steam-arm64-nix/blob/main/guest-run.sh ;
  https://github.com/Daaboulex/steam-arm64-nix/blob/main/README.md ;
  https://github.com/saphid/frame-control/blob/main/docs/evidence/mods-2026-09-28.md
- steam-runtime-tools (pressure-vessel) is under MIT, LGPL-2.1-or-later,
  Apache-2.0 and Zlib. VERIFIED IN SOURCE, in the third-party mirror of
  §2.6. The depot's own terms are UNKNOWN.

Lepton, in more detail:

- Source (VERIFIED IN SOURCE, a third-party mirror of
  gitlab.steamos.cloud/frame-public/lepton at 6135b53,
  https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b):
  - it runs Android in rootless podman containers;
  - it maps `/dev/dri/renderD128` as `/dev/kgsl-3d0`;
  - it has `image/` and `image-14/` builders;
  - the compat tool is MIT; the AOSP image uses AOSP, Waydroid (Apache-2.0)
    and Anbox/Halium/Hybris patches, and the README says GPL-3.0;
  - "Most users should use Lepton provided by the Steam Client itself."
- On the Frame, Lepton is Android 11 (API 30), 64-bit ARM only. It runs as
  podman containers named `lepton-steamlaunch-<id>`. A muvm guest without
  binder cannot run it. COMMUNITY OBSERVATION:
  https://github.com/saphid/frame-control/blob/main/docs/how-the-frame-works.md ;
  https://github.com/Daaboulex/steam-arm64-nix/blob/main/guest-run.sh

What Valve says about the Frame (UPSTREAM DOCUMENTED, search-engine snippets
only; the pages are blocked):

- Steam Frame runs a Snapdragon 8 Gen 3 (Arm64) on Arch-based SteamOS.
- The compatibility layers are Proton (Windows), FEX (x86, 32- and 64-bit)
  and Lepton (Android, run as a container).
- Most developers will run Windows x86 through Proton plus FEX.
- Steamworks SDK 1.63 and later ships Linux ARM64 and Android ARM64
  libraries.
- Sources: https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility ;
  https://partner.steamgames.com/doc/steamhardware/proton
- The upload guidance says: "Runtime: Select Android if its an APK, Steam
  Linux Runtime 3.0 ARM64 (Sniper) if you made a Linux ARM64 binary."
  https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames
- UNKNOWN: that Valve recommends Android 10 or Linux Arm64 as native targets.
  No snippet reproduced the "Android 10" part.

Dates (COMMUNITY REFERENCE, press seen through snippets):

- FEX and Lepton appeared publicly on Steam around 2026-08-02 to 08-04.
- Valve open-sourced Lepton around 2026-09-17 to 09-20. The public
  repository's first commit, "Release Lepton v3.0.0", is dated 2026-09-11
  (VERIFIED IN SOURCE, in the mirror of §1).
- Steam Frame launched 2026-09-14. Other reports say 2026-09-18.
- Sources: https://www.techspot.com/news/113337-valve-publicly-releases-lepton-fex-compatibility-tools-power.html ;
  https://www.gamingonlinux.com/2026/09/lepton-from-valve-to-run-android-games-on-linux-is-now-open-source/ ;
  https://vr.org/steam-frame

## 2. How a tool is declared

### 2.1 Two ways in

A tool reaches the client in one of two ways:

- as a Steam app whose depot carries a `toolmanifest.vdf`;
- as a directory in `compatibilitytools.d` with a `compatibilitytool.vdf`.

How Proton's build produces both (VERIFIED IN SOURCE, read while writing,
https://github.com/ValveSoftware/Proton/blob/experimental_11.0/Makefile.in
at 22f5f69):

- `toolmanifest_$(TARGET_ARCH).vdf` is copied into the dist as
  `toolmanifest.vdf` (lines 1422-1430). An arm64 build therefore ships
  `toolmanifest_arm64.vdf` under that name.
- `compatibilitytool.vdf` is generated from `compatibilitytool.vdf.template`.
  The internal tool name defaults to `$(BUILD_NAME)-proton` (lines 1434-1441).
- `make deploy`, the SteamPipe target, excludes `compatibilitytool.vdf`
  (line 1505).
- `make install` copies the whole dist, with `compatibilitytool.vdf`, into
  `~/.steam/root/compatibilitytools.d/$(BUILD_NAME)` (lines 1582-1588).

HYPOTHESIS: Valve's own depot tools are listed from the app's metadata, since
`make deploy` leaves `compatibilitytool.vdf` out. Local builds are listed
from the file.

### 2.2 toolmanifest.vdf

| key | Proton arm64 | Proton x86_64 | FEX | Lepton |
|---|---|---|---|---|
| `version` | 2 | 2 | 2 | not recorded |
| `commandline` | `/proton %verb%` | `/proton %verb%` | `/fex-compat-tool %verb% --` | `/lepton %verb% --` |
| `require_tool_appid` | 4185400 | 4183110 | not recorded | not recorded |
| `use_sessions` | 1 | 1 | not recorded | not recorded |
| `use_tool_subprocess_reaper` | not present | not present | 1 | 1 |
| `filter_exclusive_priority` | not present | not present | 2 | not recorded |
| `compatmanager_layer_name` | `proton` | `proton` | `fex` | `lepton` |
| label | VERIFIED IN SOURCE | VERIFIED IN SOURCE | VERIFIED IN SOURCE | VERIFIED IN SOURCE (a third-party mirror) |
| source | https://github.com/ValveSoftware/Proton/blob/experimental_11.0/toolmanifest_arm64.vdf | https://github.com/ValveSoftware/Proton/blob/proton_11.0/toolmanifest_x86_64.vdf | https://github.com/FEX-Emu/FEX/tree/main/Source/Steam | https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b |

- The Proton values are the same on `experimental_11.0` and `proton_11.0`.
  "not present" and the `version` key were read while writing: the Proton
  files hold only the five keys shown.
- "not recorded" means the research did not report the key. It does not mean
  the file lacks it.

### 2.3 require_tool_appid: the runtime chain

| step | what | label | source |
|---|---|---|---|
| 2025-11-12 | The ARM64 Proton manifest is added (commit b4d640e5). It requires app 3810310. | VERIFIED IN SOURCE | https://github.com/ValveSoftware/Proton/commit/b4d640e5 |
| 2026-01-26 | Commit 0a7e25b6 "Switch to steamrt4": ARM64 now requires 4185400, x86 moves from 1628350 to 4183110. | VERIFIED IN SOURCE | https://github.com/ValveSoftware/Proton/commit/0a7e25b6def85d56707099578990c8e61cf4fa5e |
| community builds | GE-Proton11-5-aarch64 and Proton-CachyOS 11.0 arm64 declare `require_tool_appid` 4185400. The UbuntuAsahi script strips that requirement from GE-Proton with sed. | COMMUNITY OBSERVATION | https://github.com/UbuntuAsahi/steam-arm64/blob/master/steam-arm64.sh ; https://github.com/Nova-Deck/os-build/blob/main/docs/windows-games-fex.md |
| Switchdeck's "Proton 11.0-1 Armv8.0 (FEX)" | Its runtime requirement is UNKNOWN. | UNKNOWN | https://github.com/SildurFX/Switchdeck/blob/main/install-steam.sh |
| DroidDeck's own tool | Its toolmanifest has no `require_tool_appid` (see §6). | COMMUNITY OBSERVATION | https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-steam-compat |

### 2.4 compatibilitytool.vdf, from_oslist and to_oslist

- Proton's template declares, per tool, `install_path` (`.` when the tool is
  a subdirectory of `compatibilitytools.d`), `display_name`,
  `from_oslist "windows"` and `to_oslist "linux"`. It has no architecture
  key. VERIFIED IN SOURCE, read while writing:
  https://github.com/ValveSoftware/Proton/blob/experimental_11.0/compatibilitytool.vdf.template
- UNKNOWN: how the arm64 client tells an ARM64 tool from an x86 one in
  `compatibilitytools.d`, given that the template carries no architecture.
- Lepton's app is reported with `from_oslist android` and
  `to_oslist linux`. COMMUNITY REFERENCE (vr.org snippet):
  https://vr.org/articles/valve-lepton-fex-steam-frame-translation-layers-2026
- A local `compatibilitytool.vdf` that declared Proton 11.0 (ARM64) (4628740)
  and Steam Linux Runtime 4.0 - Arm64 (4185400) made the arm64 client's
  `compat_log` register both. COMMUNITY OBSERVATION:
  https://github.com/huntergdavis/steamclienttermux/blob/main/config/steam-arm64-compatibilitytools.vdf.in ;
  https://github.com/huntergdavis/steamclienttermux/blob/main/docs/TECHNICAL_LOG.md

### 2.5 Tool names seen

| name | what it is | label | source |
|---|---|---|---|
| `proton` / `fex` / `lepton` | `compatmanager_layer_name` values | VERIFIED IN SOURCE | §2.2 |
| `proton-experimental-arm64` | the client's internal name for 4427310 | COMMUNITY OBSERVATION | https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-steam-compat |
| `proton-11.0-beta` | alias of 4628710 during the beta | COMMUNITY REFERENCE | https://vr.org/articles/valve-lepton-fex-steam-frame-translation-layers-2026 |
| `lepton-stable`, `fauxdroid` | aliases of 3029110 | COMMUNITY REFERENCE | same vr.org snippet |
| `steamrt4-arm64` | alias for 4185400 in a community-authored descriptor | COMMUNITY OBSERVATION | https://github.com/huntergdavis/steamclienttermux/blob/main/config/steam-arm64-compatibilitytools.vdf.in |
| `proton-cachyos-11.0-arm64`, `proton-ge-arm64` | community ARM64 builds, as registered | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/blob/main/docs/windows-games-fex.md ; https://github.com/UbuntuAsahi/steam-arm64/blob/master/steam-arm64.sh |
| `bannerlator-proton-arm64` | DroidDeck's own wrapper tool | COMMUNITY OBSERVATION | https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-steam-compat |
| `proton-stable-arm64`, `proton_11`, `proton_experimental` | names in the Steam-ARM installer's compat map | COMMUNITY OBSERVATION | https://github.com/Scrumpper/Steam-ARM/blob/main/steam-arm-install.sh |
| `proton_experimental` | the x86 client's choice under SteamARM: `compat_log.txt` "Mapping AppID 0 to tool "proton_experimental"" | MEASURED | `benchmarks/stage15-steam-proton-path.txt` (problem 1) |

### 2.6 The emulator layer

Pressure-vessel's emulator interface. VERIFIED IN SOURCE, in a third-party
GitHub mirror of steam-runtime-tools at commit 328a4a4 (2026-03-22), because
gitlab.steamos.cloud was unreachable:
https://github.com/llyyr/steam-runtime-tools/blob/328a4a48b90c31e3974f0e94a42084ff27ae757c/docs/steam-runtime-emulator.json.5.md ;
https://github.com/llyyr/steam-runtime-tools/blob/328a4a48b90c31e3974f0e94a42084ff27ae757c/docs/steam-compat-tool-interface.md ;
https://github.com/llyyr/steam-runtime-tools/blob/328a4a48b90c31e3974f0e94a42084ff27ae757c/pressure-vessel/wrap-context.c

- Steam sets `STEAM_COMPAT_EMULATOR` to the path of an `emulator.json` when
  it runs x86 code on aarch64 inside a container runtime. Pressure-vessel also
  accepts `PRESSURE_VESSEL_EMULATOR`.
- The emulator's directory is shared into the container automatically.
- `server_argv` must print `READY=1\n` on a pipe, then close it.
- `STEAM_COMPAT_GRAPHICS_PROVIDER` names a `graphics_provider.json`. Its root
  supplies the x86 `ld.so`, glibc and drivers.

FEX's Steam recipe. VERIFIED IN SOURCE:
https://github.com/FEX-Emu/FEX/tree/main/Source/Steam ;
https://github.com/FEX-Emu/FEX/blob/main/.github/workflows/steamrt4.yml ;
https://github.com/FEX-Emu/FEX/blob/main/Source/Steam/ServerManager.cpp ;
https://github.com/FEX-Emu/FEX/blob/main/Source/Common/Config.cpp

- `emulator.json` (`emulator_v0`): argv `./usr/bin/FEX` with
  `FEX_PORTABLE=1`; `container_environment` `FEX_ROOTFS=''`; `main_argv`
  `./FEXCompatTool`; `server_argv` `./usr/bin/FEXServerManager`.
- It emulates `x86_64-linux-gnu` and `i386-linux-gnu`, and requires
  `aarch64-linux-gnu` with `libc.so.6` and `libstdc++.so.6`.
- `ConfigTemplate.json` enables DiskCache and X87ReducedPrecision.
- The python `fex-compat-tool` is not in the upstream repo. Only its name is.
- The CI workflow "steamrt4 build" builds with `-DBUILD_STEAM_SUPPORT=True
  -DBUILD_THUNKS=True` in `registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk/arm64:4.0.20251117.183306`.
  It uploads the artifact `steamrt4_steampipe_depot`.
- FEXServerManager writes `READY=1` to pressure-vessel.
- Under `FEX_STEAM_SUPPORT`, config and data go to
  `$STEAM_COMPAT_DATA_PATH/fex-emu/`, and the cache to
  `$STEAM_COMPAT_SHADER_PATH/fex-emu/`.
- FEX is MIT-licensed.

Valve's `fex-compat-tool` (the python wrapper in depot 3127682). COMMUNITY
OBSERVATION:
https://github.com/Nova-Deck/os-build/blob/main/.claude/plans/fex-compat-tool.plan.md ;
https://github.com/Scrumpper/Steam-ARM/blob/main/steam-arm-install.sh

- It sets `FEX_PORTABLE=1`, so it ignores any system FEX config.
- It takes the x86 guest from the directory of
  `STEAM_COMPAT_GRAPHICS_PROVIDER`, or else from
  `/usr/share/guestos/fex-mesa`.
- It sets `STEAM_COMPAT_MACHINE_ARCHITECTURE=aarch64-linux-gnu` and
  `STEAM_COMPAT_EMULATOR=<emulator.json>`.
- It defaults `tu_override_uncached_as_cache_coherent=true`.
- It deletes `LD_PRELOAD` and logs to `/tmp/fex-compat-tool-<pid>.log`.
- The depot holds `usr/bin/{FEX,FEXServer,FEXServerManager,FEXBash,FEXGetConfig,FEXOfflineCompiler,FEXpidof}`,
  32- and 64-bit host and guest thunks, `ThunksDB.json`,
  `ConfigTemplate.json`, `emulator.json`, `fex-compat-tool`,
  `FEXCompatTool` and `unshare.py`. It has no FEXInterpreter and no
  `binfmt.d`.
- The licence of the python wrapper is UNKNOWN.

Proton ARM64 does not use this layer. It carries its own FEX as Windows DLLs
(§4.1).

## 3. How the client decides what it offers in Settings > Compatibility

No source describes the client's selection logic. What is known is its
behaviour.

| observation | label | source |
|---|---|---|
| The x86 client under SteamARM's FEX, with FEX's CPUID leaves visible, called the host arm64 and installed Proton (ARM64), Steam Linux Runtime 4.0 - Arm64 and Valve's FEX. | MEASURED | `benchmarks/stage15-steam-proton-path.txt` (problem 1) |
| A later test found that the x86 client, with the same leaves visible, registers no ARM64 Proton. The record keeps both results. | MEASURED | `docs/CURRENT_STEAM_ENVIRONMENT.md` §1; commit `06cbc41` |
| ARM64 Proton builds cannot be used in x86 Steam running via FEX. | UPSTREAM DOCUMENTED | https://github.com/ValveSoftware/Proton/blob/experimental_11.0/README.md |
| Around 2026-08-03 the native client would not, by default, download SLR 4.0 Arm64 or Proton ARM64. It tried to launch an x86_64 runtime instead, which broke containers even with global binfmt. The same post uses the publicbeta arm64 channel, and says the runtime references `emulator.json` and `graphics-provider.json`. | COMMUNITY REFERENCE (snippets of a postmarketOS blog post) | https://blog.drakulix.de/taming-the-steam-arm64-client-on-pmos/ |
| On 2026-08-05 a normal account's client never registered FEX (3127680), and `app_info_print` returned `{}`. An account gate was suggested, third-hand. | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/blob/main/.claude/plans/fex-compat-tool.plan.md |
| A later client update (seen on hardware 2026-08-18) registers 4185400, installs it on demand, and wraps any tool that asks for it in the SLR4 entry point. Proton 11.0 (ARM64) became the default for Windows titles. | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/blob/main/docs/windows-games-fex.md |
| On the steamdeck_stable line, client build 1785799196 (built 2026-08-03, tested 2026-08-31) lacked FEX 3127680, the Proton 11 arm64 offer and SLR 4185400. Build 1788291500 (built 2026-09-01, tested 2026-09-02) had all three. | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/blob/main/build/steam-seed/STEAM_SEED.pin |
| 4185400 installs lazily, when a Proton title first launches. | COMMUNITY OBSERVATION | same `STEAM_SEED.pin` |
| The client downloads Valve's FEX into `steamapps/common/FEX-Emu` when a title first starts. | COMMUNITY OBSERVATION | https://github.com/Scrumpper/Steam-ARM/blob/main/CHANGELOG.md |
| `steam://install/4427310` installs Proton Experimental (ARM64). | COMMUNITY OBSERVATION | https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-steam-compat |
| A local `compatibilitytool.vdf` is enough for the client to register 4628740 and 4185400. | COMMUNITY OBSERVATION | §2.4 |
| Steam can wrongly install `-arm64` runtime depots (`SteamLinuxRuntime_sniper-arm64`) on x86_64 hosts. Reported 2026-02-21, still open. | COMMUNITY OBSERVATION | https://github.com/ValveSoftware/steam-for-linux/issues/12936 |
| Lepton offers itself on x86_64 although only an ARM64 build exists. | COMMUNITY OBSERVATION | https://github.com/valvesoftware/steam-for-linux/issues/13634 |

**Device gating.**

- The SteamOS Devkit client offers the `SteamLinuxRuntime_4-arm64` and
  Lepton runtime aliases only for a device that reports itself as Deckard.
  This is `RUNTIME_ALIASES` in `devkit_client/__init__.py`. It concerns the
  Devkit client, not the Steam client's Compatibility list. COMMUNITY
  OBSERVATION: https://github.com/saphid/frame-control/blob/main/docs/sideloading.md
- The client flag `-deckard` asserts Steam Frame identity and tracks a gated
  arm64 line named by a branch-key hash. COMMUNITY OBSERVATION:
  https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-session
- `-steamos3` makes the client manage itself as SteamOS. It opts into
  steamdeck_stable and rewrites `package/beta`. When that conflicts with
  `-clientbeta` it restarts forever (rc=42). COMMUNITY OBSERVATION: same
  file, and https://github.com/Nova-Deck/os-build/blob/main/build/steam-seed/STEAM_SEED.pin
- Whether the Steam client's own Compatibility list depends on device
  identity: UNKNOWN. No source shows such a gate.
- HYPOTHESIS: the offer depends mostly on the client build and channel. The
  steamdeck_stable builds above differ in it, and community devices that are
  not Frames report the ARM64 Protons. Which flags those clients ran with is
  not recorded here.
- Account: 3127680 is `section_type ownersonly` (COMMUNITY OBSERVATION, §1).
  Whether an account entitlement matters for 4628740 or 4427310: UNKNOWN.

## 4. Proton's ARM64 build

### 4.1 How it is built

- "ARM64 Builds: You need an ARM64 build machine and pass
  `--target-arch=arm64` to configure.sh. It's not possible to use the
  resulting builds in x86 Steam running via FEX." UPSTREAM DOCUMENTED:
  https://github.com/ValveSoftware/Proton/blob/experimental_11.0/README.md
- ARCHS for an arm64 build: `i386-windows x86_64-windows` first, then
  `aarch64-unix aarch64-windows arm64ec-windows` are appended. The i386 and
  x86_64 PE targets are built too, for the WoW64 and x86 PE side. aarch64 and
  arm64ec are built with `-march=armv8.2-a -mtune=cortex-x3`. VERIFIED IN
  SOURCE: https://github.com/ValveSoftware/Proton/blob/experimental_11.0/Makefile.in
  (flags at lines 56-57, ARCHS at lines 72-83)
- The build image is `registry.gitlab.steamos.cloud/proton/steamrt4/sdk/arm64-llvm`.
  VERIFIED IN SOURCE, same file. Versions per branch are in §4.3.
- `wine` and `wineserver` go to `files/bin-arm64`. The prefix is
  `files/share/default_pfx_arm64`. VERIFIED IN SOURCE, same file.
- FEX is built with `BUILD_STEAM_SUPPORT=True` as aarch64 and arm64ec PE
  DLLs in `lib/wine/aarch64-windows`, plus a unixlib in
  `lib/wine/aarch64-unix`. `FEX_Config.json` is installed as
  `share/fex-emu/Config.json`. VERIFIED IN SOURCE, same file.
- x86 PE code runs through the FEX DLLs `libarm64ecfex.dll` and
  `libwow64fex.dll`. VERIFIED IN SOURCE:
  https://github.com/ValveSoftware/Proton/tree/experimental_11.0. SteamARM's
  record names the same DLLs (`benchmarks/stage18-settings-audio-controllers.txt`,
  "Proton ARM64 on macOS").

### 4.2 The proton script on aarch64

VERIFIED IN SOURCE:
https://github.com/ValveSoftware/Proton/blob/experimental_11.0/proton

- The script switches to ARM64 when all three hold:
  - `PROTON_USE_ARM64` is set (default 1);
  - `platform.machine() == 'aarch64'`;
  - `files/bin-arm64` exists.
- It then uses `host_pe_arch` `aarch64-windows` and `default_pfx_arm64`.
- Only the FEX configuration is gated on aarch64:
  - `FEX_APP_CONFIG_LOCATION` is set to the dist's `share/fex-emu/`;
  - a per-app `FEX_APP_CONFIG` is written from `STEAM_FEX_TSOENABLED`,
    `STEAM_FEX_MULTIBLOCK` and `STEAM_COMPAT_FEX_CONFIG`.
- The Steam ffmpeg libraries are added to the library path on every host
  architecture: `ubuntu12_64/video`, `ubuntu12_32/video` and
  `steamrtarm64/video` together (line 1273, "add Steam ffmpeg libraries to
  path").

### 4.3 FEX, Wine Mono and Xalia pins

| ref | commit, date | FEX | Wine Mono | Xalia | SDK image version |
|---|---|---|---|---|---|
| `proton_11.0`, tag `proton-11.0-2c` | 5b89db94, 2026-09-04 | FEX-2607 (1cc4b93e) | 11.2.0, as the x86 tarball | 0.4.9 | 4.0.20260331.220802-2 |
| `experimental_11.0` HEAD | 22f5f69, 2026-09-24 | FEX-2609-122-g4e5c18113 | 11.3.0 arm64 | 0.5.0 net48-arm64 | 4.0.20260714.251823-0 |
| `bleeding-edge`, tag `experimental-bleeding-edge-11.0-442053-20260928` | 0f6dde6, 2026-09-28 | FEX-2609-137-g0df84d384 | 11.3.0 arm64 | 0.5.0 net48-arm64 | 4.0.20260714.251823-0 |

- Label: VERIFIED IN SOURCE.
  https://github.com/ValveSoftware/Proton/tree/proton_11.0 ;
  https://github.com/ValveSoftware/Proton/tree/experimental_11.0 ;
  https://github.com/ValveSoftware/Proton/tree/bleeding-edge
- The `bleeding-edge` branch is separate. Its tag 442053 is not an ancestor
  of `experimental_11.0`.
- The three branch heads above matched `git ls-remote` while writing
  (22f5f694, 0f6dde60, 5b89db94).

### 4.4 Release notes

UPSTREAM DOCUMENTED:
https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-1-beta1 ;
https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2 ;
https://github.com/ValveSoftware/Proton/releases

| release | date | note |
|---|---|---|
| 11.0-beta1 | 2026-04-16 | "Added FEX-2604 for ARM64EC builds." |
| proton-11.0-1-beta3 | 2026-05-13 | "small update affecting ARM64 only", "Updated to FEX-2605" |
| 11.0-2 | 2026-08-21 | "Updated to FEX-2607." |

### 4.5 On a real Frame

COMMUNITY OBSERVATION:
https://github.com/saphid/frame-control/blob/main/docs/sideloading.md ;
https://github.com/saphid/frame-control/blob/main/docs/evidence/mods-2026-09-28.md

- PuTTY (x86-64) ran under Proton 11 (stable) through FEX ARM64EC inside the
  SLR 4.0 ARM64 container, via `SteamLinuxRuntime_4-arm64/_v2-entry-point`.
  This was on BUILD_ID 20260922.6101926, 2026-09-26.
- A separate test on 2026-09-28, on BUILD_ID 20260925.6191901, recorded the
  version file `1788505046 proton-11.0-2c-arm64`.
- `docs/STEAM_FRAME_REFERENCE.md` keeps 20260925.6191901 as a
  community-observed build ID. `docs/STEAM_FRAME_SNAPSHOT_2026-09-29.md`
  finds it as 0.4.1 in Valve's beta channel file, as a tracker mirrors it
  (OBSERVED EXTERNAL METADATA).

### 4.6 Licence

- Top level BSD-3-Clause (`LICENSE.proton`). Each component has its own
  licence: Wine LGPL, DXVK zlib, vkd3d-proton LGPL, FEX MIT. VERIFIED IN
  SOURCE: https://github.com/ValveSoftware/Proton/tree/experimental_11.0
- Valve's depot builds: PROPRIETARY_DO_NOT_REDISTRIBUTE.

### 4.7 What SteamARM has measured of it

| fact | label | source |
|---|---|---|
| `wine` from Proton ARM64 has PT_LOAD alignment 0x10000, a multiple of the 16 KiB host page. | MEASURED | `benchmarks/stage2-elf-alignment.txt` |
| Proton 11.0 (ARM64) `wine cmd` under lxrun cannot reserve 0x10000-0x68000000 and 0x7f000000-0x7fff0000, and exits in 2 s. | MEASURED | `benchmarks/stage18-settings-audio-controllers.txt` ("Proton ARM64 on macOS") |
| The cause: an arm64 Mach-O has a hard 4 GiB page zero, and Wine's unix ntdll maps `user_shared_data` at the fixed address 0x7ffe0000 and exits if that fails. | VERIFIED IN SOURCE (xnu and Wine) | `benchmarks/stage19-steamframe-base-and-arm64-limits.txt` §2 |
| A native Mach-O probe linked with `-Wl,-pagezero_size,0x1000` shrank `__PAGEZERO` to 0x4000, but `MAP_FIXED` at 0x7ffe0000 killed it (SIGKILL, exit 137). Shrinking the segment alone does not free that address. | MEASURED | `benchmarks/stage18-settings-audio-controllers.txt` (follow-up of 2026-09-28) |

## 5. The Steam client arm64 manifests and client runtime

The client and every package below are PROPRIETARY_DO_NOT_REDISTRIBUTE.

### 5.1 Manifests

OBSERVED EXTERNAL METADATA, from SteamTracking's mirror. The
client-update.steamstatic.com host was not queried directly.
https://github.com/SteamTracking/SteamTracking/blob/master/urls.txt ;
https://raw.githubusercontent.com/SteamTracking/SteamTracking/master/ClientManifest/steam_client_linuxarm64 ;
https://raw.githubusercontent.com/SteamTracking/SteamTracking/master/ClientManifest/steam_client_publicbeta_linuxarm64 ;
https://raw.githubusercontent.com/SteamTracking/SteamTracking/master/ClientManifest/steam_cmd_linuxarm64

| manifest | version | date |
|---|---|---|
| `steam_client_linuxarm64` (stable) | 1788652215 | 2026-09-05 |
| `steam_client_publicbeta_linuxarm64` | 1790545198 | 2026-09-27 |
| `steam_client_linux_arm64_beta_861407927dc8efb407da7cf_linuxarm64` | 1790377368 | not recorded |
| `steam_cmd_linuxarm64` (bootstrapper `steamcmd_linuxarm64.zip`; also `steamcmd_bins_linuxarm64`, `steamcmd_siteserverui_linuxarm64`, `steamcmd_public_all`) | 1788292693 | not recorded |

- The owner's Mac fetched `steam_client_linuxarm64` at version 1788652215:
  35 zips, about 1.0 GB. The client then updated itself (659 MB). MEASURED,
  `benchmarks/stage21-native-arm64-client.txt`.
- Arm64 client lines named steamdeck_stable and steamdeck_publicbeta are in
  use. COMMUNITY OBSERVATION, not SteamTracking: SteamTracking mirrors only
  the x86 `steam_client_steamdeck_{stable,publicbeta}_ubuntu12`.
  https://github.com/Nova-Deck/os-build/blob/main/build/steam-seed/STEAM_SEED.pin ;
  https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-session
- Channels seen in use: publicbeta, stable, steamdeck_stable and
  steamdeck_publicbeta. COMMUNITY OBSERVATION, same sources.
- A Frame ran beta 1790377368 on 2026-09-28. COMMUNITY OBSERVATION:
  https://github.com/saphid/frame-control/blob/main/docs/evidence/mods-2026-09-28.md
- A client version is a Unix build timestamp. For example 1785799196 is
  2026-08-03 and 1788291500 is 2026-09-01. COMMUNITY OBSERVATION, as
  corrected by the adversarial pass:
  https://github.com/Nova-Deck/os-build/blob/main/build/steam-seed/STEAM_SEED.pin.
  The manifest dates above match this reading.

### 5.2 Packages

OBSERVED EXTERNAL METADATA, same SteamTracking sources.

- The native package set is `*_all` plus `bins_`, `webkit_`, `sdl3_`,
  `codecs_`, `bins_sdk_` and `resource_` `*_linuxarm64_linuxarm64`, and
  `bins_androidarm64_linuxarm64`.
- `bins_linuxarm64_linuxarm64` is about 110 MB and holds `steamrtarm64/steam`.
  `webkit_linuxarm64_linuxarm64` (CEF) is about 111 MB.
- On the Mac, `steamrtarm64/steam` is an aarch64 PIE that needs only glibc
  (MEASURED, stage21).
- The same manifests also list single-suffix `*_linuxarm64` packages:
  - `steam_linuxarm64`, flagged `IsBootstrapperPackage`;
  - `runtime_steamrt_linuxarm64`, 127,458,179 bytes;
  - `bins_steamrt_linuxarm64`, `webkit_steamrt_linuxarm64`,
    `sdl3_steamrt_linuxarm64` and `steam_steamrt_linuxarm64`.

### 5.3 The client runtime

- Community installers fetch `steam-runtime-steamrt-arm64.tar.xz` separately
  from `https://repo.steampowered.com/steamrt3c/images/<snapshot>/`. The
  snapshot id comes from `latest-public-beta.txt` or
  `latest-public-stable.txt`. A pinned example is 3c.0.20260824.257593.
  Switchdeck extracts it to `steamrtarm64/pv-runtime/`. COMMUNITY
  OBSERVATION: https://github.com/SildurFX/Switchdeck/blob/main/install-steam.sh ;
  https://github.com/Daaboulex/steam-arm64-nix/blob/main/runtime.nix ;
  https://github.com/Nova-Deck/os-build/blob/main/build/steam-seed/fetch-steam-seed.sh
- Whether the manifest's `runtime_steamrt_linuxarm64` package carries the
  same runtime: UNKNOWN.
- This runtime carries `steam-runtime-launcher-service` and pressure-vessel,
  which the client calls. COMMUNITY OBSERVATION, same sources.
- In stage21's root, `steam-runtime-launcher-service` was not found, and the
  client went on without it (MEASURED: "not found in the root
  (non-fatal)").
- The client dlopens `libibus-1.0.so.5` from a fixed `lib/aarch64-linux-gnu`
  path. COMMUNITY OBSERVATION, same sources.
- The nix packager marks the runtime unfree (COMMUNITY OBSERVATION,
  https://github.com/Daaboulex/steam-arm64-nix/blob/main/runtime-source.nix).
  Its licence is otherwise UNKNOWN.

### 5.4 What the client needs from the host

COMMUNITY OBSERVATION:
https://github.com/huntergdavis/steamclienttermux/blob/main/docs/TECHNICAL_LOG.md ;
https://github.com/huntergdavis/steamclienttermux/blob/main/docs/ARCHITECTURE.md ;
https://github.com/Daaboulex/steam-arm64-nix/blob/main/fhs.nix

| need | community report | SteamARM today |
|---|---|---|
| Host libraries | The arm64 steamwebhelper takes GLib, GTK3, X11, NSS/NSPR, audio, font and graphics libraries from the host. The `steam` bootstrap needs only glibc. | `scripts/mkarmroot.sh` builds a Fedora 43 aarch64 root for the native client at `$STEAMARM_STATE/armroot` (default `~/SteamARM-roots/armroot`) (VERIFIED IN SOURCE). The client's home is `<armroot>/tmp/armhome` (MEASURED, stage21). The root holds the shared-library closure of what the client links: GTK 2, NSS with softokn and freebl, X11, Mesa GLX/EGL, audio, SDL2 and more; 153 packages, about 570 MB (MEASURED, stage21). It is separate from `/tmp/lxrt-arm64root`, the link meant for the Frame-derived root, which no script builds (`docs/CURRENT_STEAM_ENVIRONMENT.md` §3; `scripts/env-links.sh:13-19`). |
| `lsof` | The client checks its webhelper NetworkService WebSocket with `lsof`. | MEASURED for the x86 client: it requires `lsof` ("lsof is required to run steam") and rejects the WebSocket when the check fails (`benchmarks/stage8-steam-zero-vm.txt:296-311`). On the arm64 client under PRoot, a missing `lsof` led to "free(): invalid pointer" and an abort (COMMUNITY OBSERVATION, https://github.com/huntergdavis/steamclienttermux/blob/main/docs/TECHNICAL_LOG.md). lxrun serves `/proc/<pid>/fd` and `/proc/net/tcp` for it (`runtime/procpid.c`, VERIFIED IN SOURCE). `scripts/mkarmroot.lock` has no lsof package (VERIFIED IN SOURCE). stage21's client aborts with "free(): invalid pointer" (MEASURED), not yet diagnosed. HYPOTHESIS: the missing `lsof` is the cause. |
| SysV semaphores | The client relies on semop wakeups; PRoot had to fix them. | VERIFIED IN SOURCE: `runtime/sysv_ipc.c` passes semaphores to Darwin's kernel SysV implementation, `semctl` SETVAL and SETALL included (the `L_SETVAL` and `L_SETALL` cases). HYPOTHESIS: Darwin's kernel then wakes blocked `semop` waiters itself. `semtimedop` is emulated as a polled `semop` with a backoff from 100 us to 5 ms (VERIFIED IN SOURCE, the `semtimedop` comment in the same file), so its wakeups are late by up to that interval. |
| robust futex list | The client uses `set_robust_list` and `get_robust_list`; PRoot had to emulate them. | VERIFIED IN SOURCE: `runtime/dispatch.c:2202-2222` keeps the thread's own list. The kernel's walk of the list when a thread dies is not implemented. |
| CEF sandbox | CEF was run with `-cef-disable-gpu` / `--no-sandbox`. | VERIFIED IN SOURCE: lxrun reports `max_user_namespaces` 0, so `steamwebhelper.sh` starts CEF with `--no-sandbox` (`runtime/proc_ext.c:1174-1184`). |

Other client reports (COMMUNITY OBSERVATION):

- The native client's web helper does not carve the X input shape, so its own
  title bar cannot move the window.
  https://github.com/Daaboulex/steam-arm64-nix/blob/main/README.md
- Remote Play on the arm64 client needs Vulkan Video decode. One installer
  runs the x86-64 `streaming_client` under FEX instead.
  https://github.com/Scrumpper/Steam-ARM/blob/main/steam-arm-install.sh

### 5.5 Page size

The community claims conflict. COMMUNITY OBSERVATION:

- steam-arm64-nix runs the native client in a 4K-page muvm microVM on Apple
  Silicon, which it says the client's binaries need.
  https://github.com/Daaboulex/steam-arm64-nix/blob/main/README.md
- UbuntuAsahi also runs it under muvm.
  https://github.com/UbuntuAsahi/steam-arm64/blob/master/steam-arm64.sh
- Steam-ARM requires 4K pages only for the x86 emulation half.
  https://github.com/Scrumpper/Steam-ARM/blob/main/COMPATIBILITY.md
- Switchdeck reports that client builds newer than 2026-04-15 SIGILL on
  Cortex-A57. https://github.com/SildurFX/Switchdeck/blob/main/README.md
- Stock Proton arm64 needs ARMv8.1+ LSE atomics.
  https://github.com/Azkali/proton-arm64-a57

What SteamARM has:

- MEASURED: Fedora aarch64 binaries, FEX and Proton ARM64's `wine` all use
  `p_align` 0x10000 (`benchmarks/stage2-elf-alignment.txt`).
- MEASURED (stage21): Valve's native client ships 4 KiB-aligned images. With
  lxrun's default AT_PAGESZ of 16384, glibc refused to dlopen `steamui.so`
  ("ELF load command address/offset not page-aligned"). With
  `LXRT_GUEST_PAGE=4096`, the client loaded `steamui.so` and
  `steamclient.so`.
- VERIFIED IN SOURCE: AT_PAGESZ is 16384 unless `LXRT_GUEST_PAGE=4096` is
  set (`runtime/stack.c:21-34, 134`). `elf.c` now loads an executable or
  `ld.so` whose PT_LOAD alignment is a multiple of 4 KiB. It keeps the
  protections per 4 KiB through `runtime/subpage.c`, and refuses anything
  less aligned (`runtime/elf.c:147-157, 209-225`).
- VERIFIED IN SOURCE: `subpage.c` now maps the page-aligned interior of a
  large file segment straight from the file and copies only its edges
  (`runtime/subpage.c:273-344`). The comment names `libcef.so`'s 162 MiB
  text segment. stage21 records webhelper start at 2.52 s with libcef
  copied and 1.49 s with it mapped from the file (MEASURED).
- UNKNOWN: the `p_align` of each file in `steamrtarm64/*` and of the
  steamrt3c runtime. stage21 records the 4 KiB alignment of the client, not
  a per-file list.

### 5.6 steamcmd

- A native `steam_cmd_linuxarm64` exists (§5.1). OBSERVED EXTERNAL METADATA.
- Untested under lxrun: UNKNOWN.

## 6. Community installers and projects

"Steam-ARM" below is a separate community project, not this repository.

| project | what it does | label | source |
|---|---|---|---|
| Steam-ARM | Takes `bins_linuxarm64_linuxarm64` from the manifest (skipping the `.vz` entry) and lets the client self-update. Patches Valve's downloaded FEX in `steamapps/common/FEX-Emu`: ThunksDB overlay paths (`/run/gfx/main`, pressure-vessel overrides), ServerSocketPath and GL/Vulkan thunks. Re-applies the patch whenever Steam replaces the files. | COMMUNITY OBSERVATION | https://github.com/Scrumpper/Steam-ARM/blob/main/steam-arm-install.sh ; https://github.com/Scrumpper/Steam-ARM/blob/main/CHANGELOG.md |
| Nova-Deck os-build | Takes the manifest's bootstrap entry (skipping `.vz`). Pins client builds per channel. Recorded the steamcmd dump of FEX 3127680 and the mid-August change in tool selection. | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/blob/main/build/steam-seed/fetch-steam-seed.sh ; https://github.com/Nova-Deck/os-build/blob/main/build/steam-seed/STEAM_SEED.pin ; https://github.com/Nova-Deck/os-build/blob/main/docs/windows-games-fex.md |
| UbuntuAsahi steam-arm64 | Takes the manifest's bootstrap entry. Runs the client under muvm. Strips the 4185400 requirement from GE-Proton11-5-aarch64 with sed. | COMMUNITY OBSERVATION | https://github.com/UbuntuAsahi/steam-arm64/blob/master/steam-arm64.sh |
| DroidDeck | Downloads, sha256-verifies and unzips every `*_all` and `*_linuxarm64_linuxarm64` package of `steam_client_publicbeta_linuxarm64` itself. Registers its own tool, `bannerlator-proton-arm64`, whose toolmanifest has no `require_tool_appid`. Its launcher script finds "Proton Experimental (ARM64)" or "Proton 11.0 (ARM64)" under `steamapps/common` and execs that depot's `proton` directly. This bypasses SLR4, because Android gives no user namespaces. The script's header comment, which describes symlinks, is stale. | COMMUNITY OBSERVATION | https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-steam-install ; https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/bannerlator-steam-compat |
| Switchdeck | Skips the manifest and pins one old bootstrap zip, because builds after 2026-04-15 SIGILL on the Switch. Fetches the steamrt3c runtime separately. Ships "Proton 11.0-1 Armv8.0 (FEX)". | COMMUNITY OBSERVATION | https://github.com/SildurFX/Switchdeck/blob/main/install-steam.sh ; https://github.com/SildurFX/Switchdeck/blob/main/README.md |
| steam-arm64-nix | Runs the client in a 4K-page muvm microVM on Apple Silicon. Fetches the steamrt3c runtime. Documents the SLR4-arm64 layout. Its muvm guest has no binder, so it cannot run Lepton. | COMMUNITY OBSERVATION | https://github.com/Daaboulex/steam-arm64-nix/blob/main/README.md ; https://github.com/Daaboulex/steam-arm64-nix/blob/main/guest-run.sh |
| steamclienttermux | Runs the client under a patched PRoot on Android. PRoot emulates bwrap's syscalls (namespaces, mount, pivot_root); the project does not interpret the bwrap argument plan. Android returns EINVAL for user namespaces and EPERM for mount namespaces. Registered 4628740 and 4185400 through a local `compatibilitytool.vdf`. | COMMUNITY OBSERVATION | https://github.com/huntergdavis/steamclienttermux/blob/main/docs/TECHNICAL_LOG.md ; https://github.com/huntergdavis/steamclienttermux/blob/main/config/steam-arm64-compatibilitytools.vdf.in |
| frame-control | Records evidence from a real Frame: the Proton runs of §4.5, the Devkit client's runtime aliases, Lepton containers. Defaults to Proton Experimental (ARM64). | COMMUNITY OBSERVATION | https://github.com/saphid/frame-control/blob/main/docs/sideloading.md ; https://github.com/saphid/frame-control/blob/main/docs/how-the-frame-works.md |
| proton-arm64-a57 | Cited for the report that stock Proton arm64 needs ARMv8.1+ LSE atomics (§5.5). | COMMUNITY OBSERVATION | https://github.com/Azkali/proton-arm64-a57 |
| winlator-contents re-upload | A community re-upload of Proton Experimental (ARM64) buildid 25502785. Cited only for depot numbers. PROPRIETARY_DO_NOT_REDISTRIBUTE. | COMMUNITY OBSERVATION | https://github.com/The412Banner/winlator-contents/releases/tag/steam-proton-arm64-25502785 |
| postmarketOS blog post | Running the arm64 client on pmOS. Only snippets were read (§3, §8). | COMMUNITY REFERENCE | https://blog.drakulix.de/taming-the-steam-arm64-client-on-pmos/ |

What the installers do in common (COMMUNITY OBSERVATION, the sources above):

- Most installers (Steam-ARM, Nova-Deck, UbuntuAsahi) fetch the manifest
  from `client-update.(fastly.)steamstatic.com` and take the
  `bins_linuxarm64_linuxarm64.zip.<sha1>` entry. DroidDeck pre-fetches the
  whole set. Switchdeck pins a fixed old bootstrap.
- The zip carries no unix modes, and some entries use backslash separators.
  Installers fix the exec bits. stage21 met the backslashes too, and
  extracted the entries with the separators normalised (MEASURED).
- The result is `steamrtarm64/steam`. Installers write `package/beta`.
- The client is launched as `steamrtarm64/steam` with
  `LD_LIBRARY_PATH=steamrtarm64[:steamrtarm64/panorama]`. It self-installs
  the rest. Exit code 42 means "restart after update".
- Valve's `steam.sh` has no arm64 branch, so installers create the links:
  - `~/.steam/{root,steam}` point to the Steam root;
  - `sdkarm64` points to `linuxarm64`, which holds `steamclient.so` and
    `steam-launch-wrapper` (every game launch goes through it);
  - `bin64` and `binarm64` point to `steamrtarm64`;
  - `steamrtarm32` points to `steamrtarm64`.
- Without `sdkarm64`, launches die in `/bin/sh` before Proton runs.

All of these download from Valve on the user's machine. None of them makes
the client redistributable.

## 7. What this means for SteamARM

### 7.1 Proton ARM64 cannot run on SteamARM as it is

- Valve's Proton ARM64 cannot start unmodified in a native arm64 macOS
  process. Wine's unix ntdll needs a mapping at 0x7ffe0000. macOS gives every
  arm64 process a hard 4 GiB page zero, and nothing lowers it after exec.
  VERIFIED IN SOURCE (xnu and Wine), `benchmarks/stage19-steamframe-base-and-arm64-limits.txt` §2.
  MEASURED: `wine cmd` exits in 2 s (`benchmarks/stage18-settings-audio-controllers.txt`).
  Shrinking `__PAGEZERO` at link time did not help: `MAP_FIXED` at
  0x7ffe0000 was still killed (MEASURED, stage18 follow-up of 2026-09-28).
- x18, the ARM64 Windows TEB register, is the second blocker. A binary
  built against a macOS SDK below 13 kept x18 across preemption on a GitHub
  macOS 15.7.9 runner (MEASURED, `benchmarks/stage20-ci-macos-runner.txt`).
  That does not change the 0x7ffe0000 blocker, and it is still to be
  measured on the M4 under macOS 27.
- A patched Wine could run ARM64 code on macOS. Madeira relocates the
  shared-data page and serves reads of 0x7ffe0000 from a Mach exception
  handler (VERIFIED IN SOURCE, a third-party project, not measured here;
  stage19 §2). 32-bit (wow64) games stay on x86 Proton under FEX.
- An ARM64 Proton entry in Settings > Compatibility would therefore be a
  list entry only. Selecting it would not start a game on SteamARM.
- SteamARM's own launcher already treats it that way. `scripts/proton-command.py`
  refuses a Proton that has `files/bin-arm64` or says ARM64 in its name
  (`scripts/proton-command.py:37-38`), and `scripts/compat-status.py` lists
  such a tool as not compatible (`scripts/compat-status.py:66-73`). VERIFIED
  IN SOURCE. `docs/TROUBLESHOOTING.md` tells users the same.

### 7.2 What the entry would need even to appear

1. **A native arm64 client.** The x86 client under FEX registered no ARM64
   Proton (MEASURED, `docs/CURRENT_STEAM_ENVIRONMENT.md` §1). Valve says
   ARM64 builds cannot be used from x86 Steam under FEX (UPSTREAM
   DOCUMENTED, Proton README). The record also holds stage15, where the x86
   client installed Proton (ARM64); the native client is what counts now.
   stage21 runs that client under lxrun, up to an abort in its main process
   (MEASURED).
2. **That client up to its Settings page under lxrun.** stage21 got it to
   load `steamui.so` and `steamclient.so` and to start its UI process tree.
   steamwebhelper reached BrowserReady in an earlier run (MEASURED). Open
   items:
   - the main process's `free(): invalid pointer` abort (MEASURED). Its
     cause is now a HIGH-CONFIDENCE HYPOTHESIS from disassembly: `vgui2_s`
     ignores an Xlib error and frees an uninitialised pointer, on the
     rescue-dialog path taken after the webhelper times out, because the
     root had no X locale data (`docs/STEAM_ARM64_BRINGUP.md`). The earlier
     `lsof` HYPOTHESIS (§5.4) is not part of that chain;
   - the NSS network-process FATAL of earlier notes. The last one in
     `cef_log.txt` is from 2026-09-28 17:17:19 (`libsoftokn3.so: cannot
     open shared object`); the file is in the root now. No later run
     reached CEF logging, so whether it is gone is UNKNOWN
     (`docs/STEAMWEBHELPER_BRINGUP.md`);
   - the renderers' V8 without a JIT or with one that asks for the W^X flip
     (HYPOTHESIS, `docs/CURRENT_STEAM_ENVIRONMENT.md` §7 and §9).

   stage21's root holds the closure of what the client links (MEASURED).
   Its lock has GTK 2 but not the GTK 3 of the community report, and no
   aarch64 `lsof` (VERIFIED IN SOURCE, `scripts/mkarmroot.lock`).
3. **A launch the way installers do it.** The links of §6, `package/beta`,
   `LD_LIBRARY_PATH` and a restart on exit code 42 (COMMUNITY OBSERVATION).
   stage21 started the client by hand:
   `LXRT_ROOT=<armroot> LXRT_GUEST_PAGE=4096 HOME=/tmp/armhome DISPLAY=:2 build/lxrun /tmp/armhome/.local/share/Steam/steamrtarm64/steam`
   (MEASURED). Whether it made the links of §6 is not recorded (UNKNOWN).
   SteamARM's launch path still keys on `ubuntu12_32/steam` and `steamui.so`
   (VERIFIED IN SOURCE, `scripts/run-app.sh:41, 94-98`;
   `docs/CURRENT_STEAM_ENVIRONMENT.md` §7).
4. **A client build and channel that carries the offer.** Builds on the
   steamdeck_stable line differ in it (COMMUNITY OBSERVATION, §3). Whether
   stable 1788652215 and publicbeta 1790545198 carry it: UNKNOWN.
   HYPOTHESIS: builds after mid-August 2026 do. stage21 installed stable
   1788652215 and let it update itself; the version after the update is not
   recorded.
5. **No identity flags.** `-steamos3` can loop on restart and `-deckard`
   asserts Frame identity (COMMUNITY OBSERVATION). HYPOTHESIS: neither is
   needed for the offer. Whether any device gate applies: UNKNOWN (§3).
6. **Or a local declaration.** A `compatibilitytool.vdf` in
   `compatibilitytools.d` made the client register 4628740 and 4185400
   (COMMUNITY OBSERVATION, §2.4). Proton's template uses
   `from_oslist "windows"` and `to_oslist "linux"` (VERIFIED IN SOURCE).
   HYPOTHESIS: the entry then points at whatever `install_path` names, and
   the tool's files must already be present.
7. **The required runtime.** Whether the list hides a tool whose
   `require_tool_appid` (4185400) is not installed: UNKNOWN. Community
   reports say 4185400 installs lazily at first launch (COMMUNITY
   OBSERVATION), which suggests it is not needed for the entry to appear
   (HYPOTHESIS). The Mac's audit of 2026-09-28 found
   `SteamLinuxRuntime_4-arm64`, Proton 11 ARM64 and Experimental ARM64
   installed in the x86 client's library (MEASURED,
   `docs/CURRENT_STEAM_ENVIRONMENT.md`, appendix).
   `scripts/install-arm-proton-tools.sh` clones those three (4185400,
   4427310, 4628740), with their appmanifests, into the native client's
   library (VERIFIED IN SOURCE, `scripts/install-arm-proton-tools.sh:35-39`).
   Whether the native client then lists them: UNKNOWN.

### 7.3 Other implications

| implication | label |
|---|---|
| A native arm64 client will default Windows titles to Proton 11.0 (ARM64) (§3). SteamARM must route Windows titles elsewhere: a SteamARM tool in `compatibilitytools.d` that runs x86 Proton through SteamARM's FEX, or a Madeira-style patched ARM64EC Wine, selected through CompatToolMapping. | HYPOTHESIS |
| DroidDeck's approach (its own tool without `require_tool_appid`, exec'ing Valve's depot `proton`) removes only the SLR4 dependency. It still runs Valve's ARM64 Wine, so it does not get past 0x7ffe0000 on macOS. | HYPOTHESIS, from §6 and §7.1 |
| Valve's FEX (3127680) will be downloaded into `steamapps/common/FEX-Emu` and used as the pressure-vessel emulator through `STEAM_COMPAT_EMULATOR`. It lacks SteamARM's patches: `-ffixed-x18`, the guest-driven W^X flip and the guest base (`docs/CURRENT_STEAM_ENVIRONMENT.md` §5.2, §5.4). It should be expected to fail under lxrun. | HYPOTHESIS |
| Two ways around it: have the bwrap interpreter or the launcher substitute SteamARM's own `emulator.json` (argv FEX-emu, `server_argv` a FEXServer that follows the `READY=1` pipe protocol); or overwrite the depot's files, as Steam-ARM does. Steam re-verifies and replaces them, so the second needs a watcher. | HYPOTHESIS |
| Valve's `emulator.json` sets `FEX_ROOTFS=''` in the container. SteamARM's bwrap interpreter keeps FEX's files visible by binding the caller's `FEX_ROOTFS`, `$HOME/.fex-emu` and `/usr/lib/lxrt-emu`, and skips an empty `FEX_ROOTFS` (`runtime/mounts.c:283-312`, VERIFIED IN SOURCE). How the two meet is untested. | UNKNOWN |
| Under the x86 client, pressure-vessel detects FEX by its CPUID leaves and then builds `/run/pressure-vessel/interpreter-root`. Without the leaves it built none, and every steamwebhelper zygote died (MEASURED, `benchmarks/stage15-steam-proton-path.txt` problem 8). Whether pressure-vessel still builds that root on the `emulator.json` path, or uses the graphics provider instead: UNKNOWN. | MEASURED / UNKNOWN |
| The graphics-provider contract (`graphics_provider_v0`, architectures `x86_64-linux-gnu` and `i386-linux-gnu`, root = the x86 rootfs) could let SteamARM present its existing x86 rootfs to Valve's tooling without cloning x86 as `/`. | HYPOTHESIS |
| SLR 4.0 Arm64 is pressure-vessel plus bwrap. On Linux it needs user namespaces (COMMUNITY OBSERVATION, §1, https://github.com/Daaboulex/steam-arm64-nix/blob/main/guest-run.sh). lxrun interprets the bwrap plan in-process and needs none, but it refuses `--unshare-*`, `--cap-*`, `--uid`, `--gid` and `--seccomp` with ENOSYS (`runtime/mounts.c:456-470`, VERIFIED IN SOURCE). The x86 plan asked for none of them (MEASURED, `benchmarks/stage6-bwrap-plan.txt`). The SLR4-arm64 plan is UNKNOWN. | VERIFIED IN SOURCE / MEASURED / UNKNOWN |
| steamclienttermux solves the container differently: it emulates bwrap's syscalls in PRoot. Its fixes do not carry over to lxrun one for one. | COMMUNITY OBSERVATION / HYPOTHESIS |
| Proton ARM64 is built with `-march=armv8.2-a` (VERIFIED IN SOURCE). The M4 under macOS 27 reports FEAT_LSE, RDM, DotProd and LRCPC2 (MEASURED, `benchmarks/stage5-idregs.txt`). There is no ISA blocker there, and the Cortex-A57 SIGILL reports do not apply. | VERIFIED IN SOURCE / MEASURED; the conclusion is HYPOTHESIS |
| Lepton (3029110) needs rootless podman, user namespaces and Android binder. It is not viable under lxrun on macOS. Since it offers itself even where it cannot run (§3), SteamARM should filter it or document it as unsupported. | HYPOTHESIS |
| The arm64 client can be fetched from Valve at run time, on the user's Mac, instead of being copied out of the Frame image. stage21 did so from the `steam_client_linuxarm64` manifest, and the client then updated itself (MEASURED). No repository script does it yet: `scripts/install-steam.sh` fetches only the x86 bootstrap, and no script names a `linuxarm64` URL (VERIFIED IN SOURCE; `docs/CURRENT_STEAM_ENVIRONMENT.md` §5.2). The steamrt3c runtime was probably not part of stage21's install, since `steam-runtime-launcher-service` was missing (HYPOTHESIS). | MEASURED / VERIFIED IN SOURCE / HYPOTHESIS |
| The steamwebhelper NSS FATAL came from a missing NSS module: the last one, at 2026-09-28 17:17:19, names `libsoftokn3.so` (MEASURED, `cef_log.txt`). stage21's root now carries NSS with softokn and freebl, and steamwebhelper reached BrowserReady at 17:15:32 the same day (MEASURED). The 2026-09-29 webhelpers never reached CEF logging, so whether the FATAL is gone is UNKNOWN. The run now ends in the main process, at `free(): invalid pointer` (MEASURED; cause in `docs/STEAM_ARM64_BRINGUP.md`). | MEASURED / UNKNOWN |
| steamcmd for linuxarm64 could fetch depots 4185400, 4628740 and 3127680 for offline inventory without x86 emulation. 3127680 is ownersonly (COMMUNITY OBSERVATION); the others may also need an entitled account. | HYPOTHESIS |

### 7.4 Next steps

These steps are now ordered, with the rest of the ARM64 work, in
`docs/ARM64_FIRST_MIGRATION.md`; the list below keeps their detail.

On the Mac, in this order. stage21 has done parts of steps 1 to 4.

1. Get the native client. Done in stage21: downloaded from Valve's
   `steam_client_linuxarm64` manifest on the Mac, then self-updated
   (MEASURED). Keep it out of the `.dmg`. Still to do: a repository script
   for that download.
2. Measure `p_align` of `steamrtarm64/*`, `libcef` and the client runtime
   with `scripts/steamframe-image.py inventory` or `readelf`. Partly done:
   stage21 found the client 4 KiB-aligned and loaded it with
   `LXRT_GUEST_PAGE=4096`. Record the per-file values in `benchmarks/`.
3. Put the host libraries of §5.4 and an aarch64 `lsof` in the ARM64 root.
   Done by `scripts/mkarmroot.sh` for the libraries the client links, NSS
   with softokn and freebl included (stage21). Its lock has no GTK 3 and no
   `lsof`. Still to do: add `lsof` to its seeds, run stage21's command
   again, and record whether the `free(): invalid pointer` abort changes.
4. Start `steamrtarm64/steam` as an `aarch64` launcher entry. stage21
   started it by hand with `build/lxrun` (MEASURED). `scripts/run-native.sh`
   defaults `LXRT_ROOT` to `/tmp/lxrt-arm64root` and does not set
   `LXRT_GUEST_PAGE` (VERIFIED IN SOURCE, `scripts/run-native.sh:19-29`), so
   the entry must set both. Add the links of §6, and the launcher keys for
   `steamrtarm64/steam` beside the `ubuntu12_32/steam` ones.
5. Open Steam > Settings > Compatibility. Record the list shown,
   `compat_log.txt`, the client version and the channel in a new
   `benchmarks/` stage.
6. If no ARM64 Proton is listed, check that
   `scripts/install-arm-proton-tools.sh` has cloned the tools into the
   native client's library. Then add a local `compatibilitytool.vdf` as in
   §2.4 and record whether an entry appears.
7. Launch one Windows probe and capture the bwrap plan that SLR4-arm64
   emits (the `[lxrt] bwrap:` trace). Diff it with
   `benchmarks/stage6-bwrap-plan.txt`. Look for `--unshare-*`, the
   emulator-directory bind and `/run/gfx/main`.
8. Build the routing tool of §7.3 and map Windows titles to it. Stop Valve's
   FEX from being used as the emulator.
9. Run `tests/x18_preserve/run.sh` on the M4 under macOS 27, and check
   `_ml_satisfies_x86_64_requirements` in the kernelcache
   (`benchmarks/stage19-steamframe-base-and-arm64-limits.txt` §3).

## 8. Could not be checked from here

From the research session:

- blog.drakulix.de ("Taming the Steam arm64 client (on pmOS)"): WebFetch
  returned EGRESS_BLOCKED, and shell curl got a proxy CONNECT 403. This is an
  egress-policy block, not an anti-bot wall. Only search-engine snippets were
  used. The full article, including any NSS or library details, was not read.
- web.archive.org copy of that blog post: WebFetch refused it.
- lobste.rs discussion of the blog post: EGRESS_BLOCKED.
- partner.steamgames.com (steamframe/compatibility, loadgames, compat,
  adb_lepton, steamhardware/proton): EGRESS_BLOCKED. Only search snippets were
  used.
- steamdb.info (apps 4185400, 4628740, 1493710, 3029110, 3127680, 4427310,
  3043620): EGRESS_BLOCKED. Only result titles and snippets were used, so
  SteamDB depot lists, build IDs and branch tables are not verified.
- store.steampowered.com (app pages and news): only result titles and
  snippets were used. The pages were not fetched.
- gamingonlinux.com, techspot.com, digitalcitizen.life, hwbusters.com,
  vr.org, interfacinglinux.com: EGRESS_BLOCKED. Snippets only.
- wiki.fex-emu.com/index.php/Steam: EGRESS_BLOCKED.
- gitlab.steamos.cloud (steam-runtime-tools, the SLR4 arm64 README,
  frame-public/lepton): unreachable. Third-party GitHub mirrors were used
  instead: https://github.com/llyyr/steam-runtime-tools at 2026-03-22 and
  https://github.com/xXJSONDeruloXx/lepton at 2026-09-16.
- repo.steampowered.com (steamrt3c and steamrt4 images) and
  client-update.steamstatic.com: not reachable. SteamTracking's GitHub mirror
  of the client manifests was used.
- GitHub REST API for ValveSoftware/Proton and SteamTracking (release list,
  file history): refused by the session's repository scope. The github.com
  release pages were read with WebFetch instead.
- The Steam client's own appinfo (depot sizes and manifests for 4185400,
  4628740, 4427310, 3127680, 3029110) could not be queried. No steamcmd or
  login is available there.

Only on the Mac, with lxrun:

- what the native arm64 client's Compatibility list shows (stage21: the
  client starts, then aborts before its window);
- the SLR4-arm64 bwrap plan;
- the per-file `p_align` of the client (stage21: it needs
  `LXRT_GUEST_PAGE=4096`);
- x18 on the M4 under macOS 27.
