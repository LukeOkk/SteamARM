#!/bin/sh
# Compatibility entrypoint; Steam launch orchestration is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${STEAMARM_STEAM_LAUNCHER_BIN:-}
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-steam-launcher" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-steam-launcher"; do
        if [ -x "$candidate" ]; then
            BIN=$candidate
            break
        fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "run-steam: Rust launcher missing (install Rust or run make steam-launcher)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/steam-launcher/Cargo.toml" \
        --target-dir "$ROOT/build/rust-steam-launcher"
    install -m 755 "$ROOT/build/rust-steam-launcher/release/steamarm-steam-launcher" \
        "$ROOT/build/steamarm-steam-launcher"
    BIN=$ROOT/build/steamarm-steam-launcher
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
