#!/bin/bash
# Build PulseAudio with SteamARM's patches (patches/pulseaudio-*.patch), for
# the Mac's sound server (scripts/audio.sh), without sudo, into
# $HOME/SteamARM-build/pulseaudio.
#
#   sources : $PA_ROOT/pulseaudio-<ver>       (+ Homebrew's backport, + ours)
#   output  : $PA_ROOT/out/bin/pulseaudio
#
# scripts/audio.sh runs this server when it exists, Homebrew's otherwise.
# The patch: module-coreaudio-device gives its sinks and sources a real fixed
# latency (a few IOProc buffers) instead of PulseAudio's default 250 ms, with
# which every guest's sound was about half a second late.
#
# Configured as Homebrew's formula configures it (same dependencies, already
# installed with the pulseaudio formula). Re-running is safe: the download is
# cached and the build is incremental.
set -euo pipefail

PA_ROOT="${PA_ROOT:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/pulseaudio}"
BREW="${BREW:-/opt/homebrew}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

PA_VER=17.0
PA_SHA=053794d6671a3e397d849e478a80b82a63cb9d8ca296bd35b73317bb5ceb87b5
# Homebrew's backport for 17.0 (pulseaudio issue 3808).
BACKPORT_URL=https://gitlab.freedesktop.org/pulseaudio/pulseaudio/-/commit/c1990dd02647405b0c13aab59f75d05cbb202336.diff
BACKPORT_SHA=46505b7f915a96a4e5f4c46cd8a2cfb5a74586bfd585d69f31b7b2e27e17a4c8

log() { printf '[build-pulseaudio] %s\n' "$*"; }

if [ "${SKIP_BREW:-0}" != 1 ]; then
    log "installing Homebrew formulae"
    "$BREW/bin/brew" install meson ninja pkgconf gettext glib libsndfile libsoxr libtool openssl@3 orc speexdsp
fi
export PATH="$BREW/bin:$PATH"
export PKG_CONFIG_PATH="$BREW/lib/pkgconfig:$BREW/share/pkgconfig:$BREW/opt/openssl@3/lib/pkgconfig"
export CFLAGS="-I$BREW/opt/gettext/include -I$BREW/include"
export LDFLAGS="-L$BREW/opt/gettext/lib -L$BREW/lib"

mkdir -p "$PA_ROOT/out"
cd "$PA_ROOT"
SRC="$PA_ROOT/pulseaudio-$PA_VER"
TARBALL="$PA_ROOT/pulseaudio-$PA_VER.tar.xz"
BACKPORT="$PA_ROOT/backport-c1990dd0.diff"
[ -f "$TARBALL" ] || curl -fL --retry 3 -o "$TARBALL" "https://www.freedesktop.org/software/pulseaudio/releases/pulseaudio-$PA_VER.tar.xz"
[ -f "$BACKPORT" ] || curl -fL --retry 3 -o "$BACKPORT" "$BACKPORT_URL"
echo "$PA_SHA  $TARBALL" | shasum -a 256 -c - >/dev/null || { log "sha256 mismatch: $TARBALL"; exit 1; }
echo "$BACKPORT_SHA  $BACKPORT" | shasum -a 256 -c - >/dev/null || { log "sha256 mismatch: $BACKPORT"; exit 1; }

# A fresh tree whenever the patches change (as build-kosmickrisp.sh).
PATCH_STAMP="$( cd "$PROJECT_DIR/patches" && cat pulseaudio-*.patch | shasum -a 256 | cut -d' ' -f1 )"
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.steamarm-patches" 2>/dev/null)" != "$PATCH_STAMP" ]; then
    log "fresh source tree (new or changed patches)"
    rm -rf "$SRC"
    tar xmf "$TARBALL"
    patch -d "$SRC" -p1 -N -s < "$BACKPORT"
    for p in "$PROJECT_DIR"/patches/pulseaudio-*.patch; do
        [ -f "$p" ] || continue
        log "applying $(basename "$p")"
        patch -d "$SRC" -p1 -N -s < "$p"
    done
    echo "$PATCH_STAMP" > "$SRC/.steamarm-patches"
fi

if [ ! -f "$PA_ROOT/build/build.ninja" ]; then
    log "meson setup"
    meson setup "$PA_ROOT/build" "$SRC" --prefix="$PA_ROOT/out" --sysconfdir="$PA_ROOT/out/etc" \
        --localstatedir="$PA_ROOT/out/var" --buildtype=release \
        -Ddatabase=simple -Ddoxygen=false -Dman=false -Dtests=false \
        -Dstream-restore-clear-old-devices=true -Dalsa=disabled -Ddbus=disabled -Dglib=enabled \
        -Dgtk=disabled -Dopenssl=enabled -Dorc=enabled -Dsoxr=enabled -Dspeex=enabled \
        -Dsystemd=disabled -Dx11=disabled -Dbashcompletiondir=no -Dzshcompletiondir=no \
        > "$PA_ROOT/setup.log" 2>&1 || { tail -20 "$PA_ROOT/setup.log"; log "meson setup failed (log: $PA_ROOT/setup.log)"; exit 1; }
fi
log "ninja"
ninja -C "$PA_ROOT/build" > "$PA_ROOT/build.log" 2>&1 || { grep -a "error" "$PA_ROOT/build.log" | head -20; log "build failed (log: $PA_ROOT/build.log)"; exit 1; }
meson install -C "$PA_ROOT/build" --quiet > "$PA_ROOT/install.log" 2>&1 || { log "install failed (log: $PA_ROOT/install.log)"; exit 1; }
log "server: $PA_ROOT/out/bin/pulseaudio"
