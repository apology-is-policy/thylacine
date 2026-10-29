// nora's language-server session (Stage 8e-2b; multi-language at CL-6) -- the
// binary-side glue between `parley::lsp` (the pure client) and
// `parley::transport` (the persistent child). Everything protocol-shaped lives
// in parley; everything terminal-shaped lives in main.rs; this file is the
// seam that owns the process lifetime and translates LSP coordinates into
// `nora::diag`.
//
// DESIGN NOTES a reader will want:
//
//   * LSP is language-agnostic and so is everything below this file -- parley
//     needed ZERO changes to add C/C++. A server is a row in `LANGS`: the
//     suffixes it claims, the `languageId` to call them, its binary, and its
//     workspace-root markers. Anything a new language needs beyond those four
//     facts is a bug in this file's abstraction, not a reason to fork it.
//
//   * The editor NEVER blocks on the server. Requests are fired; answers land
//     on a later poll-wake and mark the frame dirty. A server that is slow,
//     wedged, or absent costs the editor nothing -- `Lsp::start` returning
//     None is a fully supported state (no toolchain in the image, an unclaimed
//     file type, a confined namespace), and the editor behaves exactly as it
//     did before 8e.
//
//   * Document sync fires on SAVE and on leaving Insert mode, never per
//     keystroke. A full-document didChange per keypress is the byte-storm
//     NORA-IDE-UX section 7 warns about (and nora renders inside Aurora's
//     row-granular fbcon, where emitted bytes cost twice). Leaving insert is
//     the natural "I finished a thought" boundary.
//
//   * The pipe fds are registered in the SAME poll(2) as fd 0, so an arriving
//     diagnostic wakes the loop exactly like a keystroke. There is no tick: a
//     message that nothing polls for is a message that never repaints.
//
//   * A server ENDS through its own goodbye, not around it. At nora's exit
//     `shutdown` goes out and its answer is awaited, then `exit`, and the
//     server is reaped once it closes its stdout. gopls answers `shutdown`
//     only after reaping the go commands it cancels, so killing it first
//     orphans its running `go list` to init. But gopls runs `shutdown` only
//     after the handlers queued ahead of it, and `initialized` holds the queue
//     for the whole first workspace load: an answer not back in `ANSWER_GRACE`
//     escalates to closing stdin. On that EOF gopls closes its stdout FIRST,
//     then cancels the load and reaps its go commands, then exits, so nora
//     waits for the exit itself, never just the EOF. A server still running
//     `END_GRACE` after the request is killed.

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::env;
use libthyla_rs::fs;
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;
use libthyla_rs::time::{self, Duration, Instant};

use parley::lsp::{self, Action};
use parley::transport::{Mux, Ready, Server, Tag};

use nora::diag::{Diagnostics, LineDiag, Severity};
use nora::editor::{Candidate, Editor, LspRequest};
use nora::text;

/// A language server nora knows how to drive.
///
/// The protocol is language-agnostic (that is `parley`'s whole job), so a
/// server is fully described by four facts: which files it claims, what to
/// call them on the wire, what binary to run, and how to recognize a
/// workspace root. Adding a language is a row here, not a code path.
struct ServerSpec {
    /// Filename suffix -> LSP `languageId`, first match wins.
    ///
    /// These suffixes do not shadow each other (`"x.hpp".ends_with(".h")` is
    /// FALSE -- a suffix match is not a prefix match, so `.h` cannot swallow
    /// `.hpp`), so the order WITHIN this list is cosmetic. Order across
    /// `LANGS` is not: a suffix claimed by two servers goes to the first.
    exts: &'static [(&'static str, &'static str)],
    /// The server binary, absolute (spawn does no PATH search).
    bin: &'static str,
    /// Workspace-root markers, nearest ancestor wins.
    roots: &'static [&'static str],
    /// Prefix for this server's status messages.
    name: &'static str,
}

impl ServerSpec {
    /// This server's `languageId` for `path`, or None if it does not claim it.
    fn language_id(&self, path: &str) -> Option<&'static str> {
        self.exts
            .iter()
            .find(|(ext, _)| path.ends_with(ext))
            .map(|(_, id)| *id)
    }
}

/// The servers nora ships with. A file is offered to the first entry that
/// claims its suffix; an entry whose binary is absent is simply skipped, so an
/// image without a toolchain behaves exactly as it did before 8e.
static LANGS: &[ServerSpec] = &[
    // Go (8e-2). Ships beside `go`/`gofmt` on the pool (Stage 8d).
    ServerSpec {
        exts: &[(".go", "go")],
        bin: "/goroot/bin/gopls",
        roots: &["go.mod"],
        name: "gopls",
    },
    // C/C++ (CL-6), from the Clade device toolchain. `compile_commands.json`
    // first: it is the only marker that also tells clangd HOW to compile, and
    // a tree that has one wants that directory as the root even when a
    // higher CMakeLists.txt exists.
    //
    // `.h` maps to "cpp" because clangd's own default for an ambiguous header
    // is C++. The languageId is only a fallback anyway -- when a compile
    // command exists it is authoritative, so a C-only project with a
    // compile_commands.json is unaffected by this choice.
    ServerSpec {
        exts: &[
            (".cpp", "cpp"),
            (".cxx", "cpp"),
            (".cc", "cpp"),
            (".hpp", "cpp"),
            (".hxx", "cpp"),
            (".hh", "cpp"),
            (".h", "cpp"),
            (".c", "c"),
        ],
        bin: "/clade/bin/clangd",
        roots: &["compile_commands.json", ".clangd", "CMakeLists.txt"],
        name: "clangd",
    },
];

/// The server that claims `path`, if any -- the language dispatch.
fn lang_for(path: &str) -> Option<&'static ServerSpec> {
    LANGS.iter().find(|l| l.language_id(path).is_some())
}

/// How far up the tree to look for a workspace-root marker. A root is a
/// handful of components above a source file in any sane layout; the cap keeps
/// a pathological path from walking to `/` one stat at a time.
const ROOT_SEARCH_DEPTH: usize = 32;

/// How long nora's exit waits for the answer to `shutdown` before it closes the
/// server's stdin instead. Queued behind gopls's first workspace load, the
/// answer took 13.2 s for a quit 0.4 s after `initialized` (TCG); most come
/// within 3 s.
const ANSWER_GRACE: Duration = Duration::from_secs(5);
/// How long nora's exit waits, from the request, for the server to close its
/// stdout before killing it.
const END_GRACE: Duration = Duration::from_secs(30);
/// Polls in a row across which the clock reads the same, taken as the grace
/// spent: a monotonic clock that does not advance must not make a server that
/// keeps writing hold nora's exit for good.
const STALL_POLLS: u32 = 10_000;
/// A wait where there is nothing to poll: between checks that the server has
/// exited (an exit is not a pollable event), and before retrying a failed poll.
const NAP_MS: u32 = 5;

/// How nora's exit ended the server, for its exit line.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum End {
    /// `shutdown` answered, `exit` sent, stdout closed.
    Goodbye,
    /// No answer within `ANSWER_GRACE`; stdin closed, then stdout closed
    /// unanswered.
    Disconnect,
    /// Stdout closed before the answer, stdin still open: it left by itself.
    Eof,
    /// Killed at `END_GRACE`.
    Grace,
    /// A write, a read or the stream failed, and the server was killed.
    Broken,
    /// It had already died.
    Dead,
}

impl End {
    pub fn as_str(self) -> &'static str {
        match self {
            End::Goodbye => "goodbye",
            End::Disconnect => "disconnect",
            End::Eof => "eof",
            End::Grace => "grace",
            End::Broken => "broken",
            End::Dead => "dead",
        }
    }
}

/// Poll tags. fd 0 keeps tag 0 so the stdin arm reads naturally.
pub const TAG_STDIN: Tag = 0;
pub const TAG_LSP_OUT: Tag = 1;
pub const TAG_LSP_ERR: Tag = 2;
// 3, 4 are TAG_DAP_OUT / TAG_DAP_ERR (dap_host.rs, the 8f debugger).
/// #55c: the editor's note queue (tty:winch — the console resize signal).
pub const TAG_NOTES: Tag = 5;

/// A live language-server session.
///
/// One server per editor, chosen at launch from the file's suffix. A buffer of
/// a DIFFERENT language is not served (`open_current` declines it) rather than
/// restarting the server underneath the user -- the same fail-soft the
/// no-server case already gets. Swapping servers on `:e other.cpp` is the
/// obvious follow-on and needs a teardown/respawn dance, not a table change.
pub struct Lsp {
    /// Which server this is -- fixes the languageId, root markers, and the
    /// set of files it will accept for the session's lifetime.
    lang: &'static ServerSpec,
    srv: Server,
    cl: lsp::Client,
    /// The document we have told gopls about, and its version.
    open_uri: Option<String>,
    version: i64,
    /// The editor's RAW filename for `open_uri` -- the cheap gate that keeps
    /// `open_current` from absolutizing (a getcwd syscall) every loop pass.
    open_name: Option<String>,
    /// `ed.text.rev()` as of the last document sync. O(1) change detection --
    /// this runs on the typing path, so comparing whole documents here would
    /// be real work per keystroke for nothing.
    synced_rev: u64,
    /// True once a sync has happened, so revision 0 (a never-edited buffer)
    /// is distinguishable from "not yet synced".
    synced: bool,
    /// Handshake done AND `initialized` sent.
    ready: bool,
    /// The server died or its stream broke: stop registering its fds and stop
    /// talking to it. The editor keeps working.
    dead: bool,
}

impl Lsp {
    /// Spawn the server that claims `path` and fire `initialize`.
    ///
    /// `None` (never an error the user must dismiss) when there is no usable
    /// server: no entry claims the suffix, the binary is absent, the spawn was
    /// refused, or the handshake could not be written. Editing must not depend
    /// on a language server existing.
    pub fn start(path: &str) -> Option<Lsp> {
        let lang = lang_for(path)?;
        // Cheap gate: a missing binary is the common case on a non-bake image
        // (no /goroot, or no /clade), and probing it costs one stat vs a
        // failed spawn.
        if !fs::exists(lang.bin) {
            return None;
        }
        let abs = absolutize(path)?;
        let root = workspace_root(&abs, lang.roots);
        let root_uri = lsp::path_to_uri(&root);

        let mut cmd = Command::new(lang.bin);
        // No env plumbing: libthyla-rs Command has no envp (v1.0), so the
        // server inherits nora's environment wholesale -- which is what we
        // want. The 8d port proved gopls needs PATH (to resolve `go` via
        // LookPath) and CAP_CSPRNG_READ (crypto/rand at init); both arrive
        // from login through ut through nora by inheritance. A per-Command env
        // override would be a kernel-ABI item, not a workaround to invent
        // here.
        //
        // The same inheritance carries the CL-5 per-Proc PAGE BUDGET, and
        // `Command` cannot raise it (process.rs passes `page_budget: 0` =
        // inherit). clangd holds an AST plus a preamble cache, and CL-5
        // measured one template-heavy TU at 250 MiB against a 256 MiB
        // default -- so a server that dies on its first real translation unit
        // is a BUDGET symptom, not a port bug. Measure before plumbing a
        // setter (task #100).
        let srv = Server::spawn(&mut cmd).ok()?;

        let mut cl = lsp::Client::new();
        let init = cl.initialize(&root_uri);
        let mut l = Lsp {
            lang,
            srv,
            cl,
            open_uri: None,
            version: 0,
            open_name: None,
            synced_rev: 0,
            synced: false,
            ready: false,
            dead: false,
        };
        if l.send(&init).is_err() {
            return None;
        }
        Some(l)
    }

    /// The `(fd, tag)` pairs to register this round. Empty once dead, so a
    /// dead server's fds are never polled again (the rebuild-per-poll contract
    /// in `parley::transport::Mux` makes that safe by construction).
    pub fn poll_fds(&self) -> Vec<(i32, Tag)> {
        if self.dead {
            return Vec::new();
        }
        alloc::vec![
            (self.srv.stdout_fd(), TAG_LSP_OUT),
            (self.srv.stderr_fd(), TAG_LSP_ERR),
        ]
    }

    pub fn is_dead(&self) -> bool {
        self.dead
    }

    /// The server's name, for status messages.
    pub fn name(&self) -> &'static str {
        self.lang.name
    }

    fn send(&mut self, msg: &parley::json::Value) -> Result<(), ()> {
        if self.dead {
            return Err(());
        }
        match self.srv.send(msg) {
            Ok(()) => Ok(()),
            Err(_) => {
                // A broken stdin means the server is gone; do not keep writing
                // into a dead pipe on every keystroke. A dead server is dropped
                // and never polled again, so this is the last chance to reap it.
                self.reap();
                Err(())
            }
        }
    }

    /// Handle a readiness report for one of our fds. Returns true when the
    /// editor should repaint.
    pub fn on_ready(&mut self, r: &Ready, ed: &mut Editor) -> bool {
        match r.tag {
            TAG_LSP_ERR => {
                // Drain and discard: a chatty server must not fill its stderr
                // pipe and block, nor scribble the alt-screen.
                if r.readable {
                    let _ = self.srv.drain_stderr();
                }
                false
            }
            TAG_LSP_OUT => {
                let mut dirty = false;
                if r.readable {
                    match self.srv.pump() {
                        Ok(false) => {}
                        // EOF: the server exited.
                        Ok(true) => {
                            self.reap();
                            return false;
                        }
                        Err(_) => {
                            self.reap();
                            return false;
                        }
                    }
                    loop {
                        match self.srv.next_frame() {
                            Ok(Some(body)) => dirty |= self.dispatch(&body, ed),
                            Ok(None) => break,
                            // A malformed stream is unrecoverable: we cannot
                            // find the next frame boundary. Tear it down
                            // rather than resynchronize on garbage.
                            Err(_) => {
                                self.reap();
                                return dirty;
                            }
                        }
                    }
                }
                if r.hup && !self.dead {
                    self.reap();
                }
                dirty
            }
            _ => false,
        }
    }

    /// Parse + classify one framed message and apply it. Returns repaint-worthy.
    fn dispatch(&mut self, body: &[u8], ed: &mut Editor) -> bool {
        let value = match parley::json::Value::parse(body) {
            Ok(v) => v,
            // One unparseable message is not fatal -- the framing is still in
            // sync (we consumed exactly Content-Length bytes), so skip it.
            Err(_) => return false,
        };
        let msg = match parley::jsonrpc::classify(value) {
            Ok(m) => m,
            Err(_) => return false,
        };
        match self.cl.handle(msg) {
            Action::Send(reply) => {
                let _ = self.send(&reply);
                false
            }
            Action::Ready => {
                let note = self.cl.initialized();
                let _ = self.send(&note);
                self.ready = true;
                // Open the buffer we were launched on, now that the server can
                // accept it.
                self.open_current(ed);
                false
            }
            Action::Diagnostics(uri) => {
                // Only repaint for the file on screen. gopls publishes for
                // every file in the package.
                if self.open_uri.as_deref() == Some(uri.as_str()) {
                    self.publish(ed, &uri);
                    true
                } else {
                    false
                }
            }
            Action::Log(_) => false,
            Action::Failed(msg) => {
                ed.set_status(alloc::format!("{}: {}", self.lang.name, msg));
                true
            }
            Action::Hover(Some(text)) => {
                ed.show_hover(text);
                true
            }
            Action::Hover(None) => {
                ed.set_status(String::from("no hover information"));
                true
            }
            Action::Definition(Some(loc)) => {
                self.jump(ed, loc);
                true
            }
            Action::Definition(None) => {
                ed.set_status(String::from("no definition found"));
                true
            }
            Action::Completion(items) => {
                ed.show_completion(
                    items
                        .into_iter()
                        .map(|c| Candidate {
                            label: c.label,
                            detail: c.detail,
                            insert: c.insert_text,
                        })
                        .collect(),
                );
                true
            }
            // `shutdown` goes out only at exit, where `goodbye_output` reads.
            Action::ShutDown(_) | Action::Ignored => false,
        }
    }

    /// Issue a query for the cursor position (the binary hands over whatever
    /// `Editor::take_lsp_request` produced).
    ///
    /// Silently does nothing when there is no server, the handshake has not
    /// finished, or no document is open -- pressing `gd` in a buffer gopls has
    /// never seen is a no-op, not an error to dismiss.
    pub fn request(&mut self, req: LspRequest, ed: &Editor) {
        if !self.ready || self.dead {
            return;
        }
        let uri = match self.open_uri.clone() {
            Some(u) => u,
            None => return,
        };
        // The server is answering about the text it last received, so make
        // sure that is the text on screen. `sync` is O(1) when nothing changed.
        self.sync(ed);
        let pos = self.cursor_position(ed);
        let msg = match req {
            LspRequest::Definition => self.cl.definition(&uri, pos),
            LspRequest::Hover => self.cl.hover(&uri, pos),
            LspRequest::Completion => self.cl.completion(&uri, pos),
        };
        let _ = self.send(&msg);
    }

    /// The cursor as an LSP position: nora's CHARACTER column becomes a byte
    /// offset in the line, then the count the server asked for.
    fn cursor_position(&self, ed: &Editor) -> lsp::Position {
        let (row, col) = ed.text.cursor();
        let line = ed.text.line(row);
        let byte = text::char_col_to_byte(line, col);
        lsp::Position::new(row as u32, lsp::byte_to_char(line, byte, self.cl.encoding()))
    }

    /// Move the editor to a resolved definition.
    fn jump(&self, ed: &mut Editor, loc: lsp::Location) {
        let line = loc.range.start.line as usize;
        let character = loc.range.start.character;
        // Compare URIs, not paths: `ed.filename` is whatever the user typed
        // (often relative) while a server's location is absolute, so a path
        // compare would call a same-file jump "elsewhere" and re-open the file
        // we are already editing -- losing the undo history for a cursor move.
        let same_file = self.open_uri.as_deref() == Some(loc.uri.as_str());
        // `None` means "this buffer", which is exactly what a same-file jump
        // is, and it sidesteps the relative-vs-absolute comparison entirely.
        let path = if same_file {
            None
        } else {
            lsp::uri_to_path(&loc.uri)
        };
        let col = if same_file {
            // We hold the text, so the column converts exactly.
            let byte = lsp::char_to_byte(ed.text.line(line), character, self.cl.encoding());
            text::byte_to_char_col(ed.text.line(line), byte)
        } else {
            // A jump into a file we have not read yet: no line text exists to
            // convert against, so the server's offset is used as a character
            // column. Exact whenever the target line is ASCII (every ordinary
            // Go declaration) and at worst a few columns off on a line with
            // multi-byte characters before the symbol -- the LINE is always
            // right, and `set_cursor` clamps, so the miss is cosmetic.
            character as usize
        };
        ed.jump_to(path, line, col);
    }

    /// Convert the client's diagnostics for `uri` into the editor's
    /// protocol-free form and hand them over.
    ///
    /// The conversion is where the negotiated position encoding is spent: LSP
    /// `character` offsets become BYTE columns against the actual line text.
    /// A diagnostic whose line is past the end of the buffer (the server is a
    /// version behind our edits) is DROPPED rather than clamped to the last
    /// line, where it would mark innocent code.
    fn publish(&self, ed: &mut Editor, uri: &str) {
        let enc = self.cl.encoding();
        let lines = ed.text.line_count();
        let mut out: Vec<LineDiag> = Vec::new();
        for d in self.cl.diagnostics_for(uri) {
            let line = d.range.start.line as usize;
            if line >= lines {
                continue;
            }
            // TWO conversions, and both are load-bearing: the server's offset
            // is in the negotiated encoding (UTF-8 bytes or UTF-16 units),
            // while every position in nora is a CHARACTER column. Stopping at
            // the byte offset is invisible on ASCII and lands the cursor
            // inside a multi-byte character on the first accented line.
            let src = ed.text.line(line);
            let col = text::byte_to_char_col(src, lsp::char_to_byte(src, d.range.start.character, enc));
            // A span ending on a LATER line is clipped to this line's end --
            // the gutter marks the start line, which is where the message
            // belongs.
            let end_col = if d.range.end.line as usize == line {
                text::byte_to_char_col(src, lsp::char_to_byte(src, d.range.end.character, enc))
                    .max(col)
            } else {
                src.chars().count()
            };
            out.push(LineDiag {
                line,
                col,
                end_col,
                severity: match d.severity {
                    lsp::Severity::Error => Severity::Error,
                    lsp::Severity::Warning => Severity::Warning,
                    lsp::Severity::Information => Severity::Info,
                    lsp::Severity::Hint => Severity::Hint,
                },
                message: d.message.clone(),
            });
        }
        let mut dg = Diagnostics::new();
        dg.set(out);
        ed.diags = dg;
    }

    /// Tell gopls about the editor's current file (didOpen), replacing any
    /// previously-open document. A no-op until the handshake completes.
    pub fn open_current(&mut self, ed: &mut Editor) {
        if !self.ready || self.dead {
            return;
        }
        // O(1) gate: same file as last time -> nothing to do. Absolutizing
        // costs a getcwd, and this runs once per loop pass.
        if self.open_name.as_deref() == ed.filename.as_deref() {
            return;
        }
        let path = match ed.filename.as_deref().and_then(absolutize) {
            Some(p) => p,
            None => return,
        };
        // Only files THIS server claims. A Go buffer opened in a clangd
        // session (or vice versa) is declined, not mis-served.
        let language_id = match self.lang.language_id(&path) {
            Some(id) => id,
            None => {
                // Remember the miss so a foreign buffer is not re-probed every
                // pass.
                self.open_name = ed.filename.clone();
                return;
            }
        };
        let uri = lsp::path_to_uri(&path);
        if self.open_uri.as_deref() == Some(uri.as_str()) {
            self.open_name = ed.filename.clone();
            return;
        }
        if let Some(old) = self.open_uri.take() {
            let close = self.cl.did_close(&old);
            let _ = self.send(&close);
            self.cl.forget(&old);
            self.synced = false;
        }
        let text = ed.text.content();
        self.version = 1;
        let open = self.cl.did_open(&uri, language_id, self.version, &text);
        if self.send(&open).is_ok() {
            self.synced_rev = ed.text.rev();
            self.synced = true;
            self.open_uri = Some(uri);
            self.open_name = ed.filename.clone();
        }
        // Whatever was on screen belonged to the previous file.
        ed.diags.clear();
    }

    /// Push the buffer to gopls if it changed since the last sync.
    ///
    /// The caller decides WHEN (leaving Insert, or a save) -- see the module
    /// header on why this is not per-keystroke. The change test itself is O(1)
    /// (`TextBuffer::rev`), so calling this on every loop pass costs a compare
    /// and serializes the document only when it actually changed.
    pub fn sync(&mut self, ed: &Editor) {
        if !self.ready || self.dead {
            return;
        }
        let uri = match self.open_uri.clone() {
            Some(u) => u,
            None => return,
        };
        let rev = ed.text.rev();
        if self.synced && rev == self.synced_rev {
            return;
        }
        self.version += 1;
        let text = ed.text.content();
        let msg = self.cl.did_change_full(&uri, self.version, &text);
        if self.send(&msg).is_ok() {
            self.synced_rev = rev;
            self.synced = true;
        }
    }

    /// A save landed: sync, then tell gopls (it re-checks on save).
    pub fn on_saved(&mut self, ed: &Editor) {
        if !self.ready || self.dead {
            return;
        }
        self.sync(ed);
        if let Some(uri) = self.open_uri.clone() {
            let msg = self.cl.did_save(&uri, &ed.text.content());
            let _ = self.send(&msg);
        }
    }

    /// Reap a server that has already exited or whose stream broke.
    fn reap(&mut self) {
        if self.dead {
            return;
        }
        self.dead = true;
        // It may still be running (a broken stream, not an exit), so make sure
        // before reaping -- an unreaped child is a zombie for nora's lifetime,
        // and an unkilled one is an orphan after it.
        let _ = self.srv.kill();
        let _ = self.srv.wait();
    }

    /// End the server at nora exit and wait for it here, since no later
    /// poll-wake will come. Bounded by `END_GRACE`, then killed: a server
    /// ignoring the goodbye must not outlive the editor holding the workspace.
    pub fn shutdown(&mut self) -> End {
        if self.dead {
            return End::Dead;
        }
        let since = Instant::now();
        let bye = self.cl.shutdown();
        let end = match self.send(&bye) {
            Ok(()) => match self.await_close(since) {
                End::Goodbye | End::Disconnect | End::Eof if !self.await_exit(since) => End::Grace,
                e => e,
            },
            Err(()) => End::Broken,
        };
        self.reap();
        end
    }

    /// The server closed its stdout: wait out the rest of `END_GRACE` for it to
    /// exit by itself. gopls closes stdout before it shuts its session down, so
    /// a kill here could still orphan its `go list`. Counted in naps, so a clock
    /// that does not advance still ends it. Stdin is closed first, so a server
    /// that reads it for an exit cue is released. True once it has exited
    /// (reaped).
    fn await_exit(&mut self, since: Instant) -> bool {
        self.srv.close_stdin();
        let mut naps = ms_left(since, END_GRACE) / NAP_MS;
        loop {
            match self.srv.try_wait() {
                Ok(Some(_)) => {
                    self.dead = true;
                    return true;
                }
                Ok(None) if naps > 0 => {
                    naps -= 1;
                    let _ = time::sleep(Duration::from_millis(NAP_MS as u64));
                }
                _ => return false,
            }
        }
    }

    /// Serve the goodbye until the server closes its stdout. Until the answer
    /// arrives the wait is `ANSWER_GRACE`, then stdin is closed and the wait runs
    /// on to `END_GRACE`. An empty timed poll spends a grace whatever this
    /// process's clock reads, and so does a long run of polls it reads no time
    /// across.
    fn await_close(&mut self, since: Instant) -> End {
        let mut mux = Mux::new();
        let mut err_open = true;
        let mut answered = false;
        let mut disconnected = false;
        let mut last = since.elapsed();
        let mut stalls = 0u32;
        loop {
            let limit = if answered || disconnected { END_GRACE } else { ANSWER_GRACE };
            let ms = ms_left(since, limit);
            // A poll error cannot be a note's EINTR: poll does not EINTR a native
            // Proc (kernel notes.c, proc_caught_note_eintr_ready). Whatever it
            // is, nap and poll again; the graces and the stall count bound it.
            let ready = if ms == 0 {
                Some(Vec::new())
            } else {
                let err_fd = if err_open { self.srv.stderr_fd() } else { -1 };
                let fds = [(self.srv.stdout_fd(), TAG_LSP_OUT), (err_fd, TAG_LSP_ERR)];
                mux.poll(&fds, PollTimeout::Millis(ms)).ok()
            };
            let now = since.elapsed();
            if now == last {
                stalls += 1;
            } else {
                stalls = 0;
                last = now;
            }
            if stalls > STALL_POLLS || ready.as_ref().is_some_and(|r| r.is_empty()) {
                if limit == END_GRACE {
                    return End::Grace;
                }
                self.srv.close_stdin();
                disconnected = true;
                stalls = 0;
                continue;
            }
            let Some(ready) = ready else {
                let _ = time::sleep(Duration::from_millis(NAP_MS as u64));
                continue;
            };
            for r in &ready {
                if r.tag == TAG_LSP_ERR {
                    // A closed pipe polls readable forever: stop asking.
                    err_open = matches!(self.srv.drain_stderr(), Ok(false));
                } else if r.tag == TAG_LSP_OUT && self.goodbye_output(&mut answered) {
                    return if self.dead {
                        End::Broken
                    } else if answered {
                        End::Goodbye
                    } else if disconnected {
                        End::Disconnect
                    } else {
                        End::Eof
                    };
                }
            }
        }
    }

    /// Read the server's stdout during the goodbye: answer what it asks, and
    /// send `exit` once `shutdown` is answered. True once stdout has closed or
    /// the stream broke (the server is then reaped).
    fn goodbye_output(&mut self, answered: &mut bool) -> bool {
        match self.srv.pump() {
            Ok(false) => {}
            Ok(true) => return true,
            Err(_) => {
                self.reap();
                return true;
            }
        }
        loop {
            let body = match self.srv.next_frame() {
                Ok(Some(b)) => b,
                Ok(None) => return false,
                Err(_) => {
                    self.reap();
                    return true;
                }
            };
            let msg = match parley::json::Value::parse(&body) {
                Ok(v) => parley::jsonrpc::classify(v),
                Err(_) => continue,
            };
            match msg.map(|m| self.cl.handle(m)) {
                // Past `exit` stdin is closed and the write fails; stdout's EOF,
                // not a refused reply, is what ends the wait.
                Ok(Action::Send(reply)) => {
                    let _ = self.srv.send(&reply);
                }
                // An answer can come after the disconnect closed stdin; the
                // write then fails harmlessly.
                Ok(Action::ShutDown(_)) => {
                    *answered = true;
                    let exit = self.cl.exit();
                    let _ = self.srv.send(&exit);
                    self.srv.close_stdin();
                }
                _ => {}
            }
            if self.dead {
                return true;
            }
        }
    }
}

/// Milliseconds of `limit` left since `since`, rounded up so a wake is never
/// early.
fn ms_left(since: Instant, limit: Duration) -> u32 {
    let left = limit.saturating_sub(since.elapsed());
    ((left.as_nanos() + 999_999) / 1_000_000).min(u32::MAX as u128) as u32
}


/// Make `path` absolute against the cwd. `None` when the cwd is unreadable.
fn absolutize(path: &str) -> Option<String> {
    if path.starts_with('/') {
        return Some(String::from(path));
    }
    let mut cwd = env::current_dir().ok()?;
    if !cwd.ends_with('/') {
        cwd.push('/');
    }
    cwd.push_str(path);
    Some(cwd)
}

/// The workspace root for `abs_path`: the nearest ancestor directory holding
/// any of `markers`, else the file's own directory (a single-file workspace --
/// both gopls and clangd cope, with reduced results).
///
/// NEAREST ancestor, not best marker: the walk stops at the first directory
/// containing ANY marker rather than preferring a stronger marker further up.
/// That is deliberate -- an inner `compile_commands.json` describes the code
/// being edited more precisely than an outer `CMakeLists.txt` does.
fn workspace_root(abs_path: &str, markers: &[&str]) -> String {
    let mut dir = parent_of(abs_path);
    for _ in 0..ROOT_SEARCH_DEPTH {
        for m in markers {
            let mut probe = String::from(&dir);
            if !probe.ends_with('/') {
                probe.push('/');
            }
            probe.push_str(m);
            if fs::exists(&probe) {
                return dir;
            }
        }
        if dir == "/" {
            break;
        }
        let up = parent_of(&dir);
        if up == dir {
            break;
        }
        dir = up;
    }
    parent_of(abs_path)
}

/// The directory containing `path` (no trailing slash except at the root).
fn parent_of(path: &str) -> String {
    match path.rfind('/') {
        Some(0) => String::from("/"),
        Some(i) => String::from(&path[..i]),
        None => String::from("."),
    }
}
