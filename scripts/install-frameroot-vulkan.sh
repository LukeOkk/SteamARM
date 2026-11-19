#!/bin/bash
# Thin source-checkout entry point; installer logic lives in Rust.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMARM_FRAME_VULKAN_BIN:-$REPO/build/steamarm-frame-root-vulkan}"
if [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "install-frameroot-vulkan: Rust is missing (install rustup/cargo)" >&2
        exit 1
    }
    cargo build --offline --locked --release --bin steamarm-frame-root-vulkan \
        --manifest-path "$REPO/tools/frame-root-utils/Cargo.toml" \
        --target-dir "$REPO/build/rust-frame-root-utils"
    install -m 755 "$REPO/build/rust-frame-root-utils/release/steamarm-frame-root-vulkan" \
        "$REPO/build/steamarm-frame-root-vulkan"
    BIN="$REPO/build/steamarm-frame-root-vulkan"
fi
cd "$REPO"
exec "$BIN" "$@"
