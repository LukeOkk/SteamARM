#!/bin/sh
# Measure whether macOS keeps x18 for a binary built against an SDK older than
# macOS 13 (see x18_preserve.c). Builds the same source twice and runs both.
# macOS on Apple Silicon only. Writes nothing outside build/.
set -eu
cd "$(dirname "$0")/../.."
[ "$(uname -s)-$(uname -m)" = Darwin-arm64 ] || { echo "SKIP: needs macOS on Apple Silicon"; exit 0; }
mkdir -p build
src=tests/x18_preserve/x18_preserve.c
cc="${CC:-clang}"
"$cc" -O1 -arch arm64 -Wno-inline-asm -o build/x18_preserve_cur "$src" -lpthread
"$cc" -O1 -arch arm64 -Wno-inline-asm -o build/x18_preserve_sdk12 "$src" -lpthread \
    -Wl,-platform_version,macos,12.0,12.3
codesign -f -s - build/x18_preserve_cur build/x18_preserve_sdk12 2>/dev/null
sdk() { otool -l "$1" | awk '/LC_BUILD_VERSION/{f=1} f&&/sdk/{print $2; exit}'; }
echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(sysctl -n machdep.cpu.brand_string)"
st=0
for b in cur sdk12; do
    echo "build/x18_preserve_$b  (LC_BUILD_VERSION sdk $(sdk build/x18_preserve_$b))"
    build/x18_preserve_$b || st=$((st + 1))
done
cat <<'EOF'

Expected from the public xnu source: "cur" ZEROED, "sdk12" PRESERVED.
MEASURED on macOS 27 (benchmarks/stage28-keep-x18.txt): a fork child of
the sdk12 build is ZEROED as well -- the flag is set at exec and not
inherited. lxrun linked the same way (the default since stage 28) keeps a
guest's x18 for code it never rewrote (a JIT's) in the process it exec'd,
but not in a guest's forked children (zygotes, shells), so runtime/x18.c's
rewriting stays. Record a new result in benchmarks/ with the macOS build
above.
EOF
exit 0
