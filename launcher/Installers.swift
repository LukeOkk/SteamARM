import Foundation

struct InstallError: LocalizedError {
    let message: String
    init(_ m: String) { message = m }
    var errorDescription: String? { message }
}

// MARK: - GitHub releases

enum GitHub {
    struct Asset: Decodable {
        let name: String
        let browser_download_url: String
        let size: Int
    }
    struct Release: Decodable {
        let tag_name: String
        let assets: [Asset]
    }

    static func latest(_ repo: String) async throws -> Release {
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(repo)/releases/latest")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("SteamARM-launcher", forHTTPHeaderField: "User-Agent")
        let (data, resp) = try await URLSession.shared.data(for: req)
        if let h = resp as? HTTPURLResponse, h.statusCode != 200 {
            throw InstallError("GitHub respondió \(h.statusCode) para \(repo)")
        }
        return try JSONDecoder().decode(Release.self, from: data)
    }

    /// Prism Launcher: the Linux x86_64 AppImage (not the .zsync, not aarch64).
    static func prismAsset(_ r: Release) -> Asset? {
        r.assets.first {
            let n = $0.name.lowercased()
            return n.contains("linux") && n.contains("x86_64") && n.hasSuffix(".appimage")
        }
    }
}

// MARK: - Download with progress

final class Downloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private var cont: CheckedContinuation<URL, Error>?
    private var task: URLSessionDownloadTask?
    private let dest: URL
    private let onProgress: @Sendable (Int64, Int64) -> Void

    init(dest: URL, onProgress: @escaping @Sendable (Int64, Int64) -> Void) {
        self.dest = dest
        self.onProgress = onProgress
    }

    func start(_ url: URL) async throws -> URL {
        try await withCheckedThrowingContinuation { c in
            cont = c
            let session = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
            task = session.downloadTask(with: url)
            task?.resume()
        }
    }

    func cancel() { task?.cancel() }

    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        onProgress(totalBytesWritten, totalBytesExpectedToWrite)
    }

    func urlSession(_ s: URLSession, downloadTask: URLSessionDownloadTask,
                    didFinishDownloadingTo location: URL) {
        if let h = downloadTask.response as? HTTPURLResponse, h.statusCode != 200 {
            finish(.failure(InstallError("La descarga respondió \(h.statusCode)")))
            return
        }
        do {
            try? FileManager.default.removeItem(at: dest)
            try FileManager.default.createDirectory(at: dest.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try FileManager.default.moveItem(at: location, to: dest)
            finish(.success(dest))
        } catch {
            finish(.failure(error))
        }
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { finish(.failure(error)) }
        s.finishTasksAndInvalidate()
    }

    private func finish(_ r: Result<URL, Error>) {
        guard let c = cont else { return }
        cont = nil
        c.resume(with: r)
    }
}

// MARK: - Archive handling

enum ArchiveKind: String {
    case appImage, tarball, deb, elf, windowsExe

    static func detect(_ url: URL) -> ArchiveKind? {
        let n = url.lastPathComponent.lowercased()
        if n.hasSuffix(".appimage") { return .appImage }
        if n.hasSuffix(".tar.gz") || n.hasSuffix(".tgz") || n.hasSuffix(".tar.xz")
            || n.hasSuffix(".txz") || n.hasSuffix(".tar.bz2") || n.hasSuffix(".tar") { return .tarball }
        if n.hasSuffix(".deb") { return .deb }
        if Installer.isELF(url) { return .elf }
        if Installer.isWindowsExecutable(url) { return .windowsExe }
        return nil
    }
}

enum Installer {
    static var downloads: URL { Paths.launcherDir.appendingPathComponent("downloads") }

    /// Validate the PE signature and x86 machine, not just the .exe extension.
    static func isWindowsExecutable(_ url: URL) -> Bool {
        guard url.pathExtension.lowercased() == "exe",
              let fh = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? fh.close() }
        guard let header = try? fh.read(upToCount: 64), header.count == 64,
              header[0] == 0x4d, header[1] == 0x5a else { return false }
        let offset = (0..<4).reduce(UInt64(0)) { $0 | UInt64(header[60 + $1]) << (8 * $1) }
        guard offset >= 64, offset < 16 << 20 else { return false }
        do {
            try fh.seek(toOffset: offset)
            let pe = try fh.read(upToCount: 6) ?? Data()
            return pe.count == 6 && pe.prefix(4) == Data([0x50, 0x45, 0, 0]) &&
                ((pe[4] == 0x64 && pe[5] == 0x86) || (pe[4] == 0x4c && pe[5] == 0x01))
        } catch { return false }
    }

    static func isELF(_ url: URL) -> Bool {
        guard let fh = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? fh.close() }
        let d = (try? fh.read(upToCount: 4)) ?? Data()
        return d == Data([0x7f, 0x45, 0x4c, 0x46])
    }

    /// Where the squashfs image starts inside an AppImage: right after the ELF
    /// runtime (end of its section header table), or the first "hsqs" magic.
    static func squashfsOffset(_ url: URL) throws -> UInt64 {
        let fh = try FileHandle(forReadingFrom: url)
        defer { try? fh.close() }
        let hdr = try fh.read(upToCount: 64) ?? Data()
        guard hdr.count >= 64, hdr.prefix(4) == Data([0x7f, 0x45, 0x4c, 0x46]) else {
            throw InstallError("No es un AppImage (falta la cabecera ELF)")
        }
        let b = [UInt8](hdr)
        func le(_ o: Int, _ n: Int) -> UInt64 {
            (0..<n).reduce(UInt64(0)) { $0 | UInt64(b[o + $1]) << (8 * UInt64($1)) }
        }
        let is64 = b[4] == 2
        let shoff = is64 ? le(0x28, 8) : le(0x20, 4)
        let shentsize = is64 ? le(0x3A, 2) : le(0x2E, 2)
        let shnum = is64 ? le(0x3C, 2) : le(0x30, 2)
        let end = shoff + shentsize * shnum
        let magic = Data("hsqs".utf8)
        if end > 0 {
            try fh.seek(toOffset: end)
            if (try fh.read(upToCount: 4)) == magic { return end }
        }
        // Fallback: scan the first 16 MiB after the ELF header.
        try fh.seek(toOffset: 0)
        let chunk = try fh.read(upToCount: 16 << 20) ?? Data()
        if let r = chunk.range(of: magic, in: 64..<chunk.count) {
            return UInt64(r.lowerBound)
        }
        throw InstallError("No se encontró la imagen squashfs dentro del AppImage")
    }

    private static func tool(_ name: String) throws -> String {
        guard let p = Shell.which(name) else { throw InstallError("Falta la herramienta \(name)") }
        return p
    }

    private static func check(_ r: Shell.Result, _ what: String) throws {
        if r.status != 0 {
            throw InstallError("\(what) falló (\(r.status)):\n\(r.output.suffix(800))")
        }
    }

    /// Extracts `file` into `dest` (created fresh).
    static func extract(_ file: URL, kind: ArchiveKind, to dest: URL) async throws {
        let fm = FileManager.default
        try? fm.removeItem(at: dest)
        try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        switch kind {
        case .appImage:
            let off = try squashfsOffset(file)
            let r = await Shell.run(try tool("unsquashfs"),
                                    ["-q", "-n", "-no-xattrs", "-o", String(off), "-d", dest.path, file.path])
            // Warnings (e.g. permissions on special files) still yield a usable tree.
            if r.status != 0 && !fm.fileExists(atPath: dest.path) { try check(r, "unsquashfs") }
        case .tarball:
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            try check(await Shell.run(try tool("bsdtar"), ["-xf", file.path, "-C", dest.path]), "bsdtar")
        case .deb:
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            let bsdtar = try tool("bsdtar")
            try check(await Shell.run("/bin/sh", ["-c",
                "set -o pipefail; \"$0\" -xOf \"$1\" 'data.tar*' | \"$0\" -xf - -C \"$2\"",
                bsdtar, file.path, dest.path]), "bsdtar (deb)")
        case .elf, .windowsExe:
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            let target = dest.appendingPathComponent(file.lastPathComponent)
            try fm.copyItem(at: file, to: target)
            try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: target.path)
        }
    }

    // MARK: tree inspection

    private static func walk(_ root: URL, limit: Int = 40000, _ body: (URL, String) -> Void) {
        let keys: [URLResourceKey] = [.isRegularFileKey, .isSymbolicLinkKey]
        guard let e = FileManager.default.enumerator(at: root, includingPropertiesForKeys: keys,
                                                     options: []) else { return }
        let base = root.standardizedFileURL.path + "/"
        var n = 0
        for case let u as URL in e {
            n += 1
            if n > limit { break }
            let p = u.standardizedFileURL.path
            guard p.hasPrefix(base) else { continue }
            body(u, String(p.dropFirst(base.count)))
        }
    }

    /// Relative paths of runnable files (ELF or executable bit), best first.
    static func executables(in root: URL) -> [String] {
        var out: [String] = []
        let fm = FileManager.default
        walk(root) { u, rel in
            let name = u.lastPathComponent
            if name.contains(".so") { return }
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: u.path, isDirectory: &isDir), !isDir.boolValue else { return }
            if isELF(u) || isWindowsExecutable(u) || fm.isExecutableFile(atPath: u.path) { out.append(rel) }
        }
        func rank(_ r: String) -> (Int, Int, String) {
            let name = (r as NSString).lastPathComponent
            let first = name == "AppRun" ? 0 : (r.contains("/") ? 2 : 1)
            return (first, r.split(separator: "/").count, r.lowercased())
        }
        return out.sorted { rank($0) < rank($1) }
    }

    /// Candidate icon files (png / svg), best first: named by a .desktop file,
    /// then .DirIcon, then anything that looks like an icon.
    static func icons(in root: URL) -> [URL] {
        var desktopIcons: [String] = []
        var images: [URL] = []
        walk(root) { u, _ in
            let ext = u.pathExtension.lowercased()
            if ext == "desktop", let text = try? String(contentsOf: u, encoding: .utf8) {
                for line in text.split(separator: "\n") where line.hasPrefix("Icon=") {
                    desktopIcons.append(String(line.dropFirst(5)).trimmingCharacters(in: .whitespaces))
                }
            } else if ext == "png" || ext == "svg" {
                images.append(u)
            }
        }
        func size(_ u: URL) -> Int {
            (try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        }
        var result: [URL] = []
        for name in desktopIcons {
            let base = (name as NSString).lastPathComponent
            let stem = (base as NSString).deletingPathExtension
            let hits = images.filter {
                let s = $0.deletingPathExtension().lastPathComponent
                return s == stem || $0.lastPathComponent == base
            }
            result += hits.sorted { ($0.pathExtension == "png" ? 1 : 0, size($0)) > ($1.pathExtension == "png" ? 1 : 0, size($1)) }
        }
        let dirIcon = root.appendingPathComponent(".DirIcon").resolvingSymlinksInPath()
        if FileManager.default.fileExists(atPath: dirIcon.path) { result.append(dirIcon) }
        let iconish = images.filter {
            let n = $0.lastPathComponent.lowercased()
            return n.contains("icon") || n.contains("logo")
        }.sorted { size($0) > size($1) }
        result += iconish.prefix(8)
        var seen = Set<String>()
        return result.filter { seen.insert($0.path).inserted }
    }

    // MARK: known apps

    typealias Report = @MainActor (String, Double?) -> Void

    static func download(_ asset: GitHub.Asset, report: @escaping Report,
                         holder: @MainActor (Downloader) -> Void) async throws -> URL {
        guard let url = URL(string: asset.browser_download_url) else {
            throw InstallError("URL de descarga no válida")
        }
        let dest = downloads.appendingPathComponent(asset.name)
        let name = asset.name
        let d = Downloader(dest: dest) { done, total in
            let t = total > 0 ? total : Int64(asset.size)
            let mb = Double(done) / 1_048_576, tmb = Double(t) / 1_048_576
            Task { @MainActor in
                report(String(format: "Descargando %@ (%.0f / %.0f MB)", name, mb, tmb),
                       t > 0 ? Double(done) / Double(t) : nil)
            }
        }
        await holder(d)
        return try await d.start(url)
    }

    /// Heroic Games Launcher for linux-arm64, native in the Fedora ARM64 root
    /// (docs/HEROIC_INTEGRATION.md). Heroic publishes Linux x64 builds only;
    /// scripts/install-heroic-arm64.sh assembles the arm64 one from official
    /// release files (sha256-pinned) and installs it into
    /// <armroot>/opt/apps/heroic. The x64 build under FEX is no longer
    /// installed: an ARM64 build exists, so it is the one used (ARM64-first).
    /// Experimental: the window, its pages and a clean close/reopen are
    /// measured; store sign-in, downloads and games are not, and Amazon's
    /// helper (nile) cannot run (a non-PIE binary).
    static func installHeroic(project: URL, report: @escaping Report) async throws -> AppEntry {
        let root = Paths.armRoot
        let missing = HeroicARM64.missing(inRoot: root.path) { FileManager.default.fileExists(atPath: $0) }
        guard FileManager.default.fileExists(atPath: root.path) else {
            throw InstallError("Falta la raíz ARM64 (\((root.path as NSString).abbreviatingWithTildeInPath)); créala con scripts/mkarmroot.sh")
        }
        guard missing.isEmpty else {
            throw InstallError("A la raíz ARM64 le falta \(missing.joined(separator: ", ")): reconstrúyela con scripts/mkarmroot.sh (conserva opt/apps y tmp/)")
        }
        let script = project.appendingPathComponent("scripts/install-heroic-arm64.sh")
        guard FileManager.default.isExecutableFile(atPath: script.path) else {
            throw InstallError("No se encontró \(script.path)")
        }
        await report("Preparando Heroic \(HeroicARM64.version) para ARM64…", nil)
        let r = await runStreaming(script.path, [], cwd: project) { line in
            guard let p = HeroicARM64.progress(line) else { return }
            Task { @MainActor in report(p.text, Double(p.step - 1) / Double(p.of)) }
        }
        guard r.status == 0, let program = HeroicARM64.installedProgram(fromOutput: r.output) else {
            throw InstallError("scripts/install-heroic-arm64.sh falló (\(r.status)):\n\(r.output.suffix(800))")
        }
        let dest = root.appendingPathComponent("opt/apps/heroic")
        let hostProgram = root.appendingPathComponent(String(program.dropFirst()))
        let icon = hostProgram.deletingLastPathComponent()
            .appendingPathComponent("resources/app.asar.unpacked/build/icon.png")
        return AppEntry(id: "heroic", name: "Heroic Games Launcher",
                        icon: FileManager.default.fileExists(atPath: icon.path) ? icon.path : nil,
                        command: HeroicARM64.command(program: program),
                        root: HeroicARM64.root.guestRoot, fexRootfs: nil, env: HeroicARM64.env,
                        kind: "heroic", installDir: dest.path,
                        architecture: GuestArchitecture.aarch64.rawValue,
                        readiness: CapabilityStatus.State.experimental.rawValue)
    }

    /// Runs `exe` to completion, handing each output line to `onLine` as it
    /// comes; the whole output is in the result. Cancelling the task
    /// terminates the process (the install script cleans up after itself).
    static func runStreaming(_ exe: String, _ args: [String], cwd: URL,
                             onLine: @escaping @Sendable (String) -> Void) async -> Shell.Result {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        p.currentDirectoryURL = cwd
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = Shell.path
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        final class Collector: @unchecked Sendable {
            let lock = NSLock()
            var all = Data()
            var pending = Data()
        }
        let c = Collector()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            c.lock.lock()
            c.all.append(d)
            c.pending.append(d)
            var lines: [String] = []
            while let nl = c.pending.firstIndex(of: 0x0a) {
                lines.append(String(decoding: c.pending[c.pending.startIndex..<nl], as: UTF8.self))
                c.pending.removeSubrange(c.pending.startIndex...nl)
            }
            c.lock.unlock()
            lines.forEach(onLine)
        }
        return await withTaskCancellationHandler {
            await withCheckedContinuation { (cont: CheckedContinuation<Shell.Result, Never>) in
                p.terminationHandler = { proc in
                    pipe.fileHandleForReading.readabilityHandler = nil
                    let rest = (try? pipe.fileHandleForReading.readToEnd()) ?? nil
                    c.lock.lock()
                    if let rest { c.all.append(rest) }
                    let text = String(decoding: c.all, as: UTF8.self)
                    c.lock.unlock()
                    cont.resume(returning: Shell.Result(status: proc.terminationStatus, output: text))
                }
                do { try p.run() } catch {
                    p.terminationHandler = nil
                    cont.resume(returning: Shell.Result(status: -1, output: error.localizedDescription))
                }
            }
        } onCancel: {
            if p.isRunning { p.terminate() }
        }
    }

    static func installPrism(report: @escaping Report,
                             holder: @MainActor (Downloader) -> Void) async throws -> AppEntry {
        await report("Consultando la última versión de Prism Launcher…", nil)
        let rel = try await GitHub.latest("PrismLauncher/PrismLauncher")
        guard let asset = GitHub.prismAsset(rel) else {
            throw InstallError("La versión \(rel.tag_name) no tiene un AppImage x86_64")
        }
        let file = try await download(asset, report: report, holder: holder)
        defer { try? FileManager.default.removeItem(at: file) }
        await report("Extrayendo el AppImage (sin ejecutarlo)…", nil)
        let dest = Paths.appsRoot.appendingPathComponent("prismlauncher")
        try await extract(file, kind: .appImage, to: dest)
        guard FileManager.default.fileExists(atPath: dest.appendingPathComponent("AppRun").path) else {
            throw InstallError("El AppImage no contiene AppRun")
        }
        return AppEntry(id: "prismlauncher", name: "Minecraft (Prism Launcher)",
                        icon: icons(in: dest).first?.path,
                        command: ["/opt/apps/prismlauncher/AppRun"],
                        kind: "prism", installDir: dest.path,
                        architecture: GuestArchitecture.x86_64.rawValue)
    }
}
