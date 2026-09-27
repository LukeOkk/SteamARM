import SwiftUI

enum ControllerType: String, CaseIterable, Identifiable {
    case xbox360, xboxone, xboxseries, xboxelite2, ds3, ds4, dualsense, dualsenseedge, steamcontroller,
         steamcontroller2, switchpro
    var id: String { rawValue }
    var name: String {
        switch self {
        case .xbox360: return "Xbox 360"
        case .xboxone: return "Xbox One"
        case .xboxseries: return "Xbox Series X|S"
        case .xboxelite2: return "Xbox Elite Series 2"
        case .ds3: return "DualShock 3 (PS3)"
        case .ds4: return "DualShock 4 (PS4)"
        case .dualsense: return "DualSense (PS5)"
        case .dualsenseedge: return "DualSense Edge (PS5)"
        case .steamcontroller: return "Steam Controller (2015)"
        case .steamcontroller2: return "Steam Controller (2026)"
        case .switchpro: return "Nintendo Switch Pro"
        }
    }
    var playStation: Bool { [.ds3, .ds4, .dualsense, .dualsenseedge].contains(self) }
    var hasLED: Bool { [.ds4, .dualsense, .dualsenseedge].contains(self) }
    var hasMotion: Bool { hasLED || self == .switchpro }
    var dualTone: Bool { self == .dualsense || self == .dualsenseedge }
    var paddles: Bool { [.dualsenseedge, .xboxelite2, .steamcontroller, .steamcontroller2].contains(self) }
    var trackpads: Bool { self == .steamcontroller || self == .steamcontroller2 }
    var valve: Bool { trackpads }
    func label(_ slot: PadSlot) -> String {
        let face: [PadSlot: String] = playStation ? [.a: "✕", .b: "○", .x: "□", .y: "△"]
            : self == .switchpro ? [.a: "A", .b: "B", .x: "Y", .y: "X"] : [.a: "A", .b: "B", .x: "X", .y: "Y"]
        if let label = face[slot] { return label }
        switch slot {
        case .l: return playStation ? "L1" : self == .switchpro ? "L" : "LB"
        case .r: return playStation ? "R1" : self == .switchpro ? "R" : "RB"
        case .zl: return playStation ? "L2" : self == .switchpro ? "ZL" : "LT"
        case .zr: return playStation ? "R2" : self == .switchpro ? "ZR" : "RT"
        case .minus: return playStation ? (dualTone ? "Create" : self == .ds3 ? "Select" : "Share")
            : self == .switchpro ? "−" : self == .steamcontroller || self == .xbox360 ? "Back" : "View"
        case .plus: return playStation ? (self == .ds3 ? "Start" : "Options")
            : self == .switchpro ? "+" : self == .steamcontroller || self == .xbox360 ? "Start" : "Menu"
        case .leftGrip: return self == .steamcontroller2 ? "L4" : "Palanca izq."
        case .rightGrip: return self == .steamcontroller2 ? "R4" : "Palanca der."
        case .leftGrip2: return "L5"
        case .rightGrip2: return "R5"
        case .leftPad: return "Panel izq."
        case .rightPad: return "Panel der."
        default: return slot.label(xbox: !playStation && self != .switchpro)
        }
    }
}

// All artwork and hit regions share a 600 × 380 design space.
// System-only controls are decorative: PadSlot deliberately has no Guide/Home slot.
struct ControllerDrawing: View {
    @EnvironmentObject var pads: ControllerManager
    let type: ControllerType

    var body: some View {
        VStack(spacing: 8) {
            GeometryReader { geometry in
                let scale = min(geometry.size.width / 600, geometry.size.height / 380)
                let origin = CGPoint(x: (geometry.size.width - 600 * scale) / 2,
                                     y: (geometry.size.height - 380 * scale) / 2)
                ZStack(alignment: .topLeading) {
                    Canvas { context, _ in
                        context.translateBy(x: origin.x, y: origin.y)
                        context.scaleBy(x: scale, y: scale)
                        drawShell(&context)
                        for item in controls { drawControl(item, &context) }
                    }.allowsHitTesting(false)
                    ForEach(controls) { item in
                        Button { pads.capturing = item.slot } label: {
                            Color.clear.contentShape(controlPath(item).applying(
                                CGAffineTransform(translationX: -item.rect.minX, y: -item.rect.minY)
                                    .concatenating(CGAffineTransform(scaleX: scale, y: scale))))
                        }
                        .buttonStyle(.plain)
                        .frame(width: item.rect.width * scale, height: item.rect.height * scale)
                        .position(x: origin.x + item.rect.midX * scale, y: origin.y + item.rect.midY * scale)
                        .help(item.title + " · " + type.label(item.slot))
                        .accessibilityLabel(item.title + " · " + type.label(item.slot))
                        .accessibilityIdentifier("controller." + item.id)
                    }
                }
            }.aspectRatio(600 / 380, contentMode: .fit)
            Text(type.name).font(.headline)
            Text("Pulsa un control del dibujo para reasignarlo.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .padding(12)
        .background(Color(red: 0.075, green: 0.085, blue: 0.105), in: RoundedRectangle(cornerRadius: 12))
    }

    struct Control: Identifiable {
        let id: String
        let slot: PadSlot
        let title: String
        let rect: CGRect
        let radius: CGFloat
        let kind: Kind
        enum Kind { case button, stick, stickClick, pad, direction, paddle, shoulder }
    }

    // Exposed internally so the render harness can audit mapping coverage and bounds.
    var controls: [Control] {
        var result: [Control] = []
        func add(_ slot: PadSlot, _ x: CGFloat, _ y: CGFloat, _ w: CGFloat = 28,
                 _ h: CGFloat = 28, _ title: String? = nil, _ kind: Control.Kind = .button,
                 _ suffix: String = "") {
            result.append(Control(id: slot.rawValue + suffix, slot: slot, title: title ?? type.label(slot),
                                  rect: CGRect(x: x - w / 2, y: y - h / 2, width: w, height: h),
                                  radius: kind == .stick || kind == .stickClick || kind == .pad || (w == 28 && h == 28) ? w / 2 : min(w, h) * 0.38, kind: kind))
        }
        for (slot, x) in [(PadSlot.zl, CGFloat(160)), (.zr, 440)] { add(slot, x, 47, 65, 36, nil, .shoulder) }
        for (slot, x) in [(PadSlot.l, CGFloat(160)), (.r, 440)] { add(slot, x, 78, 106, 25, nil, .shoulder) }
        if type == .steamcontroller2 { return controls2026(result) }
        let steam = type == .steamcontroller
        let ps = type.playStation
        let menuY: CGFloat = type.hasLED ? 112 : steam ? 174 : type == .ds3 ? 170 : 128
        let menuX: CGFloat = type.hasLED ? 198 : 258
        add(.minus, menuX, menuY, type.hasLED ? 32 : 40, 19)
        add(.plus, 600 - menuX, menuY, type.hasLED ? 32 : 40, 19)
        if steam {
            add(.leftPad, 157, 143, 112, 112, "", .pad)
            add(.rightPad, 443, 143, 112, 112, "", .pad)
        }
        let dx: CGFloat = ps ? 152 : steam ? 157 : 232
        let dy: CGFloat = ps ? 154 : steam ? 143 : 220
        let distance: CGFloat = ps ? 23 : 17
        for (slot, x, y, label) in [(PadSlot.dpadUp, dx, dy - distance, "▲"),
                                  (.dpadDown, dx, dy + distance, "▼"),
                                  (.dpadLeft, dx - distance, dy, "◀"), (.dpadRight, dx + distance, dy, "▶")] {
            let vertical = slot == .dpadUp || slot == .dpadDown
            add(slot, x, y, ps ? 22 : vertical ? 22 : 34,
                ps ? 27 : vertical ? 34 : 22, label, .direction)
        }
        let fx: CGFloat = steam ? 409 : 451
        let fy: CGFloat = steam ? 239 : 150
        add(.a, type == .switchpro ? fx + 30 : fx, type == .switchpro ? fy : fy + 30)
        add(.b, type == .switchpro ? fx : fx + 30, type == .switchpro ? fy + 30 : fy)
        add(.x, fx - 30, fy); add(.y, fx, fy - 30)
        let lx: CGFloat = steam ? 243 : ps ? 238 : 151
        let ly: CGFloat = steam ? 241 : ps ? 234 : 151
        add(.leftStick, lx, ly, 66, 66, "", .stick)
        add(.l3, lx, ly, 36, 36, "L3", .stickClick)
        if !steam {
            add(.rightStick, 362, 234, 66, 66, "", .stick)
            add(.r3, 362, 234, 36, 36, "R3", .stickClick)
        }
        if type.paddles {
            add(.leftGrip, 95, 327, 48, 22, type == .xboxelite2 ? "P1" : "GL", .paddle)
            add(.rightGrip, 505, 327, 48, 22, type == .xboxelite2 ? "P2" : "GR", .paddle)

        }
        return result
    }

    // Steam Controller (2026): D-pad and ABXY at the top, symmetric sticks
    // with a square trackpad under each, View/Steam/Menu and the quick access
    // button in the centre, four rear grips (L4 R4 upper, L5 R5 lower).
    private func controls2026(_ shoulders: [Control]) -> [Control] {
        var result = shoulders
        func add(_ slot: PadSlot, _ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat,
                 _ title: String?, _ kind: Control.Kind, _ radius: CGFloat) {
            result.append(Control(id: slot.rawValue, slot: slot, title: title ?? type.label(slot),
                                  rect: CGRect(x: x - w / 2, y: y - h / 2, width: w, height: h),
                                  radius: radius, kind: kind))
        }
        add(.minus, 246, 118, 34, 19, nil, .button, 7)
        add(.plus, 354, 118, 34, 19, nil, .button, 7)
        let dx: CGFloat = 132, dy: CGFloat = 146
        for (slot, x, y) in [(PadSlot.dpadUp, dx, dy - 17), (.dpadDown, dx, dy + 17),
                             (.dpadLeft, dx - 17, dy), (.dpadRight, dx + 17, dy)] {
            let vertical = slot == .dpadUp || slot == .dpadDown
            add(slot, x, y, vertical ? 22 : 34, vertical ? 34 : 22, "", .direction, 2)
        }
        let fx: CGFloat = 468, fy: CGFloat = 146
        add(.a, fx, fy + 30, 28, 28, nil, .button, 14)
        add(.b, fx + 30, fy, 28, 28, nil, .button, 14)
        add(.x, fx - 30, fy, 28, 28, nil, .button, 14)
        add(.y, fx, fy - 30, 28, 28, nil, .button, 14)
        for (stick, click, x) in [(PadSlot.leftStick, PadSlot.l3, CGFloat(222)), (.rightStick, .r3, 378)] {
            add(stick, x, 196, 64, 64, "", .stick, 32)
            add(click, x, 196, 34, 34, click == .l3 ? "L3" : "R3", .stickClick, 17)
        }
        add(.leftPad, 222, 283, 70, 70, "", .pad, 13)
        add(.rightPad, 378, 283, 70, 70, "", .pad, 13)
        add(.leftGrip, 104, 318, 46, 20, nil, .paddle, 8)
        add(.rightGrip, 496, 318, 46, 20, nil, .paddle, 8)
        add(.leftGrip2, 128, 345, 46, 20, nil, .paddle, 8)
        add(.rightGrip2, 472, 345, 46, 20, nil, .paddle, 8)
        return result
    }

    private var led: Color {
        let hex = (pads.current.ledColor ?? "#0080FF").trimmingCharacters(in: CharacterSet(charactersIn: "#"))
        let rgb = UInt32(hex, radix: 16) ?? 0x0080FF
        return Color(red: Double((rgb >> 16) & 255) / 255,
                     green: Double((rgb >> 8) & 255) / 255, blue: Double(rgb & 255) / 255)
    }

    private func text(_ value: String, _ x: CGFloat, _ y: CGFloat, _ size: CGFloat,
                      _ context: inout GraphicsContext, color: Color = .white) {
        context.draw(Text(value).font(.system(size: size, weight: .semibold, design: .rounded))
            .foregroundColor(color), at: CGPoint(x: x, y: y))
    }

    private func plate(_ rect: CGRect, _ radius: CGFloat, _ context: inout GraphicsContext,
                       top: Color = Color(white: 0.23), bottom: Color = Color(white: 0.07),
                       rim: Color = Color(white: 0.35)) {
        let path = Path(roundedRect: rect, cornerRadius: radius)
        context.fill(path, with: .linearGradient(Gradient(colors: [top, bottom]),
            startPoint: CGPoint(x: rect.minX, y: rect.minY), endPoint: CGPoint(x: rect.maxX, y: rect.maxY)))
        context.stroke(path, with: .color(rim), lineWidth: 1.2)
    }

    // One half of each shell, mirrored about the centre. Curves describe actual
    // grip profiles; Xbox revisions share tooling, while the other families do not.
    private func silhouette() -> Path {
        var p = Path()
        p.move(to: CGPoint(x: 300, y: type == .xbox360 ? 90 : 77))
        switch type {
        case .xbox360:
            p.addCurve(to: CGPoint(x: 116, y: 86), control1: CGPoint(x: 222, y: 90), control2: CGPoint(x: 173, y: 54))
            p.addCurve(to: CGPoint(x: 61, y: 301), control1: CGPoint(x: 67, y: 117), control2: CGPoint(x: 49, y: 239))
            p.addCurve(to: CGPoint(x: 145, y: 332), control1: CGPoint(x: 65, y: 353), control2: CGPoint(x: 115, y: 360))
            p.addCurve(to: CGPoint(x: 233, y: 255), control1: CGPoint(x: 181, y: 303), control2: CGPoint(x: 179, y: 260))
        case .xboxone, .xboxseries, .xboxelite2:
            p.addLine(to: CGPoint(x: 156, y: type == .xboxseries ? 76 : 82))
            p.addCurve(to: CGPoint(x: 94, y: 139), control1: CGPoint(x: 114, y: 78), control2: CGPoint(x: 101, y: 104))
            p.addLine(to: CGPoint(x: 62, y: 299))
            p.addCurve(to: CGPoint(x: 137, y: 339), control1: CGPoint(x: 51, y: 350), control2: CGPoint(x: 109, y: 357))
            p.addLine(to: CGPoint(x: 212, y: 263))
            p.addQuadCurve(to: CGPoint(x: 245, y: 254), control: CGPoint(x: 227, y: 250))
        case .ds3:
            p.addLine(to: CGPoint(x: 166, y: 84))
            p.addCurve(to: CGPoint(x: 88, y: 148), control1: CGPoint(x: 112, y: 73), control2: CGPoint(x: 83, y: 107))
            p.addLine(to: CGPoint(x: 73, y: 312))
            p.addCurve(to: CGPoint(x: 139, y: 328), control1: CGPoint(x: 69, y: 350), control2: CGPoint(x: 114, y: 355))
            p.addLine(to: CGPoint(x: 190, y: 246))
            p.addCurve(to: CGPoint(x: 282, y: 258), control1: CGPoint(x: 205, y: 298), control2: CGPoint(x: 272, y: 291))
        case .ds4:
            p.addLine(to: CGPoint(x: 155, y: 81))
            p.addCurve(to: CGPoint(x: 83, y: 148), control1: CGPoint(x: 112, y: 77), control2: CGPoint(x: 86, y: 102))
            p.addLine(to: CGPoint(x: 59, y: 299))
            p.addCurve(to: CGPoint(x: 137, y: 332), control1: CGPoint(x: 47, y: 353), control2: CGPoint(x: 110, y: 361))
            p.addLine(to: CGPoint(x: 207, y: 269))
            p.addQuadCurve(to: CGPoint(x: 277, y: 267), control: CGPoint(x: 252, y: 293))
        case .dualsense, .dualsenseedge:
            p.addCurve(to: CGPoint(x: 129, y: 85), control1: CGPoint(x: 215, y: 65), control2: CGPoint(x: 172, y: 59))
            p.addCurve(to: CGPoint(x: 55, y: 315), control1: CGPoint(x: 83, y: 117), control2: CGPoint(x: 57, y: 249))
            p.addCurve(to: CGPoint(x: 123, y: 341), control1: CGPoint(x: 50, y: 368), control2: CGPoint(x: 104, y: 366))
            p.addCurve(to: CGPoint(x: 216, y: 267), control1: CGPoint(x: 160, y: 305), control2: CGPoint(x: 157, y: 263))
            p.addLine(to: CGPoint(x: 276, y: 278))
        case .steamcontroller:
            p.addCurve(to: CGPoint(x: 128, y: 75), control1: CGPoint(x: 225, y: 91), control2: CGPoint(x: 179, y: 43))
            p.addCurve(to: CGPoint(x: 40, y: 230), control1: CGPoint(x: 62, y: 74), control2: CGPoint(x: 38, y: 146))
            p.addCurve(to: CGPoint(x: 133, y: 341), control1: CGPoint(x: 39, y: 306), control2: CGPoint(x: 77, y: 374))
            p.addCurve(to: CGPoint(x: 251, y: 284), control1: CGPoint(x: 167, y: 319), control2: CGPoint(x: 185, y: 280))
        case .steamcontroller2:
            p.addLine(to: CGPoint(x: 150, y: 82))
            p.addCurve(to: CGPoint(x: 70, y: 150), control1: CGPoint(x: 101, y: 80), control2: CGPoint(x: 76, y: 108))
            p.addCurve(to: CGPoint(x: 62, y: 318), control1: CGPoint(x: 62, y: 207), control2: CGPoint(x: 51, y: 272))
            p.addCurve(to: CGPoint(x: 150, y: 348), control1: CGPoint(x: 70, y: 366), control2: CGPoint(x: 124, y: 372))
            p.addCurve(to: CGPoint(x: 268, y: 325), control1: CGPoint(x: 182, y: 322), control2: CGPoint(x: 226, y: 325))
        case .switchpro:
            p.addLine(to: CGPoint(x: 158, y: 77))
            p.addCurve(to: CGPoint(x: 90, y: 143), control1: CGPoint(x: 123, y: 77), control2: CGPoint(x: 98, y: 99))
            p.addLine(to: CGPoint(x: 61, y: 286))
            p.addCurve(to: CGPoint(x: 143, y: 327), control1: CGPoint(x: 47, y: 346), control2: CGPoint(x: 116, y: 358))
            p.addLine(to: CGPoint(x: 212, y: 266))
            p.addQuadCurve(to: CGPoint(x: 259, y: 259), control: CGPoint(x: 231, y: 254))
        }
        let waist: CGFloat = type == .steamcontroller2 ? 325 : type == .steamcontroller ? 277 : type.dualTone ? 278
            : type == .ds4 ? 267 : type == .ds3 ? 258 : 255
        p.addQuadCurve(to: CGPoint(x: 300, y: waist), control: CGPoint(x: 284, y: waist))
        p.closeSubpath()
        var whole = p
        whole.addPath(p, transform: CGAffineTransform(a: -1, b: 0, c: 0, d: 1, tx: 600, ty: 0))
        return whole
    }

    private func drawShell(_ context: inout GraphicsContext) {
        let light = type == .xbox360 || type.dualTone
        let shell = silhouette()
        var shadow = context
        shadow.addFilter(.shadow(color: .black.opacity(0.65), radius: 9, x: 0, y: 7))
        shadow.fill(shell, with: .color(.black))
        context.fill(shell, with: .linearGradient(Gradient(colors: light
            ? [Color(white: 0.96), Color(white: 0.62)]
            : [Color(white: type == .xboxelite2 ? 0.20 : 0.29), Color(white: 0.065)]),
            startPoint: CGPoint(x: 200, y: 70), endPoint: CGPoint(x: 360, y: 355)))
        var finish = context
        finish.clip(to: shell)
        if type == .switchpro {
            plate(CGRect(x: 115, y: 91, width: 370, height: 171), 55, &finish,
                  top: Color(white: 0.22).opacity(0.45), bottom: .black.opacity(0.18),
                  rim: .gray.opacity(0.25))
            for x in [CGFloat(121), 479] {
                finish.stroke(Path(ellipseIn: CGRect(x: x - 3, y: 214, width: 6, height: 6)),
                              with: .color(.gray.opacity(0.3)), lineWidth: 1)
            }
        }
        if type == .xbox360 || type == .ds3 {
            finish.fill(Path(ellipseIn: CGRect(x: 77, y: 77, width: 450, height: 100)),
                        with: .linearGradient(Gradient(colors: [.white.opacity(0.13), .clear]),
                            startPoint: CGPoint(x: 300, y: 77), endPoint: CGPoint(x: 300, y: 177)))
        }
        // Avoid a seam at the mirrored centre by stroking only the exterior laterally.
        if type.dualTone {
            var inset = Path()
            inset.move(to: CGPoint(x: 214, y: 172))
            inset.addQuadCurve(to: CGPoint(x: 386, y: 172), control: CGPoint(x: 300, y: 201))
            inset.addLine(to: CGPoint(x: 428, y: 305))
            inset.addLine(to: CGPoint(x: 362, y: 300))
            inset.addLine(to: CGPoint(x: 238, y: 300))
            inset.addLine(to: CGPoint(x: 172, y: 305)); inset.closeSubpath()
            var clipped = context
            clipped.clip(to: shell)
            clipped.fill(inset, with: .color(Color(white: 0.065)))
        }
        if type == .ds3 || type == .ds4 {
            for x in [CGFloat(152), 451] {
                plate(CGRect(x: x - 52, y: 102, width: 104, height: 104), 52, &context,
                      top: Color(white: 0.24), bottom: Color(white: 0.13), rim: Color(white: 0.28))
            }
        }
        if type == .xbox360 || type == .xboxelite2 || type == .xboxseries {
            plate(CGRect(x: 187, y: 175, width: 90, height: 90), 45, &context,
                  top: Color(white: type == .xboxelite2 ? 0.6 : 0.27), bottom: Color(white: 0.12))
        }
        if type == .switchpro || type == .xboxelite2 || type == .xboxseries {
            for x in [CGFloat(111), 473] {
                for row in 0..<8 {
                    for col in 0..<4 {
                        let dot = CGRect(x: x + CGFloat(col) * 5, y: 242 + CGFloat(row) * 8, width: 1.5, height: 2)
                        context.fill(Path(ellipseIn: dot), with: .color(.white.opacity(0.14)))
                    }
                }
            }
        }
        if type.hasLED {
            let touch = CGRect(x: 225, y: 87, width: 150, height: type.dualTone ? 80 : 67)
            plate(touch, type.dualTone ? 17 : 9, &context,
                  top: Color(white: type == .dualsense ? 0.91 : 0.18),
                  bottom: Color(white: type == .dualsense ? 0.72 : 0.09))
            if type == .ds4 {
                plate(CGRect(x: 244, y: 77, width: 112, height: 4), 2, &context, top: led, bottom: led, rim: led)
            } else {
                for x in [CGFloat(219), 378] {
                    plate(CGRect(x: x, y: 94, width: 3, height: 68), 1.5, &context, top: led, bottom: led, rim: led)
                }
            }
            for row in 0..<3 {
                for col in 0..<7 {
                    context.fill(Path(ellipseIn: CGRect(x: 280 + col * 6, y: 176 + row * 5, width: 2, height: 2)),
                                 with: .color(type.dualTone ? .gray : .black))
                }
            }
        }
        let logoY: CGFloat = type.hasLED ? 213 : type == .ds3 ? 220 : type == .steamcontroller ? 172 : type == .steamcontroller2 ? 150 : type == .switchpro ? 178 : 102
        let logoX: CGFloat = type == .switchpro ? 331 : 300
        plate(CGRect(x: logoX - 13, y: logoY - 13, width: 26, height: 26), 13, &context,
              top: Color(white: 0.4), rim: type == .xbox360 ? .green : .gray)
        drawLogo(at: CGPoint(x: logoX, y: logoY), &context)
        if type == .xbox360 {
            for degrees in [0.0, 90, 180, 270] {
                var arc = Path()
                arc.addArc(center: CGPoint(x: 300, y: logoY), radius: 15,
                           startAngle: .degrees(degrees + 8), endAngle: .degrees(degrees + 78), clockwise: false)
                context.stroke(arc, with: .color(.green), lineWidth: 3)
            }
        }
        if type == .steamcontroller2 {
            plate(CGRect(x: 287, y: 176, width: 26, height: 16), 6, &context)
            text("···", 300, 183, 11, &context)
        }
        if type == .xboxseries {
            plate(CGRect(x: 289, y: 153, width: 22, height: 20), 6, &context)
            text("↑", 300, 163, 13, &context)
        }
        if type.dualTone {
            plate(CGRect(x: 289, y: 236, width: 22, height: 6), 3, &context, top: .orange, bottom: .orange)
        }
        if type == .dualsenseedge {
            for x in [CGFloat(238), 362] {
                plate(CGRect(x: x - 14, y: 297, width: 28, height: 17), 5, &context)
                text("Fn", x, 305, 9, &context)
            }
        }
        if type == .switchpro {
            plate(CGRect(x: 268, y: 169, width: 18, height: 18), 3, &context)
            text("●", 277, 178, 8, &context)
        }
        if type == .xboxelite2 {
            for (x, title) in [(CGFloat(126), "P3"), (474, "P4")] {
                plate(CGRect(x: x - 19, y: 337, width: 38, height: 20), 7, &context,
                      top: Color(white: 0.55), bottom: Color(white: 0.17))
                text(title, x, 347, 9, &context)
            }
        }
    }

    // The visible outline is also the hit region (including the separate PS arrow keys).
    private func controlPath(_ item: Control) -> Path {
        let r = item.rect
        if item.kind == .direction && !type.playStation {
            return Path(roundedRect: r, cornerRadius: 2)
        }
        if item.kind == .direction {
            var p = Path()
            p.move(to: CGPoint(x: 0, y: -11))
            for point in [CGPoint(x: 10, y: -4), CGPoint(x: 10, y: 11),
                          CGPoint(x: -10, y: 11), CGPoint(x: -10, y: -4)] { p.addLine(to: point) }
            p.closeSubpath()
            let angle: CGFloat = item.slot == .dpadDown ? .pi : item.slot == .dpadLeft ? -.pi / 2
                : item.slot == .dpadRight ? .pi / 2 : 0
            return p.applying(CGAffineTransform(rotationAngle: angle)
                .concatenating(CGAffineTransform(translationX: r.midX, y: r.midY)))
        }
        if item.kind == .shoulder {
            var p = Path()
            p.move(to: CGPoint(x: r.minX, y: r.maxY))
            p.addQuadCurve(to: CGPoint(x: r.minX + 9, y: r.minY + 3),
                           control: CGPoint(x: r.minX - 1, y: r.minY + 5))
            p.addQuadCurve(to: CGPoint(x: r.maxX - 9, y: r.minY + 3),
                           control: CGPoint(x: r.midX, y: r.minY - 4))
            p.addQuadCurve(to: CGPoint(x: r.maxX, y: r.maxY),
                           control: CGPoint(x: r.maxX + 1, y: r.minY + 5))
            p.closeSubpath()
            return p
        }
        return Path(roundedRect: r, cornerRadius: item.radius)
    }

    private func drawLogo(at point: CGPoint, _ context: inout GraphicsContext) {
        var logo = context
        logo.translateBy(x: point.x, y: point.y)
        if type.valve {
            var arm = Path()
            arm.move(to: CGPoint(x: -8, y: 6)); arm.addLine(to: CGPoint(x: -2, y: 8))
            arm.addLine(to: CGPoint(x: 8, y: -5))
            logo.stroke(arm, with: .color(.white), style: StrokeStyle(lineWidth: 4, lineCap: .round))
            logo.stroke(Path(ellipseIn: CGRect(x: 2, y: -11, width: 12, height: 12)),
                        with: .color(.white), lineWidth: 2)
        } else if type.playStation {
            text("P", 0, -2, 18, &logo)
            var base = Path()
            base.addEllipse(in: CGRect(x: -10, y: 3, width: 19, height: 6))
            logo.stroke(base, with: .color(.white), lineWidth: 2)
        } else if type == .switchpro {
            var house = Path()
            house.move(to: CGPoint(x: -8, y: 0)); house.addLines([
                CGPoint(x: 0, y: -7), CGPoint(x: 8, y: 0), CGPoint(x: 6, y: 0),
                CGPoint(x: 6, y: 7), CGPoint(x: -6, y: 7), CGPoint(x: -6, y: 0)])
            house.closeSubpath()
            logo.fill(house, with: .color(.white))
        } else {
            var cross = Path()
            cross.move(to: CGPoint(x: -9, y: -8))
            cross.addQuadCurve(to: CGPoint(x: 9, y: 9), control: CGPoint(x: 3, y: -7))
            cross.move(to: CGPoint(x: 9, y: -8))
            cross.addQuadCurve(to: CGPoint(x: -9, y: 9), control: CGPoint(x: -3, y: -7))
            logo.stroke(cross, with: .color(.white), style: StrokeStyle(lineWidth: 3, lineCap: .round))
        }
    }

    private func drawControl(_ item: Control, _ context: inout GraphicsContext) {
        let r = item.rect
        let active = pads.isActive(item.slot)
        let capture = pads.capturing == item.slot
        let metal = type == .xboxelite2 && (item.kind == .stick || item.kind == .direction || item.kind == .paddle)
        let rim: Color = capture ? .yellow : active ? .cyan : metal ? Color(white: 0.75) : Color(white: 0.4)
        if item.kind == .stickClick {
            if capture || active {
                context.stroke(Path(ellipseIn: r), with: .color(rim), lineWidth: 3)
            }
            return
        }
        let shape = controlPath(item)
        if type == .steamcontroller && item.kind == .direction {
            context.fill(shape, with: .color(active ? .cyan.opacity(0.5) : .black.opacity(0.15)))
            context.stroke(shape, with: .color(capture ? .yellow : active ? .cyan : .gray.opacity(0.3)), lineWidth: 1)
            return
        }
        context.fill(shape, with: .linearGradient(Gradient(colors: [
            active ? .cyan.opacity(0.65) : Color(white: metal ? 0.50 : 0.24), Color(white: 0.065)]),
            startPoint: CGPoint(x: r.midX, y: r.minY), endPoint: CGPoint(x: r.midX, y: r.maxY)))
        context.stroke(shape, with: .color(rim), lineWidth: 1.2)

        if capture || active {
            context.stroke(shape,
                           with: .color(rim), lineWidth: 2)
        }
        if item.kind == .stick {
            let cap = r.insetBy(dx: 9, dy: 9)
            plate(cap, cap.width / 2, &context, top: Color(white: 0.20), bottom: Color(white: 0.06), rim: rim)
            context.stroke(Path(ellipseIn: cap.insetBy(dx: 4, dy: 4)), with: .color(.gray.opacity(0.5)),
                           style: StrokeStyle(lineWidth: 1.5, dash: [2, 2]))
            let point = pads.stick(item.slot)
            context.fill(Path(ellipseIn: CGRect(x: r.midX - 2 + point.x * 10, y: r.midY - 2 + point.y * 10,
                                                width: 4, height: 4)), with: .color(active ? .cyan : .gray))
        } else if item.kind == .pad && type == .steamcontroller2 {
            let inner = r.insetBy(dx: 8, dy: 8)
            context.stroke(Path(roundedRect: inner, cornerRadius: 8), with: .color(.gray.opacity(0.18)), lineWidth: 0.7)
        } else if item.kind == .pad {
            for radius in [CGFloat(39), 45, 50] {
                context.stroke(Path(ellipseIn: CGRect(x: r.midX - radius, y: r.midY - radius,
                                                      width: radius * 2, height: radius * 2)),
                               with: .color(.gray.opacity(0.16)), lineWidth: 0.7)
            }
        } else {
            let colors: [PadSlot: Color] = type.playStation
                ? [.a: Color(red: 0.3, green: 0.6, blue: 1), .b: .red, .x: .pink, .y: .green]
                : type == .switchpro ? [:] : [.a: .green, .b: .red, .x: .blue, .y: .yellow]
            let menu = type.playStation && (item.slot == .minus || item.slot == .plus)
            text(item.kind == .direction ? "" : menu ? item.title.uppercased() : item.title,
                 r.midX, r.midY, menu ? 7 : r.height >= 28 ? 16 : 9,
                 &context, color: colors[item.slot] ?? .white)
        }
    }
}
