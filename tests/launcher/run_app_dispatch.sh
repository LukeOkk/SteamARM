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
 {"id":"kk","name":"kk","command":["/opt/apps/k/run"],"root":"/tmp/lxrt-steamroot","fexRootfs":"/","env":{},"kind":"custom","overrides":{"graphicsBackend":"vulkanKosmicKrisp","synchronization":"esync"}},
 {"id":"hs","name":"Heroic Steam","command":["/opt/apps/h/run"],"root":"/tmp/lxrt-steamroot","fexRootfs":"/","env":{},"kind":"custom"},
 {"id":"heroic","name":"Heroic Games Launcher","command":["/opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic","--no-sandbox","--disable-gpu","--js-flags=--no-opt"],"root":"/tmp/lxrt-armroot","fexRootfs":null,"env":{"HOME_IN_GUEST":"/tmp/heroichome","LXRT_X18_ALL_TEXT":"/opt/apps/heroic/"},"kind":"heroic","architecture":"aarch64","readiness":"experimental"},
 {"id":"steam","name":"Fake","command":["/bin/fake"],"kind":"custom"},
 {"id":"android-org.example.game","name":"Juego","command":[],"root":"/s/android/packages/org.example.game","kind":"android","architecture":"aarch64","android":{"package":"org.example.game"}},
 {"id":"android-cmd","name":"Juego2","command":["/system/bin/app_process64"],"kind":"android","architecture":"aarch64"},
 {"id":"android-dex","name":"Solitario","command":[],"kind":"android","architecture":"aarch64","android":{"package":"org.example.dex","abis":[],"minSdk":"11"}},
 {"id":"android-multi","name":"Multi","command":[],"kind":"android","architecture":"aarch64","android":{"package":"org.example.multi","abis":["arm64-v8a","armeabi-v7a","x86","x86_64"],"minSdk":"24"}},
 {"id":"android-arm64","name":"A64","command":[],"kind":"android","architecture":"aarch64","android":{"package":"org.example.a64","abis":["arm64-v8a"],"minSdk":"24"}},
 {"id":"android-arm32","name":"A32","command":[],"kind":"android","architecture":"armv7","android":{"package":"org.example.a32","abis":["armeabi-v7a"],"minSdk":"24"}},
 {"id":"android-i386","name":"I386","command":[],"kind":"android","architecture":"i386","android":{"package":"org.example.i386","abis":["x86"],"minSdk":"24"}},
 {"id":"android-new","name":"New","command":[],"kind":"android","architecture":"aarch64","android":{"package":"org.example.new","abis":[],"minSdk":"33"}},
 {"id":"android-meta","name":"Meta","command":[],"kind":"android","architecture":"x86_64","android":{"package":"org.example.meta"}}]
JSON
# android-pm.py's record of org.example.meta: the ABIs come from there when the entry has none.
mkdir -p "$W/android/packages/org.example.meta"
echo '{"package":"org.example.meta","abis":["x86_64"],"minSdk":21}' > "$W/android/packages/org.example.meta/meta.json"
# A Vulkan shim that reads STEAMARM_VK_ICD (settings-env.py looks for the name).
mkdir -p "$W/steamroot/usr/lib/lxrt-emu"
echo STEAMARM_VK_ICD > "$W/steamroot/usr/lib/lxrt-emu/libvulkan.so.1"
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
# Heroic ARM64 as launcher/Installers.swift writes it (HeroicARM64): native,
# the Fedora ARM64 root, its own home, x18 over all of the app's text.
expect heroic 'command:  scripts/run-native.sh /opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic --no-sandbox --disable-gpu --js-flags=--no-opt' 'FEX_'
expect heroic 'LXRT_ROOT=/tmp/lxrt-armroot DISPLAY=:2'
expect heroic 'HOME_IN_GUEST=/tmp/heroichome'
expect heroic 'LXRT_X18_ALL_TEXT=/opt/apps/heroic/'
expect heroic 'translator: none, session: ZERO-VM'
expect ovr 'display:  vnc (DISPLAY=:1)'
expect ovr 'VKD3D_SWAPCHAIN_PRESENT_MODE=FIFO'
expect_in "$W2" steam-arm64 'display:  native (DISPLAY=:2)'
expect_in "$W2" steam-arm64 'VNC cannot serve it'
expect_in "$W2" steam 'display:  vnc (DISPLAY=:1)'
expect bad   "architecture 'armv7'"
# Android apps (docs/APK_SUPPORT.md, benchmarks/stage28-android-apk.txt): the
# Android session (scripts/android-session.py run <package>) for dex-only and
# x86_64 code; everything else refused with the reason, before anything starts.
expect android-dex 'command:  scripts/android-session.py run org.example.dex'
expect android-dex 'android:  org.example.dex in the Android session'
expect android-dex 'arch:     x86_64 (translator: FEX, session: ZERO-VM)'
expect android-multi 'command:  scripts/android-session.py run org.example.multi'
expect android-meta 'command:  scripts/android-session.py run org.example.meta'
STEAMARM_DISPLAY=vnc expect android-dex 'its screen is a Weston window on the native X server, using native windows'
expect android-arm64 "cannot run: its native code is arm64-v8a only: Android's arm64 ART does not start on macOS" 'command:'
expect android-arm32 "32-bit ARM only" 'command:'
expect android-i386 "32-bit x86 only" 'command:'
expect android-new "it needs API 33; the session is Android 11 (API 30)" 'command:'
expect android-org.example.game "its ABIs are unknown" 'command:'
expect android-cmd "it names no valid package" 'command:'

# LaunchPlanner's rule: the ARM64 base runs aarch64 only, the Steam root x86 only.
expect armx86root "is aarch64 but its root is /tmp/lxrt-steamroot" 'command:'
expect x86armroot "is x86_64 but its root is /tmp/lxrt-arm64root" 'command:'
expect armcustom  'LXRT_ROOT=/tmp/some-other-root'
# The launcher's fallbacks (FallbackPolicy.environment) win over the settings:
# what its policy announced is what runs.
expect kk 'STEAMARM_VK_ICD=kosmickrisp' 'PROTON_NO_ESYNC'
expect_env() {  # VAR=VALUE id pattern [negative-pattern]
    out="$(env "$1" STEAMARM_STATE="$W" scripts/run-app.sh --dry-run "$2" 2>&1)"
    if printf '%s' "$out" | grep -q -- "$3" && { [ -z "${4:-}" ] || ! printf '%s' "$out" | grep -q -- "$4"; }; then
        echo "  ok    $1 $2: $3"; pass=$((pass + 1))
    else
        echo "  FAIL  $1 $2: expected '$3'${4:+ and no '$4'}"; printf '%s\n' "$out" | sed 's/^/        /'; fail=$((fail + 1))
    fi
}
expect_env STEAMARM_GRAPHICS_BACKEND=vulkanMoltenVK kk 'command:' 'STEAMARM_VK_ICD'
expect_env STEAMARM_SYNCHRONIZATION=wineserver kk 'PROTON_NO_ESYNC=1'

# What run-app.sh did, on its last line (the launcher reads it): with a fake
# guest standing in for a running Steam, and osascript/open stubbed so that no
# display is brought forward. It runs from a copy of the scripts whose
# env-links.sh fails: should it ever get past the one-app check, it stops
# there, before the /tmp links, the X server or a session.
T="$W/tree"; mkdir -p "$T/scripts"
cp scripts/run-app.sh scripts/settings-env.py scripts/session.py scripts/builtin-apps.json "$T/scripts/"
printf '#!/bin/sh\necho "env-links stub: not in this test" >&2\nexit 1\n' > "$T/scripts/env-links.sh"
chmod +x "$T/scripts/env-links.sh"
B="$W/bin"; mkdir -p "$B"
printf '#!/bin/sh\nexit 0\n' > "$B/osascript"; cp "$B/osascript" "$B/open"; chmod +x "$B/osascript" "$B/open"
bash -c 'exec -a "$0" sleep 30' "$W/build/lxrun /tmp/fexhome/.local/share/Steam/ubuntu12_32/steam -dispatch-test" &
fake=$!
sleep 0.5
pgrep -f "$W/build/lxrun" >/dev/null || { echo "  FAIL  the fake guest is not visible to pgrep"; fail=$((fail + 1)); }
last_line() { STEAMARM_STATE="$W" PATH="$B:$PATH" STEAMARM_DISPLAY=native "$T/scripts/run-app.sh" "$1" 2>&1 | tail -1; }
echo "aarch64 none" > "$W/launcher/running.arch"          # an earlier session's
rm -f "$W/launcher/running.id" "$W/launcher/running.pid"
[ "$(last_line steam)" = "session=adopted" ] && { echo "  ok    a Steam run-app.sh did not start: session=adopted"; pass=$((pass + 1)); } \
    || { echo "  FAIL  adoption: $(last_line steam)"; fail=$((fail + 1)); }
[ ! -e "$W/launcher/running.arch" ] && { echo "  ok    adoption drops an earlier session's running.arch"; pass=$((pass + 1)); } \
    || { echo "  FAIL  running.arch left: $(cat "$W/launcher/running.arch")"; fail=$((fail + 1)); }
for id in steam hs; do
    echo "$id" > "$W/launcher/running.id"; echo "$fake" > "$W/launcher/running.pid"
    [ "$(last_line "$id")" = "session=reshown" ] && { echo "  ok    $id shown again: session=reshown"; pass=$((pass + 1)); } \
        || { echo "  FAIL  $id: $(last_line "$id")"; fail=$((fail + 1)); }
done
kill "$fake" 2>/dev/null; wait "$fake" 2>/dev/null
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
