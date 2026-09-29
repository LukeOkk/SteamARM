import Foundation

// The launcher's application model without UI (docs/APPLICATION_MANAGER.md):
// what a program is (its ELF), which Linux base environment runs it, how
// (natively under lxrun, or through FEX), and the life of one session. Only
// Foundation, so tests build on Linux as well (launcher/tests/ApplicationCoreTests.swift).

// MARK: - Architecture

/// ISA of a Linux program. ARM64-first: aarch64 code runs directly under
/// lxrun; x86 code is the only thing FEX is for.
enum GuestArchitecture: String, Codable, CaseIterable {
    case aarch64
    case x86_64
    case i386

    var needsTranslator: Bool { self != .aarch64 }

    /// The value AppEntry.architecture holds; nil (entries written before the
    /// field existed, Steam's x86 client) means x86-64 under FEX, as before.
    static func of(_ entry: String?) -> GuestArchitecture? {
        guard let entry else { return .x86_64 }
        return GuestArchitecture(rawValue: entry)
    }
}

/// What an ELF header says. Read from the file itself, never from its name.
struct ELFInfo: Equatable {
    var machine: UInt16          // e_machine
    var is64: Bool
    var isPIE: Bool              // ET_DYN
    var interpreter: String?     // PT_INTERP, nil for static programs
    var architecture: GuestArchitecture? {
        switch (machine, is64) {
        case (183, true): return .aarch64
        case (62, true): return .x86_64
        case (3, false): return .i386
        default: return nil      // e.g. 32-bit ARM: SteamARM cannot run it
        }
    }
}

enum ELFInspector {
    /// nil when the file is not a little-endian ELF (scripts, archives, ...).
    static func inspect(_ url: URL) -> ELFInfo? {
        guard let fh = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? fh.close() }
        guard let head = try? fh.read(upToCount: 64), head.count >= 52,
              head.prefix(4) == Data([0x7f, 0x45, 0x4c, 0x46]), head[5] == 1 else { return nil }
        let b = [UInt8](head)
        func u16(_ o: Int) -> UInt16 { UInt16(b[o]) | UInt16(b[o + 1]) << 8 }
        func u32(_ o: Int) -> UInt32 { (0..<4).reduce(0) { $0 | UInt32(b[o + $1]) << (8 * $1) } }
        func u64(_ o: Int) -> UInt64 { (0..<8).reduce(0) { $0 | UInt64(b[o + $1]) << (8 * $1) } }
        let is64 = b[4] == 2
        guard is64 || b[4] == 1, !is64 || b.count >= 64 else { return nil }
        let info = ELFInfo(machine: u16(18), is64: is64, isPIE: u16(16) == 3, interpreter: nil)
        let phoff = is64 ? u64(32) : UInt64(u32(28))
        let phentsize = Int(is64 ? u16(54) : u16(42)), phnum = Int(is64 ? u16(56) : u16(44))
        guard phnum > 0, phnum < 512, phentsize >= (is64 ? 56 : 32) else { return info }
        guard (try? fh.seek(toOffset: phoff)) != nil,
              let ph = try? fh.read(upToCount: phentsize * phnum), ph.count == phentsize * phnum
        else { return info }
        let p = [UInt8](ph)
        func pu32(_ o: Int) -> UInt32 { (0..<4).reduce(0) { $0 | UInt32(p[o + $1]) << (8 * $1) } }
        func pu64(_ o: Int) -> UInt64 { (0..<8).reduce(0) { $0 | UInt64(p[o + $1]) << (8 * $1) } }
        var out = info
        for i in 0..<phnum where pu32(i * phentsize) == 3 {        // PT_INTERP
            let o = i * phentsize
            let off = is64 ? pu64(o + 8) : UInt64(pu32(o + 4))
            let size = Int(is64 ? pu64(o + 32) : UInt64(pu32(o + 16)))
            guard size > 0, size < 4096, (try? fh.seek(toOffset: off)) != nil,
                  let s = try? fh.read(upToCount: size) else { break }
            out.interpreter = String(decoding: s.prefix { $0 != 0 }, as: UTF8.self)
            break
        }
        return out
    }
}

// MARK: - Linux base environments

/// A Linux userspace a program runs in (a guest root), and how code in it
/// reaches the CPU. Holo/Steam Frame ARM64 is the target base; the x86-64
/// Steam root under FEX is the transitional one.
struct LinuxBaseEnvironment: Codable, Equatable, Identifiable {
    enum Translator: String, Codable { case none, fex }

    var id: String
    var name: String
    var guestRoot: String                 // LXRT_ROOT
    var architectures: [GuestArchitecture]
    var translator: Translator
    var fexRootfs: String?                // FEX_ROOTFS, only with .fex
    var transitional: Bool                // TRANSITIONAL_COMPATIBILITY

    static let legacyX86 = LinuxBaseEnvironment(
        id: "legacy-x86", name: "Raíz x86-64 de Steam (FEX)", guestRoot: "/tmp/lxrt-steamroot",
        architectures: [.x86_64, .i386], translator: .fex, fexRootfs: "/", transitional: true)
    static let arm64 = LinuxBaseEnvironment(
        id: "arm64", name: "Raíz ARM64 (Steam Frame / Holo)", guestRoot: "/tmp/lxrt-arm64root",
        architectures: [.aarch64], translator: .none, fexRootfs: nil, transitional: false)
    /// The Fedora 43 aarch64 root of scripts/mkarmroot.sh, where the native
    /// arm64 Steam client runs (benchmarks/stage21). Transitional until the
    /// Holo-derived root exists; a rebuild wipes it, client included.
    static let armroot = LinuxBaseEnvironment(
        id: "armroot", name: "Raíz ARM64 (Fedora 43, scripts/mkarmroot.sh)", guestRoot: "/tmp/lxrt-armroot",
        architectures: [.aarch64], translator: .none, fexRootfs: nil, transitional: true)
    static let builtIn = [arm64, armroot, legacyX86]

    /// The environment whose guest root is `root`, if it is a known one.
    static func named(guestRoot root: String, in environments: [LinuxBaseEnvironment] = builtIn) -> LinuxBaseEnvironment? {
        let trimmed = root.count > 1 && root.hasSuffix("/") ? String(root.dropLast()) : root
        return environments.first { $0.guestRoot == trimmed }
    }
}

// MARK: - Launch plan

struct LaunchPlan: Equatable {
    enum Runner: String {
        case native = "scripts/run-native.sh"   // build/lxrun <program>
        case fex = "scripts/run-fex.sh"         // build/lxrun FEX <program>
    }
    var architecture: GuestArchitecture
    var environment: LinuxBaseEnvironment
    var runner: Runner
    /// Only Apple Hypervisor may ever set this; no runner here uses a VM.
    let usesVirtualMachine = false
    var translatorLabel: String { runner == .fex ? "FEX" : "none" }
}

enum LaunchPlanError: Error, Equatable {
    case unsupportedArchitecture(String)
    case unknownEnvironment(String)
    case environmentCannotRun(environment: String, architecture: GuestArchitecture)
    case noEnvironment(GuestArchitecture)
}

enum LaunchPlanner {
    /// ARM64 programs run natively and never through FEX; x86 programs need
    /// an environment with FEX. `environmentID` pins one; otherwise the first
    /// environment that runs the architecture is taken.
    static func plan(architecture entry: String?, environmentID: String? = nil,
                     environments: [LinuxBaseEnvironment] = LinuxBaseEnvironment.builtIn) throws -> LaunchPlan {
        guard let arch = GuestArchitecture.of(entry) else {
            throw LaunchPlanError.unsupportedArchitecture(entry ?? "")
        }
        let runs: (LinuxBaseEnvironment) -> Bool = { env in
            env.architectures.contains(arch) && (env.translator == .fex) == arch.needsTranslator
        }
        let env: LinuxBaseEnvironment
        if let environmentID {
            guard let e = environments.first(where: { $0.id == environmentID }) else {
                throw LaunchPlanError.unknownEnvironment(environmentID)
            }
            guard runs(e) else {
                throw LaunchPlanError.environmentCannotRun(environment: e.id, architecture: arch)
            }
            env = e
        } else {
            guard let e = environments.first(where: runs) else { throw LaunchPlanError.noEnvironment(arch) }
            env = e
        }
        return LaunchPlan(architecture: arch, environment: env, runner: arch.needsTranslator ? .fex : .native)
    }
}

// MARK: - Session

/// One application session: the process tree the launcher started, followed
/// from the exclusivity lock to cleanup. One main application at a time.
enum SessionPhase: String, Codable {
    case idle, starting, running, stopping, failed, exited, crashed, cleanup
}

struct SessionRecord: Equatable {
    var appID: String
    var status: Int32?          // exit status; nil when killed or unknown
    var outcome: SessionPhase   // failed, exited or crashed, or stopping for a user stop
    var duration: TimeInterval
}

enum SessionError: Error, Equatable {
    case busy(runningApp: String)
    case invalidTransition(from: SessionPhase, event: String)
}

struct SessionMachine {
    private(set) var phase: SessionPhase = .idle
    private(set) var appID: String?
    private(set) var startedAt: Date?
    private(set) var last: SessionRecord?

    var canLaunch: Bool { phase == .idle }

    /// Takes the lock before any process exists: a second launch is refused
    /// from here on, not after the first program appears.
    mutating func begin(_ id: String, at now: Date = Date()) throws {
        guard phase == .idle else { throw SessionError.busy(runningApp: appID ?? "") }
        phase = .starting
        appID = id
        startedAt = now
    }

    mutating func started() throws { try move(.starting, to: .running, "started") }

    mutating func startFailed(status: Int32?, at now: Date = Date()) throws {
        try move(.starting, to: .failed, "startFailed")
        record(status: status, outcome: .failed, now: now)
    }

    mutating func requestStop() throws {
        guard phase == .running || phase == .starting else {
            throw SessionError.invalidTransition(from: phase, event: "requestStop")
        }
        phase = .stopping
    }

    /// The session's process tree is gone. A clean exit that the user did not
    /// ask for is `exited`; a non-zero status or a signal is `crashed`.
    mutating func ended(status: Int32?, at now: Date = Date()) throws {
        switch phase {
        case .running:
            let outcome: SessionPhase = status == 0 ? .exited : .crashed
            phase = outcome
            record(status: status, outcome: outcome, now: now)
        case .stopping:
            record(status: status, outcome: .stopping, now: now)
            phase = .cleanup
        default:
            throw SessionError.invalidTransition(from: phase, event: "ended")
        }
    }

    /// failed/exited/crashed -> cleanup; cleanup -> idle (the lock is released).
    mutating func cleanedUp() throws {
        switch phase {
        case .failed, .exited, .crashed: phase = .cleanup
        case .cleanup: break
        default: throw SessionError.invalidTransition(from: phase, event: "cleanedUp")
        }
        phase = .idle
        appID = nil
        startedAt = nil
    }

    private mutating func move(_ from: SessionPhase, to: SessionPhase, _ event: String) throws {
        guard phase == from else { throw SessionError.invalidTransition(from: phase, event: event) }
        phase = to
    }

    private mutating func record(status: Int32?, outcome: SessionPhase, now: Date) {
        last = SessionRecord(appID: appID ?? "", status: status, outcome: outcome,
                             duration: startedAt.map { now.timeIntervalSince($0) } ?? 0)
    }
}

/// The files scripts/run-app.sh and scripts/session.py leave in
/// $STATE/launcher for the running session.
enum SessionFiles {
    /// running.status: "N" for exit code N, "N signal S" for a death by signal S.
    static func status(_ text: String) -> (status: Int32?, signal: Int32?) {
        let fields = text.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" }).map(String.init)
        let status = fields.first.flatMap { Int32($0) }
        let signal = fields.count == 3 && fields[1] == "signal" ? Int32(fields[2]) : nil
        return (status, signal)
    }

    /// running.arch: "<arch> <translator>", e.g. "x86_64 FEX" or "aarch64 none".
    static func arch(_ text: String) -> (architecture: GuestArchitecture?, translator: String)? {
        let fields = text.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" }).map(String.init)
        guard let first = fields.first else { return nil }
        return (GuestArchitecture(rawValue: first), fields.count > 1 ? fields[1] : "")
    }
}

/// Per-app history kept by the launcher in $STATE/launcher/library.json (not
/// in apps.json: built-ins are not stored there). Every field is optional so
/// that an older or partial file still decodes.
struct AppStats: Codable, Equatable {
    var favorite: Bool?
    var launchCount: Int?
    var totalRuntime: TimeInterval?
    var lastLaunch: Date?
    var lastStatus: Int32?
    var lastOutcome: String?     // SessionPhase raw value: failed, exited, crashed, stopping

    /// One more launch at `date` (counted when the program was started).
    mutating func noteLaunch(at date: Date) {
        launchCount = (launchCount ?? 0) + 1
        lastLaunch = date
    }

    /// The end of a session. `countRuntime` is false for a session whose start
    /// is unknown (adopted from scripts/run-steam.sh).
    mutating func noteEnd(_ record: SessionRecord, countRuntime: Bool = true) {
        if countRuntime { totalRuntime = (totalRuntime ?? 0) + max(0, record.duration) }
        lastStatus = record.status
        lastOutcome = record.outcome.rawValue
    }
}

// MARK: - Backends, session mode and capabilities

/// How a session reaches the screen. Separate from how its code executes:
/// the launcher's presets combine the two.
enum PresentationMode: String, Codable, CaseIterable {
    case nativeWindows      // X windows as macOS windows (scripts/run-x11-native.sh)
    case vncScreenSharing   // Xvnc + Screen Sharing (display mode "vnc")
}

/// How a session's code executes. Only `appleHypervisorLegacy` may run a
/// virtual machine; every other backend is ZERO-VM by definition.
enum ExecutionBackend: String, Codable, CaseIterable {
    case auto                   // lxrun: aarch64 directly, x86 through FEX
    case lightningJIT
    case appleHypervisorLegacy

    var usesVirtualMachine: Bool { self == .appleHypervisorLegacy }
}

enum SessionVirtualizationMode: String, Codable {
    case zeroVM
    case vmAppleHypervisor

    init(_ execution: ExecutionBackend) {
        self = execution.usesVirtualMachine ? .vmAppleHypervisor : .zeroVM
    }
    var label: String { self == .zeroVM ? "ZERO-VM" : "VM — Apple Hypervisor" }
}

/// The four presets the launcher can show, each a presentation + execution pair.
enum ApplicationBackendPreset: String, Codable, CaseIterable {
    case nativeWindows, vncScreenSharing, lightningJIT, appleHypervisor

    var execution: ExecutionBackend {
        switch self {
        case .nativeWindows, .vncScreenSharing: return .auto
        case .lightningJIT: return .lightningJIT
        case .appleHypervisor: return .appleHypervisorLegacy
        }
    }
    func presentation(default d: PresentationMode = .nativeWindows) -> PresentationMode {
        self == .vncScreenSharing ? .vncScreenSharing : (self == .nativeWindows ? .nativeWindows : d)
    }
}

extension PresentationMode {
    var label: String { self == .nativeWindows ? "Ventanas nativas" : "VNC (Compartir Pantalla)" }
}

extension ApplicationBackendPreset {
    var label: String {
        switch self {
        case .nativeWindows: return "Ventanas nativas (recomendado)"
        case .vncScreenSharing: return "VNC (Compartir Pantalla)"
        case .lightningJIT: return "Lightning JIT"
        case .appleHypervisor: return "Apple Hypervisor"
        }
    }
}

enum SynchronizationBackend: String, Codable, CaseIterable {
    case auto, wineserver, msync, fsync, esync

    var label: String {
        switch self {
        case .auto: return "AUTO"
        case .wineserver: return "DEFAULT (wineserver)"
        case .msync: return "MSYNC"
        case .fsync: return "FSYNC"
        case .esync: return "ESYNC"
        }
    }
}

enum GraphicsBackend: String, Codable, CaseIterable {
    case auto, openGLWineD3D, vulkanMoltenVK, vulkanKosmicKrisp

    var label: String {
        switch self {
        case .auto: return "AUTO"
        case .openGLWineD3D: return "OpenGL (WineD3D)"
        case .vulkanMoltenVK: return "Vulkan (MoltenVK)"
        case .vulkanKosmicKrisp: return "Vulkan (KosmicKrisp)"
        }
    }
}

/// What a launch does when a setting it asks for cannot work (settings key
/// "fallbackPolicy"). Only reached when there is such a setting: VNC for a
/// program outside the x86 root, or a stored value that stopped being usable.
enum FallbackPolicy: String, Codable, CaseIterable {
    case auto, strict, ask

    var label: String {
        switch self {
        case .auto: return "AUTO (usar la alternativa)"
        case .strict: return "ESTRICTO (no abrir)"
        case .ask: return "PREGUNTAR"
        }
    }

    enum Decision: Equatable { case proceed, refuse, ask }

    func decision(for issues: [CapabilityIssue]) -> Decision {
        guard !issues.isEmpty else { return .proceed }
        switch self {
        case .auto: return .proceed
        case .strict: return .refuse
        case .ask: return .ask
        }
    }
}

/// What a capability is today, from this repository and its measurements --
/// never from what it is meant to become.
struct CapabilityStatus: Equatable {
    enum State: String {
        case ready, experimental, unsupported, unavailable

        var label: String {
            switch self {
            case .ready: return "Listo"
            case .experimental: return "Experimental"
            case .unsupported: return "No soportado"
            case .unavailable: return "No disponible"
            }
        }
    }
    var state: State
    var reason: String
    var usable: Bool { state == .ready || state == .experimental }
}

/// One requested setting that cannot work for this launch, and what runs instead.
struct CapabilityIssue: Equatable {
    var setting: String      // settings key: display, synchronization, graphicsBackend
    var requested: String    // raw values, as stored
    var fallback: String
    var summary: String      // for the user, in Spanish
}

/// What scripts/compat-status.py --json found on this Mac (the subset the
/// capability table needs). Read in a subprocess so no driver is loaded into
/// the launcher itself.
struct RuntimeProbe: Decodable, Equatable {
    struct Driver: Decodable, Equatable { var path: String; var version: String }
    struct KosmicKrisp: Decodable, Equatable {
        var icdJSON: String
        var library: String
        var version: String
        var osOK: Bool
        var exportsICD: Bool
        enum CodingKeys: String, CodingKey {
            case icdJSON = "icd_json", library, version, osOK = "os_ok", exportsICD = "exports_icd"
        }
        init(icdJSON: String = "", library: String = "", version: String = "", osOK: Bool = false, exportsICD: Bool = false) {
            (self.icdJSON, self.library, self.version, self.osOK, self.exportsICD) = (icdJSON, library, version, osOK, exportsICD)
        }
        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            icdJSON = try c.decodeIfPresent(String.self, forKey: .icdJSON) ?? ""
            library = try c.decodeIfPresent(String.self, forKey: .library) ?? ""
            version = try c.decodeIfPresent(String.self, forKey: .version) ?? ""
            osOK = try c.decodeIfPresent(Bool.self, forKey: .osOK) ?? false
            exportsICD = try c.decodeIfPresent(Bool.self, forKey: .exportsICD) ?? false
        }
    }
    struct Shim: Decodable, Equatable {
        var path: String
        var installed: Bool
        var icdSelection: Bool
        enum CodingKeys: String, CodingKey { case path, installed, icdSelection = "icd_selection" }
    }
    struct Proton: Decodable, Equatable {
        var name: String
        var supported: Bool
        var esync: Bool
        var fsync: Bool
        var ntsync: Bool
        enum CodingKeys: String, CodingKey { case name, supported, esync, fsync, ntsync }
        init(name: String, supported: Bool = true, esync: Bool = false, fsync: Bool = false, ntsync: Bool = false) {
            (self.name, self.supported, self.esync, self.fsync, self.ntsync) = (name, supported, esync, fsync, ntsync)
        }
        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            name = try c.decode(String.self, forKey: .name)
            supported = try c.decodeIfPresent(Bool.self, forKey: .supported) ?? false
            esync = try c.decodeIfPresent(Bool.self, forKey: .esync) ?? false
            fsync = try c.decodeIfPresent(Bool.self, forKey: .fsync) ?? false
            ntsync = try c.decodeIfPresent(Bool.self, forKey: .ntsync) ?? false
        }
    }
    struct Presentation: Decodable, Equatable {
        var nativeX: Bool
        var xvnc: Bool
        var screenSharing: Bool
        enum CodingKeys: String, CodingKey { case nativeX = "native_x", xvnc, screenSharing = "screen_sharing" }
    }

    var moltenvk: Driver
    var kosmickrisp: KosmicKrisp?
    var shim: Shim?
    var protons: [Proton]
    var presentation: Presentation?
}

/// The launcher's source of truth for which options exist, and why the rest
/// do not (docs/APPLICATION_MANAGER.md). The UI shows `unavailable` and
/// `unsupported` options disabled with their reason, or hides them.
struct RuntimeCapabilities {
    var presentation: [PresentationMode: CapabilityStatus]
    var execution: [ExecutionBackend: CapabilityStatus]
    var synchronization: [SynchronizationBackend: CapabilityStatus]
    var graphics: [GraphicsBackend: CapabilityStatus]

    /// The state of the tree as of 0.3.4, before anything is detected on this
    /// Mac (detect(from:) refines it). Each reason names its evidence.
    static let current = RuntimeCapabilities(
        presentation: [
            .nativeWindows: .init(state: .ready, reason: "capa Metal entre procesos en ventanas X nativas (benchmarks/stage12)"),
            .vncScreenSharing: .init(state: .experimental, reason: "sirve para la interfaz de Steam; Xvnc no muestra la capa Metal, así que los juegos Vulkan no pueden presentar ahí (stage12)"),
        ],
        execution: [
            .auto: .init(state: .ready, reason: "lxrun: aarch64 directamente, x86/i386 con FEX; sin máquina virtual"),
            .lightningJIT: .init(state: .unavailable, reason: "no existe ningún Lightning JIT en este repositorio"),
            .appleHypervisorLegacy: .init(state: .unavailable, reason: "la ruta con máquina virtual se retiró el 2026-09-27 (docs/history); SteamARM es ZERO-VM"),
        ],
        synchronization: [
            .wineserver: .init(state: .ready, reason: "el camino por defecto de Wine, sin vía rápida"),
            .esync: .init(state: .experimental, reason: "solo Proton 10.0 lo incluye; lxrun implementa eventfd (runtime/epoll_eventfd.c) pero no entre procesos: sin verificar con juegos"),
            .fsync: .init(state: .unsupported, reason: "lxrun no implementa futex_waitv (syscall 449): Proton lo prueba, recibe ENOSYS y no lo usa"),
            .msync: .init(state: .unavailable, reason: "no hay ningún Wine con MSync integrado (MSync es un parche del Wine de macOS, no del Proton de Linux)"),
        ],
        graphics: [
            .vulkanMoltenVK: .init(state: .ready, reason: "D3D9/11/12 por DXVK/VKD3D-Proton (benchmarks/stage14, stage16)"),
            .vulkanKosmicKrisp: .init(state: .unavailable, reason: "sin detectar todavía; el shim carga MoltenVK por ruta"),
            .openGLWineD3D: .init(state: .unsupported, reason: "el OpenGL del invitado es llvmpipe por software y no hay thunk de GL"),
        ])

    /// The static table refined with what is installed on this Mac. Nothing
    /// becomes more than the table allows: fsync, MSync, WineD3D, Lightning
    /// JIT and Apple Hypervisor stay what they are whatever is installed.
    static func detect(from probe: RuntimeProbe, base: RuntimeCapabilities = .current) -> RuntimeCapabilities {
        var caps = base

        if probe.moltenvk.path.isEmpty {
            caps.graphics[.vulkanMoltenVK] = .init(state: .unavailable, reason: "MoltenVK no está instalado (brew install molten-vk)")
        } else {
            caps.graphics[.vulkanMoltenVK] = .init(state: .ready, reason: "MoltenVK \(probe.moltenvk.version): D3D9/11/12 por DXVK/VKD3D-Proton (benchmarks/stage14, stage16)")
        }

        let kk = probe.kosmickrisp ?? .init()
        let kkName = kk.version.isEmpty ? "KosmicKrisp" : "KosmicKrisp \(kk.version)"
        if kk.library.isEmpty {
            caps.graphics[.vulkanKosmicKrisp] = .init(state: .unavailable, reason: "KosmicKrisp no está instalado (Mesa de Homebrew)")
        } else if !kk.osOK {
            caps.graphics[.vulkanKosmicKrisp] = .init(state: .unavailable, reason: "\(kkName) necesita macOS 26 o posterior (Metal 4)")
        } else if !kk.exportsICD {
            caps.graphics[.vulkanKosmicKrisp] = .init(state: .unavailable, reason: "\(kkName) no carga o no exporta vk_icdGetInstanceProcAddr")
        } else if probe.shim?.icdSelection != true {
            caps.graphics[.vulkanKosmicKrisp] = .init(state: .unavailable, reason: "\(kkName) está instalado, pero el shim Vulkan instalado no admite STEAMARM_VK_ICD: solo carga MoltenVK")
        } else {
            caps.graphics[.vulkanKosmicKrisp] = .init(state: .experimental, reason: "\(kkName) detectado y el shim admite STEAMARM_VK_ICD; sin medir con juegos")
        }

        let esync = probe.protons.filter { $0.supported && $0.esync }.map(\.name)
        if esync.isEmpty {
            caps.synchronization[.esync] = .init(state: .unavailable, reason: "ningún Proton instalado incluye esync (Proton Experimental y Hotfix no lo compilan)")
        } else {
            caps.synchronization[.esync] = .init(state: .experimental, reason: "solo en \(esync.joined(separator: ", ")); lxrun implementa eventfd pero no entre procesos: sin verificar con juegos")
        }

        if let p = probe.presentation {
            if !p.nativeX {
                caps.presentation[.nativeWindows] = .init(state: .unavailable, reason: "falta el servidor X nativo (scripts/build-xquartz.sh)")
            }
            if !p.xvnc || !p.screenSharing {
                let missing = !p.xvnc ? "Xvnc no está en la raíz x86" : "no se encuentra Compartir Pantalla"
                caps.presentation[.vncScreenSharing] = .init(state: .unavailable, reason: missing)
            }
        }
        return caps
    }

    /// The state of one of the launcher's backend presets.
    func status(of preset: ApplicationBackendPreset) -> CapabilityStatus {
        let missing = CapabilityStatus(state: .unavailable, reason: "sin datos")
        switch preset {
        case .nativeWindows: return presentation[.nativeWindows] ?? missing
        case .vncScreenSharing: return presentation[.vncScreenSharing] ?? missing
        case .lightningJIT: return execution[.lightningJIT] ?? missing
        case .appleHypervisor: return execution[.appleHypervisorLegacy] ?? missing
        }
    }

    /// AUTO resolved to a concrete synchronization backend: the fastest one
    /// that is usable, else Wine's default.
    func effectiveSynchronization(_ requested: SynchronizationBackend) -> SynchronizationBackend {
        if requested != .auto { return synchronization[requested]?.usable == true ? requested : .wineserver }
        for b in [SynchronizationBackend.msync, .fsync, .esync] where synchronization[b]?.state == .ready { return b }
        return .wineserver
    }

    /// AUTO resolved to a concrete graphics backend: the first ready one
    /// (MoltenVK today). A requested one that is not usable falls back.
    func effectiveGraphics(_ requested: GraphicsBackend) -> GraphicsBackend {
        if requested == .auto {
            return [GraphicsBackend.vulkanMoltenVK, .vulkanKosmicKrisp, .openGLWineD3D]
                .first { graphics[$0]?.state == .ready } ?? .vulkanMoltenVK
        }
        if graphics[requested]?.usable == true { return requested }
        return graphicsFallback(after: requested) ?? .vulkanMoltenVK
    }

    /// The next graphics backend to try after `failed`, when fallback is
    /// allowed. KosmicKrisp -> MoltenVK -> WineD3D; never a VM.
    func graphicsFallback(after failed: GraphicsBackend) -> GraphicsBackend? {
        let order: [GraphicsBackend] = [.vulkanKosmicKrisp, .vulkanMoltenVK, .openGLWineD3D]
        guard let i = order.firstIndex(of: failed) else { return order.first { graphics[$0]?.usable == true } }
        return order[(i + 1)...].first { graphics[$0]?.usable == true }
    }

    /// Execution fallback after a ZERO-VM failure. Apple Hypervisor is never
    /// an automatic fallback: it needs the user's explicit choice.
    func executionFallback(after failed: ExecutionBackend) -> ExecutionBackend? {
        failed == .lightningJIT && execution[.auto]?.usable == true ? .auto : nil
    }

    /// The settings of one launch that cannot work as asked, each with what
    /// runs instead. scripts/run-app.sh and scripts/settings-env.py apply the
    /// same fallbacks by themselves; FallbackPolicy decides whether to launch.
    func issues(display: PresentationMode, synchronization sync: SynchronizationBackend,
                graphics gfx: GraphicsBackend, appInX86Root: Bool) -> [CapabilityIssue] {
        var out: [CapabilityIssue] = []
        if display == .vncScreenSharing {
            let reason: String? = !appInX86Root
                ? "Xvnc se ejecuta dentro de la raíz x86 de Steam: un programa de otra raíz no encuentra la pantalla :1"
                : (presentation[.vncScreenSharing]?.usable == false ? presentation[.vncScreenSharing]?.reason : nil)
            if let reason {
                out.append(.init(setting: "display", requested: "vnc", fallback: "native",
                                 summary: "Pantalla: \(PresentationMode.vncScreenSharing.label) → \(PresentationMode.nativeWindows.label) (\(reason))"))
            }
        }
        let effSync = effectiveSynchronization(sync)
        if sync != .auto && effSync != sync {
            let reason = synchronization[sync]?.reason ?? "no disponible"
            out.append(.init(setting: "synchronization", requested: sync.rawValue, fallback: effSync.rawValue,
                             summary: "Sincronización: \(sync.label) → \(effSync.label) (\(reason))"))
        }
        let effGfx = effectiveGraphics(gfx)
        if gfx != .auto && effGfx != gfx {
            let reason = graphics[gfx]?.reason ?? "no disponible"
            out.append(.init(setting: "graphicsBackend", requested: gfx.rawValue, fallback: effGfx.rawValue,
                             summary: "Gráficos: \(gfx.label) → \(effGfx.label) (\(reason))"))
        }
        return out
    }
}

/// settings.json values written before the selectors existed, read so that
/// what ran before keeps running (the Proton environment included).
enum SettingsMigration {
    /// The esync/fsync booleans became one "synchronization" choice. fsync
    /// never worked under lxrun (no futex_waitv), so esync true -> "esync",
    /// false -> Wine's default; no booleans at all (a new install) -> AUTO.
    static func synchronization(stored: String?, esync: Bool?, fsync: Bool?) -> String {
        if let stored, SynchronizationBackend(rawValue: stored) != nil { return stored }
        if esync != nil || fsync != nil {
            return (esync == false ? SynchronizationBackend.wineserver : .esync).rawValue
        }
        return SynchronizationBackend.auto.rawValue
    }

    /// "vulkan" was the only value (MoltenVK); nothing stored is AUTO.
    static func graphicsBackend(stored: String?) -> String {
        guard let stored else { return GraphicsBackend.auto.rawValue }
        if stored == "vulkan" { return GraphicsBackend.vulkanMoltenVK.rawValue }
        return GraphicsBackend(rawValue: stored) != nil ? stored : GraphicsBackend.auto.rawValue
    }

    static func fallbackPolicy(stored: String?) -> String {
        stored.flatMap { FallbackPolicy(rawValue: $0) }?.rawValue ?? FallbackPolicy.auto.rawValue
    }
}

// MARK: - Library

/// The library's filter chips.
enum LibraryFilter: String, CaseIterable, Identifiable {
    case all, favorites, recent, arm64, x86, windows
    var id: String { rawValue }
    var label: String {
        switch self {
        case .all: return "Todas"
        case .favorites: return "Favoritas"
        case .recent: return "Recientes"
        case .arm64: return "ARM64"
        case .x86: return "x86"
        case .windows: return "Windows"
        }
    }
}

/// What the library needs to know of an entry to sort and filter it.
struct LibraryItem: Equatable {
    var id: String
    var name: String
    var architecture: String?   // AppEntry.architecture
    var isWindows: Bool
}

enum Library {
    /// The ids to show, in order: favourites first (the library's order
    /// otherwise); Recientes is newest launch first.
    static func visible(_ items: [LibraryItem], stats: [String: AppStats], query: String,
                        filter: LibraryFilter) -> [String] {
        let q = query.trimmingCharacters(in: .whitespacesAndNewlines)
        let opts: String.CompareOptions = [.caseInsensitive, .diacriticInsensitive]
        let matching = items.filter { item in
            guard q.isEmpty || item.name.range(of: q, options: opts) != nil
                    || item.id.range(of: q, options: opts) != nil else { return false }
            let arch = GuestArchitecture.of(item.architecture)
            switch filter {
            case .all: return true
            case .favorites: return stats[item.id]?.favorite == true
            case .recent: return stats[item.id]?.lastLaunch != nil
            case .arm64: return !item.isWindows && arch == .aarch64
            case .x86: return !item.isWindows && (arch == .x86_64 || arch == .i386)
            case .windows: return item.isWindows
            }
        }
        if filter == .recent {
            return matching.sorted {
                (stats[$0.id]?.lastLaunch ?? .distantPast) > (stats[$1.id]?.lastLaunch ?? .distantPast)
            }.map(\.id)
        }
        let fav = matching.filter { stats[$0.id]?.favorite == true }
        return (fav + matching.filter { stats[$0.id]?.favorite != true }).map(\.id)
    }

    /// run-app.sh names logs <id>-YYYYmmdd-HHMMSS.log; the exact form keeps
    /// "steam" from matching "steam-arm64-...".
    static func isLog(_ fileName: String, of id: String) -> Bool {
        let pattern = "^" + NSRegularExpression.escapedPattern(for: id) + #"-\d{8}-\d{6}\.log$"#
        return fileName.range(of: pattern, options: .regularExpression) != nil
    }

    /// The newest of an app's logs among `fileNames` (the timestamp sorts).
    static func newestLog(of id: String, in fileNames: [String]) -> String? {
        fileNames.filter { isLog($0, of: id) }.max()
    }

    /// The directory of the program a guest command line runs (the script
    /// after a shell or env), as a guest path.
    static func programDirectory(of command: [String]) -> String? {
        let wrappers: Set<String> = ["/bin/bash", "/bin/sh", "/usr/bin/bash", "/usr/bin/sh", "/usr/bin/env"]
        guard let program = command.first(where: { $0.hasPrefix("/") && !wrappers.contains($0) }) else { return nil }
        let dir = (program as NSString).deletingLastPathComponent
        return dir.isEmpty ? "/" : dir
    }

    /// "3 h 12 min", "12 min", "menos de 1 min".
    static func duration(_ seconds: TimeInterval) -> String {
        let m = Int(seconds / 60)
        if m < 1 { return "menos de 1 min" }
        return m >= 60 ? "\(m / 60) h \(m % 60) min" : "\(m) min"
    }

    /// The card's second line: last launch, count and time used.
    static func subtitle(_ stats: AppStats?, now: Date = Date()) -> String {
        guard let s = stats, let last = s.lastLaunch, let n = s.launchCount, n > 0 else { return "Sin abrir todavía" }
        let f = RelativeDateTimeFormatter()
        f.locale = Locale(identifier: "es_ES")
        f.unitsStyle = .full
        var parts = [f.localizedString(for: last, relativeTo: now), n == 1 ? "1 vez" : "\(n) veces"]
        if let t = s.totalRuntime, t > 0 { parts.append(duration(t)) }
        return parts.joined(separator: " · ")
    }
}
