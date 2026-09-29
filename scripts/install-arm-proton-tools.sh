#!/bin/bash
# Make Valve's installed ARM64 Proton tools visible to the native ARM64 Steam
# client. Both clients keep their own steamapps metadata; APFS clones share
# unchanged data blocks without linking either client's writable tool state.
set -euo pipefail

STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
SOURCE="${STEAMARM_X86_STEAMAPPS:-$STATE/steamroot/tmp/fexhome/.local/share/Steam/steamapps}"
DEST="${STEAMARM_ARM_STEAMAPPS:-$STATE/armroot/tmp/armhome/.local/share/Steam/steamapps}"

if [ ! -d "$SOURCE/common" ] || [ "$SOURCE" = "$DEST" ]; then
    echo "ARM Proton source library missing or identical to destination: $SOURCE" >&2
    exit 1
fi
mkdir -p "$DEST/common"

install_tool() {
    local appid="$1" name="$2" manifest="appmanifest_$1.acf"
    if [ ! -d "$SOURCE/common/$name" ] || [ ! -f "$SOURCE/$manifest" ]; then
        echo "ARM Proton tool incomplete: $name ($appid)" >&2
        return 1
    fi
    if [ ! -e "$DEST/common/$name" ]; then
        # -c uses clonefile on APFS: fast, copy-on-write, and no VM or network.
        cp -cR "$SOURCE/common/$name" "$DEST/common/$name"
    fi
    # Publish the manifest only after the directory is complete. Never replace
    # metadata that the ARM client may already have updated itself.
    if [ ! -e "$DEST/$manifest" ]; then
        cp "$SOURCE/$manifest" "$DEST/$manifest"
    fi
    echo "ARM Steam tool ready: $name ($appid)"
}

# Both ARM64 Proton manifests require this container runtime. Without it the
# tools are incomplete even if their own manifests are present.
install_tool 4185400 'SteamLinuxRuntime_4-arm64'
install_tool 4427310 'Proton Experimental (ARM64)'
install_tool 4628740 'Proton 11.0 (ARM64)'
