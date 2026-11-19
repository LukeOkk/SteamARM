#!/bin/sh
# Compatibility entrypoint. Runtime setup and FEXServer lifecycle are in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${STEAMARM_FEX_RUNNER_BIN:-}
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-fex-runner" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-fex-runner"; do
        if [ -x "$candidate" ]; then
            BIN=$candidate
            break
        fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "run-fex: Rust runner missing (install Rust or run make fex-runner)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/fex-runner/Cargo.toml" \
        --target-dir "$ROOT/build/rust-fex-runner"
    install -m 755 "$ROOT/build/rust-fex-runner/release/steamarm-fex-runner" \
        "$ROOT/build/steamarm-fex-runner"
    BIN=$ROOT/build/steamarm-fex-runner
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
