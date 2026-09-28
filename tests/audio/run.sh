#!/bin/bash
# scripts/audio.sh with a real PulseAudio server: one server, one socket per
# guest root (the Steam root and the ARM64 root are separate trees, and each
# guest looks for unix:/tmp/pulse/native inside its own root).
#
# Needs pulseaudio + pactl (Linux: the pulseaudio and pulseaudio-utils
# packages; macOS: brew install pulseaudio). The Mac's CoreAudio output is
# swapped for a null sink so the test plays nothing and runs anywhere.
#
#   tests/audio/run.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
REAL="$(dirname "$(command -v pulseaudio 2>/dev/null || echo /opt/homebrew/opt/pulseaudio/bin/pulseaudio)")"
[ -x "$REAL/pulseaudio" ] && [ -x "$REAL/pactl" ] || { echo "SKIP: pulseaudio/pactl not found"; exit 0; }

W="$(mktemp -d "${TMPDIR:-/tmp}/audio-test.XXXXXX")"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "  ok    $1"; }
bad() { fail=$((fail + 1)); echo "  FAIL  $1"; }

# audio.sh finds PulseAudio at $BREW/opt/pulseaudio/bin: wrappers there run the
# real one, with module-coreaudio-detect replaced by a null sink.
B="$W/brew/opt/pulseaudio/bin"
mkdir -p "$B" "$W/state" "$W/steamroot" "$W/arm64root" "$W/xdg"
chmod 700 "$W/xdg"
cat > "$B/pulseaudio" <<EOF
#!/bin/bash
args=()
while [ \$# -gt 0 ]; do
    if [ "\$1" = -F ]; then
        sed 's/^load-module module-coreaudio-detect\$/load-module module-null-sink/' "\$2" > "\$2.test"
        args+=(-F "\$2.test"); shift 2; continue
    fi
    args+=("\$1"); shift
done
exec "$REAL/pulseaudio" "\${args[@]}"
EOF
printf '#!/bin/bash\nexec "%s/pactl" "$@"\n' "$REAL" > "$B/pactl"
chmod +x "$B/pulseaudio" "$B/pactl"

export BREW="$W/brew" STEAMARM_STATE="$W/state" XDG_RUNTIME_DIR="$W/xdg"
a() { scripts/audio.sh "$@"; }
cleanup() { LXRT_ROOT="$W/steamroot" a stop >/dev/null 2>&1; pkill -f "pulseaudio.*$W" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT

npa() { pgrep -f "pulseaudio.*$W/state" | wc -l | tr -d ' '; }
live() { "$REAL/pactl" -s "unix:$1" info >/dev/null 2>&1; }

echo "== one server, a socket per root"
if LXRT_ROOT="$W/steamroot" a start 50 >"$W/log1" 2>&1; then ok "start for the Steam root"
else bad "start for the Steam root"; cat "$W/log1"; tail -5 "$W/state/logs/pulseaudio.log" 2>/dev/null; exit 1; fi
live "$W/steamroot/tmp/pulse/native" && ok "socket in the Steam root" || bad "Steam root socket"
LXRT_ROOT="$W/arm64root" a start 50 >"$W/log2" 2>&1 && ok "start for the ARM64 root" || { bad "start for the ARM64 root"; cat "$W/log2"; }
live "$W/arm64root/tmp/pulse/native" && ok "socket in the ARM64 root" || bad "ARM64 root socket"
[ "$(npa)" = 1 ] && ok "still one PulseAudio server" || bad "servers: $(npa)"
LXRT_ROOT="$W/arm64root" a start 50 >/dev/null 2>&1 && [ "$(npa)" = 1 ] && ok "start again: idempotent" || bad "second start"

echo "== a root without a socket reaches the server"
# The Steam root's socket is gone (as after an ARM64-only session started the
# server): volume/status must use the server, not start a second one.
LXRT_ROOT="$W/steamroot" a stop >/dev/null 2>&1
LXRT_ROOT="$W/arm64root" a start 40 >/dev/null 2>&1
LXRT_ROOT="$W/steamroot" a volume 30 >/dev/null 2>&1
[ "$(npa)" = 1 ] && ok "volume with another root's socket: no second server" || bad "servers after volume: $(npa)"
case "$(LXRT_ROOT="$W/steamroot" a status)" in *"running ($W/arm64root/tmp/pulse/native)"*) ok "status names the server's socket" ;;
    *) bad "status: $(LXRT_ROOT="$W/steamroot" a status)" ;; esac

echo "== stop"
LXRT_ROOT="$W/arm64root" a stop >/dev/null 2>&1
sleep 0.5
[ "$(npa)" = 0 ] && ok "stop ends the server" || bad "servers after stop: $(npa)"
[ ! -e "$W/state/launcher/pulse.socket" ] && ok "stop forgets the server's socket" || bad "pulse.socket left"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
