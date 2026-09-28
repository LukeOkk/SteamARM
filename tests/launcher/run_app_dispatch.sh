#!/bin/bash
# scripts/run-app.sh picks the runner from the entry's architecture
# (docs/APPLICATION_MANAGER.md): aarch64 -> scripts/run-native.sh with no FEX_*
# variables; x86_64/i386 and entries without the field (Steam included) ->
# scripts/run-fex.sh as before; anything else is refused. Uses --dry-run with a
# throw-away state directory: starts nothing. Runs on macOS and Linux.
set -u
cd "$(dirname "$0")/../.." || exit 1
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W/launcher"
cat > "$W/launcher/apps.json" <<'JSON'
[{"id":"arm","name":"ARM","command":["/usr/bin/hello"],"env":{},"kind":"custom","architecture":"aarch64"},
 {"id":"x86","name":"x86","command":["/opt/apps/x/run"],"root":"/tmp/lxrt-steamroot","fexRootfs":"/","env":{},"kind":"custom"},
 {"id":"i386","name":"i386","command":["/opt/apps/y/run"],"env":{},"kind":"custom","architecture":"i386"},
 {"id":"bad","name":"bad","command":["/x"],"architecture":"armv7"}]
JSON
echo '{"fexTSO":"fast"}' > "$W/launcher/settings.json"
pass=0 fail=0
expect() {  # id pattern [negative-pattern]
    out="$(STEAMARM_STATE="$W" scripts/run-app.sh --dry-run "$1" 2>&1)"
    if printf '%s' "$out" | grep -q -- "$2" && { [ -z "${3:-}" ] || ! printf '%s' "$out" | grep -q -- "$3"; }; then
        echo "  ok    $1: $2"; pass=$((pass + 1))
    else
        echo "  FAIL  $1: expected '$2'${3:+ and no '$3'}"; printf '%s\n' "$out" | sed 's/^/        /'; fail=$((fail + 1))
    fi
}
expect arm   'command:  scripts/run-native.sh /usr/bin/hello' 'FEX_'
expect arm   'LXRT_ROOT=/tmp/lxrt-arm64root'
expect x86   'command:  scripts/run-fex.sh /opt/apps/x/run'
expect x86   'FEX_TSOENABLED=1'
expect i386  'translator: FEX'
expect steam 'command:  scripts/run-fex.sh /bin/bash /tmp/fexhome/.local/share/Steam/steam.sh'
expect bad   "architecture 'armv7'"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
