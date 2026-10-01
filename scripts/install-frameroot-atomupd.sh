#!/bin/bash
# SteamOS's update client in the Steam Frame root, answered by SteamARM.
#
#   scripts/install-frameroot-atomupd.sh [ROOT]     (default $STEAMARM_STATE/arm64root)
#
# The Steam client in the Steam Frame root checks for OS updates at start
# (/usr/bin/steamos-update check -> atomupd-manager check), and
# atomupd-manager asks atomupd-daemon over the system D-Bus. There is no
# system bus here, and no daemon: the system is SteamARM's root, updated by
# SteamARM, not by SteamOS's RAUC bundles. atomupd-manager then crashed on a
# null D-Bus connection (SIGSEGV at addr 0x8, 7 times in one start of the
# client, benchmarks/stage44) and every check failed.
#
#   usr/bin/atomupd-manager            a script answering "No update
#                                      available" (steamos-update then exits
#                                      7, its "no update" code), "idle" for
#                                      the update status, and refusing to
#                                      update or switch branches
#   usr/bin/atomupd-manager.lxrt-off   the image's own, kept
# Run again after the root is rebuilt (scripts/setup.sh does).
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ROOT="${1:-$STATE/arm64root}"
log() { echo "[install-frameroot-atomupd] $*"; }

BIN="$ROOT/usr/bin/atomupd-manager"
[ -e "$BIN" ] || { log "no atomupd-manager in $ROOT"; exit 0; }
MARK="SteamARM: answers for atomupd-daemon"
if ! grep -q "$MARK" "$BIN" 2>/dev/null; then
    mv "$BIN" "$BIN.lxrt-off"
fi
cat > "$BIN.new.$$" <<EOF
#!/bin/sh
# $MARK (scripts/install-frameroot-atomupd.sh).
# This root is SteamARM's and is updated by SteamARM, not by SteamOS: there
# is no atomupd-daemon and no system D-Bus to reach one. The image's own
# client is /usr/bin/atomupd-manager.lxrt-off.
case "\${1:-}" in
    --version) echo "atomupd-manager (SteamARM stand-in)" ;;
    check) echo "No update available" ;;
    get-update-status) echo "idle" ;;
    tracked-variant) echo "steamarm" ;;
    tracked-branch) echo "stable" ;;
    list-variants) echo "steamarm" ;;
    list-branches) echo "stable" ;;
    list-builds) ;;
    status) echo "SteamARM root: updated by SteamARM (scripts/setup.sh)" ;;
    update|custom-update|switch-variant|switch-branch|simulate-update|create-dev-conf)
        echo "This system is updated by SteamARM, not by SteamOS" >&2
        exit 1 ;;
    *) ;;
esac
exit 0
EOF
chmod 0755 "$BIN.new.$$"
mv -f "$BIN.new.$$" "$BIN"
log "atomupd-manager answered by SteamARM in $ROOT (the image's is atomupd-manager.lxrt-off)"
