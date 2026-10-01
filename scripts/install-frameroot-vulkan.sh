#!/bin/bash
# Vulkan for native aarch64 programs in the Steam Frame root
# (scripts/mkframeroot.sh): SteamARM's Vulkan shim installed as an ICD for the
# image's own Khronos loader, and the image's Qualcomm ICD set aside.
#
#   scripts/install-frameroot-vulkan.sh [ROOT]     (default $STEAMARM_STATE/arm64root)
#
#   usr/lib/libvulkan_steamarm.so                       build/libvulkan.so.1 (make shim),
#                                                       which exports the ICD entry points
#   usr/share/vulkan/icd.d/steamarm_icd.aarch64.json    its manifest
#   usr/share/vulkan/icd.d.lxrt-off/freedreno_icd.aarch64.json
#                                                       the image's, moved aside: no
#                                                       Adreno here, and with only it the
#                                                       loader said "Failed to detect any
#                                                       valid GPUs" (benchmarks/stage23)
# The image's loader (usr/lib/libvulkan.so.1) is kept. Run again after the
# shim changes (scripts/setup.sh does when the root exists).
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ROOT="${1:-$STATE/arm64root}"
log() { echo "[install-frameroot-vulkan] $*"; }

[ -d "$ROOT/usr/share/vulkan" ] || { log "no Steam Frame root at $ROOT"; exit 1; }
[ -f build/libvulkan.so.1 ] || { log "no build/libvulkan.so.1 (make shim)"; exit 1; }
[ -f "$ROOT/usr/lib/libvulkan.so.1" ] || { log "no Vulkan loader in $ROOT/usr/lib"; exit 1; }

install -m 0755 build/libvulkan.so.1 "$ROOT/usr/lib/libvulkan_steamarm.so"
mkdir -p "$ROOT/usr/share/vulkan/icd.d"
cat > "$ROOT/usr/share/vulkan/icd.d/steamarm_icd.aarch64.json" <<'EOF'
{
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": "/usr/lib/libvulkan_steamarm.so",
        "library_arch": "64",
        "api_version": "1.4.0"
    }
}
EOF
if [ -f "$ROOT/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json" ]; then
    mkdir -p "$ROOT/usr/share/vulkan/icd.d.lxrt-off"
    mv "$ROOT/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json" "$ROOT/usr/share/vulkan/icd.d.lxrt-off/"
    log "freedreno ICD moved to usr/share/vulkan/icd.d.lxrt-off"
fi
log "shim installed as an ICD in $ROOT ($(shasum -a 256 build/libvulkan.so.1 | cut -c1-12))"
