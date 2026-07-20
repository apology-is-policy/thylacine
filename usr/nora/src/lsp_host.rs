// nora's gopls session (Stage 8e-2b) -- the binary-side glue between
// `parley::lsp` (the pure client) and `parley::transport` (the persistent
// child). Everything protocol-shaped lives in parley; everything
// terminal-shaped lives in main.rs; this file is the seam that owns the
// process lifetime and translates LSP coordinates into `nora::diag`.
//
// DESIGN NOTES a reader will want:
//
//   * The editor NEVER blocks on gopls. Requests are fired; answers land on a
//     later poll-wake and mark the frame dirty. A gopls that is slow, wedged,
//     or absent costs the editor nothing -- `Lsp::start` returning None is a
//     fully supported state (no toolchain in the image, a non-Go file, a
//     confined namespace), and the editor behaves exactly as it did before 8e.
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

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::env;
use libthyla_rs::fs;
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;

use parley::lsp::{self, Action};
use parley::transport::{Mux, Ready, Server, Tag};

use nora::diag::{Diagnostics, LineDiag, Severity};
use nora::editor::{Candidate, Editor, LspRequest};
use nora::text;

/// Where the Go toolchain ships in the default image (Stage 8d) -- beside `go`
/// and `gofmt` on the pool, on the login PATH.
const GOPLS_PATH: &str = "/goroot/bin/gopls";

/// How far up the tree to look for the enclosing `go.mod`. A module root is a
/// handful of components above a source file in any sane layout; the cap keeps
/// a pathological path from walking to `/` one stat at a time.
const MODULE_SEARCH_DEPTH: usize = 32;

/// Poll tags. fd 0 keeps tag 0 so the stdin arm reads naturally.
pub const TAG_STDIN: Tag = 0;
pub const TAG_LSP_OUT: Tag = 1;
pub const TAG_LSP_ERR: Tag = 2;

/// A live gopls session.
pub struct Lsp {
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
    /// Spawn gopls for the workspace enclosing `path` and fire `initialize`.
    ///
    /// `None` (never an error the user must dismiss) when there is no usable
    /// server: gopls absent, the spawn refused, or the handshake could not be
    /// written. Editing must not depend on a language server existing.
    pub fn start(path: &str) -> Option<Lsp> {
        // Cheap gate first: a missing binary is the common case on a non-bake
        // image, and probing it costs one stat vs a failed spawn.
        if !fs::exists(GOPLS_PATH) {
            return None;
        }
        let abs = absolutize(path)?;
        let root = module_root(&abs);
        let root_uri = lsp::path_to_uri(&root);

        let mut cmd = Command::new(GOPLS_PATH);
        // No env plumbing: libthyla-rs Command has no envp (v1.0), so gopls
        // inherits nora's environment wholesale -- which is what we want. The
        // 8d port proved gopls needs PATH (to resolve `go` via LookPath) and
        // CAP_CSPRNG_READ (crypto/rand at init); both arrive from login
        // through ut through nora by inheritance. A per-Command env override
        // would be a kernel-ABI item, not a workaround to invent here.
        let srv = Server::spawn(&mut cmd).ok()?;

        let mut cl = lsp::Client::new();
        let init = cl.initialize(&root_uri);
        let mut l = Lsp {
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
            l.shutdown();
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

    fn send(&mut self, msg: &parley::json::Value) -> Result<(), ()> {
        if self.dead {
            return Err(());
        }
        match self.srv.send(msg) {
            Ok(()) => Ok(()),
            Err(_) => {
                // A broken stdin means the server is gone; do not keep writing
                // into a dead pipe on every keystroke.
                self.dead = true;
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
                ed.set_status(alloc::format!("gopls: {}", msg));
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
            Action::Ignored => false,
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
        if !is_go(&path) {
            // Remember the miss so a non-Go buffer is not re-probed every pass.
            self.open_name = ed.filename.clone();
            return;
        }
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
        let open = self.cl.did_open(&uri, "go", self.version, &text);
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

    /// Orderly shutdown at nora exit: the LSP goodbye, then close stdin so a
    /// well-behaved server sees EOF, then make sure it is really gone.
    ///
    /// `kill` + `wait` are unconditional on purpose. A server that ignores the
    /// protocol goodbye would otherwise outlive the editor as an orphan
    /// holding the workspace open.
    pub fn shutdown(&mut self) {
        if !self.dead {
            let bye = self.cl.shutdown();
            let _ = self.send(&bye);
            let exit = self.cl.exit();
            let _ = self.send(&exit);
            self.srv.close_stdin();
        }
        self.dead = true;
        let _ = self.srv.kill();
        let _ = self.srv.wait();
    }
}

/// Register `stdin` plus any live server fds and block for one of them.
pub fn poll_sources(mux: &mut Mux, lsp: Option<&Lsp>) -> Option<Vec<Ready>> {
    let mut fds: Vec<(i32, Tag)> = alloc::vec![(0, TAG_STDIN)];
    if let Some(l) = lsp {
        fds.extend(l.poll_fds());
    }
    mux.poll(&fds, PollTimeout::Block).ok()
}

/// Is this a Go source file (the only language 8e-2 speaks)?
pub fn is_go(path: &str) -> bool {
    path.ends_with(".go")
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

/// The workspace root for `abs_path`: the nearest ancestor directory holding a
/// `go.mod`, else the file's own directory (a single-file workspace -- gopls
/// copes, with reduced results).
fn module_root(abs_path: &str) -> String {
    let mut dir = parent_of(abs_path);
    for _ in 0..MODULE_SEARCH_DEPTH {
        let mut probe = String::from(&dir);
        if !probe.ends_with('/') {
            probe.push('/');
        }
        probe.push_str("go.mod");
        if fs::exists(&probe) {
            return dir;
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
