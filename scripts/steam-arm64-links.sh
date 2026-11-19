#!/bin/sh
# Compatibility entrypoint; Steam ARM64 guest links are managed in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_ARM64_LINKS_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-steam-arm64-links" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-steam-arm64-links"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "steam-arm64-links: Rust tool missing (install Rust or run make steam-arm64-links)" >&2
        exit 127
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/steam-arm64-links/Cargo.toml" \
        --target-dir "$ROOT/build/rust-steam-arm64-links"
    install -m 755 "$ROOT/build/rust-steam-arm64-links/release/steamarm-steam-arm64-links" \
        "$ROOT/build/steamarm-steam-arm64-links"
    BIN=$ROOT/build/steamarm-steam-arm64-links
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
