import Foundation

@main
struct SettingsTests {
    @MainActor static func main() throws {
        guard let state = ProcessInfo.processInfo.environment["STEAMARM_STATE"],
              state.contains("steamarm-settings-test-"), !FileManager.default.fileExists(atPath: Paths.controllersFile.path) else {
            fatalError("Run with a fresh temporary STEAMARM_STATE containing steamarm-settings-test-")
        }
        let decoder = JSONDecoder()
        let defaults = try decoder.decode(LauncherSettings.self, from: Data("{}".utf8))
        precondition(defaults == LauncherSettings())
        let legacy = try decoder.decode(LauncherSettings.self, from: Data(#"{"metalHud":true,"display":"vnc","resolution":"1920x1080","extraEnv":{"TEST":"1"}}"#.utf8))
        precondition(legacy.metalHud && legacy.display == .vnc && legacy.confirmStop)
        precondition(legacy.extraEnv == ["TEST": "1"] && legacy.hotkeys["screenshot"] == "F8")
        var changed = defaults
        changed.guestLanguage = "es_ES.UTF-8"
        changed.timezone = "America/Montevideo"
        changed.fexTSO = "fast"
        changed.hotkeys["stopApp"] = "Cmd+F9"
        changed.dramGB = 12
        let decoded = try decoder.decode(LauncherSettings.self, from: JSONEncoder().encode(changed))
        precondition(decoded == changed)
        for (total, expected) in [(8, 6), (16, 12), (24, 20), (32, 28), (64, 60), (128, 124)] {
            precondition(MemoryChoices.maximum(totalGB: total) == expected)
        }
        for (old, new) in [("ProController", "switchpro"), ("Xbox", "xboxseries")] {
            let player = try decoder.decode(PlayerConfig.self, from: Data("{\"controllerType\":\"\(old)\",\"mapping\":{\"a\":\"b\"}}".utf8))
            precondition(player.controllerType == new && player.mapping["a"] == "b")
            precondition(player.mapping["leftStick"] == "left" && player.rangeLeft == 1 && player.rumbleStrength == 1)
        }
        var player = PlayerConfig()
        player.invertLX = true; player.rotateR = true; player.rangeRight = 1.5
        player.ledColor = "#AABBCC"; player.motion = true; player.controllerType = "dualsenseedge"
        let restored = try decoder.decode(PlayerConfig.self, from: JSONEncoder().encode(player))
        precondition(restored == player)
        let manager = ControllerManager()
        let baseline = manager.config
        manager.beginEditing()
        manager.current = player
        precondition(Store.load(ControllersConfig.self, from: Paths.controllersFile) == nil)
        manager.endEditing()
        precondition(manager.config == baseline)
        manager.beginEditing()
        manager.current = player
        try manager.applyEditing()
        manager.current.controllerType = "ds4"
        manager.endEditing()
        precondition(manager.current == player)
        precondition(Store.load(ControllersConfig.self, from: Paths.controllersFile)?.players[0] == player)
        print("PASS: defaults, legacy decoding, round trips, memory ceilings, controller apply/cancel")
    }
}
