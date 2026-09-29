#!/bin/bash
# The root Valve's native arm64 Steam client runs in (SteamARM64): Fedora 43
# aarch64 packages, no emulation. The client (steamrtarm64/steam) and its
# libraries are aarch64 ELF; they need a desktop's worth of system libraries
# (GTK 2, NSS, X11, audio, SDL2...), which the minimal FEX root does not have.
# The package set is the shared-library closure of the libraries the client
# links (seeded by soname), resolved by scripts/mkroot-rpm.sh into its own
# lock (scripts/mkarmroot.lock) and stage.
#
#   scripts/mkarmroot.sh [out-dir]      default $STEAMARM_STATE/armroot
#   scripts/mkarmroot.sh --stage-only   refresh RPM lock/stage, preserve live root
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
OUT="${1:-$STATE/armroot}"
STAGE_ONLY=0
if [ "${1:-}" = --stage-only ]; then STAGE_ONLY=1; OUT="${2:-$STATE/armroot}"; fi
SONAMES="libasound.so.2 libasyncns.so.0 libatk-1.0.so.0 libatk-bridge-2.0.so.0 libatspi.so.0
 libbz2.so.1 libcairo.so.2 libcrypto.so.3 libcups.so.2 libdbus-1.so.3 libEGL.so.1
 libfontconfig.so.1 libgdk_pixbuf-2.0.so.0 libgio-2.0.so.0 libgobject-2.0.so.0
 libgtk-x11-2.0.so.0 libibus-1.0.so.5 libICE.so.6 libnm.so.0 libnspr4.so libnss3.so
 libnssutil3.so libopenal.so.1 libpango-1.0.so.0 libpipewire-0.3.so.0 libpulse.so.0
 libSDL2-2.0.so.0 libSM.so.6 libsmime3.so libsndfile.so.1 libssh2.so.1 libssl.so.3
 libsystemd.so.0 libudev.so.1 libva.so.2 libvdpau.so.1 libXcomposite.so.1 libXdamage.so.1
 libXfixes.so.3 libXi.so.6 libXinerama.so.1 libxkbcommon.so.0 libXrandr.so.2 libXrender.so.1
 libXtst.so.6 libGL.so.1 libvulkan.so.1 libdrm.so.2 libgbm.so.1 libxcb.so.1 libX11-xcb.so.1"
SEEDS="glibc libgcc libstdc++ bash coreutils-single util-linux-core ca-certificates nss-softokn nss-softokn-freebl p11-kit-trust
 fontconfig dejavu-sans-fonts dejavu-sans-mono-fonts xkeyboard-config mesa-dri-drivers
 mesa-vulkan-drivers openssl mesa-libGL mesa-libEGL pciutils"
for s in $SONAMES; do SEEDS="$SEEDS so:$s"; done
SEEDS=$(echo $SEEDS)                  # one line: mkroot-rpm reads a single line
STAGE="$BUILD/armstage-f43"
MKROOT_SEEDS="$SEEDS" MKROOT_LOCK="$PWD/scripts/mkarmroot.lock" MKROOT_STAGE="$STAGE" \
    MKROOT_STAGE_ONLY=1 scripts/mkroot-rpm.sh $([ -f scripts/mkarmroot.lock ] || echo --relock) "$BUILD/armroot-unused"
if [ "$STAGE_ONLY" -eq 1 ]; then echo "armroot stage ready: $STAGE"; exit 0; fi
# The root is the stage (APFS clones: no extra space) plus what a booted
# system would have written.
if [ -e "$OUT" ] && [ ! -f "$OUT/.lxrt-armroot" ]; then
    echo "refusing: $OUT exists and was not made by this script" >&2; exit 1
fi
rm -rf "$OUT"
cp -Rc "$STAGE" "$OUT"
touch "$OUT/.lxrt-armroot"
mkdir -p "$OUT/etc" "$OUT/tmp" "$OUT/dev/shm" "$OUT/run"
cp /etc/hosts "$OUT/etc/hosts" 2>/dev/null || true
printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$OUT/etc/resolv.conf"
# Steam's ARM64 webhelper asks glibc/ICU for /etc/localtime and the zoneinfo
# tree during startup. The soname-based RPM closure does not pull in tzdata;
# without it the helper exits before the UI can initialize.
if [ ! -d "$OUT/usr/share/zoneinfo" ]; then
    mkdir -p "$OUT/usr/share"
    cp -Rc /usr/share/zoneinfo "$OUT/usr/share/zoneinfo"
fi
host_zone=$(readlink /etc/localtime 2>/dev/null || true)
host_zone=${host_zone##*/zoneinfo/}
[ -f "$OUT/usr/share/zoneinfo/$host_zone" ] || host_zone=Etc/UTC
[ -e "$OUT/etc/localtime" ] || ln -s "../usr/share/zoneinfo/$host_zone" "$OUT/etc/localtime"
# TLS roots: Fedora generates /etc/pki/ca-trust/extracted with update-ca-trust
# (a scriptlet, not run here); the Ubuntu rootfs's bundle (or macOS's)
# serves the same purpose.
# Without it the client's downloads failed with "unable to load trusted SSL
# root certificates" / "http error 0".
chmod -R u+w "$OUT/etc/pki" 2>/dev/null || true
for d in pem/tls-ca-bundle.pem openssl/ca-bundle.trust.crt pem/directory-hash/ca-bundle.crt; do
    mkdir -p "$(dirname "$OUT/etc/pki/ca-trust/extracted/$d")"
    cp "$STATE/x86-rootfs/etc/ssl/certs/ca-certificates.crt" "$OUT/etc/pki/ca-trust/extracted/$d" 2>/dev/null || cp /etc/ssl/cert.pem "$OUT/etc/pki/ca-trust/extracted/$d"
done
[ -e "$OUT/etc/machine-id" ] || uuidgen | tr -d '-' | tr 'A-Z' 'a-z' > "$OUT/etc/machine-id"
# The runtime's Vulkan shim (MoltenVK) as the system libvulkan.
[ -f build/libvulkan.so.1 ] && cp build/libvulkan.so.1 "$OUT/usr/lib64/libvulkan.so.1"
echo "armroot ready: $OUT ($(du -sh "$OUT" | cut -f1))"
