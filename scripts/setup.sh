#!/bin/bash
# SteamARM from a clean Mac: build everything and install Steam, no VM.
#
#   scripts/setup.sh              all steps (each one skips what is already done)
#   scripts/setup.sh --list       the steps, without running them
#   scripts/setup.sh <step>...    only those steps
#
# Needs: an Apple Silicon Mac, the Xcode Command Line Tools
# (xcode-select --install) and Homebrew (https://brew.sh). Disk: ~25 GB
# (build trees, the Fedora and Ubuntu packages, the guest roots, Steam).
#
# Where things go:
#   this checkout          build/lxrun (runtime), build/libvulkan.so.1 (Vulkan
#                          shim), build/SteamARM.app (launcher)
#   $STEAMARM_BUILD        FEX, its thunks, XQuartz, package caches
#                          (default ~/SteamARM-build)
#   $STEAMARM_STATE        the guest roots, logs (default ~/SteamARM-roots):
#     lxrt-root            aarch64 root (Fedora 43 RPMs): FEX, Xvnc, test programs
#     x86-rootfs           FEX's Ubuntu 24.04 x86-64 rootfs
#     steamroot            the root Steam runs in (x86-64 tree + the emulator)
#   /tmp/lxrt-root, /tmp/lxrt-steamroot   links to the roots (scripts/env-links.sh;
#                          macOS empties /tmp at boot: run-steam.sh re-creates them)
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
REPO="$(pwd)"
BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
export STEAMARM_BUILD="$BUILD" STEAMARM_STATE="$STATE"
BREW="${BREW:-/opt/homebrew}"
log() { printf '\n\033[1m[setup] %s\033[0m\n' "$*"; }
die() { printf '[setup] error: %s\n' "$*" >&2; exit 1; }

FORMULAE=(llvm lld cmake ninja zstd xz openssl@3 squashfs xxhash python
          molten-vk vulkan-headers sdl2 sdl3 mingw-w64 pulseaudio)

step_check() {
    [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || die "needs macOS on Apple Silicon"
    xcode-select -p >/dev/null 2>&1 || die "Xcode Command Line Tools missing: xcode-select --install"
    [ -x "$BREW/bin/brew" ] || die "Homebrew missing at $BREW (https://brew.sh)"
    local free
    free=$(df -g "$HOME" | awk 'NR==2 {print $4}')
    [ "${free:-0}" -ge 25 ] || die "less than 25 GB free in $HOME"
    log "check: macOS $(sw_vers -productVersion) arm64, CLT, Homebrew, ${free} GB free"
}

step_brew() {
    log "brew: ${FORMULAE[*]}"
    "$BREW/bin/brew" install "${FORMULAE[@]}"
}

step_make() {
    log "make: runtime, Vulkan shim, launcher, controller daemon"
    make -j"$(sysctl -n hw.ncpu)" all
}

step_fex() {
    log "FEX (scripts/build-fex-host.sh)"
    scripts/build-fex-host.sh all
}

step_thunks() {
    log "FEX Vulkan thunks, 64- and 32-bit guests (scripts/build-fex-thunks.sh)"
    scripts/build-fex-thunks.sh all
}

step_xquartz() {
    log "native X server + window manager (scripts/build-xquartz.sh)"
    scripts/build-xquartz.sh
    # A running server keeps its old binary and preferences (XTEST, Linux
    # keycodes: patches/xquartz-evdev-keycodes.patch) until it starts again:
    # restart it now if no guest program is running, else at its next start.
    if pgrep -f "SteamARM-X11.app/Contents/MacOS/X11.bin :2" >/dev/null 2>&1 &&
       ! pgrep -f "build/lxrun " >/dev/null 2>&1; then
        scripts/run-x11-native.sh restart :2 || true
    fi
}

step_roots() {
    mkdir -p "$STATE/logs"
    if [ ! -d "$STATE/lxrt-root/usr" ]; then
        log "aarch64 root (scripts/mkroot-rpm.sh)"
        scripts/mkroot-rpm.sh "$STATE/lxrt-root" "$STATE/samples"
    fi
    if [ ! -d "$STATE/x86-rootfs/usr" ]; then
        log "x86-64 rootfs (scripts/fetch-x86-rootfs.sh)"
        scripts/fetch-x86-rootfs.sh "$STATE/x86-rootfs"
    fi
    if [ ! -d "$STATE/steamroot/usr" ]; then
        log "Steam root (scripts/mksteamroot.sh)"
        scripts/mksteamroot.sh "$STATE/x86-rootfs" "$STATE/lxrt-root" "$STATE/steamroot" "$BUILD/out/FEX-emu"
    fi
    # The guest home lives in the Steam root; FEX finds its x86 rootfs there
    # by name (Config.json "RootFS": "Ubuntu_24_04"), and the test root shares
    # the same home. An APFS clone: no extra space.
    local home="$STATE/steamroot/tmp/fexhome" rfs
    rfs="$home/.local/share/fex-emu/RootFS/Ubuntu_24_04"
    if [ ! -d "$rfs/usr" ]; then
        log "FEX rootfs in the guest home"
        mkdir -p "$(dirname "$rfs")"
        cp -Rc "$STATE/x86-rootfs" "$rfs"
    fi
    if [ ! -e "$STATE/lxrt-root/tmp/fexhome" ]; then
        mkdir -p "$STATE/lxrt-root/tmp"
        ln -s ../../steamroot/tmp/fexhome "$STATE/lxrt-root/tmp/fexhome"
    fi
    scripts/env-links.sh "$STATE" >/dev/null
    log "roots ready under $STATE"
}

step_steam() {
    log "Steam client and graphics in the Steam root"
    scripts/install-steam.sh
    scripts/install-fex-host.sh
    LXRT_ROOT=/tmp/lxrt-root scripts/build-fex-thunks.sh install
    scripts/install-steamroot-gfx.sh
}

step_frameroot() {
    # The Steam Frame root is made by hand (scripts/mkframeroot.sh, from the
    # user's recovery image); when it exists, its Vulkan shim follows this
    # source. Reported, not fatal.
    if [ -d "$STATE/arm64root/usr/share/vulkan" ]; then
        log "Vulkan in the Steam Frame root (scripts/install-frameroot-vulkan.sh)"
        scripts/install-frameroot-vulkan.sh "$STATE/arm64root" || log "Steam Frame root: Vulkan not installed (setup goes on)"
    fi
}

step_test() {
    log "smoke tests"
    # Reported, not fatal: everything is installed by now, and a failure here
    # (a stress test that fails one run in several, say) must not skip the
    # last step -- without build/.setup-version the launcher keeps offering
    # setup again (MEASURED with 0.3.15: "102 passed, 1 failed", exit 1).
    tests/elf/run.sh | tail -1 || log "tests/elf: a test failed (setup goes on; tests/elf/run.sh shows which)"
    tests/elf/run_vk_device.sh | tail -1 || log "Vulkan/OpenGL probes: a probe failed (setup goes on)"
    # The Windows probes need Proton, which Steam downloads after the first
    # login (Settings > Compatibility, or the first Windows game).
    if [ -x "/tmp/lxrt-steamroot/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine" ]; then
        tests/win/run.sh | tail -9 || log "Windows probes: a probe failed (setup goes on)"
    else
        log "Windows probes skipped: no Proton yet (start Steam, log in, install Proton Experimental; then scripts/setup.sh test)"
    fi
}

step_done() {
    # The launcher of a downloaded app compares this with the version of the
    # source it unpacked, and offers setup again after an update.
    [ -f .bundle-version ] && cp .bundle-version build/.setup-version
    log "done. Start SteamARM:"
    echo "  open $REPO/build/SteamARM.app        (launcher: Steam and other apps)"
    echo "  scripts/run-steam.sh                  (Steam directly)"
    echo "The first Steam start updates the client (several minutes)."
}

STEPS=(check brew make fex thunks xquartz roots steam frameroot test done)
if [ "${1:-}" = --list ]; then
    printf '%s\n' "${STEPS[@]}"
    exit 0
fi
run=("${@:-${STEPS[@]}}")
[ $# -gt 0 ] || run=("${STEPS[@]}")
for s in "${run[@]}"; do
    type "step_$s" >/dev/null 2>&1 || die "unknown step '$s' (--list)"
    "step_$s"
done
