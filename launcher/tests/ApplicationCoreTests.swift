import Foundation

// launcher/ApplicationCore.swift, without the UI. Builds on macOS and Linux:
//   swiftc -parse-as-library launcher/ApplicationCore.swift \
//       launcher/tests/ApplicationCoreTests.swift -o build/application-core-tests
//   build/application-core-tests
@main
struct ApplicationCoreTests {
    static var failures = 0
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        if !ok { failures += 1; print("FAIL line \(line): \(what)") }
    }

    /// A minimal ELF with one PT_INTERP, 64- or 32-bit.
    static func elf(machine: UInt16, is64: Bool, type: UInt16 = 3, interp: String?) -> Data {
        var d = Data()
        func le<T: FixedWidthInteger>(_ v: T) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        d.append(contentsOf: [0x7f, 0x45, 0x4c, 0x46, is64 ? 2 : 1, 1, 1, 0])
        d.append(contentsOf: [UInt8](repeating: 0, count: 8))
        let ehsize: UInt16 = is64 ? 64 : 52, phentsize: UInt16 = is64 ? 56 : 32
        let bytes = Array((interp ?? "").utf8) + [0]
        let interpOff = Int(ehsize) + Int(phentsize)
        le(type); le(machine); le(UInt32(1))
        if is64 { le(UInt64(0)); le(UInt64(ehsize)); le(UInt64(0)) }
        else { le(UInt32(0)); le(UInt32(ehsize)); le(UInt32(0)) }
        le(UInt32(0)); le(ehsize); le(phentsize); le(UInt16(interp == nil ? 0 : 1)); le(UInt16(0)); le(UInt16(0)); le(UInt16(0))
        if let _ = interp {
            if is64 {
                le(UInt32(3)); le(UInt32(4)); le(UInt64(interpOff)); le(UInt64(0)); le(UInt64(0))
                le(UInt64(bytes.count)); le(UInt64(bytes.count)); le(UInt64(1))
            } else {
                le(UInt32(3)); le(UInt32(interpOff)); le(UInt32(0)); le(UInt32(0))
                le(UInt32(bytes.count)); le(UInt32(bytes.count)); le(UInt32(4)); le(UInt32(1))
            }
            d.append(contentsOf: bytes)
        }
        return d
    }

    static func main() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("appcore-\(getpid())")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        func write(_ name: String, _ data: Data) -> URL {
            let u = dir.appendingPathComponent(name); try? data.write(to: u); return u
        }

        // ELF: the architecture comes from e_machine, never from the name.
        let a64 = ELFInspector.inspect(write("steam.x86", elf(machine: 183, is64: true, interp: "/lib/ld-linux-aarch64.so.1")))
        check(a64?.architecture == .aarch64 && a64?.isPIE == true, "aarch64 PIE")
        check(a64?.interpreter == "/lib/ld-linux-aarch64.so.1", "aarch64 PT_INTERP: \(String(describing: a64?.interpreter))")
        let x64 = ELFInspector.inspect(write("game", elf(machine: 62, is64: true, type: 2, interp: "/lib64/ld-linux-x86-64.so.2")))
        check(x64?.architecture == .x86_64 && x64?.isPIE == false, "x86-64 ET_EXEC")
        check(x64?.interpreter == "/lib64/ld-linux-x86-64.so.2", "x86-64 PT_INTERP")
        let x32 = ELFInspector.inspect(write("old", elf(machine: 3, is64: false, interp: "/lib/ld-linux.so.2")))
        check(x32?.architecture == .i386 && x32?.interpreter == "/lib/ld-linux.so.2", "i386")
        let arm32 = ELFInspector.inspect(write("arm32", elf(machine: 40, is64: false, interp: nil)))
        check(arm32 != nil && arm32?.architecture == nil, "32-bit ARM: ELF but no runnable architecture")
        let stat = ELFInspector.inspect(write("static", elf(machine: 183, is64: true, type: 2, interp: nil)))
        check(stat?.architecture == .aarch64 && stat?.interpreter == nil, "static aarch64")
        check(ELFInspector.inspect(write("steam.sh", Data("#!/bin/bash\nexec steam\n".utf8))) == nil, "script is not ELF")
        check(ELFInspector.inspect(write("short", Data([0x7f, 0x45, 0x4c, 0x46]))) == nil, "truncated ELF")
        check(ELFInspector.inspect(dir.appendingPathComponent("missing")) == nil, "missing file")

        // Architecture of an AppEntry: nil (older entries, Steam x86) is x86-64.
        check(GuestArchitecture.of(nil) == .x86_64, "nil entry is x86_64")
        check(GuestArchitecture.of("aarch64") == .aarch64 && GuestArchitecture.of("arm") == nil, "entry parsing")

        // Launch plans: ARM64 native, x86 through FEX, never the other way.
        let p1 = try LaunchPlanner.plan(architecture: "aarch64")
        check(p1.runner == .native && p1.environment.id == "arm64" && p1.translatorLabel == "none", "aarch64 -> native")
        check(p1.usesVirtualMachine == false, "no VM")
        let p2 = try LaunchPlanner.plan(architecture: nil)
        check(p2.runner == .fex && p2.environment.id == "legacy-x86" && p2.environment.transitional, "legacy x86 -> FEX")
        let p3 = try LaunchPlanner.plan(architecture: "i386")
        check(p3.runner == .fex, "i386 -> FEX")
        do { _ = try LaunchPlanner.plan(architecture: "aarch64", environmentID: "legacy-x86"); check(false, "aarch64 pinned to FEX root") }
        catch let e as LaunchPlanError { check(e == .environmentCannotRun(environment: "legacy-x86", architecture: .aarch64), "\(e)") }
        do { _ = try LaunchPlanner.plan(architecture: "x86_64", environmentID: "arm64"); check(false, "x86 pinned to native root") }
        catch let e as LaunchPlanError { check(e == .environmentCannotRun(environment: "arm64", architecture: .x86_64), "\(e)") }
        do { _ = try LaunchPlanner.plan(architecture: "aarch64", environments: [.legacyX86]); check(false, "no ARM64 base") }
        catch let e as LaunchPlanError { check(e == .noEnvironment(.aarch64), "\(e)") }
        do { _ = try LaunchPlanner.plan(architecture: "armv7"); check(false, "armv7") }
        catch let e as LaunchPlanError { check(e == .unsupportedArchitecture("armv7"), "\(e)") }
        do { _ = try LaunchPlanner.plan(architecture: "aarch64", environmentID: "nope"); check(false, "unknown env") }
        catch let e as LaunchPlanError { check(e == .unknownEnvironment("nope"), "\(e)") }
        let envs = try JSONDecoder().decode([LinuxBaseEnvironment].self,
                                            from: JSONEncoder().encode(LinuxBaseEnvironment.builtIn))
        check(envs == LinuxBaseEnvironment.builtIn, "environments round-trip as JSON")

        // Sessions: lock before start, one app at a time, every path back to idle.
        let t0 = Date(timeIntervalSince1970: 1000)
        var s = SessionMachine()
        try s.begin("steam", at: t0)
        check(s.phase == .starting && !s.canLaunch, "starting holds the lock")
        do { try s.begin("heroic"); check(false, "second launch while starting") }
        catch let e as SessionError { check(e == .busy(runningApp: "steam"), "\(e)") }
        try s.started()
        do { try s.begin("heroic"); check(false, "second launch while running") } catch {}
        try s.requestStop()
        try s.ended(status: nil, at: t0.addingTimeInterval(30))
        check(s.phase == .cleanup && s.last?.outcome == .stopping && s.last?.duration == 30, "user stop")
        try s.cleanedUp()
        check(s.phase == .idle && s.canLaunch && s.appID == nil, "back to idle")

        try s.begin("heroic", at: t0)
        try s.started()
        try s.ended(status: 139, at: t0.addingTimeInterval(5))
        check(s.phase == .crashed && s.last?.status == 139, "crash recorded")
        try s.cleanedUp()
        check(s.phase == .idle, "crash -> cleanup -> idle")

        try s.begin("minecraft", at: t0)
        try s.startFailed(status: 2, at: t0)
        check(s.phase == .failed && s.last?.outcome == .failed, "start failure")
        do { try s.started(); check(false, "started after failure") } catch {}
        try s.cleanedUp()

        try s.begin("steam", at: t0)
        try s.started()
        try s.ended(status: 0, at: t0.addingTimeInterval(60))
        check(s.phase == .exited, "clean exit")
        try s.cleanedUp()
        do { try s.ended(status: 0); check(false, "ended while idle") } catch {}
        do { try s.requestStop(); check(false, "stop while idle") } catch {}

        print(failures == 0 ? "application core: all checks passed" : "application core: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }
}
