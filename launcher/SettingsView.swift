import AppKit
import SwiftUI
import ApplicationServices

enum SettingsSection: String, CaseIterable, Identifiable {
    case interface = "Interfaz", input = "Entrada", system = "Sistema", processor = "Procesador"
    case graphics = "Gráficos", runtime = "Runtime", sound = "Sonido", shortcuts = "Atajos", logs = "Registros", debug = "Depuración"
    case about = "Acerca de"
    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .interface: return "macwindow"
        case .input: return "gamecontroller"
        case .system: return "gearshape"
        case .processor: return "cpu"
        case .graphics: return "display"
        case .runtime: return "checklist"
        case .sound: return "speaker.wave.2"
        case .shortcuts: return "keyboard"
        case .logs: return "doc.text"
        case .debug: return "ladybug"
        case .about: return "info.circle"
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
    @State private var compatibility: CompatibilityStatus?
    @State private var compatibilityLoaded = false
    @State private var displayRefresh: [ScreenRefresh] = []

    private struct ScreenRefresh: Identifiable {
        let id: Int
        let name: String
        let range: DisplayRefreshRange?
    }

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
        .task {
            refreshDisplays()
            model.refreshCapabilities()
            compatibility = await CompatibilityStatus.load(project: model.projectDir)
            compatibilityLoaded = true
        }
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
            refreshDisplays()
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didChangeScreenParametersNotification)) { _ in
            refreshDisplays()
        }
    }

    private func refreshDisplays() {
        displayRefresh = NSScreen.screens.enumerated().map { index, screen in
            ScreenRefresh(id: index, name: screen.localizedName,
                range: DisplayRefreshRange(minimumInterval: screen.minimumRefreshInterval,
                                           maximumInterval: screen.maximumRefreshInterval))
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
        case .runtime: runtimeSection
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
        case .about: aboutSection
        case .input: EmptyView()
        }
    }

    private var aboutSection: some View {
        Section("SteamARM") {
            LabeledContent("Versión", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "Desconocida")
        }
    }

    private var interfaceSection: some View {
        Group {
            Section("Inicio") {
                LabeledContent("Steam principal", value: "SteamARM · Steam Frame")
                Text("Cliente ARM64 de Steam Frame.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Iniciar SteamARM al abrir el launcher", isOn: $draft.launchSteamOnStart)
                Toggle("Confirmar antes de detener una app", isOn: $draft.confirmStop)
            }
            Section("Pantalla") {
                Picker("Backend de la aplicación", selection: Binding(
                    get: { draft.backendPreset },
                    set: { preset in
                        guard model.capabilities.status(of: preset).usable else { return }
                        switch preset {
                        case .nativeWindows: draft.display = .native; draft.execution = ExecutionBackend.auto.rawValue
                        case .vncScreenSharing: draft.display = .vnc; draft.execution = ExecutionBackend.auto.rawValue
                        case .lightningJIT: draft.display = .native; draft.execution = ExecutionBackend.lightningJIT.rawValue
                        case .appleHypervisor: break
                        }
                    })) {
                    ForEach(ApplicationBackendPreset.allCases.filter { $0.offered(in: model.capabilities) }, id: \.self) { preset in
                        let status = model.capabilities.status(of: preset)
                        Text(preset.label + (status.state == .ready ? "" : " · " + status.state.label))
                            .tag(preset).disabled(!status.usable).help(status.reason)
                    }
                }
                Text(draft.display == .vnc
                     ? "Las apps se dibujan en Xvnc y se ven en Compartir Pantalla."
                     : draft.backendPreset == .lightningJIT
                     ? "Ventanas nativas, y el x86 con el JIT de FEX ajustado para velocidad: sustituye los ajustes de Procesador (FEX). Experimental."
                     : "Cada ventana de la app es una ventana normal de macOS (servidor X nativo).")
                    .font(.caption).foregroundStyle(.secondary)
                ForEach(ApplicationBackendPreset.allCases.filter { $0.offered(in: model.capabilities) && !model.capabilities.status(of: $0).usable }, id: \.self) { preset in
                    let status = model.capabilities.status(of: preset)
                    Text("\(preset.label): \(status.state.label.lowercased()) — \(status.reason).")
                        .font(.caption).foregroundStyle(.secondary)
                }
                let selected = draft.backendPreset
                Text("Modo de sesión: \(SessionVirtualizationMode(selected.execution).label)")
                    .font(.caption).foregroundStyle(.secondary)
                    .help("Ningún backend que se pueda elegir usa una máquina virtual.")
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
                choice("Sincronización vertical", $draft.vsync, [("game", "AUTO (según el juego)"), ("on", "ON (activada)"), ("off", "OFF (desactivada)")])
                Text("ON/OFF fijan DXVK (dxgi.syncInterval, d3d9.presentInterval) y VKD3D_SWAPCHAIN_PRESENT_MODE (FIFO / IMMEDIATE); Proton 10.0 no lee la variable de VKD3D. MoltenVK solo ofrece FIFO e IMMEDIATE, y el efecto en pantalla no se ha medido.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Memoria") {
                memory("Tope de memoria de las apps", $draft.dramGB)
                Text("No da memoria a los juegos (ven toda la del Mac): es el límite a partir del cual el vigilante de SteamARM cierra todas las apps para que el Mac no se cuelgue.")
                    .font(.caption).foregroundStyle(.secondary)
                memory("VRAM que ven los juegos", $draft.vramGB)
                Text("La memoria de vídeo que el juego cree tener. Automático: la que da Metal en este Mac. Bájala solo si un juego reserva de más.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Sincronización (Proton)") {
                Picker("Sincronización", selection: Binding(get: { draft.synchronization }, set: { value in
                    guard let backend = SynchronizationBackend(rawValue: value),
                          backend == .auto || model.capabilities.synchronization[backend]?.usable == true else { return }
                    draft.synchronization = value
                })) {
                    ForEach(SynchronizationBackend.allCases, id: \.self) { backend in
                        let status = model.capabilities.synchronization[backend]
                        Text(backend.label + (backend == .auto || status?.state == .ready ? "" : " · \(status?.state.label ?? "No disponible")"))
                            .tag(backend.rawValue)
                            .disabled(backend != .auto && status?.usable != true)
                            .help(status?.reason ?? "Sin datos")
                    }
                }
                Text("AUTO → \(model.capabilities.effectiveSynchronization(.auto).label)")
                    .font(.caption).foregroundStyle(.secondary)
                if let selected = SynchronizationBackend(rawValue: draft.synchronization), selected != .auto,
                   let status = model.capabilities.synchronization[selected] {
                    Text("\(status.state.label): \(status.reason).")
                        .font(.caption).foregroundStyle(.secondary)
                }
                ForEach(SynchronizationBackend.allCases.filter {
                    $0 != .auto && model.capabilities.synchronization[$0]?.usable != true
                }, id: \.self) { backend in
                    Text("\(backend.label): \(model.capabilities.synchronization[backend]?.reason ?? "Sin datos").")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text("Se aplica al iniciar Steam o la app.").font(.caption).foregroundStyle(.secondary)
            }
            Section("Alternativas") {
                Picker("Si un ajuste no puede funcionar", selection: $draft.fallbackPolicy) {
                    ForEach(FallbackPolicy.allCases, id: \.self) { policy in
                        Text(policy.label).tag(policy.rawValue)
                    }
                }
                Text("Solo actúa cuando algo pedido no puede funcionar para la app que abres: VNC con una app fuera de la raíz x86, o un valor guardado que ya no está disponible (por ejemplo KosmicKrisp sin un shim que lo cargue). AUTO abre con la alternativa, ESTRICTO no abre la app y PREGUNTAR te deja elegir. Nunca se recurre a una máquina virtual.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private var processorSection: some View {
        Group {
            Section("Compatibilidad instalada") {
                if let compatibility {
                    LabeledContent("FEX adaptado a macOS", value: compatibility.fex.patchedInstalled ? "Instalado" : "Falta instalar")
                    LabeledContent("FEX de Steam", value: compatibility.fex.steamInstalled ? "Instalado · requiere Linux" : "No instalado")
                    ForEach(compatibility.protons) { proton in
                        LabeledContent(proton.name, value: proton.status)
                    }
                    Text(compatibility.nativeArmReason).font(.caption).foregroundStyle(.secondary)
                    Text("Para programas Linux x86_64: Añadir app → Personalizada. Para juegos Windows: seleccionar Proton x86_64 en Steam → Propiedades → Compatibilidad.")
                        .font(.caption).foregroundStyle(.secondary)
                    Text(compatibility.note).font(.caption).foregroundStyle(.secondary)
                } else {
                    Text(compatibilityLoaded ? "No se pudo leer el diagnóstico de compatibilidad." : "Consultando componentes…")
                }
            }
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
                if let driver = compatibility?.moltenvk {
                    LabeledContent("MoltenVK instalado", value: driver.version)
                        .help(driver.path)
                    Text("Steam puede mostrar 0.2.2210 para MoltenVK 1.4.2: interpreta su versión decimal como una versión Vulkan.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                if let kk = model.compatibility?.runtime.kosmickrisp, !kk.library.isEmpty {
                    LabeledContent("KosmicKrisp instalado", value: kk.version).help(kk.library)
                }
                Picker("Motor", selection: Binding(get: { draft.graphicsBackend }, set: { value in
                    guard let backend = GraphicsBackend(rawValue: value),
                          backend == .auto || model.capabilities.graphics[backend]?.usable == true else { return }
                    draft.graphicsBackend = value
                })) {
                    ForEach(graphicsOrder, id: \.self) { backend in
                        let status = model.capabilities.graphics[backend]
                        Text(backend.label + (backend == .auto || status?.state == .ready ? "" : " · \(status?.state.label ?? "No disponible")"))
                            .tag(backend.rawValue)
                            .disabled(backend != .auto && status?.usable != true)
                            .help(status?.reason ?? "Sin datos")
                    }
                }
                Text("AUTO → \(model.capabilities.effectiveGraphics(.auto).label)")
                    .font(.caption).foregroundStyle(.secondary)
                ForEach(graphicsOrder.filter {
                    $0 != .auto && model.capabilities.graphics[$0]?.usable != true
                }, id: \.self) { backend in
                    Text("\(backend.label): \(model.capabilities.graphics[backend]?.reason ?? "Sin datos").")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Funcionalidades y mejoras") {
                Toggle("Caché de sombreadores", isOn: $draft.shaderCache)
                integerChoice("Filtrado anisotrópico", $draft.anisotropy, [0, 2, 4, 8, 16], zero: "Automático")
                Text("Un mínimo para los juegos (Direct3D por DXVK y Vulkan nativo como Counter-Strike 2): si el juego pide más, se queda con lo suyo. Automático: lo que elija el juego. Se aplica al reiniciar Steam.")
                    .font(.caption).foregroundStyle(.secondary)
                integerChoice("Límite de FPS", $draft.frameRateLimit, [0, 30, 60, 90, 100, 120, 144, 165], zero: "Sin límite")
                Text("Para los juegos (Direct3D por DXVK/VKD3D y Vulkan nativo), no para la interfaz de Steam. Se aplica al reiniciar Steam.")
                    .font(.caption).foregroundStyle(.secondary)
                Text("100 FPS equivalen a 10 ms por fotograma; menos de 10 ms requiere más de 100 FPS. El límite no aumenta el rendimiento del juego.")
                    .font(.caption).foregroundStyle(.secondary)
                choice("HUD de DXVK", $draft.dxvkHud, [("off", "Desactivado"), ("fps", "FPS"), ("full", "Completo")])
                Text("Solo juegos Direct3D 8 a 11 (DXVK).")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Mostrar Metal HUD", isOn: $draft.metalHud)
                Text("El HUD de rendimiento de Apple, arriba a la derecha de los juegos (no de la interfaz de Steam). Se aplica al reiniciar Steam.")
                    .font(.caption).foregroundStyle(.secondary)
                integerChoice("Suavizado de bordes (MSAA)", $draft.antialiasing, [0, 2, 4, 8], zero: "El del juego")
                Text("Lo fuerza DXVK en los juegos Direct3D 9. Los Direct3D 10, 11 y 12 lo eligen dentro del juego: no hay un ajuste global para ellos.")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Escala de resolución (experimental)", isOn: $draft.resolutionScaling)
                Text("Los juegos ofrecen resoluciones menores que la de la pantalla; al elegir una dentro del juego, se dibuja a ese tamaño (más rápido) y se agranda a toda la pantalla con el filtro de abajo, conservando su proporción. Se aplica a los prefijos de los juegos al abrir Steam o una app, con ningún juego abierto; un juego nuevo, desde su segundo arranque.")
                    .font(.caption).foregroundStyle(.secondary)
                choice("Escala de render", $draft.renderScale, RenderScaleChoice.options)
                Text(RenderScaleChoice.note(draft.renderScale))
                    .font(.caption).foregroundStyle(.secondary)
                choice("Filtro de escalado", $draft.scalingFilter, ScalingFilterChoice.options)
                if draft.scalingFilter == "fsr" || draft.scalingFilter == "auto" {
                    LabeledContent("Nitidez FSR") {
                        Slider(value: Binding(get: { Double(draft.fsrSharpness) }, set: { draft.fsrSharpness = Int($0) }),
                               in: 0...100, step: 5)
                        Text("\(draft.fsrSharpness)%").monospacedDigit().frame(width: 46)
                    }
                }
                Text(ScalingFilterChoice.note(draft.scalingFilter))
                    .font(.caption).foregroundStyle(.secondary)
                if draft.scalingFilter == "metalfx-temporal" && draft.renderScale == "1.0" {
                    Toggle("MetalFX temporal también a resolución nativa", isOn: $draft.metalfxNativeAA)
                    Text("Pasa cada fotograma por MetalFX temporal sin escalarlo. Sin los vectores de movimiento del juego suaviza la imagen (el texto se ve más borroso) y cuesta fotogramas; desactivado, la imagen queda tal cual.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                choice("Escalado del juego", $draft.gameUpscaler, GameUpscalerChoice.options)
                Text(GameUpscalerChoice.note(draft.gameUpscaler, backend: gameGraphics))
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("Interfaz de Steam ARM64 acelerada por GPU", isOn: $draft.steamUIAcceleration)
                Text("La interfaz de Steam ARM64 y Steam Frame (Chromium) dibuja con Vulkan sobre Metal (el driver elegido arriba: MoltenVK o KosmicKrisp) en lugar de con la CPU. Sin ella, su proceso GPU no arranca en macOS y Steam pinta por software. Se aplica al abrir Steam; si su ventana se viera mal, desactívala.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Pantalla y Adaptive Sync") {
                Toggle("Solicitar Adaptive Sync (FreeSync / G-SYNC compatibles)", isOn: $draft.adaptiveSync)
                ForEach(displayRefresh) { screen in
                    LabeledContent(screen.name) {
                        if let range = screen.range {
                            if range.variable {
                                Text("Rango variable informado: \(range.minimumFPS, specifier: "%.0f")–\(range.maximumFPS, specifier: "%.0f") Hz")
                            } else {
                                Text("Refresco fijo: \(range.maximumFPS, specifier: "%.0f") Hz")
                            }
                        } else {
                            Text("Rango de refresco no disponible")
                        }
                    }
                }
                if displayRefresh.isEmpty {
                    Text("No se pudo consultar ninguna pantalla.").font(.caption).foregroundStyle(.secondary)
                } else if !displayRefresh.contains(where: { $0.range?.variable == true }) {
                    Text("macOS no informa un rango variable en esta conexión. SteamARM conserva la presentación del juego hasta detectar una pantalla compatible.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text("Requiere una pantalla que macOS ofrezca como Variable o Adaptive, Adaptive Sync activado en el monitor y pantalla completa nativa de macOS. Una ventana que cubre la pantalla puede conservar la presentación normal. FreeSync y G-SYNC compatibles usan ese soporte de macOS. La opción solicita su uso; no confirma que el monitor esté sincronizando cada fotograma ni aumenta los FPS. Se aplica al reiniciar Steam.")
                    .font(.caption).foregroundStyle(.secondary)
                Text("Apple recomienda conectar por USB-C o Thunderbolt con DisplayPort. Si Variable o Adaptive no aparece en Ajustes del Sistema → Pantallas, revisa el monitor y la conexión.")
                    .font(.caption).foregroundStyle(.secondary)
                Link("Guía de Adaptive Sync de Apple", destination: URL(string: "https://support.apple.com/102144")!)
            }
        }
    }

    private var graphicsOrder: [GraphicsBackend] {
        [.auto, .vulkanMoltenVK, .vulkanKosmicKrisp, .openGLWineD3D]
    }

    /// The backend games get with the "Motor" chosen, as scripts/settings-env.py
    /// decides it: AUTO is MoltenVK there, and a backend that cannot run falls
    /// back (only KosmicKrisp has the MetalFX paths of "Escalado del juego").
    private var gameGraphics: GraphicsBackend {
        draft.graphics == .auto ? .vulkanMoltenVK : model.capabilities.effectiveGraphics(draft.graphics)
    }

    private var runtimeSection: some View {
        Group {
            Section("Modo de sesión") {
                LabeledContent("Sesión", value: "ZERO-VM")
                Text("SteamARM no usa ninguna máquina virtual: lxrun ejecuta aarch64 directamente y x86 con FEX.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Presentación") {
                ForEach(PresentationMode.allCases, id: \.self) { mode in
                    capabilityRow(mode.label, status: model.capabilities.presentation[mode])
                }
            }
            Section("Ejecución") {
                ForEach(ExecutionBackend.allCases.filter { $0.offered(in: model.capabilities) }, id: \.self) { backend in
                    capabilityRow(executionName(backend), status: model.capabilities.execution[backend])
                }
            }
            Section("Sincronización") {
                ForEach(SynchronizationBackend.allCases.filter { $0 != .auto }, id: \.self) { backend in
                    capabilityRow(backend.label, status: model.capabilities.synchronization[backend])
                }
            }
            Section("Gráficos") {
                ForEach(GraphicsBackend.allCases.filter { $0 != .auto }, id: \.self) { backend in
                    capabilityRow(backend.label, status: model.capabilities.graphics[backend])
                }
            }
            Section("Detectado en este Mac") {
                if let runtime = model.compatibility?.runtime {
                    LabeledContent("MoltenVK", value: runtime.moltenvk.version)
                        .help(runtime.moltenvk.path)
                    let kk = runtime.kosmickrisp
                    LabeledContent("KosmicKrisp", value: kk?.library.isEmpty == false ? kk?.version ?? "" : "No instalado")
                        .help(kk?.library ?? "")
                    LabeledContent("macOS compatible", value: yesNo(kk?.osOK == true))
                    LabeledContent("Carga como ICD", value: yesNo(kk?.exportsICD == true))
                    LabeledContent("Shim Vulkan", value: "Instalado: \(yesNo(runtime.shim?.installed == true))")
                        .help(runtime.shim?.path ?? "")
                    LabeledContent("Selección de ICD (STEAMARM_VK_ICD)", value: yesNo(runtime.shim?.icdSelection == true))
                    ForEach(model.compatibility?.protons.filter { $0.installed } ?? []) { proton in
                        LabeledContent(proton.name, value: "esync \(yesNo(proton.esync == true)) · fsync \(yesNo(proton.fsync == true)) · ntsync \(yesNo(proton.ntsync == true))")
                    }
                    LabeledContent("Servidor X nativo", value: yesNo(runtime.presentation?.nativeX == true))
                    LabeledContent("Xvnc", value: yesNo(runtime.presentation?.xvnc == true))
                    LabeledContent("Compartir Pantalla", value: yesNo(runtime.presentation?.screenSharing == true))
                } else {
                    Text("Consultando…").foregroundStyle(.secondary)
                }
                Button("Volver a comprobar") { model.refreshCapabilities() }
            }
        }
    }

    private func capabilityRow(_ name: String, status: CapabilityStatus?) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            LabeledContent(name) {
                Text(status?.state.label ?? "No disponible")
                    .foregroundStyle(status?.state == .ready ? Color.green :
                                     status?.state == .experimental ? Color.orange : Color.secondary)
            }
            Text(status?.reason ?? "Sin datos").font(.caption).foregroundStyle(.secondary)
        }
    }

    private func executionName(_ backend: ExecutionBackend) -> String {
        switch backend {
        case .auto: return "lxrun (AUTO)"
        case .lightningJIT: return "Lightning JIT"
        case .appleHypervisorLegacy: return "Apple Hypervisor"
        }
    }

    private func yesNo(_ value: Bool) -> String { value ? "sí" : "no" }

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
