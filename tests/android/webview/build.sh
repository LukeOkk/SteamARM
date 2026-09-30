#!/bin/bash
# Builds the WebView probe APK into $1 (default build/android/webviewprobe.apk)
# with the Android SDK: platform android-30, build-tools (aapt2, d8, zipalign,
# apksigner) and a throwaway debug key in build/android/. Exit 3: no SDK.
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(cd ../../.. && pwd)/${1:-build/android/webviewprobe.apk}
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
JAR="$SDK/platforms/android-30/android.jar"
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)
[ -f "$JAR" ] && [ -x "$BT/aapt2" ] && [ -x "$BT/apksigner" ] || { echo "webview probe: no Android SDK (platform 30, build-tools)" >&2; exit 3; }
W=$(dirname "$OUT")/webviewprobe.work; rm -rf "$W"; mkdir -p "$W/classes" "$W/dex"
"$BT/aapt2" link -o "$W/base.apk" --manifest AndroidManifest.xml -I "$JAR"
javac --release 8 -nowarn -cp "$JAR" -d "$W/classes" $(find src -name '*.java') 2>/dev/null
"$BT/d8" --min-api 24 --lib "$JAR" --output "$W/dex" $(find "$W/classes" -name '*.class')
(cd "$W/dex" && zip -q "$W/base.apk" classes.dex)
"$BT/zipalign" -f 4 "$W/base.apk" "$W/aligned.apk"
KS=$(dirname "$OUT")/webviewprobe-debug.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias probe \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=SteamARM WebView probe" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$OUT" "$W/aligned.apk"
echo "$OUT"
