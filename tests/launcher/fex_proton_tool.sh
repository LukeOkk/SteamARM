#!/bin/bash
# "SteamARM: Proton x86 via FEX" (tools/steamarm-fex-proton) and its installer
# (scripts/install-fex-proton-tool.sh) on fake roots: what gets installed and
# refused, and -- in the tool's dry run -- the command, the root switch, the
# binds of the ARM64 client's paths and the environment it hands to FEX.
# No guest runs and nothing real is touched; tests/win/run_fex_boundary.sh is
# the measurement on the Mac.
set -u
cd "$(dirname "$0")/../.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0 fail=0
check() { if eval "$2"; then echo "  ok    $1"; pass=$((pass+1)); else echo "  FAIL  $1"; fail=$((fail+1)); fi; }

elf() {   # elf FILE E_MACHINE: a file with an ELF header naming that machine
    /usr/bin/python3 -c 'import sys; open(sys.argv[1],"wb").write(b"\x7fELF"+bytes(14)+int(sys.argv[2]).to_bytes(2,"little")+bytes(44))' "$1" "$2"
}
X="$T/x86" C="$T/x86/tmp/fexhome/.local/share/Steam/steamapps/common"
proton() {   # proton NAME APPID E_MACHINE
    mkdir -p "$C/$1/files/bin"; echo '#!/usr/bin/env python3' > "$C/$1/proton"
    elf "$C/$1/files/bin/wine" "$3"
    printf '"manifest"\n{\n  "version" "2"\n  "commandline" "/proton %%verb%%"\n  "require_tool_appid" "%s"\n}\n' "$2" > "$C/$1/toolmanifest.vdf"
}
proton "Proton - Experimental" 4183110 62
proton "Proton 10.0" 1628350 3
proton "Proton Hotfix" 4183110 62
proton "Proton Mystery" 999 62
proton "Proton Aarch" 4183110 183
mkdir -p "$C/Proton Arm/files/bin-arm64"; proton "Proton Arm" 4183110 62
for r in SteamLinuxRuntime_4 SteamLinuxRuntime_sniper; do mkdir -p "$C/$r"; echo x > "$C/$r/_v2-entry-point"; done
mkdir -p "$T/fex" "$T/state/armroot/tmp/armhome/.local/share/Steam/steamrtarm64"
echo x > "$T/fex/FEX-gb"; echo x > "$T/state/armroot/tmp/armhome/.local/share/Steam/steamrtarm64/steam"
DEST="$T/state/armroot/tmp/armhome/.local/share/Steam/compatibilitytools.d/steamarm-fex-proton"
inst() { STEAMARM_STATE="$T/state" X86_LINK="$X" FEXDIR="$T/fex" NO_ENV_LINKS=1 scripts/install-fex-proton-tool.sh "$@" >"$T/inst.log" 2>&1; }

# 1. the installer
inst; rc=$?
check "installs the default, Proton - Experimental (rc $rc)" \
    '[ $rc -eq 0 ] && [ -x "$DEST/steamarm-fex-proton" ] && [ -f "$DEST/toolmanifest.vdf" ] && [ -f "$DEST/compatibilitytool.vdf" ]'
check "its conf names the x86 root link, Proton, its runtime and FEX" \
    'grep -qx "X86_ROOT=$X" "$DEST/steamarm-fex-proton.conf" && grep -qx "PROTON=Proton - Experimental" "$DEST/steamarm-fex-proton.conf" &&
     grep -qx "RUNTIME=SteamLinuxRuntime_4" "$DEST/steamarm-fex-proton.conf" && grep -qx "FEXDIR=$T/fex" "$DEST/steamarm-fex-proton.conf"'
check "the manifest runs the tool with the verb, and requires no Steam runtime of the client's" \
    'grep -q "\"commandline\" \"/steamarm-fex-proton %verb%\"" "$DEST/toolmanifest.vdf" && ! grep -q require_tool_appid "$DEST/toolmanifest.vdf"'
check "the list entry says what it is: Windows to Linux, experimental" \
    'grep -q "\"display_name\" \"SteamARM: Proton x86 via FEX (experimental)\"" "$DEST/compatibilitytool.vdf" &&
     grep -q "\"from_oslist\" \"windows\"" "$DEST/compatibilitytool.vdf" && grep -q "\"to_oslist\" \"linux\"" "$DEST/compatibilitytool.vdf"'
inst --proton "Proton 10.0"; rc=$?
check "Proton 10.0 (measured not to start) is refused without --unverified (rc $rc)" \
    '[ $rc -ne 0 ] && grep -q "measured NOT to start" "$T/inst.log" && grep -qx "PROTON=Proton - Experimental" "$DEST/steamarm-fex-proton.conf"'
inst --proton "Proton 10.0" --unverified; rc=$?
check "with --unverified it installs, with a warning, and the sniper runtime (rc $rc)" \
    '[ $rc -eq 0 ] && grep -q "^warning:" "$T/inst.log" && grep -qx "RUNTIME=SteamLinuxRuntime_sniper" "$DEST/steamarm-fex-proton.conf"'
inst --proton "Proton Hotfix"; rc=$?
check "an unmeasured Proton needs --unverified too (rc $rc)" '[ $rc -ne 0 ] && grep -q "not measured" "$T/inst.log"'
inst --proton "Proton 11.0 (ARM64)" --unverified; rc=$?
check "an ARM64 Proton by name is refused, even --unverified (rc $rc)" '[ $rc -ne 0 ] && grep -q "cannot run on macOS" "$T/inst.log"'
inst --proton "Proton Arm" --unverified; rc=$?
check "an ARM64 build (files/bin-arm64) is refused (rc $rc)" '[ $rc -ne 0 ] && grep -q "ARM64 build" "$T/inst.log"'
inst --proton "Proton Aarch" --unverified; rc=$?
check "a Wine that is not x86 is refused (rc $rc)" '[ $rc -ne 0 ] && grep -q "not an x86 ELF" "$T/inst.log"'
inst --proton "Proton Mystery" --unverified; rc=$?
check "a runtime it does not know is refused (rc $rc)" '[ $rc -ne 0 ] && grep -q "does not know" "$T/inst.log"'
inst --proton "../x"; rc=$?
check "a path for --proton is refused (rc $rc)" '[ $rc -ne 0 ] && grep -q "not a path" "$T/inst.log"'
inst --root "$T/nowhere"; rc=$?
check "a root without the native client is refused (rc $rc)" '[ $rc -ne 0 ] && grep -q "no native arm64 client" "$T/inst.log"'
inst; rc=$?
check "reinstalling the default is idempotent (rc $rc)" '[ $rc -eq 0 ] && grep -qx "RUNTIME=SteamLinuxRuntime_4" "$DEST/steamarm-fex-proton.conf"'

# 2. the tool's dry run, from "/" (not bound: a system path) with Steam's environment
G=/tmp/armhome/.local/share/Steam
tool() {   # tool [VAR=VALUE...] -- ARGS...: the installed tool, as the client would run it
    local e=()
    while [ "$1" != -- ]; do e+=("$1"); shift; done; shift
    (cd / && env -i PATH=/usr/bin:/bin STEAMARM_FEX_PROTON_DRYRUN=1 LXRT_ROOT=/tmp/lxrt-armroot HOME=/tmp/armhome \
        STEAM_COMPAT_CLIENT_INSTALL_PATH=$G STEAM_COMPAT_DATA_PATH=$G/steamapps/compatdata/42 \
        STEAM_COMPAT_INSTALL_PATH="$G/steamapps/common/A Game" STEAM_COMPAT_LIBRARY_PATHS=$G/steamapps \
        LXRT_GUEST_PAGE=4096 LXRT_X18_ALL_TEXT=libcef.so LD_LIBRARY_PATH=$G/steamrtarm64 LD_PRELOAD=$G/o.so \
        STEAM_COMPAT_EMULATOR=/e.json SteamAppId=42 ${e[@]+"${e[@]}"} \
        /bin/bash "$DEST/steamarm-fex-proton" "$@")
}
out=$(tool -- waitforexitandrun "$G/steamapps/common/A Game/game.exe" "arg 1" 2>"$T/err"); rc=$?
S="/tmp/fexhome/.local/share/Steam/steamapps/common"
check "dry run exits 0 (rc $rc)" '[ $rc -eq 0 ]'
check "its log line names the verb, the program, the app and the Proton" \
    'grep -q "^steamarm-fex-proton: waitforexitandrun game.exe (app 42) via x86 Proton - Experimental + SteamLinuxRuntime_4 under FEX" "$T/err"'
check "it execs FEX on x86 bash, the SLR entry point and Proton, verb and arguments kept" \
    'grep -qxF "exec: [$T/fex/FEX-gb] [/bin/bash] [$S/SteamLinuxRuntime_4/_v2-entry-point] [--verb=waitforexitandrun] [--] [$S/Proton - Experimental/proton] [waitforexitandrun] [$G/steamapps/common/A Game/game.exe] [arg 1]" <<<"$out"'
check "the runtime's root becomes the x86 Steam root, FEX's rootfs its /" \
    'grep -qxF "env: LXRT_ROOT=$X" <<<"$out" && grep -qxF "env: FEX_ROOTFS=/" <<<"$out" && grep -qxF "env: HOME=/tmp/fexhome" <<<"$out"'
check "the client's HOME is bound from the ARM64 root, and only once" \
    'grep -qxF "env: LXRT_MOUNTS=/tmp/armhome|/tmp/lxrt-armroot/tmp/armhome|0;" <<<"$out"'
check "the client's own settings and libraries do not reach FEX" '! grep -q "still set" <<<"$out"'
check "the tool paths are the x86 Proton and runtime" \
    'grep -qxF "env: STEAM_COMPAT_TOOL_PATHS=$S/Proton - Experimental:$S/SteamLinuxRuntime_4" <<<"$out"'
check "sound defaults to the x86 root's PulseAudio socket" 'grep -qxF "env: PULSE_SERVER=unix:/tmp/pulse/native" <<<"$out"'
check "it names the checks it would make on the x86 side" 'grep -q "^check: .*FEXServer socket" <<<"$out"'
out=$(tool STEAM_COMPAT_LIBRARY_PATHS=$G/steamapps:/mnt/games/steamapps:/usr/share -- run /mnt/games/steamapps/common/B/b.exe 2>"$T/err")
check "a library outside HOME is bound too; a system path is not" \
    'grep -qxF "env: LXRT_MOUNTS=/tmp/armhome|/tmp/lxrt-armroot/tmp/armhome|0;/mnt/games/steamapps|/tmp/lxrt-armroot/mnt/games/steamapps|0;" <<<"$out" &&
     grep -q "not binding STEAM_COMPAT_LIBRARY_PATHS .*/usr/share.*system path" "$T/err"'
out=$(tool HOME=/tmp/fexhome STEAM_COMPAT_CLIENT_INSTALL_PATH=/tmp/fexhome/x -- run x.exe 2>"$T/err")
check "the x86 side's own home is never bound over" \
    '! grep -q "fexhome|" <<<"$out" && grep -q "the x86 side" "$T/err"'
printf '# comment\n__GLX_VENDOR_LIBRARY_NAME=mesa\nLIBGL_ALWAYS_INDIRECT=1\nNOT_SET_HERE=1\nnot a line\n' > "$T/root-env"
out=$(tool STEAMARM_FEX_PROTON_ROOT_ENV="$T/root-env" LIBGL_ALWAYS_INDIRECT=1 __GLX_VENDOR_LIBRARY_NAME=mesa -- run x.exe 2>/dev/null)
check "the ARM64 root's own guest environment (the Frame root's GLX) is not passed on" \
    'grep -qxF "dropped from the ARM64 root'"'"'s guest environment: __GLX_VENDOR_LIBRARY_NAME LIBGL_ALWAYS_INDIRECT" <<<"$out" && ! grep -q "still set" <<<"$out"'
out=$(tool STEAMARM_FEXOPT_TSOENABLED=0 STEAMARM_FEXOPT_MULTIBLOCK=1 -- run x.exe 2>/dev/null)
check "the launcher's Processor settings (STEAMARM_FEXOPT_*) reach FEX as FEX_*" \
    'grep -qxF "env: FEX_TSOENABLED=0" <<<"$out" && grep -qxF "env: FEX_MULTIBLOCK=1" <<<"$out" && ! grep -q "FEXOPT" <<<"$out"'
out=$(tool STEAMARM_FEX_KEEP_LD_PRELOAD=1 -- run x.exe 2>/dev/null)
check "STEAMARM_FEX_KEEP_LD_PRELOAD=1 keeps the overlay's LD_PRELOAD" 'grep -qxF "env: LD_PRELOAD still set" <<<"$out"'
tool LXRT_ROOT= -- run x.exe >/dev/null 2>&1; rc=$?
check "outside the runtime (no LXRT_ROOT) it refuses (rc $rc)" '[ $rc -eq 6 ]'
tool LXRT_ROOT="$X" -- run x.exe >/dev/null 2>&1; rc=$?
check "from the x86 root itself it refuses (rc $rc)" '[ $rc -eq 6 ]'
tool -- 'run;x' x.exe >/dev/null 2>&1; rc=$?
check "a verb that is not a word is refused (rc $rc)" '[ $rc -eq 2 ]'
tool -- >/dev/null 2>&1; rc=$?
check "no verb: usage (rc $rc)" '[ $rc -eq 2 ]'
cp "$DEST/steamarm-fex-proton.conf" "$T/conf.saved"
echo "PROTON=Proton 11.0 (ARM64)" >> "$DEST/steamarm-fex-proton.conf"
tool -- run x.exe >/dev/null 2>&1; rc=$?
check "a conf naming an ARM64 Proton is refused (rc $rc)" '[ $rc -eq 6 ]'
cp "$T/conf.saved" "$DEST/steamarm-fex-proton.conf"; echo "SHELL=/bin/sh" >> "$DEST/steamarm-fex-proton.conf"
tool -- run x.exe >/dev/null 2>&1; rc=$?
check "an unknown conf key is refused (rc $rc)" '[ $rc -eq 3 ]'
cp "$T/conf.saved" "$DEST/steamarm-fex-proton.conf"; echo "FEXDIR=relative/dir" >> "$DEST/steamarm-fex-proton.conf"
tool -- run x.exe >/dev/null 2>&1; rc=$?
check "a relative path in the conf is refused (rc $rc)" '[ $rc -eq 3 ]'
cp "$T/conf.saved" "$DEST/steamarm-fex-proton.conf"

# 3. uninstall removes the tool's directory and nothing else
inst --uninstall; rc=$?
check "uninstall removes the tool, keeps the client (rc $rc)" \
    '[ $rc -eq 0 ] && [ ! -e "$DEST" ] && [ -f "$T/state/armroot/tmp/armhome/.local/share/Steam/steamrtarm64/steam" ]'

echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
