# Google Play Store on SteamARM: setup with your own GApps package

Status (2026-09-30): tooling and procedure only. `scripts/android-gapps.py`
checks a GApps package that you obtained yourself, builds an overlay tree
next to the pristine Android root, prints the commands that layer it onto
an APFS clone of the root, and tells you how to read the GSF ID for
Google's registration page. The Play Store itself has not run on SteamARM:
the Android framework does not reach `sys.boot_completed` yet
(`docs/ANDROID_RUNTIME_ARCHITECTURE.md`, "Boot"). No Google software was
downloaded, installed or run for this page, and no account was signed into.
Why SteamARM never ships Google's apps, and what Play Store needs on top of
Android, is in `docs/PLAY_STORE_RESEARCH.md`.

**Labels:** MEASURED (run or read on the owner's Mac; the command is given),
VERIFIED IN SOURCE (file read, with repository and commit), UPSTREAM
DOCUMENTED (the owner of the thing says so; URL given), HYPOTHESIS, UNKNOWN.
Nothing here is legal advice.

## What SteamARM does, and what it never does

| SteamARM does | SteamARM never does |
|---|---|
| reads a GApps zip you point it at, on your Mac, and says what it would install and whether it suits the root | download, mirror, bundle, commit or upload Google Mobile Services, Google Play Store, Google Play services, the Google Services Framework or any other Google app; not in the repository, not in the `.dmg`, not in a release asset |
| writes an overlay tree outside the repository and outside the Android root; refuses a destination inside either | modify the pristine Android root; the overlay is layered onto a clone you make yourself |
| prints the clone-and-layer commands; you read and run them | run those commands for you |
| shows the command that reads the GSF ID inside the Android session, and can read it from a copy of the root's database on the Mac | open a browser, sign in, fill in or submit Google's registration form, or send the ID anywhere |
| lets you sign in to your Google account in Android's own UI | type, read or store a password; hold an account token |
| records your package's licence status as "Google proprietary, no redistribution licence" in the overlay's manifest | fake device certification: no spoofed build fingerprint, model or manufacturer, no fake attestation, no keybox |
| reports which apps are expected to refuse to run | bypass Play Integrity or SafetyNet, hide root, or ship anything whose purpose is to (no "Play Integrity Fix"-style module) |

`scripts/android-gapps.py` imports nothing that can open a network
connection or start a process (checked by `tests/test_android_gapps.py`,
`PolicyTests`), and the test fails if an `.apk`, `.apks`, `.xapk`, `.tar.lz`
or a file named like a GApps package appears in the repository.

## Before you start: what is still missing

Every row must work before the Play Store can. They are ordered as Android
meets them.

| # | prerequisite | state | label |
|---|---|---|---|
| 1 | Java runs | x86-64 ART runs Java under FEX in the x86_64 root; the arm64 root cannot map ART's heap below 4 GiB on macOS | MEASURED (`benchmarks/stage25-art-x86-fex.txt`) |
| 2 | system_server reaches PackageManagerService and `sys.boot_completed=1` | the display profile boots to `sys.boot_completed=1` in 17-27 s warm; `pm install` and `am start` work | MEASURED (`benchmarks/stage28-android-apk.txt`) |
| 3 | a display and input for the store's UI | apps show in a macOS window through Weston; clicks and keys from that window reach apps (a click opens a game, Escape is BACK, A is KEYCODE_A) | MEASURED (`benchmarks/stage29-android-input-network.txt`) |
| 4 | networking Android considers online, DNS, TLS, a correct clock | an Ethernet network registered with ConnectivityService (netd stays a stand-in) and VALIDATED by the network stack's own probes; DNS through a dnsproxyd served from the Mac; `curl` https answers 204; the clock and time zone are the Mac's | MEASURED (`benchmarks/stage29-android-input-network.txt`); HYPOTHESIS (that this is all Play needs of the network, `docs/ANDROID_ZERO_VM_FEASIBILITY.md` 3.14) |
| 5 | WebView (Google's sign-in and many store pages are web content) | the image has `com.android.webview` 137.0.7151.72 with x86 and x86_64 libraries in `/system/product/app/webview`; whether Chromium's multi-process renderer runs under lxrun and FEX is not known | MEASURED (file read, see below); UNKNOWN (runs) |
| 6 | AccountManagerService and Google's account authenticator | part of system_server and of Play services: nothing to test before row 2 | UNKNOWN |
| 7 | keystore and a keymaster HAL | keymaster 4.0 and keystore run in the headless profile | MEASURED (stage 27) |
| 8 | the privileged-permission allow-list matches the package | the image enforces it (`ro.control_privapp_permissions=enforce`); a privileged app asking for a platform privileged permission that no `privapp-permissions` entry allows makes system_server throw at `systemReady` | MEASURED (property, see below); VERIFIED IN SOURCE (`LineageOS/android_frameworks_base` `8d14a16` `PermissionManagerService.java:3616-3668, 4756-4761`; the property is set by `LineageOS/android_vendor_lineage` `9acedc7` `config/common.mk:77-79`) |
| 9 | the root's `/data` persists across sessions (the GSF ID lives there) | `scripts/android-boot.py` uses the root's own `/data`, and its persistent properties default to the root's `/data/property` | VERIFIED IN SOURCE (`scripts/android-boot.py`, "--persist") |
| 10 | a package for Android 11 and the root's ABI | see "Which package" | — |

The image facts in rows 5 and 8 were read, not run, from the roots on the
owner's Mac on 2026-09-30 (MEASURED), with:

```sh
R=/Volumes/SteamARMAndroid/root-x86_64
grep control_privapp $R/system/etc/prop.default          # ro.control_privapp_permissions=enforce
scripts/apk-inspect.py $R/system/product/app/webview/webview.apk   # com.android.webview 137.0.7151.72, abis x86 x86_64
ls -l $R/system/bin/sqlite3                              # present (the image is userdebug)
ls -l $R/product $R/system_ext                           # links to /system/product, /system/system_ext
```

The arm64 root (`/Volumes/SteamARMAndroid/root`) gave the same property, a
`webview` directory, `sqlite3`, and `ro.product.cpu.abilist=arm64-v8a,armeabi-v7a,armeabi`;
the x86_64 root `x86_64,x86`; both `ro.build.version.sdk=30`. `sqlite3` is in
AOSP's `PRODUCT_PACKAGES_DEBUG`, so userdebug builds carry it (VERIFIED IN
SOURCE, `LineageOS/android_build` `4fdba55` `target/product/base_system.mk:363-377`),
and WebView reaches Waydroid's product through `full_base.mk` ->
`generic_no_telephony.mk` -> `handheld_product.mk` -> `media_product.mk`
(VERIFIED IN SOURCE, same commit; `waydroid/android_device_waydroid_waydroid`
`521cbb8` `device.mk:18`).

## Which package

You obtain it yourself, under your own responsibility. SteamARM points at no
download location.

| your root | what fits | label |
|---|---|---|
| x86_64 (`root-x86_64`, the one that runs Java) | an **Android 11, x86_64** package. MindTheGapps publishes none: its Android 11 release repositories are `11.0.0-arm` and `11.0.0-arm64` (plus `-ATV`), and x86_64 starts at 12.1.0. OpenGApps' build scripts do build x86_64 for API 30 ("11.0"); whether prebuilt zips of it can still be obtained was not checked | VERIFIED IN SOURCE (GitHub API, `orgs/MindTheGapps/repos`, 2026-09-30; `opengapps/opengapps` `500d909` `scripts/build_gapps.sh:68-86`); UNKNOWN (availability) |
| x86_64, with a 32-bit x86 package | accepted with a warning: Play services would run as a 32-bit process, and FEX's 32-bit mode does not run the image's own i386 services today (zygote_secondary, the audio HAL) | MEASURED (stage 27); HYPOTHESIS (Play services would fail the same way) |
| arm64 (`root`) | MindTheGapps 11.0.0 arm64 (its latest release is `MindTheGapps-11.0.0-arm64-20230922_081122`) fits the ABI, but nothing Java starts in the arm64 root until ART's heap problem is solved | VERIFIED IN SOURCE (GitHub API); MEASURED (the ART wall) |
| any | not a package for another Android version (the installers refuse it too), not 32-bit ARM (Apple silicon has no AArch32), not a Waydroid GAPPS system image (the root is built from VANILLA images; `inspect` refuses a zip holding `system.img`) | VERIFIED IN SOURCE (installers, below) |

## The procedure

Run everything from the repository. `R` below is the pristine root,
`/Volumes/SteamARMAndroid/root-x86_64`. The overlay should live on the same
APFS volume as the root, so that the layering step can clone instead of
copy.

1. **Check the package.**

   ```sh
   scripts/android-gapps.py inspect ~/Downloads/open_gapps-x86_64-11.0-pico-XXXXXXXX.zip
   scripts/android-gapps.py inspect --json -v <zip>          # everything, as JSON
   ```

   With no `--arch` or `--root` it checks against the default root
   (`$STEAMARM_ANDROID_ROOT`, `$ANDROID_X86_ROOT`, else `R`) if it is there.
   It prints the layout, the Android version and architecture the package
   declares, the three key components (Play services, GSF, Play Store) with
   their versions, every file it would install and where, the privileged
   permissions each privileged app would be granted, the files it leaves
   out, the findings, and a verdict. Exit status 0 means suitable; 4 means
   not.

2. **Build the overlay.**

   ```sh
   scripts/android-gapps.py overlay <zip> /Volumes/SteamARMAndroid/gapps-overlay
   scripts/android-gapps.py overlay --exclude Velvet --exclude GoogleTTS <zip> <out>   # leave apps out
   ```

   The tree mirrors the root: `system/priv-app/<App>/<App>.apk` (or
   `system/product/priv-app/...` for MindTheGapps), `system/etc/permissions`,
   `default-permissions`, `sysconfig`, `preferred-apps`, `system/framework`,
   `lib`/`lib64` and app library directories, `product/overlay`, plus
   `.steamarm-gapps-overlay.json` (source zip and its sha256, target, licence
   status, every file with its sha256). It refuses (exit 4, nothing written):
   a package that does not suit the root, unless `--allow-mismatch`; a
   destination inside the repository or inside an Android root; a directory
   that is not empty and is not an earlier overlay. It refuses (exit 2) a
   package with any entry whose path could leave the output directory. An
   earlier overlay at the same place is replaced as a whole; the same zip
   gives the same tree and the same manifest byte for byte.

3. **Print the plan, read it, run it yourself.**

   ```sh
   scripts/android-gapps.py apply-plan /Volumes/SteamARMAndroid/gapps-overlay
   ```

   For the x86_64 root it prints, with the real paths filled in:

   ```sh
   ANDROID_X86_ROOT=$R scripts/run-android-x86.sh --server-stop      # nothing may run on R
   cp -cR $R /Volumes/SteamARMAndroid/gapps-x86_64                  # APFS clone: instant, copy-on-write
   cp -cR /Volumes/SteamARMAndroid/gapps-overlay/system/ /Volumes/SteamARMAndroid/gapps-x86_64/system
   rm -rf /Volumes/SteamARMAndroid/gapps-x86_64/system/system_ext/priv-app/Provision   # only with a Google setup wizard
   cp /Volumes/SteamARMAndroid/gapps-overlay/.steamarm-gapps-overlay.json /Volumes/SteamARMAndroid/gapps-x86_64/
   ANDROID_X86_ROOT=/Volumes/SteamARMAndroid/gapps-x86_64 scripts/android-boot.py --root /Volumes/SteamARMAndroid/gapps-x86_64 --seconds 900
   scripts/android-gapps.py gsf-id --root /Volumes/SteamARMAndroid/gapps-x86_64
   rm -rf /Volumes/SteamARMAndroid/gapps-x86_64                     # to undo; R and the overlay stay
   ```

   It runs nothing. It warns when the overlay's files changed since it was
   built, when the derived root already exists, when overlay files would
   replace files of the image, when the overlay is on another volume (then
   `cp -c` cannot clone; use `cp -R`), and when the derived root's path is
   longer than 41 bytes: an x86_64 root's FEXServer socket,
   `<root>/data/local/tmp/steamarm-android-<12 hex>.FEXServer.Socket`, must
   fit Darwin's 104-byte `sun_path` (VERIFIED IN SOURCE,
   `scripts/run-android-x86.sh`, `scripts/android-boot.py`; a 44-byte root
   failed, MEASURED, `docs/ANDROID_RUNTIME_ARCHITECTURE.md`). The derived
   root carries its own copy of `R`'s `/data`; if `R` has booted before,
   PackageManagerService finds the new system apps at the next boot
   (HYPOTHESIS, AOSP behaviour, not run here).

4. **Register the device yourself**, once the framework boots, the network
   is up and Play services has checked in:

   ```sh
   scripts/android-gapps.py gsf-id --root /Volumes/SteamARMAndroid/gapps-x86_64          # how, and the command
   scripts/android-gapps.py gsf-id --root /Volumes/SteamARMAndroid/gapps-x86_64 --read   # read it on the Mac
   ```

   The command it prints for the running session is

   ```sh
   ANDROID_X86_ROOT=/Volumes/SteamARMAndroid/gapps-x86_64 scripts/run-android-x86.sh /system/bin/sqlite3 \
       /data/data/com.google.android.gsf/databases/gservices.db \
       "select value from main where name = 'android_id';"
   ```

   the same query Waydroid documents (UPSTREAM DOCUMENTED,
   https://docs.waydro.id/faq/google-play-certification). `--read` copies
   `gservices.db` with its `-wal` and `-shm` files to a private temporary
   directory and queries the copy with Python's `sqlite3`: the live database
   is never opened or locked, and nothing leaves the Mac. Exit status 5
   means the database or the value does not exist yet.

   Then, in your own browser, open https://www.google.com/android/uncertified/,
   sign in, paste the number and submit. Waydroid's page says to give Google
   some minutes and then restart the session (UPSTREAM DOCUMENTED, same
   URL). The page's own text sits behind Google's sign-in and was not read
   (MEASURED: HTTP 302 to `accounts.google.com`, stage 25). A wiped `/data`
   means a new ID and a new registration (COMMUNITY REFERENCE,
   `docs/PLAY_STORE_RESEARCH.md` section 4).

5. **Sign in** in Android's own UI (the Play Store, or Settings, Accounts).
   You type into Android; SteamARM does not see it.

## Package layouts

What `inspect` and `overlay` read, and what each installer would do on a
phone.

**MindTheGapps** (VERIFIED IN SOURCE, `gitlab.com/MindTheGapps/vendor_gapps`
branch `rho`, Android 11, head `5e27245`, 2022-02-08; the 2023-09-22 release
is newer than that head, so its exact contents are HYPOTHESIS):

- `build/gapps.sh` writes `build.prop` at the zip's root with `arch=` (the
  `uname -m` name: `aarch64`, `armv7l`, `x86`), `version=` (the SDK, 30) and
  `version_nice=` (11.0.0), copies `<arch>/proprietary/*` and
  `common/proprietary/*` to `system/`, adds `system/addon.d/`, the recovery
  `toybox` and `META-INF/`.
- `rho` puts the apps under `system/product/{app,priv-app}`, GSF and
  `privapp-permissions-google-se.xml` under `system/system_ext/`, and
  `product/lib*/libjni_latinimegoogle.so`, `product/framework/com.google.android.dialer.support.jar`,
  `product/etc/{permissions,sysconfig}`.
- `update-binary` refuses a package whose `arch` is not the device's
  `uname -m` or whose `version` is not `/system/build.prop`'s
  `ro.build.version.sdk`, copies `system/*` into the system partition
  (`/system/system` on system-as-root), and deletes
  `system_ext/priv-app/Provision` when `SetupWizardPrebuilt` is present.
  In SteamARM's x86_64 root `/product` and `/system_ext` are links to
  `/system/product` and `/system/system_ext` (MEASURED, read above), so the
  same paths land in the same place.

**OpenGApps** (VERIFIED IN SOURCE, `github.com/opengapps/opengapps` `500d909`):

- `g.prop` at the zip's root: `ro.addon.type=gapps`, `ro.addon.arch`,
  `ro.addon.sdk`, `ro.addon.platform`, `ro.addon.open_type` (the variant),
  `ro.addon.open_version` (`scripts/inc.installer.sh:13-23`).
- One tar per app, compressed with `lzip -m 273 -s 128MiB` by default (xz for
  AROMA, or none), `Core/<app>-<arch>.tar.lz` and `GApps/...`, where `<arch>`
  is the fallback the app was taken from (`x86_64`, `x86`, `arm`, `all`,
  `common`, `lib-<arch>`) (`scripts/inc.packagetarget.sh:134-180`,
  `scripts/inc.buildhelper.sh:39-73, 167-223`).
- Members are `<app>-<arch>/<density>/<path>` and `<app>-<arch>/common/<path>`;
  the installer extracts `common` and one density (the device's, else
  `nodpi`, else the nearest higher) and installs each file at
  `/system/<path>` (`scripts/templates/installer.sh` `extract_app`,
  `folder_extract`, `install_extracted`, `which_dpi`). For API 30 the core is
  `defaultetc` (privapp, default-permissions, preferred-apps, sysconfig),
  `defaultframework` (`com.google.android.maps.jar`,
  `com.google.android.media.effects.jar`), `gmscore` in `priv-app/PrebuiltGmsCore`,
  `gsfcore`, `vending` in `priv-app/Phonesky` with `product/overlay/PlayStoreOverlay.apk`,
  and a few more (`scripts/inc.buildtarget.sh:16-25, 212-290`).
- The x86 fallback chain is x86_64, x86, **arm** ("By using libhoudini"),
  all (`scripts/inc.buildtarget.sh:121-129`): an x86_64 package may hold
  ARM-only apps, which this root cannot load (no native bridge). `inspect`
  flags them per APK.
- `android-gapps.py` picks `nodpi`, else the highest density, or `--density`.
  It decodes lzip with Python's LZMA1 raw decoder and checks each member's
  CRC32 and sizes (https://www.nongnu.org/lzip/manual/lzip_manual.html,
  "File format").

A zip with a `system/` tree and no `build.prop` is read the same way as
MindTheGapps, with warnings that it declares no version or architecture (the
file name is used as a hint when it follows either project's naming).

Both installers also delete things (MindTheGapps: `Provision`; OpenGApps:
older GApps listed in `gapps-remove.txt`, and stock apps that larger
variants replace). The overlay never deletes; only the Provision step is in
the plan.

## What `inspect` reports

| code | level | meaning |
|---|---|---|
| `unsafe-path` | error | an entry (or an inner tar member) is absolute, has `..`, a backslash or a drive letter, or is a link or device; `overlay` writes nothing |
| `api-mismatch` | error | the package is for another Android version than the root (30) |
| `abi-mismatch` | error | built for an ABI the root does not have, or for 32-bit ARM |
| `apk-abi-mismatch` | error for Play services, GSF, Play Store; warning otherwise | the APK (or its app's `lib/<arch>/`) has native code only for ABIs the root cannot run |
| `apk-min-sdk` | same | the APK needs a newer API than 30 (Android would refuse it) |
| `abi-secondary`, `apk-abi-secondary` | warning | 32-bit x86 on the x86_64 root |
| `key-component-missing`, `key-component-excluded` | warning | no Play services, GSF or Play Store |
| `privapp-allowlist-missing` | warning | a privileged app without a `privapp-permissions` entry (row 8 above) |
| `package-api-unknown`, `package-arch-unknown`, `target-unknown` | warning | not declared, or no root to compare with |
| `case-collision`, `duplicate-entry`, `duplicate-destination`, `unexpected-member`, `vendor-file`, `xml-refused`, `xml-unreadable`, `apk-unreadable`, `exclude-unmatched` | warning | as named; an XML file with a DOCTYPE or ENTITY is not parsed |
| `other-file`, `setup-wizard`, `arm64-root-no-java`, `sdk-from-name`, `arch-from-name` | note | explanations |

Signatures are read, not verified (`scripts/apk-inspect.py`,
`docs/APK_SUPPORT.md`): the report says which certificate each APK names.
"Privileged" is the package's own allow-list for that app, crossed with what
the APK requests; which of those are platform privileged permissions is
decided by the framework at boot, not here.

## What will and will not work

All HYPOTHESIS until the Play Store runs; the verdict definitions are
UPSTREAM DOCUMENTED (https://developer.android.com/google/play/integrity/verdicts).

- Registration lets Google's apps run for your account on this device. It
  does not make the device "Play Protect certified".
- Play Integrity: `MEETS_DEVICE_INTEGRITY` and `MEETS_STRONG_INTEGRITY`
  never (not a certified device, no locked bootloader, no hardware-backed
  attestation); `MEETS_VIRTUAL_INTEGRITY` not expected (Google-recognised
  emulators); `MEETS_BASIC_INTEGRITY` UNKNOWN. SteamARM will not try to
  change any of them. The SafetyNet Attestation API was turned down in
  January 2025 (UPSTREAM DOCUMENTED,
  https://developer.android.com/privacy-and-security/safetynet/deprecation-timeline).
- Expected to work once rows 1-9 do: apps that do not call Play Integrity or
  only log it, and apps that need only a Google account and Play services
  APIs (sign-in, cloud saves, push).
- Expected to refuse or degrade, and unsupported by design: banking,
  payments and wallets, some streaming apps, games whose anti-cheat checks
  device integrity.
- In the x86_64 root: apps with x86_64 native code or none. Apps that ship
  native code only for arm64-v8a cannot load there (no native bridge; the
  translators that exist are proprietary and not bundled). Play delivers
  per-ABI splits, and Android's own package manager installs them inside
  the guest, so `android-pm.py`'s split limitation does not apply there.

## Tests

```sh
python3 -m unittest tests/test_android_gapps.py      # in make test-launcher-core
```

38 tests on synthetic packages built in the test (tiny APKs from
`tests/apk_fixtures.py` that only carry a package name, zipped in
MindTheGapps and OpenGApps layouts; lzip members made to lzip's format with
Python's LZMA1 encoder): both layouts and a metadata-less tree; the target
from `--arch`, a root's `build.prop`, its `.steamarm-androidroot`, its
toybox and the environment; API, ABI, secondary-ABI, AArch32, per-APK ABI
and minSdk findings; privileged permissions, default grants, libraries and
sysconfig; lzip members, trailing data, dictionary coding and damage;
refused inputs (an APK, a system image, an unknown zip, not a zip); path
traversal and links in zip entries and in inner tar members, with nothing
written anywhere; the overlay's contents, modes and manifest; idempotence;
replacing its own overlay and refusing any other directory, the repository
and the Android root, whose tree is compared before and after; mismatch
refusal and `--allow-mismatch`; `--exclude`; the plan's commands and that it
creates nothing; the GSF ID read from a copy with an unflushed WAL; the
import policy.

## Could not be checked here

- A real MindTheGapps or OpenGApps zip: none was obtained, by rule. The
  parsers follow the layouts read in the sources above; lzip decoding was
  not tried on a file made by lzip(1) itself.
- The overlay in a booted framework, Play services' check-in, the GSF
  database's real contents, and the sign-in: all need rows 2-6.
- The text of Google's registration page (behind its sign-in).
- Whether OpenGApps' x86_64 11.0 packages can still be obtained.
