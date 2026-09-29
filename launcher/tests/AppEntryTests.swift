import Foundation

// apps.json compatibility of AppEntry (launcher/Models.swift) with the core
// types it uses (launcher/ApplicationCore.swift). A non-optional field added
// to AppEntry makes every older apps.json fail to decode, and the launcher's
// next save would then write an empty list: these checks guard that.
//   swiftc -parse-as-library launcher/ApplicationCore.swift launcher/Models.swift \
//       launcher/tests/AppEntryTests.swift -o build/app-entry-tests
@main
struct AppEntryTests {
    static var failures = 0
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        if !ok { failures += 1; print("FAIL line \(line): \(what)") }
    }

    static func main() throws {
        let dec = JSONDecoder()
        // An entry as the launcher wrote it before any optional field existed.
        let old = #"[{"id":"old","name":"Old","command":["/bin/true"],"root":"/tmp/lxrt-steamroot","env":{},"kind":"custom"}]"#
        let oldApps = try? dec.decode([AppEntry].self, from: Data(old.utf8))
        check(oldApps?.count == 1 && oldApps?.first?.android == nil && oldApps?.first?.isAndroid == false,
              "an old apps.json still decodes")
        // An Android card as LauncherModel writes it, and read back.
        let info = AndroidAppInfo(package: "org.fdroid.fdroid", versionName: "2.0.0", versionCode: 2000050,
                                  minSdk: "24", targetSdk: "37", abis: ["arm64-v8a"], abiVerdict: "arm64",
                                  launcherActivity: "org.fdroid.MainActivity", permissions: ["android.permission.INTERNET"],
                                  packageDir: "/s/android/packages/org.fdroid.fdroid",
                                  dataDir: "/s/android/data/org.fdroid.fdroid", apkSha256: "94938d32")
        let card = AppEntry(id: "android-org.fdroid.fdroid", name: "F-Droid", icon: "/s/icon.png", command: [],
                            root: "/s/android/packages/org.fdroid.fdroid", fexRootfs: nil, env: [:], kind: "android",
                            installDir: "/s/android/packages/org.fdroid.fdroid", architecture: "aarch64",
                            android: info)
        let data = try JSONEncoder().encode([card])
        let back = try dec.decode([AppEntry].self, from: data)
        check(back == [card] && back.first?.isAndroid == true && back.first?.android?.verdict == .arm64, "round trip")
        // An Android card with a partial or unknown android object keeps the whole file readable.
        let mixed = #"""
        [{"id":"old","name":"Old","command":["/bin/true"],"root":"/tmp/lxrt-steamroot","env":{},"kind":"custom"},
         {"id":"android-a.b","name":"A","command":[],"root":"/s","env":{},"kind":"android","architecture":"aarch64",
          "android":{"package":"a.b","someFutureField":[1,2,3],"versionCode":"not a number","abis":7}}]
        """#
        let mixedApps = try? dec.decode([AppEntry].self, from: Data(mixed.utf8))
        check(mixedApps?.count == 2 && mixedApps?.last?.android?.package == "a.b", "partial android object")
        print(failures == 0 ? "app entry: all checks passed" : "app entry: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }
}
