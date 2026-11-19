#!/bin/sh
# Compatibility entrypoint; native Steam lifecycle is implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_STEAM_ARM64_RUNNER_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-run-steam-arm64" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-run-steam-arm64"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "run-steam-arm64: Rust runner missing (install Rust or run make steam-arm64-runner)" >&2
        exit 127
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/steam-arm64-runner/Cargo.toml" \
        --target-dir "$ROOT/build/rust-steam-arm64-runner"
    install -m 755 "$ROOT/build/rust-steam-arm64-runner/release/steamarm-run-steam-arm64" \
        "$ROOT/build/steamarm-run-steam-arm64"
    BIN=$ROOT/build/steamarm-run-steam-arm64
fi
case "$BIN" in
    /*) ;;
    *) BIN=$ROOT/$BIN ;;
esac
cd "$ROOT"
exec "$BIN" "$@"
