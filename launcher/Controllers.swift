import Foundation

// MARK: - Model

/// A Switch-style input the user maps a physical control onto.
enum PadSlot: String, CaseIterable, Codable, Identifiable {
    case a, b, x, y, dpadUp, dpadDown, dpadLeft, dpadRight
    case l, r, zl, zr, minus, plus, l3, r3, leftStick, rightStick

    case leftGrip, rightGrip, leftGrip2, rightGrip2, leftPad, rightPad

    var id: String { rawValue }
    var isStick: Bool { self == .leftStick || self == .rightStick }

    func label(xbox: Bool) -> String {
        switch self {
        case .a: return "A"
        case .b: return "B"
        case .x: return "X"
        case .y: return "Y"
        case .dpadUp: return "Cruceta ↑"
        case .dpadDown: return "Cruceta ↓"
        case .dpadLeft: return "Cruceta ←"
        case .dpadRight: return "Cruceta →"
        case .l: return xbox ? "LB" : "L"
        case .r: return xbox ? "RB" : "R"
        case .zl: return xbox ? "LT" : "ZL"
        case .zr: return xbox ? "RT" : "ZR"
        case .minus: return xbox ? "Back" : "−"
        case .plus: return xbox ? "Start" : "+"
        case .l3: return xbox ? "LS" : "L3"
        case .r3: return xbox ? "RS" : "R3"
        case .leftStick: return "Stick izq."
        case .rightStick: return "Stick der."
        case .leftGrip: return "Palanca izq."
        case .rightGrip: return "Palanca der."
        case .leftGrip2: return "Palanca izq. inferior"
        case .rightGrip2: return "Palanca der. inferior"
        case .leftPad: return "Panel izq."
        case .rightPad: return "Panel der."
        }
    }
}

struct PlayerConfig: Codable, Equatable {
    var deviceGUID: String?
    var deviceName: String?
    var controllerType: String = "xboxseries"
    var mapping: [String: String] = PlayerConfig.defaultMapping
    var deadzoneLeft: Double = 0.1
    var deadzoneRight: Double = 0.1
    var triggerThreshold: Double = 0.5
    var rumble: Bool = true

    var invertLX: Bool = false
    var invertLY: Bool = false
    var rotateL: Bool = false
    var invertRX: Bool = false
    var invertRY: Bool = false
    var rotateR: Bool = false
    var rangeLeft: Double = 1
    var rangeRight: Double = 1
    var rumbleStrength: Double = 1
    var ledColor: String? = nil
    var motion: Bool = false

    init() {}
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        deviceGUID = try c.decodeIfPresent(String.self, forKey: .deviceGUID)
        deviceName = try c.decodeIfPresent(String.self, forKey: .deviceName)
        controllerType = try c.decodeIfPresent(String.self, forKey: .controllerType) ?? "xboxseries"
        mapping = try c.decodeIfPresent([String: String].self, forKey: .mapping) ?? PlayerConfig.defaultMapping
        deadzoneLeft = try c.decodeIfPresent(Double.self, forKey: .deadzoneLeft) ?? 0.1
        deadzoneRight = try c.decodeIfPresent(Double.self, forKey: .deadzoneRight) ?? 0.1
        triggerThreshold = try c.decodeIfPresent(Double.self, forKey: .triggerThreshold) ?? 0.5
        rumble = try c.decodeIfPresent(Bool.self, forKey: .rumble) ?? true
        invertLX = try c.decodeIfPresent(Bool.self, forKey: .invertLX) ?? false
        invertLY = try c.decodeIfPresent(Bool.self, forKey: .invertLY) ?? false
        rotateL = try c.decodeIfPresent(Bool.self, forKey: .rotateL) ?? false
        invertRX = try c.decodeIfPresent(Bool.self, forKey: .invertRX) ?? false
        invertRY = try c.decodeIfPresent(Bool.self, forKey: .invertRY) ?? false
        rotateR = try c.decodeIfPresent(Bool.self, forKey: .rotateR) ?? false
        rangeLeft = try c.decodeIfPresent(Double.self, forKey: .rangeLeft) ?? 1
        rangeRight = try c.decodeIfPresent(Double.self, forKey: .rangeRight) ?? 1
        rumbleStrength = try c.decodeIfPresent(Double.self, forKey: .rumbleStrength) ?? 1
        ledColor = try c.decodeIfPresent(String.self, forKey: .ledColor)
        motion = try c.decodeIfPresent(Bool.self, forKey: .motion) ?? false
        if controllerType == "ProController" { controllerType = "switchpro" }
        if controllerType == "Xbox" { controllerType = "xboxseries" }
        mapping = Self.defaultMapping.merging(mapping) { _, saved in saved }
    }

    /// Slot -> SDL game-controller control name ("left"/"right" = whole stick).
    static let defaultMapping: [String: String] = [
        "a": "a", "b": "b", "x": "x", "y": "y",
        "dpadUp": "dpup", "dpadDown": "dpdown", "dpadLeft": "dpleft", "dpadRight": "dpright",
        "l": "leftshoulder", "r": "rightshoulder", "zl": "lefttrigger", "zr": "righttrigger",
        "minus": "back", "plus": "start", "l3": "leftstick", "r3": "rightstick",
        "leftStick": "left", "rightStick": "right",
        "leftGrip": "paddle2", "rightGrip": "paddle1", "leftGrip2": "paddle4", "rightGrip2": "paddle3", "leftPad": "touchpad", "rightPad": "rightstick",
    ]
}

struct ControllersConfig: Codable, Equatable {
    var players: [PlayerConfig] = Array(repeating: PlayerConfig(), count: 4)
}

struct PadDevice: Identifiable, Hashable {
    let instance: Int32
    let guid: String
    let name: String
    var id: Int32 { instance }
}

// MARK: - SDL2 manager

/// Detects game controllers through SDL2's game-controller API (the same
/// mapping database Ryujinx uses) and pumps SDL on a main-thread timer.
@MainActor
final class ControllerManager: ObservableObject {
    static let shared = ControllerManager()

    @Published private(set) var devices: [PadDevice] = []
    @Published var config = ControllersConfig() {
        didSet { if !editing && config != oldValue { Store.save(config, to: Paths.controllersFile) } }
    }
    @Published var capturing: PadSlot?
    @Published var player = 0
    @Published private(set) var live: [String: Double] = [:]
    @Published private(set) var sdlError: String?

    private var editing = false
    private var savedConfig: ControllersConfig?

    func beginEditing() { savedConfig = config; editing = true }
    func applyEditing() throws { try Store.saveChecked(config, to: Paths.controllersFile); savedConfig = config }
    func endEditing() {
        capturing = nil
        if let savedConfig { config = savedConfig }
        savedConfig = nil
        editing = false
    }
    func refresh() {
        start()
        for index in 0..<shim_num_joysticks() { open(index: index) }
        pump()
    }

    private var handles: [Int32: UnsafeMutableRawPointer] = [:]
    private var timer: Timer?
    private var started = false

    init() {
        if var c = Store.load(ControllersConfig.self, from: Paths.controllersFile) {
            while c.players.count < 4 { c.players.append(PlayerConfig()) }
            config = c
        }
    }

    func start() {
        guard !started else { return }
        started = true
        if shim_init() != 0 {
            sdlError = String(cString: SDL_GetError())
            return
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.pump() }
        }
        pump()
    }

    var current: PlayerConfig {
        get { config.players[player] }
        set { config.players[player] = newValue }
    }

    private func handle(for guid: String?) -> UnsafeMutableRawPointer? {
        guard let guid, let d = devices.first(where: { $0.guid == guid }) else { return nil }
        return handles[d.instance]
    }

    private func device(instance: Int32) -> PadDevice? { devices.first { $0.instance == instance } }

    // MARK: events

    private func pump() {
        var which: Int32 = 0, code: Int32 = 0, value: Int32 = 0
        while true {
            let kind = shim_poll(&which, &code, &value)
            if kind == SHIM_EV_NONE { break }
            switch kind {
            case SHIM_EV_ADDED: open(index: which)
            case SHIM_EV_REMOVED: close(instance: which)
            case SHIM_EV_BUTTON:
                if value != 0 { captured(instance: which, control: String(cString: shim_button_name(code)), strength: 1) }
            case SHIM_EV_AXIS:
                captured(instance: which, control: String(cString: shim_axis_name(code)),
                         strength: Double(value) / 32767)
            default: break
            }
        }
        refreshLive()
    }

    private func open(index: Int32) {
        guard shim_is_controller(index) != 0, let h = shim_open(index) else { return }
        let inst = shim_instance_id(h)
        guard handles[inst] == nil else { shim_close(h); return }
        var buf = [CChar](repeating: 0, count: 64)
        shim_device_guid(index, &buf, Int32(buf.count))
        let guid = String(cString: buf)
        handles[inst] = h
        devices.append(PadDevice(instance: inst, guid: guid, name: String(cString: shim_device_name(index))))
    }

    private func close(instance: Int32) {
        if let h = handles.removeValue(forKey: instance) { shim_close(h) }
        devices.removeAll { $0.instance == instance }
    }

    /// While a slot is being assigned, the next strong input on the player's
    /// device (any device when none is chosen) becomes its mapping.
    private func captured(instance: Int32, control: String, strength: Double) {
        guard let slot = capturing, let dev = device(instance: instance) else { return }
        if let g = current.deviceGUID, g != dev.guid { return }
        guard abs(strength) > 0.6 else { return }
        var mapped: String?
        if slot.isStick {
            if control.hasPrefix("left") && control != "lefttrigger" && control != "leftshoulder"
                && control != "leftstick" { mapped = "left" }
            if control.hasPrefix("right") && control != "righttrigger" && control != "rightshoulder"
                && control != "rightstick" { mapped = "right" }
        } else if ["leftx", "lefty", "rightx", "righty"].contains(control) {
            mapped = nil   // stick axis for a button slot: ignore
        } else {
            mapped = control
        }
        guard let mapped else { return }
        var p = current
        p.mapping[slot.rawValue] = mapped
        if p.deviceGUID == nil { p.deviceGUID = dev.guid; p.deviceName = dev.name }
        current = p
        capturing = nil
    }

    private func refreshLive() {
        guard let h = handle(for: current.deviceGUID) else {
            if !live.isEmpty { live = [:] }
            return
        }
        var s: [String: Double] = [:]
        for i in 0..<shim_button_count() {
            let n = String(cString: shim_button_name(i))
            if !n.isEmpty { s[n] = Double(shim_button(h, i)) }
        }
        for i in 0..<shim_axis_count() {
            let n = String(cString: shim_axis_name(i))
            if !n.isEmpty { s[n] = Double(shim_axis(h, i)) / 32767 }
        }
        if s != live { live = s }
    }

    // MARK: queries for the UI

    /// Whether the control mapped to `slot` is active right now.
    func isActive(_ slot: PadSlot) -> Bool {
        guard let c = current.mapping[slot.rawValue] else { return false }
        if slot.isStick {
            let x = live["\(c)x"] ?? 0, y = live["\(c)y"] ?? 0
            let dz = slot == .leftStick ? current.deadzoneLeft : current.deadzoneRight
            return (x * x + y * y).squareRoot() > dz
        }
        let v = live[c] ?? 0
        return c.hasSuffix("trigger") ? v > current.triggerThreshold : v > 0.5
    }

    func stick(_ slot: PadSlot) -> CGPoint {
        let c = current.mapping[slot.rawValue] ?? (slot == .leftStick ? "left" : "right")
        return CGPoint(x: live["\(c)x"] ?? 0, y: live["\(c)y"] ?? 0)
    }

    func testRumble() {
        if let h = handle(for: current.deviceGUID) { let strength = Int32(max(0, min(1, current.rumbleStrength)) * 65535)
            _ = shim_rumble(h, strength, strength, 400) }
    }

    var currentHasRumble: Bool {
        guard let h = handle(for: current.deviceGUID) else { return false }
        return shim_has_rumble(h) != 0
    }

    // MARK: Ryujinx import

    /// .NET Guid text ("00000003-045e-0000-8e02-000014016800") -> SDL's hex GUID.
    static func sdlGUID(fromDotNet s: String) -> String? {
        let hex = Array(s.replacingOccurrences(of: "-", with: "").lowercased())
        guard hex.count == 32 else { return nil }
        var bytes: [String] = stride(from: 0, to: 32, by: 2).map { String(hex[$0..<$0 + 2]) }
        bytes.replaceSubrange(0..<4, with: bytes[0..<4].reversed())
        bytes.replaceSubrange(4..<6, with: bytes[4..<6].reversed())
        bytes.replaceSubrange(6..<8, with: bytes[6..<8].reversed())
        return bytes.joined()
    }

    /// Equal GUIDs, ignoring the CRC in bytes 2-3 (Ryujinx zeroes it).
    static func sameDevice(_ a: String, _ b: String) -> Bool {
        let x = Array(a.lowercased()), y = Array(b.lowercased())
        guard x.count == 32, y.count == 32 else { return false }
        return x[0..<4] == y[0..<4] && x[8...] == y[8...]
    }

    private static func sdlName(_ ryu: String) -> String {
        if ryu.hasPrefix("Dpad") { return "dp" + ryu.dropFirst(4).lowercased() }
        return ryu.lowercased()
    }

    func importRyujinx() -> String {
        let url = Paths.home.appendingPathComponent("Library/Application Support/Ryujinx/Config.json")
        guard let data = try? Data(contentsOf: url),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let inputs = root["input_config"] as? [[String: Any]] else {
            return "No se pudo leer la configuración de Ryujinx."
        }
        var imported = 0
        var c = config
        for e in inputs {
            guard (e["backend"] as? String) == "GamepadSDL2",
                  let pi = e["player_index"] as? String, pi.hasPrefix("Player"),
                  let n = Int(pi.dropFirst(6)), (1...4).contains(n) else { continue }
            var p = PlayerConfig()
            if let id = e["id"] as? String, let g = Self.sdlGUID(fromDotNet: String(id.suffix(36))) {
                let match = devices.first { Self.sameDevice($0.guid, g) }
                p.deviceGUID = match?.guid ?? g
                p.deviceName = match?.name ?? (e["name"] as? String)
            }
            // Ryujinx types are all Switch layouts (Pro / Joy-Con / handheld).
            p.controllerType = "switchpro"
            p.deadzoneLeft = e["deadzone_left"] as? Double ?? p.deadzoneLeft
            p.deadzoneRight = e["deadzone_right"] as? Double ?? p.deadzoneRight
            p.triggerThreshold = e["trigger_threshold"] as? Double ?? p.triggerThreshold
            p.rumble = (e["rumble"] as? [String: Any])?["enable_rumble"] as? Bool ?? false
            let fields: [(String, String, PadSlot)] = [
                ("left_joycon", "button_minus", .minus), ("left_joycon", "button_l", .l),
                ("left_joycon", "button_zl", .zl), ("left_joycon", "dpad_up", .dpadUp),
                ("left_joycon", "dpad_down", .dpadDown), ("left_joycon", "dpad_left", .dpadLeft),
                ("left_joycon", "dpad_right", .dpadRight), ("right_joycon", "button_plus", .plus),
                ("right_joycon", "button_r", .r), ("right_joycon", "button_zr", .zr),
                ("right_joycon", "button_a", .a), ("right_joycon", "button_b", .b),
                ("right_joycon", "button_x", .x), ("right_joycon", "button_y", .y),
                ("left_joycon_stick", "stick_button", .l3), ("right_joycon_stick", "stick_button", .r3),
                ("left_joycon_stick", "joystick", .leftStick), ("right_joycon_stick", "joystick", .rightStick),
            ]
            for (group, key, slot) in fields {
                if let v = (e[group] as? [String: Any])?[key] as? String, v != "Unbound" {
                    p.mapping[slot.rawValue] = Self.sdlName(v)
                }
            }
            c.players[n - 1] = p
            imported += 1
        }
        config = c
        return imported == 0 ? "Ryujinx no tiene mandos SDL2 configurados."
                             : "Importados \(imported) jugador(es) de Ryujinx."
    }
}
