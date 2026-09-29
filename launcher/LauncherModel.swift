import AppKit
import Foundation

enum RunPhase: Equatable { case idle, starting, running, stopping }

/// Library, settings and the one app that may be running at a time.
@MainActor
final class LauncherModel: ObservableObject {
    static let shared = LauncherModel()

    @Published var apps: [AppEntry] = []
    @Published var settings = LauncherSettings() {
        didSet { if settings != oldValue { Store.save(settings, to: Paths.settingsFile) } }
    }
    @Published private(set) var phase: RunPhase = .idle
    @Published private(set) var running: AppEntry?
    @Published private(set) var logPath: String?
    @Published var alert: String?
    /// The last launch was refused because other Linux programs are running.
    @Published var offerStop = false
    /// Display mode of the running app (the setting may change meanwhile).
    @Published private(set) var runningDisplay: DisplayMode = .native

    private var timer: Timer?
    private var startedAt = Date()
    private var screenSharingWasRunning = true
    private var openedScreenSharing = false
    private var polling = false

    var allApps: [AppEntry] { [AppEntry.steam] + apps }

    init() {
        Paths.ensureDirs()
        apps = Store.load([AppEntry].self, from: Paths.appsFile) ?? []
        settings = Store.load(LauncherSettings.self, from: Paths.settingsFile) ?? LauncherSettings()
        unpackBundledSource()
        adoptRunningApp()
    }

    // MARK: project / scripts

    var bundledProjectDir: String {
        (Bundle.main.object(forInfoDictionaryKey: "SteamARMProjectDir") as? String) ?? ""
    }

    /// Where a downloaded SteamARM.app (make release) unpacks the source it
    /// carries: the scripts it runs and the tree scripts/setup.sh builds in.
    static var supportDir: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/SteamARM")
    }
    var installedSourceDir: URL { Self.supportDir.appendingPathComponent("src") }

    /// The checkout the settings name, else the one this build came from
    /// (make launcher), else the unpacked copy of the bundled source.
    var projectDir: URL {
        if let p = settings.projectDir, !p.isEmpty {
            return URL(fileURLWithPath: (p as NSString).expandingTildeInPath)
        }
        let built = bundledProjectDir
        if !built.isEmpty && built != "@PROJECT_DIR@" &&
            FileManager.default.fileExists(atPath: built + "/scripts/run-app.sh") {
            return URL(fileURLWithPath: built)
        }
        return installedSourceDir
    }

    /// A release app carries Contents/Resources/SteamARM-src.tar.gz; unpack it
    /// on first start and whenever the app is newer than the unpacked copy.
    /// Extracting over the old copy keeps what setup.sh built in it.
    func unpackBundledSource() {
        guard let archive = Bundle.main.url(forResource: "SteamARM-src", withExtension: "tar.gz") else { return }
        let version = (Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String) ?? "0"
        let dest = installedSourceDir
        let stamp = dest.appendingPathComponent(".bundle-version")
        if (try? String(contentsOf: stamp, encoding: .utf8)) == version { return }
        try? FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
        let tar = Process()
        tar.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        tar.arguments = ["-xzf", archive.path, "-C", dest.path]
        do {
            try tar.run()
            tar.waitUntilExit()
            if tar.terminationStatus == 0 { try? version.write(to: stamp, atomically: true, encoding: .utf8) }
        } catch {
            alert = "No se pudo preparar SteamARM: \(error.localizedDescription)"
        }
    }

    /// Nothing built or no Steam root yet: the first-start banner offers setup.
    var needsSetup: Bool {
        let fm = FileManager.default
        return !fm.fileExists(atPath: projectDir.appendingPathComponent("build/lxrun").path) ||
            !fm.fileExists(atPath: Paths.steamRoot.appendingPathComponent("tmp/fexhome/.local/share/Steam/steam.sh").path) ||
            needsUpdate
    }

    var needsFirstInstall: Bool {
        !FileManager.default.fileExists(atPath: projectDir.appendingPathComponent("build/lxrun").path)
    }

    /// The unpacked source is newer than what setup.sh last built from it
    /// (a new SteamARM.app over an old install): rebuild before running.
    var needsUpdate: Bool {
        let unpacked = try? String(contentsOf: projectDir.appendingPathComponent(".bundle-version"), encoding: .utf8)
        guard let unpacked else { return false }                 // a checkout: not ours to manage
        let built = try? String(contentsOf: projectDir.appendingPathComponent("build/.setup-version"), encoding: .utf8)
        return built != unpacked
    }

    /// scripts/setup.sh in a Terminal window: long (downloads and builds for
    /// a while), and Homebrew may ask the user things there.
    func runSetup() {
        let script = Self.supportDir.appendingPathComponent("install.command")
        let q = "'" + projectDir.path.replacingOccurrences(of: "'", with: "'\\''") + "'"
        let body = """
        #!/bin/bash
        # SteamARM: build everything and install Steam (scripts/setup.sh).
        cd \(q) || exit 1
        if [ ! -x /opt/homebrew/bin/brew ]; then
            echo "SteamARM necesita Homebrew (https://brew.sh). Instálalo con:"
            echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
            echo "y vuelve a pulsar Instalar en SteamARM."
            read -n 1 -s -r -p "Pulsa una tecla para cerrar."
            exit 1
        fi
        scripts/setup.sh
        status=$?
        echo
        if [ $status -eq 0 ]; then echo "SteamARM está listo: vuelve a la app y abre Steam."
        else echo "La instalación se detuvo (código $status). Puedes volver a pulsar Instalar: continúa donde quedó."; fi
        read -n 1 -s -r -p "Pulsa una tecla para cerrar."
        """
        do {
            try FileManager.default.createDirectory(at: Self.supportDir, withIntermediateDirectories: true)
            try body.write(to: script, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script.path)
            NSWorkspace.shared.open(script)
        } catch {
            alert = "No se pudo abrir el instalador: \(error.localizedDescription)"
        }
    }

    var runAppScript: URL { projectDir.appendingPathComponent("scripts/run-app.sh") }

    var vncPassword: String {
        ((try? String(contentsOf: Paths.vncPasswordFile, encoding: .utf8)) ?? "steamarm")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    // MARK: library

    func saveApps() { Store.save(apps, to: Paths.appsFile) }

    func upsert(_ app: AppEntry) {
        if let i = apps.firstIndex(where: { $0.id == app.id }) { apps[i] = app } else { apps.append(app) }
        saveApps()
    }

    func delete(_ app: AppEntry) {
        guard !app.isBuiltIn else { return }
        apps.removeAll { $0.id == app.id }
        saveApps()
        let dir = URL(fileURLWithPath: app.installDir ?? Paths.appsRoot.appendingPathComponent(app.id).path)
            .standardizedFileURL
        // Only ever remove something that lives under opt/apps of the guest root.
        let base = Paths.appsRoot.standardizedFileURL.path + "/"
        if dir.path.hasPrefix(base), dir.path.count > base.count {
            try? FileManager.default.removeItem(at: dir)
        }
    }

    func uniqueId(for name: String) -> String {
        let base = name.slug
        var id = base, n = 2
        let taken = Set(allApps.map(\.id))
        while taken.contains(id)
                || FileManager.default.fileExists(atPath: Paths.appsRoot.appendingPathComponent(id).path) {
            id = "\(base)-\(n)"; n += 1
        }
        return id
    }

    // MARK: running

    private func screenSharingRunning() -> Bool {
        !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.ScreenSharing").isEmpty
    }

    func launch(_ app: AppEntry) {
        guard phase == .idle else { return }
        guard FileManager.default.isExecutableFile(atPath: runAppScript.path)
                || FileManager.default.fileExists(atPath: runAppScript.path) else {
            alert = "No se encuentra \(runAppScript.path). Revisa el directorio del proyecto en Ajustes."
            return
        }
        let mode = settings.display
        runningDisplay = mode
        screenSharingWasRunning = mode == .vnc ? screenSharingRunning() : true
        openedScreenSharing = false
        running = app
        phase = .starting
        startedAt = Date()
        let script = runAppScript.path, cwd = projectDir
        Task {
            let r = await Shell.run("/bin/bash", [script, app.id], cwd: cwd)
            if r.status != 0 {
                self.offerStop = r.status == 3
                self.alert = r.status == 3
                    ? "Ya hay otro programa de Linux en marcha. Detenlo antes de abrir \(app.name)."
                    : "No se pudo iniciar \(app.name) (código \(r.status)).\n\n" + r.output.suffix(1200)
                self.phase = .idle
                self.running = nil
                return
            }
            // run-app.sh re-shows a running app on the display it started on.
            if let m = Self.recordedDisplay() { self.runningDisplay = m }
            self.openedScreenSharing = self.runningDisplay == .vnc
            self.logPath = (try? String(contentsOf: Paths.logs.appendingPathComponent("current"),
                                        encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
            // A Detener pressed while run-app.sh was starting the app already
            // moved the phase on to .stopping; keep it.
            if self.phase == .starting { self.phase = .running }
            self.startPolling()
        }
    }

    func stop() {
        guard phase == .running || phase == .starting else { return }
        if settings.confirmStop {
            let confirmation = NSAlert()
            confirmation.messageText = "¿Detener \(running?.name ?? "la app")?"
            confirmation.informativeText = "Se perderá cualquier progreso sin guardar."
            confirmation.addButton(withTitle: "Detener")
            confirmation.addButton(withTitle: "Cancelar")
            NSApp.activate(ignoringOtherApps: true)
            guard confirmation.runModal() == .alertFirstButtonReturn else { return }
        }
        phase = .stopping
        let script = runAppScript.path, cwd = projectDir
        Task {
            _ = await Shell.run("/bin/bash", [script, "--stop"], cwd: cwd)
            self.startPolling()
        }
    }

    /// `run-app.sh --stop`: every guest program (X and FEXServer keep running).
    func stopAll() {
        let script = runAppScript.path, cwd = projectDir
        Task { _ = await Shell.run("/bin/bash", [script, "--stop"], cwd: cwd) }
    }

    /// Native mode: bring the X server app (whose windows are the app's) to the
    /// front. VNC mode: open Screen Sharing on Xvnc.
    func showDisplay() {
        if runningDisplay == .native {
            if let x = NSRunningApplication.runningApplications(
                withBundleIdentifier: DisplayMode.x11BundleId).first {
                x.activate()
            } else {
                _ = Shell.runSync("/usr/bin/osascript",
                                  ["-e", "tell application id \"\(DisplayMode.x11BundleId)\" to activate"])
            }
            return
        }
        if let u = URL(string: "vnc://127.0.0.1:5901") {
            NSWorkspace.shared.open(u)
            openedScreenSharing = true
        }
    }

    /// The display mode run-app.sh recorded for the running app, if any.
    private static func recordedDisplay() -> DisplayMode? {
        (try? String(contentsOf: Paths.displayFile, encoding: .utf8))
            .flatMap { DisplayMode(rawValue: $0.trimmingCharacters(in: .whitespacesAndNewlines)) }
    }

    func openLog() {
        if let p = logPath { NSWorkspace.shared.open(URL(fileURLWithPath: p)) }
        else { NSWorkspace.shared.open(Paths.logs) }
    }

    /// A launch from an earlier session (or the script) that is still alive.
    private func adoptRunningApp() {
        let id: String
        if let s = try? String(contentsOf: Paths.pidFile, encoding: .utf8),
           let pid = Int32(s.trimmingCharacters(in: .whitespacesAndNewlines)), Shell.isAlive(pid) {
            id = ((try? String(contentsOf: Paths.idFile, encoding: .utf8)) ?? "steam")
                .trimmingCharacters(in: .whitespacesAndNewlines)
        } else if Shell.guestProcesses().contains(where: { $0.command.contains("ubuntu12_32/steam ") }) {
            id = "steam"   // started by scripts/run-steam.sh
            // No session wrapper: a status or group on disk is an earlier session's.
            try? FileManager.default.removeItem(at: Paths.statusFile)
            try? FileManager.default.removeItem(at: Paths.pgidFile)
        } else {
            return
        }
        running = allApps.first { $0.id == id } ?? AppEntry(id: id, name: id, command: [])
        runningDisplay = Self.recordedDisplay() ?? settings.display
        phase = .running
        screenSharingWasRunning = true   // unknown: never quit it
        logPath = (try? String(contentsOf: Paths.logs.appendingPathComponent("current"),
                               encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
        startedAt = .distantPast
        startPolling()
    }

    private func startPolling() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    private func poll() {
        guard !polling, phase == .running || phase == .stopping else { return }
        polling = true
        Task {
            let alive: Bool = await withCheckedContinuation { cont in
                DispatchQueue.global().async {
                    var pidAlive = false
                    if let s = try? String(contentsOf: Paths.pidFile, encoding: .utf8),
                       let pid = Int32(s.trimmingCharacters(in: .whitespacesAndNewlines)) {
                        pidAlive = Shell.isAlive(pid)
                    }
                    cont.resume(returning: pidAlive || !Shell.guestProcesses().isEmpty)
                }
            }
            self.polling = false
            if !alive { self.finish() }
        }
    }

    private func finish() {
        timer?.invalidate()
        timer = nil
        // scripts/session.py records how the program ended: "N" for exit code
        // N, "N signal S" for a death by signal S. Without it (a run adopted
        // from run-steam.sh), the only sign of a crash is that it was gone
        // within 15 s.
        let fields = ((try? String(contentsOf: Paths.statusFile, encoding: .utf8)) ?? "")
            .split(separator: " ").map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        let status = fields.first.flatMap { Int32($0) }
        let bySignal = fields.count == 3 && fields[1] == "signal" ? Int32(fields[2]) : nil
        let stopped = phase == .stopping
        let quick = phase == .running && Date().timeIntervalSince(startedAt) < 15
        if runningDisplay == .vnc && openedScreenSharing && !screenSharingWasRunning {
            _ = Shell.runSync("/usr/bin/osascript", ["-e", "quit app \"Screen Sharing\""])
        }
        for f in [Paths.pidFile, Paths.idFile, Paths.displayFile, Paths.archFile, Paths.pgidFile, Paths.statusFile] {
            try? FileManager.default.removeItem(at: f)
        }
        let logAt = logPath ?? Paths.logs.path
        if !stopped, let name = running?.name {
            if bySignal == 9, let reason = Self.recentGuardStop() {
                alert = "El guardián de memoria detuvo \(name): \(reason). "
                    + "Cierra otras apps o cambia el límite en Configuración → Sistema → DRAM. "
                    + "Registro: \(logAt)."
            } else if let s = status, s != 0 {
                let how = bySignal.map { "la señal \($0)" } ?? "el código \(s)"
                alert = "\(name) terminó con \(how). Revisa el registro en \(logAt)."
            } else if status == nil, quick {
                alert = "\(name) terminó enseguida. Revisa el registro en \(logAt)."
            }
        }
        running = nil
        phase = .idle
        openedScreenSharing = false
    }

    /// scripts/safeguard.sh writes "<epoch> <reason>" to logs/safeguard.last
    /// whenever it kills guests. A SIGKILL within a minute of that is its doing.
    static func recentGuardStop(now: Date = Date()) -> String? {
        guard let s = try? String(contentsOf: Paths.logs.appendingPathComponent("safeguard.last"),
                                  encoding: .utf8) else { return nil }
        let parts = s.trimmingCharacters(in: .whitespacesAndNewlines).split(separator: " ", maxSplits: 1)
        guard parts.count == 2, let t = TimeInterval(parts[0]),
              abs(now.timeIntervalSince1970 - t) < 60 else { return nil }
        return String(parts[1])
    }
}
