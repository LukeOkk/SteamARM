#!/bin/sh
# Compatibility entrypoint; native guest setup and exec are implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_NATIVE_RUNNER_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-run-native" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-run-native"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "run-native: Rust runner missing (install Rust or run make native-runner)" >&2
        exit 127
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/native-runner/Cargo.toml" \
        --target-dir "$ROOT/build/rust-native-runner"
    install -m 755 "$ROOT/build/rust-native-runner/release/steamarm-run-native" \
        "$ROOT/build/steamarm-run-native"
    BIN=$ROOT/build/steamarm-run-native
fi
case "$BIN" in
    /*) ;;
    *) BIN=$ROOT/$BIN ;;
esac
cd "$ROOT"
exec "$BIN" "$@"
