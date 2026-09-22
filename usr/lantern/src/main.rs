// lantern -- the Beacon deck presenter (docs/LANTERN-DESIGN.md). Shows a
// directory of Markdown slides one at a time: rich under Halcyon, the same
// payload without frames on serial, a plain concatenation down a pipe. The
// manifest, the key map and the navigation live in the library; this body
// supplies the files, the tier and the keystrokes.
//
// Memory: one slide at a time. The deck is VALIDATED whole at startup (each
// slide read, checked and dropped) and each slide is re-read when it is shown,
// so the working set is one section -- `manual`'s own bound -- whatever the
// deck's size. Re-reading also means a slide edited while the deck is open
// shows its new text the next time it comes round, which is what rehearsing
// wants.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

// One section and its largest block, exactly as the `manual` reader bounds it
// (MANUAL-DESIGN 8.1): lantern holds no more than that at once.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAllocN<{ manual::HEAP_BYTES }> =
    libthyla_rs::alloc::ThylaAllocN;

use beacon::sink::{Em, Sink};
use beacon::{BeaconMode, Tier};
use libthyla_rs::env;
use libthyla_rs::eprintln;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Read};
use libthyla_rs::println;

use kaua::input::Parser;
use lantern::deck::{self, Deck};
use lantern::nav::{action_for, Action};
use manual::{format as section, sanitize, SECTION_MAX};

const USAGE: &str = "usage: lantern [--beacon=auto|always|never] [--no-footer] <deck-directory>\n       lantern --check <deck-directory>\n";

/// How the deck is shown.
enum Show {
    /// One slide at a time, driven by keys: stdout is a terminal something
    /// renders AND stdin is one too.
    Present,
    /// Every slide in order, once, no clear and no keys -- a pipe, a file, a
    /// `lantern deck | less`. The same bytes the presentation shows, minus the
    /// interactivity that has no meaning here.
    Cat,
}

struct Args {
    dir: String,
    beacon: BeaconMode,
    check: bool,
    footer: bool,
}

fn parse_args() -> Result<Args, i64> {
    let mut beacon = BeaconMode::Auto;
    let mut check = false;
    let mut footer = true;
    let mut operands: Vec<String> = Vec::new();
    let mut options_done = false;
    for raw in env::args().operands() {
        let Ok(arg) = core::str::from_utf8(raw) else {
            eprintln!("lantern: an argument is not valid UTF-8");
            return Err(2);
        };
        if !options_done && arg.starts_with('-') && arg != "-" {
            if arg == "--" {
                options_done = true;
            } else if arg == "--check" {
                check = true;
            } else if arg == "--no-footer" {
                footer = false;
            } else if arg == "-h" || arg == "--help" {
                io::out(USAGE.as_bytes());
                return Err(0);
            } else if let Some(when) = arg.strip_prefix("--beacon=") {
                match BeaconMode::parse_when(when) {
                    Some(m) => beacon = m,
                    None => {
                        eprintln!("lantern: --beacon takes auto, always, or never");
                        io::err(USAGE.as_bytes());
                        return Err(2);
                    }
                }
            } else {
                eprintln!("lantern: unknown option '{}'", sanitize(arg, false));
                io::err(USAGE.as_bytes());
                return Err(2);
            }
            continue;
        }
        operands.push(String::from(arg));
    }
    if operands.len() != 1 {
        io::err(USAGE.as_bytes());
        return Err(2);
    }
    Ok(Args {
        dir: operands.remove(0),
        beacon,
        check,
        footer,
    })
}

/// The effective tier, resolved exactly as `manual` and the coreutils resolve it.
fn resolve_tier(flag: BeaconMode) -> Tier {
    let env_tier = env::var("BEACON")
        .and_then(|v| Tier::parse(&v))
        .unwrap_or(Tier::None);
    beacon::effective_tier(env_tier, libthyla_rs::fd_devclass(1), flag)
}

/// True for a fd the kernel calls an interactive console or a pts slave -- the
/// same two classes the Beacon emission gate admits.
fn is_terminal(fd: i32) -> bool {
    matches!(
        libthyla_rs::fd_devclass(fd),
        Some(beacon::DC_CONSOLE) | Some(beacon::DC_PTS)
    )
}

/// Presenting needs a screen to clear AND a keyboard to read. Both are asked
/// of the kernel; neither is assumed from the other, because `lantern deck |
/// tee log` has a terminal on stdin and a pipe on stdout, and clearing a pipe
/// would write escapes into a file.
fn show_mode() -> Show {
    if is_terminal(1) && is_terminal(0) {
        Show::Present
    } else {
        Show::Cat
    }
}

/// The wrap width, as `manual` computes it: only at a plain tier, only on a
/// terminal, only when `/dev/winsize` reports one. At the rich tier the
/// renderer owns the width and lantern must not wrap for it.
fn plain_width(tier: Tier) -> Option<usize> {
    if tier == Tier::Rich || !is_terminal(1) {
        return None;
    }
    let mut f = File::open("/dev/winsize").ok()?;
    let bytes = io::slurp_capped(&mut f, 256).ok()?;
    manual::console_width(&bytes)
}

fn read_capped(f: &mut File, cap: usize) -> Result<Vec<u8>, Error> {
    let len = f.metadata().map_or(0, |m| m.len() as usize);
    let mut v = Vec::with_capacity(len.min(cap));
    let mut buf = [0u8; 8 * 1024];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            return Ok(v);
        }
        if v.len() + n > cap {
            return Err(Error::NoMemory);
        }
        v.extend_from_slice(&buf[..n]);
    }
}

fn read_text(path: &str, cap: usize) -> Result<String, String> {
    let shown = sanitize(path, false);
    let mut f = File::open(path).map_err(|e| format!("{}: {}", shown, e))?;
    let bytes = read_capped(&mut f, cap).map_err(|e| match e {
        Error::NoMemory => format!("{}: larger than {} bytes", shown, cap),
        e => format!("{}: {}", shown, e),
    })?;
    String::from_utf8(bytes).map_err(|_| format!("{}: not valid UTF-8", shown))
}

/// stdout, cooking LF to CR-LF when the line discipline has stopped doing it.
struct Out {
    sink: io::OutSink,
    cook: bool,
}

impl Out {
    /// Cook iff stdout is a TERMINAL -- which is not the same as "we are
    /// presenting". `ut` puts lantern in `RAW_MODE` (`-onlcr`) for EVERY
    /// invocation, so `echo x | lantern deck` is a concatenation whose stdout is
    /// still a terminal with output translation off; keying the cook on the
    /// posture instead of on the fd would staircase it. Never cook into a pipe
    /// or a file, which want the document's own bare LFs.
    fn to_stdout() -> Out {
        Out {
            sink: io::OutSink::new(),
            cook: is_terminal(1),
        }
    }

    fn put(&mut self, bytes: &[u8]) {
        if self.cook {
            let sink = &mut self.sink;
            lantern::cook(bytes, &mut |c| sink.put(c));
        } else {
            self.sink.put(bytes);
        }
    }

    fn failed(&self) -> bool {
        self.sink.failed()
    }
}

impl beacon::sink::Out for Out {
    fn out(&mut self, bytes: &[u8]) {
        self.put(bytes);
    }
}

fn slide_path(dir: &str, name: &str) -> String {
    if dir.ends_with('/') {
        format!("{}{}", dir, name)
    } else {
        format!("{}/{}", dir, name)
    }
}

/// Read a slide and check it. `Err` carries the lines to print.
fn slide_source(dir: &str, name: &str) -> Result<String, Vec<String>> {
    let path = slide_path(dir, name);
    let src = read_text(&path, SECTION_MAX).map_err(|m| alloc::vec![m])?;
    let mut problems: Vec<String> = Vec::new();
    // The slide is checked as an ANONYMOUS section: the number-in-the-title
    // rule exists for the manual's `NN-name.md` book ordering, and a deck's
    // order comes from its manifest, so `01-open.md` titled "01 Opening" is
    // the author's business and not an error here.
    section::check(None, &src, &mut |line, p| {
        problems.push(format!("{}:{}: {}", sanitize(name, false), line, p));
    });
    if problems.is_empty() {
        Ok(src)
    } else {
        Err(problems)
    }
}

/// Validate the whole deck, holding one slide at a time. Returns the number of
/// slides that failed, having printed every diagnostic.
fn validate(dir: &str, d: &Deck) -> usize {
    let mut bad = 0;
    for name in &d.slides {
        if let Err(lines) = slide_source(dir, name) {
            bad += 1;
            for l in lines {
                eprintln!("lantern: {}", l);
            }
        }
    }
    bad
}

/// The footer: which slide this is, and the deck's title when it has one.
///
/// It is written AFTER the slide's content, not at the bottom of the screen.
/// Beacon has no op that places a line at a screen edge and must not grow one
/// -- position is the renderer's, and a program reaching for it is the failure
/// the format exists to avoid. Dim is a MEANING (de-emphasised), which is why
/// it is available.
fn footer(out: &mut Out, tier: Tier, d: &Deck, at: usize) {
    let mut s = Sink::new(out, tier);
    s.text("\n");
    let counter = format!("{} / {}", at + 1, d.slides.len());
    match &d.title {
        Some(t) => s.em(Em::Dim, &format!("{}  ·  {}", sanitize(t, false), counter)),
        None => s.em(Em::Dim, &counter),
    }
    s.text("\n");
}

/// Write one slide: its rendered body, then the footer.
fn paint(
    out: &mut Out,
    tier: Tier,
    width: Option<usize>,
    d: &Deck,
    dir: &str,
    at: usize,
    foot: bool,
) {
    match slide_source(dir, &d.slides[at]) {
        Ok(src) => manual::render::render(&src, tier, width, &mut |chunk| out.put(chunk)),
        Err(lines) => {
            // A slide that has become invalid while the deck is open (an edit
            // mid-rehearsal) shows its diagnostics IN PLACE rather than ending
            // the presentation. Losing the deck is a far worse failure on a
            // stage than a slide that reports what is wrong with it.
            out.put(b"This slide cannot be shown:\n\n");
            for l in &lines {
                out.put(l.as_bytes());
                out.put(b"\n");
            }
        }
    }
    if foot {
        footer(out, tier, d, at);
    }
}

fn present(dir: &str, d: &Deck, tier: Tier, foot: bool) -> i64 {
    let width = plain_width(tier);
    let mut out = Out::to_stdout();
    let mut at = 0usize;
    let mut parser = Parser::new();
    let mut stdin = io::stdin();
    let mut buf = [0u8; 64];

    out.put(lantern::CLEAR);
    paint(&mut out, tier, width, d, dir, at, foot);

    loop {
        if out.failed() {
            eprintln!("lantern: write error");
            return 1;
        }
        // A read error is fatal, as it is in `kaua::source` -- there is no
        // EINTR in this Error set (the raw-mode dance's `-isig` means no note is
        // cooked for this program anyway), and spinning on a would-block with no
        // poll in hand would be worse than stopping.
        let n = match stdin.read(&mut buf) {
            Ok(0) => return 0, // stdin closed: the deck is over.
            Ok(n) => n,
            Err(e) => {
                eprintln!("lantern: read: {}", e);
                return 1;
            }
        };
        for &b in &buf[..n] {
            let event = parser.feed(b);
            // A cursor-position report is the console's answer to a size query,
            // never a key (kaua's own source surfaces it as a Resize), so the
            // latch is drained UNCONDITIONALLY and before the no-event bail.
            // Draining it after that bail would leave a CPR latched -- feed
            // yields no event for one -- and the next REAL key would then be
            // swallowed as if it were the report.
            let _ = parser.take_resize();
            let Some(key) = event else { continue };
            let action = action_for(key);
            if action == Action::Quit {
                // Leave the last slide on the screen: a talk ends on its
                // closing slide, and clearing it would blank the room mid
                // question.
                out.put(b"\n");
                return 0;
            }
            let repaint = match action.target(at, d.slides.len()) {
                Some(to) => {
                    at = to;
                    true
                }
                None => action == Action::Redraw,
            };
            if repaint {
                out.put(lantern::CLEAR);
                paint(&mut out, tier, width, d, dir, at, foot);
            }
        }
    }
}

fn cat(dir: &str, d: &Deck, tier: Tier, foot: bool) -> i64 {
    let width = plain_width(tier);
    let mut out = Out::to_stdout();
    for at in 0..d.slides.len() {
        if at > 0 {
            out.put(b"\n");
        }
        paint(&mut out, tier, width, d, dir, at, foot);
    }
    if out.failed() {
        eprintln!("lantern: write error");
        return 1;
    }
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = match parse_args() {
        Ok(a) => a,
        Err(status) => return status,
    };

    let manifest = slide_path(&args.dir, deck::MANIFEST);
    let src = match read_text(&manifest, deck::MANIFEST_MAX) {
        Ok(s) => s,
        Err(m) => {
            eprintln!("lantern: {}", m);
            return 1;
        }
    };
    let d = match deck::parse(&src) {
        Ok(d) => d,
        Err(diag) => {
            eprintln!("lantern: {}:{}", sanitize(&manifest, false), diag);
            return 1;
        }
    };

    // The whole deck is validated before anything is shown. A deck fails at
    // the start or not at all -- never on the slide the talk has reached.
    let bad = validate(&args.dir, &d);
    if bad > 0 {
        eprintln!(
            "lantern: {} of {} slides cannot be shown",
            bad,
            d.slides.len()
        );
        return 1;
    }

    if args.check {
        println!("lantern: {} slides, all valid", d.slides.len());
        return 0;
    }

    let tier = resolve_tier(args.beacon);
    match show_mode() {
        Show::Present => present(&args.dir, &d, tier, args.footer),
        Show::Cat => cat(&args.dir, &d, tier, args.footer),
    }
}
