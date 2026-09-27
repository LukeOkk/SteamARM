import AppKit
import SwiftUI

@MainActor
final class HotkeyManager {
    static let shared = HotkeyManager()
    var settingsOpen = false
    private var localMonitor: Any?
    private var globalMonitor: Any?

    func start() {
        guard localMonitor == nil else { return }
        localMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
            let handled = MainActor.assumeIsolated { self?.handle(event) == true }
            return handled ? nil : event
        }
        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: .keyDown) { [weak self] event in
            MainActor.assumeIsolated { _ = self?.handle(event) }
        }
    }

    private func handle(_ event: NSEvent) -> Bool {
        guard !settingsOpen, !event.isARepeat else { return false }
        if NSApp.keyWindow?.firstResponder is NSTextView { return false }
        let model = LauncherModel.shared
        let key = Self.name(event)
        guard !key.isEmpty else { return false }
        for action in ["screenshot", "stopApp", "toggleMetalHud"] where model.settings.hotkeys[action] == key {
            switch action {
            case "screenshot": screenshot()
            case "stopApp": model.stop()
            case "toggleMetalHud": model.settings.metalHud.toggle()
            default: break
            }
            return true
        }
        return false
    }

    static func name(_ event: NSEvent) -> String {
        let names: [UInt16: String] = [122: "F1", 120: "F2", 99: "F3", 118: "F4", 96: "F5", 97: "F6",
            98: "F7", 100: "F8", 101: "F9", 109: "F10", 103: "F11", 111: "F12", 105: "F13",
            107: "F14", 113: "F15", 106: "F16", 64: "F17", 79: "F18", 80: "F19", 90: "F20",
            36: "Return", 48: "Tab", 49: "Space", 51: "Delete", 117: "ForwardDelete",
            123: "Left", 124: "Right", 125: "Down", 126: "Up", 115: "Home", 119: "End", 116: "PageUp", 121: "PageDown"]
        guard event.keyCode != 53 else { return "" }
        let key = names[event.keyCode] ?? event.charactersIgnoringModifiers?.uppercased() ?? ""
        guard !key.isEmpty else { return "" }
        var modifiers: [String] = []
        for (flag, name) in [(NSEvent.ModifierFlags.control, "Ctrl"), (.option, "Alt"), (.shift, "Shift"), (.command, "Cmd")] {
            if event.modifierFlags.contains(flag) { modifiers.append(name) }
        }
        return (modifiers + [key]).joined(separator: "+")
    }

    private func screenshot() {
        guard let windows = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]],
              let window = windows.first(where: {
                  ($0[kCGWindowOwnerPID as String] as? Int32) != ProcessInfo.processInfo.processIdentifier
                      && ($0[kCGWindowLayer as String] as? Int) == 0
                      && ($0[kCGWindowAlpha as String] as? Double ?? 1) > 0
              }), let id = window[kCGWindowNumber as String] as? UInt32 else {
            LauncherModel.shared.alert = "No hay otra ventana visible para capturar."
            return
        }
        let directory = Paths.home.appendingPathComponent("Pictures/SteamARM")
        do { try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true) }
        catch { LauncherModel.shared.alert = error.localizedDescription; return }
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd-HH-mm-ss-SSS"
        let file = directory.appendingPathComponent("SteamARM-\(formatter.string(from: Date())).png")
        Task {
            let result = await Shell.run("/usr/sbin/screencapture", ["-x", "-o", "-l", String(id), file.path])
            if result.status != 0 {
                LauncherModel.shared.alert = "No se pudo capturar la ventana. Revisa el permiso de grabación de pantalla.\n" + result.output
            }
        }
    }
}

struct HotkeyRecorder: NSViewRepresentable {
    @Binding var value: String
    func makeNSView(context: Context) -> RecorderButton { RecorderButton() }
    func updateNSView(_ view: RecorderButton, context: Context) {
        view.value = value
        view.onRecord = { value = $0 }
        if !view.recording { view.title = value.isEmpty ? "Sin asignar" : value }
    }

    final class RecorderButton: NSButton {
        var value = ""
        var onRecord: ((String) -> Void)?
        var recording = false
        override var acceptsFirstResponder: Bool { true }
        init() {
            super.init(frame: .zero)
            bezelStyle = .rounded
            target = self
            action = #selector(record)
            setAccessibilityLabel("Grabar atajo de teclado")
        }
        required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
        @objc private func record() {
            window?.makeFirstResponder(self)
            recording = true
            title = "Pulsa una tecla…"
        }
        override func performKeyEquivalent(with event: NSEvent) -> Bool {
            guard recording else { return super.performKeyEquivalent(with: event) }
            keyDown(with: event)
            return true
        }
        override func keyDown(with event: NSEvent) {
            guard recording else { super.keyDown(with: event); return }
            value = HotkeyManager.name(event)
            recording = false
            title = value.isEmpty ? "Sin asignar" : value
            onRecord?(value)
        }
        override func resignFirstResponder() -> Bool {
            recording = false
            title = value.isEmpty ? "Sin asignar" : value
            return super.resignFirstResponder()
        }
    }
}
