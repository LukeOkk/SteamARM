# Steam Linux Runtime 4.0 on arm64, and pressure-vessel

**Status (2026-09-29).** Updated 2026-09-29 after
`benchmarks/stage21-native-arm64-client.txt`. This page covers the Steam Linux
Runtime 4.0 for arm64 and pressure-vessel on arm64, as they matter to
SteamARM. It was written from web research by a cloud session: a Linux
container with no Mac and no lxrun. A second reviewer then tried to refute
every claim. It confirmed 31, refuted or corrected 8, and changed the
label of 6. Those corrections are applied here. SteamARM's own facts come from
`benchmarks/`, `docs/` and the source tree at commit `1911825`. Line numbers
in files that main changed were re-checked at `dfab6e2`, the merge that
brought stage21. stage21 facts are MEASURED on the owner's Mac (M4, macOS 27).
Nothing on this page was run on a Mac for it. Blocked from the session:
`repo.steampowered.com`, `gitlab.steamos.cloud`, `steamdb.info`,
`partner.steamgames.com`, `wiki.debian.org` and GitHub's global search API.
`blog.drakulix.de`, `gamingonlinux.com`, `discourse.libsdl.org` and
`interfacinglinux.com` were blocked or not fetched.
`registry.gitlab.steamos.cloud` was not tested. Valve's steam-runtime-tools
was read in a third-party GitHub mirror at upstream snapshot 0.20260320.0.
That snapshot is older than the pressure-vessel builds seen in use
(0.20260714.0 and 0.20260728.0).

**Evidence labels:**

| label | meaning |
|---|---|
| MEASURED | observed by SteamARM on a Mac or a CI runner, as recorded in this repository's `benchmarks/` or `docs/` |
| VERIFIED IN SOURCE | read in a source tree; the repository URL and the commit or path are given |
| UPSTREAM DOCUMENTED | Valve or upstream documentation says so |
| OBSERVED EXTERNAL METADATA | SteamDB and similar trackers |
| COMMUNITY OBSERVATION | reported by community projects or users, not confirmed by Valve |
| COMMUNITY REFERENCE | seen only in press or in search snippets. The research also uses it for community code that was read but not run (umu-launcher, packaging scripts). |
| HYPOTHESIS | inferred, not tested |
| UNKNOWN | not established |

`PROPRIETARY_DO_NOT_REDISTRIBUTE` marks Valve files that SteamARM must never
ship: the Steam client, Proton builds and the contents of the Steam Frame
image.

**Source trees read.** Links below point into these.

| tree | commit | used for |
|---|---|---|
| https://github.com/LukeOkk/SteamARM | `1911825`; line numbers re-checked at `dfab6e2` | `runtime/`, `scripts/`, `tests/` (paths without a host below) |
| https://github.com/llyyr/steam-runtime-tools (third-party mirror of `gitlab.steamos.cloud/steamrt/steam-runtime-tools`) | `2691ca4da05a1979debfc4b4d2cc614831cabd50` (upstream 0.20260320.0) | pressure-vessel |
| https://github.com/ValveSoftware/steam-runtime | `8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24` | runtime documentation |
| https://github.com/ValveSoftware/Proton | `5b89db940e0ebe3a137a6009a3589232fe084c09` | tool manifests, build |
| https://github.com/FEX-Emu/FEX | `0df84d3844bcdb87bb7d3f5b8fb0959cd009c038` | FEX's Steam tool files |

## In short

- SLR 4.0 arm64 is app 4185400. Proton's arm64 builds require it
  (VERIFIED IN SOURCE). The older sniper arm64 runtime, app 3810310, is being
  phased out (VERIFIED IN SOURCE, an SDL commit).
- pressure-vessel has two ways to run x86 code on arm64. One is an x86
  pressure-vessel under FEX, in "interpreter-root" mode. The other is an
  aarch64 pressure-vessel with an `emulator.json` (VERIFIED IN SOURCE).
- SteamARM uses the first today, and its working Steam session depends on
  interpreter-root mode (MEASURED). The research's advice to keep
  `FEX_ROOTFS=/` so that the mode stays off is contradicted by SteamARM's
  measurements and is dropped.
- `runtime/mounts.c` accepts every bwrap option seen in pressure-vessel
  plans. Some are no-ops. It has no case for `--bind-fd`, `--ro-bind-fd` or
  `--size` (VERIFIED IN SOURCE).
- SLR 4.0 arm64 does not make Proton ARM64 usable on SteamARM. Its Wine
  still needs 0x7ffe0000, which macOS cannot map
  (`benchmarks/stage19-steamframe-base-and-arm64-limits.txt` §2).

## 1. The runtimes

### 1.1 Runtimes

| runtime | app ID | suite and base distribution | architectures | status | label and source |
|---|---|---|---|---|---|
| Steam Linux Runtime 4.0 | 4183110 | steamrt4, Debian 13 "trixie" | x86-64. i386: see note 1. Also holds a `pressure-vessel-arm64/` directory. | current | UPSTREAM DOCUMENTED: app ID, install directory `SteamLinuxRuntime_4`, Debian 13 ([known-issues.md:26-37](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/doc/steamlinuxruntime-known-issues.md#L26-L37), [goals.md:114-116](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/doc/goals.md#L114-L116), [reporting-steamlinuxruntime-bugs.md:91](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/doc/reporting-steamlinuxruntime-bugs.md#L91)); used by Proton 11 and newer ([README.md:65-73](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/README.md#L65-L73)). UNKNOWN: i386. COMMUNITY OBSERVATION: `pressure-vessel-arm64/` ([prepare-arm64-runtime-shadow.sh:7](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/scripts/prepare-arm64-runtime-shadow.sh#L7)). |
| Steam Linux Runtime 4.0 - Arm64 | 4185400 | steamrt4 (Debian 13) | aarch64 only; no 32-bit ARM | current | VERIFIED IN SOURCE: Proton's arm64 manifest requires 4185400 ([toolmanifest_arm64.vdf](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/toolmanifest_arm64.vdf)), since 2026-01-26 ([0a7e25b6](https://github.com/ValveSoftware/Proton/commit/0a7e25b6def85d56707099578990c8e61cf4fa5e), "Switch to steamrt4."). COMMUNITY OBSERVATION: the name, the directory `SteamLinuxRuntime_4-arm64` and a `steamrt4_platform_*` tree ([TECHNICAL_LOG.md:220-300](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/docs/TECHNICAL_LOG.md#L220-L300), [faugus-launcher#761](https://github.com/Faugus/faugus-launcher/issues/761)). COMMUNITY OBSERVATION: no 32-bit ARM, per a packager quoting the runtime's README ([steam-arm64-nix README](https://github.com/Mr-Banana-Egg/steam-arm64-nix/blob/5d236a5a13d9ee34e743beedb3755db3e56677ca/README.md)). |
| Steam Linux Runtime 3.0 (arm64), "sniper arm64" | 3810310 | sniper; sniper is Debian 11 | aarch64 | phase-out | VERIFIED IN SOURCE: Proton's arm64 builds required it from 2025-11-12 ([b4d640e5](https://github.com/ValveSoftware/Proton/commit/b4d640e5675a3ae8e00424e1b494f92f69371ebe)) to 2026-01-26 ([0a7e25b6](https://github.com/ValveSoftware/Proton/commit/0a7e25b6def85d56707099578990c8e61cf4fa5e)). VERIFIED IN SOURCE: the steam-runtime maintainer's SDL commit [d834351d](https://github.com/libsdl-org/SDL/commit/d834351d69c8567d09c0a036d48f4b6661ab5dc3) (2026-06-02, steamrt/tasks#1032) says "The experimental Steam Linux Runtime 3.0 (arm64) container is being phased out, so games that want native arm64 binaries should upgrade to Steam Linux Runtime 4.0". UPSTREAM DOCUMENTED: sniper is Debian 11 ([goals.md](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/doc/goals.md)). COMMUNITY REFERENCE: umu-launcher removed it on 2026-06-03 (commit d43525c in [umu-launcher](https://github.com/Open-Wine-Components/umu-launcher)). |
| steamrt3c, "Steam client runtime 3" | none; ships with the client | a miniaturised sniper | x86-64 tree at `~/.steam/root/steamrt64/steam-runtime-steamrt`; arm64 images exist (§2) | current, for the Steam client itself | VERIFIED IN SOURCE: [container-runtime.md:199-209](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/container-runtime.md#L199-L209). COMMUNITY REFERENCE: the arm64 images (§2). |
| medic | none | Debian 12 | not stated | discontinued | VERIFIED IN SOURCE: [container-runtime.md:216-230](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/container-runtime.md#L216-L230) |
| steamrt5 | none | Debian testing | not stated | prototype only | VERIFIED IN SOURCE: [container-runtime.md:216-230](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/container-runtime.md#L216-L230) |

Notes:

1. **i386 in SLR 4.0.** Neither Valve's steam-runtime documents nor the
   steam-runtime-tools docs say anything about i386 or 32-bit for steamrt4
   (UNKNOWN). Proton 11's x86-64 build builds `i386-unix`
   ([Makefile.in:77-78](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/Makefile.in#L77-L78)),
   which suggests the runtime provides i386 (HYPOTHESIS). On SteamARM, 32-bit
   Windows probes ran through `SteamLinuxRuntime_4/_v2-entry-point`
   (MEASURED, `benchmarks/stage16-32bit-vulkan.txt:30-35`), and pressure-vessel
   ran its 32-bit `capsule-capture-libs` there (MEASURED,
   `benchmarks/stage15-steam-proton-path.txt:50-57`). That shows 32-bit code
   runs in that container. It does not show where the i386 libraries come
   from.
2. **The Steam Frame developer page.** A search excerpt of Valve's Steamworks
   page "How to load and run games on Steam Frame" tells developers to pick
   "Steam Linux Runtime 3.0 ARM64 (Sniper)" for Linux ARM64 binaries. A search
   excerpt of the steamrt README calls the arm64 runtime experimental, with
   no secondary architecture. Neither page could be fetched (UNKNOWN;
   https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames,
   https://gitlab.steamos.cloud/steamrt/steamrt/-/blob/steamrt/sniper/README.md).
3. **The arm64 client's directory.** The `proton` script adds
   `steamdir + '/steamrtarm64/video/'` to the library path (VERIFIED IN SOURCE,
   [proton:1239](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/proton#L1239)).
   That the arm64 client has a `steamrtarm64/` directory is a COMMUNITY
   OBSERVATION ([TECHNICAL_LOG.md:221](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/docs/TECHNICAL_LOG.md#L221);
   steam-arm64-nix `guest-run.sh`). It is now MEASURED too: stage21 runs
   `steamrtarm64/steam` on the owner's Mac.
4. **On a real Steam Frame.** An x86-64 Windows `.exe` ran under Proton 11
   through FEX (ARM64EC) inside the SLR 4.0 arm64 container. An x86-64 Linux
   ELF mapped to app 4183110 did not start, because the Frame would not
   install that runtime. The devkit client offered `SteamLinuxRuntime_4-arm64`
   only when the device reported itself as Deckard. COMMUNITY OBSERVATION,
   [frame-control sideloading.md:44-66, 160-177](https://github.com/saphid/frame-control/blob/HEAD/docs/sideloading.md).

### 1.2 Which tool requires which runtime

| tool | app ID | requires | label and source |
|---|---|---|---|
| Proton 5.0 and older | - | the scout `LD_LIBRARY_PATH` runtime (no `require_tool_appid`) | UPSTREAM DOCUMENTED ([README.md:65-73](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/README.md#L65-L73)); VERIFIED IN SOURCE (branch `proton_5.0` manifests) |
| Proton 5.13, 6.3, 7.0 | - | soldier, 1391110 | UPSTREAM DOCUMENTED (same README); VERIFIED IN SOURCE (`toolmanifest_runtime.vdf` on each branch of [Proton](https://github.com/ValveSoftware/Proton)) |
| Proton 8.0, 9.0, 10.0 | - | sniper, 1628350 | same as above |
| Proton Experimental 10.0 | - | x86-64 build: 1628350. arm64 build: 4185400. | VERIFIED IN SOURCE (`toolmanifest_{x86_64,arm64}.vdf`, branch `experimental_10.0`) |
| Proton 11.0, Experimental 11.0, bleeding-edge | - | x86-64 build: 4183110. arm64 build: 4185400. | UPSTREAM DOCUMENTED ("Proton 11 or newer uses the Steam Runtime 4 container", README); VERIFIED IN SOURCE (branch manifests; [toolmanifest_arm64.vdf](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/toolmanifest_arm64.vdf), `compatmanager_layer_name proton`) |
| Proton 11.0 (ARM64). PROPRIETARY_DO_NOT_REDISTRIBUTE. | 4628740 (depot 4628741) | 4185400 | COMMUNITY OBSERVATION, with a qualification: the stock client did not list the tool. The project wrote its own `compatibilitytools.d/steam-arm64-official/compatibilitytool.vdf` declaring these app and depot IDs, and only then did Steam register it ([TECHNICAL_LOG.md:238-294](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/docs/TECHNICAL_LOG.md#L238-L294)). |
| FEX, Valve's compatibility tool | 3127680 (depot 3127681 guest rootfs, 0 bytes on public; depot 3127682 FEX and thunks, `FEX-2607-76-g37265b1`) | runs outside the container and sets `STEAM_COMPAT_EMULATOR` | COMMUNITY OBSERVATION ([Nova-Deck fex-compat-tool plan, lines 19-125, 225-400](https://raw.githubusercontent.com/Nova-Deck/os-build/HEAD/.claude/plans/fex-compat-tool.plan.md)). The tool files themselves are VERIFIED IN SOURCE upstream ([FEX Source/Steam](https://github.com/FEX-Emu/FEX/tree/0df84d3844bcdb87bb7d3f5b8fb0959cd009c038/Source/Steam)). |

More on Valve's FEX tool (COMMUNITY OBSERVATION, same Nova-Deck plan, and the
[steam-arm64-nix README](https://github.com/Mr-Banana-Egg/steam-arm64-nix/blob/5d236a5a13d9ee34e743beedb3755db3e56677ca/README.md)):

- The depot has no `FEXInterpreter` and no `binfmt.d`. Its `fex-compat-tool`
  is a python3 script. It sets `STEAM_COMPAT_MACHINE_ARCHITECTURE=aarch64-linux-gnu`
  and `STEAM_COMPAT_EMULATOR=<emulator.json>`.
- The x86 rootfs is expected from the OS (`/usr/share/guestos/fex-mesa`) or
  from `dirname(STEAM_COMPAT_GRAPHICS_PROVIDER)`.
- On 2026-08-05 the client did not select the tool on an ordinary account
  (reported as account-gated). pressure-vessel then fell back to binfmt with
  `FEX_ROOTFS=/run/pressure-vessel/interpreter-root`.
- By September 2026 a NixOS packager reports that the native arm64 client
  sends x86 Linux games and x86 Proton through this tool, with
  `STEAM_COMPAT_GRAPHICS_PROVIDER=/run/fex-emu/rootfs/graphics_provider.json`.
  The two reports conflict on whether the tool is gated.

The Proton README says of ARM64 builds: "It's not possible to use the
resulting builds in x86 Steam running via FEX." (UPSTREAM DOCUMENTED,
[README.md:192-196](https://github.com/ValveSoftware/Proton/blob/proton_11.0/README.md#L192-L196)).

## 2. The steamrt4 apt repository and arm64 images

Only versions that a source actually showed are listed.

| item | what was shown | label and source |
|---|---|---|
| apt repository | `deb https://repo.steampowered.com/steamrt4/apt steamrt4 main contrib non-free` | UPSTREAM DOCUMENTED ([steam-runtime README.md](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/README.md), apt section) |
| architectures in the apt repository | Valve's GitHub documents do not list them. A Debian wiki search snippet shows `[arch=amd64,i386,arm64]`; the page was not read. | UNKNOWN |
| image directory | https://repo.steampowered.com/steamrt4/images/, with `SteamLinuxRuntime_4.tar.xz` | UPSTREAM DOCUMENTED ([reporting-steamlinuxruntime-bugs.md:244-252](https://github.com/ValveSoftware/steam-runtime/blob/8cb6d88c6c4467e76a1176d4815f6dac0eb9cd24/doc/reporting-steamlinuxruntime-bugs.md#L244-L252)) |
| arm64 image | `steamrt4/images/<version>/SteamLinuxRuntime_4-arm64.tar.xz`, version from `latest-public-beta.txt`, checked against `SHA256SUMS`. umu maps 4185400 to `steamrt4-arm64` and 4183110 to `steamrt4`. | COMMUNITY REFERENCE ([umu_runtime.py:68-120, 270-290, 520-527](https://github.com/Open-Wine-Components/umu-launcher/blob/e2b203a1fdd2af9f35166f5713cb3f85d72587e2/umu/umu_runtime.py)) |
| arm64 image version seen | `4.0.20260608.242786`, from the endpoint `latest-public-beta/`, platform directory `steamrt4_platform_4.0.20260608.242786`. `pv-verify` from it failed on x86-64, so it is an aarch64 ELF. | COMMUNITY OBSERVATION ([faugus-launcher#761](https://github.com/Faugus/faugus-launcher/issues/761), [proton-ge-custom#569](https://github.com/GloriousEggroll/proton-ge-custom/issues/569)) |
| steamrt4 arm64 SDK image | `registry.gitlab.steamos.cloud/steamrt/steamrt4/sdk/arm64:4.0.20251117.183306` (FEX CI) and `:latest` (SDL CI) | VERIFIED IN SOURCE ([FEX steamrt4.yml](https://github.com/FEX-Emu/FEX/blob/0df84d3844bcdb87bb7d3f5b8fb0959cd009c038/.github/workflows/steamrt4.yml), [SDL create-test-plan.py](https://github.com/libsdl-org/SDL/blob/main/.github/workflows/create-test-plan.py)) |
| Proton 11 build images | `registry.gitlab.steamos.cloud/proton/steamrt4/sdk/x86_64:4.0.20260331.220802-0` and `.../arm64-llvm:4.0.20260331.220802-2` | VERIFIED IN SOURCE ([Makefile.in:29-33](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/Makefile.in#L29-L33)) |
| sysroot tarball name | `com.valvesoftware.SteamRuntime.Sdk-arm64-steamrt4-sysroot.tar.gz` would follow the depot tool's naming, whose architecture table includes arm64 | HYPOTHESIS. The template is VERIFIED IN SOURCE ([populate-depot.py](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/subprojects/container-runtime/populate-depot.py)); the directory listing was not read. |
| pressure-vessel in SLR 4.0 arm64 | 0.20260714.0, installed in August 2026 | COMMUNITY OBSERVATION ([TECHNICAL_LOG.md:1049](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/docs/TECHNICAL_LOG.md#L1049)) |
| pressure-vessel that SteamARM ran | x86-64 `pressure-vessel-wrap --version` 0.20260728.0; `srt-bwrap --version` "bubblewrap 0.12.0"; `steam-runtime-system-info` 0.20260728.0 (2026-09-26). Which depot they came from is not recorded. | MEASURED (`benchmarks/stage7-guest-base.txt:313-317`) |
| steamrt3c arm64 client runtime | `https://repo.steampowered.com/steamrt3c/images/<3c.0.YYYYMMDD.N>/steam-runtime-steamrt-arm64.tar.xz` and `com.valvesoftware.SteamRuntime.Platform-arm64-steamrt3c-runtime.tar.gz`, indexed by `latest-public-beta.txt` and `latest-public-stable.txt`. The only image version seen is `3c.0.20260824.257593`. The arm64 client manifests are `steam_client_linuxarm64` and `steam_client_publicbeta_linuxarm64`. | MEASURED for `steam_client_linuxarm64`, which stage21 fetched on the owner's Mac. The rest: COMMUNITY REFERENCE ([runtime-source.nix](https://github.com/Mr-Banana-Egg/steam-arm64-nix/blob/5d236a5a13d9ee34e743beedb3755db3e56677ca/runtime-source.nix), [scripts/update.sh:103-107](https://github.com/Mr-Banana-Egg/steam-arm64-nix/blob/5d236a5a13d9ee34e743beedb3755db3e56677ca/scripts/update.sh#L103-L107)) |
| `3.0.20260618.246540` | This is an Arch package version, not a steamrt3c image version. The PKGBUILD reads the real version from `latest-public-stable.txt` at build time and accepts only `^3c\.`. | COMMUNITY REFERENCE ([PKGBUILD lines 2, 13-14, 29-34](https://github.com/silime/ArchLinux-Packages/blob/HEAD/steam-arm64/PKGBUILD)) |

## 3. pressure-vessel on arm64

### 3.1 Builds and depot layout

Relocatable builds (VERIFIED IN SOURCE,
[.gitlab-ci.yml:403-439](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/.gitlab-ci.yml#L403-L439),
[build-relocatable-install.py:90-113](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/build-relocatable-install.py#L90-L113)):

| archive | architectures | built in |
|---|---|---|
| `pressure-vessel-bin.tar.gz` | amd64 + i386 | scout |
| `pressure-vessel-arm64.tar.gz` | arm64 | `steamrt/steamrt3c/sdk/arm64:beta`, on aarch64 CI |
| `pressure-vessel-arm64+amd64+i386.tar.gz` | arm64 + amd64 + i386, "suitable for use with CPU emulation" | same |

Each holds `bin/` (`pressure-vessel-unruntime`, `pressure-vessel-wrap`,
`pv-verify`, `steam-runtime-*`) and `libexec/steam-runtime-tools-0/`
(`pv-adverb`, `pv-locale-gen`, `pv-try-setlocale`, `srt-bwrap`, `srt-logger`,
`launch-options.py`).

Changelog steps toward arm64 (VERIFIED IN SOURCE,
[debian/changelog](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/debian/changelog)):

| version | change |
|---|---|
| 0.20251103.0 | experimental option to run foreign-architecture programs under a non-transparent emulator such as FEX, without binfmt_misc (steamrt/tasks#787) |
| 0.20251120.0 | `STEAM_COMPAT_EMULATOR` (set by Steam) accepted as `PRESSURE_VESSEL_EMULATOR`; graphics-provider JSON manifests |
| 0.20251201.0 | experimental `pressure-vessel-arm64+amd64+i386.tar.gz` |
| 0.20251210.0 | `server_argv` in `emulator.json`, so FEXServer is shared |
| 0.20260312.0 | host GBM backends appended when pressure-vessel runs inside FEX, for thunks |

Depot layout, from the upstream template (VERIFIED IN SOURCE,
[populate-depot.py](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/subprojects/container-runtime/populate-depot.py)
lines 75-160, 337-360, 808-885, 898-962):

- `_v2-entry-point`, a POSIX sh script. It parses `--verb`, cleans
  `LD_LIBRARY_PATH` and execs `./run`.
- `run` and `run-in-<suite>`. They export `PRESSURE_VESSEL_ARCHITECTURES`,
  `PRESSURE_VESSEL_COPY_RUNTIME=1`, `PRESSURE_VESSEL_RUNTIME` and
  `RUNTIME_BASE`, then exec `pressure-vessel/bin/pressure-vessel-unruntime`.
- `<suite>_platform_<version>/files` (the merged `/usr`), mtree manifests,
  `VERSIONS.txt`, `var/`.
- `toolmanifest.vdf`: commandline `/_v2-entry-point --verb=%verb% --`,
  `compatmanager_layer_name container-runtime`, `use_tool_subprocess_reaper 1`,
  `filter_exclusive_priority 4` for steamrt4.
- The steamrt4 depot builder itself lives in `steamrt/steamlinuxruntime` on
  `gitlab.steamos.cloud`, which is not mirrored.

For the arm64 depot specifically (COMMUNITY OBSERVATION,
[TECHNICAL_LOG.md:220-300, 484-490, 1049](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/docs/TECHNICAL_LOG.md)):

- `run` exports `PRESSURE_VESSEL_ARCHITECTURES=aarch64-linux-gnu`.
- The pressure-vessel in it is aarch64, version 0.20260714.0 in August 2026.
- Steam's command prefix for 4185400 was
  `.../steamapps/common/SteamLinuxRuntime_4-arm64/_v2-entry-point --verb=run --`.
  This was after the project registered the tool itself (§1.2).
- The x86 `SteamLinuxRuntime_4` depot has a `pressure-vessel-arm64/`
  directory. Its `pressure-vessel-wrap` matched the arm64 depot's byte for
  byte.

### 3.2 Two ways to run x86 code

| mode | pressure-vessel build | how x86 code runs | label and source |
|---|---|---|---|
| Interpreter-root ("transparent") | x86-64 or i386, running under FEX | FEX runs pressure-vessel itself. pressure-vessel detects FEX (§3.3) and builds an interpreter-root container (§3.4). x86 execs inside the container still need something to hand them to FEX: binfmt_misc on Linux, `lxrun`'s own hand-off on SteamARM (`runtime/main.c:387-420`). | VERIFIED IN SOURCE ([virtualization.c](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/steam-runtime-tools/virtualization.c), [runtime.c:3401-3463](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/runtime.c#L3401-L3463)) |
| Emulator ("non-transparent"), 0.20251103.0 and later | aarch64 | `STEAM_COMPAT_EMULATOR` or `PRESSURE_VESSEL_EMULATOR` names an `emulator.json`. pressure-vessel prepends the emulator to foreign-architecture commands, shares the emulator's directory, provides aarch64 `ld.so` and libraries in the container, starts the emulator's server through `server_argv`, and takes x86 glibc and drivers from a graphics provider. binfmt_misc is not needed. | VERIFIED IN SOURCE ([steam-runtime-emulator.json.5.md](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/steam-runtime-emulator.json.5.md), [CONTRIBUTING.md:108-165](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/CONTRIBUTING.md#L108-L165)) |
| Neither: Proton ARM64 | - | The whole process is aarch64. Proton bundles a pinned FEX as Wine-internal DLLs (`aarch64-windows`, `arm64ec-windows`) plus an `aarch64-unix` unixlib. It does not use Linux FEX, binfmt_misc or `emulator.json`. | VERIFIED IN SOURCE ([Makefile.in:848-890](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/Makefile.in#L848-L890), [proton](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/proton) lines 549-556, 1646-1680, 2036-2048) |

Details of the emulator mode (all VERIFIED IN SOURCE unless marked):

- `emulator_v0` fields: `argv` (required; must be an ELF, not a script),
  `container_argv`, `main_argv`, `environment`, `container_environment`,
  `emulated_architectures` (required), `required_architectures`,
  `required_libraries`, `server_argv`. The server writes `READY=1\n` to
  stdout, closes it, and exits when its stdin reaches EOF. Programs of an
  architecture not listed run directly. The manifest's parent directory is
  shared into the container automatically. The exec chain stays
  reaper → `SteamLinuxRuntime_*/_v2-entry-point` → game
  ([steam-runtime-emulator.json.5.md](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/steam-runtime-emulator.json.5.md)).
- `STEAM_COMPAT_EMULATOR` "should be set by Steam when using CPU emulation
  ... in conjunction with a container runtime, instead of adding the emulator
  to the exec chain". It stays unset for a native aarch64 game.
  `STEAM_COMPAT_GRAPHICS_PROVIDER` points at x86-64 and i386 `ld.so`, glibc
  and drivers when the real root is aarch64 only
  ([steam-compat-tool-interface.md:296-376](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/steam-compat-tool-interface.md#L296-L376)).
- `graphics_provider_v0`: per-architecture `dri`, `gbm`, `gconv`,
  `fallback_library_paths`, `va_api`, `vdpau`, and a `root` that must be a
  sysroot-like merged-`/usr` tree with `etc/ld.so.cache`, `sbin/ldconfig`,
  the `ld.so`, the drivers and their dependencies down to glibc, and relative
  symlinks
  ([steam-runtime-graphics-provider.json.5.md](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/steam-runtime-graphics-provider.json.5.md)).
- `PRESSURE_VESSEL_EMULATOR` is read before `STEAM_COMPAT_EMULATOR`.
  `PRESSURE_VESSEL_GRAPHICS_PROVIDER` wins over
  `STEAM_COMPAT_GRAPHICS_PROVIDER`. `PRESSURE_VESSEL_ARCHITECTURES` is an
  experimental `:`-separated list; the first entry is primary
  ([wrap-context.c:751-754, 1179-1182](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap-context.c#L751-L754)).
- In the emulated design, an aarch64 pressure-vessel and an aarch64 FEX
  launch a runtime whose primary architecture is x86-64 with foreign i386.
  The container adds aarch64 as a foreign architecture
  ([CONTRIBUTING.md:108-165](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/CONTRIBUTING.md#L108-L165)).
- FEX's own `emulator.json`: `argv` and `container_argv` `./usr/bin/FEX`,
  `environment` `FEX_PORTABLE=1`, `container_environment` `FEX_ROOTFS=''`,
  `main_argv` `./FEXCompatTool`, `server_argv` `./usr/bin/FEXServerManager`,
  emulating x86-64 and i386, requiring aarch64 `libc.so.6` and
  `libstdc++.so.6`. `FEXServerManager` implements the `READY=1` protocol
  ([FEX Source/Steam](https://github.com/FEX-Emu/FEX/tree/0df84d3844bcdb87bb7d3f5b8fb0959cd009c038/Source/Steam),
  `CompatTool.cpp:19-73`, `ServerManager.cpp:21-81`).
- FEX rootfs images ship `/graphics_provider.json` at their root (COMMUNITY
  OBSERVATION,
  [steam-arm64-nix README](https://github.com/Mr-Banana-Egg/steam-arm64-nix/blob/5d236a5a13d9ee34e743beedb3755db3e56677ca/README.md)).

### 3.3 How pressure-vessel detects FEX

The research first said this mode needs FEX started through binfmt_misc. The
reviewer refuted that. Corrected:

- pressure-vessel detects FEX by the CPUID leaf 0x40000000 signature
  `FEXIFEXIEMU`, and the host machine by leaf 0x40000001. Nothing in the
  detection depends on binfmt_misc. It applies to any x86-64 or i386
  pressure-vessel under FEX, whether FEX was started by binfmt_misc or
  invoked explicitly. VERIFIED IN SOURCE,
  [virtualization.c:236-256, 451-492](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/steam-runtime-tools/virtualization.c#L236-L256).
- It finds FEX's rootfs by `opendir("/.")` and reading
  `/proc/self/fd/<n>`. This works because FEX special-cases `/` but not
  `/.`. If the result is exactly `/`, no interpreter root is used.
  VERIFIED IN SOURCE,
  [virtualization.c:451-492](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/steam-runtime-tools/virtualization.c#L451-L492),
  [wrap-setup.c:602-627](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap-setup.c#L602-L627).
- This code exists only under `#if` x86-64 or i386. An aarch64
  pressure-vessel has no FEX detection and no interpreter-root mode.
  VERIFIED IN SOURCE (same files, 0.20260320.0).
- On SteamARM, FEX is started explicitly by `lxrun`, with no binfmt_misc,
  and pressure-vessel detects it there. Hiding the leaves broke it (MEASURED,
  `benchmarks/stage15-steam-proton-path.txt:77-83`; §4.2). The Steam client
  reads the same leaves to decide the host is arm64 (MEASURED, same file,
  lines 24-32).

### 3.4 Interpreter-root mode

VERIFIED IN SOURCE,
[runtime.c:3401-3463](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/runtime.c#L3401-L3463),
[wrap.c:515-540, 608-609](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L515-L540),
`runtime.h:44` (`PV_RUNTIME_FLAGS_INTERPRETER_ROOT`):

| what | where in the container |
|---|---|
| the real root's `/usr` (on an arm64 Linux host: the aarch64 `/usr`) | the container's `/usr` |
| `/etc/ld.so.cache`, `ld.so.conf(.d)`, alternatives | symlinks into `/run/interpreter-host` |
| the runtime | `/run/pressure-vessel/interpreter-root` |
| `FEX_ROOTFS` | set by `--setenv FEX_ROOTFS /run/pressure-vessel/interpreter-root` |
| FEX rootfs's `/usr` and `/etc` | `/run/host` |
| the real root | `/run/interpreter-host` |

The source carries the comment "TODO: Generalize this to other
interpreters/emulators".

To see the real root past FEX's path rewriting, pressure-vessel opens
`/proc/self/root` and resolves host paths component by component with
`openat(O_PATH|O_NOFOLLOW)` from that descriptor. VERIFIED IN SOURCE,
[resolve-in-sysroot.c:101-113, 153-156, 377](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/steam-runtime-tools/resolve-in-sysroot.c#L101-L113),
[wrap.c:136-144](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L136-L144).

On SteamARM that real root is `LXRT_ROOT`: `lxrun` answers `/proc/self/root`
with it (VERIFIED IN SOURCE, `runtime/procfs.c:573`). The Steam root is an
x86-64 tree used as `/`, plus FEX's aarch64 side in `/usr/lib/lxrt-emu`
(VERIFIED IN SOURCE, `scripts/mksteamroot.sh:1-19`). So the container's
`/usr` here is the x86-64 tree's, not an aarch64 one as on an arm64 Linux
host (HYPOTHESIS, from reading the two together; not traced). The script's
comment gives the reason for that layout: with the aarch64 tree as `/`,
pressure-vessel found no x86 graphics stack through its directory
descriptors. That comment is not backed by a benchmark
(`docs/CURRENT_STEAM_ENVIRONMENT.md` §3).

### 3.5 What pressure-vessel needs from bwrap and the kernel

VERIFIED IN SOURCE unless marked:

- **Choosing bwrap.** `$PRESSURE_VESSEL_BWRAP` first, and then no other
  candidate is tried. Otherwise the bundled
  `libexec/steam-runtime-tools-0/srt-bwrap`, then a system or setuid
  `bwrap`/`flatpak-bwrap`. Each candidate is test-run as
  `<bwrap> --bind / / true`, then probed with `--level-prefix` and with
  `--perms 0700 --dir /`. Its `st_mode` is checked for `S_ISUID`. The final
  plan goes through `--args <sealed memfd or tmpfile>`
  ([bwrap.c:81-140, 163-293](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/steam-runtime-tools/bwrap.c#L81-L140),
  [flatpak-bwrap.c:360-413](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/flatpak-bwrap.c#L360-L413),
  [wrap.c:1093](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L1093)).
- **Namespaces.** pressure-vessel never calls `unshare` itself; bwrap does
  all of it. It adds `--unshare-pid` only when `PRESSURE_VESSEL_SHARE_PID=0`,
  and warns that unsharing is "not expected to work". It never unshares IPC:
  "We must not unshare the IPC namespace", because the client uses SysV
  shared memory and semaphores. It adds `--new-session` unless `--tty` or
  `--devel` is in effect. It uses `--dev-bind /dev`, `--proc /proc` and
  `--ro-bind /sys` (`--bind` with `--devel`). No FUSE and no seccomp are in
  its path. Inside Flatpak 1.12 or later it uses a Flatpak subsandbox
  instead of bwrap
  ([wrap.c:496-512, 748-760](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L496-L512),
  [pressure-vessel.md:139-144](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/pressure-vessel.md#L139-L144),
  [distro-assumptions.md:76-94](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/docs/distro-assumptions.md#L76-L94)).
- **What the plan uses.** A root directory, recursive and file binds,
  `--symlink`, `--dir`, `--ro-bind-data`/`--file` from memfds, `--proc`,
  `--dev-bind`, `--ro-bind /sys`, `--remount-ro`, `--setenv`, `--chdir`,
  `--lock-file`, `--args` and `--new-session`. Not needed by default: PID,
  IPC or network namespaces, seccomp, capabilities, FUSE, overlayfs. Only
  the interpreter-root path relies on x86 execs being routed to FEX.
- **Newer builds.** `--not-a-security-boundary` is an upstream bubblewrap
  flag, first released in 0.12.0 on 2026-08-26. It makes setup failures such
  as remounting a submount non-fatal. It is absent from the 0.20260320.0
  source
  ([bubblewrap NEWS.md](https://github.com/containers/bubblewrap/blob/main/NEWS.md)).
  A captured SLR 4.0 arm64 plan from pressure-vessel 0.20260714.0 has it
  (COMMUNITY OBSERVATION, below), and so does SteamARM's own capture of
  2026-09-23 (MEASURED, `benchmarks/stage6-bwrap-plan.txt:15, 22`).
- **A captured SLR 4.0 arm64 plan** (pressure-vessel 0.20260714.0): 864
  bwrap arguments, passed through `--args <fd>`. They include
  `--not-a-security-boundary`, `--new-session`, `--dev-bind /dev /dev`,
  `--proc /proc`, `--ro-bind-data` from memfds (`etc-passwd`, `etc-group`,
  timezone, `asound.conf`, `container-manager`), `--remount-ro`, `--tmpfs`,
  and many `--symlink`, `--ro-bind`, `--bind` and `--dir`. There is no
  `--unshare-*`. The payload is `pv-adverb --prefix=/run/pressure-vessel/pv-from-host
  --generate-locales --regenerate-ld.so-cache ... -- proton waitforexitandrun`.
  The two `--ro-bind-fd` entries in the capture were added by that project's
  own bwrap wrapper, not by pressure-vessel. COMMUNITY OBSERVATION,
  [tombraider-pressure-vessel-plan-20260817.json](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/docs/evidence/tombraider-pressure-vessel-plan-20260817.json),
  [pressure-vessel-route-bwrap.c:1201-1258](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/diagnostics/pressure-vessel-route-bwrap.c#L1201-L1258).
- **Per-process needs.** `/proc/self/root` must open as a directory;
  `openat(O_PATH)` resolution must work; `d_type` must be consistent; hard
  links and OFD locks are used.

### 3.6 The Termux log line: EACCES, not EPERM

A community project ran the native SLR 4.0 arm64 pressure-vessel on Android
(Termux), which has no usable namespaces. The research first said it failed
at `opendir(/proc/self/root)` with EPERM. The reviewer refuted that. The log
reads `opendir(/proc/self/root): Permission denied`, which is
`strerror(EACCES)`. Corrected record (COMMUNITY OBSERVATION,
[TECHNICAL_LOG.md:4583, 4645-4655, 1519](https://github.com/huntergdavis/steamclienttermux/blob/dee92b8432424e873011542e6926b12651bc823c/docs/TECHNICAL_LOG.md#L4583)):

| step | result |
|---|---|
| `opendir(/proc/self/root)` | "Permission denied" (EACCES) |
| creating a user namespace | EINVAL |
| creating a mount namespace | EPERM |
| bwrap | failed at `/proc/sys/kernel/overflowuid` |
| workaround | bwrap routed through PRoot with `PRESSURE_VESSEL_BWRAP=.../steam-arm64-bwrap-route` |
| then | `pv-adverb: wait: Function not implemented`; 12,320 "Invalid cross-device link" warnings from the runtime copy's hard links |
| crash | SIGSEGV in `pv_runtime_remove_overridden_libraries()`: `getdents64` reported `DT_LNK`, but `readlink` returned EINVAL (PRoot's link2symlink) |

For SteamARM the lesson is about the kernel surface, not namespaces:
`/proc/self/root`, `wait`, hard links and `d_type` consistency. §5.2 lists
where `lxrun` stands on each.

## 4. SteamARM today

### 4.1 The x86 pressure-vessel under FEX

- The whole Steam client runs as x86 code under FEX, and pressure-vessel with
  it (MEASURED, `benchmarks/stage8-steam-zero-vm.txt:4-22`;
  `docs/CURRENT_STEAM_ENVIRONMENT.md` §1).
- The pressure-vessel tools are x86-64 `ET_EXEC` images. They run through
  FEX's 64-bit "low window" (MEASURED,
  `benchmarks/stage7-guest-base.txt:276-320`).
- No bwrap runs. `lxrun` catches any exec whose basename is `bwrap` or
  `srt-bwrap`, including FEX re-executing itself into an x86 bwrap, and
  interprets the plan in `runtime/mounts.c` (VERIFIED IN SOURCE,
  `runtime/process.c:86-107`). The plan becomes a fresh sandbox directory,
  used as the child's `LXRT_ROOT`, plus a bind table carried to children in
  `LXRT_MOUNTS` (`runtime/mounts.h:1-13`).
- An x86 or i386 ELF exec'd inside the container is handed to
  `/usr/lib/lxrt-emu/FEX`, as binfmt_misc would do on Linux (VERIFIED IN
  SOURCE, `runtime/main.c:387-420`; MEASURED,
  `benchmarks/stage8-steam-zero-vm.txt:99-101`).
- `steam-runtime-check-requirements` passes by actually running its bwrap
  probe (MEASURED, `benchmarks/stage8-steam-zero-vm.txt:105-106`).
  pressure-vessel launches steamwebhelper in its container (MEASURED, same
  file, lines 236-238).
- Steam's Windows launch path works up to the rendered window:
  `SteamLinuxRuntime_4/_v2-entry-point --verb=waitforexitandrun -- proton
  waitforexitandrun <exe>`, with D3D probes at about 160 fps, 64-bit and
  32-bit (MEASURED, `benchmarks/stage15-steam-proton-path.txt:8-20`,
  `benchmarks/stage16-32bit-vulkan.txt:30-35`). This is the x86 SLR 4.0,
  app 4183110. Real games are not verified (`README.md`).
- pressure-vessel under FEX is slow. It took 142 s to reach the webhelper
  before a `/proc/self/fd` fix and 27 s after (MEASURED,
  `benchmarks/stage8-steam-zero-vm.txt:121-122, 220-225`).
- stage21 re-checked this route after its runtime changes: the x86 Steam
  login window came up 85 s after start, and `tests/win` passed 11/11
  (MEASURED, `benchmarks/stage21-native-arm64-client.txt`).

### 4.2 FEX_ROOTFS and interpreter-root mode: what SteamARM measured

The research concluded that `FEX_ROOTFS=/` makes pressure-vessel's `/.`
probe return `/`, so interpreter-root mode stays off, and that `FEX_ROOTFS`
should stay `/`. SteamARM's own record says the opposite.

| what | evidence | label |
|---|---|---|
| The Steam client's environment has `FEX_ROOTFS=/`, and the Steam root is an x86-64 tree used as `/`. (`docs/CURRENT_STEAM_ENVIRONMENT.md:120` shows this variable.) | `scripts/run-app.sh:170, 354`; `scripts/run-steam.sh:92`; `scripts/mksteamroot.sh:1-10` | VERIFIED IN SOURCE |
| FEXServer starts with `FEX_ROOTFS=/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04` and `LXRT_ROOT=/tmp/lxrt-root`, whoever starts it. Since the merge of main (`dfab6e2`), `FEX_SERVER_ROOTFS` and `FEX_SERVER_LXRT_ROOT` can override the two. The comments reserve that for an isolated root, such as a Holo Core smoke test. | `scripts/run-fex.sh:38-41, 51-59, 81-85` | VERIFIED IN SOURCE |
| One FEXServer serves every FEX client of the user and hands each client its own rootfs path. So the client's `FEX_ROOTFS=/` is not what pressure-vessel sees; FEXServer's rootfs is. | `scripts/run-fex.sh:53-59`; `benchmarks/stage8-steam-zero-vm.txt:321-325` | MEASURED |
| Started from a Steam run with `FEX_ROOTFS=/`, FEXServer told pressure-vessel the interpreter root was `/`. pressure-vessel set up no `/run/pressure-vessel/interpreter-root`. FEX found the container's empty `proc/` first, and every steamwebhelper zygote died: `FATAL thread_helpers.cc: fstatat(/proc, "self/task/") ENOENT`. | `benchmarks/stage8-steam-zero-vm.txt:321-325`; `scripts/run-fex.sh:55-58` | MEASURED |
| With FEX's CPUID leaves hidden from every program, pressure-vessel did not detect FEX, built no interpreter root, and the zygotes died the same way. The leaves are now hidden from the `steam` executable only (`AppConfig/steam.json`). After that the Steam UI came up. | `benchmarks/stage15-steam-proton-path.txt:77-83`; `scripts/install-steamroot-gfx.sh:100-105` | MEASURED |
| Inside the container, pressure-vessel binds the real `resolv.conf` over the FEX rootfs image's empty one at `/run/pressure-vessel/interpreter-root/etc/resolv.conf`. FEX resolves guest paths relative to its rootfs descriptor. Lookups relative to a directory descriptor ignored the binds, so every Chromium request failed with `ERR_NAME_NOT_RESOLVED`. They now go through the bind table (`at_through_mounts`: `openat`, `openat2`, `fstatat`, `statx`, `faccessat(2)`, `readlinkat`). | `benchmarks/stage8-steam-zero-vm.txt:326-332`; `runtime/dispatch.c:303-309` | MEASURED |

**The measured behaviour.** SteamARM's working Steam session runs the x86
pressure-vessel in interpreter-root mode, and needs it. What pressure-vessel
takes as FEX's rootfs is the rootfs FEXServer serves, not the client's
`FEX_ROOTFS=/`. Keep FEXServer's rootfs at a real directory, never `/`
(`FEX_SERVER_ROOTFS` included), and keep FEX's CPUID leaves visible to
pressure-vessel. The research
implication is dropped. Only its reading of pressure-vessel stands: a rootfs
of exactly `/` turns the mode off (VERIFIED IN SOURCE, §3.3), which is the
failure SteamARM measured.

### 4.3 bwrap options: pressure-vessel against `runtime/mounts.c`

The right-hand column was read in `runtime/mounts.c` at commit `1911825`
(https://github.com/LukeOkk/SteamARM/blob/1911825d4238e9d055e9402d9253614756ef7bea/runtime/mounts.c).
Numbers in brackets are its lines. The middle column says whether
pressure-vessel emits the option, with the evidence.

| bwrap option (arguments) | does pressure-vessel emit it? | `runtime/mounts.c`: VERIFIED IN SOURCE |
|---|---|---|
| `--args FD` (1) | Yes: the whole plan (VERIFIED IN SOURCE, [wrap.c:1093](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L1093)). Also in the arm64 capture (COMMUNITY OBSERVATION, §3.5). | Implemented. Reads the fd's NUL-separated arguments and splices them in place [328-344]. |
| `--bind`, `--ro-bind`, `--dev-bind` (2) | Yes. MEASURED in the VM-era x86 plan: `--ro-bind` x19, `--bind` x18, `--dev-bind` x1 (`benchmarks/stage6-bwrap-plan.txt:38-41`). VERIFIED IN SOURCE: `--dev-bind /dev /dev`, `--ro-bind /sys /sys` ([wrap.c:496-512](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L496-L512)). | Implemented as bind-table entries; the longest destination wins. `--ro-*` binds make writes fail with EROFS [54-67]. `/proc`, `/sys` and `/dev` bound onto themselves are skipped, since the runtime serves those trees itself. A missing source fails with ENOENT [345-376]. |
| `--bind-try`, `--ro-bind-try`, `--dev-bind-try` (2) | Yes: `--ro-bind-try /gnu/store`, `/nix/store` (MEASURED, `stage6-bwrap-plan.txt:33-34`). | Implemented. A missing source is skipped [364]. |
| `--symlink` (2) | Yes (MEASURED, `stage6-bwrap-plan.txt:27-31, 89-92`; COMMUNITY OBSERVATION in the arm64 capture). | Implemented: a real symlink in the sandbox directory [377-386]. |
| `--dir` (1), `--tmpfs` (1) | Yes (MEASURED: `--perms 0700 --dir /` in the probe, `--tmpfs` x2 in the plan, `stage6-bwrap-plan.txt:24-25, 41`). | A directory in the sandbox directory, mode from `--perms` or 0755 [387-394]. `--tmpfs` is not a separate filesystem. |
| `--proc` (1) | Yes: `--proc /proc` (VERIFIED IN SOURCE, wrap.c:496-512; COMMUNITY OBSERVATION in the arm64 capture). | An empty directory; `procfs.c` synthesises `/proc` [395-403]. |
| `--dev` (1) | Not in the evidence. | Binds the host `/dev` [400]. |
| `--ro-bind-data`, `--bind-data`, `--file` (2) | `--ro-bind-data`: yes, from memfds (MEASURED, `stage6-bwrap-plan.txt:41, 69, 75-76`; COMMUNITY OBSERVATION in the arm64 capture). `--file`: yes (VERIFIED IN SOURCE, §3.5). `--bind-data`: not in the evidence. | Copies the fd's bytes into a file in the sandbox directory, mode from `--perms` or 0644 [404-417]. The file is not entered as read-only, so an `--ro-bind-data` file can be written. |
| `--setenv` (2), `--unsetenv` (1) | `--setenv`: yes, for example `FEX_ROOTFS` in interpreter-root mode (VERIFIED IN SOURCE, [wrap.c:515-540](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L515-L540)). `--unsetenv`: not in the evidence. | Applied to the child's environment [418-435]. |
| `--chdir` (1) | Yes (VERIFIED IN SOURCE, §3.5). | Applied through the child's view [436, 506-513]. |
| `--perms` (1) | Yes, in the probe (MEASURED, `stage6-bwrap-plan.txt:24`; VERIFIED IN SOURCE, §3.5). | Sets the mode of the next `--dir`, `--tmpfs` or data file [437]. |
| `--chmod OCTAL PATH` (2) | Not in the 0.20260320.0 source (VERIFIED IN SOURCE). | Two arguments since commit `1911825` (2026-09-29). It changes the mode of what the plan made in the sandbox, never a bind's host source [438-448]. Before that commit it took one argument, so `PATH` was misparsed. The new case has not run under `lxrun` yet (UNKNOWN). |
| `--lock-file` (1), `--remount-ro` (1) | Yes (VERIFIED IN SOURCE, §3.5; `--remount-ro` also in the arm64 capture, COMMUNITY OBSERVATION). | Accepted with no effect [449-455]. No lock is taken. A `--remount-ro` path stays writable unless a `--ro-bind` covers it. |
| `--sync-fd`, `--info-fd`, `--json-status-fd`, `--block-fd`, `--userns-block-fd`, `--exec-label`, `--file-label`, `--hostname`, `--argv0` (1 each) | Not in the evidence. | Accepted with no effect [449-455]. Nothing is written to the status fds. |
| `--new-session`, `--level-prefix` (0) | Yes. `--new-session` unless `--tty` or `--devel` (VERIFIED IN SOURCE, [wrap.c:748-760](https://github.com/llyyr/steam-runtime-tools/blob/2691ca4da05a1979debfc4b4d2cc614831cabd50/pressure-vessel/wrap.c#L748-L760)). `--level-prefix` in the probe (MEASURED, `stage6-bwrap-plan.txt:23`). | Accepted with no effect [456-472]. `--new-session` does not call `setsid`. |
| `--not-a-security-boundary` (0) | Yes, in recent builds (MEASURED, `stage6-bwrap-plan.txt:15, 22`; COMMUNITY OBSERVATION, 0.20260714.0 arm64 capture). Not in the 0.20260320.0 source. | Accepted [456-472]. |
| `--die-with-parent`, `--as-pid-1`, `--clearenv`, `--disable-userns`, `--assert-userns-disabled` (0) | Not in the evidence. | Accepted with no effect [456-472]. `--clearenv` leaves the environment as it is. |
| `--unshare-pid` (0) | Only with `PRESSURE_VESSEL_SHARE_PID=0`, which pressure-vessel warns is not expected to work (VERIFIED IN SOURCE, wrap.c:496-512). No `--unshare-*` in the measured plan (MEASURED, `stage6-bwrap-plan.txt:7, 14, 40`) or in the arm64 capture (COMMUNITY OBSERVATION). | Refused with ENOSYS and a message, as is every `--unshare-*`, including `--unshare-all` [461-468]. |
| `--cap-add`, `--cap-drop`, `--uid`, `--gid`, `--seccomp`, `--add-seccomp-fd` (1 each) | No. No capabilities or seccomp are in pressure-vessel's path (VERIFIED IN SOURCE, §3.5). | Refused with ENOSYS [461-468]. |
| `--bind-fd FD DEST`, `--ro-bind-fd FD DEST` (2) | Not by pressure-vessel in the evidence. The two in the arm64 capture were added by that project's bwrap wrapper (COMMUNITY OBSERVATION). The options first shipped in bubblewrap v0.10.0 (VERIFIED IN SOURCE, commit [a253257](https://github.com/containers/bubblewrap/commit/a253257), 2024-06-18). | No case: "unknown option", EINVAL, and the whole exec fails [473-474]. |
| `--size` (1) | Not in the evidence. | No case: EINVAL [473-474]. |
| any other option | - | EINVAL [473-474]. |

Around the options (VERIFIED IN SOURCE unless marked):

- **pressure-vessel's probes.** `<bwrap> --bind / / true`, `--level-prefix`
  and `--perms 0700 --dir /` all have cases. `true` is looked up in `PATH`,
  under `FEX_ROOTFS` too [514-552]. MEASURED: pressure-vessel accepts the
  interpreter and builds the steamwebhelper container
  (`benchmarks/stage8-steam-zero-vm.txt:105-106, 236-238`).
- **`--version`** prints `bubblewrap 0.10.0 (lxrt interpreter)` and exits 0
  [319-326]. It claims 0.10.0 but lacks 0.10.0's `--bind-fd` and
  `--ro-bind-fd`. The real `srt-bwrap` SteamARM ran reports 0.12.0
  (MEASURED, `benchmarks/stage7-guest-base.txt:316`).
- **Capacity.** The table holds 256 binds; more are dropped, with a message
  on the trace stream [20, 36-41]. The arm64 capture has 864 arguments. How
  many of them are binds is not recorded here (UNKNOWN).
- **Emulator binds.** After every plan, `mounts.c` adds read-only binds of
  `FEX_ROOTFS` (skipped when it is `/`), `$HOME/.fex-emu` and
  `/usr/lib/lxrt-emu`, when they exist [283-312, 479-487].
- **Tests.** `tests/elf/bwrap_test.c` runs one plan through the real bwrap
  on Linux and through `mounts.c` under the runtime. The record says 11/11
  on both before `--chmod` was added (MEASURED,
  `benchmarks/stage6-steam-gap.txt:105-114`). `tests/elf/run.sh` now expects
  12/12 (`tests/elf/run.sh:444-458`). stage21's `tests/elf` 37/37 on the Mac
  ran on main's tree, whose test still expected 11/11 (MEASURED; VERIFIED IN
  SOURCE at `9e7bc2b`). So 12/12 has not run on a Mac yet.

### 4.4 The ARM64 tools, as SteamARM has seen them

- When the x86 client under FEX saw FEX's CPUID leaves, it called the
  machine arm64 and installed Proton (ARM64), Steam Linux Runtime 4.0 -
  Arm64 and Valve's FEX (MEASURED,
  `benchmarks/stage15-steam-proton-path.txt:24-32`). A later commit,
  `06cbc41`, records that with the leaves visible the x86 client registered
  no ARM64 Proton, and only Valve's native arm64 client lists them. Both are
  in the record (`docs/CURRENT_STEAM_ENVIRONMENT.md` §1).
- SLR 4.0 - Arm64 has not been run under `lxrun` (UNKNOWN;
  `docs/CURRENT_STEAM_ENVIRONMENT.md` §5.3). The Mac's audit of 2026-09-28
  found it installed in the x86 client's library, with Proton 11 ARM64 and
  Experimental ARM64 (MEASURED, `docs/CURRENT_STEAM_ENVIRONMENT.md`,
  appendix). `scripts/install-arm-proton-tools.sh` clones the three into the
  native client's library (VERIFIED IN SOURCE,
  `scripts/install-arm-proton-tools.sh:35-39`). stage21 records no run of
  them.
- Proton 11.0 (ARM64), PROPRIETARY_DO_NOT_REDISTRIBUTE: `wine cmd` run
  natively under `lxrun` could not reserve 0x10000-0x68000000 and
  0x7f000000-0x7fff0000, and exited in 2 s (MEASURED,
  `benchmarks/stage18-settings-audio-controllers.txt:135-149`). macOS keeps
  the low 4 GiB of every arm64 process unmapped, and unmodified Wine maps
  `user_shared_data` at 0x7ffe0000 and exits if it cannot (VERIFIED IN
  SOURCE, `benchmarks/stage19-steamframe-base-and-arm64-limits.txt` §2). A
  probe whose `__PAGEZERO` was shrunk at link time was still killed at
  0x7ffe0000 (MEASURED, stage18 follow-up of 2026-09-28). SteamARM's
  launcher refuses ARM64 Proton for Windows apps (VERIFIED IN SOURCE,
  `scripts/proton-command.py:37-38`).

## 5. What this means for SteamARM

### 5.1 Implications

1. **Interpreter-root mode is load-bearing today.** Keep FEXServer's rootfs
   at the Ubuntu 24.04 directory, never `/`, and do not point
   `FEX_SERVER_ROOTFS` at `/` either. Keep the CPUID leaves visible
   to everything but `steam`. (MEASURED, §4.2.)
2. **SLR 4.0 arm64 ships pressure-vessel.** `docs/CURRENT_STEAM_ENVIRONMENT.md`
   §5.1 held this as a HYPOTHESIS. It is now a COMMUNITY OBSERVATION: the
   depot carries an aarch64 pressure-vessel 0.20260714.0 (§3.1). The
   aarch64 relocatable build is VERIFIED IN SOURCE.
3. **The arm64 plan uses only options `mounts.c` has a case for.** The
   community capture shows no `--unshare-*`, no `--chmod` and no `--size`
   (COMMUNITY OBSERVATION). HYPOTHESIS: `mounts.c` would run it, if it fits
   in 256 binds.
4. **A native aarch64 pressure-vessel never enters interpreter-root mode.**
   Its FEX code is compiled out (VERIFIED IN SOURCE, §3.3). HYPOTHESIS:
   SteamARM's interpreter-root fixes (`runtime/dispatch.c:303-309`, the
   FEXServer rootfs pinning) would not come into play for it. x86 payloads
   would go through `emulator.json` instead.
5. **The ARM64 client probably needs a native pressure-vessel before any
   game.** The x86 client starts its own UI through pressure-vessel
   (MEASURED, `benchmarks/stage6-steam-gap.txt:57-58`). HYPOTHESIS: the
   ARM64 client does the same with the steamrt3c arm64 runtime. stage21
   started the native client's UI process tree in a root where
   `steam-runtime-launcher-service` was missing (MEASURED). Whether
   pressure-vessel ran there is not recorded (UNKNOWN).
6. **SteamARM could keep its own FEX under an ARM64 client.**
   `PRESSURE_VESSEL_EMULATOR` wins over the client's `STEAM_COMPAT_EMULATOR`
   (VERIFIED IN SOURCE). Valve's FEX tool (3127680) lacks SteamARM's
   guest-base, x18 and W^X patches, so it would probably fail under `lxrun`
   (HYPOTHESIS). Valve's tool is a python3 script run outside the container
   (COMMUNITY OBSERVATION): SteamARM needs python3 in its aarch64 root, or
   its own compatibility-tool shim.
7. **Native pressure-vessel could be faster.** Container setup under FEX
   took 27 s to the webhelper (MEASURED, §4.1). HYPOTHESIS: native setup is
   faster; not measured.
8. **SLR 4.0 arm64 does nothing for Windows games here.** Proton ARM64
   embeds FEX inside aarch64 Wine (VERIFIED IN SOURCE), and unmodified Wine
   needs 0x7ffe0000, which macOS cannot map (MEASURED and VERIFIED IN
   SOURCE, `stage18`, `stage19` §2). Windows games stay on x86 Proton under
   FEX, inside the x86 SLR 4.0.
9. **Page size: handled for the client, open for pressure-vessel.** A
   packager reports that Valve's aarch64 client crashes with SIGSEGV on a
   16K-page Asahi kernel and runs in a 4K-page guest (COMMUNITY OBSERVATION,
   [steam-arm64-nix README](https://github.com/Mr-Banana-Egg/steam-arm64-nix/blob/5d236a5a13d9ee34e743beedb3755db3e56677ca/README.md)).
   `lxrun` publishes `AT_PAGESZ` 16384 by default, and 4096 with
   `LXRT_GUEST_PAGE=4096` (VERIFIED IN SOURCE, `runtime/stack.c:21-34, 134`,
   `runtime/lxrt.h:19`). Under 16384, glibc refused the client's 4 KiB-aligned
   `steamui.so`; under 4096 the client loaded `steamui.so` and
   `steamclient.so` (MEASURED, stage21). Whether pressure-vessel needs the
   same: UNKNOWN.

### 5.2 The native arm64 pressure-vessel under lxrun, without FEX

What running SLR 4.0 arm64's own pressure-vessel as a native aarch64 program
under `lxrun` would require, first for aarch64 payloads (no FEX at all),
then for x86 payloads.

| # | requirement | where SteamARM stands | label |
|---|---|---|---|
| 1 | Steam installs app 4185400, or the x86 SLR 4.0 carries `pressure-vessel-arm64/`. SteamARM must not ship either; Steam downloads them on the user's Mac. | The x86 client installed SLR 4.0 - Arm64 once, when it saw FEX's leaves (§4.4). The `pressure-vessel-arm64/` copy is reported by the community (§3.1). | MEASURED; COMMUNITY OBSERVATION |
| 2 | An aarch64 root with `/bin/sh`: `_v2-entry-point` is a POSIX sh script, and `run` execs `pressure-vessel-unruntime` (§3.1). | The Fedora aarch64 root `/tmp/lxrt-root` exists. `scripts/mkarmroot.sh` builds a second Fedora 43 aarch64 root, with bash, for Valve's native client at `$STEAMARM_STATE/armroot` (VERIFIED IN SOURCE); stage21 ran the client in it (MEASURED). The Frame-derived ARM64 base behind `/tmp/lxrt-arm64root` is still built by no script (`docs/CURRENT_STEAM_ENVIRONMENT.md` §3). `scripts/run-native.sh` runs `lxrun` with no FEX and unsets `FEX_ROOTFS` (`scripts/run-native.sh:29`). It defaults `LXRT_ROOT` to `/tmp/lxrt-arm64root` (`scripts/run-native.sh:19`). | VERIFIED IN SOURCE; MEASURED |
| 3 | Loadable ELF files. `lxrun` refuses an aarch64 `ET_EXEC` (`runtime/elf.c:113-122`), and a PT_LOAD whose `p_align` is not a multiple of 4 KiB (`runtime/elf.c:147-157`). Since stage21, an executable or `ld.so` aligned to 4 KiB but not 16 KiB loads, with its protections kept per 4 KiB by `subpage.c` (`runtime/elf.c:209-225`). | The x86 pressure-vessel tools are all `ET_EXEC` (MEASURED, `stage7-guest-base.txt:276-283`). The aarch64 builds come from the steamrt3c arm64 SDK (VERIFIED IN SOURCE, §3.1). Whether they, and the steamrt4 arm64 `ld.so`, are PIE is UNKNOWN. HYPOTHESIS: PIE, as Debian-based toolchains default to it. If they are `ET_EXEC`, this route is closed. Alignment is less of a risk now: Valve's 4 KiB-aligned native client loaded this way (MEASURED, stage21). | VERIFIED IN SOURCE; MEASURED; UNKNOWN |
| 4 | x18. aarch64 Linux code uses x18 as an ordinary register, and Darwin zeroes it on every exception return. | MEASURED (`benchmarks/stage5-x18.txt`). `lxrun` rewrites x18 uses in the aarch64 code it loads. Since stage21 it touches only words inside the function ranges of a file's `.eh_frame`, when the file has one. A file whose path contains the value of `LXRT_X18_ALL_TEXT` gets the whole-text pass (VERIFIED IN SOURCE, `runtime/elfsect.c:129-139, 184-205, 290-298`; `runtime/rewrite.c:324-331`). stage21 needed the filter for the client's static OpenSSL, and x18 rewriting over all of libcef's text, which uses x18 outside its FDEs (MEASURED). HYPOTHESIS: pressure-vessel needs nothing new, unless it too uses x18 outside its FDEs. | MEASURED; VERIFIED IN SOURCE; HYPOTHESIS |
| 5 | Page size. | `AT_PAGESZ` is 16384, or 4096 with `LXRT_GUEST_PAGE=4096` (§5.1 item 9). The native client needed 4096 (MEASURED, stage21). pressure-vessel's needs: UNKNOWN. | VERIFIED IN SOURCE; MEASURED; UNKNOWN |
| 6 | bwrap. pressure-vessel test-runs its bundled aarch64 `srt-bwrap`. | `lxrun` catches an exec by the basename `bwrap` or `srt-bwrap`, whatever the architecture (`runtime/process.c:86-91`). `PRESSURE_VESSEL_BWRAP` can name the candidate, but it must still end in `bwrap` or `srt-bwrap` for `lxrun` to catch it. HYPOTHESIS: the aarch64 plan reaches `mounts.c` unchanged. | VERIFIED IN SOURCE; HYPOTHESIS |
| 7 | The plan. | Every option in the arm64 capture has a case (§4.3). Gaps: the 256-bind limit (UNKNOWN whether the plan exceeds it); `--remount-ro` and `--ro-bind-data` are not read-only (VERIFIED IN SOURCE; HYPOTHESIS: harmless). | as marked |
| 8 | No interpreter-root mode (§3.3). | The emulator binds in `mounts.c` are skipped when their directories do not exist (`runtime/mounts.c:299-311`), so a no-FEX root gets none. | VERIFIED IN SOURCE |
| 9 | The host view: `/proc/self/root` as a directory, walked with `openat(O_PATH\|O_NOFOLLOW)`. | `lxrun` answers `/proc/self/root` with `LXRT_ROOT` (`runtime/procfs.c:573`) and implements Linux `O_PATH` semantics (MEASURED for the x86 pressure-vessel under FEX, `stage7-guest-base.txt:312`, `stage8-steam-zero-vm.txt:107-108`). HYPOTHESIS: the same for a native one, since FEX forwards to the same system calls. | MEASURED; HYPOTHESIS |
| 10 | `d_type` consistent with `readlinkat` (the Termux crash, §3.6); hard links for `PRESSURE_VESSEL_COPY_RUNTIME=1`. | `lxrun` converts Darwin `d_type` values to Linux ones (`runtime/dirents.c:122`). The x86 pressure-vessel builds its runtime copy under `lxrun` (MEASURED, `stage8-steam-zero-vm.txt:114-117`). | VERIFIED IN SOURCE; MEASURED |
| 11 | Locks and process control. pressure-vessel uses OFD locks, and pv-adverb uses `PR_SET_CHILD_SUBREAPER` and `PR_SET_PDEATHSIG` (from the research, without line references: HYPOTHESIS). | `lxrun` maps OFD locks onto Darwin process locks (`runtime/fsflags.c:153-159`). `prctl` options without a Darwin meaning, including those two, return 0 and do nothing (`runtime/dispatch.c:2626-2668`). The x86 pv-adverb already runs with this (MEASURED, `stage15-steam-proton-path.txt:72-75`). HYPOTHESIS: an orphaned grandchild is not reparented to pv-adverb, unlike on Linux. | VERIFIED IN SOURCE; HYPOTHESIS |
| 12 | No namespaces. pressure-vessel leaves them all to bwrap and shares PID and IPC by default (§3.5). | `mounts.c` turns the mount namespace into a per-process path view and refuses `--unshare-*`. Never set `PRESSURE_VESSEL_SHARE_PID=0`. | VERIFIED IN SOURCE |
| 13 | Graphics for an aarch64 payload. pressure-vessel takes drivers from the root it treats as the host. | HYPOTHESIS: SteamARM's `libvulkan.so.1` shim and its ICD JSON in the aarch64 root, with Qualcomm ICDs moved aside (`docs/STEAM_FRAME_IMAGE.md`). An aarch64 program links the shim directly (MEASURED, `benchmarks/stage4-shim.txt`). `scripts/mkarmroot.sh` already installs the shim as that root's `/usr/lib64/libvulkan.so.1` (VERIFIED IN SOURCE, `scripts/mkarmroot.sh:70-71`). | HYPOTHESIS |
| 14 | x86 payloads in that container (FEX comes back here): an `emulator.json` and a graphics provider (§3.2). | HYPOTHESIS: a SteamARM-owned `emulator.json` naming `lxrun`'s patched FEX-emu (an ELF, as required), FEXServer through `server_argv` with the `READY=1` protocol, aarch64 `libc.so.6` and `libstdc++.so.6` in `/`, and `PRESSURE_VESSEL_GRAPHICS_PROVIDER` pointing at the x86 rootfs with the thunks and the shim. FEXServer today is started detached with `--persistent=0` (`scripts/run-fex.sh:76-85`), not as a child that exits on stdin EOF. Whether SteamARM's Ubuntu 24.04 rootfs has `/graphics_provider.json`: UNKNOWN. This inverts `scripts/mksteamroot.sh`: `/` becomes aarch64 and x86 comes from the provider. | HYPOTHESIS |

### 5.3 Next steps

1. Record, in every Steam benchmark, the rootfs FEXServer serves and the
   pressure-vessel version from the runtime's `VERSIONS.txt`. The plan's
   shape changes between releases.
2. On the Mac, run `tests/elf/run.sh`. It now expects 12/12 from the bwrap
   test, which covers the `--chmod` fix under `lxrun` for the first time.
   stage21's 37/37 predates the fix (§4.3).
3. Add `--bind-fd` and `--ro-bind-fd` to `mounts.c` (the fd's path through
   `F_GETPATH`, as `at_through_mounts` already does), and accept `--size`.
   Add a case for each to `tests/elf/bwrap_test.c`. Decide whether
   `--version` should keep claiming 0.10.0.
4. On the next Steam run with `LXRT_TRACE=1`, read the `bwrap: <n> mounts`
   line (`runtime/mounts.c:551`) for each plan and compare it with the
   256-bind limit.
5. SLR 4.0 - Arm64 is installed in the x86 client's library (MEASURED,
   §4.4). Check its `pressure-vessel-wrap`, `pv-adverb`, `srt-bwrap` and the
   platform's `ld-linux-aarch64.so.1` with `llvm-readelf -h -l`: `ET_DYN` or
   `ET_EXEC`, and each `p_align` (a multiple of 4 KiB is enough now, §5.2
   row 3). Read them in place; do not copy them out of the Steam install.
6. If they load: run that `pressure-vessel-wrap --version` through
   `scripts/run-native.sh`, with `LXRT_ROOT` set to the root that
   `scripts/mkarmroot.sh` builds, and `LXRT_GUEST_PAGE=4096` if the files
   are 4 KiB-aligned. Then run the depot's
   `_v2-entry-point --verb=run -- true` with `LXRT_TRACE=1`, and compare its
   plan with §4.3.
7. From the Mac, read `https://repo.steampowered.com/steamrt4/apt/dists/steamrt4/Release`
   (its `Architectures:` field) and the `steamrt4/images/` listing. Record
   the results in this file with a date.
8. Only after steps 5 and 6 pass, try the `emulator.json` route (§5.2 row
   14) with one x86 probe.

## 6. Could not be checked from here

- `repo.steampowered.com` (`steamrt4/images`, `steamrt4/apt`,
  `steamrt3c/images`): 403 in the shell, blocked in the fetch tool. File
  names, versions and the apt `Architectures` field were not read directly;
  the only evidence is community code and logs.
- `gitlab.steamos.cloud` (steam-runtime-tools, the steamrt READMEs for
  steamrt4 and sniper, the `steamlinuxruntime` depot builder, the SDK
  READMEs): blocked. There is no ValveSoftware GitHub mirror of
  steam-runtime-tools. The third-party mirror read here is at upstream
  0.20260320.0, older than the shipped arm64 pressure-vessel 0.20260714.0.
- `registry.gitlab.steamos.cloud`: not tested. Image tags come from the FEX,
  SDL and Proton CI files.
- `steamdb.info/app/4185400` and `/app/3127680`: blocked. Depot data came
  only from community write-ups.
- `partner.steamgames.com` (the Steam Frame "load games" and compatibility
  pages, the Linux platform page): blocked; search snippets only.
- `wiki.debian.org` Derivatives/Census/SteamRuntime: blocked. A search
  snippet claims `[arch=amd64,i386,arm64] https://repo.steampowered.com/steamrt4/apt steamrt4 main`;
  the page was not read.
- `blog.drakulix.de` ("Taming the Steam arm64 client"), `gamingonlinux.com`,
  `discourse.libsdl.org`, `interfacinglinux.com`: blocked or not fetched;
  search snippets only.
- GitHub's global search API: blocked for the session (repository-scoped
  search worked). The Nova-Deck `os-build` plan was read through
  `raw.githubusercontent.com`.
- Nothing on this page was run under `lxrun`. Every SteamARM MEASURED fact
  comes from the existing record.
