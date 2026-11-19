#!/bin/bash
# Build and run the Rust temporary-root linker for direct source-checkout use.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
REPO="$(pwd)"
BIN="${STEAMARM_ENV_LINKS_BIN:-$REPO/build/steamarm-env-links}"
if [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "env-links: Rust is missing (brew install rust)" >&2
        exit 1
    }
    cargo build --offline --locked --release --manifest-path "$REPO/tools/env-links/Cargo.toml" \
        --target-dir "$REPO/build/rust-env-links"
    install -m 755 "$REPO/build/rust-env-links/release/steamarm-env-links" \
        "$REPO/build/steamarm-env-links"
    BIN="$REPO/build/steamarm-env-links"
fi
exec "$BIN" "$@"
