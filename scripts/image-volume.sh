#!/bin/sh
# Compatibility entrypoint; sparse-image lifecycle is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_IMAGE_VOLUME_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-image-volume" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-image-volume"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "image-volume: Rust tool missing (install Rust or run make image-volume)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/image-volume/Cargo.toml" \
        --target-dir "$ROOT/build/rust-image-volume"
    install -m 755 "$ROOT/build/rust-image-volume/release/steamarm-image-volume" \
        "$ROOT/build/steamarm-image-volume"
    BIN=$ROOT/build/steamarm-image-volume
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
