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
        id: "legacy-x86", name: "x86-64 Steam root (FEX)", guestRoot: "/tmp/lxrt-steamroot",
        architectures: [.x86_64, .i386], translator: .fex, fexRootfs: "/", transitional: true)
    static let arm64 = LinuxBaseEnvironment(
        id: "arm64", name: "ARM64 root (Steam Frame / Holo)", guestRoot: "/tmp/lxrt-arm64root",
        architectures: [.aarch64], translator: .none, fexRootfs: nil, transitional: false)
    static let builtIn = [arm64, legacyX86]
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

enum SynchronizationBackend: String, Codable, CaseIterable {
    case auto, wineserver, msync, fsync, esync
}

enum GraphicsBackend: String, Codable, CaseIterable {
    case auto, openGLWineD3D, vulkanMoltenVK, vulkanKosmicKrisp
}

/// What a capability is today, from this repository and its measurements --
/// never from what it is meant to become.
struct CapabilityStatus: Equatable {
    enum State: String { case ready, experimental, unsupported, unavailable }
    var state: State
    var reason: String
    var usable: Bool { state == .ready || state == .experimental }
}

/// The launcher's source of truth for which options exist, and why the rest
/// do not (docs/APPLICATION_MANAGER.md). The UI shows `unavailable` and
/// `unsupported` options disabled with their reason, or hides them.
struct RuntimeCapabilities {
    var presentation: [PresentationMode: CapabilityStatus]
    var execution: [ExecutionBackend: CapabilityStatus]
    var synchronization: [SynchronizationBackend: CapabilityStatus]
    var graphics: [GraphicsBackend: CapabilityStatus]

    /// The state of the tree as of 0.3.4. Each reason names its evidence.
    static let current = RuntimeCapabilities(
        presentation: [
            .nativeWindows: .init(state: .ready, reason: "cross-process Metal layer in native X windows (benchmarks/stage12)"),
            .vncScreenSharing: .init(state: .experimental, reason: "cannot show the Metal layer, so games cannot present through it (stage12)"),
        ],
        execution: [
            .auto: .init(state: .ready, reason: "lxrun: aarch64 directly, x86/i386 through FEX; no VM"),
            .lightningJIT: .init(state: .unavailable, reason: "no Lightning JIT exists in this repository"),
            .appleHypervisorLegacy: .init(state: .unavailable, reason: "the VM path was removed on 2026-09-27 (docs/history)"),
        ],
        synchronization: [
            .wineserver: .init(state: .ready, reason: "Wine's default, no fast path"),
            .esync: .init(state: .experimental, reason: "eventfd is implemented in lxrun (runtime/epoll_eventfd.c); not measured with games"),
            .fsync: .init(state: .unsupported, reason: "lxrun has no futex_waitv (syscall 449)"),
            .msync: .init(state: .unavailable, reason: "no MSync-capable Wine is integrated"),
        ],
        graphics: [
            .vulkanMoltenVK: .init(state: .ready, reason: "D3D9/11/12 probes through DXVK/VKD3D-Proton (benchmarks/stage14, stage16)"),
            .vulkanKosmicKrisp: .init(state: .unavailable, reason: "not integrated; the shim loads MoltenVK by path, no ICD selection yet"),
            .openGLWineD3D: .init(state: .unavailable, reason: "no host OpenGL path for guest GL (no GL thunk target on macOS)"),
        ])

    /// AUTO resolved to a concrete synchronization backend: the fastest one
    /// that is usable, else Wine's default.
    func effectiveSynchronization(_ requested: SynchronizationBackend) -> SynchronizationBackend {
        if requested != .auto { return synchronization[requested]?.usable == true ? requested : .wineserver }
        for b in [SynchronizationBackend.msync, .fsync, .esync] where synchronization[b]?.state == .ready { return b }
        return .wineserver
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
}
