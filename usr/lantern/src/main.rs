// lantern -- the Beacon deck presenter (docs/LANTERN-DESIGN.md). Shows a
// directory of Markdown slides one at a time: rich under Halcyon, the same
// payload without frames on serial, a plain concatenation down a pipe. The
// manifest, the key map and the navigation live in the library; this body
// supplies the files, the tier and the keystrokes. A picture slide is shown by
// `view` in a process of its own (LANTERN-DESIGN 14); lantern decodes nothing.
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

// One section and its largest block, exactly as the `manual` reader holds them:
// the manual's `bounds` test measures that peak under this heap (MANUAL-DESIGN
// 8.1), and lantern holds no more than that at once.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use beacon::sink::{Em, Sink};
use beacon::{BeaconMode, Tier};
use libthyla_rs::env;
use libthyla_rs::eprintln;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Read};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::println;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::time::{self, Duration};
use libthyla_rs::T_CAP_CSPRNG_READ;

use kaua::input::Parser;
use lantern::deck::{self, Deck, SlideKind};
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
    if operands[0].is_empty() {
        eprintln!("lantern: the deck directory name is empty");
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

/// The wrap width, by `manual`'s own rule: only at a plain tier, only on the
/// console, only when `/dev/winsize` reports one. A tile's pts is a terminal,
/// but that leaf is the console's width, not the tile's.
fn plain_width(tier: Tier) -> Option<usize> {
    if !manual::wraps_at_console(tier, libthyla_rs::fd_devclass(1)) {
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

/// Open a file of the deck as a regular file IN the deck directory: a symbolic
/// link is refused (`T_ONOFOLLOW`), and so is anything but a regular file. The
/// name rules keep a slide's name inside the deck; this keeps its content there,
/// so a deck someone else wrote cannot put the presenter's own files on the
/// screen (LANTERN-DESIGN 8).
fn open_deck_file(path: &str) -> Result<File, String> {
    let shown = sanitize(path, false);
    let f = File::open_nofollow(path).map_err(|e| match e {
        Error::SymlinkLoop => format!("{}: a link; a deck's files are files in its directory", shown),
        e => format!("{}: {}", shown, e),
    })?;
    match f.metadata() {
        Ok(m) if m.is_file() => Ok(f),
        Ok(_) => Err(format!("{}: not a regular file", shown)),
        Err(e) => Err(format!("{}: {}", shown, e)),
    }
}

fn read_text(path: &str, cap: usize) -> Result<String, String> {
    let shown = sanitize(path, false);
    let mut f = open_deck_file(path)?;
    let bytes = read_capped(&mut f, cap).map_err(|e| match e {
        Error::NoMemory => format!("{}: larger than {} bytes", shown, cap),
        e => format!("{}: {}", shown, e),
    })?;
    String::from_utf8(bytes).map_err(|_| format!("{}: not valid UTF-8", shown))
}

/// stdout, cooking LF to CR-LF when the line discipline has stopped doing it.
/// Each `put` is one write.
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
            let mut cooked = Vec::with_capacity(bytes.len() + bytes.len() / 8);
            lantern::cook(bytes, &mut |c| cooked.extend_from_slice(c));
            self.sink.put(&cooked);
        } else {
            self.sink.put(bytes);
        }
    }

    fn failed(&self) -> bool {
        self.sink.failed()
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

/// The most of `view`'s standard output, and of its standard error, that is
/// kept: a reference is under 200 bytes and a reason is one line. The rest is
/// read and dropped, so a child that writes more cannot block on a full pipe.
const VIEW_REPLY_MAX: usize = 1024;

/// How long `view` may go without a byte or an exit before it is killed. A
/// decode within its budget and an upload take well under a second; the bound
/// exists so a hung child cannot freeze the talk, or the check before it,
/// while lantern is not reading keys.
const VIEW_STALL_MS: u32 = 30_000;

/// The step of the reap once both pipes are closed: `view` exits as it closes
/// them, so the first look nearly always finds it gone.
const VIEW_REAP_STEP_MS: u32 = 10;

/// The picture checker and placer, named by its absolute path: a spawn resolves
/// a relative name against the working directory, never a search path, so a bare
/// `view` would run whatever file of that name sits where lantern was started --
/// a deck directory included.
const VIEW: &str = "/bin/view";

/// All `view` needs: the random reference it places a picture under. A decoder
/// that a hostile picture subverts holds none of lantern's other capabilities;
/// its identity, namespace and environment are still the presenter's.
const VIEW_CAPS: u64 = T_CAP_CSPRNG_READ;

/// Run `view <mode> -` with `picture` as its standard input, and collect its
/// exit status, standard output and standard error, both drained together so
/// neither pipe can fill while the other is read. `Err` when it cannot run or
/// stalls.
fn run_view(mode: &str, picture: File) -> Result<(i64, Vec<u8>, Vec<u8>), String> {
    let mut child = Command::new(VIEW)
        .caps(VIEW_CAPS)
        .arg(mode)
        .arg("-")
        .stdin(Stdio::File(picture))
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
        .map_err(|e| format!("cannot run view: {}", e))?;
    let (Some(out), Some(err)) = (child.stdout.take(), child.stderr.take()) else {
        let _ = child.kill();
        let _ = child.wait();
        return Err(String::from("cannot run view: no pipe to read its answer"));
    };
    let mut pipes = [out, err];
    let mut kept: [Vec<u8>; 2] = [Vec::new(), Vec::new()];
    let mut open = [true, true];
    let mut buf = [0u8; 512];
    while open[0] || open[1] {
        let mut ps = PollSet::new();
        for (i, p) in pipes.iter().enumerate() {
            if open[i] {
                ps.add_raw(p.as_raw_fd(), PollEvents::READ);
            }
        }
        let ready: Vec<i32> = match ps.poll(PollTimeout::Millis(VIEW_STALL_MS)) {
            Ok(events) => events.map(|e| e.fd).collect(),
            Err(e) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("cannot wait for view: {}", e));
            }
        };
        if ready.is_empty() {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("view made no progress for {} seconds", VIEW_STALL_MS / 1000));
        }
        for i in 0..2 {
            if !open[i] || !ready.contains(&pipes[i].as_raw_fd()) {
                continue;
            }
            match pipes[i].read(&mut buf) {
                Ok(0) | Err(_) => open[i] = false,
                Ok(n) => {
                    let room = VIEW_REPLY_MAX - kept[i].len();
                    kept[i].extend_from_slice(&buf[..n.min(room)]);
                }
            }
        }
    }
    // Both pipes are closed. A child that closed them and lives on is held to
    // the same bound as a silent one, never waited on without end.
    let mut waited = 0;
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break s.code().unwrap_or(1) as i64,
            Ok(None) if waited < VIEW_STALL_MS => {
                let _ = time::sleep(Duration::from_millis(VIEW_REAP_STEP_MS as u64));
                waited += VIEW_REAP_STEP_MS;
            }
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("view made no progress for {} seconds", VIEW_STALL_MS / 1000));
            }
            Err(e) => return Err(format!("view: wait failed: {}", e)),
        }
    };
    let [out, err] = kept;
    Ok((status, out, err))
}

/// The first line `view` wrote on standard error: its reason, alone.
fn view_reason(err: &[u8]) -> String {
    let text = String::from_utf8_lossy(err);
    let line = text.lines().next().unwrap_or("").trim();
    if line.is_empty() {
        String::from("view failed without a reason")
    } else {
        sanitize(line, false)
    }
}

/// Check a picture slide as the talk will show it: `view --check`, with the
/// picture as its standard input, so what is checked and what is shown are one
/// file and one decoder. `Err` carries the line to print.
fn check_picture(dir: &str, name: &str) -> Result<(), String> {
    let shown = sanitize(name, false);
    let f = open_deck_file(&slide_path(dir, name))?;
    match run_view("--check", f) {
        Ok((0, _, _)) => Ok(()),
        Ok((_, _, err)) => Err(format!("{}: {}", shown, view_reason(&err))),
        Err(why) => Err(format!("{}: {}", shown, why)),
    }
}

/// Validate the whole deck, holding one slide at a time. Returns the number of
/// slides that failed, having printed every diagnostic.
fn validate(dir: &str, d: &Deck) -> usize {
    let mut bad = 0;
    for name in &d.slides {
        let checked = match deck::kind(name) {
            Some(SlideKind::Picture) => check_picture(dir, name).map_err(|l| alloc::vec![l]),
            _ => slide_source(dir, name).map(|_| ()),
        };
        if let Err(lines) = checked {
            bad += 1;
            for l in lines {
                eprintln!("lantern: {}", l);
            }
        }
    }
    bad
}

/// Place a picture in this pane with `view --embed` and return the reference to
/// write where the picture belongs, or why it could not be placed.
fn embed_picture(dir: &str, name: &str) -> Result<Vec<u8>, String> {
    let f = open_deck_file(&slide_path(dir, name))?;
    let (status, out, err) = run_view("--embed", f)?;
    if status != 0 {
        return Err(view_reason(&err));
    }
    if !lantern::is_reference(&out) {
        return Err(String::from("view answered with something other than one picture reference"));
    }
    Ok(out)
}

/// A picture slide: the picture itself where one can be shown, the rich tier;
/// everywhere else, and wherever `view` could not place it, its stand-in, which
/// then says why (LANTERN-DESIGN 14).
fn paint_picture(out: &mut Vec<u8>, tier: Tier, width: Option<usize>, dir: &str, name: &str) {
    let why = if tier == Tier::Rich {
        match embed_picture(dir, name) {
            Ok(reference) => {
                out.extend_from_slice(&reference);
                return;
            }
            Err(why) => Some(why),
        }
    } else {
        None
    };
    let src = lantern::stand_in(name, why.as_deref());
    manual::render::render(&src, tier, width, &mut |chunk| out.extend_from_slice(chunk));
}

/// The footer: which slide this is, and the deck's title when it has one.
///
/// It is written AFTER the slide's content, not at the bottom of the screen.
/// Beacon has no op that places a line at a screen edge and must not grow one
/// -- position is the renderer's, and a program reaching for it is the failure
/// the format exists to avoid. Dim is a MEANING (de-emphasised), which is why
/// it is available.
fn footer(out: &mut Vec<u8>, tier: Tier, d: &Deck, at: usize) {
    let mut s = Sink::new(out, tier);
    s.text("\n");
    let counter = format!("{} / {}", at + 1, d.slides.len());
    match &d.title {
        Some(t) => s.em(Em::Dim, &format!("{}  ·  {}", sanitize(t, false), counter)),
        None => s.em(Em::Dim, &counter),
    }
    s.text("\n");
}

/// Render one slide into `out`: its body, then the footer.
fn paint(
    out: &mut Vec<u8>,
    tier: Tier,
    width: Option<usize>,
    d: &Deck,
    dir: &str,
    at: usize,
    foot: bool,
) {
    let name = &d.slides[at];
    if deck::kind(name) == Some(SlideKind::Picture) {
        paint_picture(out, tier, width, dir, name);
        if foot {
            footer(out, tier, d, at);
        }
        return;
    }
    match slide_source(dir, name) {
        Ok(src) => {
            manual::render::render(&src, tier, width, &mut |chunk| out.extend_from_slice(chunk))
        }
        Err(lines) => {
            // A slide that has become invalid while the deck is open (an edit
            // mid-rehearsal) shows its diagnostics IN PLACE rather than ending
            // the presentation. Losing the deck is a far worse failure on a
            // stage than a slide that reports what is wrong with it.
            out.extend_from_slice(b"This slide cannot be shown:\n\n");
            for l in &lines {
                out.extend_from_slice(l.as_bytes());
                out.push(b'\n');
            }
        }
    }
    if foot {
        footer(out, tier, d, at);
    }
}

/// Show one slide as one synchronized frame in one write (LANTERN-DESIGN 13).
fn show(
    out: &mut Out,
    tier: Tier,
    width: Option<usize>,
    d: &Deck,
    dir: &str,
    at: usize,
    foot: bool,
) {
    let frame = lantern::slide_frame(&mut |f| paint(f, tier, width, d, dir, at, foot));
    out.put(&frame);
}

fn present(dir: &str, d: &Deck, tier: Tier, foot: bool) -> i64 {
    let width = plain_width(tier);
    let mut out = Out::to_stdout();
    let mut at = 0usize;
    let mut parser = Parser::new();
    let mut stdin = io::stdin();
    // One byte per read: the quit key is the last byte this program takes. Bytes
    // typed behind it -- the next command, a paste -- stay queued for the shell; a
    // larger read would carry them away with the slide.
    let mut buf = [0u8; 1];

    out.put(lantern::HIDE_CARET);
    show(&mut out, tier, width, d, dir, at, foot);

    loop {
        if out.failed() {
            out.put(lantern::SHOW_CARET);
            eprintln!("lantern: write error");
            return 1;
        }
        // A read error is fatal, as it is in `kaua::source` -- there is no
        // EINTR in this Error set (the raw-mode dance's `-isig` means no note is
        // cooked for this program anyway), and spinning on a would-block with no
        // poll in hand would be worse than stopping.
        let n = match stdin.read(&mut buf) {
            Ok(0) => {
                // stdin closed: the deck is over.
                out.put(lantern::SHOW_CARET);
                return 0;
            }
            Ok(n) => n,
            Err(e) => {
                out.put(lantern::SHOW_CARET);
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
                out.put(lantern::SHOW_CARET);
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
                show(&mut out, tier, width, d, dir, at, foot);
            }
        }
    }
}

fn cat(dir: &str, d: &Deck, tier: Tier, foot: bool) -> i64 {
    let width = plain_width(tier);
    let mut out = Out::to_stdout();
    for at in 0..d.slides.len() {
        let mut slide = Vec::new();
        if at > 0 {
            slide.push(b'\n');
        }
        paint(&mut slide, tier, width, d, dir, at, foot);
        out.put(&slide);
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
    let slides = if d.slides.len() == 1 { "slide" } else { "slides" };
    if bad > 0 {
        eprintln!(
            "lantern: {} of {} {} cannot be shown",
            bad,
            d.slides.len(),
            slides
        );
        return 1;
    }

    if args.check {
        println!("lantern: {} {}, all valid", d.slides.len(), slides);
        return 0;
    }

    let tier = resolve_tier(args.beacon);
    match show_mode() {
        Show::Present => present(&args.dir, &d, tier, args.footer),
        Show::Cat => cat(&args.dir, &d, tier, args.footer),
    }
}
