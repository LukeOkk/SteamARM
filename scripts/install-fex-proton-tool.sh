#!/bin/bash
# Install "SteamARM: Proton x86 via FEX (experimental)", the Steam
# compatibility tool that sends a Windows game started by Valve's native
# arm64 Steam client to the x86 Proton and Steam Linux Runtime of SteamARM's
# x86 Steam root, run by SteamARM's FEX (docs/FEX_GAME_BOUNDARY.md).
# EXPERIMENTAL: measured with Windows probes invoked the way Steam invokes a
# tool, not with a game, and not from the client's UI (which needs a sign-in).
#
#   scripts/install-fex-proton-tool.sh [--root armroot|arm64root|DIR] [--proton NAME [--unverified]] [--dry-run]
#   scripts/install-fex-proton-tool.sh --uninstall [--root ...]
#
# --root     the ARM64 root whose client gets the tool: armroot (default, the
#            Fedora root of scripts/mkarmroot.sh), arm64root (the Steam Frame
#            root of scripts/mkframeroot.sh) or a directory. The client lives
#            in its tmp/armhome (HOME=/tmp/armhome in the guest).
# --proton   the x86 Proton in the x86 Steam root's library (default
#            "Proton - Experimental"); the Steam Linux Runtime it runs in is
#            the one its toolmanifest.vdf requires (Proton - Experimental:
#            SteamLinuxRuntime_4; Proton 10.0: SteamLinuxRuntime_sniper).
#            Only "Proton - Experimental" is measured to work. Proton 10.0 is
#            measured NOT to start a probe, on the x86 client's own path too;
#            any other is not measured. Those need --unverified.
#
# Writes only <root>/tmp/armhome/.local/share/Steam/compatibilitytools.d/
# steamarm-fex-proton/: the tool (tools/steamarm-fex-proton/) and
# steamarm-fex-proton.conf. Reads, never writes, the x86 Steam root.
# Test overrides: STEAMARM_STATE, X86_LINK (default /tmp/lxrt-steamroot, the
# link the runtime sees the x86 root through), FEXDIR (default
# /tmp/lxrt-root/usr/bin), FEXBIN (default FEX-gb), NO_ENV_LINKS=1.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
X86_LINK="${X86_LINK:-/tmp/lxrt-steamroot}"
FEXDIR="${FEXDIR:-/tmp/lxrt-root/usr/bin}"
FEXBIN="${FEXBIN:-FEX-gb}"
ROOT="$STATE/armroot" PROTON="Proton - Experimental" DRY=0 UNINSTALL=0 UNVERIFIED=0
while [ $# -gt 0 ]; do
    case "$1" in
        --root)
            case "${2:?--root needs armroot, arm64root or a directory}" in
                armroot) ROOT="$STATE/armroot" ;;
                arm64root) ROOT="$STATE/arm64root" ;;
                *) ROOT="$2" ;;
            esac
            shift 2 ;;
        --proton) PROTON="${2:?--proton needs a name}"; shift 2 ;;
        --unverified) UNVERIFIED=1; shift ;;
        --dry-run) DRY=1; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
die() { echo "install-fex-proton-tool: $*" >&2; exit 1; }

STEAM_ARM="$ROOT/tmp/armhome/.local/share/Steam"
DEST="$STEAM_ARM/compatibilitytools.d/steamarm-fex-proton"
if [ "$UNINSTALL" = 1 ]; then
    [ -d "$DEST" ] || { echo "not installed: $DEST"; exit 0; }
    [ "$DRY" = 1 ] && { echo "would remove $DEST"; exit 0; }
    rm -rf -- "$DEST"
    echo "removed $DEST"
    exit 0
fi

[ -f "$STEAM_ARM/steamrtarm64/steam" ] ||
    die "no native arm64 client at $STEAM_ARM/steamrtarm64/steam (docs/STEAM_ARM64_BRINGUP.md)"
case "$PROTON" in
    */*|.|..|'') die "--proton takes a directory name in steamapps/common, not a path" ;;
    *ARM64*|*arm64*) die "$PROTON: Proton ARM64 cannot run on macOS (benchmarks/stage18); pick an x86 Proton" ;;
esac
# What benchmarks/stage24-fex-game-boundary.txt measured, per Proton.
case "$PROTON" in
    "Proton - Experimental") status="" ;;
    "Proton 10.0") status="measured NOT to start: its Wine does not finish starting under FEX in 240-300 s, on the x86 client's own path as well (benchmarks/stage24-fex-game-boundary.txt)" ;;
    *) status="not measured with this tool" ;;
esac
if [ -n "$status" ]; then
    [ "$UNVERIFIED" = 1 ] || die "$PROTON is $status; --unverified installs it anyway"
    echo "warning: $PROTON is $status" >&2
fi

# The x86 side, through the links the runtime will use.
[ "${NO_ENV_LINKS:-}" = 1 ] || scripts/env-links.sh >/dev/null
[ -d "$X86_LINK/" ] || die "no x86 Steam root at $X86_LINK (scripts/env-links.sh; scripts/setup.sh installs it)"
X86_HOME=/tmp/fexhome
X86_STEAM=$X86_HOME/.local/share/Steam
COMMON="$X86_LINK$X86_STEAM/steamapps/common"
P="$COMMON/$PROTON"
[ -f "$P/proton" ] && [ -f "$P/files/bin/wine" ] ||
    die "x86 $PROTON is not installed in the x86 Steam root ($P); install it from the x86 client's library"
[ ! -d "$P/files/bin-arm64" ] || die "$P is an ARM64 build: Proton ARM64 cannot run on macOS"
# Wine's loader must be x86 (ELF e_machine 62, x86-64: Proton 11 and
# Experimental; 3, i386: Proton 10.0's multilib loader), whatever the name says.
machine=$(/usr/bin/python3 -c 'import sys; d=open(sys.argv[1],"rb").read(20); print(int.from_bytes(d[18:20],"little") if d[:4]==b"\x7fELF" else -1)' "$P/files/bin/wine")
case "$machine" in 62|3) ;; *) die "$P/files/bin/wine is not an x86 ELF (e_machine $machine)" ;; esac
appid=$(sed -n 's/.*"require_tool_appid"[[:space:]]*"\([0-9]*\)".*/\1/p' "$P/toolmanifest.vdf" 2>/dev/null | head -1)
case "$appid" in
    1628350) RUNTIME=SteamLinuxRuntime_sniper ;;
    4183110) RUNTIME=SteamLinuxRuntime_4 ;;
    1391110) RUNTIME=SteamLinuxRuntime_soldier ;;
    '') die "$P/toolmanifest.vdf names no require_tool_appid: which runtime $PROTON needs is unknown" ;;
    *) die "$PROTON requires tool $appid, a runtime this installer does not know" ;;
esac
[ -f "$COMMON/$RUNTIME/_v2-entry-point" ] ||
    die "$PROTON requires $RUNTIME (app $appid), which is not installed in the x86 Steam root"
[ -f "$FEXDIR/$FEXBIN" ] || die "no FEX at $FEXDIR/$FEXBIN (scripts/setup.sh builds it)"

conf="# Written by scripts/install-fex-proton-tool.sh; read by steamarm-fex-proton.
# Host paths are the runtime's /tmp links (scripts/env-links.sh).
X86_ROOT=$X86_LINK
X86_HOME=$X86_HOME
X86_STEAM=$X86_STEAM
PROTON=$PROTON
RUNTIME=$RUNTIME
FEXDIR=$FEXDIR
FEXBIN=$FEXBIN"
if [ "$DRY" = 1 ]; then
    echo "would install tools/steamarm-fex-proton into $DEST with:"
    printf '%s\n' "$conf"
    exit 0
fi
mkdir -p "$DEST"
for f in steamarm-fex-proton toolmanifest.vdf compatibilitytool.vdf; do
    cp -f "tools/steamarm-fex-proton/$f" "$DEST/$f"
done
chmod 755 "$DEST/steamarm-fex-proton"
printf '%s\n' "$conf" > "$DEST/steamarm-fex-proton.conf"
echo "installed $DEST"
echo "  x86 $PROTON + $RUNTIME (app $appid) from $X86_LINK, FEX $FEXDIR/$FEXBIN"
echo "  restart the client so that it rereads compatibilitytools.d (whether it lists the tool before a sign-in: docs/FEX_GAME_BOUNDARY.md)"
