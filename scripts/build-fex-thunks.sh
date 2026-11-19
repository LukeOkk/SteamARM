#!/bin/sh
# Compatibility entrypoint; SteamARM's thunk build orchestration is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_FEX_THUNKS_BUILD_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-fex-thunks-build" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-fex-thunks-build"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "build-fex-thunks: Rust tool missing (install Rust or run make fex-thunks-build)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/fex-thunks-build/Cargo.toml" \
        --target-dir "$ROOT/build/rust-fex-thunks-build"
    install -m 755 "$ROOT/build/rust-fex-thunks-build/release/steamarm-fex-thunks-build" \
        "$ROOT/build/steamarm-fex-thunks-build"
    BIN=$ROOT/build/steamarm-fex-thunks-build
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
