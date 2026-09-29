import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// "Añadir app": Heroic, Prism Launcher (Minecraft Java) or a custom package.
struct AddAppView: View {
    @EnvironmentObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss

    enum Step { case choose, working, customPick, done }
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

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Añadir app").font(.title2.bold())
            switch step {
            case .choose: chooser
            case .working: working
            case .customPick: customForm
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
                if step == .customPick {
                    Button("Añadir") { finishCustom() }
                        .keyboardShortcut(.defaultAction)
                        .disabled(chosenExec.isEmpty || customName.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
        .padding(22)
        .frame(width: 580)
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.data, .item],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let u = urls.first { startCustom(u) }
        }
    }

    // MARK: pieces

    private var chooser: some View {
        VStack(spacing: 12) {
            option("Heroic Games Launcher", "Epic, GOG y Amazon. Descarga la última versión para Linux x64.",
                   "shield.lefthalf.filled") { runKnown { r, h in try await Installer.installHeroic(report: r, holder: h) } }
            option("Minecraft Java (Prism Launcher)", "Descarga el AppImage x86_64 y lo extrae sin ejecutarlo.",
                   "cube.fill") { runKnown { r, h in try await Installer.installPrism(report: r, holder: h) } }
            option("Personalizada", "Un .AppImage, .tar.gz / .tar.xz / .tgz, .deb o un ejecutable ELF.",
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
        let name = customName.trimmingCharacters(in: .whitespaces)
        let id = model.uniqueId(for: name)
        let final = Paths.appsRoot.appendingPathComponent(id)
        do {
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
        let guest = Paths.guestPath(for: final.appendingPathComponent(chosenExec))
        // The program's ISA from its ELF header (a script keeps the x86 default).
        let arch = ELFInspector.inspect(final.appendingPathComponent(chosenExec))?.architecture
        model.upsert(AppEntry(id: id, name: name, icon: icon, command: [guest],
                              kind: "custom", installDir: final.path,
                              architecture: arch?.rawValue))
        status = "\(name) añadida."
        step = .done
    }

    private func cancel() {
        job?.cancel()
        downloader?.cancel()
        removeStaging()
        dismiss()
    }

    private func removeStaging() {
        if let s = staging { try? FileManager.default.removeItem(at: s) }
        staging = nil
    }

    static func guessName(_ file: URL) -> String {
        var n = file.lastPathComponent
        for ext in [".appimage", ".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".txz", ".tar", ".deb"]
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
