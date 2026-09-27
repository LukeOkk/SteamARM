#!/bin/bash
# Build the i386 probes (tests/elf/i386_*.c) with clang into <root>/tmp, where
# the runtime finds them as /tmp/<name>. Freestanding probes (no libc) link
# static; the rest link against the i386 glibc of the FEX x86 rootfs.
#
# Usage: tests/elf/build_i386.sh <lxrt-root> <x86-rootfs>
set -euo pipefail
ROOT="$1" RF="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$ROOT/tmp"
for src in "$HERE"/i386_*.c; do
    name=$(basename "$src" .c)
    # A probe may ask for extra compiler flags with a "// build-flags:" line.
    extra=$(sed -n 's|^// build-flags: *||p' "$src" | head -1)
    if grep -q "extern .*printf\|libc" "$src"; then
        clang --target=i386-linux-gnu -m32 -O1 $extra -fno-pie -nostdlib -nostartfiles -fuse-ld=lld \
              -Wl,-dynamic-linker,/lib/ld-linux.so.2 -Wl,--no-pie \
              -L"$RF/lib/i386-linux-gnu" -o "$ROOT/tmp/$name" "$src" -l:libc.so.6 -l:libX11.so.6 2>/dev/null ||
        clang --target=i386-linux-gnu -m32 -O1 $extra -fno-pie -nostdlib -nostartfiles -fuse-ld=lld \
              -Wl,-dynamic-linker,/lib/ld-linux.so.2 -Wl,--no-pie \
              -L"$RF/lib/i386-linux-gnu" -o "$ROOT/tmp/$name" "$src" -l:libc.so.6
    else
        # -ffreestanding: no libc here, so clang must not turn loops into strlen
        clang --target=i386-linux-gnu -m32 -O1 $extra -ffreestanding -nostdlib -static -fno-pie \
              -fuse-ld=lld -o "$ROOT/tmp/$name" "$src"
    fi
    echo "built $name"
done
