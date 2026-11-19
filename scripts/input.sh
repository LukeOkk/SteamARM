#!/bin/sh
# Compatibility entrypoint; controller service lifecycle is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${STEAMARM_INPUTCTL_BIN:-}
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-inputctl" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-inputctl" \
        "${STEAMARM_BUILD:-}/steamarm-inputctl"; do
        if [ -x "$candidate" ]; then
            BIN=$candidate
            break
        fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "input: Rust controller tool missing (install Rust or run tools/inputd/build.sh)" >&2
        exit 1
    }
    "$ROOT/tools/inputd/build.sh"
    BIN=${STEAMARM_BUILD:-$ROOT/build}/steamarm-inputctl
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
