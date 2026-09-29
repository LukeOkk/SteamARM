#!/bin/bash
# Real-file checks for the native arm64 Steam client that never start it:
# rewrite reports from `lxrun --dry-run` and a static look at the NSS files.
# Every check skips cleanly when its files are missing (STEAMARM_STATE, ARMROOT).
set -uo pipefail
cd "$(dirname "$0")/../.."
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ARMROOT="${ARMROOT:-$STATE/armroot}"
SD="$ARMROOT/tmp/armhome/.local/share/Steam"
RT="$SD/steamrtarm64"
READELF=/opt/homebrew/opt/llvm/bin/llvm-readelf
PASS=0; FAIL=0; SKIP=0
ok() { echo "  ok    $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL  $1"; echo "        $2"; FAIL=$((FAIL+1)); }
skip() { echo "  skip  $1 (no $2)"; SKIP=$((SKIP+1)); }

echo "== tests/arm64"
if [ ! -x build/lxrun ]; then
    bad "lxrun executable" "no build/lxrun"
    echo "== $PASS passed, $FAIL failed"
    exit 1
fi

# Each report has one svc/tls line and one x18 line. Check both report pairs.
reports_ok() {
    local out=$1 expected=$2 line svc_count=0 x18_count=0
    local svc_found svc_done svc_bad tls_reads tls_writes tls_done tls_bad
    local x18_found x18_windows x18_done x18_bad x18_unreachable
    REPORT_IMAGE=""
    while IFS= read -r line; do
        if [[ "$line" =~ svc\ ([0-9]+)\ found,\ ([0-9]+)\ rewritten,\ ([0-9]+)\ poisoned ]] &&
           [[ "$line" =~ tls\ ([0-9]+)\ reads\ \+\ ([0-9]+)\ writes,\ ([0-9]+)\ rewritten,\ ([0-9]+)\ poisoned ]]; then
            # The tls regex overwrote BASH_REMATCH; capture each report again.
            [[ "$line" =~ svc\ ([0-9]+)\ found,\ ([0-9]+)\ rewritten,\ ([0-9]+)\ poisoned ]]
            svc_found=${BASH_REMATCH[1]}; svc_done=${BASH_REMATCH[2]}; svc_bad=${BASH_REMATCH[3]}
            [[ "$line" =~ tls\ ([0-9]+)\ reads\ \+\ ([0-9]+)\ writes,\ ([0-9]+)\ rewritten,\ ([0-9]+)\ poisoned ]]
            tls_reads=${BASH_REMATCH[1]}; tls_writes=${BASH_REMATCH[2]}
            tls_done=${BASH_REMATCH[3]}; tls_bad=${BASH_REMATCH[4]}
            [ "$svc_found" -eq "$svc_done" ] && [ "$svc_bad" -eq 0 ] &&
            [ $((tls_reads + tls_writes)) -eq "$tls_done" ] && [ "$tls_bad" -eq 0 ] || return 1
            svc_count=$((svc_count+1))
            if [ "$svc_count" -eq 1 ]; then REPORT_IMAGE="svc $svc_found/$svc_done, tls $((tls_reads + tls_writes))/$tls_done"; fi
        fi
        if [[ "$line" =~ x18\ ([0-9]+)\ found\ in\ ([0-9]+)\ code\ windows,\ ([0-9]+)\ rewritten,\ ([0-9]+)\ unsupported,\ ([0-9]+)\ unreachable ]]; then
            x18_found=${BASH_REMATCH[1]}; x18_windows=${BASH_REMATCH[2]}
            x18_done=${BASH_REMATCH[3]}; x18_bad=${BASH_REMATCH[4]}
            x18_unreachable=${BASH_REMATCH[5]}
            [ "$x18_found" -eq "$x18_done" ] && [ "$x18_bad" -eq 0 ] &&
            [ "$x18_unreachable" -eq 0 ] || return 1
            x18_count=$((x18_count+1))
            if [ "$x18_count" -eq 1 ]; then REPORT_IMAGE="$REPORT_IMAGE, x18 $x18_found/$x18_done"; fi
        fi
    done <<<"$out"
    [ "$svc_count" -eq "$expected" ] && [ "$x18_count" -eq "$expected" ]
}

# LIBCEF_FULL_EXECUTABLE_REWRITE_COVERAGE: full text and FDE-filter controls.
if [ -f "$RT/libcef.so" ]; then
    full=$(LXRT_X18_ALL_TEXT=libcef.so ./build/lxrun --dry-run "$RT/libcef.so" 2>&1); full_rc=$?
    filtered=$(./build/lxrun --dry-run "$RT/libcef.so" 2>&1); filtered_rc=$?
    full_x18=0; filtered_x18=0
    if [[ "$full" =~ x18\ ([0-9]+)\ found\ in\ ([0-9]+)\ code\ windows,\ ([0-9]+)\ rewritten,\ ([0-9]+)\ unsupported,\ ([0-9]+)\ unreachable ]]; then
        full_x18=${BASH_REMATCH[1]}; full_windows=${BASH_REMATCH[2]}
    fi
    if [[ "$filtered" =~ x18\ ([0-9]+)\ found\ in\ ([0-9]+)\ code\ windows,\ ([0-9]+)\ rewritten,\ ([0-9]+)\ unsupported,\ ([0-9]+)\ unreachable ]]; then
        filtered_x18=${BASH_REMATCH[1]}
    fi
    full_ok=0; filtered_ok=0
    if reports_ok "$full" 1; then full_ok=1; full_report=$REPORT_IMAGE; fi
    if reports_ok "$filtered" 1; then filtered_ok=1; fi
    if [ "$full_rc" -eq 0 ] && [ "$filtered_rc" -eq 0 ] &&
       [ "$full_ok" -eq 1 ] && [ "$filtered_ok" -eq 1 ] &&
       [ "$full_x18" -gt "$filtered_x18" ] && [ "$filtered_x18" -gt 0 ]; then
        ok "LIBCEF_FULL_EXECUTABLE_REWRITE_COVERAGE: $full_report, $full_windows windows; FDE x18 $filtered_x18, full x18 $full_x18"
    else
        bad "LIBCEF_FULL_EXECUTABLE_REWRITE_COVERAGE" "rc=$full_rc/$filtered_rc; x18 full=$full_x18 filtered=$filtered_x18; $(grep 'rewrite:' <<<"$full" | tail -2)"
    fi
else skip "LIBCEF_FULL_EXECUTABLE_REWRITE_COVERAGE" "$RT/libcef.so"; fi

# Resolve a DT_NEEDED name using the guest root, including absolute symlinks.
resolve_guest() {
    local path=$1 link i
    for ((i=0; i<32; i++)); do
        [[ "$path" == "$ARMROOT"/* ]] || return 1
        [ -e "$path" ] || [ -L "$path" ] || return 1
        [ -L "$path" ] || { RESOLVED="$path"; return 0; }
        link=$(readlink "$path") || return 1
        if [[ "$link" == /* ]]; then path="$ARMROOT$link"
        else path="$(dirname "$path")/$link"; fi
        path=$(python3 -c 'import os,sys; print(os.path.normpath(sys.argv[1]))' "$path")
    done
    return 1
}

# NSS_SOFTOKN_DISCOVERY: dlopen siblings and transitive DT_NEEDED closure.
NSS="$ARMROOT/usr/lib64/libnss3.so"
if [ -f "$NSS" ]; then
    nss_dir=$(dirname "$NSS")
    missing=()
    for name in libsoftokn3.so libfreeblpriv3.so; do
        [ -f "$nss_dir/$name" ] || missing+=("$nss_dir/$name")
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        queue=("$NSS" "$nss_dir/libsoftokn3.so" "$nss_dir/libfreeblpriv3.so")
        seen=()
        count=0
        while [ "${#queue[@]}" -gt 0 ]; do
            path=${queue[0]}; queue=("${queue[@]:1}")
            if ! resolve_guest "$path"; then missing+=("$path"); continue; fi
            path=$RESOLVED
            already=0
            for previous in "${seen[@]-}"; do
                if [ "$previous" = "$path" ]; then already=1; break; fi
            done
            [ "$already" -eq 0 ] || continue
            seen+=("$path"); count=$((count+1))
            hdr=$($READELF -hW "$path" 2>&1)
            if ! grep -q 'Class: *ELF64' <<<"$hdr" ||
               ! grep -q 'Machine: *AArch64' <<<"$hdr"; then
                missing+=("wrong ELF machine: $path"); continue
            fi
            dyn=$($READELF -dW "$path" 2>&1)
            origin=$(dirname "$path")
            paths=("$origin" "$ARMROOT/usr/lib64" "$ARMROOT/lib64")
            [ -f "$ARMROOT/lib/ld-linux-aarch64.so.1" ] && paths+=("$ARMROOT/lib")
            runpath=$(sed -nE 's/.*\((RUNPATH|RPATH)\).*\[([^]]+)\].*/\2/p' <<<"$dyn")
            IFS=: read -r -a extra <<<"$runpath"
            for dir in "${extra[@]-}"; do
                dir=${dir//\$\{ORIGIN\}/$origin}
                dir=${dir//\$ORIGIN/$origin}
                if [[ "$dir" == /usr/* || "$dir" == /lib64/* ]]; then dir="$ARMROOT$dir"; fi
                [[ "$dir" == "$ARMROOT"/* ]] && paths+=("$dir")
            done
            while IFS= read -r dep; do
                [ -n "$dep" ] || continue
                found=""
                for dir in "${paths[@]}"; do
                    if resolve_guest "$dir/$dep"; then found=$RESOLVED; break; fi
                done
                if [ -n "$found" ]; then queue+=("$found")
                else missing+=("$path needs $dep"); fi
            done <<<"$(sed -nE 's/.*\(NEEDED\).*\[([^]]+)\].*/\1/p' <<<"$dyn")"
        done
    fi
    if [ "${#missing[@]}" -eq 0 ]; then
        ok "NSS_SOFTOKN_DISCOVERY: libsoftokn3.so and libfreeblpriv3.so next to libnss3.so, $count-library DT_NEEDED closure resolves, all aarch64"
    else bad "NSS_SOFTOKN_DISCOVERY" "${missing[*]}"; fi
else skip "NSS_SOFTOKN_DISCOVERY" "$NSS"; fi

# Client image and its PT_INTERP each need clean rewrite reports.
for name in steam steamwebhelper; do
    if [ -f "$RT/$name" ]; then
        out=$(LXRT_ROOT="$ARMROOT" LXRT_GUEST_PAGE=4096 ./build/lxrun --dry-run "$RT/$name" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && reports_ok "$out" 2; then
            ok "$name rewrite coverage: $REPORT_IMAGE; image and PT_INTERP clean"
        else bad "$name rewrite coverage" "rc=$rc $(grep 'rewrite:' <<<"$out" | tail -4)"; fi
    else skip "$name rewrite coverage" "$RT/$name"; fi
done

summary="== $PASS passed, $FAIL failed"
[ "$SKIP" -eq 0 ] || summary="$summary ($SKIP skipped)"
echo "$summary"
[ "$FAIL" -eq 0 ]
