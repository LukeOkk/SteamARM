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
        // The esync/fsync booleans become one choice that keeps what ran; "vulkan" was MoltenVK.
        let old = try decoder.decode(LauncherSettings.self, from: Data(#"{"esync":true,"fsync":true,"graphicsBackend":"vulkan"}"#.utf8))
        precondition(old.synchronization == "esync" && old.graphicsBackend == "vulkanMoltenVK" && old.fallbackPolicy == "auto")
        let noEsync = try decoder.decode(LauncherSettings.self, from: Data(#"{"esync":false,"fsync":true}"#.utf8))
        precondition(noEsync.synchronization == "wineserver")
        precondition(defaults.synchronization == "auto" && defaults.graphicsBackend == "auto")
        let saved = String(decoding: try JSONEncoder().encode(old), as: UTF8.self)
        precondition(!saved.contains("\"esync\":") && !saved.contains("\"fsync\":") && saved.contains("\"synchronization\":\"esync\""))
        // apps.json from before builtIn/readiness/overrides still decodes; builtin-apps.json decodes.
        let oldApps = try decoder.decode([AppEntry].self, from: Data(#"[{"id":"h","name":"H","command":["/opt/apps/h/run"],"root":"/tmp/lxrt-steamroot","fexRootfs":"/","env":{},"kind":"heroic"}]"#.utf8))
        precondition(oldApps.count == 1 && oldApps[0].overrides == nil && !oldApps[0].isBuiltIn)
        // Run from the checkout: scripts/builtin-apps.json is read from the current directory.
        let builtIns = try decoder.decode([AppEntry].self, from: Data(contentsOf: URL(fileURLWithPath:
            FileManager.default.currentDirectoryPath).appendingPathComponent("scripts/builtin-apps.json")))
        precondition(builtIns.map(\.id) == ["steam", "steam-arm64"] && builtIns.allSatisfy(\.isBuiltIn))
        precondition(builtIns[1].isExperimental && builtIns[1].architecture == "aarch64" && builtIns[1].fexRootfs == nil)
        var withOverrides = oldApps[0]
        withOverrides.overrides = ["display": "vnc"]
        let reread = try decoder.decode(AppEntry.self, from: JSONEncoder().encode(withOverrides))
        precondition(reread == withOverrides)
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
        print("PASS: defaults, legacy decoding, sync/graphics migration, app entries, round trips, memory ceilings, controller apply/cancel")
    }
}
