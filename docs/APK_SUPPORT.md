# APK support: install, list, update, uninstall, and open in the Android session

Status, 2026-09-30 (stage 25, `benchmarks/stage25-apk-install.txt`; stage 28,
`benchmarks/stage28-android-apk.txt`).
SteamARM reads Android APKs, installs them into its state directory, keeps
their data across updates, uninstalls them and shows them in the launcher's
library. Since stage 28 it also **runs** the ones its Android session can
run: `scripts/android-session.py` boots Waydroid's LineageOS 18.1 x86_64
image under FEX with zero VM (`docs/ANDROID_RUNTIME_ARCHITECTURE.md`,
"Session"), installs the APK with Android's own `pm install`, starts it with
`am start`, and its window is a macOS window of its own. That covers apps with no
native code (dex only) and apps with x86_64 native code. Apps whose native
code is arm64-v8a only stay disabled, with the reason: Android's arm64 ART
does not start on macOS (the ART heap wall). No button pretends to open an
app it cannot run.

Labels: MEASURED (run on the owner's Mac mini M4, macOS 27, recorded in
stage 25), VERIFIED IN SOURCE (file and line, or a URL at a commit),
UPSTREAM DOCUMENTED, HYPOTHESIS, UNKNOWN.

## What exists

| piece | where | state | evidence |
|---|---|---|---|
| APK reader: manifest, label, icon, ABIs, splits, signing, bundles | `scripts/apk-inspect.py` (Python 3.9+, standard library only) | bundle selection added | MEASURED: 10 real F-Droid APKs agree with F-Droid's `index-v2.json` on the 9 compared fields; synthetic unit tests cover XAPK, APKS, APKM, a plain APK bag and refusal cases; a real XAPK (an F-Droid base re-signed with apksigner, plus a `config.xhdpi` split built with aapt2 and signed with the same key) is read and its split chosen (stage 29). |
| Package manager | `scripts/android-pm.py` | split and OBB staging added | MEASURED on real single APKs in a scratch state: install, update with data kept, downgrade refused, uninstall with and without data. Synthetic unit tests cover split and OBB storage; the real XAPK above was stored with its split (stage 29). |
| Library card, filter, context menu, disabled launch | `launcher/AddAppView.swift`, `HomeView.swift`, `LauncherModel.swift`, `ApplicationCore.swift` (`AndroidApps`, `AndroidABI`) | built; the model path was driven with real APKs; the sheet itself was not clicked | MEASURED (a harness that calls `LauncherModel`, stage 25 section E); core tests |
| `run-app.sh` runs Android entries in the session, or refuses them with the reason | `scripts/run-app.sh` | done | `tests/launcher/run_app_dispatch.sh` (12 Android checks) |
| Running an app | `scripts/android-session.py` (x86_64 Android under FEX) | dex-only and x86_64 apps; split install path added | MEASURED: a single F-Droid APK installed (`pm install` Success) and started (`am start` ok), its window focused and in the macOS window (`benchmarks/stage28-android-apk.txt`); `tests/android/run.sh` does it headless. The real XAPK installed in one `pm install` session with its split ("Success" in 2.6 s; `pm path` lists `base.apk` and `split_config.xhdpi.apk`) and started with its window focused (stage 29, `benchmarks/stage29-android-input-network.txt`). OBB copy in a real session is UNTESTED. |

## Reading an APK (`scripts/apk-inspect.py`)

```sh
scripts/apk-inspect.py app.apk                    # JSON
scripts/apk-inspect.py --extract-icon icon.png app.apk
scripts/apk-inspect.py --xml app.apk              # the decoded AndroidManifest.xml
scripts/apk-inspect.py --fdroid-index index-v2.json app.apk
```

It exits 0 for an APK or installable APK set, 2 for unreadable input, and 3
for a recognised but un-installable bundle or AAB.

### The ZIP

The standard `zipfile` module reads the archive. An entry whose
compression method is unknown is read as stored, the way Android's own zip
reader treats it. Some obfuscators use that trick to break other tools
(tested). An entry flagged as encrypted is read as if it were not: the
flag, without any real encryption, is another trick aimed at analysis tools
(tested). A damaged deflate stream is an error of the APK, not of the tool.
Every read is capped: the manifest at 16 MiB, `resources.arsc` at 128 MiB,
an icon at 16 MiB.

### The binary manifest (AXML)

This follows the chunk format of AOSP `ResourceTypes.h` (UPSTREAM DOCUMENTED):

- the string pool, UTF-8 or UTF-16, including the two-unit lengths of
  strings longer than 127 or 32,767 units;
- the resource map;
- start and end element chunks. Each attribute is read from its typed
  `Res_value`: string, decimal and hex integer, boolean, reference,
  attribute, float and colour.

An android attribute is named by its resource id when the map has one.
Android matches framework attributes by id, so an obfuscated or emptied
name string does not change what the attribute is. The id table
(`ANDROID_ATTRS`) was checked against the resource maps of the 10 real APKs.

The report holds:

- `package`;
- `versionCode` (with `versionCodeMajor` when present) and `versionName`;
- `minSdk`, which defaults to 1, or a preview codename kept as a string;
- `targetSdk`, which defaults to minSdk, as Android does;
- `maxSdk` and `compileSdk`;
- `label`;
- the launcher activity;
- permissions (`uses-permission` and `uses-permission-sdk-23`, with
  maxSdkVersion);
- the permissions Android implies (`impliedPermissions`);
- features;
- the OpenGL ES version;
- Vulkan version, level and compute;
- `isGame`;
- `extractNativeLibs`;
- `hasCode`.

Values given as references are resolved through the resource table. Some
examples are `@string/app_name` and an integer minSdk.

**The launcher activity** is the first enabled `<activity>` or
`<activity-alias>` with an intent filter that has action `MAIN` and category
`LAUNCHER`. A relative name (`.Main`) gets the package prepended, and an
alias reports its `targetActivity`. The LEANBACK (TV) launcher is reported
apart. Lepton's own extractor (`compat_tool/liblepton/apk_extractor/src/main.rs`
at `6135b53`, VERIFIED IN SOURCE) reads package, versionCode, minSdkVersion
and the first `<activity>` with MAIN and LAUNCHER, as written, relative
names included. It does not look at aliases. F-Droid 2.0.0 declares its
launcher entry only as an `activity-alias` (`org.fdroid.IconActivity`), after
a disabled one (its calculator disguise), and its `MainActivity` has only the
LEANBACK category (MEASURED, `--xml`). This parser returns MainActivity
through the enabled alias; Lepton's would find no launcher activity
(HYPOTHESIS: read from its source, not run).

### resources.arsc

The table's global string pool, then each package's type and key pools, then
every type chunk. Type chunks come in these entry layouts:

- dense, with 32-bit offsets;
- sparse (`FLAG_SPARSE`);
- 16-bit offsets (`FLAG_OFFSET16`);
- compact entries (`FLAG_COMPACT`).

Bags (styles, arrays) are skipped. Of each configuration, the parser uses
the locale, the density, the SDK level and night mode. Any other qualifier
makes a configuration "not default".

- **Label:** the default configuration first, then English, then any other.
  References are followed, at most 8 deep.
- **Icon** (`android:icon`, else `roundIcon`): every configuration of the
  resource, references followed. The highest-density PNG, WebP or JPEG
  wins; night and locale variants count only when nothing else exists.
  - An `anydpi` adaptive icon is XML. When it is the only choice, its
    `<foreground>` drawable is followed (then `<background>`), and so is a
    `<bitmap src>`, `<inset drawable>` or layer item.
  - An icon that is only a vector has no raster. `icon.path` is then null,
    with a note, unless a `res/mipmap*/ic_launcher.png|webp` exists, which is
    then taken by its file name and marked `source: filename`.
  - Width and height come from the PNG IHDR, or the WebP VP8, VP8L or VP8X
    header.

Resource-shortened paths (`res/o-.png`) resolve through the table like any
other; 8 of the 10 real APKs use them.

### Native code

`abis` lists the `lib/<abi>/` directories. `nativeLibs` gives, per ABI:

- the file count and total bytes;
- how many files are stored uncompressed, so they are loadable straight
  from the APK;
- the smallest `PT_LOAD` `p_align` among the ELF files.

The last one matters here. Apple Silicon has 16 KiB pages, and lxrun
serves 4 KiB-aligned images through its sub-page path
(`docs/ARCHITECTURE.md`). MEASURED: Termux's arm64 libraries are aligned at
4 KiB, and Shattered Pixel Dungeon's, F-Droid's and the fcitx5 plugin's at
16 KiB.

### Splits and bundles

- A split APK (manifest attribute `split`) is reported as `isSplit`.
- A base APK that needs splits sets `needsSplits`. That is
  `isSplitRequired`, `requiredSplitTypes`, or the Play meta-data
  `com.android.vending.splits.required`.
- XAPK (`manifest.json` + APKs, optionally `Android/obb/<package>/*.obb`),
  APKS (bundletool `toc.pb` with `splits/*.apk` or `standalones/*.apk`),
  APKM (`info.json`) and a plain bag of APKs are inspected member by member.
  Every member must have one package, versionCode and signing certificate set.
  Encrypted APKM is refused with the reason.
- `bundle` in the JSON lists every member, base, all splits, chosen APKs,
  chosen ABI and OBB files. Default device ABIs are `x86_64,x86`, density
  320 dpi and language `$LANG` (English fallback). Selection includes the
  base, feature splits, best matching ABI, nearest density, the chosen
  language and English when present. `--abis`, `--density`, `--language`
  override the defaults. Missing base, mixed identity or signer, and no
  device ABI when native code is only in ABI splits return exit 3.
- AAB (`BundleConfig.pb`, `base/manifest/`) remains refused (exit 3): it
  needs bundletool to generate APKs and a signing key.

### Signing

- **v1:** `META-INF/MANIFEST.MF` + `*.SF` + `*.RSA|DSA|EC`. The PKCS#7
  `SignedData` is read through a small DER reader. A block may carry a
  chain or other certificates as well. Only the certificate each
  `SignerInfo` names (by issuer and serial number, RFC 5652) is the
  signer's; when none can be matched, all of them are kept.
- **APK Signing Block:** found from the end of central directory (magic
  `APK Sig Block 42`), with its id-value pairs listed:
  - v2 `0x7109871a`, v3 `0xf05368c0`, v3.1 `0x1b93ad61`;
  - verity padding, source stamp, Play frosting and dependency info.
- **Signers:** the signing certificate of each v2, v3 and v3.1 signer is
  read, and so is any v3 proof-of-rotation lineage.
- **`certificates`:** SHA-256 digests of the DER certificates, from the
  strongest scheme present (v3, then v2, then v1), as Android picks them.
- **`rotatedCertificates`:** the v3.1 signers. v3.1 carries a rotated key
  that Android 13 and later use in place of the v3 one (UPSTREAM DOCUMENTED,
  source.android.com "APK signature scheme v3.1").
- For all 10 real APKs these digests equal F-Droid's `signer.sha256`: v1
  only for 2 APKs, v1+v2 for 4, and v1+v2+v3 for 4 (MEASURED).

**Signatures are not verified cryptographically** (`verified: false`).
The digests over the APK contents are not checked either. The report says
which certificate an APK names, not that the APK was really signed with
it.

## Installing (`scripts/android-pm.py`)

```sh
scripts/android-pm.py install [--force] [--allow-downgrade] app.apk
scripts/android-pm.py uninstall [--keep-data] <package>
scripts/android-pm.py list
scripts/android-pm.py info <package>
```

The state lives under `$STEAMARM_STATE` (default `~/SteamARM-roots`) or
`--state`, and the output is JSON:

```
android/packages/<package>/base.apk     a copy of the APK (checked by SHA-256 after the copy)
android/packages/<package>/split_*.apk  selected split APKs from a bundle
android/packages/<package>/obb/*.obb    OBB data from a bundle
android/packages/<package>/meta.json    the apk-inspect report, installedAt, updatedAt, how it was installed
android/packages/<package>/icon.png     the icon (a WebP is converted with sips; icon.webp if that fails)
android/data/<package>/                 the app's data
android/kept/<package>.json             the signer of data kept by uninstall --keep-data
```

- **Package names** must follow Android's rule: at least two segments, each
  a letter followed by letters, digits or `_`, 255 characters at most. The
  whole string must match (a trailing newline does not pass), which also
  keeps `../` out of the paths.
- **Install** stages into `packages/.install-*` and renames it into place.
  - An existing version is moved aside (`packages/.old-*`), then removed.
  - If the second rename fails, the old version is put back.
  - If the process dies between the two renames, the next command puts
    the old version back before anything else. Each command takes one
    `flock`, and on taking it removes stages left by an install that never
    finished.
  - The data directory is made before anything is replaced.
- **What it refuses, as Android does** (UPSTREAM DOCUMENTED, `PackageManager`
  install failure codes):
  - an unsigned APK (`INSTALL_PARSE_FAILED_NO_CERTIFICATES`; exit 3);
  - a lone split APK, or a base that needs absent splits
    (`INSTALL_FAILED_MISSING_SPLIT`; exit 3);
  - a malformed or incompatible bundle (exit 3), and AAB (exit 3).
- A bundle stages its selected base and splits in one package directory,
  records their filenames and OBB filenames in `meta.json`, and uses the
  same update, signer, downgrade and uninstall rules as a single APK.
- **Update:** the same package with the same certificate, or a new key
  whose v3 lineage holds the old certificate (key rotation). A v3.1 rotated
  key counts as the same signer as its v3 key. The data directory is kept.
  - Another certificate is refused (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`;
    exit 4) unless `--force`. The replaced signer is then recorded in
    `meta.json`.
  - A lower versionCode is refused (`INSTALL_FAILED_VERSION_DOWNGRADE`;
    exit 4) unless `--allow-downgrade`.
  - The whole block digest is never compared, because it contains the
    digests of the APK's contents and so differs between any two builds.
- **Unknown signer:** an installed package whose `meta.json` is lost or
  unreadable, or a non-empty data directory with neither an installed app
  nor a kept-data record, has no known signer. Any APK is refused there
  unless `--force`, so that nobody's data is handed to whichever APK comes
  next.
- **Uninstall** removes the data first, unless `--keep-data`, and then the
  package directory. If the data cannot all be removed, the app stays
  installed and the command fails (exit 6). Data kept with `--keep-data`
  remembers its signer: reinstalling under another certificate is refused,
  as for an update.
- **Exit statuses:** 0 done, 2 bad input, 3 not supported, 4 refused,
  5 not installed, 6 I/O.

**Where it differs from Android:**

- It installs an APK whose native code cannot run here (32-bit ARM only,
  32-bit x86 only, arm64-v8a only). Android would refuse the first two with
  `INSTALL_FAILED_NO_MATCHING_ABIS`. Here the card shows why the app cannot
  run.
- This is SteamARM's copy of the APK. The Android session installs it again,
  inside Android, with `pm install` (dexopt, a uid, permissions: Android's
  own), the first time the card is opened and whenever its versionCode
  changes. The app's data then lives in the session root's `/data`, not in
  `android/data/<package>/`; uninstalling the card does not uninstall it
  inside the session root (`scripts/android-session.py shell
  /system/bin/pm uninstall <package>` does).

## ABI policy (ARM64-first)

| APK's native code | verdict (`abiVerdict.id`) | card architecture | what it means |
|---|---|---|---|
| has `arm64-v8a` | `arm64` | `aarch64` | preferred by the policy, but Android's arm64 ART does not start on macOS (the ART heap wall): with `x86_64` code too the session runs that under FEX; `arm64-v8a` only is disabled |
| only `armeabi-v7a` / `armeabi` | `arm32-only` | `armv7` | cannot run natively: Apple Silicon has no AArch32 execution state. UPSTREAM DOCUMENTED for the M1 by the box86 project ("it only supports 64bits operations. No ARM32 there", https://box86.org/2022/03/box64-running-on-m1-with-asahi/); for this M4, HYPOTHESIS by extension. SteamARM runs no AArch32 code anywhere (`ELFInspector` rejects e_machine 40, `launcher/ApplicationCore.swift`) |
| only `x86_64` / `x86` | `x86-only` | `x86_64` / `i386` | `x86_64`: runs in the session under FEX; `x86` (32-bit) only: disabled, the session declares no 32-bit ABI |
| other ABIs only (mips, riscv64) | `unsupported` | that ABI | not supported |
| none (dex only) | `none` | `aarch64` | ART only: runs in the session (x86_64 ART under FEX) |

`abi_verdict` in `scripts/apk-inspect.py` and `AndroidABI.verdict` in
`launcher/ApplicationCore.swift` implement the same table; both are tested
on the same cases.

## In the launcher

- **Añadir app → Añadir APK (Android)** opens a file panel. It accepts
  `.apk`, `.xapk`, `.apks` and `.apkm`. `.aab` is selected only to explain
  why it needs bundletool and a signing key. The review screen shows:
  - the icon (extracted to a temporary file) and the label;
  - the package, the version and the SDK levels;
  - the ABI verdict in Spanish;
  - the launcher activity;
  - the signing schemes, marked as not cryptographically verified;
  - the size and the permissions;
  - the number of selected split APKs and the chosen ABI split;
  - whether it will update an installed version (keeping its data);
  - anything that blocks the install.

  **Instalar** or **Actualizar** runs `android-pm.py install`. A refusal
  comes back in Spanish (`AndroidApps.errorMessage`).
- **The card** is an `AppEntry` with:
  - `kind: "android"`;
  - `command: []`;
  - `architecture` from the ABI (table above);
  - the icon from `packages/<package>/icon.png`;
  - `installDir` = the package directory;
  - `android`: an `AndroidAppInfo` with package, version, SDK, ABIs,
    verdict, launcher activity, permissions and directories.

  Every new field is Optional, and `AndroidAppInfo` drops a field of the
  wrong type instead of failing. A non-optional field would make every
  older `apps.json` undecodable, and the next save would empty it
  (`launcher/tests/AppEntryTests.swift`). The id is
  `android-<package>`, with the package name kept as it is, so an update
  replaces its own card. Folding it (dots to dashes, lower case) would give
  `org.foo_bar` and `org.foo.bar` a single card.
- **The library** gains the **Android** filter. Android cards are not in
  the ARM64 or x86 (Linux) filters. The chip reads `Android · ARM64`,
  `Android · ARM 32 bits`, `Android · x86-64` / `x86`, or `Android · ART`.
- **The context menu** has:
  - **Información…** (read-only details);
  - **Abrir carpeta** (the package directory);
  - **Abrir carpeta de datos**;
  - favourites;
  - **Desinstalar…**, whose dialog offers **Desinstalar y borrar sus datos**
    and **Desinstalar y conservar sus datos**.
- **Abrir** runs `scripts/run-app.sh`, which runs
  `scripts/android-session.py run <package>` in the launcher's session
  wrapper: the x86_64 root booted to `sys.boot_completed=1` (about 20-30 s,
  longer the first time), the APK installed if needed and its launcher
  activity started, each app in a macOS window of its own through
  `steamarm-wlmac` (`tools/wlmac/README.md`; `ANDROID_SESSION_COMPOSITOR=
  weston` puts all of Android in one window instead). **Detener** stops the
  whole session, and so does closing the app's window (MEASURED, stage 30:
  the window 35 s after **Abrir** through `run-app.sh`; closed, the session
  was gone 10 s later with nothing left running). Always native windows,
  whatever the display setting.
- **Opening is disabled**, with the reason (`AndroidApps.unavailableReason`,
  on the card, as its help, in the Información sheet and in the alert of a
  launch attempt), when the session cannot run the app: arm64-v8a-only code
  (the ART heap wall), 32-bit ARM, 32-bit x86, another ABI, a minSdk above
  30 (the session is Android 11) or a preview one. `scripts/run-app.sh`
  refuses the same entries, exit 2, before anything starts.

## What is missing

1. **The rest of the runtime.** The session runs dex-only and x86_64 apps
   (stage 28). Missing: arm64-v8a-only apps (Android's arm64 ART, the heap
   wall), 32-bit x86 apps in the session, camera, a GPU path (SwiftShader
   draws on the CPU). Clicks, keys and the network work since stage 29
   (`benchmarks/stage29-android-input-network.txt`), sound (compressed
   files included) since stage 30, the Mac's keyboard layout and the
   clipboard, text both ways, since stage 31
   (`benchmarks/stage31-runtime-waits-clipboard.txt`), WebView since stage 33
   (`benchmarks/stage33-android-webview.txt`). Since stage 34
   (`benchmarks/stage34-android-termux.txt`): apps that run programs (a
   script or an x86 program from an app's seccomp-filtered process; FEX
   read the program from a bogus descriptor before), pseudo-terminals
   (`/dev/ptmx`, `/dev/pts/N`), and shared storage (`/sdcard`,
   `/storage/emulated/0`, each app's external directory). MEASURED with
   Termux: its bootstrap installs, its shell runs, `apt update` and
   `apt install` work.
2. **Real-session bundle validation.** Synthetic unit tests MEASURE the
   selection and local package storage of XAPK, APKS, APKM, plain APK bags
   and OBB data. `scripts/android-session.py` stages chosen APKs into one
   Android install session (with an explicit install session fallback) and
   copies OBB data to `/data/media/0/Android/obb/<package>/`. MEASURED in a
   real session with a base and a density split (stage 29); an ABI split
   (no F-Droid app to build one from was at hand) and an app reading its
   OBB remain UNTESTED.
3. **Signature verification.** It needs RSA, ECDSA and DSA over the signed
   data, and the chunked content digests. Today only the certificate an APK
   names is compared.
4. **Per-app state an Android system keeps:**
   - uid and gid;
   - runtime permission grants;
   - `/data/data/<package>` inside a runtime: today's data directory is
     where it would be bound;
   - general app media directories: shared storage works since stage 34
     (an app's external directory is made, `/sdcard` is `/data/media/0`);
     bundle OBB files are staged, but their real-session use is UNTESTED.
5. **Google Play and Google Mobile Services.** These are policy, not code
   yet:
   - Google's proprietary components (GMS, the Play Store) are never
     committed, bundled or downloaded by SteamARM: no source licenses them
     for this use (`docs/PLAY_STORE_RESEARCH.md`, rules 1-2). The owner
     supplies a GApps package; `scripts/android-gapps.py` checks it and
     layers it onto a clone of the root (`docs/PLAY_STORE_SETUP.md`).
   - Device certification is never falsified, and Play Integrity and
     SafetyNet are never bypassed.
   - Nothing of it runs yet: the framework does not boot.

## Tests

```sh
python3 -m unittest tests/test_apk_inspect.py tests/test_android_pm.py   # in make test-launcher-core
make test-launcher-core    # + ApplicationCoreTests (Android block), AppEntryTests, run_app_dispatch.sh
```

- `tests/apk_fixtures.py` generates every test APK: AXML, `resources.arsc`
  in 4 entry layouts, PNG/WebP/ELF headers, PKCS#7 and v2/v3 signing blocks
  with opaque stand-in certificates.
- No third-party APK is committed.
- The real APKs of stage 25 live in `~/SteamARM-roots/android/apk-samples/`
  on the owner's Mac only.

## Licences

- The code on this page is SteamARM's (MIT).
- The F-Droid APKs used to validate it were downloaded from
  `https://f-droid.org/repo/` and are not in the repository. Their licences
  are recorded in `benchmarks/stage25-apk-install.txt`: GPL-3.0-only,
  GPL-3.0-or-later and LGPL-2.1-only.
- Lepton's extractor was read for comparison, not copied (MIT, Valve).
