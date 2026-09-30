import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// "Añadir app": Heroic, Prism Launcher (Minecraft Java), a custom package,
/// or an Android APK (installed by scripts/android-pm.py; it cannot be opened
/// yet: docs/APK_SUPPORT.md).
struct AddAppView: View {
    @EnvironmentObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss

    enum Step { case choose, working, customPick, apkReview, done }
    @State private var step: Step = .choose
    @State private var status = ""
    @State private var progress: Double?
    @State private var error: String?
    @State private var downloader: Downloader?
    @State private var job: Task<Void, Never>?

    // Custom package state
    @State private var staging: URL?
    @State private var executables: [String] = []
    @State private var iconChoices: [URL] = []
    @State private var chosenExec = ""
    @State private var chosenIcon: URL?
    @State private var customName = ""
    @State private var showImporter = false
    @State private var compatibility: CompatibilityStatus?
    @State private var protonTool = ""

    // APK state (docs/APK_SUPPORT.md)
    @State private var apkURL: URL?
    @State private var apkInfo: AndroidPackageInfo?
    @State private var apkIcon: URL?
    @State private var apkInstalledVersion: String?
    @State private var apkIsUpdate = false

    private var isWindows: Bool {
        guard let staging else { return false }
        return Installer.isWindowsExecutable(staging.appendingPathComponent(chosenExec))
    }

    private var stagedELF: ELFInfo? {
        staging.map { ELFInspector.inspect($0.appendingPathComponent(chosenExec)) } ?? nil
    }

    private var targetEnvironment: LinuxBaseEnvironment? {
        if isWindows { return .legacyX86 }
        if let elf = stagedELF {
            guard let arch = elf.architecture else { return nil }
            return arch == .aarch64 ? .armroot : .legacyX86
        }
        return .legacyX86
    }

    private var architectureDescription: String {
        if isWindows { return "Windows (con Proton)" }
        guard let elf = stagedELF else { return "Script: se trata como x86-64 con FEX" }
        switch elf.architecture {
        case .aarch64: return "ARM64 (aarch64)"
        case .x86_64: return "x86-64"
        case .i386: return "i386"
        case nil: return "ELF sin arquitectura compatible (por ejemplo ARM de 32 bits)"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Añadir app").font(.title2.bold())
            switch step {
            case .choose: chooser
            case .working: working
            case .customPick: customForm
            case .apkReview: apkForm
            case .done:
                Label(status, systemImage: "checkmark.circle.fill").foregroundStyle(.green)
            }
            if let error {
                ScrollView {
                    Text(error).font(.callout).foregroundStyle(.red).textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 120)
            }
            HStack {
                Spacer()
                if step == .done {
                    Button("Cerrar") { dismiss() }.keyboardShortcut(.defaultAction)
                } else {
                    Button("Cancelar") { cancel() }.keyboardShortcut(.cancelAction)
                }
                if step == .apkReview {
                    Button(apkIsUpdate ? "Actualizar" : "Instalar") { installAPK() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(apkInfo?.installBlocker != nil)
                }
                if step == .customPick {
                    Button("Añadir") { finishCustom() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(chosenExec.isEmpty || customName.trimmingCharacters(in: .whitespaces).isEmpty
                                  || (isWindows && protonTool.isEmpty) || targetEnvironment == nil
                                  || !FileManager.default.fileExists(atPath: Paths.hostRoot(
                                      forGuestRoot: targetEnvironment?.guestRoot ?? "").path))
                }
            }
        }
        .padding(22)
        .frame(width: 580)
        .task {
            compatibility = await CompatibilityStatus.load(project: model.projectDir)
            protonTool = compatibility?.protons.first(where: { $0.supported })?.name ?? ""
        }
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.data, .item],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let u = urls.first { startCustom(u) }
        }
    }

    // MARK: pieces

    private var chooser: some View {
        VStack(spacing: 12) {
            option("Heroic Games Launcher (ARM64, experimental)",
                   "Epic y GOG. Heroic \(HeroicARM64.version) nativo para ARM64 en la raíz ARM64, armado con archivos oficiales; sin iniciar sesión ni juegos verificados. Amazon no funciona aún.",
                   "shield.lefthalf.filled") {
                let project = model.projectDir
                runKnown { r, _ in try await Installer.installHeroic(project: project, report: r) }
            }
            option("Minecraft Java (Prism Launcher)", "Descarga el AppImage x86_64 y lo extrae sin ejecutarlo.",
                   "cube.fill") { runKnown { r, h in try await Installer.installPrism(report: r, holder: h) } }
            option("Añadir APK (Android)",
                   "Instala un .apk (ARM64 preferido) y lo muestra en la biblioteca. Todavía no se puede abrir: el entorno Android de SteamARM aún no ejecuta apps.",
                   "apps.iphone") { pickAPK() }
            option("Personalizada (Linux / Windows)", "AppImage, archivo tar, .deb, ELF (ARM64 o x86) o .exe x86/x86_64. Windows usa el Proton que elijas.",
                   "shippingbox") { error = nil; showImporter = true }
        }
    }

    private func option(_ title: String, _ subtitle: String, _ icon: String,
                        action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Image(systemName: icon).font(.title).frame(width: 44)
                VStack(alignment: .leading, spacing: 3) {
                    Text(title).font(.headline)
                    Text(subtitle).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Image(systemName: "chevron.right").foregroundStyle(.secondary)
            }
            .padding(14)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.card))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private var working: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(status).font(.callout)
            if let progress {
                ProgressView(value: progress)
            } else {
                ProgressView().progressViewStyle(.linear)
            }
        }
    }

    private var customForm: some View {
        Form {
            TextField("Nombre", text: $customName)
            Picker("Ejecutable", selection: $chosenExec) {
                ForEach(executables, id: \.self) { Text($0).tag($0) }
            }
            LabeledContent("Arquitectura", value: architectureDescription)
            LabeledContent("Se instalará en", value: targetEnvironment.map { "\($0.name)  ·  opt/apps" } ?? "No disponible")
            if stagedELF?.architecture == .aarch64 {
                Text("Reconstruir la raíz ARM64 con scripts/mkarmroot.sh conserva sus apps: se queda con opt/apps y tmp/ de la raíz anterior.")
                    .font(.caption).foregroundStyle(.secondary)
                if !FileManager.default.fileExists(atPath: Paths.armRoot.path) {
                    Text("No hay raíz ARM64 instalada (scripts/mkarmroot.sh).")
                        .font(.caption).foregroundStyle(.red)
                }
            }
            if isWindows {
                Picker("Proton", selection: $protonTool) {
                    Text("Seleccionar Proton instalado").tag("")
                    ForEach(compatibility?.protons ?? []) { tool in
                        Text(tool.supported ? tool.name : "\(tool.name) · \(tool.status)")
                            .tag(tool.name).disabled(!tool.supported)
                    }
                }
                Text("Cada app tiene su propio prefijo. Para programas con DLL o datos adicionales, importa un archivo tar que incluya toda su carpeta. ARM64 todavía requiere adaptar el runtime zero-VM.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Picker("Icono", selection: $chosenIcon) {
                Text("Ninguno").tag(URL?.none)
                ForEach(iconChoices, id: \.self) { u in
                    Text(u.lastPathComponent).tag(URL?.some(u))
                }
            }
            if let chosenIcon, let img = NSImage(contentsOf: chosenIcon) {
                Image(nsImage: img).resizable().scaledToFit().frame(height: 64)
            }
        }
        .formStyle(.grouped)
        .frame(minHeight: 220)
    }

    private var apkForm: some View {
        Form {
            if let info = apkInfo {
                let abis = info.abis ?? []
                HStack(spacing: 14) {
                    if let apkIcon, let img = NSImage(contentsOf: apkIcon) {
                        Image(nsImage: img).resizable().interpolation(.high).scaledToFit().frame(width: 64, height: 64)
                    } else {
                        Image(systemName: "apps.iphone").font(.system(size: 40)).frame(width: 64, height: 64)
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        Text(info.label ?? info.package ?? "APK").font(.headline)
                        Text(info.package ?? "sin nombre de paquete").font(.caption.monospaced())
                            .foregroundStyle(.secondary).textSelection(.enabled)
                    }
                }
                LabeledContent("Versión", value: info.versionLabel)
                LabeledContent("SDK", value: "mínimo \(info.minSdk ?? "1") · objetivo \(info.targetSdk ?? info.minSdk ?? "1")")
                LabeledContent("Código nativo") {
                    Text(info.abiVerdict.label(abis: abis))
                        .foregroundStyle(info.abiVerdict == .arm64 || info.abiVerdict == .none ? Color.secondary : Color.orange)
                        .multilineTextAlignment(.trailing)
                }
                if let bundle = info.bundle {
                    LabeledContent("APK divididos", value: "\(bundle.selectedSplitCount) se instalarán")
                    if let abi = bundle.chosenAbi {
                        LabeledContent("Arquitectura elegida", value: abi)
                    }
                }
                LabeledContent("Actividad principal", value: info.launcherActivity ?? "ninguna: no tiene icono de inicio")
                LabeledContent("Firma", value: info.signingSchemes.map { "\($0) (no se verifica la firma criptográfica)" } ?? "sin firma")
                LabeledContent("Tamaño", value: ByteCountFormatter.string(fromByteCount: Int64(info.size ?? 0), countStyle: .file))
                if apkIsUpdate {
                    Text("Ya está instalada (\(apkInstalledVersion ?? "otra versión")): se actualizará y se conservarán sus datos.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                let perms = (info.permissions ?? []).map(\.name)
                DisclosureGroup("Permisos (\(perms.count))") {
                    ForEach(perms, id: \.self) { Text($0).font(.caption.monospaced()).textSelection(.enabled) }
                }
                if let blocker = info.installBlocker {
                    Text(blocker).font(.callout).foregroundStyle(.red)
                }
                let preview = AndroidAppInfo(package: info.package, minSdk: info.minSdk, abis: info.abis ?? [])
                if let reason = AndroidApps.unavailableReason(preview) {
                    Text("Se instala en SteamARM y aparece en la biblioteca, pero no se puede abrir: \(reason).")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    Text(AndroidApps.sessionNote).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .frame(minHeight: 320, maxHeight: 480)
    }

    // MARK: actions

    private func report(_ s: String, _ p: Double?) {
        status = s
        progress = p
    }

    private func runKnown(_ install: @escaping (@escaping Installer.Report,
                                                @MainActor (Downloader) -> Void) async throws -> AppEntry) {
        error = nil
        step = .working
        job = Task { @MainActor in
            do {
                let entry = try await install({ s, p in report(s, p) }, { d in downloader = d })
                model.upsert(entry)
                status = "\(entry.name) instalado."
                step = .done
            } catch {
                if !Task.isCancelled { self.error = error.localizedDescription }
                step = .choose
            }
            downloader = nil
        }
    }

    private func startCustom(_ file: URL) {
        guard let kind = ArchiveKind.detect(file) else {
            error = "Formato no reconocido: \(file.lastPathComponent)"
            return
        }
        error = nil
        step = .working
        report("Extrayendo \(file.lastPathComponent)…", nil)
        let dest = Paths.appsRoot.appendingPathComponent(".staging-\(UUID().uuidString.prefix(8))")
        staging = dest
        job = Task { @MainActor in
            do {
                try await Installer.extract(file, kind: kind, to: dest)
                let (execs, icons) = await Task.detached {
                    (Installer.executables(in: dest), Installer.icons(in: dest))
                }.value
                guard !execs.isEmpty else { throw InstallError("No hay ningún ejecutable en el paquete") }
                executables = execs
                iconChoices = icons
                chosenExec = execs[0]
                chosenIcon = icons.first
                customName = Self.guessName(file)
                step = .customPick
            } catch {
                if !Task.isCancelled { self.error = error.localizedDescription }
                removeStaging()
                step = .choose
            }
        }
    }

    private func finishCustom() {
        guard let staging else { return }
        let windows = isWindows
        let arch = stagedELF?.architecture
        guard let env = targetEnvironment else { return }
        guard !windows || !protonTool.isEmpty else { return }
        let name = customName.trimmingCharacters(in: .whitespaces)
        let id = model.uniqueId(for: name)
        let hostRoot = Paths.hostRoot(forGuestRoot: env.guestRoot)
        guard FileManager.default.fileExists(atPath: hostRoot.path) else { return }
        let apps = hostRoot.appendingPathComponent("opt/apps")
        let final = apps.appendingPathComponent(id)
        do {
            try FileManager.default.createDirectory(at: apps, withIntermediateDirectories: true)
            try FileManager.default.moveItem(at: staging, to: final)
        } catch {
            self.error = error.localizedDescription
            return
        }
        self.staging = nil
        var icon: String?
        if let chosenIcon {
            let sp = staging.standardizedFileURL.path, ip = chosenIcon.standardizedFileURL.path
            icon = ip.hasPrefix(sp) ? final.path + ip.dropFirst(sp.count) : ip
        }
        let guest = Paths.guestPath(for: final.appendingPathComponent(chosenExec), in: hostRoot)
        model.upsert(AppEntry(id: id, name: name, icon: icon, command: [guest],
                              root: env.guestRoot, fexRootfs: arch == .aarch64 ? nil : "/",
                              kind: windows ? "windows" : "custom", installDir: final.path,
                              architecture: windows ? nil : arch?.rawValue,
                              protonTool: windows ? protonTool : nil))
        status = "\(name) añadida."
        step = .done
    }

    private func cancel() {
        job?.cancel()
        downloader?.cancel()
        removeStaging()
        removeAPKIcon()
        dismiss()
    }

    // MARK: APK

    private func pickAPK() {
        error = nil
        let panel = NSOpenPanel()
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        // Accept installable APK sets; AAB still needs bundletool and a signing key.
        panel.allowedContentTypes = ["apk", "xapk", "apks", "apkm", "aab"].compactMap { UTType(filenameExtension: $0) }
        panel.message = "Elige un APK de Android"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        step = .working
        report("Leyendo \(url.lastPathComponent)…", nil)
        removeAPKIcon()
        let icon = FileManager.default.temporaryDirectory
            .appendingPathComponent("steamarm-apk-icon-\(UUID().uuidString)")
        job = Task { @MainActor in
            let (info, problem) = await model.inspectAPK(url, iconTo: icon)
            if Task.isCancelled { return }
            guard let info, problem == nil else {
                error = problem
                try? FileManager.default.removeItem(at: icon)
                step = .choose
                return
            }
            apkURL = url
            apkInfo = info
            apkIcon = FileManager.default.fileExists(atPath: icon.path) ? icon : nil
            let installed = info.package.flatMap { model.installedAndroidVersion($0) }
            apkIsUpdate = installed != nil
            apkInstalledVersion = installed.map { v in
                [v.name, v.code.map { "(\($0))" }].compactMap { $0 }.joined(separator: " ")
            }
            step = .apkReview
        }
    }

    private func installAPK() {
        guard let url = apkURL, let info = apkInfo, info.installBlocker == nil else { return }
        error = nil
        step = .working
        report("Instalando \(info.label ?? url.lastPathComponent)…", nil)
        let update = apkIsUpdate
        job = Task { @MainActor in
            let (entry, problem) = await model.installAPK(url, info: info)
            if let entry {
                status = "\(entry.name) \(update ? "actualizada" : "instalada"). Está en la biblioteca (filtro Android)."
                removeAPKIcon()
                step = .done
            } else {
                error = problem
                step = .apkReview
            }
        }
    }

    private func removeAPKIcon() {
        if let apkIcon { try? FileManager.default.removeItem(at: apkIcon) }
        apkIcon = nil
    }

    private func removeStaging() {
        if let s = staging { try? FileManager.default.removeItem(at: s) }
        staging = nil
    }

    static func guessName(_ file: URL) -> String {
        var n = file.lastPathComponent
        for ext in [".appimage", ".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".txz", ".tar", ".deb", ".exe"]
            where n.lowercased().hasSuffix(ext) {
            n = String(n.dropLast(ext.count))
            break
        }
        // Drop a trailing "-1.2.3", "_x86_64", "-linux" and the like.
        let parts = n.split(whereSeparator: { $0 == "-" || $0 == "_" })
        let kept = parts.prefix { p in
            let s = p.lowercased()
            return !(s.first?.isNumber ?? false) && !["linux", "x86", "x86", "amd64", "x64", "x86"].contains(s)
                && !s.hasPrefix("x86") && s != "v"
        }
        let name = kept.isEmpty ? n : kept.joined(separator: " ")
        return name.prefix(1).uppercased() + name.dropFirst()
    }
}
