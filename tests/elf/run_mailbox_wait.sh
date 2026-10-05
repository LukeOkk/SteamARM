#!/bin/bash
# Test mailbox notification, timeout, error, spurious wake and teardown.
# Does not launch a game, install a shim or modify display/game settings.
# RUN_FEX=1 also compiles/runs the same predicate through x86-64 glibc/FEX.
set -eu
cd "$(dirname "$0")/../.."
ROOT="${1:-/tmp/lxrt-arm64root}"
STAGE="${STEAMARM_BUILD:-$HOME/SteamARM-build}/rootstage-f43"
CLANG=/opt/homebrew/opt/llvm/bin/clang
GCCDIR=$(ls -d "$STAGE"/usr/lib/gcc/aarch64-redhat-linux/* | tail -1)
mkdir -p "$ROOT/tmp"
OUT=$(mktemp -d "$ROOT/tmp/mailbox-wait.XXXXXX")
GUEST="/tmp/${OUT##*/}/mailbox_wait"
"$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" \
    --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
    --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I/opt/homebrew/include -Iruntime/include \
    -o "$OUT/mailbox_wait" tests/elf/mailbox_wait.c
run_native() {
    local label=$1; shift
    local log="$OUT/$label.log"
    if ! env LXRT_ROOT="$ROOT" "$@" perl -e 'alarm 15; exec @ARGV' \
        scripts/run-native.sh "$GUEST" > "$log" 2>&1; then
        echo "FAIL $label; log: $log"; return 1
    fi
    rg '^== mailbox wait .*: ok$' "$log"
}
run_native event LXRT_VK_MAILBOX_POLL=0
run_native polling LXRT_VK_MAILBOX_POLL=1
if ! env LXRT_ROOT="$ROOT" LXRT_VK_MAILBOX_POLL=0 perl -e 'alarm 15; exec @ARGV' \
    scripts/run-native.sh "$GUEST" unsupported > "$OUT/unsupported.log" 2>&1; then
    echo "FAIL unsupported clock; log: $OUT/unsupported.log"; exit 1
fi
rg '^== mailbox wait .*: ok$' "$OUT/unsupported.log"
if [ "${RUN_FEX:-0}" = 1 ]; then
    X86ROOT="${MAILBOX_X86_ROOT:-/tmp/lxrt-steamroot}"
    SYSROOT="${STEAMARM_BUILD:-$HOME/SteamARM-build}/thunks-build/sysroot-x86_64"
    mkdir -p "$X86ROOT/tmp"
    X86OUT=$(mktemp -d "$X86ROOT/tmp/mailbox-wait.XXXXXX")
    X86GUEST="/tmp/${X86OUT##*/}/mailbox_wait"
    "$CLANG" --target=x86_64-linux-gnu --sysroot="$SYSROOT" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 \
        -ffunction-sections -fdata-sections -Wl,--gc-sections \
        -I/opt/homebrew/include -Iruntime/include \
        -o "$X86OUT/mailbox_wait" tests/elf/mailbox_wait.c
    for mode in event polling unsupported; do
        args=("$X86GUEST")
        poll=0
        [ "$mode" != polling ] || poll=1
        [ "$mode" != unsupported ] || args+=(unsupported)
        if ! env LXRT_ROOT="$X86ROOT" FEX_ROOTFS=/ LXRT_VK_MAILBOX_POLL="$poll" \
            perl -e 'alarm 30; exec @ARGV' scripts/run-fex.sh "${args[@]}" \
            > "$X86OUT/$mode.log" 2>&1; then
            echo "FAIL FEX $mode; log: $X86OUT/$mode.log"; exit 1
        fi
        rg '^== mailbox wait .*: ok$' "$X86OUT/$mode.log"
    done
    echo "FEX artifacts: $X86OUT"
fi
echo "PASS; retained artifacts: $OUT"
