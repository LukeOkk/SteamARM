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
    static var vncPasswordFile: URL { state.appendingPathComponent("vncpasswd.txt") }

    /// Host directory of the x86-64 Steam root (guest "/").
    static var steamRoot: URL { state.appendingPathComponent("steamroot") }
    /// Host directory that is /opt/apps in the guest.
    static var appsRoot: URL { steamRoot.appendingPathComponent("opt/apps") }
    static let guestRoot = "/tmp/lxrt-steamroot"

    /// Host path -> guest path for anything under the Steam root.
    static func guestPath(for url: URL) -> String {
        let root = steamRoot.standardizedFileURL.path
        let p = url.standardizedFileURL.path
        if p.hasPrefix(root) {
            let rest = String(p.dropFirst(root.count))
            return rest.isEmpty ? "/" : rest
        }
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

    /// The x86 client under FEX: TRANSITIONAL_COMPATIBILITY until the ARM64
    /// client runs (docs/APPLICATION_MANAGER.md).
    static let steam = AppEntry(
        id: "steam", name: "Steam", icon: nil,
        command: ["/bin/bash", "/tmp/fexhome/.local/share/Steam/steam.sh", "-noverifyfiles"],
        root: Paths.guestRoot, fexRootfs: "/", env: ["LXRT_GUEST_FAULTS": "1"], kind: "steam",
        architecture: GuestArchitecture.x86_64.rawValue)

    var isBuiltIn: Bool { id == "steam" }
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
    var resolution: String = "1600x900"
    var projectDir: String? = nil
    var extraEnv: [String: String] = [:]

    var launchSteamOnStart: Bool = false
    var confirmStop: Bool = true
    var guestLanguage: String = "auto"
    var timezone: String = "auto"
    var vsync: String = "game"
    var dramGB: Int = 0
    var vramGB: Int = 0
    var esync: Bool = true
    var fsync: Bool = true
    var fexDiskCache: Bool = false
    var fexTSO: String = "full"
    var fexMultiblock: Bool = true
    var fexSMC: String = "mtrack"
    var fexX87Reduced: Bool = false
    var graphicsBackend: String = "vulkan"
    var shaderCache: Bool = true
    var anisotropy: Int = 0
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

    init() {}
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        launchSteamOnStart = try c.decodeIfPresent(Bool.self, forKey: .launchSteamOnStart) ?? false
        confirmStop = try c.decodeIfPresent(Bool.self, forKey: .confirmStop) ?? true
        guestLanguage = try c.decodeIfPresent(String.self, forKey: .guestLanguage) ?? "auto"
        timezone = try c.decodeIfPresent(String.self, forKey: .timezone) ?? "auto"
        vsync = try c.decodeIfPresent(String.self, forKey: .vsync) ?? "game"
        dramGB = try c.decodeIfPresent(Int.self, forKey: .dramGB) ?? 0
        vramGB = try c.decodeIfPresent(Int.self, forKey: .vramGB) ?? 0
        esync = try c.decodeIfPresent(Bool.self, forKey: .esync) ?? true
        fsync = try c.decodeIfPresent(Bool.self, forKey: .fsync) ?? true
        fexDiskCache = try c.decodeIfPresent(Bool.self, forKey: .fexDiskCache) ?? false
        fexTSO = try c.decodeIfPresent(String.self, forKey: .fexTSO) ?? "full"
        fexMultiblock = try c.decodeIfPresent(Bool.self, forKey: .fexMultiblock) ?? true
        fexSMC = try c.decodeIfPresent(String.self, forKey: .fexSMC) ?? "mtrack"
        fexX87Reduced = try c.decodeIfPresent(Bool.self, forKey: .fexX87Reduced) ?? false
        graphicsBackend = try c.decodeIfPresent(String.self, forKey: .graphicsBackend) ?? "vulkan"
        shaderCache = try c.decodeIfPresent(Bool.self, forKey: .shaderCache) ?? true
        anisotropy = try c.decodeIfPresent(Int.self, forKey: .anisotropy) ?? 0
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
    static func load<T: Decodable>(_ type: T.Type, from url: URL) -> T? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }

    static func save<T: Encodable>(_ value: T, to url: URL) {
        try? saveChecked(value, to: url)
    }

    static func saveChecked<T: Encodable>(_ value: T, to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
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

    /// PIDs of runtime processes that are guest programs (not Xvnc/FEXServer).
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
