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
        /// Compiled into its ntdll.so / wineserver (steamarm-compat-status).
        let esync: Bool?
        let fsync: Bool?
        let ntsync: Bool?
        var id: String { name }
        var status: String { supported ? "Disponible" : installed ? "No compatible aquí" : "Incompleto" }
    }
    let moltenvk: Driver
    let fex: FEX
    let protons: [Proton]
    let nativeArmReason: String
    let note: String
    /// The same report as RuntimeCapabilities.detect(from:) reads it.
    let runtime: RuntimeProbe

    private enum CodingKeys: String, CodingKey { case moltenvk, fex, protons, nativeArmReason, note }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        moltenvk = try c.decode(Driver.self, forKey: .moltenvk)
        fex = try c.decode(FEX.self, forKey: .fex)
        protons = try c.decode([Proton].self, forKey: .protons)
        nativeArmReason = try c.decode(String.self, forKey: .nativeArmReason)
        note = try c.decode(String.self, forKey: .note)
        runtime = try RuntimeProbe(from: decoder)
    }

    static func load(project: URL) async -> CompatibilityStatus? {
        await Task.detached(priority: .utility) {
            let bundled = Bundle.main.resourceURL?.appendingPathComponent("steamarm-compat-status")
            let candidates = [
                project.appendingPathComponent("build/steamarm-compat-status"),
                bundled,
            ].compactMap { $0 }
            guard let executable = candidates.first(where: {
                FileManager.default.isExecutableFile(atPath: $0.path)
            }) else { return nil }

            let process = Process()
            let pipe = Pipe()
            process.executableURL = executable
            process.arguments = ["--json"]
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
