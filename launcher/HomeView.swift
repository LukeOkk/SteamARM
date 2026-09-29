import AppKit
import SwiftUI

struct HomeView: View {
    @EnvironmentObject var model: LauncherModel
    @State private var showAdd = false
    @State private var editing: AppEntry?
    @State private var deleting: AppEntry?
    @State private var builtInInfo: AppEntry?
    @State private var query = ""
    @State private var filter: LibraryFilter = .all

    private let columns = [GridItem(.adaptive(minimum: 170, maximum: 210), spacing: 20)]

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider().opacity(0.3)
            if model.phase != .idle, let app = model.running {
                RunningView(app: app)
            } else {
                if model.needsSetup { SetupBanner() }
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 20) {
                        ForEach(model.visibleApps(query: query, filter: filter)) { app in
                            AppCard(app: app) { model.launch(app) }
                                .contextMenu {
                                    Button("Abrir") { model.launch(app) }
                                        .disabled(!model.canLaunch || model.unavailableReason(app) != nil)
                                    Button("Ajustes…") {
                                        if app.isBuiltIn { builtInInfo = app } else { editing = app }
                                    }
                                    Button("Abrir carpeta") { model.openFolder(app) }
                                    Button("Abrir registros") { model.openLogs(app) }
                                    Button(model.isFavorite(app) ? "Quitar de favoritas" : "Añadir a favoritas") {
                                        model.toggleFavorite(app)
                                    }
                                    if !app.isBuiltIn {
                                        Divider()
                                        Button("Eliminar", role: .destructive) { deleting = app }
                                    }
                                }
                        }
                        AddCard { showAdd = true }
                    }
                    .padding(24)
                    if model.visibleApps(query: query, filter: filter).isEmpty {
                        Text(query.isEmpty ? "No hay apps en «\(filter.label)»." : "Ninguna app coincide con la búsqueda.")
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity)
                            .padding(.top, 28)
                    }
                }
            }
        }
        .background(Theme.background)
        .sheet(isPresented: $showAdd) { AddAppView().environmentObject(model) }
        .sheet(item: $editing) { app in EditAppView(app: app).environmentObject(model) }
        .sheet(item: $builtInInfo) { app in BuiltInInfoView(app: app).environmentObject(model) }
        .confirmationDialog("¿Eliminar \(deleting?.name ?? "")?",
                            isPresented: Binding(get: { deleting != nil },
                                                 set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible) {
            Button("Eliminar", role: .destructive) {
                if let d = deleting { model.delete(d) }
                deleting = nil
            }
            Button("Cancelar", role: .cancel) { deleting = nil }
        } message: {
            Text("Se borrará también su carpeta de instalación.")
        }
        .alert("SteamARM", isPresented: Binding(get: { model.alert != nil },
                                                set: { if !$0 { model.alert = nil } })) {
            if model.offerStop {
                Button("Detener procesos Linux", role: .destructive) {
                    model.stopAll(); model.offerStop = false; model.alert = nil
                }
            }
            Button("Aceptar") { model.offerStop = false; model.alert = nil }
        } message: {
            Text(model.alert ?? "")
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 14) {
                Image(systemName: "cpu").font(.title2).foregroundStyle(Theme.accent)
                VStack(alignment: .leading, spacing: 2) {
                    Text("SteamARM").font(.title2.bold())
                    Text("Apps de Linux en tu Mac, sin máquina virtual")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Button { showAdd = true } label: {
                    Label("Añadir app", systemImage: "plus")
                }
                .disabled(!model.canLaunch)
                SettingsLink {
                    Label("Ajustes", systemImage: "gearshape")
                }
            }
            HStack {
                TextField("Buscar", text: $query)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 180)
                Picker("Filtro", selection: $filter) {
                    ForEach(LibraryFilter.allCases) { item in Text(item.label).tag(item) }
                }
                .labelsHidden()
                .pickerStyle(.segmented)
            }
        }
        .padding(.horizontal, 24)
        .padding(.top, 30)
        .padding(.bottom, 14)
    }
}

struct AppCard: View {
    @EnvironmentObject var model: LauncherModel
    let app: AppEntry
    let action: () -> Void
    @State private var hover = false

    private var reason: String? { model.unavailableReason(app) }
    private var platform: String {
        if app.isWindows { return "Windows" }
        switch GuestArchitecture.of(app.architecture) {
        case .aarch64: return "ARM64"
        case .x86_64: return "x86-64"
        case .i386: return "i386"
        case nil: return app.architecture ?? "Desconocida"
        }
    }

    var body: some View {
        Button(action: action) {
            VStack(spacing: 8) {
                AppIconView(app: app, size: 80)
                    .frame(maxWidth: .infinity)
                    .padding(.top, 18)
                Text(app.name)
                    .font(.headline)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 8)
                Text(Library.subtitle(model.stats[app.id]))
                    .font(.caption).foregroundStyle(.secondary)
                    .lineLimit(2).multilineTextAlignment(.center)
                HStack(spacing: 5) {
                    Text(platform).padding(.horizontal, 7).padding(.vertical, 3)
                        .background(.gray.opacity(0.3), in: Capsule())
                    if app.isExperimental {
                        Text("Experimental").padding(.horizontal, 7).padding(.vertical, 3)
                            .background(.orange.opacity(0.3), in: Capsule())
                    }
                    if model.isFavorite(app) { Image(systemName: "star.fill").foregroundStyle(.yellow) }
                }
                .font(.caption2)
                if let reason {
                    Text(reason.prefix(1).uppercased() + reason.dropFirst())
                        .font(.caption2).foregroundStyle(.orange)
                        .lineLimit(3).multilineTextAlignment(.center)
                }
            }
            .padding(.horizontal, 8)
            .frame(height: 300)
            .background(RoundedRectangle(cornerRadius: 14).fill(Theme.card))
            .overlay(RoundedRectangle(cornerRadius: 14)
                .stroke(hover ? Theme.accent : .clear, lineWidth: 2))
            .scaleEffect(hover ? 1.03 : 1)
            .animation(.easeOut(duration: 0.12), value: hover)
        }
        .buttonStyle(.plain)
        .onHover { hover = $0 }
        .disabled(!model.canLaunch || reason != nil)
        .help(reason ?? "Abrir \(app.name)")
    }
}

struct AddCard: View {
    let action: () -> Void
    @State private var hover = false

    var body: some View {
        Button(action: action) {
            VStack(spacing: 12) {
                Image(systemName: "plus.circle").font(.system(size: 44))
                Text("Añadir app").font(.headline)
            }
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity)
            .frame(height: 300)
            .background(RoundedRectangle(cornerRadius: 14)
                .strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [7]))
                .foregroundStyle(hover ? Theme.accent : .gray.opacity(0.4)))
        }
        .buttonStyle(.plain)
        .onHover { hover = $0 }
    }
}

struct RunningView: View {
    @EnvironmentObject var model: LauncherModel
    let app: AppEntry

    var body: some View {
        VStack(spacing: 22) {
            Spacer()
            AppIconView(app: app, size: 128)
            Text(title).font(.largeTitle.bold())
            if model.phase == .starting {
                ProgressView().controlSize(.small)
                Text("Arrancando el servidor X y la app…").foregroundStyle(.secondary)
            } else if model.phase == .stopping {
                ProgressView().controlSize(.small)
            } else {
                Text(model.runningDisplay == .vnc
                     ? "La app se muestra en Compartir Pantalla (contraseña: \(model.vncPassword))."
                     : "La app se abre en ventanas nativas de macOS.")
                    .foregroundStyle(.secondary)
            }
            VStack(alignment: .leading, spacing: 7) {
                LabeledContent("Arquitectura", value: architectureLabel)
                LabeledContent("Entorno base", value: environmentLabel)
                LabeledContent("Modo de sesión", value: sessionMode)
                    .help("Sin máquina virtual: lxrun ejecuta el programa directamente (aarch64) o con FEX (x86).")
                TimelineView(.periodic(from: .now, by: 1)) { context in
                    LabeledContent("Tiempo") {
                        Text(elapsed(at: context.date)).monospacedDigit()
                    }
                }
            }
            .frame(maxWidth: 460)
            .foregroundStyle(.secondary)
            HStack(spacing: 14) {
                Button(role: .destructive) { model.stop() } label: {
                    Label("Detener", systemImage: "stop.fill").frame(minWidth: 120)
                }
                .controlSize(.large)
                .disabled(model.phase == .stopping)
                Button { model.showDisplay() } label: {
                    Label("Mostrar pantalla", systemImage: "display").frame(minWidth: 160)
                }
                .controlSize(.large)
                .buttonStyle(.borderedProminent)
                .disabled(model.phase != .running)
                Button { model.openLog() } label: {
                    Label("Registro", systemImage: "doc.text")
                }
                .controlSize(.large)
            }
            Spacer()
            Text("Se volverá a la biblioteca cuando la app termine.")
                .font(.caption).foregroundStyle(.secondary)
                .padding(.bottom, 16)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var title: String {
        switch model.phase {
        case .stopping: return "Deteniendo \(app.name)"
        case .starting: return "Iniciando \(app.name)"
        default: return "Ejecutando \(app.name)"
        }
    }

    private var architectureLabel: String {
        if let actual = model.runningArch, let arch = actual.architecture {
            return "\(arch.rawValue) · \(actual.translator == "FEX" ? "FEX" : "sin traductor")"
        }
        if let plan = try? LaunchPlanner.plan(architecture: app.architecture) {
            return "\(plan.architecture.rawValue) · \(plan.runner == .fex ? "FEX" : "sin traductor") (según la entrada)"
        }
        return "\(app.architecture ?? "x86_64") (según la entrada)"
    }

    private var environmentLabel: String {
        guard let env = model.environment(of: app) else { return app.root }
        return env.name + (env.transitional ? " (transicional)" : "")
    }

    private var sessionMode: String {
        let plan = try? LaunchPlanner.plan(architecture: app.architecture)
        return SessionVirtualizationMode(plan?.usesVirtualMachine == true ? .appleHypervisorLegacy : .auto).label
    }

    private func elapsed(at date: Date) -> String {
        let seconds = max(0, Int(date.timeIntervalSince(model.session.startedAt ?? date)))
        let time = String(format: "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60)
        return time + (model.runtimeUnknown ? " (desde que el launcher la encontró)" : "")
    }
}

/// Read-only details of a bundled app.
struct BuiltInInfoView: View {
    @EnvironmentObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    let app: AppEntry

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(app.name).font(.title2.bold())
            Form {
                LabeledContent("Nombre", value: app.name)
                LabeledContent("Estado", value: app.isExperimental ? "Experimental" : "Listo")
                LabeledContent("Arquitectura", value: app.architecture ?? "x86_64")
                LabeledContent("Traductor", value: GuestArchitecture.of(app.architecture)?.needsTranslator == false ? "ninguno" : "FEX")
                LabeledContent("Entorno base", value: environmentLabel)
                LabeledContent("Raíz", value: app.root)
                LabeledContent("Orden") {
                    Text(app.command.joined(separator: " ")).font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                }
                LabeledContent("Variables") {
                    Text(app.env.isEmpty ? "Ninguna" : app.env.sorted { $0.key < $1.key }
                        .map { "\($0.key)=\($0.value)" }.joined(separator: "\n"))
                        .font(.system(.body, design: .monospaced)).textSelection(.enabled)
                }
                LabeledContent("Modo de sesión", value: "ZERO-VM")
                if app.id == "steam-arm64" {
                    Text("El cliente ARM64 nativo, sin FEX: llega a su ventana de inicio de sesión (benchmarks/stage22-23); lo demás aún no está verificado. Vive en la raíz ARM64 transicional de scripts/mkarmroot.sh (Fedora); reconstruirla conserva tmp/ y opt/apps.")
                        .font(.caption).foregroundStyle(.secondary)
                } else if app.id == "steam-arm64-frame" {
                    Text("El mismo cliente ARM64 sobre la base oficial de Steam Frame (SteamOS holo), derivada de tu imagen de recuperación con scripts/mkframeroot.sh: la base que busca la migración ARM64. Llega a su ventana de inicio de sesión (benchmarks/stage23-frame-root).")
                        .font(.caption).foregroundStyle(.secondary)
                } else if app.id == "steam" {
                    Text("El cliente x86 bajo FEX: la ruta que funciona hoy (compatibilidad transicional).")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text("Los ajustes por app no se aplican a las entradas integradas: usan la configuración global.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .formStyle(.grouped)
            HStack {
                Spacer()
                Button("Cerrar") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 520)
    }

    private var environmentLabel: String {
        guard let env = model.environment(of: app) else { return app.root }
        return env.name + (env.transitional ? " (transicional)" : "")
    }
}

/// Name, command line (one argument per line), environment and icon.
struct EditAppView: View {
    @EnvironmentObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    @State var app: AppEntry
    @State private var commandText = ""
    @State private var envText = ""
    @State private var compatibility: CompatibilityStatus?

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Editar \(app.name)").font(.title2.bold())
            Form {
                TextField("Nombre", text: $app.name)
                if app.kind == "windows" {
                    Picker("Proton", selection: Binding(get: { app.protonTool ?? "Proton - Experimental" },
                                                        set: { app.protonTool = $0 })) {
                        ForEach(compatibility?.protons ?? []) { tool in
                            Text(tool.supported ? tool.name : "\(tool.name) · \(tool.status)")
                                .tag(tool.name).disabled(!tool.supported)
                        }
                    }
                    Text("Cambiar Proton conserva el prefijo de esta app; una versión anterior puede no aceptar un prefijo actualizado.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                LabeledContent("Icono") {
                    HStack {
                        AppIconView(app: app, size: 32)
                        Text(app.icon ?? "Ninguno").lineLimit(1).truncationMode(.middle)
                            .foregroundStyle(.secondary)
                        Spacer()
                        Button("Elegir…") { pickIcon() }
                        if app.icon != nil { Button("Quitar") { app.icon = nil } }
                    }
                }
                VStack(alignment: .leading) {
                    Text("Orden (ruta del invitado; un argumento por línea)")
                    TextEditor(text: $commandText)
                        .font(.system(.body, design: .monospaced))
                        .frame(height: 90)
                }
                VStack(alignment: .leading) {
                    Text("Variables de entorno (CLAVE=valor por línea)")
                    TextEditor(text: $envText)
                        .font(.system(.body, design: .monospaced))
                        .frame(height: 70)
                }
                Section("Ajustes de esta app") {
                    Picker("Pantalla", selection: overrideBinding("display")) {
                        Text("Global (\(model.settings.display == .vnc ? "VNC" : "Ventanas nativas"))").tag("")
                        Text("Ventanas nativas").tag("native")
                        Text("VNC (Compartir Pantalla)").tag("vnc")
                            .disabled(!Paths.isX86Root(app.root))
                            .help("VNC solo sirve a programas de la raíz x86 de Steam")
                    }
                    Picker("Sincronización vertical", selection: overrideBinding("vsync")) {
                        Text("Global (\(vsyncLabel(model.settings.vsync)))").tag("")
                        Text("AUTO (según el juego)").tag("game")
                        Text("ON (activada)").tag("on")
                        Text("OFF (desactivada)").tag("off")
                    }
                    Picker("Sincronización", selection: overrideBinding("synchronization")) {
                        Text("Global (\(model.settings.synchronizationBackend.label))").tag("")
                        ForEach(SynchronizationBackend.allCases, id: \.self) { backend in
                            let status = model.capabilities.synchronization[backend]
                            Text(backend.label + (backend == .auto || status?.state == .ready ? "" : " · \(status?.state.label ?? "No disponible")"))
                                .tag(backend.rawValue)
                                .disabled(backend != .auto && status?.usable != true)
                                .help(status?.reason ?? "Sin datos")
                        }
                    }
                    Picker("Gráficos", selection: overrideBinding("graphicsBackend")) {
                        Text("Global (\(model.settings.graphics.label))").tag("")
                        ForEach(graphicsOrder, id: \.self) { backend in
                            let status = model.capabilities.graphics[backend]
                            Text(backend.label + (backend == .auto || status?.state == .ready ? "" : " · \(status?.state.label ?? "No disponible")"))
                                .tag(backend.rawValue)
                                .disabled(backend != .auto && status?.usable != true)
                                .help(status?.reason ?? "Sin datos")
                        }
                    }
                    Text("Se aplican solo a esta app, por encima de la configuración global. Los juegos que abre Steam usan la de Steam.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
            HStack {
                Spacer()
                Button("Cancelar") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Guardar") { save() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(app.name.isEmpty || commandLines.isEmpty)
            }
        }
        .padding(20)
        .frame(width: 560)
        .task { compatibility = await CompatibilityStatus.load(project: model.projectDir) }
        .onAppear {
            commandText = app.command.joined(separator: "\n")
            envText = app.env.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }
                .joined(separator: "\n")
        }
    }

    private var commandLines: [String] {
        commandText.split(separator: "\n").map { String($0) }.filter { !$0.isEmpty }
    }

    private var graphicsOrder: [GraphicsBackend] {
        [.auto, .vulkanMoltenVK, .vulkanKosmicKrisp, .openGLWineD3D]
    }

    private func vsyncLabel(_ value: String) -> String {
        switch value {
        case "on": return "ON (activada)"
        case "off": return "OFF (desactivada)"
        default: return "AUTO (según el juego)"
        }
    }

    private func overrideBinding(_ key: String) -> Binding<String> {
        Binding(get: { app.overrides?[key] ?? "" }, set: { value in
            if key == "display" && value == "vnc" && !Paths.isX86Root(app.root) { return }
            if key == "synchronization", let backend = SynchronizationBackend(rawValue: value),
               backend != .auto && model.capabilities.synchronization[backend]?.usable != true { return }
            if key == "graphicsBackend", let backend = GraphicsBackend(rawValue: value),
               backend != .auto && model.capabilities.graphics[backend]?.usable != true { return }
            let valid: Bool
            switch key {
            case "display": valid = ["", "native", "vnc"].contains(value)
            case "vsync": valid = ["", "game", "on", "off"].contains(value)
            case "synchronization": valid = value.isEmpty || SynchronizationBackend(rawValue: value) != nil
            case "graphicsBackend": valid = value.isEmpty || GraphicsBackend(rawValue: value) != nil
            default: valid = false
            }
            guard valid else { return }
            var values = app.overrides ?? [:]
            if value.isEmpty { values.removeValue(forKey: key) } else { values[key] = value }
            app.overrides = values.isEmpty ? nil : values
        })
    }

    private func save() {
        app.command = commandLines
        var env: [String: String] = [:]
        for line in envText.split(separator: "\n") {
            let parts = line.split(separator: "=", maxSplits: 1)
            if parts.count == 2 {
                env[parts[0].trimmingCharacters(in: .whitespaces)] = String(parts[1])
            }
        }
        app.env = env
        model.upsert(app)
        dismiss()
    }

    private func pickIcon() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.image]
        panel.canChooseDirectories = false
        if panel.runModal() == .OK, let u = panel.url { app.icon = u.path }
    }
}

/// First start of a downloaded app: nothing is built and there is no Steam
/// root yet. scripts/setup.sh does both (docs/INSTALL.md).
struct SetupBanner: View {
    @EnvironmentObject var model: LauncherModel

    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: "shippingbox.and.arrow.backward")
                .font(.system(size: 30)).foregroundStyle(Theme.accent)
            VStack(alignment: .leading, spacing: 6) {
                Text(model.needsUpdate && !model.needsFirstInstall ? "SteamARM se actualizó: falta recompilar"
                     : "Falta instalar SteamARM").font(.headline)
                Text("El instalador compila el runtime y FEX, descarga los sistemas Linux y Steam, "
                     + "y lo deja todo listo sin máquina virtual. Necesita las Command Line Tools "
                     + "de Xcode, Homebrew, unos 25 GB libres y entre 20 y 60 minutos. Se abre en "
                     + "Terminal; si se interrumpe, vuelve a pulsar Instalar y continúa.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button("Instalar") { model.runSetup() }.buttonStyle(.borderedProminent)
                    Button("Guía de instalación") {
                        NSWorkspace.shared.open(URL(string: "https://github.com/LukeOkk/SteamARM/blob/main/docs/INSTALL.md")!)
                    }
                }
            }
            Spacer()
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.white.opacity(0.05)))
        .padding(.horizontal, 24).padding(.top, 16)
    }
}
