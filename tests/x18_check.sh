#!/bin/sh
# Audit saved llvm-objdump listings (directory X18_LISTINGS, default
# /tmp/x18-listings). No guest image or runtime is executed.
set -eu
cd "$(dirname "$0")/.."
root=${X18_LISTINGS:-/tmp/x18-listings}
dump_n=${X18_DUMP_N:-1}
if [ "${1:-}" = --dump ]; then
    [ "$#" -ge 2 ] || { printf 'usage: %s [--dump N] [ALL SITES]\n' "$0" >&2; exit 2; }
    dump_n=$2
    shift 2
fi
all=${1:-$root/all_insns_raw.txt}
sites=${2:-$root/x18_sites_raw.txt}
llvm=${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
out=build/x18-dumps
make build/x18_check
build/x18_check --self-test
mkdir -p "$out" build/x18-extra-dumps
status=0
build/x18_check "$all" > build/x18-all.txt || status=$?
cat build/x18-all.txt
build/x18_check --dump "$dump_n" --dump-dir "$out" "$sites" > build/x18-sites.txt || status=$?
cat build/x18-sites.txt
# A first-per-mnemonic sample may miss SP addressing. Preserve the first real
# examples of each explicitly requested SP form in a separate supplemental set.
awk '$4 == "add" && /x18, sp,/ && !a++ { print }
     $4 == "ldr" && /\[sp/ && !l++ { print }
     $4 == "str" && /\[sp/ && !s++ { print }' "$sites" > build/x18-extra-sites.txt
build/x18_check --dump 1 --dump-dir build/x18-extra-dumps build/x18-extra-sites.txt > build/x18-extra.txt || status=$?
# LLVM objdump does not accept GNU's -b binary (tested, LLVM 23.1.1).
# Try the exact requested command once; if unavailable, wrap the unchanged
# little-endian bytes in an AArch64 ELF .text section using llvm-objcopy.
first=$(awk 'NR == 1 { print $1 }' "$out/manifest.tsv")
raw=no
if [ -n "$first" ] && "$llvm/llvm-objdump" -D -b binary --triple=aarch64 "$first" > build/x18-raw-probe.txt 2>&1; then
    raw=yes
fi
: > build/x18-disassembly.txt
for manifest in "$out/manifest.tsv" build/x18-extra-dumps/manifest.tsv; do
    while IFS="$(printf '\t')" read -r binary word indices original; do
        [ -n "$binary" ] || continue
        printf '\n%s\noriginal word: %s; %s\n%s\n' "$binary" "$word" "$indices" "$original" >> build/x18-disassembly.txt
        if [ "$raw" = yes ]; then
            "$llvm/llvm-objdump" -D -b binary --triple=aarch64 --adjust-vma=0x20000000 "$binary" >> build/x18-disassembly.txt
        else
            "$llvm/llvm-objcopy" -I binary -O elf64-littleaarch64 \
                --rename-section .data=.text,alloc,load,readonly,code,contents "$binary" "${binary%.bin}.o"
            "$llvm/llvm-objdump" -D --section=.text --triple=aarch64 --adjust-vma=0x20000000 \
                "${binary%.bin}.o" >> build/x18-disassembly.txt
        fi
    done < "$manifest"
done
printf '\nReports: build/x18-{all,sites,disassembly}.txt\n'
printf 'Audit exit=%s (nonzero denotes decoder gaps or unexplained failures).\n' "$status"
exit "$status"
