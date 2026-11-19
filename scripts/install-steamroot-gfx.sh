#!/bin/sh
# Compatibility entrypoint; SteamARM graphics installation is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_STEAMROOT_GFX_INSTALL_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-steamroot-gfx-install" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-steamroot-gfx-install"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "install-steamroot-gfx: Rust tool missing (install Rust or run make steamroot-gfx-install)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/steamroot-gfx-install/Cargo.toml" \
        --target-dir "$ROOT/build/rust-steamroot-gfx-install"
    install -m 755 "$ROOT/build/rust-steamroot-gfx-install/release/steamarm-steamroot-gfx-install" \
        "$ROOT/build/steamarm-steamroot-gfx-install"
    BIN=$ROOT/build/steamarm-steamroot-gfx-install
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
