#!/bin/bash
# Thin bootstrap for the Rust installer (tools/setup). Release apps bundle the
# installer; source checkouts build it locally after installing Rust with brew.
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
REPO="$(pwd)"
export STEAMARM_PROJECT_DIR="$REPO"

SETUP_BIN="${STEAMARM_SETUP_BIN:-$REPO/build/steamarm-setup}"
if [ ! -x "$SETUP_BIN" ]; then
    if ! command -v cargo >/dev/null 2>&1; then
        BREW="${BREW:-/opt/homebrew}"
        if [ ! -x "$BREW/bin/brew" ]; then
            echo "[setup] Rust bootstrap requires Homebrew at $BREW (https://brew.sh)" >&2
            exit 1
        fi
        "$BREW/bin/brew" install rust
    fi
    cargo build --offline --locked --release --manifest-path "$REPO/tools/setup/Cargo.toml" \
        --target-dir "$REPO/build/rust-setup"
    install -m 755 "$REPO/build/rust-setup/release/steamarm-setup" \
        "$REPO/build/steamarm-setup"
    SETUP_BIN="$REPO/build/steamarm-setup"
fi

exec "$SETUP_BIN" "$@"
