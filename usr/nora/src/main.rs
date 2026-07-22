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
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::env;
use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{slurp_capped, Write};
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::{t_fsync, t_putstr};

use kaua::event::Event;
use kaua::rect::Rect;
use kaua::source::{EventSource, PollSource};
use kaua::term::Terminal;

use parley::transport::Mux;

use nora::editor::{Editor, Mode, Request};
use nora::view;

mod lsp_host;
use lsp_host::{Lsp, TAG_LSP_ERR, TAG_LSP_OUT, TAG_NOTES, TAG_STDIN};

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
// The launch CPR-probe budget. Generous enough for a hypervisor (HVF) serial
// round-trip, which is slower + dribbled vs the local console; the deadline-based
// probe (kaua::query) returns as soon as the reply lands, so this only bounds the
// wait when the terminal does NOT answer (a one-time launch cost). A late reply
// past this budget is still caught by the steady-state resize backstop
// (bug_nora_hvf_cpr_handshake).
const SIZE_QUERY_TIMEOUT_MS: u32 = 150;
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
    let probe = kaua::query::terminal_size(SIZE_QUERY_TIMEOUT_MS);
    let (cols, rows) = probe
        .size
        .map(|(c, r)| (c.clamp(MIN_DIM, MAX_DIM), r.clamp(MIN_DIM, MAX_DIM)))
        .unwrap_or((COLS, ROWS));

    let mut term = match Terminal::enter(Rect::new(0, 0, cols, rows)) {
        Ok(t) => t,
        Err(_) => {
            t_putstr("nora: cannot acquire the console screen\n");
            return 1;
        }
    };
    // Replay any keystroke typed during the launch probe so type-ahead is not
    // lost (kaua::query #117-audit F2).
    let mut src = PollSource::with_pending(probe.pending);

    // #55c: the console-resize signal. Open the editor's note queue so a
    // `tty:winch` (posted to the session pgrp when the renderer reweaves)
    // wakes the loop -> re-read /dev/winsize -> Terminal::resize (the seam
    // term.rs documented since LS-7). Opening a notes fd makes nora
    // SELF-MANAGING (LS-5): an uncaught `interrupt` note now queues here
    // instead of default-terminating -- deliberate for a fullscreen editor
    // (Ctrl-C in the raw console arrives as the 0x03 BYTE anyway [ISIG off],
    // and dying mid-edit to a stray note is exactly what an editor must not
    // do; :q is the exit). Best-effort: a failed open just leaves resize on
    // the launch size (the pre-#55 behavior). #55 audit F5: self-managing
    // also queues (instead of default-terminating) tty:hup/tty:quit -- sound
    // on the CONSOLE (no tty:hup is posted there; kill/snare are unaffected),
    // but a future nora-under-a-pts would need to re-honor tty:hup (a hangup
    // must still tear the editor down) -- a pts-scoped seam, not a console
    // defect.
    let notes = libthyla_rs::notes::Notes::open_self().ok();

    // Bring up gopls for a Go buffer. Absent / unspawnable / non-Go all mean
    // "no language server" -- a fully supported state in which nora behaves
    // exactly as it did before 8e (NORA-IDE-UX section 6: never block the UI).
    let mut lsp = match ed.filename.as_deref() {
        Some(p) if lsp_host::is_go(p) => Lsp::start(p),
        _ => None,
    };

    let code = run(&mut term, &mut src, &mut ed, &mut lsp, notes.as_ref());
    // The server must not outlive the editor: an orphaned gopls holds the
    // workspace (and its memory) for the rest of the session.
    if let Some(mut l) = lsp {
        l.shutdown();
    }
    // Explicit restore (Drop also runs it; both are idempotent).
    let _ = term.leave();
    code as i64
}

/// The event loop: poll input, dispatch keys, execute any file request, redraw
/// when the state changed. Returns the process exit code.
fn run(
    term: &mut Terminal,
    src: &mut PollSource,
    ed: &mut Editor,
    lsp: &mut Option<Lsp>,
    notes: Option<&libthyla_rs::notes::Notes>,
) -> i32 {
    if redraw(term, ed).is_err() {
        return 1;
    }
    let mut mux = Mux::new();
    loop {
        if src.is_eof() {
            return 0;
        }
        // ONE poll(2) over fd 0 and any live server pipes (8e-2). A keystroke
        // and an arriving diagnostic wake the loop identically -- there is no
        // tick, so a message nothing polls for never repaints.
        let ready = match lsp_host::poll_sources(&mut mux, lsp.as_ref(), notes.map(|n| {
            use libthyla_rs::poll::AsFd;
            n.as_raw_fd()
        })) {
            Some(r) => r,
            // fd 0 gone (no console) -> exit cleanly rather than spin.
            None => return 1,
        };
        let mut dirty = false;
        let mut saved = false;
        for r in ready {
            match r.tag {
                TAG_STDIN => {
                    // Zero timeout: the mux already established readability;
                    // PollSource still runs its own drain sweep, so the paste
                    // and split-escape handling (#106-F2 / #173) is unchanged.
                    let events = match src.poll(PollTimeout::Zero) {
                        Ok(e) => e,
                        Err(_) => return 1,
                    };
                    // A wake with no decoded key (a bare HUP) loops; is_eof
                    // breaks at the top. The console read is #811
                    // death-interruptible, so a dying nora unwinds here rather
                    // than wedging.
                    for ev in events {
                        match ev {
                            Event::Key(k) => {
                                ed.handle_key(k);
                                dirty = true;
                                if let Some(req) = ed.take_request() {
                                    saved |= matches!(req, Request::Save(_));
                                    handle_request(ed, req);
                                }
                                if ed.quit {
                                    break;
                                }
                            }
                            // A late CPR the launch probe missed under HVF (the
                            // slow serial answered after the deadline): resize
                            // to the real console, swapping the 80x24 fallback
                            // for fullscreen + repainting
                            // (bug_nora_hvf_cpr_handshake -- the steady-state
                            // backstop).
                            Event::Resize(c, r) => {
                                let c = c.clamp(MIN_DIM, MAX_DIM);
                                let r = r.clamp(MIN_DIM, MAX_DIM);
                                if (c, r) != (term.area().width, term.area().height) {
                                    term.resize(Rect::new(0, 0, c, r));
                                    dirty = true;
                                }
                            }
                            _ => {}
                        }
                    }
                }
                TAG_NOTES => {
                    // #55c: drain the queue; a tty:winch means the console
                    // reweaved -- /dev/winsize is the authoritative geometry
                    // (the renderer's grid). On the serial posture (0x0 /
                    // unreachable) fall back to the CPR re-probe: the reply
                    // rides fd 0 and the PollSource's unsolicited-CPR
                    // backstop delivers it as Event::Resize (the same arm
                    // the HVF late-reply path uses). Every other note is
                    // drained + dropped (informational; kill never surfaces
                    // through the fd; snare terminates before delivery).
                    let mut winch = false;
                    if let Some(nq) = notes {
                        while let Ok(Some(note)) = nq.try_read() {
                            if note.name == "tty:winch" {
                                winch = true;
                            }
                        }
                    }
                    if winch {
                        if let Some((c, r)) = kaua::query::read_winsize() {
                            let c = c.clamp(MIN_DIM, MAX_DIM);
                            let r = r.clamp(MIN_DIM, MAX_DIM);
                            if (c, r) != (term.area().width, term.area().height) {
                                term.resize(Rect::new(0, 0, c, r));
                                dirty = true;
                            }
                        } else {
                            kaua::query::request_resize_probe();
                        }
                    }
                }
                TAG_LSP_OUT | TAG_LSP_ERR => {
                    if let Some(l) = lsp.as_mut() {
                        dirty |= l.on_ready(&r, ed);
                    }
                }
                // Unknown tag: the fd set is ours, so this cannot happen --
                // ignore rather than assume.
                _ => {}
            }
        }
        if ed.quit {
            return 0;
        }
        if let Some(l) = lsp.as_mut() {
            // A save re-checks the file; otherwise push the buffer only at a
            // typing boundary (leaving Insert), never per keystroke -- see
            // lsp_host's module header on the byte-storm discipline. Both
            // gates are O(1) (a revision compare), so this is free when
            // nothing changed.
            if saved {
                l.on_saved(ed);
            } else if ed.mode != Mode::Insert {
                l.sync(ed);
            }
            // A key may have asked the server something (`gd` / `K` / Ctrl-N).
            // Fired here rather than in the key handler so the editor stays
            // free of the client, and always AFTER the sync above so the
            // question is asked about the text on screen.
            if let Some(req) = ed.take_lsp_request() {
                l.request(req, ed);
            }
            // A server answer can itself raise a file op -- a cross-file
            // go-to-definition asks for the target to be opened. The per-key
            // drain above only covers requests a KEY raised, so without this
            // the jump would park forever.
            if let Some(req) = ed.take_request() {
                handle_request(ed, req);
                dirty = true;
            }
            // `:e` / buffer switches change which file is on screen.
            l.open_current(ed);
            // A server that died takes its session with it: drop the handle so
            // its fds leave the poll set for good and nothing tries to talk to
            // a corpse. The editor carries on without diagnostics.
            if l.is_dead() {
                *lsp = None;
            }
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
    // The text region (above the one-row status line, below the optional tab
    // strip); soft-wrap needs the width.
    let (_, tw, text_h) = view::text_metrics(area, ed.text.line_count(), ed.show_tabs());
    ed.scroll_to(tw, text_h);
    let cur;
    {
        let buf = term.back_mut();
        buf.reset();
        cur = view::render(ed, area, buf);
    }
    // In text modes the painted block (mode colour) IS the cursor, so hide the
    // real terminal cursor; on the command line show the real cursor instead.
    term.set_cursor(if ed.block_cursor() { None } else { Some(cur) });
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
                    // Stage 6: format-on-save for Go sources -- pipe the buffer
                    // THROUGH gofmt (stdin -> stdout) BEFORE the durable write,
                    // so one write lands the formatted bytes and unformatted
                    // content never touches disk. The save is NEVER blocked:
                    // a gofmt reject (syntax error mid-edit) or an absent
                    // toolchain saves the buffer as-is.
                    let mut bytes = ed.text.content();
                    let mut fmt_note: Option<String> = None;
                    if p.ends_with(".go") {
                        match gofmt_source(&bytes) {
                            FmtOutcome::Formatted(new) => {
                                if new != bytes {
                                    ed.text.replace_content(&new);
                                    bytes = new;
                                }
                            }
                            FmtOutcome::Rejected(msg) => fmt_note = Some(msg),
                            FmtOutcome::Unavailable => {}
                        }
                    }
                    match write_file(&p, bytes.as_bytes()) {
                        Ok(n) => {
                            ed.mark_saved(p, n);
                            if let Some(msg) = fmt_note {
                                ed.set_status(format!("saved unformatted; gofmt: {}", msg));
                            }
                        }
                        Err(e) => ed.set_status(format!("{}: {}", p, e)),
                    }
                }
                None => ed.set_status(String::from("no file name (use :w <name>)")),
            }
        }
        Request::Open(p) => {
            // Open in a new buffer (T3). Mirror the launch new-file handling
            // (#114): an unresolvable path opens an empty buffer (created on
            // `:w`); a real read error is surfaced, not masked.
            let content = if !fs::exists(&p) {
                String::new()
            } else {
                match read_file(&p) {
                    Ok(c) => c,
                    Err(Error::NotFound) => String::new(),
                    Err(e) => {
                        ed.set_status(format!("{}: {}", p, e));
                        return;
                    }
                }
            };
            ed.open_buffer(Some(p), &content);
        }
        Request::ListDir(dir) => match list_dir_files(&dir) {
            Ok(names) => ed.open_file_picker(names),
            Err(e) => ed.set_status(format!("{}: {}", dir, e)),
        },
    }
}

/// List `dir`'s file entries (names, sorted; directories skipped) for the fuzzy
/// file picker (Space-f). A flat cwd listing -- a recursive walk and directory
/// navigation in the picker are documented v1.x niceties (KAUA.md seam).
fn list_dir_files(dir: &str) -> Result<Vec<String>> {
    let mut names = Vec::new();
    for ent in fs::read_dir(dir)? {
        let ent = ent?;
        if !ent.is_dir() {
            names.push(String::from(ent.file_name()));
        }
    }
    names.sort();
    Ok(names)
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

/// Where the Go toolchain ships in the default image (Stage 6); matches ut's
/// static `$path` entry. A namespace without it (a non-bake image, a confined
/// Proc) fails the spawn -> `Unavailable` -> the save proceeds unformatted.
const GOFMT_PATH: &str = "/goroot/bin/gofmt";
/// stderr drain cap: go/parser bails out after ~10 errors per file, so real
/// diagnostics are far under this (and under the 4 KiB kernel pipe ring --
/// which is why draining stdout BEFORE stderr cannot deadlock).
const GOFMT_STDERR_CAP: usize = 4096;

/// Outcome of piping a buffer through gofmt.
enum FmtOutcome {
    /// rc 0: the formatted source (may equal the input).
    Formatted(String),
    /// gofmt rejected the input (syntax error mid-edit is the normal case);
    /// the first diagnostic line, for the status bar. The save proceeds
    /// with the UNFORMATTED buffer -- a formatter must never block a save.
    Rejected(String),
    /// No usable formatter (absent binary / spawn failure / output pathology).
    /// Silently save as-is.
    Unavailable,
}

/// Format Go source by piping it THROUGH gofmt: write the buffer to the
/// child's stdin, close it, read the formatted result from stdout. No
/// tempfile, no `-w` + reload -- one durable write later lands the formatted
/// bytes, and unformatted content never touches disk.
///
/// Deadlock-freedom: gofmt slurps ALL of stdin before emitting anything
/// (processFile does io.ReadAll first), so write-all -> close -> read-all is
/// safe; stderr stays under the pipe ring (see GOFMT_STDERR_CAP).
///
/// Console discipline: all three stdio slots are Piped -- the child can never
/// scribble on nora's raw-mode alt-screen (fd 1) or read its keystrokes
/// (fd 0). nora is not self-managing (no notes fd), so a `pipe` note from a
/// died-early gofmt is dropped by the kernel and the failed write surfaces
/// here as a plain Err (LS-5: only `interrupt` default-terminates).
fn gofmt_source(input: &str) -> FmtOutcome {
    let mut child = match Command::new(GOFMT_PATH)
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            return FmtOutcome::Unavailable;
        }
    };
    // Feed the buffer; the drop closes the write end so gofmt sees EOF. A
    // write error (child died early) falls through -- the exit status decides.
    let mut stdin_ok = false;
    if let Some(mut si) = child.stdin.take() {
        stdin_ok = si.write_all(input.as_bytes()).is_ok();
    }
    // Drain stdout first (the big stream), then stderr. On overflow
    // slurp_capped errors (never truncates) -> empty -> the guards below
    // refuse the result rather than adopt a partial one.
    let out = match child.stdout.take() {
        Some(mut so) => slurp_capped(&mut so, MAX_FILE).unwrap_or_default(),
        None => Vec::new(),
    };
    let err_text = match child.stderr.take() {
        Some(mut se) => slurp_capped(&mut se, GOFMT_STDERR_CAP).unwrap_or_default(),
        None => Vec::new(),
    };
    // ALWAYS reap -- nora is long-lived; an unreaped child is a zombie leak.
    let ok = match child.wait() {
        Ok(st) => st.success() && stdin_ok,
        Err(_) => false,
    };
    if !ok {
        // First diagnostic line ("<standard input>:LINE:COL: message"),
        // clipped for the one-line status bar.
        let first = err_text.split(|&b| b == b'\n').next().unwrap_or(&[]);
        let msg = core::str::from_utf8(first).unwrap_or("(non-utf8 diagnostic)");
        let msg = if msg.is_empty() { "formatter failed" } else { msg };
        let clipped: String = msg.chars().take(120).collect();
        return FmtOutcome::Rejected(clipped);
    }
    // rc 0: adopt the output -- guarded so a pathological empty/oversized/
    // non-UTF-8 result can never clobber the user's buffer.
    if out.is_empty() && !input.is_empty() {
        return FmtOutcome::Unavailable;
    }
    match String::from_utf8(out) {
        Ok(s) => FmtOutcome::Formatted(s),
        Err(_) => FmtOutcome::Unavailable,
    }
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
