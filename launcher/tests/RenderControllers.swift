// Run from the repository root (macOS 14+):
// export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
// export CLANG_MODULE_CACHE_PATH=/tmp/steamarm-module-cache
// export SWIFT_MODULECACHE_PATH=/tmp/steamarm-module-cache
// swiftc -target arm64-apple-macos14 -parse-as-library -swift-version 5 -warnings-as-errors \
//   -import-objc-header launcher/SDLShim.h \
//   -Xcc -I/opt/homebrew/opt/sdl2/include/SDL2 \
//   -L/opt/homebrew/opt/sdl2/lib -lSDL2-2.0.0 \
//   launcher/Models.swift launcher/Controllers.swift launcher/ControllerDrawing.swift \
//   launcher/ControllersView.swift launcher/tests/RenderControllers.swift \
//   -o /tmp/steamarm-render-controllers
// STEAMARM_STATE="$(mktemp -d /tmp/steamarm-render-state-XXXXXX)" \
//   /tmp/steamarm-render-controllers /tmp/steamarm-render/
import AppKit
import SwiftUI

/// Offscreen rendering only: no window, SDL initialization, or screen capture.
@main
struct RenderControllers {
    @MainActor static func main() throws {
        guard CommandLine.arguments.count == 2 else {
            throw Failure(message: "Usage: RenderControllers OUTPUT_DIRECTORY")
        }
        let output = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        let pads = ControllerManager()
        pads.beginEditing()
        defer { pads.endEditing() }
        pads.config = ControllersConfig()
        var images = Set<Data>()
        for type in ControllerType.allCases {
            pads.current.controllerType = type.rawValue
            pads.current.ledColor = nil
            pads.capturing = nil
            let drawing = ControllerDrawing(type: type)
            let controls = drawing.controls
            precondition(Set(controls.map(\.id)).count == controls.count)
            let required = Set(PadSlot.allCases.filter {
                if [.leftGrip, .rightGrip].contains($0) { return type.paddles }
                if [.leftGrip2, .rightGrip2].contains($0) { return type == .steamcontroller2 }
                if [.leftPad, .rightPad].contains($0) { return type.trackpads }
                if [.rightStick, .r3].contains($0) { return type != .steamcontroller }
                return true
            })
            precondition(Set(controls.map(\.slot)) == required, "Missing mapping: \(type)")
            precondition(controls.allSatisfy { CGRect(x: 0, y: 0, width: 600, height: 380).contains($0.rect) })
            if type == .switchpro {
                let a = controls.first { $0.slot == .a }!
                let b = controls.first { $0.slot == .b }!
                precondition(a.rect.midX > b.rect.midX && a.rect.midY < b.rect.midY)
            }
            if type == .xboxelite2 {
                precondition(controls.filter { $0.kind == .paddle }.count == 2)
            }
            let normal = try render(drawing, pads, width: 900, height: 600)
            precondition(images.insert(normal).inserted, "Identical controller render: \(type)")
            try normal.write(to: output.appendingPathComponent(type.rawValue + ".png"))
            let compact = try render(drawing, pads, width: 420, height: 320)
            try compact.write(to: output.appendingPathComponent(type.rawValue + "-compact.png"))
            pads.capturing = .leftStick
            let capture = try render(drawing, pads, width: 900, height: 600)
            precondition(capture != normal, "Capture highlight missing")
            try capture.write(to: output.appendingPathComponent(type.rawValue + "-capture.png"))
            pads.capturing = nil
            if type.hasLED {
                pads.current.ledColor = "#FF3080"
                let customLED = try render(drawing, pads, width: 900, height: 600)
                precondition(customLED != normal, "Custom LED missing")
                try customLED.write(to: output.appendingPathComponent(type.rawValue + "-led.png"))
            }
            print("PASS: \(type.rawValue)")
        }
        pads.capturing = nil
        pads.current.controllerType = ControllerType.dualsense.rawValue
        let page = try renderPage(pads)
        try page.write(to: output.appendingPathComponent("controllers-view.png"))
        print("PASS: \(ControllerType.allCases.count) distinct controllers; mapping coverage, bounds, compact size, capture and LED")
    }

    // Native controls (GroupBox, TextField) are omitted by ImageRenderer. Cache an
    // offscreen hosting view for the page; this reads no screen pixels or windows.
    @MainActor static func renderPage(_ pads: ControllerManager) throws -> Data {
        _ = NSApplication.shared
        let view = NSHostingView(rootView: ControllersView(rendersOffscreen: true)
            .environmentObject(pads).environment(\.colorScheme, .dark)
            .frame(width: 1500, height: 1200).background(Color(white: 0.06)))
        view.frame = NSRect(x: 0, y: 0, width: 1500, height: 1200)
        view.layoutSubtreeIfNeeded()
        guard let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
            throw Failure(message: "Could not allocate page bitmap")
        }
        view.cacheDisplay(in: view.bounds, to: bitmap)
        guard let data = bitmap.representation(using: .png, properties: [:]) else {
            throw Failure(message: "Could not encode page bitmap")
        }
        return data
    }

    @MainActor static func render<V: View>(_ drawing: V, _ pads: ControllerManager,
                                  width: CGFloat, height: CGFloat) throws -> Data {
        let renderer = ImageRenderer(content: drawing.environmentObject(pads)
            .frame(width: width, height: height).background(Color(white: 0.06))
            .environment(\.colorScheme, .dark))
        renderer.scale = 1
        guard let image = renderer.cgImage else { throw Failure(message: "ImageRenderer returned no image") }
        precondition(image.width == Int(width) && image.height == Int(height))
        let bitmap = NSBitmapImageRep(cgImage: image)
        guard let data = bitmap.representation(using: .png, properties: [:]) else {
            throw Failure(message: "PNG encoding failed")
        }
        precondition(data.count > 8000, "Unexpectedly empty drawing")
        return data
    }

    struct Failure: Error { let message: String }
}

// Standalone harness theme: the app's Theme lives beside its @main entry point.
enum Theme {
    static let card = Color(red: 0.15, green: 0.16, blue: 0.19)
    static let accent = Color(red: 0.23, green: 0.56, blue: 0.98)
}
