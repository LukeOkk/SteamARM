#!/usr/bin/env bash
# Install the freshly built FEX into both zero-VM guest roots.
# Run only while no lxrun guest is active; old copies are kept for rollback.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
LXRT_ROOT="${LXRT_ROOT:-/tmp/lxrt-root}"
STEAM_ROOT="${LXRT_STEAM_ROOT:-/tmp/lxrt-steamroot}"
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="$STATE/backups/fex/$STAMP"

log() { printf '[install-fex-host] %s\n' "$*"; }
die() { log "error: $*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

for f in FEX FEXServer FEXGetConfig FEX-emu; do
    [ -f "$BUILD/out/$f" ] || die "missing $BUILD/out/$f (run scripts/build-fex-host.sh first)"
done
[ -d "$LXRT_ROOT/usr/bin" ] || die "missing $LXRT_ROOT/usr/bin"
[ -d "$STEAM_ROOT/usr/bin" ] || die "missing $STEAM_ROOT/usr/bin"
[ -d "$STEAM_ROOT/usr/lib/lxrt-emu" ] || die "missing $STEAM_ROOT/usr/lib/lxrt-emu"
if pgrep -f 'build/lxrun' >/dev/null 2>&1; then
    die "an lxrun guest is active; close it with scripts/run-app.sh --stop before updating FEX"
fi

put() { # source destination backup label
    local src="$1" dst="$2" label="$3" old new
    [ -f "$dst" ] || die "missing installed file $dst"
    old="$(sha256 "$dst")"
    new="$(sha256 "$src")"
    [ "$old" = "$new" ] && { log "current $dst"; return 0; }
    mkdir -p "$BACKUP"
    cp -p "$dst" "$BACKUP/$label"
    cp -p "$src" "$dst.new.$$"
    mv -f "$dst.new.$$" "$dst"
    log "$dst: $old -> $new; previous saved as $BACKUP/$label"
}

log "FEX binaries -> $LXRT_ROOT"
put "$BUILD/out/FEX" "$LXRT_ROOT/usr/bin/FEX-gb" lxrt-FEX-gb
put "$BUILD/out/FEXServer" "$LXRT_ROOT/usr/bin/FEXServer" lxrt-FEXServer
put "$BUILD/out/FEXGetConfig" "$LXRT_ROOT/usr/bin/FEXGetConfig" lxrt-FEXGetConfig

log "FEX binaries -> $STEAM_ROOT"
put "$BUILD/out/FEX" "$STEAM_ROOT/usr/bin/FEX-gb" steam-FEX-gb
put "$BUILD/out/FEXServer" "$STEAM_ROOT/usr/bin/FEXServer" steam-FEXServer
put "$BUILD/out/FEXGetConfig" "$STEAM_ROOT/usr/bin/FEXGetConfig" steam-FEXGetConfig
put "$BUILD/out/FEX-emu" "$STEAM_ROOT/usr/lib/lxrt-emu/FEX" steam-FEX-emu

log "done"
