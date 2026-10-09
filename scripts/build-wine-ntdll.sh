#!/bin/bash
# Build Wine's ntdll.dll (the PE side, ARM64X) for the native ARM64 tool from the
# Wine source of the Proton ARM64 build the tool is copied from, plus
# patches/wine-ntdll/*.patch. scripts/install-native-proton.sh puts it into the
# tool in place of Valve's when the tool's Proton is that build.
#
#   source  : ValveSoftware/wine $WINE_COMMIT (Proton experimental-11.0-20261001)
#             + patches/wine-ntdll/*.patch
#   output  : $OUT/ntdll.dll   (OUT=$STEAMARM_BUILD/wine-ntdll/out)
#
# Only ntdll.dll is built: Wine's configure runs on macOS with the unix side
# disabled as far as it goes, then the build tools and the one DLL. Valve's
# tree lacks the generated files (configure, config.h.in, the syscall tables,
# the Vulkan thunks), which are generated into the build directory, next to a
# symlink overlay of the source, so the checkout stays clean.
#
# The DLL must match the ntdll.so it runs with: install-native-proton.sh only
# installs it into the Proton build it was made from (KNOWN_VERSION there).
#
# Needs: autoconf, bison >= 3, flex, perl, python3 (Homebrew: autoconf bison
# flex) and llvm-mingw (downloaded into $STEAMARM_BUILD/toolchains when missing,
# as scripts/build-vkd3d-proton.sh does). Idempotent (stamp file).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
WINE_COMMIT=6d211aabd1d325c9990a5fd7a81b9db37caae12d
MINGW_REL=20260922
ROOT_DIR="$B/wine-ntdll"
SRC="$ROOT_DIR/src"
BUILD="$ROOT_DIR/build"
OUT="$ROOT_DIR/out"
log() { printf '[build-wine-ntdll] %s\n' "$*"; }

for t in autoconf autoheader perl python3 git curl make; do
    command -v "$t" >/dev/null || { log "missing $t (brew install autoconf bison flex)"; exit 1; }
done
BISON_DIR=""
for d in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin; do
    [ -x "$d/bison" ] && { BISON_DIR="$d"; break; }
done
[ -n "$BISON_DIR" ] || { log "missing Homebrew bison (brew install bison): macOS's bison 2.3 is too old"; exit 1; }
FLEX_DIR=""
for d in /opt/homebrew/opt/flex/bin /usr/local/opt/flex/bin; do
    [ -x "$d/flex" ] && { FLEX_DIR="$d"; break; }
done
MINGW="${LLVM_MINGW:-$B/toolchains/llvm-mingw-$MINGW_REL-ucrt-macos-universal}"
if [ ! -x "$MINGW/bin/aarch64-w64-mingw32-clang" ]; then
    log "llvm-mingw $MINGW_REL"
    mkdir -p "$B/toolchains"
    curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$MINGW_REL/llvm-mingw-$MINGW_REL-ucrt-macos-universal.tar.xz" \
        | tar -xJ -C "$B/toolchains"
fi

stamp="$WINE_COMMIT $(cat "$REPO"/patches/wine-ntdll/*.patch | shasum -a 256 | cut -c1-16)"
if [ -f "$OUT/ntdll.dll" ] && [ "$(cat "$OUT/.stamp" 2>/dev/null)" = "$stamp" ]; then
    log "up to date ($OUT)"
    exit 0
fi

if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$WINE_COMMIT" ]; then
    log "fetching Wine $WINE_COMMIT"
    rm -rf "${SRC:?}"
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" remote add origin https://github.com/ValveSoftware/wine.git
    git -C "$SRC" fetch -q --depth 1 origin "$WINE_COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
git -C "$SRC" checkout -q -- .
git -C "$SRC" clean -qfd
for p in "$REPO"/patches/wine-ntdll/*.patch; do
    git -C "$SRC" apply "$p" || { log "patch $(basename "$p") does not apply"; exit 1; }
done

rm -rf "${BUILD:?}"
mkdir -p "$BUILD"
cd "$BUILD"
export PATH="$MINGW/bin:$BISON_DIR${FLEX_DIR:+:$FLEX_DIR}:$PATH"

log "generating configure and the generated sources"
autoconf -I "$SRC" -o configure "$SRC/configure.ac"
mkdir -p include
autoheader -I "$SRC" "$SRC/configure.ac"

# Vulkan thunks and headers (make_vulkan writes next to itself).
mkdir -p .vulkan-gen/dlls/winevulkan .vulkan-gen/dlls/vulkan-1 .vulkan-gen/include/wine
cp "$SRC/dlls/winevulkan/make_vulkan" .vulkan-gen/dlls/winevulkan/make_vulkan
cp "$SRC/dlls/winevulkan/vk.xml" "$SRC/dlls/winevulkan/video.xml" "$SRC/dlls/winevulkan/winevk.xml" .vulkan-gen/dlls/winevulkan/
XDG_CACHE_HOME="$BUILD/.cache" python3 .vulkan-gen/dlls/winevulkan/make_vulkan \
    --xml "$BUILD/.vulkan-gen/dlls/winevulkan/vk.xml" --video-xml "$BUILD/.vulkan-gen/dlls/winevulkan/video.xml" \
    > make_vulkan.log 2>&1 || { tail -20 make_vulkan.log >&2; exit 1; }

# The ntdll and win32u syscall tables: make_specfiles, cut to those two.
SRC="$SRC" python3 - <<'PY'
import os
from pathlib import Path
source = Path(os.environ["SRC"])
gen = Path(".ntdll-gen")
for d in ("tools", "dlls/ntdll", "dlls/win32u"):
    (gen / d).mkdir(parents=True)
script = (source / "tools/make_specfiles").read_text()
marker = "foreach my $group (@dll_groups)\n"
assert marker in script, "make_specfiles changed"
script = script[:script.index(marker)] + (
    'update_syscalls( "ntdll", "dlls/ntdll/ntsyscalls.h", 0, '
    '{ NtQueryInformationProcess => 1, NtQuerySystemTime => 1 } );\n'
    'update_syscalls( "win32u", "dlls/win32u/win32syscalls.h", 0x1000, {} );\n'
    'exit 0;\n')
(gen / "tools/make_specfiles").write_text(script)
(gen / "dlls/ntdll/ntdll.spec").symlink_to((source / "dlls/ntdll/ntdll.spec").resolve())
(gen / "dlls/win32u/win32u.spec").symlink_to((source / "dlls/win32u/win32u.spec").resolve())
PY
(cd .ntdll-gen && perl tools/make_specfiles)

# The overlay: the source by symlink, the generated files as copies.
BUILD="$BUILD" SRC="$SRC" python3 - <<'PY'
import os, shutil
from pathlib import Path
build = Path(os.environ["BUILD"]); source = Path(os.environ["SRC"])
overlay = build / "src-overlay"; vulkan = build / ".vulkan-gen"; syscalls = build / ".ntdll-gen"
def link(src, dst):
    if dst.is_symlink() or dst.exists():
        dst.unlink()
    dst.symlink_to(src.resolve(), target_is_directory=src.is_dir())
def link_dir(src, dst, skip=()):
    dst.mkdir(parents=True)
    for child in src.iterdir():
        if child.name not in set(skip):
            link(child, dst / child.name)
overlay.mkdir()
for child in source.iterdir():
    if child.name not in {"include", "dlls", "configure", ".git"}:
        link(child, overlay / child.name)
include = overlay / "include"
include.mkdir()
for child in (source / "include").iterdir():
    if child.name not in {"config.h.in", "wine"}:
        link(child, include / child.name)
shutil.copyfile(build / "include/config.h.in", include / "config.h.in")
link_dir(source / "include/wine", include / "wine", {"vulkan.h"})
shutil.copyfile(vulkan / "include/wine/vulkan.h", include / "wine/vulkan.h")
link(source / "libs/tomcrypt/src/headers/tomcrypt.h", include / "tomcrypt.h")
link(source / "libs/tomcrypt/src/headers/tomcrypt.h", build / "include/tomcrypt.h")
dlls = overlay / "dlls"
dlls.mkdir()
for child in (source / "dlls").iterdir():
    if child.name not in {"bcrypt", "ntdll", "win32u", "winevulkan", "vulkan-1"}:
        link(child, dlls / child.name)
link_dir(source / "dlls/bcrypt", dlls / "bcrypt")
link(source / "libs/tomcrypt/src/headers/tomcrypt.h", dlls / "bcrypt/tomcrypt.h")
link_dir(source / "dlls/ntdll", dlls / "ntdll", {"ntsyscalls.h"})
shutil.copyfile(syscalls / "dlls/ntdll/ntsyscalls.h", dlls / "ntdll/ntsyscalls.h")
link_dir(source / "dlls/win32u", dlls / "win32u", {"win32syscalls.h"})
shutil.copyfile(syscalls / "dlls/win32u/win32syscalls.h", dlls / "win32u/win32syscalls.h")
outs = {"winevulkan.json", "winevulkan.spec", "vulkan_thunks.c", "vulkan_thunks.h", "loader_thunks.c", "loader_thunks.h"}
link_dir(source / "dlls/winevulkan", dlls / "winevulkan", outs)
for name in outs:
    shutil.copyfile(vulkan / "dlls/winevulkan" / name, dlls / "winevulkan" / name)
link_dir(source / "dlls/vulkan-1", dlls / "vulkan-1", {"vulkan-1.spec"})
shutil.copyfile(vulkan / "dlls/vulkan-1/vulkan-1.spec", dlls / "vulkan-1/vulkan-1.spec")
shutil.copyfile(build / "configure", overlay / "configure")
PY

log "configure (PE: arm64ec + aarch64)"
WITHOUT=""
for w in alsa capi coreaudio cups dbus ffmpeg fontconfig freetype gcrypt gettext gphoto gnutls gssapi gstreamer \
         hwloc inotify krb5 netapi opencl opengl oss pcap pcsclite piper pulse sane sdl udev unwind usb v4l2 vosk \
         vulkan wayland xcomposite xcursor xfixes xinerama xinput xinput2 xrandr xrender xshape xshm xxf86vm; do
    WITHOUT="$WITHOUT --without-$w"
done
# shellcheck disable=SC2086
./configure --srcdir="$BUILD/src-overlay" --enable-archs=arm64ec,aarch64 $WITHOUT --disable-tests \
    > configure.log 2>&1 || { tail -40 configure.log >&2; log "configure failed: $BUILD/configure.log"; exit 1; }

log "build"
J=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)
make -j"$J" tools/winebuild/winebuild tools/winegcc/winegcc tools/widl/widl tools/wrc/wrc tools/makedep > build.log 2>&1 \
    || { tail -40 build.log >&2; log "tools failed: $BUILD/build.log"; exit 1; }
make -j"$J" dlls/ntdll/aarch64-windows/ntdll.dll >> build.log 2>&1 \
    || { tail -40 build.log >&2; log "ntdll failed: $BUILD/build.log"; exit 1; }

mkdir -p "$OUT"
llvm-strip --strip-debug -o "$OUT/ntdll.dll" "$BUILD/dlls/ntdll/aarch64-windows/ntdll.dll"
printf '%s' "$stamp" > "$OUT/.stamp"
log "built $OUT/ntdll.dll ($(shasum -a 256 "$OUT/ntdll.dll" | cut -c1-12))"
