#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
out=${STEAMARM_BUILD:-build}
mkdir -p "$out"
RUSTFLAGS="${RUSTFLAGS:-} -L native=/opt/homebrew/opt/sdl2/lib" \
  cargo build --offline --locked --release --manifest-path tools/inputd/Cargo.toml \
    --target-dir "$out/rust-inputd"
install -m 755 "$out/rust-inputd/release/steamarm-inputd" "$out/steamarm-inputd"
install -m 755 "$out/rust-inputd/release/steamarm-inputctl" "$out/steamarm-inputctl"
