import AppKit
import SwiftUI

struct ControllersView: View {
    @EnvironmentObject var pads: ControllerManager
    @State private var message: String?
    @State private var profile = ""
    @State private var profileName = ""
    @State private var profiles: [String] = []
    private var type: ControllerType { ControllerType(rawValue: pads.current.controllerType) ?? .xboxseries }
    private var profileDirectory: URL { Paths.launcherDir.appendingPathComponent("controller-profiles") }

    // ImageRenderer cannot rasterize the AppKit-backed scroll container.
    var rendersOffscreen = false

    var body: some View {
        Group {
            if rendersOffscreen { page }
            else { ScrollView { page } }
        }
        .onAppear { if !rendersOffscreen { pads.refresh(); refreshProfiles() } }
        .onChange(of: pads.player) { _, _ in pads.capturing = nil }
        .onChange(of: pads.current.deviceGUID) { _, _ in pads.capturing = nil }
        .onChange(of: pads.current.controllerType) { _, _ in pads.capturing = nil }
    }

    private var page: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Picker("Jugador", selection: $pads.player) {
                    ForEach(0..<4, id: \.self) { Text("\($0 + 1)").tag($0) }
                }.frame(width: 120)
                Picker("Perfil", selection: $profile) {
                    Text("Sin perfil").tag("")
                    ForEach(profiles, id: \.self) { Text($0).tag($0) }
                }
                Button("Cargar") { loadProfile() }.disabled(profile.isEmpty)
                Button("Eliminar") { deleteProfile() }.disabled(profile.isEmpty)
                TextField("Nombre del perfil", text: $profileName).frame(width: 140)
                Button("Guardar") { saveProfile() }.disabled(profileName.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            HStack {
                Picker("Dispositivo", selection: deviceBinding) {
                    Text("Desactivado").tag("")
                    ForEach(uniqueDevices) { device in Text(device.name).tag(device.guid) }
                    if let guid = pads.current.deviceGUID, !pads.devices.contains(where: { $0.guid == guid }) {
                        Text("\(pads.current.deviceName ?? "Mando") (desconectado)").tag(guid)
                    }
                }
                Button { pads.refresh() } label: { Image(systemName: "arrow.clockwise") }.help("Actualizar dispositivos")
                Picker("Tipo de mando", selection: $pads.current.controllerType) {
                    ForEach(ControllerType.allCases) { Text($0.name).tag($0.rawValue) }
                }
            }
            HStack {
                Button("Importar de Ryujinx") { message = pads.importRyujinx() }
                Button("Restablecer asignación") { pads.current.mapping = PlayerConfig.defaultMapping }
                Spacer()
                if pads.capturing != nil {
                    Text("Pulsa un botón o mueve el stick…").foregroundStyle(.yellow)
                    Button("Cancelar asignación") { pads.capturing = nil }
                }
            }
            if let error = pads.sdlError { Text("SDL2: \(error)").foregroundStyle(.red) }
            if let message { Text(message).font(.caption).foregroundStyle(.secondary) }
            HStack(alignment: .top, spacing: 14) {
                VStack(spacing: 12) {
                    mappingGroup("Gatillos y botones superiores", [.zl, .l])
                    stickPanel(left: true)
                    mappingGroup("Cruceta", [.dpadUp, .dpadDown, .dpadLeft, .dpadRight])
                }.frame(width: 220)
                VStack(spacing: 16) {
                    ControllerDrawing(type: type)
                    HStack(spacing: 40) {
                        VStack {
                            StickView(point: pads.stick(.leftStick), deadzone: pads.current.deadzoneLeft,
                                      pressed: pads.isActive(.l3))
                            Text(type.label(.leftStick)).font(.caption)
                        }
                        VStack {
                            StickView(point: pads.stick(.rightStick), deadzone: pads.current.deadzoneRight,
                                      pressed: pads.isActive(.r3))
                            Text(type.label(.rightStick)).font(.caption)
                        }
                    }
                    mappingGroup("Centro", [.minus, .plus])
                    if type.trackpads { mappingGroup("Paneles táctiles", [.leftPad, .rightPad]) }
                    if type.paddles { mappingGroup("Palancas traseras", type == .steamcontroller2
                        ? [.leftGrip, .rightGrip, .leftGrip2, .rightGrip2] : [.leftGrip, .rightGrip]) }
                    slider("Umbral de gatillo", $pads.current.triggerThreshold, 0...1)
                    HStack(spacing: 40) {
                        TriggerBar(label: type.label(.zl), value: pads.live[pads.current.mapping["zl"] ?? ""] ?? 0,
                                   threshold: pads.current.triggerThreshold)
                        TriggerBar(label: type.label(.zr), value: pads.live[pads.current.mapping["zr"] ?? ""] ?? 0,
                                   threshold: pads.current.triggerThreshold)
                    }
                }.frame(maxWidth: .infinity)
                VStack(spacing: 12) {
                    mappingGroup("Gatillos y botones superiores", [.zr, .r])
                    mappingGroup("Botones", [.a, .b, .x, .y])
                    stickPanel(left: false)
                    GroupBox("Vibración") {
                        VStack {
                            Toggle("Activada", isOn: $pads.current.rumble)
                            slider("Intensidad", $pads.current.rumbleStrength, 0...1)
                            Button("Probar vibración") { pads.testRumble() }
                                .disabled(!pads.current.rumble || !pads.currentHasRumble)
                        }.padding(4)
                    }
                    if type.hasLED {
                        GroupBox("LED") {
                            Toggle("Color personalizado", isOn: Binding(get: { pads.current.ledColor != nil },
                                set: { pads.current.ledColor = $0 ? "#0080FF" : nil }))
                            ColorPicker("Color", selection: ledBinding, supportsOpacity: false)
                                .disabled(pads.current.ledColor == nil)
                        }
                    }
                    if type.hasMotion { Toggle("Movimiento", isOn: $pads.current.motion) }
                }.frame(width: 220)
            }
        }.padding(16)
    }

    private var uniqueDevices: [PadDevice] {
        var seen = Set<String>()
        return pads.devices.filter { seen.insert($0.guid).inserted }
    }
    private var deviceBinding: Binding<String> {
        Binding(get: { pads.current.deviceGUID ?? "" }, set: { guid in
            pads.current.deviceGUID = guid.isEmpty ? nil : guid
            pads.current.deviceName = pads.devices.first { $0.guid == guid }?.name
        })
    }
    private var ledBinding: Binding<Color> {
        Binding(get: {
            let hex = UInt32((pads.current.ledColor ?? "#0080FF").dropFirst(), radix: 16) ?? 0x0080FF
            return Color(red: Double((hex >> 16) & 255) / 255, green: Double((hex >> 8) & 255) / 255, blue: Double(hex & 255) / 255)
        }, set: { value in
            guard let rgb = NSColor(value).usingColorSpace(.deviceRGB) else { return }
            pads.current.ledColor = String(format: "#%02X%02X%02X", Int((rgb.redComponent * 255).rounded()),
                                          Int((rgb.greenComponent * 255).rounded()), Int((rgb.blueComponent * 255).rounded()))
        })
    }
    private func mappingGroup(_ title: String, _ slots: [PadSlot]) -> some View {
        GroupBox(title) {
            VStack(spacing: 5) { ForEach(slots) { slot in slotButton(slot) } }.padding(4)
        }
    }
    private func slotButton(_ slot: PadSlot) -> some View {
        Button { pads.capturing = pads.capturing == slot ? nil : slot } label: {
            HStack {
                Text(type.label(slot))
                Spacer(minLength: 4)
                Text(pads.capturing == slot ? "…" : pads.current.mapping[slot.rawValue] ?? "Sin asignar")
                    .font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
            }.padding(5)
                .background(pads.isActive(slot) ? Theme.accent.opacity(0.5) : Theme.card, in: RoundedRectangle(cornerRadius: 4))
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(pads.capturing == slot ? .yellow : .clear))
        }.buttonStyle(.plain)
    }
    private func stickPanel(left: Bool) -> some View {
        GroupBox(left ? "Stick izquierdo" : "Stick derecho") {
            VStack(alignment: .leading, spacing: 5) {
                slotButton(left ? .l3 : .r3)
                slotButton(left ? .leftStick : .rightStick)
                Toggle("Invertir X", isOn: left ? $pads.current.invertLX : $pads.current.invertRX)
                Toggle("Invertir Y", isOn: left ? $pads.current.invertLY : $pads.current.invertRY)
                Toggle("Rotar 90°", isOn: left ? $pads.current.rotateL : $pads.current.rotateR)
                slider("Zona muerta", left ? $pads.current.deadzoneLeft : $pads.current.deadzoneRight, 0...1)
                slider("Rango", left ? $pads.current.rangeLeft : $pads.current.rangeRight, 0...2)
            }.padding(4)
        }
    }
    private func slider(_ title: String, _ value: Binding<Double>, _ range: ClosedRange<Double>) -> some View {
        VStack(spacing: 0) {
            HStack { Text(title); Spacer(); Text(String(format: "%.2f", value.wrappedValue)).monospacedDigit() }.font(.caption)
            Slider(value: value, in: range)
        }
    }
    private func refreshProfiles() {
        profiles = ((try? FileManager.default.contentsOfDirectory(at: profileDirectory, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "json" }.map { $0.deletingPathExtension().lastPathComponent }.sorted()
        if !profiles.contains(profile) { profile = "" }
    }
    private func saveProfile() {
        let name = profileName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, name != ".", name != "..", !name.contains("/"), !name.contains(":"), !name.contains("\0") else {
            message = "Nombre de perfil no válido."; return
        }
        do {
            try FileManager.default.createDirectory(at: profileDirectory, withIntermediateDirectories: true)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            try encoder.encode(pads.current).write(to: profileDirectory.appendingPathComponent(name).appendingPathExtension("json"), options: .atomic)
            refreshProfiles(); profile = name; message = "Perfil guardado."
        } catch { message = error.localizedDescription }
    }
    private func loadProfile() {
        guard profiles.contains(profile), let config = Store.load(PlayerConfig.self,
            from: profileDirectory.appendingPathComponent(profile).appendingPathExtension("json")) else {
            message = "No se pudo leer el perfil."; return
        }
        pads.capturing = nil
        pads.current = config
        profileName = profile
        message = "Perfil cargado; pulsa Aplicar para usarlo."
    }
    private func deleteProfile() {
        guard profiles.contains(profile) else { return }
        do {
            try FileManager.default.removeItem(at: profileDirectory.appendingPathComponent(profile).appendingPathExtension("json"))
            refreshProfiles(); message = "Perfil eliminado."
        } catch { message = error.localizedDescription }
    }
}

struct StickView: View {
    let point: CGPoint
    let deadzone: Double
    let pressed: Bool

    var body: some View {
        ZStack {
            Circle().stroke(Color.gray.opacity(0.5), lineWidth: 2)
            Circle().fill(Color.gray.opacity(0.15)).frame(width: 90 * deadzone, height: 90 * deadzone)
            Circle().fill(pressed ? Color.yellow : Theme.accent)
                .frame(width: 14, height: 14)
                .offset(x: point.x * 38, y: point.y * 38)
        }
        .frame(width: 90, height: 90)
    }
}

struct TriggerBar: View {
    let label: String
    let value: Double
    let threshold: Double

    var body: some View {
        VStack(spacing: 4) {
            GeometryReader { g in
                ZStack(alignment: .bottom) {
                    RoundedRectangle(cornerRadius: 4).fill(Color.gray.opacity(0.2))
                    RoundedRectangle(cornerRadius: 4)
                        .fill(value > threshold ? Theme.accent : Color.gray)
                        .frame(height: g.size.height * max(0, min(1, value)))
                    Rectangle().fill(Color.yellow).frame(height: 1)
                        .offset(y: -g.size.height * threshold)
                }
            }
            .frame(width: 22, height: 60)
            Text(label).font(.caption2)
        }
    }
}
