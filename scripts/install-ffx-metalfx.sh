#!/bin/sh
# Compatibility entrypoint; installation behavior is in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${STEAMARM_FFX_METALFX_INSTALL_BIN:-}
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-ffx-metalfx-install" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-ffx-metalfx-install"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v make >/dev/null 2>&1 || {
        echo "install-ffx-metalfx: Rust installer missing (run make ffx-metalfx-install)" >&2
        exit 127
    }
    make -s -C "$ROOT" ffx-metalfx-install
    BIN=$ROOT/build/steamarm-ffx-metalfx-install
fi
case "$BIN" in
    /*) ;;
    *) BIN=$ROOT/$BIN ;;
esac
cd "$ROOT"
exec "$BIN" "$@"
