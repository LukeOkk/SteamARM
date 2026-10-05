import Foundation

// MARK: - Paths

enum Paths {
    static var home: URL { FileManager.default.homeDirectoryForCurrentUser }

    /// ~/SteamARM-roots (or $STEAMARM_STATE, same as the scripts).
    static var state: URL {
        if let s = ProcessInfo.processInfo.environment["STEAMARM_STATE"], !s.isEmpty {
            return URL(fileURLWithPath: s)
        }
        return home.appendingPathComponent("SteamARM-roots")
    }
    static var launcherDir: URL { state.appendingPathComponent("launcher") }
    static var logs: URL { state.appendingPathComponent("logs") }
    static var appsFile: URL { launcherDir.appendingPathComponent("apps.json") }
    static var settingsFile: URL { launcherDir.appendingPathComponent("settings.json") }
    static var controllersFile: URL { launcherDir.appendingPathComponent("controllers.json") }
    static var pidFile: URL { launcherDir.appendingPathComponent("running.pid") }
    static var idFile: URL { launcherDir.appendingPathComponent("running.id") }
    static var displayFile: URL { launcherDir.appendingPathComponent("running.display") }
    static var archFile: URL { launcherDir.appendingPathComponent("running.arch") }   // "<arch> <translator>"
    static var pgidFile: URL { launcherDir.appendingPathComponent("running.pgid") }   // the session's process group
    static var statusFile: URL { launcherDir.appendingPathComponent("running.status") } // "N" or "N signal S"
    static var vncPasswordFile: URL { state.appendingPathComponent("vncpasswd.txt") }

    /// scripts/android-pm.py's tree: packages/<package>/ (base.apk, meta.json,
    /// icon.png) and data/<package>/ (docs/APK_SUPPORT.md).
    static var androidRoot: URL { state.appendingPathComponent("android") }
    static func androidPackageDir(_ package: String) -> URL {
        androidRoot.appendingPathComponent("packages").appendingPathComponent(package)
    }
    static func androidDataDir(_ package: String) -> URL {
        androidRoot.appendingPathComponent("data").appendingPathComponent(package)
    }

    /// Per-app history: launches, runtime, last result, favourites.
    static var libraryFile: URL { launcherDir.appendingPathComponent("library.json") }

    /// Host directory of the x86-64 Steam root (guest "/").
    static var steamRoot: URL { state.appendingPathComponent("steamroot") }
    /// Host directory of the Fedora ARM64 root (scripts/mkarmroot.sh), /tmp/lxrt-armroot.
    static var armRoot: URL { state.appendingPathComponent("armroot") }
    /// Host directory of the Holo-derived ARM64 root, /tmp/lxrt-arm64root.
    static var arm64Root: URL { state.appendingPathComponent("arm64root") }
    /// Host directory that is /opt/apps in the guest.
    static var appsRoot: URL { steamRoot.appendingPathComponent("opt/apps") }
    static let guestRoot = "/tmp/lxrt-steamroot"

    /// The host directory behind a guest root (the /tmp links of
    /// scripts/env-links.sh); any other root is its own path.
    static func hostRoot(forGuestRoot root: String) -> URL {
        switch root.count > 1 && root.hasSuffix("/") ? String(root.dropLast()) : root {
        case guestRoot: return steamRoot
        case LinuxBaseEnvironment.armroot.guestRoot: return armRoot
        case LinuxBaseEnvironment.arm64.guestRoot: return arm64Root
        default: return URL(fileURLWithPath: root)
        }
    }

    /// opt/apps of every root the launcher installs into or may delete from.
    static var knownAppsRoots: [URL] {
        [steamRoot, armRoot, arm64Root].map { $0.appendingPathComponent("opt/apps") }
    }

    /// The x86 Steam root, however it is named (the link or its target).
    static func isX86Root(_ root: String) -> Bool {
        let a = URL(fileURLWithPath: root).resolvingSymlinksInPath().standardizedFileURL.path
        let b = URL(fileURLWithPath: guestRoot).resolvingSymlinksInPath().standardizedFileURL.path
        return a == b || hostRoot(forGuestRoot: root) == steamRoot
    }

    /// Host path -> guest path for anything under the Steam root.
    static func guestPath(for url: URL) -> String { guestPath(for: url, in: steamRoot) }

    /// Host path -> guest path for anything under the host root `root`.
    static func guestPath(for url: URL, in root: URL) -> String {
        let r = root.standardizedFileURL.path
        let p = url.standardizedFileURL.path
        if p == r { return "/" }
        if p.hasPrefix(r + "/") { return String(p.dropFirst(r.count)) }
        return p
    }

    static func hostPath(forGuest guest: String) -> URL {
        steamRoot.appendingPathComponent(String(guest.drop(while: { $0 == "/" })))
    }

    static func ensureDirs() {
        for d in [launcherDir, logs] {
            try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        }
    }
}

// MARK: - Apps

struct AppEntry: Codable, Identifiable, Hashable {
    var id: String
    var name: String
    var icon: String?            // host path to an image, optional
    var command: [String]        // guest command line
    var root: String = Paths.guestRoot
    var fexRootfs: String? = "/"
    var env: [String: String] = [:]
    var kind: String = "custom"  // steam | heroic | prism | custom
    var installDir: String?      // host directory removed on delete
    /// ISA of the program (GuestArchitecture raw value: aarch64 | x86_64 |
    /// i386), read from its ELF header when it is added. nil = x86_64, the
    /// FEX path every entry took before this field existed.
    var architecture: String? = nil
    var protonTool: String? = nil // Windows apps: installed Steam Proton directory name
    // Every field below must stay Optional: a missing non-optional key makes
    // the whole apps.json fail to decode, and the next save would empty it.
    /// Set on the entries of scripts/builtin-apps.json.
    var builtIn: Bool? = nil
    /// A CapabilityStatus.State raw value; nil means ready.
    var readiness: String? = nil
    /// This app's own display / vsync / synchronization / graphicsBackend
    /// (settings keys and values); scripts/run-app.sh puts them over the
    /// global settings (scripts/settings-env.py with_overrides).
    var overrides: [String: String]? = nil
    /// kind "android": what scripts/apk-inspect.py and android-pm.py found
    /// (docs/APK_SUPPORT.md). Such an entry has no command: SteamARM has no
    /// Android runtime yet, and the launcher never starts one.
    var android: AndroidAppInfo? = nil

    /// The x86 client under FEX: TRANSITIONAL_COMPATIBILITY until the ARM64
    /// client runs (docs/APPLICATION_MANAGER.md). Only a fallback: the
    /// definition run-app.sh uses is scripts/builtin-apps.json.
    static let steam = AppEntry(
        id: "steam", name: "Steam", icon: nil,
        command: ["/bin/bash", "/tmp/fexhome/.local/share/Steam/steam.sh", "-noverifyfiles"],
        root: Paths.guestRoot, fexRootfs: "/", env: [:], kind: "steam",
        architecture: GuestArchitecture.x86_64.rawValue, builtIn: true)

    var isBuiltIn: Bool { builtIn == true || id == "steam" }
    var isExperimental: Bool { readiness == CapabilityStatus.State.experimental.rawValue }
    var isWindows: Bool { kind == "windows" }
    var isAndroid: Bool { kind == "android" }
}

// MARK: - Settings

/// How the X display reaches the Mac ("display" in settings.json).
enum DisplayMode: String, Codable, CaseIterable {
    /// Native rootless X server on :2: every X window is a macOS window.
    case native
    /// Xvnc on :1 shown through Screen Sharing.
    case vnc

    static let x11BundleId = "org.steamarm.X11"
}

struct LauncherSettings: Codable, Equatable {
    var metalHud: Bool = false
    var display: DisplayMode = .native
    /// ExecutionBackend raw value: "auto", or "lightningJIT" (FEX's JIT set
    /// for speed, scripts/settings-env.py) with native windows.
    var execution: String = "auto"
    var resolution: String = "1600x900"
    var projectDir: String? = nil
    var extraEnv: [String: String] = [:]

    var launchSteamOnStart: Bool = false
    var primarySteam: String = "steam-arm64-frame"   // PrimarySteam
    /// "Ajustes de esta app" for built-in entries (whose definition comes from
    /// scripts/builtin-apps.json and is not saved): id -> overrides.
    var builtinOverrides: [String: [String: String]] = [:]
    var confirmStop: Bool = true
    var guestLanguage: String = "auto"
    var timezone: String = "auto"
    var vsync: String = "game"
    /// Requests macOS Adaptive Sync on a compatible screen and fullscreen
    /// surface. The WSI checks the actual surface before using it; an old
    /// settings file keeps the game's presentation behavior.
    var adaptiveSync: Bool = false
    var dramGB: Int = 0
    var vramGB: Int = 0
    /// SynchronizationBackend raw value; replaces the esync/fsync booleans
    /// (SettingsMigration.synchronization reads those).
    var synchronization: String = "auto"
    /// FallbackPolicy raw value.
    var fallbackPolicy: String = "auto"
    var fexDiskCache: Bool = false
    var fexTSO: String = "full"
    var fexMultiblock: Bool = true
    var fexSMC: String = "mtrack"
    var fexX87Reduced: Bool = false
    var graphicsBackend: String = "auto"   // GraphicsBackend raw value
    var shaderCache: Bool = true
    var anisotropy: Int = 0
    var antialiasing: Int = 0          // MSAA samples DXVK forces on D3D9 swapchains; 0 = the game's
    var resolutionScaling: Bool = false // Wine's display-mode emulation in the game prefixes
    var scalingFilter: String = "auto"   // a picture smaller than its window: auto, linear, nearest, fsr, metalfx, metalfx-temporal (shim/scaler.c)
    var fsrSharpness: Int = 90         // FSR's RCAS strength, percent
    var gameUpscaler: String = "auto"  // a game's own FSR (GameUpscalerChoice): auto (MetalFX where it can take it), fsr (the game's, untouched)
    var metalfxNativeAA: Bool = false  // MetalFX temporal at 100 %: a temporal pass at the native size (softens; opt-in)
    var renderScale: String = "1.0"    // games covering the screen render at this scale, the shim enlarges: 1.0 (native, the default), 0.77, 0.67, 0.59, 0.5, 0.33; an old "auto" is native
    var steamUIAcceleration: Bool = true // ARM64 Steam's webhelper on ANGLE/Vulkan (LXRT_EXEC_ARGS)
    var frameRateLimit: Int = 0
    var dxvkHud: String = "off"
    var audioBackend: String = "coreaudio"
    var volume: Int = 100
    var hotkeys: [String: String] = ["screenshot": "F8", "stopApp": "", "toggleMetalHud": ""]
    var protonLog: Bool = false
    var wineDebug: String = ""
    var dxvkLogLevel: String = "warn"
    var vkd3dLogLevel: String = "err"
    var guestFaults: Bool = true
    var traceMatch: String = ""
    var vulkanDebug: Bool = false

    static let resolutions = ["1280x720", "1600x900", "1920x1080", "2560x1440"]

    var synchronizationBackend: SynchronizationBackend { SynchronizationBackend(rawValue: synchronization) ?? .auto }
    var graphics: GraphicsBackend { GraphicsBackend(rawValue: graphicsBackend) ?? .auto }
    var fallback: FallbackPolicy { FallbackPolicy(rawValue: fallbackPolicy) ?? .auto }
    var executionBackend: ExecutionBackend {
        let e = ExecutionBackend(rawValue: execution) ?? .auto
        return e.usesVirtualMachine ? .auto : e
    }
    /// The launcher preset these settings select.
    var backendPreset: ApplicationBackendPreset {
        if display == .vnc { return .vncScreenSharing }
        return executionBackend == .lightningJIT ? .lightningJIT : .nativeWindows
    }

    /// Keys of settings.json that are no longer properties, read once to migrate.
    private enum LegacyKeys: String, CodingKey { case esync, fsync }

    init() {}
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let legacy = try decoder.container(keyedBy: LegacyKeys.self)
        synchronization = SettingsMigration.synchronization(
            stored: try c.decodeIfPresent(String.self, forKey: .synchronization),
            esync: try legacy.decodeIfPresent(Bool.self, forKey: .esync),
            fsync: try legacy.decodeIfPresent(Bool.self, forKey: .fsync))
        fallbackPolicy = SettingsMigration.fallbackPolicy(stored: try c.decodeIfPresent(String.self, forKey: .fallbackPolicy))
        launchSteamOnStart = try c.decodeIfPresent(Bool.self, forKey: .launchSteamOnStart) ?? false
        primarySteam = try c.decodeIfPresent(String.self, forKey: .primarySteam) ?? "steam-arm64-frame"
        builtinOverrides = try c.decodeIfPresent([String: [String: String]].self, forKey: .builtinOverrides) ?? [:]
        confirmStop = try c.decodeIfPresent(Bool.self, forKey: .confirmStop) ?? true
        guestLanguage = try c.decodeIfPresent(String.self, forKey: .guestLanguage) ?? "auto"
        timezone = try c.decodeIfPresent(String.self, forKey: .timezone) ?? "auto"
        vsync = try c.decodeIfPresent(String.self, forKey: .vsync) ?? "game"
        adaptiveSync = try c.decodeIfPresent(Bool.self, forKey: .adaptiveSync) ?? false
        dramGB = try c.decodeIfPresent(Int.self, forKey: .dramGB) ?? 0
        vramGB = try c.decodeIfPresent(Int.self, forKey: .vramGB) ?? 0
        fexDiskCache = try c.decodeIfPresent(Bool.self, forKey: .fexDiskCache) ?? false
        fexTSO = try c.decodeIfPresent(String.self, forKey: .fexTSO) ?? "full"
        fexMultiblock = try c.decodeIfPresent(Bool.self, forKey: .fexMultiblock) ?? true
        fexSMC = try c.decodeIfPresent(String.self, forKey: .fexSMC) ?? "mtrack"
        fexX87Reduced = try c.decodeIfPresent(Bool.self, forKey: .fexX87Reduced) ?? false
        graphicsBackend = SettingsMigration.graphicsBackend(stored: try c.decodeIfPresent(String.self, forKey: .graphicsBackend))
        shaderCache = try c.decodeIfPresent(Bool.self, forKey: .shaderCache) ?? true
        anisotropy = try c.decodeIfPresent(Int.self, forKey: .anisotropy) ?? 0
        antialiasing = try c.decodeIfPresent(Int.self, forKey: .antialiasing) ?? 0
        resolutionScaling = try c.decodeIfPresent(Bool.self, forKey: .resolutionScaling) ?? false
        scalingFilter = try c.decodeIfPresent(String.self, forKey: .scalingFilter) ?? "auto"
        fsrSharpness = try c.decodeIfPresent(Int.self, forKey: .fsrSharpness) ?? 90
        gameUpscaler = SettingsMigration.gameUpscaler(
            stored: try c.decodeIfPresent(String.self, forKey: .gameUpscaler))
        renderScale = try c.decodeIfPresent(String.self, forKey: .renderScale) ?? "1.0"
        if renderScale == "auto" { renderScale = "1.0" }   // was 59 % on an M4: the whole picture blurred
        steamUIAcceleration = try c.decodeIfPresent(Bool.self, forKey: .steamUIAcceleration) ?? true
        frameRateLimit = try c.decodeIfPresent(Int.self, forKey: .frameRateLimit) ?? 0
        dxvkHud = try c.decodeIfPresent(String.self, forKey: .dxvkHud) ?? "off"
        audioBackend = try c.decodeIfPresent(String.self, forKey: .audioBackend) ?? "coreaudio"
        volume = try c.decodeIfPresent(Int.self, forKey: .volume) ?? 100
        hotkeys = try c.decodeIfPresent([String: String].self, forKey: .hotkeys) ?? ["screenshot": "F8", "stopApp": "", "toggleMetalHud": ""]
        protonLog = try c.decodeIfPresent(Bool.self, forKey: .protonLog) ?? false
        wineDebug = try c.decodeIfPresent(String.self, forKey: .wineDebug) ?? ""
        dxvkLogLevel = try c.decodeIfPresent(String.self, forKey: .dxvkLogLevel) ?? "warn"
        vkd3dLogLevel = try c.decodeIfPresent(String.self, forKey: .vkd3dLogLevel) ?? "err"
        guestFaults = try c.decodeIfPresent(Bool.self, forKey: .guestFaults) ?? true
        traceMatch = try c.decodeIfPresent(String.self, forKey: .traceMatch) ?? ""
        vulkanDebug = try c.decodeIfPresent(Bool.self, forKey: .vulkanDebug) ?? false
        metalHud = try c.decodeIfPresent(Bool.self, forKey: .metalHud) ?? false
        let d: String? = try? c.decodeIfPresent(String.self, forKey: .display) ?? nil
        display = d.flatMap { DisplayMode(rawValue: $0) } ?? .native
        execution = try c.decodeIfPresent(String.self, forKey: .execution) ?? "auto"
        metalfxNativeAA = try c.decodeIfPresent(Bool.self, forKey: .metalfxNativeAA) ?? false
        resolution = try c.decodeIfPresent(String.self, forKey: .resolution) ?? "1600x900"
        projectDir = try c.decodeIfPresent(String.self, forKey: .projectDir)
        extraEnv = try c.decodeIfPresent([String: String].self, forKey: .extraEnv) ?? [:]
    }
}

enum MemoryChoices {
    static func maximum(totalGB: Int) -> Int { max(0, totalGB - (totalGB <= 8 ? 2 : 4)) }
    static var maximum: Int { maximum(totalGB: Int(ProcessInfo.processInfo.physicalMemory / 1_073_741_824)) }
    static var values: [Int] { Array(Set([maximum] + [48, 32, 24, 16, 12, 8, 6, 4, 2].filter { $0 <= maximum })).filter { $0 > 0 }.sorted(by: >) }
}

// MARK: - JSON persistence

enum Store {
    static func load<T: Decodable>(_ type: T.Type, from url: URL,
                                   dates: JSONDecoder.DateDecodingStrategy = .deferredToDate) -> T? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = dates
        return try? dec.decode(T.self, from: data)
    }

    static func save<T: Encodable>(_ value: T, to url: URL,
                                   dates: JSONEncoder.DateEncodingStrategy = .deferredToDate) {
        try? saveChecked(value, to: url, dates: dates)
    }

    static func saveChecked<T: Encodable>(_ value: T, to url: URL,
                                          dates: JSONEncoder.DateEncodingStrategy = .deferredToDate) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        enc.dateEncodingStrategy = dates
        try enc.encode(value).write(to: url, options: .atomic)
    }
}

// MARK: - Processes

enum Shell {
    /// PATH for child processes: a Finder-launched app gets a bare one.
    static let path = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

    struct Result { var status: Int32; var output: String }

    /// Runs a program to completion off the main thread; output goes through a
    /// temporary file so background grandchildren can never hold a pipe open.
    static func run(_ exe: String, _ args: [String], cwd: URL? = nil,
                    env extra: [String: String] = [:]) async -> Result {
        await withCheckedContinuation { cont in
            DispatchQueue.global().async {
                cont.resume(returning: runSync(exe, args, cwd: cwd, env: extra))
            }
        }
    }

    static func runSync(_ exe: String, _ args: [String], cwd: URL? = nil,
                        env extra: [String: String] = [:]) -> Result {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        if let cwd { p.currentDirectoryURL = cwd }
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = path
        for (k, v) in extra { env[k] = v }
        p.environment = env
        let out = FileManager.default.temporaryDirectory
            .appendingPathComponent("steamarm-\(UUID().uuidString).out")
        FileManager.default.createFile(atPath: out.path, contents: nil)
        defer { try? FileManager.default.removeItem(at: out) }
        guard let fh = try? FileHandle(forWritingTo: out) else {
            return Result(status: -1, output: "no se pudo crear el archivo temporal")
        }
        p.standardOutput = fh
        p.standardError = fh
        p.standardInput = FileHandle.nullDevice
        do { try p.run() } catch {
            return Result(status: -1, output: error.localizedDescription)
        }
        p.waitUntilExit()
        try? fh.close()
        let text = (try? String(contentsOf: out, encoding: .utf8)) ?? ""
        return Result(status: p.terminationStatus, output: text)
    }

    /// First existing executable among candidates / PATH lookups.
    static func which(_ name: String) -> String? {
        for dir in path.split(separator: ":") {
            let p = "\(dir)/\(name)"
            if FileManager.default.isExecutableFile(atPath: p) { return p }
        }
        return nil
    }

    static func isAlive(_ pid: Int32) -> Bool {
        pid > 0 && (kill(pid, 0) == 0 || errno == EPERM)
    }

    /// Whether process group `pgid` still has a process in it.
    static func groupAlive(_ pgid: Int32) -> Bool {
        pgid > 1 && (killpg(pgid, 0) == 0 || errno == EPERM)
    }

    /// A PID file (running.pid, running.pgid): one number.
    static func readPID(_ url: URL) -> Int32? {
        (try? String(contentsOf: url, encoding: .utf8))
            .flatMap { Int32($0.trimmingCharacters(in: .whitespacesAndNewlines)) }
    }

    /// The command line of `pid`; nil when ps cannot tell.
    static func command(of pid: Int32) -> String? {
        let r = runSync("/bin/ps", ["-o", "command=", "-p", String(pid)])
        let text = r.output.trimmingCharacters(in: .whitespacesAndNewlines)
        return r.status == 0 && !text.isEmpty ? text : nil
    }

    /// PIDs of runtime processes that are guest programs (not Xvnc/FEXServer).
    /// One variable of a process's environment (ps -E; the runtime keeps
    /// LXRT_* variables through guest execs), or nil.
    static func environmentValue(_ key: String, of pid: Int32) -> String? {
        let r = runSync("/bin/ps", ["-E", "-o", "command=", "-p", String(pid)])
        for word in r.output.split(separator: " ") where word.hasPrefix(key + "=") {
            return String(word.dropFirst(key.count + 1)).trimmingCharacters(in: .whitespacesAndNewlines)
        }
        return nil
    }

    static func guestProcesses() -> [(pid: Int32, command: String)] {
        let r = runSync("/bin/ps", ["-axo", "pid=,command="])
        var out: [(Int32, String)] = []
        for line in r.output.split(separator: "\n") {
            let t = line.trimmingCharacters(in: .whitespaces)
            guard t.contains("build/lxrun") else { continue }
            if t.contains("Xvnc") || t.contains("FEXServer") { continue }
            let parts = t.split(separator: " ", maxSplits: 1)
            guard let pid = Int32(parts.first ?? "") else { continue }
            out.append((pid, parts.count > 1 ? String(parts[1]) : ""))
        }
        return out
    }
}

extension String {
    /// Lower-case id usable as a directory name.
    var slug: String {
        let allowed = Set("abcdefghijklmnopqrstuvwxyz0123456789-")
        var s = lowercased().map { allowed.contains($0) ? $0 : "-" }
            .reduce(into: "") { acc, c in
                if !(c == "-" && acc.last == "-") { acc.append(c) }
            }
        while s.hasPrefix("-") { s.removeFirst() }
        while s.hasSuffix("-") { s.removeLast() }
        return s.isEmpty ? "app" : s
    }
}
