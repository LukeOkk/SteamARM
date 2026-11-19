#!/bin/sh
# Compatibility entrypoint; FEX host build orchestration is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_FEX_HOST_BUILD_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-fex-host-build" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-fex-host-build"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "build-fex-host: Rust tool missing (install Rust or run make fex-host-build)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/fex-host-build/Cargo.toml" \
        --target-dir "$ROOT/build/rust-fex-host-build"
    install -m 755 "$ROOT/build/rust-fex-host-build/release/steamarm-fex-host-build" \
        "$ROOT/build/steamarm-fex-host-build"
    BIN=$ROOT/build/steamarm-fex-host-build
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
