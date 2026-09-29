# Google Play Store on SteamARM: research

Status (2026-09-29, stage 25): research only. No Google software was
downloaded, installed or run for this page, and no account was signed into.
The record is `benchmarks/stage25-android-research.txt`. What Android
itself needs under lxrun is in `docs/ANDROID_ZERO_VM_FEASIBILITY.md`; this
page covers what Play Store adds on top, how Google software can lawfully
reach a non-certified Android environment, and what SteamARM will not do.

**Labels:** MEASURED (run on the owner's Mac and recorded), VERIFIED IN
SOURCE (file read, with repository and commit), UPSTREAM DOCUMENTED (the
owner of the thing says so; URL given), COMMUNITY REFERENCE (third party),
HYPOTHESIS, UNKNOWN. Nothing here is legal advice; where a conclusion is a
reading of licence text, it is marked HYPOTHESIS.

## SteamARM's rules for Google software

These follow from sections 1-5 and from the mission rules. They bind every
later stage.

1. SteamARM never commits, bundles, mirrors or redistributes Google Mobile
   Services (GMS), Google Play Store, Google Play services, the Google
   Services Framework or any other Google proprietary app or library. Not
   in the repository, not in the `.dmg`, not in a release asset.
2. SteamARM does not download them automatically either. No source was
   found from which Google licenses them for this use (section 1); a
   GAPPS package or image is something the owner obtains and points
   SteamARM at, on their own Mac, under their own responsibility. SteamARM
   records the licence status of whatever it is pointed at as "Google
   proprietary, no redistribution licence".
3. SteamARM never falsifies device certification: no spoofed build
   fingerprint, model or manufacturer of a certified device, no fake
   hardware attestation, no keybox.
4. SteamARM never bypasses Play Integrity or SafetyNet, and ships nothing
   whose purpose is to (no "Play Integrity Fix"-style modules, no root
   hiding).
5. SteamARM never signs into any account and never types, reads or stores
   a password. Registration of an uncertified device (section 4) and the
   Google sign-in are done by the user, in the user's browser and in
   Android's own UI.
6. The Android card is either real or disabled with its reason; a "Play
   Store" entry appears only once it opens for real.

## 1. Why SteamARM must never ship GMS

| fact | label | source |
|---|---|---|
| "GMS is only available through a license with Google". | UPSTREAM DOCUMENTED | https://www.android.com/gms/ |
| Google asks GMS partners to pass compatibility testing and meet its compatibility requirements (page served in Spanish: "Pedimos a los socios de GMS que hagan una prueba de compatibilidad simple y se adhieran a nuestros requisitos de compatibilidad"). | UPSTREAM DOCUMENTED | https://www.android.com/gms/ |
| "Only Play Protect certified devices are eligible to include Google apps, like the Google Play Store app." "Google apps on devices that aren't Play Protect certified aren't licensed and aren't real Google apps." | UPSTREAM DOCUMENTED | https://support.google.com/android/answer/7165974 |
| The licence is the Mobile Application Distribution Agreement (MADA). The European Commission found that the MADA tied the Play Store licence to pre-installing Google Search and Chrome (decision of 2018-07-18, case AT.40099, €4.34 bn); the General Court largely upheld it on 2022-09-14. | UPSTREAM DOCUMENTED (official documents; only search-engine excerpts of them were read) | https://ec.europa.eu/competition/antitrust/cases/dec_docs/40099/40099_9993_3.pdf ; https://curia.europa.eu/jcms/upload/docs/application/pdf/2022-09/cp220147en.pdf |
| MADA partners must pass CTS, CTS Verifier and GTS. | COMMUNITY REFERENCE | https://www.androidauthority.com/google-mobile-services-gms-3025963/ |
| LineageOS: "Due to licensing restrictions, these apps cannot come pre-installed with LineageOS and must be installed separately"; the packages "have been packaged by developers independent of LineageOS" and "are not supported in any way by LineageOS". | UPSTREAM DOCUMENTED | https://wiki.lineageos.org/gapps/ (fetched 2026-09-29) |
| Google's own public images with Play Store are the Android SDK's emulator system images. The SDK licence grants use "solely to develop applications for compatible implementations of Android" (§3.1), forbids copying, modifying or redistributing the SDK or any part of it (§3.4), and defines the SDK as "specifically including the Android system files, packaged APIs, and Google APIs add-ons" (§1.1). | UPSTREAM DOCUMENTED | https://developer.android.com/studio/terms |

HYPOTHESIS, from the rows above: there is no Google-sanctioned source from
which SteamARM, or its user, may take GMS for a non-certified environment.
The SDK emulator images are for app development only; GMS licences go to
device makers under an agreement SteamARM does not have and could not meet
(a Mac running Android under lxrun is not a CTS-compatible device). So
rule 2: SteamARM does not fetch GApps itself, not even "from an official
source", because none exists for this use.

## 2. Community GApps packages

Each packages Google's apps, taken from Google's own images, as a zip
installed after the ROM. None holds a licence from Google to redistribute
them (HYPOTHESIS, from each project's own licence text below and from the
Google Help statement in section 1).

| package | who, where | Android versions | licence text | label |
|---|---|---|---|---|
| MindTheGapps | build recipe `gitlab.com/MindTheGapps/vendor_gapps` (created 2018-08-08; branches `pi` to `baklava`; newer branches moved to GitHub); flashable zips as GitHub releases, `github.com/MindTheGapps/<version>-<arch>` | LineageOS's wiki recommends it for LineageOS 18.1 (Android 11) to 23 (Android 16), ARM and ARM64 | the recipe's `LICENSE`: files under `proprietary/` "are closed source/propietary/prebuilt files. I do not own them ... Contacts their authors for information about licensing"; other files GPLv2. The release repository declares no licence (GitHub API: `license: null`) | VERIFIED IN SOURCE (GitLab `tau` branch `LICENSE`; GitHub API, 2026-09-29) |
| MindTheGapps 11.0.0 arm64 content | `rho` branch lists, among others, `PrebuiltGmsCore.apk` (Google Play services), `GoogleServicesFramework.apk`, `Phonesky.apk` (Play Store), Google calendar and contacts sync adapters, `privapp-permissions-google*.xml`, `google.xml` sysconfig, all `PRESIGNED` (Google's signatures kept); the README says some are taken from a Pixel ("marlin") factory image | Android 11 | as above | VERIFIED IN SOURCE (`proprietary-files-common.txt`, `proprietary-files-arm64.txt`, `README.md`) |
| MindTheGapps 11.0.0 arm64 release | latest release `MindTheGapps-11.0.0-arm64-20230922_081122`: a 194,494,611-byte zip and `release.x509.pem` (metadata only; not downloaded) | Android 11 | none declared | VERIFIED IN SOURCE (GitHub API) |
| Open GApps | `github.com/opengapps` and opengapps.org; the site offers Android 4.4 to 11.0 only; its latest blog post is "Migration to SourceForge"; the main repository is not archived, last push 2026-08-12 | up to Android 11 | the build scripts are GPLv3 with an "installable zip exception" so that the output zips need not be GPL; the Google APKs are not covered by it | VERIFIED IN SOURCE (`opengapps/opengapps` `LICENSE`; opengapps.org, 2026-09-29) |
| LiteGapps | `github.com/litegapps/litegapps` ("The LiteGapps OpenSource Project Source Code"), active (last push 2026-09-27) | UNKNOWN (not read) | the repository's code is MIT (GitHub API); the Google APKs it packages are not the project's to license | VERIFIED IN SOURCE (licence of the repository); HYPOTHESIS (the APKs) |
| microG | `github.com/microg/GmsCore`, "Free implementation of Play Services" | — | Apache-2.0 | VERIFIED IN SOURCE (GitHub API) |

microG is the only one here whose code may be redistributed. It is not
Google Play services and not the Play Store: it re-implements part of the
GMS APIs, and apps see it as Google's package only on a system that allows
"signature spoofing". Waydroid's image carries that patch
(`frameworks/base/0007-core-Add-support-for-MicroG.patch`: a
`FAKE_PACKAGE_SIGNATURE` permission, VERIFIED IN SOURCE, `waydroid/android_vendor_waydroid`
`e2619850`). HYPOTHESIS: signature spoofing makes an app present another
developer's signing certificate to other apps; that is not device
certification, but it is close enough to rule 3 that SteamARM should not
enable it without an explicit decision by the owner. microG does not help
with Play Integrity (section 5).

## 3. Waydroid's GAPPS images

| fact | label | source |
|---|---|---|
| Waydroid's tool reads its system images from the channel `https://ota.waydro.id/system` and vendor images from `https://ota.waydro.id/vendor`. | VERIFIED IN SOURCE | `waydroid/waydroid` `c78a305` `tools/config/__init__.py:76-79` |
| `https://ota.waydro.id/system/lineage/waydroid_arm64/GAPPS.json` lists 129 arm64 GAPPS builds: 5 of LineageOS 17.1 (2021-10-19 to 2022-07-23), 113 of 18.1 (2022-10-30 to 2025-06-28), 11 of 20.0 (2025-07-05 to 2026-04-03). The VANILLA channel lists 153; the FOSS channel is empty. | MEASURED (metadata fetched 2026-09-29) | stage 25 record |
| Every entry's URL (129 GAPPS, 153 VANILLA, 148 arm64 vendor) points to SourceForge, `sourceforge.net/projects/waydroid/files/images/...`. The latest GAPPS system image is `lineage-20.0-20260403-GAPPS-waydroid_arm64-system.zip`, 1,326,285,880 bytes, against 904,855,273 for the VANILLA one of the same day; for 18.1 the last pair (2025-06-28) is 884,683,409 against 796,742,550 bytes. | MEASURED (metadata) | stage 25 record |
| Publisher: the Waydroid project (the OTA channel its tool uses). The images are the system partition; vendor images are separate. | VERIFIED IN SOURCE (tool) ; MEASURED (channel) | as above |
| How a GAPPS build differs: Waydroid's patch to LineageOS adds the build types VANILLA, FOSS and GAPPS; GAPPS sets `WITH_GMS := true` and `WITH_GMS_MINIMAL := true`. | VERIFIED IN SOURCE | `android_vendor_waydroid` `e2619850` `waydroid-patches/base-patches-30/vendor/lineage/0001-lineage-Add-more-build-types.patch` |
| Which GApps tree the official builds use | UNKNOWN (not in the sources read) | |
| A community build guide for Waydroid uses Open GApps' `aosp_build` for 18.1 and MindTheGapps (`vendor_gapps`, branch `tau`) for 20.0. | COMMUNITY REFERENCE | https://github.com/YogSottot/waydroid_stuff/blob/master/kernel_build/lineage-20.0/README.md |
| Licence of the Google apps inside the GAPPS images: not stated next to the files. | UNKNOWN; HYPOTHESIS: the same as section 2 | |

For SteamARM (HYPOTHESIS): a Waydroid GAPPS image is the most complete
"user-provided GAPPS image" of section 6, since its system partition
already has the Google apps installed with their permissions. It is the
owner's choice to obtain one; SteamARM would only read it from where the
owner put it. The VANILLA images (no Google apps) are what the Android
bring-up uses (`docs/ANDROID_ZERO_VM_FEASIBILITY.md` §6).

## 4. Google's registration page for uncertified devices

| fact | label | source |
|---|---|---|
| The page is `https://www.google.com/android/uncertified/`. Fetched without a session it redirects to Google sign-in (HTTP 302 to `accounts.google.com/ServiceLogin`). SteamARM did not sign in; the page's own text was not read. | MEASURED | stage 25 record |
| Since 2018 Google blocks Google apps on newly built uncertified firmware, and set up this page so that custom-ROM users can register their device with its Google Services Framework (GSF) Android ID (the article notes the page first said "Android ID" but wants the GSF ID). | COMMUNITY REFERENCE | https://www.xda-developers.com/google-blocks-gapps-uncertified-devices-custom-rom-whitelist/ (2018-03-27, read) ; https://www.androidpolice.com/2018/03/27/google-confirms-blocking-google-apps-uncertified-android-devices-heres-deal/ (search result, not read) |
| A limit of 100 registered devices per user, later removed. A new GSF ID is generated after a data reset, and must be registered again. | COMMUNITY REFERENCE (search snippets, pages not read) | https://customrombay.org/posts/play_protect_certification_fix/ ; https://xdaforums.com/t/guide-to-avoid-registering-a-new-gsf-android_id-at-each-clean-install.3772123/ |
| Waydroid documents the steps: read the ID with `sqlite3 /data/data/*/*/gservices.db 'select value from main where name = "android_id";'` inside the container, register it at the page above, "give the Google services some minutes to reflect the change", then restart the session. | UPSTREAM DOCUMENTED | https://docs.waydro.id/faq/google-play-certification |

What SteamARM may do (HYPOTHESIS, within the rules above): once Google
Play services has created `gservices.db` in the Android profile's `/data`,
SteamARM may read the `android_id` value locally (no network), show it with
a button that opens the registration page in the user's own browser, and
explain that the user signs in and submits it. SteamARM never fills in the
form, never signs in and never automates the step. Since the ID is tied to
`/data`, the Android profile's data must persist across launches, or the
user registers again.

Registration makes the Google apps usable on that device for that user. It
does not make the device "Play Protect certified" in the Play Integrity
sense (section 5) (HYPOTHESIS, consistent with the verdict definitions).

## 5. Play Integrity and SafetyNet

| fact | label | source |
|---|---|---|
| The SafetyNet Attestation API was deprecated in 2022 and fully turned down in January 2025; calls now fail with status 7 (`NETWORK_ERROR`). Play Integrity replaces it. | UPSTREAM DOCUMENTED | https://developer.android.com/privacy-and-security/safetynet/deprecation-timeline |
| `MEETS_DEVICE_INTEGRITY`: "a genuine and certified Android device. On Android 13 and higher, there is hardware-backed proof that the device bootloader is locked and the loaded Android OS is a certified device manufacturer image." | UPSTREAM DOCUMENTED | https://developer.android.com/google/play/integrity/verdicts |
| `MEETS_BASIC_INTEGRITY`: "passes basic system integrity checks ... The device may not be certified ... On Android 13 and higher, ... requires only that the attestation root of trust is provided by Google." | UPSTREAM DOCUMENTED | same |
| `MEETS_STRONG_INTEGRITY`: device integrity plus recent security updates (Android 13+), or hardware-backed boot integrity (Android 12 and lower). | UPSTREAM DOCUMENTED | same |
| `MEETS_VIRTUAL_INTEGRITY`: "an Android-powered emulator with Google Play services" that "passes system integrity checks and meets core Android compatibility requirements". | UPSTREAM DOCUMENTED | same |

What that means for SteamARM (HYPOTHESIS):

- `MEETS_DEVICE_INTEGRITY` and `MEETS_STRONG_INTEGRITY`: never. The
  environment is not a certified device, has no locked bootloader and no
  hardware-backed key attestation.
- `MEETS_VIRTUAL_INTEGRITY`: not expected; it describes Google-recognised
  emulators.
- `MEETS_BASIC_INTEGRITY`: UNKNOWN for an Android 11 image under lxrun.
  SteamARM will not try to influence it.
- SteamARM will not bypass any of this (rule 4).

Which apps then work (HYPOTHESIS, to be measured app by app at stage 6):

- apps that do not call Play Integrity, or call it and only log: most
  games and tools;
- apps that need only a Google account and Play services APIs (sign-in,
  cloud saves, push messaging) once the user has registered the device
  and signed in;
- apps that require the device verdict will refuse or degrade: banking,
  payments and wallets, some streaming apps, and games whose anti-cheat
  checks integrity. These stay unsupported, by design.

## 6. What Play Store needs from the system

| need | why | label |
|---|---|---|
| system_server with PackageManagerService | installs, updates and verifies packages; applies `privapp-permissions-google*.xml` so Play Store and Play services get privileged permissions; `sysconfig/google.xml` | HYPOTHESIS for the details; the files are VERIFIED IN SOURCE in MindTheGapps' lists (section 2) |
| Google Play services (`com.google.android.gms`, `PrebuiltGmsCore`) | accounts, check-in, Play APIs, the Play Store's backend client | VERIFIED IN SOURCE (package list); HYPOTHESIS (roles) |
| Google Services Framework (`com.google.android.gsf`) | device check-in and the `android_id` in `gservices.db` used for registration (section 4) | UPSTREAM DOCUMENTED (Waydroid's steps read that database) |
| AccountManagerService and a Google account authenticator | the user's own sign-in; SteamARM does not sign in | HYPOTHESIS |
| WebView | Google sign-in pages and many store pages are web content | HYPOTHESIS |
| A network that Android considers online, with DNS, TLS and a correct clock | Play Store checks connectivity before it does anything; Lepton turns network time off (`config.disable_networktime=true`, `compat_tool/liblepton/properties.sh:118` at Lepton `6135b53`), so the clock must come from the host | HYPOTHESIS; `docs/ANDROID_ZERO_VM_FEASIBILITY.md` §3.14 |
| Keystore with a keymaster HAL | Play services uses keys; a software keymaster exists in the Waydroid device tree (`android.hardware.keymaster@4.0-service`) | VERIFIED IN SOURCE (device tree `589fd9f`); HYPOTHESIS (need) |
| Content providers and system services Lepton removes | e.g. DownloadProvider, ContactsProvider, CalendarProvider are removed from Lepton's build, and backup and device-storage monitoring are disabled in its system_server; Play services' sync adapters and the store may need some of them | VERIFIED IN SOURCE (Lepton `11-removes.xml`, patch `1015`); HYPOTHESIS (need) |
| A display and input | the store is an interactive app | `docs/ANDROID_ZERO_VM_FEASIBILITY.md` §3.10-3.12 |
| Storage | `/data` for the apps, `/sdcard` for downloads; Lepton bind-mounts instead of FUSE | VERIFIED IN SOURCE (Lepton); HYPOTHESIS (need) |

HYPOTHESIS: Lepton's image is trimmed for one game per container and is a
poor base for the Play Store; a Waydroid GAPPS image (or VANILLA plus a
user-provided GApps package) keeps the services the store expects.

## 7. Ordered prerequisites under lxrun, with what exists today

Every step depends on the ones before it. "Today" is this repository at
`53432eb`.

| # | prerequisite | what exists today | label |
|---|---|---|---|
| 1 | bionic's `linker64` and libc run under lxrun, with x18 kept (`LXRT_KEEP_X18=1`) and 4 KiB guest pages | lxrun runs glibc aarch64 programs, loads 4 KiB-aligned ELF, virtualises x18 or has the kernel keep it; no bionic program has been started | MEASURED (glibc, stages 21-24); UNKNOWN (bionic) |
| 2 | ART runs a dex file (interpreter, then AOT); JIT later | memfd exists; the JIT's dual view does not (lxrun forces the executable view private); Darwin allows the dual view with `mach_vm_remap` aliases | VERIFIED IN SOURCE; MEASURED (native probe) |
| 3 | binder: `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` implemented in user space; `servicemanager`, `hwservicemanager`, `vndservicemanager` start | nothing | VERIFIED IN SOURCE |
| 4 | property service, logging, an init replacement or a PIE Android init; Android-mode answers for `setuid`, `setgroups`, `capset`, `unshare` | building blocks only (sockets, futexes, shared memory); those calls fail today | VERIFIED IN SOURCE |
| 5 | zygote and a system_server that reaches `sys.boot_completed=1`; `pm install` of an APK | nothing | UNKNOWN |
| 6 | SurfaceFlinger with a composer that draws into a macOS window; input; audio | the X11/CAMetalLayer path Linux programs use; evdev from inputd; PulseAudio | MEASURED (for Linux programs) |
| 7 | an app activity launched from SteamARM's launcher | nothing; the launcher must show Android as unavailable with the reason until this works | — |
| 8 | networking that ConnectivityService reports as validated; DNS | host sockets work for Linux programs; no route netlink | VERIFIED IN SOURCE |
| 9 | WebView | UNKNOWN (part of the image) | UNKNOWN |
| 10 | the user's GAPPS image or package placed by the user; Play services check-in; SteamARM shows the GSF ID; the user registers it and signs in themselves | nothing; no Google proprietary app (GMS, GSF, Play Store) was obtained for this project, and the Android state directory holds a VANILLA image only | — |
| 11 | Play Store opens, installs an arm64-v8a app, and the app launches | — | — |

## 8. Could not be checked from here

- The text of `google.com/android/uncertified` behind the sign-in (not
  signed in, by rule).
- `wiki.lineageos.org/gapps` refused WebFetch (403); it was read with
  `curl` and a browser user agent (200).
- `android.googlesource.com` answered 503 to the shell; AOSP sources were
  read from LineageOS's GitHub mirrors instead.
- Which GApps tree Waydroid's official GAPPS builds include.
- How Play Integrity evaluates an Android 11 image with no hardware
  attestation (`MEETS_BASIC_INTEGRITY` or nothing).
