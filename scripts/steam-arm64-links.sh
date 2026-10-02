#!/bin/bash
# ~/.steam's links for Valve's ARM64 Steam client, as Valve's launcher makes
# them before it starts the client (RUNSTEAM.sh in SteamOS for ARM64's
# /usr/lib/steam/steam.tar.zst): steam, root, sdk32, sdk64, sdkarm64,
# binarm64, bin32, bin64. SteamARM starts steamrtarm64/steam itself, so
# nothing made them, and the client starts every game as
#   ~/.steam/sdkarm64/steam-launch-wrapper -- ~/.steam/binarm64/reaper ...
# with the overlay preloaded from ~/.steam/bin64 and bin32: each game ended
# in two seconds with "/tmp/armhome/.steam/sdkarm64/steam-launch-wrapper: No
# such file or directory" (MEASURED, benchmarks/stage50).
#
#   steam-arm64-links.sh ROOT [GUEST_HOME]      (default home /tmp/armhome)
#
# Links are guest paths (absolute, inside ROOT), like the client's own
# ~/.steam/steam. Nothing of Valve's is changed; a path that exists and is
# not a link is left alone.
set -u
root=${1:?usage: steam-arm64-links.sh ROOT [GUEST_HOME]}
home=${2:-/tmp/armhome}
steam="$home/.local/share/Steam"
rt=steamrtarm64 sdk=linuxarm64
# The same fallbacks as RUNSTEAM.sh, for older and for in-between clients.
[ -d "$root$steam/$rt" ] || rt=linuxarm64
[ -d "$root$steam/$sdk" ] || sdk=steamrtarm64
[ -d "$root$steam/$rt" ] || exit 0          # no ARM64 client in this root
dot="$root$home/.steam"
mkdir -p "$dot" || exit 1
link() {   # link NAME GUEST_TARGET
    local l="$dot/$1"
    if [ -e "$l" ] && [ ! -L "$l" ]; then
        echo "steam-arm64-links: $l exists and is not a link: left alone" >&2
        return 0
    fi
    [ "$(readlink "$l" 2>/dev/null)" = "$2" ] || ln -sfn "$2" "$l"
}
link steam "$steam"
link root "$steam"
link sdk32 "$steam/linux32"
link sdk64 "$steam/linux64"
link sdkarm64 "$steam/$sdk"
link binarm64 "$steam/$rt"
link bin32 "$steam/ubuntu12_32"
link bin64 "$steam/ubuntu12_64"
