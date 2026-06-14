// nora -- the binary: wire the pure editor engine (nora::{editor,view}) to the
// Kaua backend (Terminal output + PollSource input) and to file I/O.
//
// This is the only nora file that touches a terminal or libthyla-rs; it is
// behind the crate's `backend` feature (default-on for the device build,
// dropped for `cargo test --lib`). The engine it drives is host-tested.
//
// CONSOLE DISCIPLINE (KAUA.md 3.5 / 5; I-27): nora acquires the SCREEN on fd 1
// (Terminal) and reads input on fd 0 (PollSource); it NEVER touches the line
// discipline (consctl). Raw termios is `ut`'s job, set via its private consctl
// fd before nora is spawned (the T-4 dance); nora reads fd 0 assuming bytes
// already arrive raw. nora is never console-attached -> I-27 untouched. On a
// clean exit Terminal::Drop restores the screen; `no_std` panic = abort means
// Drop does NOT run on a crash, so `ut`'s post-reap restore is the backstop.
//
// SIZE: there is no winsize-query syscall, so nora measures the console at
// launch via a CPR round-trip (kaua::query::terminal_size, #117) and sizes the
// viewport to fill the real terminal -- falling back to 80x24 (the size the
// ls-7 LS-CI pins its PTY to) when the terminal does not answer (a dumb terminal
// / the non-interactive harness). Live resize mid-edit (no winsize signal over
// UART) stays a KAUA.md seam -- a re-query keybind is the v1.x answer.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::env;
use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{slurp_capped, Write};
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::{t_fsync, t_putstr};

use kaua::event::Event;
use kaua::rect::Rect;
use kaua::source::{EventSource, PollSource};
use kaua::term::Terminal;

use nora::editor::{Editor, Request};
use nora::view;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

/// Fallback console geometry when the terminal does not answer CPR (see the
/// module header's SIZE note).
const COLS: u16 = 80;
const ROWS: u16 = 24;
/// Viewport bounds applied to a queried size: reject a 0 (parse already does)
/// and cap the max so a garbled reply can't drive a huge buffer allocation. A
/// real terminal is far under 1000 cells per side.
const MIN_DIM: u16 = 1;
const MAX_DIM: u16 = 1000;
/// How long to wait for the CPR reply before falling back. A real round-trip is
/// sub-millisecond; this is generous headroom, paid once at launch and only in
/// full when the terminal never answers.
const SIZE_QUERY_TIMEOUT_MS: u32 = 50;
/// The largest file nora will load (matches the Stratum POC's 2 MiB cap and
/// leaves headroom under the userspace heap for the editor's working set).
const MAX_FILE: usize = 2 * 1024 * 1024;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let (readonly, filename) = parse_args();

    // Load the initial buffer. A named-but-absent file starts empty -- `:w`
    // creates it (the new-file case); any other read error aborts before we
    // touch the screen, so the message reaches the cooked console.
    //
    // #114: the kernel returns a flat -1 for a missing open today (-> Error::Io,
    // NOT NotFound; the clean -T_E_NOENT errno is #102, gated on the #20 errno
    // re-vote), so the NotFound arm below cannot fire for "absent" yet. A stat
    // precheck (fs::exists) is what actually distinguishes absent from a genuine
    // read error: an unresolvable path is the new-file case; a path that resolves
    // but fails to read is a real error worth surfacing (not silently masked as
    // an empty new buffer that would overwrite on save). The interim conflation
    // of "absent" with "exists-but-unstattable" is the documented #102 gap.
    let content = match &filename {
        Some(p) if !fs::exists(p) => String::new(),
        Some(p) => match read_file(p) {
            Ok(c) => c,
            // #102 forward-compat + the stat/open TOCTOU (deleted between the
            // precheck and the open): treat a clean NotFound as the new file too.
            Err(Error::NotFound) => String::new(),
            Err(e) => {
                t_putstr(&format!("nora: {}: {}\n", p, e));
                return 1;
            }
        },
        None => String::new(),
    };

    let mut ed = Editor::new(filename, &content, readonly);

    // Measure the real console (CPR round-trip) and fill it; fall back to 80x24
    // when the terminal does not answer. The console is already raw (ut set it
    // before the spawn) and we have not yet entered the alt-screen, so the probe
    // saves/restores the cursor and leaves the visible screen undisturbed.
    let (cols, rows) = kaua::query::terminal_size(SIZE_QUERY_TIMEOUT_MS)
        .map(|(c, r)| (c.clamp(MIN_DIM, MAX_DIM), r.clamp(MIN_DIM, MAX_DIM)))
        .unwrap_or((COLS, ROWS));

    let mut term = match Terminal::enter(Rect::new(0, 0, cols, rows)) {
        Ok(t) => t,
        Err(_) => {
            t_putstr("nora: cannot acquire the console screen\n");
            return 1;
        }
    };
    let mut src = PollSource::new();

    let code = run(&mut term, &mut src, &mut ed);
    // Explicit restore (Drop also runs it; both are idempotent).
    let _ = term.leave();
    code as i64
}

/// The event loop: poll input, dispatch keys, execute any file request, redraw
/// when the state changed. Returns the process exit code.
fn run(term: &mut Terminal, src: &mut PollSource, ed: &mut Editor) -> i32 {
    if redraw(term, ed).is_err() {
        return 1;
    }
    loop {
        if src.is_eof() {
            return 0;
        }
        let events = match src.poll(PollTimeout::Block) {
            Ok(e) => e,
            // fd 0 gone (no console) -> exit cleanly rather than spin.
            Err(_) => return 1,
        };
        // A wake with no decoded key (a bare HUP) loops; is_eof breaks at the
        // top. The console read is #811 death-interruptible, so a dying nora
        // unwinds here rather than wedging.
        let mut dirty = false;
        for ev in events {
            if let Event::Key(k) = ev {
                ed.handle_key(k);
                dirty = true;
                if let Some(req) = ed.take_request() {
                    handle_request(ed, req);
                }
                if ed.quit {
                    break;
                }
            }
        }
        if ed.quit {
            return 0;
        }
        if dirty && redraw(term, ed).is_err() {
            return 1;
        }
    }
}

/// Scroll to the cursor, render the editor into the back buffer, place the
/// cursor, and flush one diff frame to fd 1.
fn redraw(term: &mut Terminal, ed: &mut Editor) -> Result<()> {
    let area = term.area();
    // One row is the status/command line; the rest is the text viewport.
    let text_h = area.height.saturating_sub(1) as usize;
    ed.scroll_to(text_h);
    let cur;
    {
        let buf = term.back_mut();
        buf.reset();
        cur = view::render(ed, area, buf);
    }
    term.set_cursor(Some(cur));
    term.flush()
}

/// Execute a file request raised by the editor (save / open), reporting the
/// outcome back through the editor's status line.
fn handle_request(ed: &mut Editor, req: Request) {
    match req {
        Request::Save(name) => {
            let path = name.or_else(|| ed.filename.clone());
            match path {
                Some(p) => {
                    let bytes = ed.text.content();
                    match write_file(&p, bytes.as_bytes()) {
                        Ok(n) => ed.mark_saved(p, n),
                        Err(e) => ed.set_status(format!("{}: {}", p, e)),
                    }
                }
                None => ed.set_status(String::from("no file name (use :w <name>)")),
            }
        }
        Request::Open(p) => match read_file(&p) {
            Ok(c) => ed.load(Some(p), &c),
            Err(e) => ed.set_status(format!("{}: {}", p, e)),
        },
    }
}

/// Read `path` as UTF-8 text, bounded by `MAX_FILE`. A non-UTF-8 or oversized
/// file is reported (the editor is text-only); the bytes are never partially
/// loaded.
fn read_file(path: &str) -> Result<String> {
    let mut f = File::open(path)?;
    let bytes = slurp_capped(&mut f, MAX_FILE)?;
    let s = core::str::from_utf8(&bytes).map_err(|_| Error::InvalidArgument)?;
    Ok(String::from(s))
}

/// Write `data` to `path` (create-or-truncate) and fsync it -- an editor save
/// must be durable, not just buffered. Returns the byte count written.
fn write_file(path: &str, data: &[u8]) -> Result<usize> {
    let mut f = File::create(path)?;
    f.write_all(data)?;
    // SAFETY: f.as_raw_fd() is a live KOBJ_SPOOR with RIGHT_WRITE (File::create
    // opened OWRITE). datasync 0 = full metadata+data barrier.
    let rc = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
    // A server without Tsync-on-this-fd may reject fsync; the bytes are still
    // written, so a fsync error is non-fatal to the save (best-effort barrier).
    let _ = Error::from_syscall_return(rc);
    Ok(data.len())
}

/// Parse `[-R] [file]` from argv operands. `-R` opens read-only (a viewer);
/// the first non-flag operand is the file to edit.
fn parse_args() -> (bool, Option<String>) {
    let mut readonly = false;
    let mut filename = None;
    for op in env::args().operands() {
        if op == b"-R" {
            readonly = true;
        } else if filename.is_none() {
            if let Ok(s) = core::str::from_utf8(op) {
                filename = Some(String::from(s));
            }
        }
    }
    (readonly, filename)
}
