# Installing SteamARM

SteamARM runs the Linux Steam client, and the Windows games Steam runs
through Proton, on an Apple Silicon Mac **without a virtual machine**. This
page covers installing from the downloadable package (the easy way) and from
source.

> **Status:** experimental. Steam itself (store, library, downloads, login)
> works, and Windows programs run with Direct3D 9/11/12, sound and
> controllers in the test probes. **Real games are not verified yet.** Expect
> games that do not start, or that run slowly.

## Requirements

| | |
|---|---|
| Mac | Apple Silicon (M1 or later) |
| macOS | 14 Sonoma or later |
| Disk | about 25 GB free in your home folder (plus room for games) |
| Tools | Xcode Command Line Tools and [Homebrew](https://brew.sh) |
| Steam Frame base | the official recovery image, downloaded by you after accepting [Valve's license](https://store.steampowered.com/steamos/download/?ver=steamframe) |
| Time | 20–60 minutes the first time: it extracts the Steam Frame base, compiles FEX and downloads the x86 compatibility root and Steam |

Install the Command Line Tools with `xcode-select --install` if you don't
have them. For Homebrew, follow the instructions at <https://brew.sh>. The
installer checks both for you and says what's missing.

The source archive includes SteamARM's Rust crates in `vendor/`. Cargo is
configured for offline, locked builds, so compiling SteamARM does not contact
a crate registry. Setup still downloads Homebrew formulae, upstream projects
such as FEX and Linux packages needed by the runtime.

## 1. Download

From the [Releases page](https://github.com/LukeOkk/SteamARM/releases),
download `SteamARM-<version>-macOS-arm64.dmg`. You can check it against
`SHA256SUMS.txt`:

```sh
shasum -a 256 SteamARM-*-macOS-arm64.dmg
```

Open the .dmg and drag **SteamARM** onto **Applications**.

## 2. First open (Gatekeeper)

The app is signed ad hoc, not with an Apple Developer ID, so macOS blocks
the first open:

1. Open SteamARM from Applications. macOS says it can't verify the developer.
   Close the message.
2. Go to **System Settings → Privacy & Security**. Near the bottom, under
   Security, click **Open Anyway** next to SteamARM, and confirm.

Or, from Terminal (this only removes the download quarantine flag):

```sh
xattr -dr com.apple.quarantine /Applications/SteamARM.app
```

## 3. Install the runtime and Steam

On first start the launcher unpacks its source into
`~/Library/Application Support/SteamARM/src` and shows **"Falta instalar
SteamARM"** with an **Instalar** button. Clicking it opens Terminal and runs
`scripts/setup.sh`. The setup:

1. checks the Mac (Apple Silicon, Command Line Tools, Homebrew, free space);
2. installs the Homebrew formulae it builds with (LLVM, CMake, Ninja,
   KosmicKrisp/Metal 4 when supported, MoltenVK fallback, SDL, mingw-w64,
   PulseAudio…);
3. builds the runtime (`lxrun`), the Vulkan shim, the launcher and the
   controller service;
4. extracts the Steam Frame image from `~/Downloads` and derives the required
   case-sensitive ARM64 root. The image may be `.img` or `.img.bz2`; set
   `STEAMARM_FRAME_IMAGE` to use another path;
5. installs the native ARM64 Steam client from the same licensed image into
   the Frame root;
6. builds [FEX](https://github.com/FEX-Emu/FEX) (x86 → ARM64) against that
   SteamOS/Arch root, then builds its Vulkan thunks with SteamARM's patches;
7. builds XQuartz's X server and quartz-wm (windows for Linux programs);
8. assembles the x86 compatibility roots from FEX's Ubuntu 24.04 image and
   merges them with the Steam Frame base. Fedora is no longer used as the
   ARM64 build or runtime base;
9. installs the x86 Steam client bootstrap when the compatibility client is
   needed;
10. runs quick self-tests.

SteamARM does not package or redistribute Valve's recovery image. Download it
from Valve's page above, accept its license, and leave the image in Downloads.
The installer extracts it locally and keeps the derived root in
`~/Developer/SteamARM-roots/steamframe-root.sparsebundle`.

If it stops (network error, not enough space, Terminal closed), click
**Instalar** again. Every step skips what is already done.

When it finishes, the banner disappears. Click **Steam** in the launcher.
The first Steam start updates the client (a few minutes) and then shows the
sign-in window.

## 4. First steps in Steam

1. Sign in.
2. **Steam → Settings → Compatibility**: enable Steam Play for all titles
   and choose **Proton Experimental**. Steam downloads Proton and the Steam
   Linux Runtime the first time a Windows game starts.
3. Install and start a game. See [USAGE.md](USAGE.md) for controllers, sound,
   memory limits and the other settings.

## Where things go

| Path | What |
|---|---|
| `/Applications/SteamARM.app` | the launcher |
| `~/Library/Application Support/SteamARM/src` | the source the app unpacked, and what setup builds in it (`build/`) |
| `~/Developer/SteamARM-build` | FEX, its thunks, XQuartz, package caches |
| `~/Developer/SteamARM-roots` | Steam Frame base (`steamframe-root.sparsebundle`), x86 Linux roots, Steam and your games (`steamroot/`), launcher settings (`launcher/`), logs (`logs/`) |

An install made before 0.3.74 keeps using `~/SteamARM-build` and
`~/SteamARM-roots`; to move it, quit SteamARM and Steam and move both folders
into `~/Developer`. `STEAMARM_BUILD` and `STEAMARM_STATE` override either
location.
| `/tmp/lxrt-root`, `/tmp/lxrt-steamroot` | links to the roots (re-created at each start; macOS empties `/tmp` at boot) |

Nothing is installed system-wide, and nothing needs root, except what
Homebrew itself installs.

## Updating

In **Ajustes → Actualizaciones de SteamARM**, click **Buscar actualizaciones**.
When a newer stable release exists, click its download link, open the .dmg,
and replace SteamARM in Applications. On its first start the launcher
unpacks the newer source over the old copy. Then click **Instalar** (shown
again if something needs rebuilding), or run `scripts/setup.sh` from that
folder. Your Steam library and settings are kept.

## Uninstalling

Quit SteamARM and Steam, then:

```sh
rm -rf /Applications/SteamARM.app \
       "$HOME/Library/Application Support/SteamARM" \
       ~/Developer/SteamARM-build ~/Developer/SteamARM-roots \
       ~/SteamARM-build ~/SteamARM-roots
rm -f /tmp/lxrt-root /tmp/lxrt-steamroot
```

`SteamARM-roots` contains your installed games: move them elsewhere first
if you want to keep them. The Homebrew formulae stay; remove them with
`brew uninstall` if you don't need them.

## Installing from source

```sh
git clone https://github.com/LukeOkk/SteamARM.git && cd SteamARM
scripts/setup.sh            # resumable; scripts/setup.sh --list shows the steps
open build/SteamARM.app     # or: scripts/run-steam.sh
```

An app built this way uses the checkout it was built from. To build the
downloadable package yourself: `scripts/make-release.sh` (output in
`build/release/`). Before publishing one, run the test suites **and** start
Steam with the final runtime until its window appears: the probes alone do
not exercise Steam's web helper.
