import AppKit
import Foundation

/// running.arch of the session: what run-app.sh ran it as.
struct RunningArch: Equatable {
    var architecture: GuestArchitecture?
    var translator: String
}

/// Library, settings and the one app that may be running at a time.
@MainActor
final class LauncherModel: ObservableObject {
    static let shared = LauncherModel()

    @Published var apps: [AppEntry] = []
    @Published var settings = LauncherSettings() {
        didSet { if settings != oldValue { Store.save(settings, to: Paths.settingsFile) } }
    }
    /// scripts/builtin-apps.json: Steam (x86, under FEX) and Steam ARM64 (experimental).
    @Published private(set) var builtIns: [AppEntry] = [AppEntry.steam]
    /// library.json: launches, time, last result and favourites per app id.
    @Published var stats: [String: AppStats] = [:] {
        didSet { if stats != oldValue { Store.save(stats, to: Paths.libraryFile, dates: .iso8601) } }
    }
    /// What this Mac can run: the static table until scripts/compat-status.py answers.
    @Published private(set) var capabilities = RuntimeCapabilities.current
    @Published private(set) var compatibility: CompatibilityStatus?
    /// scripts/compat-status.py has not answered yet: a launch waits for it.
    private var detection: Task<Void, Never>?
    private var detectionRun = 0
    /// A launch is waiting for that answer.
    private var launchQueued = false
    /// One session at a time; its lock is taken before run-app.sh starts anything.
    @Published private(set) var session = SessionMachine()
    @Published private(set) var running: AppEntry?
    @Published private(set) var runningArch: RunningArch?
    @Published private(set) var logPath: String?
    @Published var alert: String?
    /// The last launch was refused because other Linux programs are running.
    @Published var offerStop = false
    /// Display mode of the running app (the setting may change meanwhile).
    @Published private(set) var runningDisplay: DisplayMode = .native

    private var timer: Timer?
    private var screenSharingWasRunning = true
    private var openedScreenSharing = false
    private var polling = false
    /// run-app.sh has not returned yet: the session's processes may not exist.
    private var launchInFlight = false
    /// Adopted from scripts/run-steam.sh: no session wrapper, so no exit
    /// status and no known start.
    private var adoptedWithoutWrapper = false
    /// The session's start is not known (adopted without a wrapper, or a
    /// running app shown again): its time is not added to the stats.
    @Published private(set) var runtimeUnknown = false

    var phase: SessionPhase { session.phase }
    var canLaunch: Bool { session.canLaunch }

    /// Built-ins first; an apps.json entry cannot shadow one.
    var allApps: [AppEntry] {
        let ids = Set(builtIns.map(\.id))
        return builtIns + apps.filter { !ids.contains($0.id) }
    }

    init() {
        Paths.ensureDirs()
        apps = Store.load([AppEntry].self, from: Paths.appsFile) ?? []
        settings = Store.load(LauncherSettings.self, from: Paths.settingsFile) ?? LauncherSettings()
        stats = Store.load([String: AppStats].self, from: Paths.libraryFile, dates: .iso8601) ?? [:]
        unpackBundledSource()
        loadBuiltIns()
        adoptRunningApp()
        refreshCapabilities()
    }

    /// The same definitions run-app.sh reads; Steam (x86) is always there.
    private func loadBuiltIns() {
        var list = Store.load([AppEntry].self, from: projectDir.appendingPathComponent("scripts/builtin-apps.json")) ?? []
        if !list.contains(where: { $0.id == AppEntry.steam.id }) { list.insert(AppEntry.steam, at: 0) }
        builtIns = list.map { var e = $0; e.builtIn = true; return e }
    }

    /// Re-reads what is installed (scripts/compat-status.py, in a subprocess).
    /// Launches wait for the answer: the fallback policy is decided on what
    /// this Mac has, never on the static table (whose KosmicKrisp, say, is
    /// "not detected yet").
    func refreshCapabilities() {
        let project = projectDir
        detectionRun += 1
        let run = detectionRun
        detection = Task {
            if let c = await CompatibilityStatus.load(project: project) {
                self.compatibility = c
                self.capabilities = RuntimeCapabilities.detect(from: c.runtime)
            } else if self.compatibility == nil {
                self.capabilities = .undetected
            }
            if self.detectionRun == run { self.detection = nil }
        }
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
        stats[app.id] = nil
        let fallback = Paths.hostRoot(forGuestRoot: app.root).appendingPathComponent("opt/apps").appendingPathComponent(app.id)
        let dir = URL(fileURLWithPath: app.installDir ?? fallback.path).standardizedFileURL
        // Only ever remove something that lives under opt/apps of a known root.
        for root in Paths.knownAppsRoots {
            let base = root.standardizedFileURL.path + "/"
            if dir.path.hasPrefix(base), dir.path.count > base.count {
                try? FileManager.default.removeItem(at: dir)
                return
            }
        }
    }

    func uniqueId(for name: String) -> String {
        let base = name.slug
        var id = base, n = 2
        let taken = Set(allApps.map(\.id))
        while taken.contains(id) || Paths.knownAppsRoots.contains(where: {
            FileManager.default.fileExists(atPath: $0.appendingPathComponent(id).path)
        }) {
            id = "\(base)-\(n)"; n += 1
        }
        return id
    }

    func isFavorite(_ app: AppEntry) -> Bool { stats[app.id]?.favorite == true }

    func toggleFavorite(_ app: AppEntry) {
        stats[app.id, default: AppStats()].favorite = isFavorite(app) ? nil : true
    }

    /// The library as shown: search, filter chip, favourites first.
    func visibleApps(query: String, filter: LibraryFilter) -> [AppEntry] {
        let all = allApps
        let items = all.map { LibraryItem(id: $0.id, name: $0.name, architecture: $0.architecture, isWindows: $0.isWindows) }
        let byID = Dictionary(all.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        return Library.visible(items, stats: stats, query: query, filter: filter).compactMap { byID[$0] }
    }

    /// Why `app` cannot start on this Mac, in Spanish; nil when it can be
    /// tried. Only programs outside the x86 Steam root are checked here (that
    /// one is the setup banner's business): their root and program must exist.
    func unavailableReason(_ app: AppEntry) -> String? {
        guard !app.isWindows, !Paths.isX86Root(app.root) else { return nil }
        let host = Paths.hostRoot(forGuestRoot: app.root)
        let shown = (host.path as NSString).abbreviatingWithTildeInPath
        let created: String
        switch app.root {
        case LinuxBaseEnvironment.armroot.guestRoot:
            created = "la raíz ARM64 se crea con scripts/mkarmroot.sh"
        case LinuxBaseEnvironment.arm64.guestRoot:
            created = "la raíz de Steam Frame se crea con scripts/mkframeroot.sh "
                + "a partir de la imagen de recuperación (docs/STEAM_FRAME_IMAGE.md)"
        default:
            created = ""
        }
        switch RootPresence.of(host.path) {
        case .present:
            break
        case .volumeNotAttached(let volume, let target):
            // The root exists on that volume: attaching it is all it takes.
            let bundle = Paths.state.appendingPathComponent("steamframe-root.sparsebundle")
            let how = volume == "SteamFrameRoot" && FileManager.default.fileExists(atPath: bundle.path)
                ? "conéctalo con hdiutil attach \((bundle.path as NSString).abbreviatingWithTildeInPath)"
                : "conéctalo" + (app.root == LinuxBaseEnvironment.arm64.guestRoot ? " (docs/STEAM_FRAME_IMAGE.md)" : "")
            return "el volumen «\(volume)» no está conectado (\(shown) apunta a \(target)); \(how)"
        case .danglingLink(let target):
            return "\(shown) apunta a \(target), que no existe" + (created.isEmpty ? "" : "; \(created)")
        case .missing:
            if app.root == LinuxBaseEnvironment.armroot.guestRoot {
                return "falta la raíz ARM64 (\(shown)); se crea con scripts/mkarmroot.sh"
            }
            if app.root == LinuxBaseEnvironment.arm64.guestRoot {
                return "falta la raíz de Steam Frame (\(shown)); se crea con scripts/mkframeroot.sh "
                    + "a partir de la imagen de recuperación (docs/STEAM_FRAME_IMAGE.md)"
            }
            return "falta la raíz \(app.root) (\(shown))"
        }
        guard let program = app.command.first, program.hasPrefix("/") else { return nil }
        // lstat: a guest symlink may point at a guest path the host cannot follow.
        let path = host.appendingPathComponent(String(program.dropFirst())).path
        guard (try? FileManager.default.attributesOfItem(atPath: path)) != nil else {
            return app.id.hasPrefix("steam-arm64")
                ? "falta el cliente ARM64 de Steam en \(app.root) (\(program)); benchmarks/stage21 explica cómo se descargó"
                : "falta el programa \(program) en la raíz \(app.root)"
        }
        return nil
    }

    /// The environment an entry runs in (by its root), if it is a known one.
    func environment(of app: AppEntry) -> LinuxBaseEnvironment? {
        LinuxBaseEnvironment.named(guestRoot: app.root)
    }

    /// Host folder of the app: its install directory, else the folder of its
    /// program inside its root, else the root.
    func folder(of app: AppEntry) -> URL? {
        let fm = FileManager.default
        if let d = app.installDir, fm.fileExists(atPath: d) { return URL(fileURLWithPath: d) }
        let host = Paths.hostRoot(forGuestRoot: app.root)
        if let dir = Library.programDirectory(of: app.command) {
            let u = host.appendingPathComponent(String(dir.drop(while: { $0 == "/" })))
            if fm.fileExists(atPath: u.path) { return u }
        }
        return fm.fileExists(atPath: host.path) ? host : nil
    }

    func openFolder(_ app: AppEntry) {
        if let u = folder(of: app) { NSWorkspace.shared.open(u) }
        else { alert = "No se encuentra la carpeta de \(app.name)." }
    }

    /// The app's newest log (<id>-YYYYmmdd-HHMMSS.log), else the logs folder.
    func openLogs(_ app: AppEntry) {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: Paths.logs.path)) ?? []
        if let newest = Library.newestLog(of: app.id, in: names) {
            NSWorkspace.shared.open(Paths.logs.appendingPathComponent(newest))
        } else {
            Paths.ensureDirs()
            NSWorkspace.shared.open(Paths.logs)
        }
    }

    // MARK: running

    private func screenSharingRunning() -> Bool {
        !NSRunningApplication.runningApplications(withBundleIdentifier: "com.apple.ScreenSharing").isEmpty
    }

    /// What `app` asks for: its own choices over the settings.
    func requestedDisplay(_ app: AppEntry) -> DisplayMode {
        DisplayMode(rawValue: app.overrides?["display"] ?? "") ?? settings.display
    }

    /// The settings of this launch that cannot work, with what runs instead.
    func launchIssues(_ app: AppEntry) -> [CapabilityIssue] {
        let sync = SynchronizationBackend(rawValue: app.overrides?["synchronization"] ?? "") ?? settings.synchronizationBackend
        let gfx = GraphicsBackend(rawValue: app.overrides?["graphicsBackend"] ?? "") ?? settings.graphics
        return capabilities.issues(display: requestedDisplay(app) == .vnc ? .vncScreenSharing : .nativeWindows,
                                   synchronization: sync, graphics: gfx, appInX86Root: Paths.isX86Root(app.root))
    }

    func launch(_ app: AppEntry) { launch(app, waitedForDetection: false) }

    private func launch(_ app: AppEntry, waitedForDetection: Bool) {
        guard session.canLaunch, !launchQueued else { return }
        // The fallback policy below is decided on what this Mac has: wait for
        // scripts/compat-status.py (a launchSteamOnStart launch always comes
        // before it has answered), 10 s at most.
        if detection != nil && !waitedForDetection {
            launchQueued = true
            Task {
                var tenths = 0
                while self.detection != nil && tenths < 100 {
                    try? await Task.sleep(nanoseconds: 100_000_000)
                    tenths += 1
                }
                if self.detection != nil && self.compatibility == nil { self.capabilities = .undetected }
                self.launchQueued = false
                self.launch(app, waitedForDetection: true)
            }
            return
        }
        guard FileManager.default.isExecutableFile(atPath: runAppScript.path)
                || FileManager.default.fileExists(atPath: runAppScript.path) else {
            alert = "No se encuentra \(runAppScript.path). Revisa el directorio del proyecto en Ajustes."
            return
        }
        if let reason = unavailableReason(app) {
            alert = "No se puede abrir \(app.name): \(reason)."
            return
        }
        if app.isExperimental && !confirmExperimental(app) { return }
        // Settings that cannot work: the fallback policy decides.
        let issues = launchIssues(app)
        switch settings.fallback.decision(for: issues) {
        case .proceed: break
        case .refuse:
            alert = "No se abrió \(app.name): la política de alternativas es ESTRICTA y esto no puede funcionar así:\n\n"
                + issues.map(\.summary).joined(separator: "\n")
            return
        case .ask:
            guard confirmFallback(app, issues) else { return }
        }
        // Every fallback goes to run-app.sh, which puts it over the settings:
        // what the policy announced is what runs.
        let env = FallbackPolicy.environment(for: issues)
        var mode = requestedDisplay(app)
        if let display = env["STEAMARM_DISPLAY"], let fallback = DisplayMode(rawValue: display) { mode = fallback }
        // The lock, before any process exists.
        do { try session.begin(app.id) } catch { return }
        runningDisplay = mode
        screenSharingWasRunning = mode == .vnc ? screenSharingRunning() : true
        openedScreenSharing = false
        adoptedWithoutWrapper = false
        runtimeUnknown = false
        running = app
        runningArch = nil
        launchInFlight = true
        let script = runAppScript.path, cwd = projectDir
        Task {
            let r = await Shell.run("/bin/bash", [script, app.id], cwd: cwd, env: env)
            self.launchInFlight = false
            if r.status != 0 {
                self.offerStop = r.status == 3
                self.alert = r.status == 3
                    ? "Ya hay otro programa de Linux en marcha. Detenlo antes de abrir \(app.name)."
                    : "No se pudo iniciar \(app.name) (código \(r.status)).\n\n" + r.output.suffix(1200)
                if self.session.phase == .stopping { try? self.session.ended(status: r.status) }
                else { try? self.session.startFailed(status: r.status) }
                // Status 3 is another program's session, not this app's result.
                if r.status != 3, let rec = self.session.last {
                    self.stats[app.id, default: AppStats()].noteEnd(rec, countRuntime: false)
                }
                try? self.session.cleanedUp()
                self.timer?.invalidate()   // a Detener meanwhile started polling
                self.timer = nil
                self.running = nil
                return
            }
            // run-app.sh re-shows a running app on the display it started on,
            // and adopts a Steam started by run-steam.sh: neither is a launch.
            // Its last line says which ("session=..."), whatever the app is called.
            let kind = SessionFiles.launchKind(r.output) ?? .started
            let reshown = kind != .started
            // Shown again, the running app keeps what it had: a session
            // wrapper (running.pgid) or none (a Steam adopted earlier).
            self.adoptedWithoutWrapper = kind == .adopted
                || (kind == .reshown && !FileManager.default.fileExists(atPath: Paths.pgidFile.path))
            self.runtimeUnknown = reshown
            if let m = Self.recordedDisplay() { self.runningDisplay = m }
            self.openedScreenSharing = self.runningDisplay == .vnc
            self.logPath = (try? String(contentsOf: Paths.logs.appendingPathComponent("current"),
                                        encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
            // An adopted Steam has no record of how it runs (run-app.sh
            // removes running.arch there): the entry says it, marked as such.
            self.runningArch = kind == .adopted ? nil : Self.recordedArch()
            let wasStopping = self.session.phase == .stopping
            if self.session.phase == .starting { try? self.session.started() }
            if !reshown, let at = self.session.startedAt {
                self.stats[app.id, default: AppStats()].noteLaunch(at: at)
            }
            if wasStopping {
                // Detener was pressed while run-app.sh was starting the app;
                // its --stop may have run before the session existed.
                _ = await Shell.run("/bin/bash", [script, "--stop"], cwd: cwd)
            }
            self.startPolling()
        }
    }

    /// Experimental entries (Steam ARM64) say what to expect first.
    private func confirmExperimental(_ app: AppEntry) -> Bool {
        let a = NSAlert()
        a.messageText = "\(app.name) es experimental"
        a.informativeText = app.id.hasPrefix("steam-arm64")
            ? "El cliente ARM64 nativo de Steam llega a su ventana de inicio de sesión sin emulación y sin "
                + "máquina virtual (benchmarks/stage22 y stage23), pero el inicio de sesión, la biblioteca y "
                + "Proton ARM64 todavía no están verificados. Para jugar, usa Steam."
            : "Esta app está marcada como experimental: puede no funcionar todavía."
        a.addButton(withTitle: "Abrir de todos modos")
        a.addButton(withTitle: "Cancelar")
        NSApp.activate(ignoringOtherApps: true)
        return a.runModal() == .alertFirstButtonReturn
    }

    /// Fallback policy PREGUNTAR: what cannot work, and what would run instead.
    private func confirmFallback(_ app: AppEntry, _ issues: [CapabilityIssue]) -> Bool {
        let a = NSAlert()
        a.messageText = "\(app.name): hay ajustes que no pueden funcionar"
        a.informativeText = issues.map(\.summary).joined(separator: "\n\n")
            + "\n\n¿Abrir con las alternativas? Nunca se usa una máquina virtual."
        a.addButton(withTitle: "Abrir con alternativas")
        a.addButton(withTitle: "Cancelar")
        NSApp.activate(ignoringOtherApps: true)
        return a.runModal() == .alertFirstButtonReturn
    }

    func stop() {
        guard session.phase == .running || session.phase == .starting else { return }
        if settings.confirmStop {
            let confirmation = NSAlert()
            confirmation.messageText = "¿Detener \(running?.name ?? "la app")?"
            confirmation.informativeText = "Se perderá cualquier progreso sin guardar."
            confirmation.addButton(withTitle: "Detener")
            confirmation.addButton(withTitle: "Cancelar")
            NSApp.activate(ignoringOtherApps: true)
            guard confirmation.runModal() == .alertFirstButtonReturn else { return }
        }
        // The session may have ended while the question was open.
        do { try session.requestStop() } catch { return }
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

    /// running.arch ("<arch> <translator>"); run-app.sh writes it for the
    /// launches it starts, not for a Steam it adopts.
    private static func recordedArch() -> RunningArch? {
        (try? String(contentsOf: Paths.archFile, encoding: .utf8))
            .flatMap(SessionFiles.arch)
            .map { RunningArch(architecture: $0.architecture, translator: $0.translator) }
    }

    func openLog() {
        if let p = logPath { NSWorkspace.shared.open(URL(fileURLWithPath: p)) }
        else { NSWorkspace.shared.open(Paths.logs) }
    }

    /// A launch from an earlier session (or the script) that is still alive.
    private func adoptRunningApp() {
        let id: String
        var start: Date?
        // session.py writes running.pgid, which an adopted run-steam.sh lacks.
        let wrapped = FileManager.default.fileExists(atPath: Paths.pgidFile.path)
        if let pid = Shell.readPID(Paths.pidFile), Shell.isAlive(pid),
           SessionLiveness.isLeader(command: Shell.command(of: pid), wrapped: wrapped) {
            id = ((try? String(contentsOf: Paths.idFile, encoding: .utf8)) ?? "steam")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            // run-app.sh wrote running.pid when it started the session.
            start = (try? FileManager.default.attributesOfItem(atPath: Paths.pidFile.path))?[.modificationDate] as? Date
            adoptedWithoutWrapper = !wrapped
        } else if Shell.guestProcesses().contains(where: { $0.command.contains("ubuntu12_32/steam ") }) {
            id = "steam"   // started by scripts/run-steam.sh
            // No session wrapper: a status, group or architecture on disk is
            // an earlier session's, not this Steam's.
            for f in [Paths.statusFile, Paths.pgidFile, Paths.archFile] {
                try? FileManager.default.removeItem(at: f)
            }
            adoptedWithoutWrapper = true
        } else {
            return
        }
        runtimeUnknown = adoptedWithoutWrapper || start == nil
        do {
            try session.begin(id, at: start ?? Date())
            try session.started()
        } catch { return }
        running = allApps.first { $0.id == id } ?? AppEntry(id: id, name: id, command: [])
        runningDisplay = Self.recordedDisplay() ?? settings.display
        runningArch = Self.recordedArch()
        screenSharingWasRunning = true   // unknown: never quit it
        logPath = (try? String(contentsOf: Paths.logs.appendingPathComponent("current"),
                               encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines)
        startPolling()
    }

    private func startPolling() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    private func poll() {
        // While run-app.sh is still starting the app, its processes may not
        // exist yet: "nothing alive" would end a session that is beginning.
        guard !polling, !launchInFlight, session.phase == .running || session.phase == .stopping else { return }
        polling = true
        let wrapped = !adoptedWithoutWrapper
        let script = runAppScript.path, cwd = projectDir
        Task {
            // The session's own leader, status and group decide (SessionLiveness);
            // other guest programs on the Mac only for a Steam without a wrapper.
            let (alive, group): (Bool, Int32?) = await withCheckedContinuation { cont in
                DispatchQueue.global().async {
                    let group = Shell.readPID(Paths.pgidFile)
                    let leader = Shell.readPID(Paths.pidFile).map {
                        Shell.isAlive($0) && SessionLiveness.isLeader(command: Shell.command(of: $0), wrapped: wrapped)
                    } ?? false
                    let probe = SessionLiveness.Probe(
                        leaderAlive: leader,
                        statusWritten: FileManager.default.fileExists(atPath: Paths.statusFile.path),
                        groupAlive: group.map(Shell.groupAlive) ?? false,
                        anyGuest: !wrapped && !leader && !Shell.guestProcesses().isEmpty)
                    cont.resume(returning: (SessionLiveness.alive(probe, wrapped: wrapped), group))
                }
            }
            if alive { self.polling = false; return }
            if wrapped, let group {
                // What the program left in its session outside its group
                // (helpers in groups of their own, reparented to launchd)
                // would outlive it and refuse the next launch: stopped first.
                _ = await Shell.run("/bin/bash", [script, "--reap", String(group)], cwd: cwd)
            }
            self.polling = false
            self.finish()
        }
    }

    private func finish() {
        guard session.phase == .running || session.phase == .stopping else { return }
        timer?.invalidate()
        timer = nil
        // scripts/session.py records how the program ended: "N" for exit code
        // N, "N signal S" for a death by signal S. Without it (a run adopted
        // from run-steam.sh), the only sign of a crash is that it was gone
        // within 15 s.
        let (status, bySignal) = SessionFiles.status((try? String(contentsOf: Paths.statusFile, encoding: .utf8)) ?? "")
        let stopped = session.phase == .stopping
        let quick = session.phase == .running && !runtimeUnknown
            && Date().timeIntervalSince(session.startedAt ?? .distantPast) < 15
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
        let id = session.appID
        try? session.ended(status: status)
        // Without a wrapper there is no status: nothing true to record.
        if let id, !adoptedWithoutWrapper, let rec = session.last {
            stats[id, default: AppStats()].noteEnd(rec, countRuntime: !runtimeUnknown)
        }
        try? session.cleanedUp()
        running = nil
        runningArch = nil
        openedScreenSharing = false
        adoptedWithoutWrapper = false
        runtimeUnknown = false
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
