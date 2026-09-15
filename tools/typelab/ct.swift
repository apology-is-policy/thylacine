// ct -- render a typelab spec through CoreText/Quartz into a P6 PPM.
//
//   ct <spec> <out.ppm> [smooth=0|1] [subpx=0|1] [kern=0|1]
//
// The spec is typelab's shared line format (see spec.rs in the Rust half):
//   size W H
//   bg RRGGBB
//   face <key> <ttf path>
//   line <baseline_y> <x0>
//   run <facekey> <px> <RRGGBB> <text...>
// Anything else is ignored here (crops are the Rust half's business).
// Every run is drawn as one CTLine at the running pen; the pen advances by
// the line's typographic width (fractional), so with subpixel positioning ON
// the runs land where a real layout would put them.
import Foundation
import CoreGraphics
import CoreText

func die(_ s: String) -> Never { FileHandle.standardError.write((s + "\n").data(using: .utf8)!); exit(1) }

func hexColor(_ s: String) -> (CGFloat, CGFloat, CGFloat) {
    var v: UInt64 = 0
    Scanner(string: s).scanHexInt64(&v)
    return (CGFloat((v >> 16) & 0xFF) / 255.0, CGFloat((v >> 8) & 0xFF) / 255.0, CGFloat(v & 0xFF) / 255.0)
}

let args = CommandLine.arguments
if args.count < 3 { die("usage: ct <spec> <out.ppm> [smooth=0|1] [subpx=0|1] [kern=0|1]") }
var smooth = true, subpx = true, kern = false
for a in args.dropFirst(3) {
    let kv = a.split(separator: "=").map(String.init)
    if kv.count == 2 {
        let on = kv[1] == "1"
        switch kv[0] { case "smooth": smooth = on; case "subpx": subpx = on; case "kern": kern = on; default: die("unknown option \(a)") }
    }
}

guard let specText = try? String(contentsOfFile: args[1], encoding: .utf8) else { die("cannot read \(args[1])") }
var W = 0, H = 0
var bg: (CGFloat, CGFloat, CGFloat) = (1, 1, 1)
var faces: [String: CGFont] = [:]
struct Run { let face: String; let px: Double; let color: (CGFloat, CGFloat, CGFloat); let text: String }
struct Line { let baseline: Double; let x0: Double; var runs: [Run] }
var lines: [Line] = []

for raw in specText.split(separator: "\n", omittingEmptySubsequences: false) {
    let line = String(raw)
    if line.trimmingCharacters(in: .whitespaces).isEmpty || line.hasPrefix("#") { continue }
    let parts = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
    switch parts[0] {
    case "size": W = Int(parts[1])!; H = Int(parts[2])!
    case "bg": bg = hexColor(parts[1])
    case "face":
        let url = URL(fileURLWithPath: parts[2]) as CFURL
        guard let prov = CGDataProvider(url: url), let f = CGFont(prov) else { die("cannot load font \(parts[2])") }
        faces[parts[1]] = f
    case "line": lines.append(Line(baseline: Double(parts[1])!, x0: Double(parts[2])!, runs: []))
    case "run":
        // text = everything after the fourth field, VERBATIM -- a run's leading
        // or trailing space is layout, and a trimmed one collapses two runs.
        var idx = line.startIndex
        var fields = 0
        while fields < 4 {
            while line[idx] != " " { idx = line.index(after: idx) }
            fields += 1
            if fields < 4 { while line[idx] == " " { idx = line.index(after: idx) } }
        }
        idx = line.index(after: idx)
        let text = String(line[idx...])
        lines[lines.count - 1].runs.append(Run(face: parts[1], px: Double(parts[2])!, color: hexColor(parts[3]), text: text))
    default: break
    }
}
if W == 0 || H == 0 { die("no size") }

let cs = CGColorSpace(name: CGColorSpace.sRGB)!
guard let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: W * 4,
                          space: cs, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { die("no context") }
ctx.setFillColor(CGColor(colorSpace: cs, components: [bg.0, bg.1, bg.2, 1])!)
ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
ctx.setAllowsAntialiasing(true)
ctx.setShouldAntialias(true)
ctx.setAllowsFontSmoothing(smooth)
ctx.setShouldSmoothFonts(smooth)
ctx.setAllowsFontSubpixelPositioning(subpx)
ctx.setShouldSubpixelPositionFonts(subpx)
ctx.setAllowsFontSubpixelQuantization(!subpx)
ctx.setShouldSubpixelQuantizeFonts(!subpx)
ctx.textMatrix = .identity

for ln in lines {
    var pen = ln.x0
    for r in ln.runs {
        guard let cgf = faces[r.face] else { die("unknown face \(r.face)") }
        let font = CTFontCreateWithGraphicsFont(cgf, CGFloat(r.px), nil, nil)
        var attrs: [NSAttributedString.Key: Any] = [
            kCTFontAttributeName as NSAttributedString.Key: font,
            kCTForegroundColorAttributeName as NSAttributedString.Key: CGColor(colorSpace: cs, components: [r.color.0, r.color.1, r.color.2, 1])!,
            kCTLigatureAttributeName as NSAttributedString.Key: NSNumber(value: 0),
        ]
        if !kern { attrs[kCTKernAttributeName as NSAttributedString.Key] = NSNumber(value: 0.0) }
        let astr = NSAttributedString(string: r.text, attributes: attrs)
        let ctline = CTLineCreateWithAttributedString(astr)
        ctx.textPosition = CGPoint(x: pen, y: Double(H) - ln.baseline)
        CTLineDraw(ctline, ctx)
        pen += CTLineGetTypographicBounds(ctline, nil, nil, nil)
    }
}

guard let data = ctx.data else { die("no data") }
let bytes = data.assumingMemoryBound(to: UInt8.self)
var out = Data()
out.append("P6\n\(W) \(H)\n255\n".data(using: .ascii)!)
for y in 0..<H {
    let row = bytes + y * ctx.bytesPerRow
    for x in 0..<W {
        out.append(row[x * 4]); out.append(row[x * 4 + 1]); out.append(row[x * 4 + 2])
    }
}
do { try out.write(to: URL(fileURLWithPath: args[2])) } catch { die("write failed: \(error)") }
