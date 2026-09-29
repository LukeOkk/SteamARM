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

    /// Heroic: the Linux x64 tarball.
    static func heroicAsset(_ r: Release) -> Asset? {
        r.assets.first { $0.name.lowercased().hasSuffix("linux-x64.tar.xz") }
            ?? r.assets.first {
                let n = $0.name.lowercased()
                return n.contains("linux") && n.hasSuffix(".tar.xz") && !n.contains("arm")
            }
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
    case appImage, tarball, deb, elf

    static func detect(_ url: URL) -> ArchiveKind? {
        let n = url.lastPathComponent.lowercased()
        if n.hasSuffix(".appimage") { return .appImage }
        if n.hasSuffix(".tar.gz") || n.hasSuffix(".tgz") || n.hasSuffix(".tar.xz")
            || n.hasSuffix(".txz") || n.hasSuffix(".tar.bz2") || n.hasSuffix(".tar") { return .tarball }
        if n.hasSuffix(".deb") { return .deb }
        if Installer.isELF(url) { return .elf }
        return nil
    }
}

enum Installer {
    static var downloads: URL { Paths.launcherDir.appendingPathComponent("downloads") }

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
        case .elf:
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
            if isELF(u) || fm.isExecutableFile(atPath: u.path) { out.append(rel) }
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

    static func installHeroic(report: @escaping Report,
                              holder: @MainActor (Downloader) -> Void) async throws -> AppEntry {
        await report("Consultando la última versión de Heroic…", nil)
        let rel = try await GitHub.latest("Heroic-Games-Launcher/HeroicGamesLauncher")
        guard let asset = GitHub.heroicAsset(rel) else {
            throw InstallError("La versión \(rel.tag_name) no tiene un .tar.xz para Linux x64")
        }
        let file = try await download(asset, report: report, holder: holder)
        defer { try? FileManager.default.removeItem(at: file) }
        await report("Extrayendo \(asset.name)…", nil)
        let dest = Paths.appsRoot.appendingPathComponent("heroic")
        try await extract(file, kind: .tarball, to: dest)
        let subdirs = (try? FileManager.default.contentsOfDirectory(atPath: dest.path)) ?? []
        guard let dir = subdirs.sorted().first(where: {
            FileManager.default.fileExists(atPath: dest.appendingPathComponent("\($0)/heroic").path)
        }) else { throw InstallError("No se encontró el ejecutable heroic tras extraer") }
        let icon = icons(in: dest.appendingPathComponent(dir)).first
        return AppEntry(id: "heroic", name: "Heroic Games Launcher", icon: icon?.path,
                        command: ["/opt/apps/heroic/\(dir)/heroic", "--no-sandbox"],
                        kind: "heroic", installDir: dest.path,
                        architecture: GuestArchitecture.x86_64.rawValue)
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
