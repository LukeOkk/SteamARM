#!/bin/bash
# Rust implementation is shipped as steamarm-frame-root-build.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMARM_FRAME_ROOT_BUILD_BIN:-$REPO/build/steamarm-frame-root-build}"
if [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "mkframeroot: Rust is missing (install rustup/cargo)" >&2
        exit 1
    }
    cargo build --offline --locked --release --bin steamarm-frame-root-build \
        --manifest-path "$REPO/tools/frame-root-utils/Cargo.toml" \
        --target-dir "$REPO/build/rust-frame-root-utils"
    install -m 755 "$REPO/build/rust-frame-root-utils/release/steamarm-frame-root-build" "$BIN"
fi
exec "$BIN" "$@"
