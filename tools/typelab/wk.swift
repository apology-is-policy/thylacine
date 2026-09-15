// wk -- snapshot an HTML file through WebKit (Safari's engine) at the
// screen's backing scale, and report where a selector's text lands.
//
//   wk <file.html> <out.png> [css_width] [css_height] [selector]
//
// Prints the selector's client rect (CSS px) and the rect of every 'n'
// character inside it, then writes the snapshot (device pixels) as PNG.
import Cocoa
import WebKit

let args = CommandLine.arguments
if args.count < 3 { FileHandle.standardError.write("usage: wk <file.html> <out.png> [w] [h] [selector]\n".data(using: .utf8)!); exit(2) }
let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let url = URL(fileURLWithPath: args[1])
let out = URL(fileURLWithPath: args[2])
let cssW = args.count > 3 ? Double(args[3])! : 1000
let cssH = args.count > 4 ? Double(args[4])! : 700
let selector = args.count > 5 ? args[5] : "h1"

final class D: NSObject, WKNavigationDelegate {
    func webView(_ wv: WKWebView, didFinish nav: WKNavigation!) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
            let js = """
            (function(){
              var el = document.querySelector('\(selector)'); if (!el) return 'NOEL';
              var r = el.getBoundingClientRect();
              var out = ['el ' + r.left + ' ' + r.top + ' ' + r.width + ' ' + r.height + ' dpr=' + window.devicePixelRatio];
              var walker = document.createTreeWalker(el, NodeFilter.SHOW_TEXT); var node;
              while ((node = walker.nextNode())) {
                var t = node.textContent;
                for (var i = 0; i < t.length; i++) {
                  if (t[i] === 'n') {
                    var rg = document.createRange(); rg.setStart(node, i); rg.setEnd(node, i + 1);
                    var b = rg.getBoundingClientRect();
                    out.push('n ' + b.left + ' ' + b.top + ' ' + b.width + ' ' + b.height + ' ' + JSON.stringify(t.substr(Math.max(0, i - 3), 7)));
                  }
                }
              }
              return out.join('\\n');
            })()
            """
            wv.evaluateJavaScript(js) { res, err in
                print(res as? String ?? "JS error \(String(describing: err))")
                let cfg = WKSnapshotConfiguration()
                cfg.rect = CGRect(x: 0, y: 0, width: cssW, height: cssH)
                wv.takeSnapshot(with: cfg) { img, err in
                    guard let img = img, let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
                        print("snapshot failed \(String(describing: err))"); exit(1)
                    }
                    print("pixels \(cg.width) x \(cg.height)")
                    let rep = NSBitmapImageRep(cgImage: cg)
                    guard let data = rep.representation(using: .png, properties: [:]) else { print("png failed"); exit(1) }
                    do { try data.write(to: out) } catch { print("write failed: \(error)"); exit(1) }
                    exit(0)
                }
            }
        }
    }
}

let wv = WKWebView(frame: CGRect(x: 0, y: 0, width: cssW, height: cssH))
let delegate = D()
wv.navigationDelegate = delegate
// A real window on the main screen (behind everything) so the view has the
// display's backing scale and WebKit actually paints.
let win = NSWindow(contentRect: CGRect(x: 0, y: 0, width: cssW, height: cssH), styleMask: [.borderless], backing: .buffered, defer: false)
win.contentView = wv
win.orderBack(nil)
wv.loadFileURL(url, allowingReadAccessTo: url.deletingLastPathComponent())
// Safety: never hang the operator's shell.
DispatchQueue.main.asyncAfter(deadline: .now() + 20) { print("timeout"); exit(3) }
app.run()
