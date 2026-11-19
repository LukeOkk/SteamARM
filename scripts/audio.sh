#!/bin/sh
# Compatibility entrypoint; PulseAudio lifecycle and routing are implemented in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${STEAMARM_AUDIO_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-audio" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-audio"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v cargo >/dev/null 2>&1 || {
        echo "audio: Rust audio tool missing (install Rust or run make audio)" >&2
        exit 1
    }
    cargo build --offline --locked --release \
        --manifest-path "$ROOT/tools/audio/Cargo.toml" \
        --target-dir "$ROOT/build/rust-audio"
    install -m 755 "$ROOT/build/rust-audio/release/steamarm-audio" \
        "$ROOT/build/steamarm-audio"
    BIN=$ROOT/build/steamarm-audio
fi
exec env STEAMARM_PROJECT_ROOT="$ROOT" "$BIN" "$@"
