#!/bin/bash
# Thin source-checkout entry point; installer logic lives in Rust.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMARM_FRAME_ATOMUPD_BIN:-$REPO/build/steamarm-frame-root-atomupd}"
if [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "install-frameroot-atomupd: Rust is missing (install rustup/cargo)" >&2
        exit 1
    }
    cargo build --offline --locked --release --bin steamarm-frame-root-atomupd \
        --manifest-path "$REPO/tools/frame-root-utils/Cargo.toml" \
        --target-dir "$REPO/build/rust-frame-root-utils"
    install -m 755 "$REPO/build/rust-frame-root-utils/release/steamarm-frame-root-atomupd" \
        "$REPO/build/steamarm-frame-root-atomupd"
    BIN="$REPO/build/steamarm-frame-root-atomupd"
fi
cd "$REPO"
exec "$BIN" "$@"
