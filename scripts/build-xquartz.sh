#!/bin/bash
# Build the XQuartz X server (xorg-server hw/xquartz) natively for macOS arm64,
# without sudo, into $HOME/SteamARM-build/xquartz.
#
#   sources : $XQ_ROOT/xorg-server-<ver>        (+ patches/xquartz-*.patch)
#   prefix  : $XQ_ROOT/prefix                    (Xquartz stub, fonts, man, data)
#   bundle  : $XQ_ROOT/SteamARM-X11.app          (Contents/MacOS/X11.bin = the server)
#
# Only Homebrew formulae are installed (no casks / pkgs / sudo).
# Re-running is safe: downloads are cached, the build is incremental.
set -euo pipefail

XQ_ROOT="${XQ_ROOT:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/xquartz}"
PREFIX="$XQ_ROOT/prefix"
BREW="${BREW:-/opt/homebrew}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

XS_VER=21.1.24
XS_SHA=1a4eb36ca65cc3b1b936566d677a9786e13c11cd5806e951ac55f3f5ce3984af
FONT_PKGS=(
    "font-misc-misc-1.1.3 79abe361f58bb21ade9f565898e486300ce1cc621d5285bec26e14b6a8618fed"
    "font-cursor-misc-1.0.4 25d9c9595013cb8ca08420509993a6434c917e53ca1fec3f63acd45a19d4f982"
    "font-alias-1.0.5 9f89e217bb73e0e3636a0a493fbf8b7c995156e0c53d9a0476d201b67c2d6b6e"
)

log() { printf '[build-xquartz] %s\n' "$*"; }

fetch() { # url sha256
    local url=$1 sha=$2 f
    f="$XQ_ROOT/$(basename "$url")"
    [ -f "$f" ] || curl -fL --retry 3 -o "$f" "$url"
    echo "$sha  $f" | shasum -a 256 -c - >/dev/null || { log "sha256 mismatch: $f"; exit 1; }
}

# 1. Homebrew formulae (build deps + runtime deps + small test clients)
if [ "${SKIP_BREW:-0}" != 1 ]; then
    log "installing Homebrew formulae"
    "$BREW/bin/brew" install meson ninja pkgconf util-macros xorgproto xtrans font-util \
        libxkbfile pixman libx11 libxau libxdmcp libxext libxfixes libxfont2 mesa \
        xcb-util xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm \
        libapplewm libxinerama libxrandr autoconf automake libtool \
        xkbcomp xkeyboard-config bdftopcf mkfontscale \
        xdpyinfo xeyes xterm xwininfo xprop xmodmap
fi
export PATH="$BREW/bin:$PATH"

mkdir -p "$XQ_ROOT"
cd "$XQ_ROOT"

# 2. xorg-server source + SteamARM patches
SRC="$XQ_ROOT/xorg-server-$XS_VER"
fetch "https://www.x.org/releases/individual/xserver/xorg-server-$XS_VER.tar.xz" "$XS_SHA"
# A fresh tree whenever the set of patches changes: one patch builds on
# another's files (xquartz-remote-order on xquartz-remote-layer), so "is it
# applied already" cannot be asked of each alone -- the second setup run
# tried to apply xquartz-remote-layer again and failed (0.3.27). The build
# directory is kept; the sources get the time of extraction, so everything
# is compiled again (about two minutes).
PATCH_STAMP="$( cd "$PROJECT_DIR/patches" && cat xquartz-*.patch | shasum -a 256 | cut -d' ' -f1 )"
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.steamarm-patches" 2>/dev/null)" != "$PATCH_STAMP" ]; then
    log "fresh source tree (new or changed patches)"
    rm -rf "$XQ_ROOT/build.keep"
    [ -d "$SRC/build" ] && mv "$SRC/build" "$XQ_ROOT/build.keep"
    rm -rf "$SRC"
    tar xmf "xorg-server-$XS_VER.tar.xz"
    for p in "$PROJECT_DIR"/patches/xquartz-*.patch; do
        [ -f "$p" ] || continue
        log "applying $(basename "$p")"
        patch -d "$SRC" -p1 -N -s < "$p"
    done
    [ -d "$XQ_ROOT/build.keep" ] && mv "$XQ_ROOT/build.keep" "$SRC/build"
    echo "$PATCH_STAMP" > "$SRC/.steamarm-patches"
fi

# 3. Configure (XQuartz DDX only) + build + install
# -I$BREW/include: hw/xquartz/{GL,xpr} do not declare their gl/xfont2 deps in
# meson, so GL/gl.h (mesa) and X11/fonts/libxfont2.h are not found otherwise.
if [ ! -f "$SRC/build/build.ninja" ]; then
    log "meson setup"
    meson setup "$SRC/build" "$SRC" --prefix="$PREFIX" --buildtype=release \
        -Dc_args="-I$BREW/include" -Dobjc_args="-I$BREW/include" \
        -Dxquartz=true -Dxorg=false -Dxephyr=false -Dxnest=false -Dxvfb=false -Dxwin=false \
        -Dglamor=false -Dudev=false -Dudev_kms=false -Dhal=false -Dsystemd_logind=false \
        -Ddri1=false -Ddri2=false -Ddri3=false -Ddrm=false -Dpciaccess=false \
        -Dsecure-rpc=false -Dxselinux=false -Dlinux_apm=false -Dlinux_acpi=false \
        -Dint10=false -Dvgahw=false -Ddga=false -Dagp=false \
        -Ddocs=false -Ddevel-docs=false -Ddocs-pdf=false -Dglx=true \
        -Dxkb_dir="$BREW/share/X11/xkb" -Dxkb_bin_dir="$BREW/bin" \
        -Dxkb_output_dir="$PREFIX/var/lib/xkb" \
        -Ddefault_font_path="$PREFIX/share/fonts/X11/misc/,$PREFIX/share/fonts/X11/cursor/,built-ins" \
        -Dapple-applications-dir="$XQ_ROOT" -Dapple-application-name=SteamARM-X11 \
        -Dbundle-id-prefix=org.steamarm -Dbuilder_string=SteamARM
fi
# A Homebrew upgrade moves a formula's Cellar directory, and meson recorded
# the old one (mesa's libGL.dylib: "missing and no known rule to make it").
if ! ninja -C "$SRC/build" -n >/dev/null 2>&1; then
    log "meson setup --wipe (dependency paths changed)"
    meson setup --wipe "$SRC/build" "$SRC" >/dev/null
fi
log "ninja"
ninja -C "$SRC/build"
log "meson install"
meson install -C "$SRC/build" --quiet
mkdir -p "$PREFIX/var/lib/xkb"

# 4. Core fonts: misc-fixed (ISO10646-1 + ISO8859-1) and cursor, with aliases
FONTDIR="$PREFIX/share/fonts/X11"
if [ ! -f "$FONTDIR/misc/fonts.dir" ] || [ ! -f "$FONTDIR/cursor/fonts.dir" ]; then
    for e in "${FONT_PKGS[@]}"; do
        set -- $e
        fetch "https://www.x.org/releases/individual/font/$1.tar.xz" "$2"
        [ -d "$1" ] || tar xf "$1.tar.xz"
    done
    mkdir -p "$FONTDIR/misc" "$FONTDIR/cursor"
    work="$XQ_ROOT/fontwork"; rm -rf "$work"; mkdir -p "$work"
    mapdir="$(pkg-config --variable=mapdir fontutil 2>/dev/null || echo "$BREW/share/fonts/X11/util")"
    for bdf in "$XQ_ROOT"/font-misc-misc-1.1.3/*.bdf; do
        base=$(basename "$bdf" .bdf)
        bdftopcf -t "$bdf" | gzip -9n > "$FONTDIR/misc/$base.pcf.gz"
        # 8859-1 subset, like upstream's ucs2any rules (only for ISO10646 sources)
        if grep -q '^CHARSET_REGISTRY "ISO10646"' "$bdf"; then
            (cd "$work" && ucs2any "$bdf" "$mapdir/map-ISO8859-1" ISO8859-1 >/dev/null)
        fi
    done
    for bdf in "$work"/*.bdf; do
        [ -f "$bdf" ] || continue
        bdftopcf -t "$bdf" | gzip -9n > "$FONTDIR/misc/$(basename "$bdf" .bdf).pcf.gz"
    done
    bdftopcf -t "$XQ_ROOT/font-cursor-misc-1.0.4/cursor.bdf" | gzip -9n > "$FONTDIR/cursor/cursor.pcf.gz"
    cp "$XQ_ROOT/font-alias-1.0.5/misc/fonts.alias" "$FONTDIR/misc/fonts.alias"
    mkfontdir "$FONTDIR/misc"
    mkfontdir "$FONTDIR/cursor"
    rm -rf "$work"
fi

# 5. quartz-wm: the window manager that gives X toplevels native macOS title
#    bars, traffic lights and resizing (AppleWM extension; dock support
#    through the system's libXplugin). Pinned upstream commit + patches, in
#    this order: a QuickDraw/Xrender "Picture" typedef clash with the current
#    SDK, then _NET_WM_MOVERESIZE & co. for frameless clients (Steam's SDL3
#    windows, Chromium/CEF custom frames), then fullscreen as the whole head
#    (a game at the display's own size). Rebuilt when the patches change;
#    the link makes a new file, so a running quartz-wm keeps working and
#    picks the patches up when it is restarted.
QWM_COMMIT=3570364dd893713e6697d0d80beb61ad03e43c6c
QWM="$XQ_ROOT/quartz-wm"
QWM_PATCHES=(quartz-wm-picture.patch quartz-wm-netwm-moveresize.patch quartz-wm-fullscreen-head.patch quartz-wm-resize-fixed.patch quartz-wm-fullscreen-keep.patch)
QWM_STAMP="$QWM_COMMIT $( cd "$PROJECT_DIR/patches" && cat "${QWM_PATCHES[@]}" | shasum -a 256 | cut -d' ' -f1 )"
if [ ! -x "$QWM/src/quartz-wm" ] || [ "$(cat "$QWM/.steamarm-patches" 2>/dev/null)" != "$QWM_STAMP" ]; then
    log "building quartz-wm"
    if [ ! -d "$QWM/.git" ]; then
        git clone -q https://github.com/XQuartz/quartz-wm.git "$QWM"
    fi
    git -C "$QWM" checkout -q "$QWM_COMMIT"
    git -C "$QWM" checkout -q -- .
    for p in "${QWM_PATCHES[@]}"; do
        git -C "$QWM" apply "$PROJECT_DIR/patches/$p"
    done
    (
        cd "$QWM"
        export ACLOCAL_PATH="$BREW/share/aclocal"
        export PKG_CONFIG_PATH="$BREW/lib/pkgconfig:$BREW/share/pkgconfig"
        autoreconf -fi >/dev/null 2>&1
        ./configure --prefix="$PREFIX" --enable-xplugin-dock-support \
            --with-bundle-id-prefix=org.steamarm >/dev/null
        make -j"$(sysctl -n hw.ncpu)" >/dev/null
    )
    echo "$QWM_STAMP" > "$QWM/.steamarm-patches"
fi

# The patches this server was built with, for scripts/compat-status.py: the
# GLX fbconfig one decides whether Mesa can render direct (Zink, llvmpipe) or
# every GL client falls back to Apple's indirect OpenGL 2.1.
if [ -d "$XQ_ROOT/SteamARM-X11.app/Contents" ]; then
    mkdir -p "$XQ_ROOT/SteamARM-X11.app/Contents/Resources"
    ( cd "$PROJECT_DIR/patches" && ls xquartz-*.patch ) > "$XQ_ROOT/SteamARM-X11.app/Contents/Resources/steamarm-patches.txt"
fi

log "done"
log "server : $XQ_ROOT/SteamARM-X11.app/Contents/MacOS/X11.bin"
log "wm     : $QWM/src/quartz-wm"
log "fonts  : $(grep -c . "$FONTDIR/misc/fonts.dir") misc entries"
