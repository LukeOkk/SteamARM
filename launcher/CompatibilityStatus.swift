import Foundation

/// Read diagnostics in a subprocess so loading the driver never blocks the UI.
struct CompatibilityStatus: Decodable {
    struct Driver: Decodable { let path: String; let version: String }
    struct FEX: Decodable { let patchedInstalled: Bool; let steamInstalled: Bool }
    struct Proton: Decodable, Identifiable {
        let name: String
        let architecture: String
        let installed: Bool
        let supported: Bool
        var id: String { name }
        var status: String { supported ? "Disponible" : installed ? "No compatible aquí" : "Incompleto" }
    }
    let moltenvk: Driver
    let fex: FEX
    let protons: [Proton]
    let nativeArmReason: String
    let note: String

    static func load(project: URL) async -> CompatibilityStatus? {
        await Task.detached(priority: .utility) {
            let process = Process()
            let pipe = Pipe()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
            process.arguments = [project.appendingPathComponent("scripts/compat-status.py").path, "--json"]
            process.standardOutput = pipe
            process.standardError = FileHandle.nullDevice
            do {
                try process.run()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                process.waitUntilExit()
                guard process.terminationStatus == 0 else { return nil }
                return try JSONDecoder().decode(CompatibilityStatus.self, from: data)
            } catch { return nil }
        }.value
    }
}
