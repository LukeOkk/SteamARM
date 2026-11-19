#!/bin/sh
# Compatibility entrypoint; XQuartz lifecycle is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_X11_CONTROL_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-x11-control" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-x11-control"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "run-x11-native: Rust X11 tool missing (install Rust or run make x11-control)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/x11-control/Cargo.toml" \
        --target-dir "$ROOT/build/rust-x11-control"
    install -m 755 "$ROOT/build/rust-x11-control/release/steamarm-x11-control" \
        "$ROOT/build/steamarm-x11-control"
    BIN=$ROOT/build/steamarm-x11-control
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
