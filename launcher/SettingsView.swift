import AppKit
import SwiftUI
import ApplicationServices

enum SettingsSection: String, CaseIterable, Identifiable {
    case interface = "Interfaz", input = "Entrada", system = "Sistema", processor = "Procesador"
    case graphics = "Gráficos", sound = "Sonido", shortcuts = "Atajos", logs = "Registros", debug = "Depuración"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .interface: return "macwindow"
        case .input: return "gamecontroller"
        case .system: return "gearshape"
        case .processor: return "cpu"
        case .graphics: return "display"
        case .sound: return "speaker.wave.2"
        case .shortcuts: return "keyboard"
        case .logs: return "doc.text"
        case .debug: return "ladybug"
        }
    }
}

struct SettingsView: View {
    @EnvironmentObject var model: LauncherModel
    @EnvironmentObject var pads: ControllerManager
    @Environment(\.dismiss) private var dismiss
    @State private var draft = LauncherSettings()
    @State private var selection: SettingsSection = .interface
    @State private var reset = false
    @State private var trusted = AXIsProcessTrusted()
    @State private var sessionStarted = false
    @State private var saveError: String?

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                VStack(alignment: .leading, spacing: 5) {
                    Text("Configuración").font(.headline).padding(.bottom, 16)
                    ForEach(SettingsSection.allCases) { section in
                        Button { selection = section } label: {
                            Label(section.rawValue, systemImage: section.symbol)
                                .frame(maxWidth: .infinity, alignment: .leading).padding(10)
                                .background(selection == section ? Theme.accent.opacity(0.3) : .clear,
                                            in: RoundedRectangle(cornerRadius: 7))
                        }.buttonStyle(.plain)
                    }
                    Spacer()
                }.padding(16).frame(width: 180).background(Theme.background)
                Divider()
                Group {
                    if selection == .input { ControllersView() }
                    else { Form { sectionContent }.formStyle(.grouped) }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            Divider()
            VStack(spacing: 8) {
                HStack {
                    Toggle("Quiero restablecer mi configuración", isOn: $reset).toggleStyle(.checkbox)
                    Button("Restablecer la configuración") {
                        draft = LauncherSettings()
                        pads.config = ControllersConfig()
                        reset = false
                    }.disabled(!reset)
                    Spacer()
                    Button("Aplicar") { _ = apply() }
                    Button("Cancelar") { dismiss() }.keyboardShortcut(.cancelAction)
                    Button("Aceptar") { if apply() { dismiss() } }.keyboardShortcut(.defaultAction)
                }
                if model.running != nil {
                    Text("Los cambios se aplican al reiniciar Steam / la app.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }.padding(14)
        }
        .frame(width: 1160, height: 790)
        .preferredColorScheme(.dark)
        .onAppear {
            guard !sessionStarted else { return }
            draft = model.settings
            pads.beginEditing()
            sessionStarted = true
            HotkeyManager.shared.settingsOpen = true
        }
        .onDisappear {
            pads.endEditing()
            sessionStarted = false
            HotkeyManager.shared.settingsOpen = false
        }
        .alert("No se pudo guardar la configuración", isPresented: Binding(get: { saveError != nil }, set: { if !$0 { saveError = nil } })) {
            Button("Aceptar") { saveError = nil }
        } message: { Text(saveError ?? "") }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            trusted = AXIsProcessTrusted()
        }
    }

    @discardableResult private func apply() -> Bool {
        do {
            try Store.saveChecked(draft, to: Paths.settingsFile)
            try pads.applyEditing()
            model.settings = draft
            return true
        } catch {
            saveError = error.localizedDescription
            return false
        }
    }

    @ViewBuilder private var sectionContent: some View {
        switch selection {
        case .interface: interfaceSection
        case .system: systemSection
        case .processor: processorSection
        case .graphics: graphicsSection
        case .sound:
            Section("Salida de sonido") {
                choice("Dispositivo", $draft.audioBackend, [("coreaudio", "Salida del Mac (CoreAudio)"), ("none", "Silencio")])
                LabeledContent("Volumen") {
                    Slider(value: Binding(get: { Double(draft.volume) }, set: { draft.volume = Int($0) }), in: 0...100, step: 1)
                    Text("\(draft.volume)%").monospacedDigit().frame(width: 46)
                }
            }
        case .shortcuts:
            Section("Atajos de teclado") {
                shortcut("Captura de pantalla", "screenshot")
                shortcut("Detener app", "stopApp")
                shortcut("Alternar Metal HUD", "toggleMetalHud")
                Text("Haz clic y pulsa una tecla. Escape elimina la asignación. Capturas en Imágenes/SteamARM.").font(.caption)
                if !trusted {
                    Text("Activa Accesibilidad para usar atajos mientras otra app está en primer plano.")
                    Button("Abrir ajustes de Accesibilidad") {
                        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") {
                            NSWorkspace.shared.open(url)
                        }
                    }
                }
            }
        case .logs:
            Section("Registros") {
                Toggle("Registro de Proton", isOn: $draft.protonLog)
                TextField("WINEDEBUG", text: $draft.wineDebug)
                choice("Nivel DXVK", $draft.dxvkLogLevel, ["none", "error", "warn", "info", "debug"])
                choice("Nivel VKD3D", $draft.vkd3dLogLevel, ["none", "err", "warn", "info"])
                Button("Abrir carpeta de registros") { Paths.ensureDirs(); NSWorkspace.shared.open(Paths.logs) }
            }
        case .debug:
            Section("Diagnóstico") {
                Toggle("Informar fallos del invitado", isOn: $draft.guestFaults)
                TextField("Procesos a trazar", text: $draft.traceMatch)
                    .help("Puede generar registros muy grandes")
                Toggle("Depuración de Vulkan", isOn: $draft.vulkanDebug)
            }
            Section("Variables de entorno extra") {
                EnvEditor(env: $draft.extraEnv)
                Text("Se exportan a todas las apps que lance SteamARM.").font(.caption).foregroundStyle(.secondary)
            }
        case .input: EmptyView()
        }
    }

    private var interfaceSection: some View {
        Group {
            Section("Inicio") {
                Toggle("Iniciar Steam al abrir el launcher", isOn: $draft.launchSteamOnStart)
                Toggle("Confirmar antes de detener una app", isOn: $draft.confirmStop)
            }
            Section("Pantalla") {
                Picker("Modo de pantalla", selection: $draft.display) {
                    Text("Ventanas nativas (recomendado)").tag(DisplayMode.native)
                    Text("VNC (Compartir Pantalla)").tag(DisplayMode.vnc)
                }
                Text(draft.display == .vnc
                     ? "Las apps se dibujan en Xvnc y se ven en Compartir Pantalla."
                     : "Cada ventana de la app es una ventana normal de macOS (servidor X nativo).")
                    .font(.caption).foregroundStyle(.secondary)
                choice("Resolución de la pantalla X", $draft.resolution, LauncherSettings.resolutions)
                    .disabled(draft.display != .vnc)
                Text("Solo VNC. Se aplica cuando Xvnc se reinicia sin apps en marcha; las ventanas nativas tienen su propio tamaño.")
                    .font(.caption).foregroundStyle(.secondary)
                if draft.display == .vnc {
                    LabeledContent("Dirección", value: "vnc://127.0.0.1:5901")
                    LabeledContent("Contraseña", value: model.vncPassword)
                }
            }
            Section("Directorio del proyecto") {
                TextField("Ruta", text: Binding(get: { draft.projectDir ?? "" }, set: { draft.projectDir = $0.isEmpty ? nil : $0 }),
                          prompt: Text("Directorio incluido con SteamARM"))
                HStack {
                    Button("Elegir…") {
                        let panel = NSOpenPanel()
                        panel.canChooseDirectories = true
                        panel.canChooseFiles = false
                        if panel.runModal() == .OK { draft.projectDir = panel.url?.path }
                    }
                    Button("Por defecto") { draft.projectDir = nil }
                }
            }
        }
    }

    private var systemSection: some View {
        Group {
            Section("Núcleo") {
                choice("Idioma del invitado", $draft.guestLanguage, [("auto", "Automático"), ("es_ES.UTF-8", "Español"),
                    ("en_US.UTF-8", "English"), ("pt_BR.UTF-8", "Português"), ("fr_FR.UTF-8", "Français"),
                    ("de_DE.UTF-8", "Deutsch"), ("it_IT.UTF-8", "Italiano"), ("ja_JP.UTF-8", "日本語")])
                LabeledContent("Zona horaria") {
                    TextField("auto o zona IANA", text: $draft.timezone)
                    Menu("Elegir") {
                        Button("Automático") { draft.timezone = "auto" }
                        ForEach(TimeZone.knownTimeZoneIdentifiers, id: \.self) { zone in
                            Button(zone) { draft.timezone = zone }
                        }
                        Button("UTC") { draft.timezone = "UTC" }
                    }.frame(width: 90)
                }
                choice("Sincronización vertical", $draft.vsync, [("game", "Según el juego"), ("on", "Activada"), ("off", "Desactivada")])
            }
            Section("Memoria") { memory("DRAM", $draft.dramGB); memory("VRAM", $draft.vramGB) }
            Section("Hacks (pueden causar inestabilidad)") {
                Toggle("Esync", isOn: $draft.esync)
                Toggle("Fsync", isOn: $draft.fsync)
            }
        }
    }

    private var processorSection: some View {
        Group {
            Section("Caché de CPU") { Toggle("Caché de traducción en disco", isOn: $draft.fexDiskCache) }
            Section("Emulación x86 (FEX)") {
                choice("Orden de memoria (TSO)", $draft.fexTSO, [("full", "Completo"), ("fast", "Rápido"), ("off", "Desactivado")])
                    .help("rápido, puede romper juegos multihilo")
                Toggle("Multibloque", isOn: $draft.fexMultiblock)
                choice("Código automodificable (SMC)", $draft.fexSMC, ["none", "mtrack", "full"])
                Toggle("Precisión reducida x87", isOn: $draft.fexX87Reduced)
            }
        }
    }

    private var graphicsSection: some View {
        Group {
            Section("API de gráficos") {
                Picker("Motor", selection: $draft.graphicsBackend) {
                    Text("Vulkan (MoltenVK / Metal)").tag("vulkan")
                    Text("OpenGL (WineD3D)").tag("opengl").disabled(true)
                        .help("no disponible: sin OpenGL en este sistema")
                }
                Text("OpenGL: no disponible: sin OpenGL en este sistema").font(.caption).foregroundStyle(.secondary)
            }
            Section("Funcionalidades y mejoras") {
                Toggle("Caché de sombreadores", isOn: $draft.shaderCache)
                integerChoice("Filtrado anisotrópico", $draft.anisotropy, [0, 2, 4, 8, 16], zero: "Automático")
                integerChoice("Límite de FPS", $draft.frameRateLimit, [0, 30, 60, 90, 120, 144], zero: "Sin límite")
                choice("HUD de DXVK", $draft.dxvkHud, [("off", "Desactivado"), ("fps", "FPS"), ("full", "Completo")])
                Toggle("Mostrar Metal HUD", isOn: $draft.metalHud)
                unavailable("Escala de resolución / FSR", "Proton 11 ya no incluye el escalado FSR de pantalla completa")
                unavailable("Suavizado de bordes", "No hay un ajuste global compatible; configúralo dentro de cada juego")
            }
        }
    }

    private func choice(_ title: String, _ value: Binding<String>, _ values: [String]) -> some View {
        choice(title, value, values.map { ($0, $0) })
    }
    private func choice(_ title: String, _ value: Binding<String>, _ values: [(String, String)]) -> some View {
        Picker(title, selection: value) {
            ForEach(values, id: \.0) { Text($0.1).tag($0.0) }
            if !values.contains(where: { $0.0 == value.wrappedValue }) { Text(value.wrappedValue).tag(value.wrappedValue) }
        }
    }
    private func integerChoice(_ title: String, _ value: Binding<Int>, _ values: [Int], zero: String) -> some View {
        Picker(title, selection: value) {
            ForEach(values, id: \.self) { Text($0 == 0 ? zero : "\($0)").tag($0) }
            if !values.contains(value.wrappedValue) { Text("\(value.wrappedValue) (guardado)").tag(value.wrappedValue) }
        }
    }
    private func memory(_ title: String, _ value: Binding<Int>) -> some View {
        integerChoice(title + " (GiB)", value, [0] + MemoryChoices.values, zero: "Automático (\(MemoryChoices.maximum) GiB)")
    }
    private func unavailable(_ title: String, _ reason: String) -> some View {
        LabeledContent(title) { Text("No disponible") }.disabled(true).help(reason)
    }
    private func shortcut(_ title: String, _ key: String) -> some View {
        LabeledContent(title) {
            HotkeyRecorder(value: Binding(get: { draft.hotkeys[key] ?? "" }, set: { draft.hotkeys[key] = $0 }))
                .frame(width: 200, height: 28)
        }
    }
}

struct EnvEditor: View {
    @Binding var env: [String: String]
    @State private var text = ""
    private var formatted: String { env.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }.joined(separator: "\n") }
    var body: some View {
        TextEditor(text: $text).font(.system(.body, design: .monospaced)).frame(height: 120)
            .onAppear { text = formatted }
            .onChange(of: env) { _, _ in
                if parse(text) != env { text = formatted }
            }
            .onChange(of: text) { _, value in
                let parsed = parse(value)
                if parsed != env { env = parsed }
            }
    }
    private func parse(_ value: String) -> [String: String] {
        var result: [String: String] = [:]
        for line in value.split(separator: "\n") {
            let pair = line.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            let key = pair[0].trimmingCharacters(in: .whitespaces)
            if pair.count == 2 && !key.isEmpty { result[key] = String(pair[1]) }
        }
        return result
    }
}
