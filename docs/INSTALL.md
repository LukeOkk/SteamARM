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
| Time | 20–60 minutes the first time: it compiles FEX and downloads Linux packages and Steam |

Install the Command Line Tools with `xcode-select --install` if you don't
have them. For Homebrew, follow the instructions at <https://brew.sh>. The
installer checks both for you and says what's missing.

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
   MoltenVK, SDL, mingw-w64, PulseAudio…);
3. builds the runtime (`lxrun`), the Vulkan shim, the launcher and the
   controller service;
4. clones and builds [FEX](https://github.com/FEX-Emu/FEX) (x86 → ARM64) and
   its Vulkan thunks, with SteamARM's patches;
5. builds XQuartz's X server and quartz-wm (windows for Linux programs);
6. assembles the Linux roots: Fedora packages for the aarch64 side, Ubuntu
   24.04 x86-64 for FEX, and the root Steam runs in;
7. installs the Steam client (Valve's `steam_latest.deb` bootstrap);
8. runs quick self-tests.

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
| `~/SteamARM-build` | FEX, its thunks, XQuartz, package caches |
| `~/SteamARM-roots` | the Linux roots, Steam and your games (`steamroot/`), launcher settings (`launcher/`), logs (`logs/`) |
| `/tmp/lxrt-root`, `/tmp/lxrt-steamroot` | links to the roots (re-created at each start; macOS empties `/tmp` at boot) |

Nothing is installed system-wide, and nothing needs root, except what
Homebrew itself installs.

## Updating

Install the new .dmg over the old app. On its first start the launcher
unpacks the newer source over the old copy. Then click **Instalar** (shown
again if something needs rebuilding), or run `scripts/setup.sh` from that
folder. Your Steam library and settings are kept.

## Uninstalling

Quit SteamARM and Steam, then:

```sh
rm -rf /Applications/SteamARM.app \
       "$HOME/Library/Application Support/SteamARM" \
       ~/SteamARM-build ~/SteamARM-roots
rm -f /tmp/lxrt-root /tmp/lxrt-steamroot
```

`~/SteamARM-roots` contains your installed games: move them elsewhere first
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
`build/release/`).
