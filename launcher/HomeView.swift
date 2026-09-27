import AppKit
import SwiftUI

struct HomeView: View {
    @EnvironmentObject var model: LauncherModel
    @State private var showAdd = false
    @State private var editing: AppEntry?
    @State private var deleting: AppEntry?

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
                        ForEach(model.allApps) { app in
                            AppCard(app: app) { model.launch(app) }
                                .contextMenu {
                                    if !app.isBuiltIn {
                                        Button("Editar") { editing = app }
                                        Button("Eliminar", role: .destructive) { deleting = app }
                                    }
                                }
                        }
                        AddCard { showAdd = true }
                    }
                    .padding(24)
                }
            }
        }
        .background(Theme.background)
        .sheet(isPresented: $showAdd) { AddAppView().environmentObject(model) }
        .sheet(item: $editing) { app in EditAppView(app: app).environmentObject(model) }
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
            .disabled(model.phase != .idle)
            SettingsLink {
                Label("Ajustes", systemImage: "gearshape")
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

    var body: some View {
        Button(action: action) {
            VStack(spacing: 12) {
                AppIconView(app: app, size: 96)
                    .frame(maxWidth: .infinity)
                    .padding(.top, 18)
                Text(app.name)
                    .font(.headline)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 8)
                    .padding(.bottom, 16)
            }
            .frame(height: 190)
            .background(RoundedRectangle(cornerRadius: 14).fill(Theme.card))
            .overlay(RoundedRectangle(cornerRadius: 14)
                .stroke(hover ? Theme.accent : .clear, lineWidth: 2))
            .scaleEffect(hover ? 1.03 : 1)
            .animation(.easeOut(duration: 0.12), value: hover)
        }
        .buttonStyle(.plain)
        .onHover { hover = $0 }
        .disabled(model.phase != .idle)
        .help("Abrir \(app.name)")
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
            .frame(height: 190)
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
}

/// Name, command line (one argument per line), environment and icon.
struct EditAppView: View {
    @EnvironmentObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    @State var app: AppEntry
    @State private var commandText = ""
    @State private var envText = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Editar \(app.name)").font(.title2.bold())
            Form {
                TextField("Nombre", text: $app.name)
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
        .onAppear {
            commandText = app.command.joined(separator: "\n")
            envText = app.env.sorted { $0.key < $1.key }.map { "\($0.key)=\($0.value)" }
                .joined(separator: "\n")
        }
    }

    private var commandLines: [String] {
        commandText.split(separator: "\n").map { String($0) }.filter { !$0.isEmpty }
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
                Text("Falta instalar SteamARM").font(.headline)
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
