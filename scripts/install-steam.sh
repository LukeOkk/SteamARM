#!/bin/sh
# Compatibility entrypoint; Steam client installation is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_STEAM_INSTALL_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-steam-install" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-steam-install"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "install-steam: Rust tool missing (install Rust or run make steam-install)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/steam-install/Cargo.toml" \
        --target-dir "$ROOT/build/rust-steam-install"
    install -m 755 "$ROOT/build/rust-steam-install/release/steamarm-steam-install" \
        "$ROOT/build/steamarm-steam-install"
    BIN=$ROOT/build/steamarm-steam-install
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
