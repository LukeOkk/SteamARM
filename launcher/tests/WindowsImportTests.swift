import Foundation

@main
struct WindowsImportTests {
    static func main() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let exe = dir.appendingPathComponent("program.exe")
        var bytes = Data(repeating: 0, count: 134)
        bytes[0] = 0x4d; bytes[1] = 0x5a; bytes[60] = 128
        bytes.replaceSubrange(128..<134, with: [0x50, 0x45, 0, 0, 0x64, 0x86])
        try bytes.write(to: exe)
        precondition(Installer.isWindowsExecutable(exe))
        precondition(ArchiveKind.detect(exe) == .windowsExe)
        precondition(Installer.executables(in: dir).contains("program.exe"))
        bytes[132] = 0x64; bytes[133] = 0xaa // ARM64 is not x86-64
        try bytes.write(to: exe)
        precondition(!Installer.isWindowsExecutable(exe))
        try Data([0x4d, 0x5a]).write(to: exe)
        precondition(!Installer.isWindowsExecutable(exe))
        let old = try JSONDecoder().decode(AppEntry.self, from: Data(#"{"id":"old","name":"Old","command":["/bin/true"],"root":"/","env":{},"kind":"custom"}"#.utf8))
        precondition(old.protonTool == nil)
        print("Windows import: x64 PE, ARM64 rejection, truncated PE and legacy app decoding passed")
    }
}
