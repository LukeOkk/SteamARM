#!/bin/sh
# Compatibility entrypoint; the installer and root selection live in Rust.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TOOL_BIN="${STEAMARM_FEX_PROTON_BIN:-}"
if [ -z "$TOOL_BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-fex-proton-arm64" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-fex-proton-arm64"; do
        if [ -x "$candidate" ]; then TOOL_BIN=$candidate; break; fi
    done
fi
if [ -z "$TOOL_BIN" ] || [ ! -x "$TOOL_BIN" ]; then
    command -v make >/dev/null 2>&1 || {
        echo "install-fex-proton-tool: AArch64 Rust tool missing (run make fex-proton-tool)" >&2
        exit 127
    }
    make -s -C "$ROOT" fex-proton-tool
    TOOL_BIN=$ROOT/build/steamarm-fex-proton-arm64
fi
export STEAMARM_FEX_PROTON_BIN="$TOOL_BIN"
BIN="${STEAMARM_FEX_PROTON_INSTALL_BIN:-}"
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    for candidate in \
        "$ROOT/build/steamarm-fex-proton-install" \
        "$ROOT/build/SteamARM.app/Contents/Resources/steamarm-fex-proton-install"; do
        if [ -x "$candidate" ]; then BIN=$candidate; break; fi
    done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    command -v make >/dev/null 2>&1 || {
        echo "install-fex-proton-tool: Rust installer missing (run make fex-proton-install)" >&2
        exit 127
    }
    make -s -C "$ROOT" fex-proton-install
    BIN=$ROOT/build/steamarm-fex-proton-install
fi
case "$BIN" in
    /*) ;;
    *) BIN=$ROOT/$BIN ;;
esac
cd "$ROOT"
exec "$BIN" "$@"
