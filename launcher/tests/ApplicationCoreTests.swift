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

    static let t0Stats = Date(timeIntervalSince1970: 2_000_000)

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
        // The Fedora ARM64 root of the native arm64 Steam client: native, never FEX.
        let p4 = try LaunchPlanner.plan(architecture: "aarch64", environmentID: "armroot")
        check(p4.runner == .native && p4.environment.guestRoot == "/tmp/lxrt-armroot" && !p4.usesVirtualMachine, "aarch64 in armroot -> native")
        check(p4.environment.transitional, "armroot is transitional")
        do { _ = try LaunchPlanner.plan(architecture: "x86_64", environmentID: "armroot"); check(false, "x86 pinned to armroot") }
        catch let e as LaunchPlanError { check(e == .environmentCannotRun(environment: "armroot", architecture: .x86_64), "\(e)") }
        check(LinuxBaseEnvironment.named(guestRoot: "/tmp/lxrt-armroot/")?.id == "armroot", "environment by guest root")
        check(LinuxBaseEnvironment.named(guestRoot: "/tmp/lxrt-steamroot")?.id == "legacy-x86", "x86 root by guest root")
        check(LinuxBaseEnvironment.named(guestRoot: "/tmp/elsewhere") == nil, "unknown root")

        // Session files: running.status and running.arch as the scripts write them.
        check(SessionFiles.status("7\n") == (7, nil), "exit status 7")
        check(SessionFiles.status("137 signal 9") == (137, 9), "death by signal 9")
        check(SessionFiles.status("") == (nil, nil), "no status")
        check(SessionFiles.arch("x86_64 FEX\n")?.architecture == .x86_64 && SessionFiles.arch("x86_64 FEX")?.translator == "FEX", "running.arch x86")
        check(SessionFiles.arch("aarch64 none")?.architecture == .aarch64 && SessionFiles.arch("aarch64 none")?.translator == "none", "running.arch aarch64")
        check(SessionFiles.arch("") == nil, "empty running.arch")

        // What run-app.sh did: its last "session=" line, never the app's name.
        check(SessionFiles.launchKind("Steam (x86_64, translator: FEX) starting on :2 (log l).\nsession=started\n") == .started, "started")
        check(SessionFiles.launchKind("Steam is already running.\nsession=reshown\n") == .reshown,
              "the x86 Steam of run-app.sh shown again is not an adoption")
        check(SessionFiles.launchKind("Steam is already running.\nsession=adopted") == .adopted, "run-steam.sh Steam adopted")
        check(SessionFiles.launchKind("Heroic Steam is already running.\nsession=reshown") == .reshown, "a name ending in Steam")
        check(SessionFiles.launchKind("Steam is already running.") == nil, "no marker: nothing guessed from the text")
        check(SessionFiles.launchKind("session=adopted is already running.\nsession=started") == .started, "only a whole line counts")
        check(SessionFiles.launchKind("session=bogus") == nil, "unknown kind")

        // When a session is over: its own leader, status and group decide.
        typealias Probe = SessionLiveness.Probe
        let orphansOnly = Probe(leaderAlive: false, statusWritten: true, groupAlive: false, anyGuest: true)
        check(!SessionLiveness.alive(orphansOnly, wrapped: true),
              "wrapper gone after the status: over, whatever guests (orphaned zygotes) remain")
        check(SessionLiveness.alive(Probe(leaderAlive: true, statusWritten: true, groupAlive: false, anyGuest: false), wrapped: true),
              "wrapper still clearing its group: running")
        check(SessionLiveness.alive(Probe(leaderAlive: false, statusWritten: false, groupAlive: true, anyGuest: true), wrapped: true),
              "wrapper killed, program's group alive: running")
        check(!SessionLiveness.alive(Probe(leaderAlive: false, statusWritten: false, groupAlive: false, anyGuest: true), wrapped: true),
              "wrapper killed, group empty: over")
        check(SessionLiveness.alive(orphansOnly, wrapped: false), "adopted Steam without a wrapper: any guest keeps it")
        check(!SessionLiveness.alive(Probe(leaderAlive: false, statusWritten: false, groupAlive: false, anyGuest: false), wrapped: false),
              "adopted Steam: no guest left")
        check(SessionLiveness.isLeader(command: "/usr/bin/python3 scripts/session.py run /x/launcher scripts/run-native.sh", wrapped: true),
              "the wrapper leads")
        check(!SessionLiveness.isLeader(command: "/Applications/Other.app/Contents/MacOS/Other", wrapped: true),
              "a reused PID is not the session")
        check(SessionLiveness.isLeader(command: "/x/build/lxrun /tmp/fexhome/.local/share/Steam/ubuntu12_32/steam -x", wrapped: false),
              "an adopted Steam's runtime process")
        check(SessionLiveness.isLeader(command: nil, wrapped: true), "unreadable command: not taken for a reused PID")

        // A root that is not there: never made, or a link into a volume that is not attached.
        let fm = FileManager.default
        let absent = dir.appendingPathComponent("absent-root").path
        check(RootPresence.of(absent) == .missing, "no root at all")
        check(RootPresence.of(dir.path) == .present, "a directory")
        let volume = "SteamARM-test-volume-\(getpid())"
        let intoVolume = dir.appendingPathComponent("arm64root").path
        try fm.createSymbolicLink(atPath: intoVolume, withDestinationPath: "/Volumes/\(volume)/arm64root")
        check(RootPresence.of(intoVolume) == .volumeNotAttached(volume: volume, target: "/Volumes/\(volume)/arm64root"),
              "link into a volume that is not attached: \(RootPresence.of(intoVolume))")
        let dangling = dir.appendingPathComponent("armroot").path
        try fm.createSymbolicLink(atPath: dangling, withDestinationPath: "gone/armroot")
        check(RootPresence.of(dangling) == .danglingLink(target: dir.appendingPathComponent("gone/armroot").path),
              "relative link to nothing: \(RootPresence.of(dangling))")
        let live = dir.appendingPathComponent("liveroot").path
        try fm.createSymbolicLink(atPath: live, withDestinationPath: dir.path)
        check(RootPresence.of(live) == .present, "a link that resolves")

        // Per-app stats (library.json): optional fields, partial files decode.
        let partial = try JSONDecoder().decode([String: AppStats].self, from: Data(#"{"steam":{"favorite":true},"x":{}}"#.utf8))
        check(partial["steam"]?.favorite == true && partial["steam"]?.launchCount == nil && partial["x"] == AppStats(), "partial library.json")
        var st = AppStats()
        st.noteLaunch(at: t0Stats)
        st.noteLaunch(at: t0Stats.addingTimeInterval(100))
        st.noteEnd(SessionRecord(appID: "steam", status: 0, outcome: .exited, duration: 42))
        st.noteEnd(SessionRecord(appID: "steam", status: nil, outcome: .stopping, duration: 8), countRuntime: false)
        check(st.launchCount == 2 && st.lastLaunch == t0Stats.addingTimeInterval(100), "launch count and last launch")
        check(st.totalRuntime == 42 && st.lastStatus == nil && st.lastOutcome == "stopping", "runtime counted only when known")
        check(try JSONDecoder().decode(AppStats.self, from: JSONEncoder().encode(st)) == st, "stats round-trip")

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

        // Backends: only Apple Hypervisor may use a VM, and nothing falls back to it.
        for e in ExecutionBackend.allCases {
            check(e.usesVirtualMachine == (e == .appleHypervisorLegacy), "VM flag of \(e)")
            check((SessionVirtualizationMode(e) == .vmAppleHypervisor) == (e == .appleHypervisorLegacy), "session mode of \(e)")
        }
        check(SessionVirtualizationMode(.auto).label == "ZERO-VM", "ZERO-VM label")
        for p in ApplicationBackendPreset.allCases {
            check(p.execution.usesVirtualMachine == (p == .appleHypervisor), "preset \(p) VM flag")
        }
        check(ApplicationBackendPreset.vncScreenSharing.presentation() == .vncScreenSharing, "VNC preset presents through VNC")
        check(ApplicationBackendPreset.lightningJIT.presentation(default: .vncScreenSharing) == .vncScreenSharing, "JIT preset keeps chosen presentation")
        let caps = RuntimeCapabilities.current
        for e in ExecutionBackend.allCases {
            check(caps.executionFallback(after: e) != .appleHypervisorLegacy, "no silent fallback to a VM after \(e)")
        }
        check(caps.execution[.appleHypervisorLegacy]?.usable == false, "no VM path in this tree")
        check(caps.execution[.lightningJIT]?.usable == false, "no Lightning JIT in this tree")
        // Sync: fsync needs futex_waitv, which lxrun lacks; AUTO never picks an unusable one.
        check(caps.synchronization[.fsync]?.state == .unsupported, "fsync unsupported")
        check(caps.effectiveSynchronization(.fsync) == .wineserver, "requested fsync falls back to wineserver")
        check(caps.effectiveSynchronization(.esync) == .esync, "esync usable (experimental)")
        check(caps.effectiveSynchronization(.auto) == .wineserver, "AUTO picks only a ready fast path: none yet")
        // Graphics: KosmicKrisp and WineD3D are not integrated, so fallback lands on MoltenVK or nothing.
        check(caps.graphicsFallback(after: .vulkanKosmicKrisp) == .vulkanMoltenVK, "KosmicKrisp falls back to MoltenVK")
        check(caps.graphicsFallback(after: .auto) == .vulkanMoltenVK, "AUTO starts at MoltenVK today")
        check(caps.graphicsFallback(after: .vulkanMoltenVK) == nil, "no WineD3D fallback yet")
        for (k, v) in caps.graphics { check(!v.reason.isEmpty, "\(k) has a reason") }
        check(caps.graphics[.openGLWineD3D]?.state == .unsupported, "WineD3D: software GL only")
        check(caps.effectiveGraphics(.auto) == .vulkanMoltenVK, "AUTO graphics is MoltenVK")
        check(caps.effectiveGraphics(.vulkanKosmicKrisp) == .vulkanMoltenVK, "undetected KosmicKrisp -> MoltenVK")
        check(caps.effectiveGraphics(.openGLWineD3D) == .vulkanMoltenVK, "WineD3D -> MoltenVK")
        // Presets: the two ZERO-VM ones follow presentation; the others are never usable.
        check(caps.status(of: .nativeWindows).state == .ready, "native windows preset")
        check(caps.status(of: .vncScreenSharing).state == .experimental, "VNC preset")
        check(!caps.status(of: .lightningJIT).usable && !caps.status(of: .appleHypervisor).usable, "JIT/VM presets unusable")
        check(!caps.status(of: .appleHypervisor).reason.isEmpty, "Apple Hypervisor says why")

        // Detection (scripts/compat-status.py --json) refines the table, never beyond it.
        let json = #"""
        {"moltenvk":{"path":"/opt/homebrew/lib/libMoltenVK.dylib","version":"1.4.2","vulkan":"1.4.357"},
         "fex":{"patchedInstalled":true,"steamInstalled":false},
         "protons":[{"name":"Proton - Experimental","architecture":"x86_64","installed":true,"supported":true,"esync":false,"fsync":true,"ntsync":true},
                    {"name":"Proton 10.0","architecture":"x86_64","installed":true,"supported":true,"esync":true,"fsync":true,"ntsync":false}],
         "kosmickrisp":{"icd_json":"/k.json","library":"/k.dylib","version":"26.2.3","api_version":"1.4.354","os_ok":true,"exports_icd":true},
         "shim":{"path":"/s","installed":true,"icd_selection":false},
         "presentation":{"native_x":true,"xvnc":true,"screen_sharing":true},
         "nativeArmReason":"r","note":"n"}
        """#
        var probe = try JSONDecoder().decode(RuntimeProbe.self, from: Data(json.utf8))
        check(probe.kosmickrisp?.version == "26.2.3" && probe.shim?.icdSelection == false && probe.protons.count == 2, "probe decodes")
        var d = RuntimeCapabilities.detect(from: probe)
        check(d.graphics[.vulkanMoltenVK]?.state == .ready && d.graphics[.vulkanMoltenVK]?.reason.contains("1.4.2") == true, "MoltenVK detected")
        check(d.graphics[.vulkanKosmicKrisp]?.state == .unavailable && d.graphics[.vulkanKosmicKrisp]?.reason.contains("STEAMARM_VK_ICD") == true,
              "KosmicKrisp installed but the shim cannot select it")
        check(d.synchronization[.esync]?.state == .experimental && d.synchronization[.esync]?.reason.contains("Proton 10.0") == true, "esync: Proton 10.0 only")
        check(d.synchronization[.fsync]?.state == .unsupported && d.synchronization[.msync]?.state == .unavailable, "fsync/msync unchanged by detection")
        check(d.execution[.appleHypervisorLegacy]?.usable == false && d.execution[.lightningJIT]?.usable == false, "detection never enables JIT or a VM")
        check(d.graphics[.openGLWineD3D]?.state == .unsupported, "detection never enables WineD3D")
        check(d.effectiveSynchronization(.auto) == .wineserver, "AUTO still wineserver with esync experimental")
        probe.shim?.icdSelection = true
        d = RuntimeCapabilities.detect(from: probe)
        check(d.graphics[.vulkanKosmicKrisp]?.state == .experimental, "KosmicKrisp with an ICD-selecting shim: experimental")
        check(d.effectiveGraphics(.auto) == .vulkanMoltenVK && d.effectiveGraphics(.vulkanKosmicKrisp) == .vulkanKosmicKrisp, "AUTO stays on MoltenVK")
        probe.kosmickrisp?.osOK = false
        check(RuntimeCapabilities.detect(from: probe).graphics[.vulkanKosmicKrisp]?.state == .unavailable, "KosmicKrisp needs macOS 26")
        probe.kosmickrisp = .init(foundElsewhere: "/opt/mesa-src/lib/libvulkan_kosmickrisp.dylib")
        let elsewhere = RuntimeCapabilities.detect(from: probe).graphics[.vulkanKosmicKrisp]
        check(elsewhere?.state == .unavailable && elsewhere?.reason.contains("/opt/mesa-src/lib") == true
              && elsewhere?.reason.contains("/opt/homebrew/lib") == true, "KosmicKrisp the shim does not load: says where it is")
        let kkJSON = #"{"icd_json":"","library":"","version":"","os_ok":true,"exports_icd":false,"found_elsewhere":"/e/k.dylib"}"#
        check(try JSONDecoder().decode(RuntimeProbe.KosmicKrisp.self, from: Data(kkJSON.utf8)).foundElsewhere == "/e/k.dylib",
              "found_elsewhere decodes")
        probe.kosmickrisp = nil
        check(RuntimeCapabilities.detect(from: probe).graphics[.vulkanKosmicKrisp]?.reason.contains("no está instalado") == true, "no KosmicKrisp")
        let undetected = RuntimeCapabilities.undetected
        check(undetected.graphics[.vulkanKosmicKrisp]?.usable == false
              && undetected.graphics[.vulkanKosmicKrisp]?.reason.contains("compat-status.py") == true, "no answer: says so")
        probe.protons = [.init(name: "Proton - Experimental", fsync: true, ntsync: true)]
        probe.moltenvk = .init(path: "", version: "no instalado")
        probe.presentation = .init(nativeX: false, xvnc: true, screenSharing: false)
        d = RuntimeCapabilities.detect(from: probe)
        check(d.synchronization[.esync]?.state == .unavailable, "no esync-capable Proton")
        check(d.effectiveSynchronization(.esync) == .wineserver, "unavailable esync -> wineserver")
        check(d.graphics[.vulkanMoltenVK]?.state == .unavailable, "no MoltenVK")
        check(d.presentation[.nativeWindows]?.state == .unavailable && d.presentation[.vncScreenSharing]?.state == .unavailable, "display pieces missing")

        // Issues and the fallback policy: only settings that cannot work count.
        check(caps.issues(display: .nativeWindows, synchronization: .auto, graphics: .auto, appInX86Root: false).isEmpty, "nothing to fall back from")
        check(caps.issues(display: .vncScreenSharing, synchronization: .esync, graphics: .vulkanMoltenVK, appInX86Root: true).isEmpty, "VNC in the x86 root is fine")
        let vncArm = caps.issues(display: .vncScreenSharing, synchronization: .auto, graphics: .auto, appInX86Root: false)
        check(vncArm.count == 1 && vncArm[0].setting == "display" && vncArm[0].fallback == "native", "VNC outside the x86 root -> native")
        let bad = caps.issues(display: .nativeWindows, synchronization: .fsync, graphics: .vulkanKosmicKrisp, appInX86Root: true)
        check(bad.map(\.setting) == ["synchronization", "graphicsBackend"], "fsync and KosmicKrisp both reported")
        check(bad.first?.fallback == "wineserver" && bad.last?.fallback == "vulkanMoltenVK", "their fallbacks")
        check(bad.allSatisfy { !$0.summary.isEmpty }, "issues say why")
        check(FallbackPolicy.auto.decision(for: bad) == .proceed && FallbackPolicy.strict.decision(for: bad) == .refuse
              && FallbackPolicy.ask.decision(for: bad) == .ask, "policy decisions")
        check(FallbackPolicy.strict.decision(for: []) == .proceed, "no issue: every policy launches")
        // Every announced fallback reaches run-app.sh (settings-env.py fallback_overrides).
        check(FallbackPolicy.environment(for: bad) == ["STEAMARM_SYNCHRONIZATION": "wineserver",
                                                        "STEAMARM_GRAPHICS_BACKEND": "vulkanMoltenVK"], "fallbacks as variables")
        check(FallbackPolicy.environment(for: vncArm) == ["STEAMARM_DISPLAY": "native"], "display fallback")
        check(FallbackPolicy.environment(for: []).isEmpty, "nothing to hand over")

        // settings.json from before the selectors keeps what it ran.
        check(SettingsMigration.synchronization(stored: nil, esync: true, fsync: true) == "esync", "esync+fsync -> esync")
        check(SettingsMigration.synchronization(stored: nil, esync: false, fsync: true) == "wineserver", "no esync -> wineserver")
        check(SettingsMigration.synchronization(stored: nil, esync: nil, fsync: false) == "esync", "only fsync off -> esync")
        check(SettingsMigration.synchronization(stored: nil, esync: nil, fsync: nil) == "auto", "new install -> AUTO")
        check(SettingsMigration.synchronization(stored: "wineserver", esync: true, fsync: true) == "wineserver", "stored choice wins")
        check(SettingsMigration.graphicsBackend(stored: "vulkan") == "vulkanMoltenVK", "legacy vulkan -> MoltenVK")
        check(SettingsMigration.graphicsBackend(stored: nil) == "auto" && SettingsMigration.graphicsBackend(stored: "bogus") == "auto", "graphics default AUTO")
        check(SettingsMigration.fallbackPolicy(stored: "ask") == "ask" && SettingsMigration.fallbackPolicy(stored: nil) == "auto", "fallback policy")

        // Library: search, filters, favourites first, recent by date, logs by exact id.
        let items = [LibraryItem(id: "steam", name: "Steam", architecture: "x86_64", isWindows: false),
                     LibraryItem(id: "steam-arm64", name: "Steam ARM64 (experimental)", architecture: "aarch64", isWindows: false),
                     LibraryItem(id: "heroic", name: "Heroic Games Launcher", architecture: nil, isWindows: false),
                     LibraryItem(id: "juego", name: "Juego Añejo", architecture: nil, isWindows: true)]
        var lib: [String: AppStats] = [:]
        lib["heroic", default: AppStats()].favorite = true
        lib["steam", default: AppStats()].noteLaunch(at: t0Stats)
        lib["juego", default: AppStats()].noteLaunch(at: t0Stats.addingTimeInterval(60))
        check(Library.visible(items, stats: lib, query: "", filter: .all) == ["heroic", "steam", "steam-arm64", "juego"], "favourites first")
        check(Library.visible(items, stats: lib, query: "", filter: .favorites) == ["heroic"], "favourites filter")
        check(Library.visible(items, stats: lib, query: "", filter: .recent) == ["juego", "steam"], "recent, newest first")
        check(Library.visible(items, stats: lib, query: "", filter: .arm64) == ["steam-arm64"], "ARM64 filter")
        check(Library.visible(items, stats: lib, query: "", filter: .x86) == ["heroic", "steam"], "x86 filter (nil arch is x86_64)")
        check(Library.visible(items, stats: lib, query: "", filter: .windows) == ["juego"], "Windows filter")
        check(Library.visible(items, stats: lib, query: "anejo", filter: .all) == ["juego"], "search ignores case and accents")
        check(Library.visible(items, stats: lib, query: "steam", filter: .x86) == ["steam"], "search within a filter")
        check(Library.isLog("steam-20260929-054400.log", of: "steam"), "a steam log")
        check(!Library.isLog("steam-arm64-20260929-054400.log", of: "steam"), "steam-arm64 logs are not steam's")
        check(Library.isLog("steam-arm64-20260929-054400.log", of: "steam-arm64"), "a steam-arm64 log")
        check(!Library.isLog("steam-20260929-0544.log", of: "steam") && !Library.isLog("xsteam-20260929-054400.log", of: "steam"), "exact form only")
        check(Library.newestLog(of: "steam", in: ["steam-20260928-100000.log", "steam-arm64-20260930-100000.log",
                                                  "steam-20260929-090000.log", "current"]) == "steam-20260929-090000.log", "newest log")
        check(Library.programDirectory(of: ["/bin/bash", "/tmp/fexhome/.local/share/Steam/steam.sh", "-noverifyfiles"]) == "/tmp/fexhome/.local/share/Steam", "script after a shell")
        check(Library.programDirectory(of: ["/tmp/armhome/.local/share/Steam/steamrtarm64/steam"]) == "/tmp/armhome/.local/share/Steam/steamrtarm64", "program dir")
        check(Library.programDirectory(of: ["run"]) == nil, "relative command")
        check(Library.duration(30) == "menos de 1 min" && Library.duration(720) == "12 min" && Library.duration(11_520) == "3 h 12 min", "durations")
        check(Library.subtitle(nil) == "Sin abrir todavía", "never opened")
        var sub = AppStats(); sub.noteLaunch(at: t0Stats); sub.totalRuntime = 720
        let line = Library.subtitle(sub, now: t0Stats.addingTimeInterval(3600))
        check(line.contains("1 vez") && line.contains("12 min") && line.contains("hace"), "subtitle: \(line)")

        // Heroic (linux-arm64): an aarch64 program in the Fedora ARM64 root,
        // native under lxrun, never FEX, never a VM.
        let heroicPlan = try LaunchPlanner.plan(architecture: GuestArchitecture.aarch64.rawValue,
                                                environmentID: HeroicARM64.root.id)
        check(heroicPlan.runner == .native && !heroicPlan.usesVirtualMachine, "Heroic ARM64 runs natively")
        check(HeroicARM64.root.guestRoot == "/tmp/lxrt-armroot", "Heroic lives in the Fedora ARM64 root")
        check(HeroicARM64.command() == ["/opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic", "--no-sandbox", "--disable-gpu", "--js-flags=--no-opt"],
              "Heroic command line")
        check(HeroicARM64.env["HOME_IN_GUEST"] == "/tmp/heroichome" && HeroicARM64.env["LXRT_X18_ALL_TEXT"] == "/opt/apps/heroic/",
              "Heroic environment")
        check(HeroicARM64.missing(inRoot: "/r", exists: { _ in true }).isEmpty, "a complete root lacks nothing")
        check(HeroicARM64.missing(inRoot: "/r", exists: { !$0.hasSuffix("libgtk-3.so.0") }) == ["usr/lib64/libgtk-3.so.0"],
              "a root without GTK 3")
        check(HeroicARM64.installedProgram(fromOutput: "[6/6] installing\ninstalled: /opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic\n")
              == "/opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic", "installer's last line")
        check(HeroicARM64.installedProgram(fromOutput: "installed: /usr/bin/true") == nil, "only a program under opt/apps/heroic")
        check(HeroicARM64.installedProgram(fromOutput: "[1/6] downloading") == nil, "no program without the last line")
        let prog = HeroicARM64.progress("[3/6] taking resources/ from the linux-x64 release")
        check(prog?.step == 3 && prog?.of == 6 && prog?.text == "taking resources/ from the linux-x64 release", "progress line")
        check(HeroicARM64.progress("      legendary (cached)") == nil && HeroicARM64.progress("[7/6] x") == nil, "not progress")

        // Android (docs/APK_SUPPORT.md): ABI verdicts, the inspector's JSON,
        // the card, the library filter and the reason no card can be opened.
        check(AndroidABI.verdict(abis: ["arm64-v8a", "armeabi-v7a", "x86", "x86_64"]) == .arm64, "arm64-v8a preferred")
        check(AndroidABI.verdict(abis: ["armeabi-v7a"]) == .arm32Only && AndroidABI.verdict(abis: ["armeabi"]) == .arm32Only,
              "32-bit ARM only")
        check(AndroidABI.verdict(abis: ["x86_64", "x86"]) == .x86Only, "x86 only")
        check(AndroidABI.verdict(abis: []) == AndroidABI.none, "ART only")
        check(AndroidABI.verdict(abis: ["mips"]) == .unsupported, "other ABIs")
        check(AndroidABI.arm32Only.label(abis: ["armeabi-v7a"]).contains("AArch32"), "32-bit ARM says why")
        check(AndroidABI.x86Only.label(abis: ["x86"]).contains("FEX"), "x86 says FEX")
        check(AndroidABI.arm64.architecture(abis: ["arm64-v8a"]) == "aarch64"
              && AndroidABI.none.architecture(abis: []) == "aarch64"
              && AndroidABI.arm32Only.architecture(abis: ["armeabi-v7a"]) == "armv7"
              && AndroidABI.x86Only.architecture(abis: ["x86"]) == "i386"
              && AndroidABI.x86Only.architecture(abis: ["x86", "x86_64"]) == "x86_64", "card architecture from the ABI")
        check(AndroidABI.arm64.platformLabel(abis: ["arm64-v8a"]) == "Android · ARM64", "platform chip")
        for (raw, verdict) in [("arm64", AndroidABI.arm64), ("arm32-only", .arm32Only), ("x86-only", .x86Only),
                               ("unsupported", .unsupported), ("none", AndroidABI.none)] {
            check(AndroidABI(rawValue: raw) == verdict, "verdict id \(raw) matches apk-inspect.py")
        }
        let inspectJSON = #"""
        {"format": "apk", "supported": true, "fileName": "org.fdroid.fdroid_2000050.apk", "size": 12496223,
         "package": "org.fdroid.fdroid", "versionCode": 2000050, "versionName": "2.0.0", "minSdk": 24, "targetSdk": 37,
         "maxSdk": null, "label": "F-Droid", "labelSource": "application", "launcherActivity": "org.fdroid.MainActivity",
         "permissions": [{"name": "android.permission.INTERNET"}, {"name": "android.permission.READ_EXTERNAL_STORAGE", "maxSdkVersion": 32}],
         "features": [], "glEsVersion": null, "vulkan": null, "abis": ["arm64-v8a", "armeabi-v7a", "x86", "x86_64"],
         "abiVerdict": {"id": "arm64", "abi": "arm64-v8a", "summary": "..."}, "isGame": false,
         "splits": {"split": null, "isSplit": false, "needsSplits": false},
         "signing": {"v1": true, "v2": true, "v3": true, "v31": false, "blocks": ["v2", "v3"], "certificates": ["43238d51"],
                     "certificateSource": "v3", "lineage": [], "verified": false},
         "icon": {"path": "res/o-.png", "density": 640, "format": "png", "width": 192, "height": 192, "source": "manifest"},
         "sha256": "94938d32"}
        """#
        let apk = try JSONDecoder().decode(AndroidPackageInfo.self, from: Data(inspectJSON.utf8))
        check(apk.package == "org.fdroid.fdroid" && apk.minSdk == "24" && apk.targetSdk == "37", "inspector JSON")
        check(apk.versionLabel == "2.0.0 (2000050)" && apk.signingSchemes == "v1 + v2 + v3", "version and signing labels")
        check(apk.abiVerdict == .arm64 && apk.installBlocker == nil && apk.icon?.width == 192, "installable")
        check(apk.permissions?.last?.maxSdkVersion == 32, "permission details")
        let codename = try JSONDecoder().decode(AndroidPackageInfo.self, from: Data(#"{"package": "a.b", "minSdk": "Baklava"}"#.utf8))
        check(codename.minSdk == "Baklava", "a preview codename as minSdk")
        let bundle = try JSONDecoder().decode(AndroidPackageInfo.self, from: Data(
            #"{"format": "xapk", "supported": false, "reason": "split installs are not supported yet"}"#.utf8))
        check(bundle.installBlocker?.contains("XAPK") == true, "an XAPK is refused with the reason")
        var unsigned = apk; unsigned.signing = nil
        check(unsigned.installBlocker?.contains("firmado") == true, "an unsigned APK is refused")
        var split = apk; split.splits = .init(split: "config.arm64_v8a", isSplit: true, needsSplits: false)
        check(split.installBlocker?.contains("dividido") == true, "a split APK is refused")
        check(AndroidApps.isValidPackage("org.fdroid.fdroid") && AndroidApps.isValidPackage("a.B_1"), "valid packages")
        check(!AndroidApps.isValidPackage("../x.y") && !AndroidApps.isValidPackage("single")
              && !AndroidApps.isValidPackage("a..b") && !AndroidApps.isValidPackage("a.1b") && !AndroidApps.isValidPackage("a.b/c"),
              "invalid packages")
        check(AndroidApps.entryID(package: "org.fdroid.fdroid") == "android-org.fdroid.fdroid", "card id")
        check(AndroidApps.entryID(package: "org.foo_bar") != AndroidApps.entryID(package: "org.foo.bar")
              && AndroidApps.entryID(package: "org.Foo.bar") != AndroidApps.entryID(package: "org.foo.bar"),
              "distinct packages, distinct cards")
        let pmJSON = #"""
        {"ok": true, "action": "installed", "package": "org.fdroid.fdroid", "label": "F-Droid", "versionName": "2.0.0",
         "versionCode": 2000050, "minSdk": 24, "targetSdk": 37, "abis": ["arm64-v8a", "armeabi-v7a", "x86", "x86_64"],
         "abiVerdict": "arm64", "launcherActivity": "org.fdroid.MainActivity",
         "packageDir": "/s/android/packages/org.fdroid.fdroid", "dataDir": "/s/android/data/org.fdroid.fdroid",
         "icon": "/s/android/packages/org.fdroid.fdroid/icon.png", "installedAt": "2026-09-29T19:00:00Z",
         "updatedAt": "2026-09-29T19:00:00Z", "abiSummary": "...", "previousVersion": null, "dataKept": false}
        """#
        let noise = "some warning on stderr\n" + pmJSON + "\n"
        let pmResult = try JSONDecoder().decode(AndroidPMResult.self, from: AndroidApps.jsonPayload(noise) ?? Data())
        check(pmResult.ok && pmResult.action == "installed", "android-pm.py JSON, with stderr around it")
        // The file changed between the preview and the install: the card
        // takes what was installed, never the preview's details.
        let changed = try JSONDecoder().decode(AndroidPMResult.self, from: Data(#"""
        {"ok": true, "package": "org.fdroid.fdroid", "label": "F-Droid", "versionName": "2.0.1", "versionCode": 2000060,
         "minSdk": 26, "targetSdk": "Baklava", "abis": ["x86_64"], "sha256": "ffff", "permissions": ["android.permission.CAMERA"],
         "packageDir": "/s/p", "dataDir": "/s/d"}
        """#.utf8))
        let changedCard = AndroidApps.card(result: changed, info: apk)
        check(changedCard?.info.minSdk == "26" && changedCard?.info.targetSdk == "Baklava"
              && changedCard?.info.permissions == ["android.permission.CAMERA"] && changedCard?.info.apkSha256 == "ffff"
              && changedCard?.architecture == "x86_64" && changedCard?.info.launcherActivity == nil,
              "a file replaced after the preview: the installed file's details")
        let card = AndroidApps.card(result: pmResult, info: apk)
        check(card?.id == "android-org.fdroid.fdroid" && card?.name == "F-Droid" && card?.architecture == "aarch64", "the card")
        check(card?.icon == "/s/android/packages/org.fdroid.fdroid/icon.png" && card?.installDir == "/s/android/packages/org.fdroid.fdroid",
              "card icon and folder")
        check(card?.info.dataDir == "/s/android/data/org.fdroid.fdroid" && card?.info.abiVerdict == "arm64"
              && card?.info.permissions?.count == 2 && card?.info.apkSha256 == "94938d32", "card details")
        let failed = try JSONDecoder().decode(AndroidPMResult.self, from: Data(
            #"{"ok": false, "code": "different-signer", "error": "x"}"#.utf8))
        check(AndroidApps.card(result: failed, info: apk) == nil, "no card for a refused install")
        check(AndroidApps.errorMessage(code: "different-signer", detail: nil).contains("otro certificado"), "signer error in Spanish")
        check(AndroidApps.errorMessage(code: "downgrade", detail: "x").hasSuffix("(x)."), "error detail")
        // Opening (benchmarks/stage28-android-apk.txt): the session runs dex-only
        // apps and apps with x86_64 code; arm64-v8a-only code meets the ART heap
        // wall, 32-bit ARM has no AArch32, 32-bit x86 is not run by the session.
        check(AndroidApps.unavailableReason(card?.info) == nil && AndroidApps.runsInSession(card?.info),
              "F-Droid (arm64-v8a and x86_64): opens in the session")
        func info(_ abis: [String], minSdk: String? = "21") -> AndroidAppInfo {
            AndroidAppInfo(package: "org.example.app", minSdk: minSdk, abis: abis)
        }
        check(AndroidApps.unavailableReason(info([])) == nil, "dex only: opens")
        check(AndroidApps.unavailableReason(info(["x86_64"])) == nil, "x86_64 only: opens")
        let arm64Only = AndroidApps.unavailableReason(info(["arm64-v8a"])) ?? ""
        check(arm64Only.contains("ARM64") && arm64Only.contains("4 GiB") && arm64Only.contains("ANDROID_RUNTIME_ARCHITECTURE"),
              "arm64-v8a only: disabled with the ART heap wall")
        check(AndroidApps.unavailableReason(info(["arm64-v8a", "armeabi-v7a"]))?.contains("ARM64") == true,
              "arm64-v8a and 32-bit ARM: still the heap wall")
        check(AndroidApps.unavailableReason(info(["armeabi-v7a"]))?.contains("32 bits") == true, "32-bit ARM says why")
        check(AndroidApps.unavailableReason(info(["x86"]))?.contains("x86 de 32 bits") == true, "32-bit x86 says why")
        check(AndroidApps.unavailableReason(info(["mips"]))?.contains("no admitida") == true, "other ABIs")
        check(AndroidApps.unavailableReason(info([], minSdk: "33"))?.contains("API 33") == true, "minSdk above Android 11")
        check(AndroidApps.unavailableReason(info([], minSdk: "Baklava"))?.contains("preliminar") == true, "a preview minSdk")
        check(AndroidApps.unavailableReason(AndroidAppInfo(abis: [])) != nil, "no package: disabled")
        check(AndroidApps.unavailableReason(nil) != nil, "no Android info: disabled")
        check(AndroidABI.arm64.label(abis: ["arm64-v8a", "x86_64"]).contains("FEX")
              && AndroidABI.arm64.label(abis: ["arm64-v8a"]).contains("no puede"), "ABI labels say what the session does")
        // AppEntry.android: every field optional, so a partial object decodes.
        let partialInfo = try JSONDecoder().decode(AndroidAppInfo.self, from: Data(#"{"package": "a.b"}"#.utf8))
        check(partialInfo.package == "a.b" && partialInfo.verdict == AndroidABI.none, "partial Android info decodes")
        let androidItems = items + [LibraryItem(id: "android-a.b", name: "Juego Android", architecture: "aarch64",
                                                isWindows: false, isAndroid: true)]
        check(Library.visible(androidItems, stats: [:], query: "", filter: .android) == ["android-a.b"], "Android filter")
        check(!Library.visible(androidItems, stats: [:], query: "", filter: .arm64).contains("android-a.b"),
              "Android apps are not in the ARM64 (Linux) filter")
        check(LibraryFilter.android.label == "Android", "filter label")

        print(failures == 0 ? "application core: all checks passed" : "application core: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }
}
