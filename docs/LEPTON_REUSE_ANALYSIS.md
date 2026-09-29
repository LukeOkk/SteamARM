# Lepton reuse analysis

Status (2026-09-29): this page is the Lepton reuse analysis the plan asks for.
Updated 2026-09-29 after `benchmarks/stage21-native-arm64-client.txt`. Lepton
is Valve's Android compatibility layer for Steam Frame. The page answers two
questions. What can SteamARM reuse from Lepton to run Android apps on a Mac
without a VM? What cannot work on Darwin? The Lepton facts come from web
research by a cloud session. An adversarial reviewer then re-checked every
claim: 30 stood, 9 were refuted and appear here only in corrected form, and 5
were relabelled. The lxrun facts were read in this repository at commit
`4c5efd5`. They were re-checked at `dfab6e2`, the merge of main that brought
stage21: line numbers are corrected, and the ELF loader, page size and x18
rows follow main's runtime changes. The research ran nothing on a Mac; the
MEASURED facts come from the repository's record. stage21 facts are MEASURED
on the owner's Mac (M4, macOS 27). The Lepton app entry, and how the client
offers tools, are in `docs/STEAM_FRAME_COMPAT_TOOLS.md`; this page does not
repeat them. The session could not reach these hosts:

- `gitlab.steamos.cloud` (Valve's upstream for Lepton);
- `partner.steamgames.com`, `store.steampowered.com` and `steamdb.info`;
- the press sites `gamingonlinux.com`, `mixed-news.com`, `videocardz.com`,
  `techspot.com`, `itsfoss.com`, `ghacks.net` and `hwbusters.com`;
- the GitHub API for `ValveSoftware/SteamOS` (access denied).

**Evidence labels:**

| label | meaning |
|---|---|
| MEASURED | observed by SteamARM on a Mac or a CI runner, as recorded in this repo's `benchmarks/` or `docs/` |
| VERIFIED IN SOURCE | read in a source tree; the repo URL and the commit or path are given |
| UPSTREAM DOCUMENTED | Valve or upstream documentation says so |
| OBSERVED EXTERNAL METADATA | SteamDB and similar trackers |
| COMMUNITY OBSERVATION | reported by community projects or users, not confirmed by Valve |
| COMMUNITY REFERENCE | seen only in press or in search snippets |
| HYPOTHESIS | inferred, not tested |
| UNKNOWN | not established |

**Other markers:**

| marker | meaning |
|---|---|
| PROPRIETARY_DO_NOT_REDISTRIBUTE | Valve proprietary files. SteamARM never ships them. |
| reuse as is | the component can be used unchanged |
| port | the logic carries over; the Linux-specific parts must be rewritten for lxrun |
| reference only | read it to learn what is needed; do not use it directly |
| cannot work | it depends on something Darwin and lxrun do not have |

**Where the sources are:**

- Lepton: the GitHub mirror
  https://github.com/xXJSONDeruloXx/lepton at commit
  `6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b`. Every mirror link below points
  at that commit.
- SteamARM: paths in https://github.com/LukeOkk/SteamARM at commit `4c5efd5`,
  with line numbers re-checked at `dfab6e2`.

## 1. What Lepton is and where its source is

### What it is

| fact | label | source |
|---|---|---|
| Lepton is a Steam compatibility tool. It runs Android arm64 apps in a rootless podman container on the host's Linux kernel. | VERIFIED IN SOURCE | [liblepton.sh#L174-L228](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/liblepton.sh#L174-L228) |
| It uses no VM: no qemu, crosvm or AVF. Android `/init` runs directly on the host kernel. | VERIFIED IN SOURCE | [liblepton.sh#L174-L228](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/liblepton.sh#L174-L228) |
| Valve's README says Lepton "incorporates patches and components from the Waydroid, Anbox, Halium and Hybris projects". | VERIFIED IN SOURCE | [README.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/README.md) |
| "Lepton is a fork of Waydroid". This phrase appears in press. A site-restricted search found it on no partner.steamgames.com page. | COMMUNITY REFERENCE | press, for example https://vr.org |
| SteamDB: "Lepton is the official name for Valve's version of Android on Linux compatibility layer, based on Waydroid". The post dates from 2025-12-02, so the name was public by then. | OBSERVED EXTERNAL METADATA | https://x.com/SteamDB/status/1995778245103419671 |
| GamingOnLinux reported the Lepton name from Steam documentation in December 2025. | COMMUNITY REFERENCE | https://www.gamingonlinux.com/2025/12/valves-version-of-android-on-linux-based-on-waydroid-is-now-called-lepton/ |
| Press dates the open-sourcing to about 2026-09-17 to 2026-09-20. | COMMUNITY REFERENCE | https://9to5google.com/2026/09/18/valves-open-source-lepton-brings-android-games-to-the-1059-steam-frame/ |
| How many Frame titles run through Lepton is disputed. One outlet says 52 of 130 "Great on Frame" titles; another says 52 of 120 featured titles. Both were seen in search snippets only. | COMMUNITY REFERENCE | https://vr.org/articles/steam-frame-lepton-android-runtime-52-of-130-certified-2026 ; https://www.technology.org/2026/09/17/valve-steamos-fex-lepton-compatibility-layers-steam-frame/ |
| On the Frame, Lepton runs Android 11 (API 30), 64-bit ARM only, as podman containers named `lepton-steamlaunch-<id>`. | COMMUNITY OBSERVATION | https://github.com/saphid/frame-control/blob/main/docs/how-the-frame-works.md (as recorded in `docs/STEAM_FRAME_COMPAT_TOOLS.md` §1) |
| Lepton is Steam app 3029110. | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/issues/58 ; https://github.com/valvesoftware/steam-for-linux/issues/13634 ; a search listing titled "Lepton" for https://store.steampowered.com/news/app/3029110 |

### Where the source is

| fact | label | source |
|---|---|---|
| The upstream is Valve's GitLab, `gitlab.steamos.cloud/frame-public/lepton`. It returned EGRESS_BLOCKED from the shell and from server-side WebFetch. It was not read. | UNKNOWN (upstream contents) | https://gitlab.steamos.cloud/frame-public/lepton |
| Only a GitHub mirror could be read. Its description is "Mirror of https://gitlab.steamos.cloud/frame-public/lepton". | VERIFIED IN SOURCE | https://github.com/xXJSONDeruloXx/lepton |
| Pinned commit: `6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b`, dated 2026-09-16. | VERIFIED IN SOURCE | https://github.com/xXJSONDeruloXx/lepton/commits/main |
| `git describe --tags 6135b53` gives `v3.0.1-11-g6135b53`: 11 commits past v3.0.1, 8 of them non-merge. | VERIFIED IN SOURCE | https://github.com/xXJSONDeruloXx/lepton/commits/main |
| Public history starts at the parentless root commit `4a592d2`, "Release Lepton v3.0.0", dated 2026-09-11. Tag v3.0.1 is `552b188` (2026-09-12). | VERIFIED IN SOURCE | https://github.com/xXJSONDeruloXx/lepton/commits/main |
| The mirror has 18 commits at `6135b53`. A Valve-address author wrote 4. An author with a personal-domain address wrote 14. | VERIFIED IN SOURCE | https://github.com/xXJSONDeruloXx/lepton/commits/main |
| A community fork carries identical SHAs from `4a592d2` to `6135b53`. This shows that two GitHub copies agree. It does not show that they match GitLab. | VERIFIED IN SOURCE | https://github.com/Lolig4/lepton-x86 |
| The same fork carries later upstream-looking commits, up to `f64fb91` (2026-09-17). Whether GitLab has more is UNKNOWN. | VERIFIED IN SOURCE | https://github.com/Lolig4/lepton-x86 |
| Internal history is not public. CI API URLs name the internal project `deckard/lepton`. Merge-commit messages cite merge requests !196 to !203. !202 is not in the mirror; it appears only in the fork's merge commit `b307d5a`. | VERIFIED IN SOURCE | [.gitlab-ci.yml](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/.gitlab-ci.yml) ; https://github.com/Lolig4/lepton-x86/commit/b307d5a |
| Commit `2f89a06` (2026-09-15, merged in `b307d5a`) re-points two `.gitmodules` entries from `gitlab.steamos.cloud/valve/waydroid/*` to `gitlab.steamos.cloud/frame-public/*`: `android_hardware_waydroid`, and `android_device_valve_waydroid`, renamed `android_device_waydroid_waydroid`. It also moves manifest entries: `hardware/waydroid`, and `alsa-lib` from `valve/waydroid/valvedroid` to `frame-public/valvedroid`. | VERIFIED IN SOURCE | https://github.com/Lolig4/lepton-x86/commit/2f89a06 |
| The `android_vendor_waydroid` submodule already pointed at github.com/waydroid. | VERIFIED IN SOURCE | https://github.com/Lolig4/lepton-x86/commit/2f89a06 |
| `android-vulkan-headers` is not a submodule. Commit `9882438` switches the vulkan-headers manifest project from `frame-public/vulkan-headers` to `frame-public/android-vulkan-headers`. | VERIFIED IN SOURCE | https://github.com/Lolig4/lepton-x86/commit/9882438 |

### Repository layout

VERIFIED IN SOURCE:
[tree at 6135b53](https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b),
[compat_tool/README.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/README.md).

| path | what it holds |
|---|---|
| `README.md`, `LICENSE.md`, `LICENSES/compat_tool.md`, `LICENSES/image.md` | description and licences |
| `.gitlab-ci.yml`, `.ci_scripts/` | build and prepare the rootfs (API 30 and 34), sysbake, tests, deploy |
| `compat_tool/lepton` | the bash entry point |
| `compat_tool/toolmanifest.vdf` | the Steam compat tool manifest |
| `compat_tool/liblepton/*.sh` | mounting, networking, properties, baking, vulkan_layers, gdb, perfetto, strace, app_metadata |
| `compat_tool/liblepton/lepton.seccomp.json` | the seccomp profile |
| `compat_tool/liblepton/openvrpaths.vrpath` | OpenVR paths |
| `compat_tool/liblepton/apk_extractor` | the Rust crate `apk-info-extractor` |
| `compat_tool/images/rootfs_overlay` | init `.rc` files, a `/system/bin/cmd` wrapper, the OpenXR runtime JSON |
| `image/` | Android 11 build from LineageOS 18.1, about 101 patches |
| `image-14/` | Android 14 build from AOSP: `android_device_valve_lepton`, `android_vendor_valve` (manifests-34 and base-patches-34, 121 patches), `builder_image/Containerfile`, `.buildscripts` |
| `tests/` | tests |

The prebuilt rootfs is not stored in the repo.

### What is public

VERIFIED IN SOURCE:
https://github.com/xXJSONDeruloXx/lepton ;
[compat_tool/README.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/README.md) ;
https://github.com/Lolig4/lepton-x86/commit/2f89a06.

Public:

- the full compat tool: bash, the Rust APK parser, the seccomp profile and the
  rootfs overlay;
- both Android image build recipes, with the device tree, local manifests,
  patches, the builder Containerfile, CI and tests;
- the Waydroid-derived submodules, re-pointed to public `frame-public/*`
  repos in September 2026.

Not public:

- the prebuilt rootfs, sysbake and symbol artifacts (CI and the Steam depot
  only);
- the internal git history, the internal `deckard/*` projects and the CI
  registry images;
- the Frame's `/usr/share/guestos/android`: bionic-built Mesa/Turnip and
  Valve's RPO and FDM Vulkan layers;
- `libsteamclient.so` for androidarm64 and SteamVR's androidarm64
  `vrclient.so`.

## 2. Licences

### What the files say

| file | what it says | label | source |
|---|---|---|---|
| `LICENSE.md` | "Redistribution and use of Lepton ... is governed by a variety of licenses". | VERIFIED IN SOURCE | [LICENSE.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/LICENSE.md) |
| `LICENSES/compat_tool.md` | MIT, "Copyright (c) 2026, Valve Corporation". It covers the top-level project and the compat tool. | VERIFIED IN SOURCE | [LICENSES/compat_tool.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/LICENSES/compat_tool.md) |
| `LICENSES/image.md` | Lists AOSP, Waydroid (Apache-2.0, including Anbox, Halium and Hybris patches), Boringdroid and LineageOS. It says: "The AOSP image, as a product of the combination of these opensource projects, is released under a GPL-3.0 license." Component licences still apply. A `NOTICE.txt` is generated at build time. | VERIFIED IN SOURCE | [LICENSES/image.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/LICENSES/image.md) |
| `README.md` | Still links to `LICENSE.AOSP.image` and `LICENSE.lepton`, which have moved. The links are stale. | VERIFIED IN SOURCE | [README.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/README.md) |

SteamARM's own code is MIT (`LICENSE`, `README.md`).

### What SteamARM may do

| item | SteamARM may | why | label |
|---|---|---|---|
| The compat tool: `lepton`, `liblepton/*.sh`, `toolmanifest.vdf`, `lepton.seccomp.json`, `rootfs_overlay`, `apk_extractor` | copy and modify, keeping Valve's MIT notice | These files sit under `compat_tool/`, which `LICENSES/compat_tool.md` covers. The Rust crate's third-party dependencies keep their own licences: UNKNOWN which. | VERIFIED IN SOURCE (licence text); HYPOTHESIS (coverage of each file) |
| The Android build recipes and patches in `image/` and `image-14/` | reference; copy patches with each component's licence | Each patch keeps the licence of the component it patches. | HYPOTHESIS |
| An Android rootfs that SteamARM builds itself | ship only under GPL-3.0 | Valve declares the combined image GPL-3.0. Corresponding source must then be offered: this repo plus the AOSP, LineageOS and Waydroid trees and patches, with component NOTICEs. | HYPOTHESIS |
| Valve's prebuilt `rootfs.tar.zst`, `sysbake.tar.zst` and `sysbake.xattrs` from the Steam depot | not ship: PROPRIETARY_DO_NOT_REDISTRIBUTE | The licence files do not state the terms of Valve's depot binaries. This repo's policy covers Lepton depot builds: SteamARM may build from source, but not ship Valve's builds (`docs/STEAM_FRAME_COMPAT_TOOLS.md`). | HYPOTHESIS (terms) |
| `androidarm64/libsteamclient.so` and the other `androidarm64/*.so` from the Steam client | not ship: PROPRIETARY_DO_NOT_REDISTRIBUTE | Part of the Steam client. Lepton itself mounts them from the user's own Steam install. | VERIFIED IN SOURCE (how Lepton gets them, [mounting.sh#L540-L551](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L540-L551)) |
| SteamVR's androidarm64 `vrclient.so` | not ship: PROPRIETARY_DO_NOT_REDISTRIBUTE | Proprietary, and not public. | HYPOTHESIS |
| The Frame's `/usr/share/guestos/android`: bionic Mesa/Turnip build, `VALVE_rpo`, `VALVE_fdm_injection`, `lldb-server`, perfetto `tracebox` | not ship: PROPRIETARY_DO_NOT_REDISTRIBUTE | Contents of the Steam Frame image. | HYPOTHESIS |
| Mesa and Turnip source | rebuild | Mesa is MIT. | HYPOTHESIS |

## 3. How it runs

### 3.1 Launch and host tools

| fact | label | source |
|---|---|---|
| The container engine is rootless podman. The launch is `podman run --read-only --init=false --userns=keep-id:uid=0,gid=0 --user 0:0 --group-add keep-groups --rootfs <images/rootfs>:O /init`. | VERIFIED IN SOURCE | [liblepton.sh#L174-L228](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/liblepton.sh#L174-L228) |
| Android init runs as PID 1. | VERIFIED IN SOURCE | same |
| The `lepton` script brings up catatonit through `systemctl --user` (podman.service). It takes a flock on `/tmp/lepton.lock`. | VERIFIED IN SOURCE | [lepton#L36-L63](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/lepton#L36-L63) |
| README: Lepton "runs as a non-root user without requiring any rootful setup or helpers". | VERIFIED IN SOURCE | [README.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/README.md) |
| README: "Every process inside Lepton is running as the same non-root user. There is no enforced isolation between apps, each app instead should run within its own Android container". There is one container per game. | VERIFIED IN SOURCE | same |
| README: it disables Android services games do not need, and caches state to speed up boot. | VERIFIED IN SOURCE | same |
| The source invokes podman, pasta, catatonit and `systemctl --user`. | VERIFIED IN SOURCE | [liblepton.sh#L174-L228](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/liblepton.sh#L174-L228) ; [networking.sh#L83-L137](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/networking.sh#L83-L137) |
| The source never invokes crun, runc or fuse-overlayfs. fuse-overlayfs appears only in comments in `.ci_scripts/build-sysbake*.sh`. Rootless podman may use kernel overlay instead. | VERIFIED IN SOURCE | [.ci_scripts/](https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/.ci_scripts) |
| A community distro reports that Lepton needs podman, crun or runc, pasta and fuse-overlayfs on the host, and does not vendor them. It also needs newuidmap/newgidmap, subuid/subgid, and overlay and FUSE in the kernel. | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/issues/58 |
| Without the `steamvr` binary, Lepton's logging breaks. | COMMUNITY OBSERVATION | https://github.com/Nova-Deck/os-build/issues/58 |

### 3.2 Namespaces, cgroups and capabilities

VERIFIED IN SOURCE:
[mounting.sh#L190-L244](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L190-L244).

| setting | value |
|---|---|
| always | `--pid=private --ipc=private --uts=private` |
| user namespace | `--userns=keep-id:uid=0,gid=0`: the host user appears as uid 0 ([liblepton.sh#L174-L228](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/liblepton.sh#L174-L228)) |
| API 30 | `--cgroupns=private` |
| other SDK versions | `--cgroupns=host`, plus a delegated systemd user slice `lepton-<ctx>.slice`, bind-mounted `rw,U` at `/sys/fs/cgroup` |
| capabilities | `--cap-drop ALL`, then 24 added back: audit_control, sys_nice, wake_alarm, setpcap, setgid, setuid, sys_ptrace, sys_admin, block_suspend, sys_time, net_admin, net_raw, net_bind_service, kill, dac_override, dac_read_search, fsetid, mknod, syslog, chown, sys_resource, fowner, ipc_lock, sys_chroot |
| seccomp | a custom profile (3.3) |

HYPOTHESIS: init needs CAP_SYS_ADMIN, or an equivalent, for its own mounts
inside the container: binderfs, the rbind of layers and app directories, the
`/proc/sys` remount and FUSE. It also needs a cgroup subtree it can write.
Source: [mounting.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh).

### 3.3 Seccomp and the fake uid

| fact | label | source |
|---|---|---|
| The seccomp profile allows everything by default. It lists only `SCMP_ARCH_AARCH64`. | VERIFIED IN SOURCE | [lepton.seccomp.json](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/lepton.seccomp.json) |
| chown, fchown, fchownat, lchown, setuid, setgid, setgroups, setpriority, nice, the clock-setting calls and the key calls return errno 0. They fake success. | VERIFIED IN SOURCE | same |
| `open_by_handle_at` returns ENOSYS (38). Module loading, kexec and reboot are denied. | VERIFIED IN SOURCE | same |
| bionic's getuid, getgid, setuid, setgid, setgroups, setreuid and setregid become userspace functions backed by the `PARENT_UID` and `PARENT_GID` environment variables. The real syscalls are renamed `*_sys`. | VERIFIED IN SOURCE | [bionic 0004-lepton-Fake-uid-implementation.patch](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_vendor_valve/patches/base-patches-34/bionic/0004-lepton-Fake-uid-implementation.patch) |
| The patch comment says direct syscalls and `/proc/<pid>/status` still show 0. | VERIFIED IN SOURCE | same |
| installd fakes `st_uid` and `st_gid`. | VERIFIED IN SOURCE | same |

### 3.4 Filesystem and devices

| fact | label | source |
|---|---|---|
| tmpfs on `/dev`, `/dev/socket`, `/tmp`, `/var`, `/run` and `/waydroid/xdg`; devpts; debugfs and tracefs bind mounts; tmpfs on `/sys/fs/bpf`. | VERIFIED IN SOURCE | [mounting.sh#L425-L501](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L425-L501) |
| Device nodes bind-mounted when present: zero, null, full, fuse, tty, shm, snd, uhid, net/tun, random, urandom, `/dev/dri/renderD128` and card0. | VERIFIED IN SOURCE | same |
| renderD128 is also mounted as `/dev/kgsl-3d0` for the Qualcomm KGSL blob driver. | VERIFIED IN SOURCE | same |
| `/dev/kmsg` goes to `/dev/null` by default. | VERIFIED IN SOURCE | same |
| `/data` is a podman `:O` overlay. The baked system `/data` is the lower layer, with a per-app upperdir and workdir. | VERIFIED IN SOURCE | [mounting.sh#L105-L112](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L105-L112) |
| Host paths mounted at identical paths: `STEAM_COMPAT_CLIENT_INSTALL_PATH`, `STEAM_COMPAT_LIBRARY_PATHS`, `LEPTON_DATA_DIR`, `~/Documents`, `~/Videos` and `~/Downloads`. | VERIFIED IN SOURCE | [mounting.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh) |
| The shader cache (`STEAM_COMPAT_SHADER_PATH`) is not mounted at an identical path. It goes to `/data/shaders`. | VERIFIED IN SOURCE | same |
| The host directory `/usr/share/guestos/android` is bind-mounted into the rootfs file by file, over the same paths. | VERIFIED IN SOURCE | [mounting.sh#L72-L95](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L72-L95) |
| `/proc/sys` is remounted read-write. The host scripts use inotify. | VERIFIED IN SOURCE | [mounting.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh) |
| FUSE is still used. `/dev/fuse` is passed in. MediaProvider's FuseDaemon, which serves `/storage/emulated`, is patched: readlink support and a symlinked `/storage/emulated/0`. vold's BindMount tolerates host bind mounts. | VERIFIED IN SOURCE | [MediaProvider 0001](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_vendor_valve/patches/base-patches-34/packages/providers/MediaProvider/0001-lepton-Make-it-possible-to-have-storage-emulated-0-b.patch) ; [vold 0007](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_vendor_valve/patches/base-patches-34/system/vold/0007-lepton-Make-it-possible-to-have-storage-emulated-0-b.patch) |
| If `/dev/ashmem` is absent, `sys.use_memfd=true` is set and Android uses memfd. A system/core patch lets that property be set before boot. The gralloc "Force use ashmem" patch comes from Waydroid. | VERIFIED IN SOURCE | [properties.sh#L19-L22](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L19-L22) |

### 3.5 Networking

VERIFIED IN SOURCE:
[networking.sh#L83-L137](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/networking.sh#L83-L137).

- Networking uses pasta:
  `--network=pasta:-I eth0,--ipv4-only,--no-ndp,-a <ip>,-g <subnet>.1,--map-gw,--dns-forward ...`.
- Android receives a static eth0 through a generated binary `ipconfig.txt`.
- `--map-gw` is needed, "otherwise steamclient can't connect".
- Sysbake runs with `--network=none`.

### 3.6 Binder and binderfs

| fact | label | source |
|---|---|---|
| The overlay init file `binder.rc` runs `mount -t binder binder /dev/binderfs` on early-init. It then renames `/dev/binderfs/anbox-{binder,hwbinder,vndbinder}` to binder, hwbinder and vndbinder. So binderfs is mounted from inside the rootless container's user namespace. | VERIFIED IN SOURCE | [binder.rc](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/images/rootfs_overlay/system/etc/init/binder.rc) |
| Userspace is stock AOSP libbinder, libhwbinder, servicemanager, hwservicemanager and vndservicemanager. SELinux checks are removed, and TXN_SECURITY_CTX is never set. | VERIFIED IN SOURCE | [base-patches-34](https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_vendor_valve/patches/base-patches-34) |
| All processes share one kernel uid. libbinder's IPCThreadState writes `(getuid() & 0x1ffff) << 8` into `binder_transaction_data.flags`. It reads mCallingUid back from those bits, not from the kernel's `sender_euid`. libhwbinder gets the same patch. | VERIFIED IN SOURCE | [frameworks/native 0013](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_vendor_valve/patches/base-patches-34/frameworks/native/0013-lepton-Send-fake-uid-along-with-binder-calls.patch) |
| CI runners without binderfs build `choff/anbox-modules` `binder_linux.ko` at commit `1434f1e`. They insmod it with `devices=binder,hwbinder,vndbinder`. The comment reads: "(Can be dropped if/when the runners support binderfs)". | VERIFIED IN SOURCE | [.gitlab-ci.yml](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/.gitlab-ci.yml) |
| Sysbake and tests run in the image `registry.gitlab.steamos.cloud/deckard/deckardos/holo/steamos-arm64-vr-sysimg` on aarch64 runners. | VERIFIED IN SOURCE | same |
| The Frame kernel must provide binderfs (CONFIG_ANDROID_BINDER_IPC and CONFIG_ANDROID_BINDERFS), mountable in an unprivileged user namespace. The anbox-* rename suggests its binder device list uses anbox-binder, anbox-hwbinder and anbox-vndbinder. Neither point has been checked. | HYPOTHESIS | [binder.rc](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/images/rootfs_overlay/system/etc/init/binder.rc) |
| A SteamOS issue, reported on a Lenovo Legion Go S, lists binder set with `CONFIG_ANDROID_BINDER_DEVICES=""` in linux-neptune-615 (6.15.11.valve1) and linux-neptune-616 (6.16.12.valve27/28). It lists binder unset in linux-neptune-618 (6.18.50) and linux-neptune-72 (7.2.4/7.2.7). The issue does not mention Lepton or the Frame. | COMMUNITY OBSERVATION | https://github.com/ValveSoftware/SteamOS/issues/2848 |

### 3.7 Kernel dependencies Valve removed

VERIFIED IN SOURCE:
[base-patches-34](https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_vendor_valve/patches/base-patches-34) ;
[ubpf_syscall.h](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/ubpf/ubpf_syscall.h) ;
[properties.sh#L37-L45](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L37-L45).

| removed | how |
|---|---|
| SELinux | removed from init, the property service, servicemanager, hwservicemanager, installd, vold, keystore2 and Connectivity; 18 patches touch SELinux |
| ueventd | off (`ro.cold_boot_done=true`); init's uevent listener is disabled |
| device-mapper, fstab, dm-verity, loop devices | disabled; APEXes are flattened, so no loop devices are needed |
| `/proc/cmdline`, mknod | not required |
| bpf(2) | replaced by a userspace shim, `ubpf_syscall.h`, in libbpf and in Connectivity's bpf wrappers, with state under `/sys/fs/bpf/.ubpf`; bpfloader is disabled (`bpf.progs_loaded=1`) |
| CAP_WAKE_ALARM | init drops it |
| Scudo | disabled (`MALLOC_SVELTE`) |

What the kernel must still provide (VERIFIED IN SOURCE, same sources plus
[mounting.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh)
and
[properties.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh)):

- user, mount, pid, ipc, uts and net namespaces, plus a cgroup namespace or
  cgroup v2 delegation;
- binder IPC with binderfs, mounted from inside the user namespace;
- memfd_create (ashmem is optional);
- FUSE (`/dev/fuse`) for MediaProvider;
- overlayfs in the user namespace, or fuse-overlayfs;
- seccomp-bpf, tmpfs and devpts;
- the DRM render node `/dev/dri/renderD128` and card0 (Adreno msm);
- debugfs/tracefs, `/dev/uhid`, `/dev/net/tun`, `/dev/snd`, a writable
  `/proc/sys`, and the kcmp syscall (for Mesa).

## 4. Graphics, page size and ABI

### 4.1 Graphics

| fact | label | source |
|---|---|---|
| Default mode: `ro.hardware.egl=mesa`, `ro.hardware.vulkan=freedreno` (Turnip), `ro.hardware.gralloc=minigbm_msm`, `mesa.loader.driver.override=zink`, `mesa.libgl.kopper.disable=true`. | VERIFIED IN SOURCE | [properties.sh#L47-L110](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L47-L110) |
| GLES, and SurfaceFlinger itself, run on Zink over Turnip. `service.sf.present_timestamp=0` avoids a deadlock. | VERIFIED IN SOURCE | same |
| `LEPTON_USE_QCOM_DRIVER` mode: ANGLE EGL, `vulkan=adreno`, `gralloc=qti-display`. The zink override and kopper-disable properties are set in this mode too. | VERIFIED IN SOURCE | [properties.sh#L64-L78](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L64-L78) |
| CI software mode: SwiftShader for API 30; ANGLE plus `vulkan.pastel` for API 34. | VERIFIED IN SOURCE | [properties.sh#L47-L110](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L47-L110) |
| The `aosp_lepton_arm64_only` image builds Mesa with the zink gallium driver and libgbm only, for minigbm gralloc. It builds no Mesa Vulkan driver. The base `BoardConfig.mk` sets zink and freedreno, but `lepton_arm64_only/BoardConfig.mk` then empties `BOARD_MESA3D_VULKAN_DRIVERS`, under the comment "We inject vulkan drivers from elsewhere". | VERIFIED IN SOURCE | [lepton_arm64_only/BoardConfig.mk](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/lepton_arm64_only/BoardConfig.mk) |
| So the Turnip ICD that `ro.hardware.vulkan=freedreno` selects must come from the host's `/usr/share/guestos/android`. | VERIFIED IN SOURCE | same ; [properties.sh#L64-L78](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L64-L78) |
| Mesa is built with `-Dallow-kcmp=enabled`. A bionic seccomp patch allows `SYS_kcmp` "for mesa". | VERIFIED IN SOURCE | [BoardConfig.mk](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/BoardConfig.mk) |
| README: host libraries mounted in are "Graphics drivers (mesa, zink, etc...)", "Steam/Steamworks libraries" and "Vulkan layers (foveated rendering injector, renderpass optimizer)". | VERIFIED IN SOURCE | [README.md](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/README.md) |
| The Vulkan layers come from `/usr/share/guestos/android/vendor/vulkan_layers`: `libVkLayer_VALVE_rpo.so`, `libVkLayer_VALVE_fdm_injection.so`, fossilize, khronos_validation, gfxreconstruct and the RenderDoc GLES layer. The same tree provides `lldb-server` and the perfetto `tracebox`. The contents are not in the repo. | VERIFIED IN SOURCE | [mounting.sh#L72-L95](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L72-L95) ; [vulkan_layers.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/vulkan_layers.sh) |
| Android's Vulkan loader does not read layer manifests. Lepton rbind-mounts each enabled layer `.so` into every installed app's `lib/arm64` directory. It does so through a hooked `/system/bin/cmd` wrapper and a `cmd lepton mount_vulkan_layers` call. | VERIFIED IN SOURCE | [rootfs_overlay/system/bin/cmd](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/images/rootfs_overlay/system/bin/cmd) ; [vulkan_layers.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/vulkan_layers.sh) |
| The same wrapper grants every requested permission after install ("no need for a permissions model"). | VERIFIED IN SOURCE | [rootfs_overlay/system/bin/cmd](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/images/rootfs_overlay/system/bin/cmd) |

### 4.2 Display, audio and VR

| fact | label | source |
|---|---|---|
| Display goes through Waydroid's hwcomposer to the host Wayland socket `${XDG_RUNTIME_DIR}/gamescope-0`, mounted as `/waydroid/xdg/wayland-0`. | VERIFIED IN SOURCE | [mounting.sh#L568-L571](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L568-L571) |
| The flat window is hidden by default (`lepton.headless=true`) and shown on request. | VERIFIED IN SOURCE | [properties.sh#L123-L137](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L123-L137) |
| Audio uses the host PulseAudio native socket. | VERIFIED IN SOURCE | [mounting.sh#L568-L571](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L568-L571) |
| The OpenXR runtime JSON points at `/data/steamvr/runtime/bin/androidarm64/vrclient.so`. That path is bind-mounted from the host's `steamvr path`. | VERIFIED IN SOURCE | [active_runtime.json](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/images/rootfs_overlay/vendor/etc/openxr/1/active_runtime.json) |
| `VR_PATHREG_OVERRIDE`, `VR_CONFIG_PATH` and `VR_LOG_PATH` are set for zygote in `generate_zygote_launch_rc`. | VERIFIED IN SOURCE | [mounting.sh, around L380-L386](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L380-L386) |

### 4.3 Host Mesa inside bionic

| fact | label | source |
|---|---|---|
| On a non-Frame distro, `/usr/share/guestos/android` is empty. | COMMUNITY OBSERVATION | https://github.com/shuuri-labs/pocknix-os/issues/82 |
| Host glibc Mesa cannot be dlopened into a bionic process because of Initial-Exec TLS. bionic's linker fails on a "TLS symbol ... in dlopened ... using IE access model". | VERIFIED IN SOURCE | https://raw.githubusercontent.com/aosp-mirror/platform_bionic/android14-release/linker/linker_relocate.cpp |
| bionic does support GNU symbol versioning (`VersionTracker`, verneed and verdef). Symbol versioning is not the obstacle. | VERIFIED IN SOURCE | https://raw.githubusercontent.com/aosp-mirror/platform_bionic/android14-release/linker/linker.cpp |
| glibc `DT_NEEDED` and symbol dependencies (`libc.so.6`, `ld-linux`) are a second obstacle. | HYPOTHESIS | |
| So the guestos tree must hold Mesa/Turnip built against bionic or the NDK, or a libhybris-style bridge. | COMMUNITY OBSERVATION | https://github.com/shuuri-labs/pocknix-os/issues/82 |

### 4.4 Page size

| fact | label | source |
|---|---|---|
| No 16 KiB page setting appears in Lepton's BoardConfig files. | VERIFIED IN SOURCE | [BoardConfig.mk](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/BoardConfig.mk) |
| The API 34 image targets 4 KiB pages. In AOSP `android14-qpr3-release`, `build/core/config.mk` defaults `TARGET_MAX_PAGE_SIZE_SUPPORTED` to 4096. It becomes 65536 on arm64 only when `VSR_VENDOR_API_LEVEL >= 34`. | VERIFIED IN SOURCE | https://raw.githubusercontent.com/aosp-mirror/platform_build/android14-qpr3-release/core/config.mk |
| Lepton's `device.mk` inherits `product_launched_with_p.mk` (`PRODUCT_SHIPPING_API_LEVEL` 28), so the VSR level is min(28, 34) = 28. The result is 4096-aligned ELF, and a bionic `PAGE_SIZE` macro (`TARGET_NO_BIONIC_PAGE_SIZE_MACRO=false`). | VERIFIED IN SOURCE | [device.mk](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/device.mk) |
| Many Quest-targeted APK `.so` files have 4 KiB segment alignment. | HYPOTHESIS | |

### 4.5 ABI and Android base

| fact | label | source |
|---|---|---|
| `image-14` runs `repo init -u https://android.googlesource.com/platform/manifest -b android-14.0.0_r75` and lunches `aosp_lepton_arm64_only-ap2a-userdebug` (API 34). | VERIFIED IN SOURCE | [update_repositories.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/.buildscripts/update_repositories.sh) ; [build_image.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/.buildscripts/build_image.sh) |
| Local manifests add Waydroid's lineage-18.1 mesa, libdrm and audio repos, and LineageOS lineage-21.0 QCOM display repos. | VERIFIED IN SOURCE | same |
| `platform/prebuilts/runtime` is removed, so ART is built from AOSP 14 source, as a flattened `com.android.art`. | VERIFIED IN SOURCE | same |
| The legacy `image/` builds LineageOS lineage-18.1 (Android 11, API 30). | VERIFIED IN SOURCE | [image/.buildscripts/update_repositories.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image/.buildscripts/update_repositories.sh) |
| CI builds and tests both rootfs variants (`build-rootfs-job` and `build-rootfs-14-job`, each with sysbake and test jobs). The compat tool branches on `android_sdk_version == 30`. | VERIFIED IN SOURCE | [.gitlab-ci.yml](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/.gitlab-ci.yml) |
| Recent commits include "Fix documentsui for android 14" and "Embed the sdk version into app metadata to make sure we re-bake on android version change". | VERIFIED IN SOURCE | https://github.com/xXJSONDeruloXx/lepton/commits/main |
| Which variant Steam ships is not stated in the source. | UNKNOWN | |
| A community project reports that the Frame runs Android 11 (API 30). | COMMUNITY OBSERVATION | https://github.com/saphid/frame-control/blob/main/docs/how-the-frame-works.md |
| The ABI is arm64 only and 64-bit only. `lepton_arm64_only` inherits `core_64_bit_only.mk`, sets `TARGET_ARCH` arm64 with variant armv8-2a and CPU variant cortex-a76, and leaves every `TARGET_2ND_*` empty. | VERIFIED IN SOURCE | [lepton_arm64_only/BoardConfig.mk](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/lepton_arm64_only/BoardConfig.mk) ; [aosp_lepton_arm64_only.mk](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/lepton_arm64_only/aosp_lepton_arm64_only.mk) |
| There is no native bridge: no houdini, ndk_translation or berberis. FEX is not used inside Lepton. | VERIFIED IN SOURCE | [tree](https://github.com/xXJSONDeruloXx/lepton/tree/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b) |
| The CI test-APK build rewrites the ABI filter list to `arm64-v8a` only. x86, x86_64 and armeabi-v7a native libraries are not supported. Java-only APKs work whatever their ABI. | VERIFIED IN SOURCE | [build-test-apk.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/.ci_scripts/build-test-apk.sh) |
| ART properties: `dalvik.vm.dex2oat64.enabled=true` and `usejit=true`. Apps are compiled `speed-profile` at install time. | VERIFIED IN SOURCE | [aosp_lepton_arm64_only.mk](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_device_valve_lepton/lepton_arm64_only/aosp_lepton_arm64_only.mk) |
| Steamworks advises custom engines to target "Android 10 or Linux Arm64". `docs/STEAM_FRAME_COMPAT_TOOLS.md` lists this as UNKNOWN, because no snippet it saw reproduced "Android 10". | UPSTREAM DOCUMENTED (search-index excerpt; page not fetched) | https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom |
| A community x86_64 port builds on the LineageOS 18.1 image, adds an "ARM bridge", and installs to `compatibilitytools.d/lepton-x86_64`. Its README says upstream "only ships an aarch64 target". | COMMUNITY REFERENCE | https://github.com/Lolig4/lepton-x86/blob/main/README.x86_64.md |

## 5. Steam integration

| fact | label | source |
|---|---|---|
| `toolmanifest.vdf`: commandline `"/lepton %verb% --"`, version `"2"`, `use_tool_subprocess_reaper "1"`, `compatmanager_layer_name "lepton"`. | VERIFIED IN SOURCE | [toolmanifest.vdf](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/toolmanifest.vdf) |
| The verbs `start` and `waitforexitandrun` are handled. | VERIFIED IN SOURCE | [lepton#L65-L77](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/lepton#L65-L77) |
| `SteamAppId` sets the launch context to `steamlaunch-<AppID>`. | VERIFIED IN SOURCE | same |
| AppID 3056000 is special-cased as the `dev` context. | VERIFIED IN SOURCE | same |
| AppID 3056000 is the store app "Lepton Development". This was seen in a search listing only. | UPSTREAM DOCUMENTED (search listing only) | https://store.steampowered.com/app/3056000/Lepton_Development/ |
| First Steam launch of a game: the game's install directory is overlay-mounted into `/data`, at `/data/steam_app` or at the baked app directory, with a per-app upper layer. `adb install -g` runs over TCP 5555+offset, then the app is baked. | VERIFIED IN SOURCE | [liblepton.sh#L466-L533](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/liblepton.sh#L466-L533) ; [lepton#L252-L263](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/lepton#L252-L263) ; [mounting.sh#L253-L310](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L253-L310) |
| Later launches skip adb. They mount the baked app, and a generated init `.rc` runs `am start -S <pkg>/<activity>` on `sys.boot_completed=1 && ro.lepton.app_baked=1`. | VERIFIED IN SOURCE | same |
| Pushing `steam_appid.txt`, `UECommandLine.txt` and OBBs over adb happens only for non-Steam (dev and `.apk`) contexts. | VERIFIED IN SOURCE | [liblepton.sh#L466-L533](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/liblepton.sh#L466-L533) |
| The host runs adb from `/usr/lib/android-sdk/platform-tools/adb`. The adb service is advertised through avahi as `_adb._tcp`. | VERIFIED IN SOURCE | same |
| A patched Process Observer touches `/data/lepton-on-app-exit` when the app dies. The container is then stopped. | VERIFIED IN SOURCE | same |
| Persistent data lives in `compatdata/<appid>/internal/<pkg>`, symlinked to `/data/data/<pkg>`. External storage is `compatdata/.../external`. | VERIFIED IN SOURCE | [mounting.sh#L253-L310](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L253-L310) ; [app_metadata.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/app_metadata.sh) |
| Every `$STEAM_COMPAT_CLIENT_INSTALL_PATH/androidarm64/*.so`, including `libsteamclient.so`, is bind-mounted into `/system/lib64` and appended to `public.libraries.txt`. | VERIFIED IN SOURCE | [mounting.sh#L540-L551](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/mounting.sh#L540-L551) |
| The property `lepton.steamclient.path=/system/lib64/libsteamclient.so` tells `libsteam_api.so` where to look. | VERIFIED IN SOURCE | [properties.sh#L141-L142](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/properties.sh#L141-L142) |
| Zygote gets `Steam3Master=<gw>:57343`, reached over pasta's mapped gateway. | VERIFIED IN SOURCE | same ; [networking.sh#L83-L137](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/networking.sh#L83-L137) |
| `~/.steam/steam.pipe` is mounted at `/lepton/steam.pipe`. A frameworks/base patch forwards ACTION_VIEW URIs to it as `steam://openurl/<url>`. | VERIFIED IN SOURCE | [frameworks/base 0023](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/android_vendor_valve/patches/base-patches-34/frameworks/base/0023-lepton-Intercept-VIEW-intent-for-URIs-and-forward-it.patch) |
| Packaging: `compat_tool.tar.zst` holds the lepton script, liblepton, `toolmanifest.vdf`, rootfs_overlay, apk-info-extractor, LICENSES and `version.txt`, plus a temporary symlink named `fauxdroid`. | VERIFIED IN SOURCE | [build-compat_tool.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/.ci_scripts/build-compat_tool.sh) |
| `rootfs.tar.zst` is the squashfs `system.img` and `vendor.img`, converted with sqfs2tar and merged, with APEXes flattened and a `NOTICE.txt`. `sysbake.tar.zst` and `sysbake.xattrs` hold a pre-booted `/data`. | VERIFIED IN SOURCE | [package_rootfs.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/image-14/.buildscripts/package_rootfs.sh) |
| The tool installs to `~/.local/share/Steam/steamapps/common/Lepton`. With the sysbake missing it prints "verify the files of Lepton in Settings->Properties->Installed files". So it is a Steam-distributed app. | VERIFIED IN SOURCE | [deploy-compat_tool.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/.ci_scripts/deploy-compat_tool.sh) ; [baking.sh](https://github.com/xXJSONDeruloXx/lepton/blob/6135b53dcfe6111ca8fc310a3b35e0fec9e65b9b/compat_tool/liblepton/baking.sh) |
| "Connecting adb to Lepton": start "Lepton Development" from the Frame library, run `adb connect localhost:5555` (or the headset IP), and expect "connected to frame:5555". | UPSTREAM DOCUMENTED (search-index excerpt; page not fetched) | https://partner.steamgames.com/doc/steamhardware/steamframe/adb_lepton |
| An Android depot enables Lepton launches. A top-level `.apk` is listed as the launch option's Executable. Several `.apk`s are allowed. | UPSTREAM DOCUMENTED (search-index excerpt; page not fetched) | https://partner.steamgames.com/doc/steamhardware/steamframe/apk_upload |
| Steamworks SDK 1.63 and later include Android ARM64 and Linux ARM64 libraries. Use `libsteam_api.so` 1.64 or newer. | UPSTREAM DOCUMENTED (search-index excerpt; page not fetched) | https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom ; https://partner.steamgames.com/doc/steamframe/engines/unity |

## 6. Reuse table

The "what lxrun has" column was read in `runtime/` at `4c5efd5` and
re-checked at `dfab6e2`. It is marked
VERIFIED IN SOURCE only where the code was read. The label column gives three
labels: for the Lepton side, for the lxrun side, and for the verdict.

| Lepton component | what it needs from the host kernel | what lxrun on Darwin has today | reuse verdict | label |
|---|---|---|---|---|
| compat_tool scripts (`lepton`, `liblepton/*.sh`, `toolmanifest.vdf`) | Nothing directly. They drive podman, `systemctl --user`, flock, adb, avahi and inotify on a Linux host. | Linux shell scripts run under lxrun: Steam's `steam.sh` runs there today as x86-64 bash under FEX (MEASURED, `benchmarks/stage8-steam-zero-vm.txt`). flock (`LNR_flock`, `runtime/dispatch.c`) and inotify (`runtime/inotify.c`) are implemented. No container engine, systemd user session or avahi is in SteamARM's roots (HYPOTHESIS: nothing in `scripts/` installs one). | port. Keep the context, bake, property, `.rc` and adb logic. Replace every podman and systemd call with an lxrun launcher. | Lepton: VERIFIED IN SOURCE. lxrun: MEASURED, VERIFIED IN SOURCE, HYPOTHESIS. Verdict: HYPOTHESIS. |
| podman, pasta, catatonit (crun/runc and fuse-overlayfs per a community report) | User namespaces with newuidmap/newgidmap and subuid/subgid; mount, pid, ipc, uts and net namespaces; overlayfs or FUSE; cgroup v2; a tap device for pasta; a systemd user session. | None of these. `mount`, `umount2`, `pivot_root`, `unshare` and `setns` have no case in `lxrt_dispatch`, so they return ENOSYS (`runtime/dispatch.c:3268-3289`). `clone` without CLONE_THREAD and CLONE_VM is a plain fork; namespace flags are not looked at (`runtime/thread.c:464-470`). `/proc/sys/user/max_user_namespaces` reads 0 (`runtime/proc_ext.c:1174-1184`). | cannot work. Replace with an interpreter of Lepton's podman command line, in the style of `runtime/mounts.c`. | Lepton: VERIFIED IN SOURCE (podman, pasta, catatonit), COMMUNITY OBSERVATION (crun/runc, fuse-overlayfs). lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Namespaces and cgroups: private pid, ipc and uts; host user as uid 0; cgroupns or a delegated slice | Kernel namespaces and cgroup v2. | Mount namespaces are emulated per process for pressure-vessel's bwrap plan (`runtime/mounts.c`). It is a table of at most 256 binds (`MAX_MOUNTS`), matched longest prefix first; writes under a read-only bind get EROFS. `--tmpfs` and `--dir` become plain directories in a sandbox root. The bwrap options `--unshare-*`, `--cap-add`, `--cap-drop`, `--uid`, `--gid`, `--seccomp` and `--add-seccomp-fd` are refused with ENOSYS. There is no PID namespace: `getpid` returns the Darwin pid (`dispatch.c:3055`). `getuid` returns the Mac user's uid, not 0 (`dispatch.c:2669`). Unknown `prctl` options, such as PR_SET_CHILD_SUBREAPER, return 0 and do nothing (`dispatch.c:2664-2666`). `runtime/` has no cgroup filesystem. | port for the mount view (path redirection, as `mounts.c` does). The rest cannot work as namespaces. It needs new lxrun pieces: a uid-0 view, a virtual PID 1 that reaps, and a stub cgroupfs. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Capabilities: `--cap-drop ALL`, then 24 added back (sys_admin, net_admin, mknod, setuid, setgid and others) | Capabilities inside the user namespace. | `capget` reports an empty set. `capset` succeeds only when it asks for nothing; otherwise it returns EPERM (`dispatch.c:2150-2178`). | port: in a Lepton mode, answer `capget` and `capset` as Lepton's container would. HYPOTHESIS: today zygote's capset for system_server would fail. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Seccomp profile (`lepton.seccomp.json`) | seccomp-bpf. | No `seccomp` syscall (277 falls to ENOSYS) and no bwrap `--seccomp`. The profile's effects differ from lxrun's current answers: `fchown` and `fchownat` go to Darwin (`dispatch.c:2389-2391`, `2419-2422`); setuid and setgid to another id fail with EPERM (`lxrt_setresuid`, `runtime/fileops2.c:2334`); `setgroups` has no case, so ENOSYS. | reference only for the JSON. Port its results into the syscall table: success for the chown family, setuid, setgid, setgroups, setpriority, and the clock and key calls. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| binderfs and the binder driver | CONFIG_ANDROID_BINDER_IPC and CONFIG_ANDROID_BINDERFS; `mount -t binder` inside the user namespace; the BINDER_WRITE_READ ioctl protocol; per-process mmap'd receive buffers; node and handle refcounts; fd translation; death notifications. | Nothing. `mount` returns ENOSYS. Under `/dev`, only null, zero, full, random, urandom, `/dev/fd/N` and `/dev/std*` reach the host; every other path resolves inside the guest root (`dispatch.c:407-423`). `ioctl` handles terminals, FIONREAD, FIONBIO, FIOCLEX and evdev; any other request returns ENOTTY (`runtime/ioctl_tty.c:148-251`). Building blocks exist: process-shared futexes (`runtime/futex_ops.c`), SCM_RIGHTS, SysV shared memory (`runtime/sysv_ipc.c`) and memfd. | cannot work (the driver). A userspace binder, for example a broker process with shared memory, would be new lxrun work. The Android side is reuse as is: stock libbinder with Valve's uid-in-flags patch. Update, stage 25: that work exists. `runtime/binder.c` and `runtime/binder_hub.c` provide `/dev/binder`, `/dev/hwbinder` and `/dev/vndbinder` from a hub process; Android 11's servicemanager, `service` and a native service in another process work over it (`benchmarks/stage25-binder.txt`). `mount -t binder` (binderfs) is still ENOSYS: the devices simply exist. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS; the userspace driver: MEASURED. |
| memfd in place of ashmem | memfd_create with seals, shared over SCM_RIGHTS. | memfd_create exists. It is an unlinked temporary file, so growing, exact size, pread/pwrite and MAP_SHARED work. F_ADD_SEALS and F_GET_SEALS are bookkeeping inside one process: another process holding the fd, by SCM_RIGHTS or fork, is not bound by them (`runtime/fex_support.c:140-205`). A process can hold at most 256 memfd objects; the next memfd_create returns EMFILE (`fex_support.c:346-353`). | reuse as is for shared memory. HYPOTHESIS: the 256 limit and cross-process seals need checking against SurfaceFlinger and gralloc. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| FUSE for MediaProvider's `/storage/emulated` | `/dev/fuse`. | None. `/dev/fuse` is not passed through (`dispatch.c:407-423`). | cannot work as is. Either patch MediaProvider and vold to bind `/storage/emulated` directly, or serve the FUSE protocol inside lxrun. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Copy-on-write layers (podman `:O` for the rootfs, `/data` and the app directory) | overlayfs in the user namespace, or fuse-overlayfs. | No overlay in `runtime/`. `docs/STEAM_FRAME_IMAGE.md` already derives roots as APFS clones (`cp -c`). | port: an APFS clone of the baked `/data` per app. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Networking: pasta, a static eth0, a gateway to the host's Steam client (57343) and adbd (5555+n) | A network namespace and a tap device. | Guest sockets are host sockets, on the Mac's own interfaces and loopback. Abstract AF_UNIX names map to a directory (`runtime/socket.c:55-131`). AF_UNIX SOCK_SEQPACKET becomes SOCK_DGRAM (`socket.c:267-273`). Netlink exists only as a silent NETLINK_KOBJECT_UEVENT socket inside a bwrap plan (`socket.c:282-321`); there is no NETLINK_ROUTE. | port: point Steam3Master and adb at 127.0.0.1. UNKNOWN: how Android's network stack behaves with no route netlink and no eth0. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| SELinux, eBPF, uevent, device-mapper, loop devices | Nothing, after Valve's patches. | None of these, and none is needed. | reuse as is (Valve's patches). | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Android rootfs images: API 30 (LineageOS 18.1) and API 34 (AOSP android-14.0.0_r75) | A Linux arm64 kernel with 4 KiB pages, plus everything in the rows above. | Loads PIE aarch64 ELF only (`runtime/elf.c:113-122`). Since stage21 it also loads a main program or interpreter aligned to 4 KiB: it refuses only a PT_LOAD `p_align` that is not a multiple of 4 KiB (`elf.c:147-157`), and keeps protections per 4 KiB through `runtime/subpage.c` (`elf.c:209-225`). `LXRT_GUEST_PAGE=4096` makes AT_PAGESZ 4096 (`runtime/stack.c:21-34, 134`). Valve's 4 KiB-aligned arm64 Steam client loaded this way (MEASURED, `benchmarks/stage21-native-arm64-client.txt`). The `p_align` of Lepton's `/system/bin/linker64`, `/init` and `app_process64` is UNKNOWN. | port: rebuild an image from source with SteamARM's changes. Which API level follows what Steam ships (a community report says API 30 on the Frame). HYPOTHESIS: a 4 KiB-aligned Android 11 image loads the same way. Valve's depot build is reference only (PROPRIETARY_DO_NOT_REDISTRIBUTE; 4 KiB pages). | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE, MEASURED, UNKNOWN. Verdict: HYPOTHESIS. |
| Mesa, Zink and Turnip graphics injection (host `/usr/share/guestos/android` bind-mounted in; `ro.hardware.*` properties) | The msm DRM render node `/dev/dri/renderD128`, or KGSL for the Qualcomm blob; kcmp for Mesa. | No DRM or KGSL device. Vulkan reaches Metal through the shim `libvulkan.so.1`, which calls MoltenVK through the host bridge (`lxrt_host_dlopen`, `shim/vulkan_shim.c:458-462`). The shim is linked `-nostdlib` (`Makefile:98`). Its WSI is X11 only: it dlopens `libxcb.so.1` and `libX11-xcb.so.1` (`shim/wsi.c:123-124`), and `shim/` has no Android surface or Wayland code. `kcmp` has no case: ENOSYS. | cannot work for Turnip, freedreno, minigbm_msm and KGSL. port the injection scheme: bind a host-provided bionic driver into the rootfs and set the properties. That driver would be a bionic Vulkan HAL over MoltenVK, with Zink or ANGLE on top for GLES (HYPOTHESIS). | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Vulkan layers (VALVE_rpo, VALVE_fdm_injection, fossilize, validation, gfxreconstruct, RenderDoc), rbind-mounted into each app's `lib/arm64` | Bind mounts inside the container. | Binds exist only as the per-process bwrap table (`runtime/mounts.c`). | reference only. VALVE_rpo and VALVE_fdm_injection come from the Frame image (PROPRIETARY_DO_NOT_REDISTRIBUTE) and target Adreno and foveated VR (HYPOTHESIS). The injection method can be ported if a layer is ever needed. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| `apk_extractor` (Rust crate `apk-info-extractor`) | Nothing from the kernel; host userspace. | lxrun runs aarch64 Linux programs. The crate could also be built for macOS directly. | reuse as is (MIT). | Lepton: VERIFIED IN SOURCE. lxrun: HYPOTHESIS. Verdict: HYPOTHESIS. |
| The 4 KiB page-size assumption, against 16 KiB macOS host pages | A 4 KiB-page kernel. The API 34 image is 4096-aligned and compiles bionic's `PAGE_SIZE` macro. | `runtime/subpage.c` serves 4 KiB guest mappings on 16 KiB host pages. For a MAP_FIXED request at a 4 KiB address or file offset, and for data maps with a partial length, it backs the host pages with anonymous memory, reads the file bytes in with pread, and gives each 16 KiB page the union of its guest pages' protections (`subpage.c:1-24`, `dispatch.c:1100-1217`). A 4 KiB PROT_NONE guard next to a writable page therefore does not fault. A writable MAP_SHARED placed this way becomes a private copy (`dispatch.c:1156-1183`); read-only shared ranges are kept current by polling (`runtime/shmirror.c`). Since stage21, a large file segment whose address and offset agree modulo 16 KiB has its page-aligned interior mapped from the file, and only its edges copied (`subpage.c:273-344`). This works for FEX's x86 images (MEASURED, `benchmarks/stage5-subpage.txt`). Since stage21, `elf.c` also loads 4 KiB-aligned main programs and interpreters through `subpage.c` (`elf.c:147-157, 209-225`), and AT_PAGESZ is 4096 with `LXRT_GUEST_PAGE=4096`, 16384 otherwise (`runtime/stack.c:21-34, 134`). Valve's arm64 Steam client runs this way (MEASURED, stage21). | port. The second of the two earlier options, loading 4 KiB executables through `subpage.c`, now exists. HYPOTHESIS: page size is no longer a load-time wall. The costs remain: small mappings are copied, not shared, and guard pages are weaker. | Lepton: VERIFIED IN SOURCE. lxrun: VERIFIED IN SOURCE, MEASURED. Verdict: HYPOTHESIS. |
| bionic's TLS against glibc Mesa | Nothing from the kernel (userspace ABI). | Every `mrs` and `msr` of TPIDR_EL0 in guest code is rewritten to use a Darwin TSD slot (`runtime/tls.c:1-13`). The mechanism works at the register level; it has only been exercised with glibc. SteamARM's own Mesa (swrast in the Fedora root, for Xvnc) is glibc-built, and bionic's linker refuses a dlopened library with IE-model TLS (see 4.3). | cannot work for glibc Mesa or any glibc driver. A bionic-built driver is needed. The `-nostdlib` shim is the closest SteamARM piece (HYPOTHESIS). | Lepton: VERIFIED IN SOURCE (bionic linker). lxrun: VERIFIED IN SOURCE, HYPOTHESIS (bionic). Verdict: HYPOTHESIS. |
| x18 in Android code (the ABI reserves it; ShadowCallStack uses it) | Nothing from the kernel. | A virtual x18 in a second TSD slot, with x18 uses rewritten (`runtime/x18.c`, `runtime/tls.c:24-29`). Since stage21, `br`, `blr` and `ret` through x18 are rewritten too (`runtime/x18.c:273-288`), and the pass touches only function ranges from a file's `.eh_frame` when it has one (`runtime/elfsect.c:129-139, 184-205`). A binary linked against a macOS SDK below 13 keeps x18 across preemption (MEASURED on a GitHub M1 runner, `benchmarks/stage20-ci-macos-runner.txt`). | reuse as is (the lxrun mechanism). HYPOTHESIS: hand-written assembly without CFI would be skipped by the `.eh_frame` filter; `LXRT_X18_ALL_TEXT` names files to scan whole (`runtime/elfsect.c:290-298`). | Lepton: HYPOTHESIS. lxrun: VERIFIED IN SOURCE, MEASURED. Verdict: HYPOTHESIS. |
| Display: Waydroid hwcomposer to a Wayland socket | Sockets only; a Wayland compositor on the host. | SteamARM presents X11 only: XQuartz, rootless, with quartz-wm (`docs/ARCHITECTURE.md`). There is no Wayland compositor. | port: a composer that presents to X11 or Metal, or a Wayland compositor for macOS. | Lepton: VERIFIED IN SOURCE. SteamARM: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Audio: the host PulseAudio native socket | A Unix socket. | A PulseAudio server (Homebrew) on CoreAudio, with a socket inside each guest root that asks for one (`docs/ARCHITECTURE.md`; `docs/CURRENT_STEAM_ENVIRONMENT.md` §5.5). | reuse as is. | Lepton: VERIFIED IN SOURCE. SteamARM: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |
| Steamworks wiring: androidarm64 `libsteamclient.so`, `Steam3Master=<gw>:57343`, `steam.pipe` | TCP and a named pipe. | SteamARM's working Steam client is the x86 Linux client under FEX (MEASURED, `benchmarks/stage8-steam-zero-vm.txt`; still so in stage21). Valve's native arm64 client started under lxrun but aborted before its window in stage21; since stages 22-23 it reaches its sign-in window, and sign-in was not attempted (MEASURED, `benchmarks/stage22-native-arm64-bringup.txt`, `stage23-native-arm64-jit.txt`). Whether either carries `androidarm64/*.so` or listens on 57343 is UNKNOWN. Lepton is reported to offer itself on x86_64 clients although only an ARM64 build exists (COMMUNITY OBSERVATION, https://github.com/valvesoftware/steam-for-linux/issues/13634). | reference only until checked. The `.so` files are PROPRIETARY_DO_NOT_REDISTRIBUTE; they would be mounted from the user's own install, as Lepton does. | Lepton: VERIFIED IN SOURCE. SteamARM: MEASURED, UNKNOWN. Verdict: HYPOTHESIS. |
| OpenXR through SteamVR's androidarm64 `vrclient.so` | Nothing beyond the above. | SteamARM has no SteamVR. | cannot work. SteamVR has no macOS server, so flatscreen only (`lepton.headless=false`). | Lepton: VERIFIED IN SOURCE. SteamARM: VERIFIED IN SOURCE. Verdict: HYPOTHESIS. |

## 7. What this means for SteamARM

### Implications

1. HYPOTHESIS: Android apps on a Mac without a VM are possible in principle
   on lxrun. Valve already removed SELinux, eBPF, uevent, device-mapper,
   loop devices and real uid switching from what Android needs. What remains
   is kernel objects and a container view:
   - binder, the largest piece;
   - FUSE, or a patch that avoids it;
   - a container view: `mount(2)`, a uid-0 view, capabilities, a virtual
     PID 1, a stub cgroupfs.

   memfd already exists in lxrun (VERIFIED IN SOURCE,
   `runtime/fex_support.c`). Its seals hold only inside one process.
2. HYPOTHESIS: page size is no longer the first wall; binder is. The API 34
   image is built for 4 KiB pages (VERIFIED IN SOURCE, 4.4). The API 30 image
   that a community report says the Frame runs is Android 11, which predates
   16 KiB page support in AOSP (HYPOTHESIS). Since stage21, `runtime/elf.c`
   loads a main program or interpreter aligned to 4 KiB, and
   `LXRT_GUEST_PAGE=4096` reports 4 KiB pages (VERIFIED IN SOURCE,
   `runtime/elf.c:147-157`, `runtime/stack.c:21-34`). Valve's 4 KiB-aligned
   Steam client loaded this way (MEASURED, stage21). The cost stays:
   `subpage.c` copies small 4 KiB-aligned mappings instead of sharing them
   (MEASURED for FEX, `benchmarks/stage5-subpage.txt`).
3. HYPOTHESIS: the Frame kernel uses 4 KiB pages. The reasoning: Lepton's
   Android image is 4 KiB-aligned, and it runs on the Frame.
   `docs/STEAM_FRAME_SNAPSHOT_2026-09-29.md` records the page size as
   UNKNOWN, and it is open question 2 of `docs/CURRENT_STEAM_ENVIRONMENT.md`.
   `scripts/steamframe-image.py inventory` reads the kernel page size and
   settles it.
4. HYPOTHESIS: graphics is the largest piece of new code. Turnip, freedreno,
   minigbm_msm and KGSL are Adreno-only, and glibc drivers cannot load into
   bionic (4.3). A Darwin port needs:
   - a bionic-built Vulkan HAL that forwards to MoltenVK through the host
     bridge;
   - a gralloc HAL;
   - a composer, since SteamARM has X11 and no Wayland.

   Zink or ANGLE could then give GLES. MoltenVK's gaps (geometry shaders,
   transform feedback, `README.md`) would apply to Android games too.
5. HYPOTHESIS: the plan-interpreter pattern of `runtime/mounts.c` carries
   over to Lepton's podman command line, with the opposite policy. mounts.c
   refuses `--unshare-*`, capability and seccomp options, because a sandbox
   that silently is not one is worse than none. Lepton enforces no isolation
   between apps (VERIFIED IN SOURCE, README), so a Lepton interpreter could
   accept those options as no-ops.
6. VERIFIED IN SOURCE: Lepton is arm64-v8a only, with no native bridge
   (4.5). Games that ship only armeabi-v7a or x86 native code do not run on
   Lepton either. HYPOTHESIS: this scope fits Apple Silicon, and FEX,
   `FEX_ROOTFS` and pressure-vessel's interpreter root play no part in an
   Android port.
7. HYPOTHESIS: the Steam side is the least known part. SteamARM's working
   client today is the x86 Linux client under FEX (MEASURED,
   `benchmarks/stage8-steam-zero-vm.txt`; still so in stage21). Whether a
   Linux client on a Mac is offered Android depots is UNKNOWN. Lepton is
   reported to offer itself on x86_64 although only an ARM64 build exists
   (COMMUNITY OBSERVATION,
   https://github.com/valvesoftware/steam-for-linux/issues/13634). Valve's
   native ARM64 client is the likelier host. stage21 fetched it from Valve's
   manifest on the Mac and started it under lxrun, up to an abort before its
   window (MEASURED).
8. HYPOTHESIS: VR does not carry over. SteamVR has no macOS server, so only
   the flat window (`lepton.headless=false`) is a target.
9. HYPOTHESIS: Lepton runs Android apps only and plays no part in running
   Windows games. Nothing here changes
   `benchmarks/stage19-steamframe-base-and-arm64-limits.txt` (VERIFIED IN
   SOURCE there): macOS cannot map 0x7ffe0000, which unmodified Wine needs,
   so Valve's Proton ARM64 does not start unmodified on SteamARM.
10. HYPOTHESIS: Valve's Lepton tool, as shipped, cannot run under lxrun. It
    needs rootless podman, user namespaces and binder. This agrees with
    `docs/STEAM_FRAME_COMPAT_TOOLS.md` §7, which suggests filtering the tool
    or documenting it as unsupported. Only a Darwin port, as in the reuse
    table, could run Android apps.
11. COMMUNITY OBSERVATION: the VM route does not avoid binder either. A
    project that runs the Steam client in a 4K-page muvm microVM on Apple
    Silicon cannot run Lepton, because its guest has no binder
    (https://github.com/Daaboulex/steam-arm64-nix/blob/main/guest-run.sh).
12. HYPOTHESIS: licensing is workable. The compat tool is MIT and can be
    forked into a Darwin launcher with Valve's notice kept. A self-built
    rootfs is GPL-3.0 per Valve, with source offered. The androidarm64 Steam
    libraries, `vrclient.so` and the guestos tree are
    PROPRIETARY_DO_NOT_REDISTRIBUTE and stay out of the `.dmg`.

### Next steps

In order. Each step decides whether the next one is worth doing. Where
these sit among the other ARM64 work: `docs/ARM64_FIRST_MIGRATION.md`.

1. Measure alignment. Run `readelf -lW` (or `llvm-readelf -lW` on the Mac)
   on `system/bin/linker64`, `system/bin/init`, `system/bin/app_process64`,
   `system/lib64/libc.so` and `system/lib64/libbinder.so`. Do it for the
   rootfs Steam ships (API 30 per a community report) and for a self-built
   `image-14`. Valve's build is read locally on the owner's machine and
   never copied into the repo (PROPRIETARY_DO_NOT_REDISTRIBUTE). Do the same
   for `lib/arm64-v8a/*.so` of one Lepton game. Record every `p_align` in
   `benchmarks/`.
2. Start one bionic program under lxrun:
   `LXRT_ROOT=<android root> LXRT_GUEST_PAGE=4096 LXRT_REPORT_ENOSYS=1 build/lxrun <android root>/system/bin/toybox true`.
   Record `elf.c` refusals and the missing syscalls it names. Done for the
   Waydroid LineageOS 18.1 image, the base of Lepton's legacy `image/`
   (MEASURED, `benchmarks/stage25-android-userspace.txt`): linker64,
   toybox and mksh run after seven runtime fixes; ART cannot map its heap
   below 4 GiB on macOS (`docs/ANDROID_RUNTIME_ARCHITECTURE.md`).
3. `elf.c` no longer refuses 4 KiB-aligned executables (stage21). If
   `linker64` is 4 KiB-aligned, measure the cost of `subpage.c` for it, and
   compare with an `image-14` build with 16 KiB segments.
4. Extend `scripts/steamframe-image.py inventory` to report
   `/usr/share/guestos/android` (files, ELF machine, `p_align`) and the
   kernel's CONFIG_ANDROID_BINDER_IPC, CONFIG_ANDROID_BINDERFS and
   CONFIG_ANDROID_BINDER_DEVICES. Record the results with the rest of the
   image inventory in `docs/STEAM_FRAME_ROOTFS_AUDIT.md`, with the podman,
   crun, pasta and fuse-overlayfs versions, and the Lepton app version if the
   image holds it. Partly done (MEASURED, 2026-09-29,
   `docs/STEAM_FRAME_INVENTORY.md`): `usr/share/guestos/android` exists
   (57 MB), with podman 5.5.2, crun 1.14.4, passt 2026_06_11 and
   `lepton-podman-timeout.conf`; the per-file report and the binder config
   are not done.
5. On a Linux arm64 host with Lepton, capture the full podman command line
   for one game. Count its binds against `MAX_MOUNTS` (256). List the options
   a plan interpreter must accept.
6. The native ARM64 Steam client now installs and updates itself on the Mac
   (stage21). `androidarm64/` is in that install, with 5 aarch64 files
   including `libsteamclient.so` (37,517,612 bytes; MEASURED, audit
   2026-09-29). Once the client reaches its window, check whether it offers
   Lepton and Android depots, and whether it listens on 57343.
7. Only after steps 1 to 3 succeed: write design notes for a userspace binder
   in lxrun and for a bionic Vulkan HAL over MoltenVK.
8. If any Lepton file is copied into SteamARM, add Valve's MIT notice to
   `NOTICE`.

## 8. Could not be checked from here

- `https://gitlab.steamos.cloud/frame-public/lepton`: EGRESS_BLOCKED for both
  the shell and WebFetch. The upstream README, LICENSE, releases and GitLab
  project page were not seen directly. Mirror integrity against GitLab was
  not verified.
- `https://partner.steamgames.com/doc/steamhardware/steamframe/adb_lepton`,
  `apk_upload`, `compat`, `compatibility` and `engines/*`: EGRESS_BLOCKED.
  Only search-engine excerpts were used.
- The submodule repos `gitlab.steamos.cloud/frame-public/android_hardware_waydroid`,
  `android_device_waydroid_waydroid` and `android-vulkan-headers` were not
  fetched. The Waydroid hwcomposer and gralloc display source was not read.
- `steamdb.info` (app 3029110): EGRESS_BLOCKED.
- `store.steampowered.com` (apps 3029110 and 3056000): EGRESS_BLOCKED. Only
  search listings were seen.
- The press sites `gamingonlinux.com`, `mixed-news.com`, `videocardz.com`,
  `techspot.com`, `itsfoss.com`, `ghacks.net` and `hwbusters.com`:
  EGRESS_BLOCKED for WebFetch. Only search snippets were seen.
- GitHub API access to `ValveSoftware/SteamOS` was denied (session not
  configured). The issue page was read through WebFetch of github.com
  instead.
- Not inspectable here, and needing the owner's Mac and the recovery image:
  - Valve's prebuilt Steam depot rootfs and sysbake;
  - the real Frame's `/usr/share/guestos/android`;
  - the Frame kernel config, including its page size and binder settings.
- Not measured, and needing a Mac with lxrun:
  - the `p_align` of Lepton's platform binaries and of real APK libraries;
  - whether bionic's `linker64` starts under lxrun at all;
  - what SteamARM's Steam client offers for Android depots and the Lepton
    tool.
