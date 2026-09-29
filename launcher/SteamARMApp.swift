import AppKit
import SwiftUI

@main
struct SteamARMApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = LauncherModel.shared
    @StateObject private var pads = ControllerManager.shared

    var body: some Scene {
        WindowGroup("SteamARM", id: "main") {
            HomeView()
                .environmentObject(model)
                .environmentObject(pads)
                .frame(minWidth: 760, minHeight: 520)
                .preferredColorScheme(.dark)
        }
        .windowStyle(.hiddenTitleBar)
        .commands {
            CommandGroup(replacing: .newItem) {}
        }

        Settings {
            SettingsView()
                .environmentObject(model)
                .environmentObject(pads)
                .preferredColorScheme(.dark)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        HotkeyManager.shared.start()
        let model = LauncherModel.shared
        if model.settings.launchSteamOnStart && model.canLaunch {
            model.launch(model.allApps.first { $0.id == "steam" } ?? .steam)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

// MARK: - Shared styling

enum Theme {
    static let background = Color(red: 0.09, green: 0.10, blue: 0.12)
    static let card = Color(red: 0.15, green: 0.16, blue: 0.19)
    static let accent = Color(red: 0.23, green: 0.56, blue: 0.98)
}

/// Icon for an app: its image file when there is one, otherwise a glyph.
struct AppIconView: View {
    let app: AppEntry
    var size: CGFloat = 96

    var body: some View {
        Group {
            if let p = app.icon, let img = NSImage(contentsOfFile: p) {
                Image(nsImage: img).resizable().interpolation(.high).scaledToFit()
            } else {
                Image(systemName: glyph)
                    .resizable().scaledToFit()
                    .padding(size * 0.2)
                    .foregroundStyle(.white.opacity(0.9))
            }
        }
        .frame(width: size, height: size)
    }

    private var glyph: String {
        switch app.kind {
        case "steam": return "gamecontroller.fill"
        case "heroic": return "shield.lefthalf.filled"
        case "prism": return "cube.fill"
        case "android": return "apps.iphone"
        default: return "app.dashed"
        }
    }
}
