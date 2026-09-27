#!/bin/bash
# Build the guest root the Steam client runs in: an x86-64 distribution tree
# as "/", plus the aarch64 side of the emulator.
#
# Why a merged root rather than FEX's rootfs overlay: pressure-vessel inspects
# the "host" through directory descriptors (openat relative to an fd of "/",
# /proc/self/fd/N paths). FEX's overlay only applies to path-based lookups,
# so with the aarch64 tree as "/" pressure-vessel found no x86 graphics
# stack and its capsule-capture-libs crashed on the failed lookups. With the
# x86 tree as "/" every view agrees, as on an x86 Linux host.
#
# The aarch64 side keeps out of the x86 tree's way:
#   /usr/lib/lxrt-emu/   FEX plus the loader and libraries it links against
#                        (FEX's PT_INTERP and DT_RPATH point here, so the
#                        runtime's bwrap interpreter can bind this one
#                        directory into every sandbox)
#   /usr/lib64/*.so*     the Fedora aarch64 libraries for Xvnc/FEXServer
#                        (the x86 tree only keeps ld-linux-x86-64.so.2 there)
#   /usr/bin/{FEX*,Xvnc,xkbcomp}
#
# Usage: scripts/mksteamroot.sh <x86-rootfs> <aarch64-root> <out> <FEX-emu>
#   x86-rootfs  e.g. FEX's Ubuntu_24_04 rootfs directory
#               (scripts/fetch-x86-rootfs.sh)
#   aarch64-root the aarch64 root, merged /usr (scripts/mkroot-rpm.sh)
#   out         the new root (created with APFS clones: no extra space)
#   FEX-emu     FEX with PT_INTERP /usr/lib/lxrt-emu/ld-linux-aarch64.so.1 and
#               DT_RPATH /usr/lib/lxrt-emu: $STEAMARM_BUILD/out/FEX-emu from
#               scripts/build-fex-host.sh (the VM era patchelf'd FEX to that)
set -euo pipefail
X86="$1" ARM="$2" OUT="$3" FEXEMU="$4"

[ -e "$OUT" ] && { echo "refusing: $OUT exists" >&2; exit 1; }
cp -Rc "$X86" "$OUT"

# aarch64 libraries (no name clashes with the x86 tree's /usr/lib64).
for f in "$ARM"/usr/lib64/*; do
    b=$(basename "$f")
    [ -e "$OUT/usr/lib64/$b" ] && { echo "skip existing usr/lib64/$b"; continue; }
    cp -Rc "$f" "$OUT/usr/lib64/"
done
cp -c "$ARM/usr/lib/ld-linux-aarch64.so.1" "$OUT/usr/lib/"
for b in FEX FEX-gb FEXServer FEXGetConfig Xvnc xkbcomp; do
    [ -e "$ARM/usr/bin/$b" ] && cp -c "$ARM/usr/bin/$b" "$OUT/usr/bin/"
done
# X data for Xvnc/xkbcomp, only where the x86 tree has nothing of that name
# (Ubuntu's usr/share/X11/xkb is a symlink into xkeyboard-config).
if [ -d "$ARM/usr/share/X11" ]; then
    mkdir -p "$OUT/usr/share/X11"
    for f in "$ARM"/usr/share/X11/*; do
        b=$(basename "$f")
        [ -e "$OUT/usr/share/X11/$b" ] || [ -L "$OUT/usr/share/X11/$b" ] || cp -Rc "$f" "$OUT/usr/share/X11/"
    done
fi
for f in resolv.conf hosts; do [ -e "$ARM/etc/$f" ] && cp "$ARM/etc/$f" "$OUT/etc/"; done

# Accounts. On Linux FEX finds /etc/passwd in the host's tree; here the host
# is macOS, so the root carries its own: root, and the Mac user's uid/gid
# (the runtime passes them through) under a generic name, home /tmp/fexhome.
# Without an entry getpwuid() fails -- lsof prints "no pwd entry for UID"
# for every process, glib falls back to "somebody".
ME_UID=$(id -u) ME_GID=$(id -g)
printf 'root:x:0:0:root:/root:/bin/bash\nsteam:x:%s:%s:Steam:/tmp/fexhome:/bin/bash\nnobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n' \
    "$ME_UID" "$ME_GID" > "$OUT/etc/passwd"
printf 'root:x:0:\nsteam:x:%s:\nnogroup:x:65534:\n' "$ME_GID" > "$OUT/etc/group"
# D-Bus and Steam read a machine id; a fresh random one per root, as
# systemd-machine-id-setup would write it.
[ -s "$OUT/etc/machine-id" ] || uuidgen | tr -d '-' | tr 'A-F' 'a-f' > "$OUT/etc/machine-id"
mkdir -p "$OUT/var/lib/dbus"
ln -sf /etc/machine-id "$OUT/var/lib/dbus/machine-id"

# The emulator prefix.
mkdir -p "$OUT/usr/lib/lxrt-emu"
cp "$FEXEMU" "$OUT/usr/lib/lxrt-emu/FEX"
for l in ld-linux-aarch64.so.1 libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6; do
    src="$ARM/usr/lib64/$l"; [ -e "$src" ] || src="$ARM/usr/lib/$l"
    cp -L "$src" "$OUT/usr/lib/lxrt-emu/$l"
done

# /dev/input must exist even while it is empty: SDL (the Steam client's input
# stack) watches it with inotify, and without it fell back to re-enumerating
# every joystick four times a second -- the client crashed in that churn.
mkdir -p "$OUT/tmp" "$OUT/dev/shm" "$OUT/dev/input"
echo "ready: LXRT_ROOT=$OUT FEX_ROOTFS=/"
