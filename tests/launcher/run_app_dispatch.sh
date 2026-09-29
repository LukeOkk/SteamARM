#!/bin/bash
# scripts/run-app.sh picks the runner from the entry's architecture
# (docs/APPLICATION_MANAGER.md): aarch64 -> scripts/run-native.sh with no FEX_*
# variables; x86_64/i386 and entries without the field (Steam included) ->
# scripts/run-fex.sh as before; anything else is refused. Uses --dry-run with a
# throw-away state directory: starts nothing. Runs on macOS and Linux.
set -u
cd "$(dirname "$0")/../.." || exit 1
W="$(mktemp -d)"; W2="$(mktemp -d)"; trap 'rm -rf "$W" "$W2"' EXIT
mkdir -p "$W/launcher"
cat > "$W/launcher/apps.json" <<'JSON'
[{"id":"arm","name":"ARM","command":["/usr/bin/hello"],"env":{},"kind":"custom","architecture":"aarch64"},
 {"id":"x86","name":"x86","command":["/opt/apps/x/run"],"root":"/tmp/lxrt-steamroot","fexRootfs":"/","env":{},"kind":"custom"},
 {"id":"i386","name":"i386","command":["/opt/apps/y/run"],"env":{},"kind":"custom","architecture":"i386"},
 {"id":"bad","name":"bad","command":["/x"],"architecture":"armv7"},
 {"id":"armx86root","name":"a","command":["/opt/apps/a/run"],"root":"/tmp/lxrt-steamroot","architecture":"aarch64"},
 {"id":"x86armroot","name":"b","command":["/opt/apps/b/run"],"root":"/tmp/lxrt-arm64root","architecture":"x86_64"},
 {"id":"armcustom","name":"c","command":["/bin/c"],"root":"/tmp/some-other-root","architecture":"aarch64"},
 {"id":"ovr","name":"ovr","command":["/opt/apps/o/run"],"root":"/tmp/lxrt-steamroot","fexRootfs":"/","env":{},"kind":"custom","overrides":{"display":"vnc","vsync":"on","bogus":"x"}},
 {"id":"steam","name":"Fake","command":["/bin/fake"],"kind":"custom"}]
JSON
echo '{"fexTSO":"fast"}' > "$W/launcher/settings.json"
mkdir -p "$W2/launcher"
echo '{"display":"vnc"}' > "$W2/launcher/settings.json"
echo '[]' > "$W2/launcher/apps.json"
pass=0 fail=0
expect_in() {  # state id pattern [negative-pattern]
    out="$(STEAMARM_STATE="$1" scripts/run-app.sh --dry-run "$2" 2>&1)"
    if printf '%s' "$out" | grep -q -- "$3" && { [ -z "${4:-}" ] || ! printf '%s' "$out" | grep -q -- "$4"; }; then
        echo "  ok    $2: $3"; pass=$((pass + 1))
    else
        echo "  FAIL  $2: expected '$3'${4:+ and no '$4'}"; printf '%s\n' "$out" | sed 's/^/        /'; fail=$((fail + 1))
    fi
}
expect() { expect_in "$W" "$@"; }
expect arm   'command:  scripts/run-native.sh /usr/bin/hello' 'FEX_'
expect arm   'LXRT_ROOT=/tmp/lxrt-arm64root'
expect x86   'command:  scripts/run-fex.sh /opt/apps/x/run'
expect x86   'FEX_TSOENABLED=1'
expect i386  'translator: FEX'
expect steam 'command:  scripts/run-fex.sh /bin/bash /tmp/fexhome/.local/share/Steam/steam.sh'
expect steam 'LXRT_ROOT=/tmp/lxrt-steamroot FEX_ROOTFS=/ DISPLAY=:2'
expect steam-arm64 'command:  scripts/run-native.sh /tmp/armhome/.local/share/Steam/steamrtarm64/steam' 'FEX_'
expect steam-arm64 'LXRT_ROOT=/tmp/lxrt-armroot DISPLAY=:2'
expect steam-arm64 'LXRT_GUEST_PAGE=4096'
expect steam-arm64 'HOME_IN_GUEST=/tmp/armhome'
expect steam-arm64 'LXRT_X18_ALL_TEXT=libcef.so'
expect steam-arm64 'translator: none, session: ZERO-VM'
expect steam-arm64-frame 'command:  scripts/run-native.sh /tmp/armhome/.local/share/Steam/steamrtarm64/steam' 'FEX_'
expect steam-arm64-frame 'LXRT_ROOT=/tmp/lxrt-arm64root DISPLAY=:2'
expect steam-arm64-frame 'LXRT_GUEST_PAGE=4096'
expect ovr 'display:  vnc (DISPLAY=:1)'
expect ovr 'VKD3D_SWAPCHAIN_PRESENT_MODE=FIFO'
expect_in "$W2" steam-arm64 'display:  native (DISPLAY=:2)'
expect_in "$W2" steam-arm64 'VNC cannot serve it'
expect_in "$W2" steam 'display:  vnc (DISPLAY=:1)'
expect bad   "architecture 'armv7'"
# LaunchPlanner's rule: the ARM64 base runs aarch64 only, the Steam root x86 only.
expect armx86root "is aarch64 but its root is /tmp/lxrt-steamroot" 'command:'
expect x86armroot "is x86_64 but its root is /tmp/lxrt-arm64root" 'command:'
expect armcustom  'LXRT_ROOT=/tmp/some-other-root'
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
