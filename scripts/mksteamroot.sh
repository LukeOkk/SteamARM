#!/usr/bin/env bash
# Compatibility entry point. The root assembly implementation is Rust:
# tools/steamroot-build/src/main.rs.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILDER="${STEAMARM_STEAMROOT_BUILDER:-$REPO/build/steamarm-steamroot-build}"
if [ ! -x "$BUILDER" ]; then
    command -v cargo >/dev/null || { echo "mksteamroot: Rust is missing (brew install rust)" >&2; exit 1; }
    cargo build --offline --locked --release --manifest-path "$REPO/tools/steamroot-build/Cargo.toml" \
        --target-dir "$REPO/build/rust-steamroot-build"
    BUILDER="$REPO/build/rust-steamroot-build/release/steamarm-steamroot-build"
fi
exec "$BUILDER" "$@"
