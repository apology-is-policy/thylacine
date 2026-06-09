// libutopia::repl -- the read-parse-eval main loop (U-6g).
//
// === Position in the U-* arc ===
//
// U-6g is the "poll() main loop + line-editor integration" step from the
// U-6 sketch (eval/mod.rs) + UTOPIA-SHELL-DESIGN.md section 10.1. It glues
// the three earlier U-* halves into a working REPL:
//
//   U-4 line editor  --(bytes -> EditorAction)-->  this loop
//   U-5 parser       <--(accepted line)-----------  eval_source
//   U-6 evaluator    <--(parsed Script)-----------  eval_source
//
// The `ut` binary owns the actual fd-0 read (and the single-fd poll()); this
// module is the fd-agnostic core so it is testable against in-memory byte
// streams (the boot probe + host cfg(test)).
//
// === Scope boundary (binding -- do not smear later chunks in here) ===
//
//   U-6g (HERE): the read-parse-eval cycle. Feed input bytes to the line
//     editor; evaluate accepted lines via eval::eval_source; render the
//     prompt + echo + completions to the output sink; exit on Ctrl-D /
//     `exit` / input EOF. `Env::interactive == true` (scripture 8.9: at the
//     interactive prompt a non-zero $status does NOT end the session; the
//     fail-fast model is for scripts).
//
//   U-7 (later): the MULTI-FD poll() across the per-child + per-shell notes
//     fds for job control (Ctrl-C/Ctrl-Z, `&`, jobs/fg/bg, `on note` /
//     `mask note`). Per UTOPIA-SHELL-DESIGN.md section 10.2 the Ctrl-C path
//     is predicated on PTY line discipline routing `snare:int` to the notes
//     fd; that fd is what the U-7 loop adds to the poll set. At U-6g the
//     loop reads a single input fd.
//
//   U-PTY (later): the pollable line-discipline substrate -- a real `.poll`
//     hook on the slave + termios (raw/cooked, ECHO, ISIG) + per-Proc fd
//     0/1/2 inheritance. At v1.0 `/dev/cons` is a blocking-read-only Dev
//     with NO `.poll` hook (cons.c), so the `ut` loop blocks in read(); the
//     single-fd poll() there is the U-7 seam, not a load-bearing wait.
//
// === Why fd-agnostic ===
//
// `Repl::feed` consumes an already-read byte chunk and writes its rendering
// to an injected `io::Write` sink, returning `Some(exit_code)` when the
// session should end. The `ut` binary passes `io::stdout()`; `u-repl-test`
// (and the host cfg(test) below) pass a `Vec<u8>` sink and assert on the
// resulting Env state + the return value. Evaluation OUTPUT (echo, external
// command stdout) does NOT flow through the sink -- it goes to fd 1 from the
// builtin / spawned child directly; on a real terminal `out` IS fd 1 so the
// rendering and the eval output interleave correctly.

use alloc::string::String;

use libthyla_rs::io::Write as IoWrite;
use libthyla_rs::t_putstr;

use crate::ansi;
use crate::eval::{builtin, deliver_pending_notes, eval_source, Env};
use crate::line_editor::{EditorAction, LineEditor};
use crate::palette::Role;

/// The Utopia read-parse-eval loop driver.
pub struct Repl {
    env: Env,
    editor: LineEditor,
}

impl Default for Repl {
    fn default() -> Self {
        Self::new()
    }
}

impl Repl {
    /// A fresh interactive REPL. `Env::interactive` is set so a non-zero
    /// `$status` does not end the session (scripture 8.9).
    pub fn new() -> Self {
        let mut env = Env::new();
        env.interactive = true;
        Repl {
            env,
            editor: LineEditor::new(),
        }
    }

    /// Set whether external commands inherit the shell's fd 1/2 (LS-2).
    /// The session `ut` calls this with `io::stdout_is_live()` after
    /// startup -- it holds the console (login spawned it with fd 0/1/2
    /// inherited), so externals should write there (visible output). A
    /// fd-less harness leaves it false (the default), keeping the
    /// `Stdio::Piped`-then-drop convention.
    pub fn set_stdio_inherit(&mut self, v: bool) {
        self.env.stdio_inherit = v;
    }

    /// Borrow the evaluator state (tests + callers that inspect `$status`).
    pub fn env(&self) -> &Env {
        &self.env
    }

    /// Mutable evaluator state (callers that pre-seed variables / source an
    /// rc file before the loop starts).
    pub fn env_mut(&mut self) -> &mut Env {
        &mut self.env
    }

    /// The current prompt. Mirrors the default `prompt` function shipped in
    /// `/etc/utopia/utopia.rc` (UTOPIA-SHELL-DESIGN.md section 11.1):
    /// path-coloured cwd, then the glyph-orange right-tack. The render path
    /// strips the SGR escapes for width (ansi::visible_width), so the
    /// colour does not disturb cursor positioning.
    ///
    /// v1.0 emits this built-in default directly. Capturing the user's
    /// `prompt` shell function's stdout is deferred to a later U-* chunk
    /// (it needs rc loading + function-output capture).
    fn prompt(&self) -> String {
        let cwd = self.env.cwd();
        let cwd = if cwd.is_empty() { "/" } else { cwd };
        let mut p = ansi::fg(Role::Path, cwd);
        p.push_str(&ansi::fg(Role::Glyph, " \u{22a2} ")); // RIGHT TACK
        p
    }

    /// Emit the current prompt + buffer to `out`. Call once before the first
    /// `feed` to draw the initial prompt.
    pub fn draw_prompt(&self, out: &mut dyn IoWrite) {
        let _ = out.write_all(self.editor.render(&self.prompt()).as_bytes());
        let _ = out.flush();
    }

    /// Feed one chunk of input bytes through the editor + evaluator.
    ///
    /// Returns `Some(exit_code)` iff the session should terminate: the user
    /// ran `exit`, or pressed Ctrl-D on an empty buffer. The caller (the
    /// `ut` binary's read loop) treats a read of 0 bytes (input EOF) as the
    /// same terminate signal and stops calling `feed`. A normal return
    /// (`None`) means "read more input".
    pub fn feed(&mut self, input: &[u8], out: &mut dyn IoWrite) -> Option<i32> {
        for action in self.editor.feed_bytes(input) {
            match action {
                EditorAction::NoChange => {}
                EditorAction::Redraw => {
                    self.emit_prompt(out);
                }
                EditorAction::Accept(line) => {
                    // The user pressed Enter; move the terminal past the
                    // edited line before any eval output or the next prompt.
                    let _ = out.write_all(b"\r\n");
                    if !line.trim().is_empty() {
                        self.editor.push_history(line.clone());
                    }
                    self.run_line(&line, out);
                    // `exit` (directly, or via a function / sourced / eval'd
                    // sub-script) sets the exit request; honour it now.
                    if let Some(code) = self.env.exit_requested() {
                        return Some(code);
                    }
                    // U-7a: reclaim + report any background jobs that finished
                    // (including ones that exited WHILE the just-run command
                    // was foreground-waiting), bash-style: `[N]+ Done` BEFORE
                    // the fresh prompt.
                    self.reap_jobs();
                    // U-7c: drain the shell's note queue + fire any `on note`
                    // handlers whose note arrived since the last prompt cycle.
                    // The cons fd is not pollable at v1.0 (the line-edit read
                    // stays blocking until U-PTY), so notes are delivered at
                    // this sync point rather than asynchronously mid-edit.
                    self.deliver_notes();
                    self.emit_prompt(out);
                }
                EditorAction::Cancel => {
                    // Ctrl-C: discard the edit, fresh prompt. The editor has
                    // already reset its buffer.
                    let _ = out.write_all(b"\r\n");
                    self.emit_prompt(out);
                }
                EditorAction::Eof => {
                    // Ctrl-D on an empty buffer -> end the session
                    // (UTOPIA-SHELL-DESIGN.md section 10.4). Exit with the
                    // last command's status, shell tradition.
                    let _ = out.write_all(b"\r\n");
                    return Some(self.env.status());
                }
                EditorAction::ClearScreen => {
                    let _ = out.write_all(b"\x1b[2J\x1b[H");
                    self.emit_prompt(out);
                }
                EditorAction::ShowCompletions(cands) => {
                    let _ = out.write_all(b"\r\n");
                    for c in &cands {
                        let _ = out.write_all(c.as_bytes());
                        let _ = out.write_all(b"  ");
                    }
                    let _ = out.write_all(b"\r\n");
                    self.emit_prompt(out);
                }
            }
        }
        None
    }

    /// The exit code to return when the input stream ends (EOF) without an
    /// explicit `exit`: the last command's `$status`.
    pub fn exit_code(&self) -> i32 {
        self.env.exit_requested().unwrap_or_else(|| self.env.status())
    }

    fn emit_prompt(&self, out: &mut dyn IoWrite) {
        let _ = out.write_all(self.editor.render(&self.prompt()).as_bytes());
        let _ = out.flush();
    }

    /// Parse + evaluate one accepted line. An error is reported and the
    /// session continues (interactive policy, scripture 8.9). The error
    /// detail lives in `$errstr` (eval_source records parse failures there);
    /// the StatementFlow result is irrelevant at the top level.
    fn run_line(&mut self, line: &str, out: &mut dyn IoWrite) {
        if let Err(e) = eval_source(&mut self.env, line) {
            // Prefer the rich `$errstr` (set by eval_source on a parse error,
            // and by many evaluator error paths); fall back to the error
            // kind so a diagnostic always reaches the user.
            let mut msg = String::from("ut: ");
            if self.env.errstr().is_empty() {
                let _ = core::fmt::write(&mut FmtSink(&mut msg), format_args!("{:?}", e.kind));
            } else {
                msg.push_str(self.env.errstr());
            }
            msg.push_str("\r\n");
            let _ = out.write_all(msg.as_bytes());
            let _ = out.flush();
        }
    }

    /// Reclaim finished background jobs and emit their `[N]+ Done` lines
    /// (U-7a). Drives the pure `JobTable` with real reaps: WNOHANG-poll every
    /// live bg pid (the U-7-pre `wait_pid_for` ground truth), feed the
    /// results back via `mark_reaped`, then drain + print the notifications.
    ///
    /// This is the syscall-driven half of job tracking (the table itself is
    /// pure). It runs once per prompt cycle -- ONE WNOHANG poll per live pid,
    /// never a busy-loop, so it cannot starve a child (U-7-pre F1). A bg job
    /// that finishes while the user is idle at the prompt is reported at the
    /// next accepted line; the async-while-idle path (the notes fd in the
    /// poll set) lands at U-7c.
    ///
    /// Public so a non-interactive probe can drive the real reap path
    /// (`/u-job-test`); the interactive shell calls it from `feed`.
    pub fn reap_jobs(&mut self) {
        // The WNOHANG poll-and-mark half is shared with the `jobs` builtin's
        // refresh (`builtin::reap_background`); the drain-and-print half is the
        // prompt-cycle's own. One poll per live pid -- never a busy-loop.
        builtin::reap_background(&mut self.env);
        for line in self.env.jobs_mut().take_done_notifications() {
            t_putstr(&line);
        }
    }

    /// Open the shell's own note queue so `on note` / `mask note` handlers
    /// fire and (U-7c-b) Ctrl-C can be forwarded. Best-effort: a failed open
    /// simply leaves note delivery inert (the shell degrades to U-7b
    /// behaviour). Called by the consumer ON-TARGET after `new()` -- `new()`
    /// stays syscall-free so host tests + the bare-spawn boot check pay
    /// nothing. Idempotent in effect (a second open replaces the fd).
    pub fn open_notes(&mut self) {
        if let Ok(n) = libthyla_rs::notes::Notes::open_self() {
            self.env.set_notes(Some(n));
        }
    }

    /// Drain the shell's note queue + fire any `on note` handler whose note
    /// arrived since the last prompt cycle (U-7c). Sits beside `reap_jobs`
    /// in the prompt cycle: notes are delivered at this sync point because
    /// the cons fd is not pollable at v1.0 (the interactive line-edit read
    /// stays blocking until U-PTY). No-op until `open_notes` runs. Public so
    /// a probe can drive delivery directly.
    pub fn deliver_notes(&mut self) {
        deliver_pending_notes(&mut self.env);
    }
}

// core::fmt::Write adapter so run_line can format an EvalErrorKind (Debug)
// into the diagnostic String without pulling alloc::format into every path.
struct FmtSink<'a>(&'a mut String);
impl core::fmt::Write for FmtSink<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        self.0.push_str(s);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    // Feed a script through a fresh REPL, capturing rendering into a Vec.
    // Returns (exit_code_option, sink_bytes).
    fn drive(chunks: &[&[u8]]) -> (Repl, Option<i32>, Vec<u8>) {
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        let mut exit = None;
        for ch in chunks {
            if let Some(code) = repl.feed(ch, &mut sink) {
                exit = Some(code);
                break;
            }
        }
        (repl, exit, sink)
    }

    #[test]
    fn accepts_and_evaluates_assignment() {
        let (repl, exit, _) = drive(&[b"let greeting = hello\n"]);
        assert_eq!(exit, None);
        assert_eq!(repl.env().get("greeting").as_scalar(), "hello");
    }

    #[test]
    fn input_accumulates_across_feeds() {
        // A line split across two reads (the realistic byte-at-a-time / chunked
        // arrival) must accumulate in the editor before the trailing newline
        // submits it.
        let (repl, exit, _) = drive(&[b"let part", b" = abc\n"]);
        assert_eq!(exit, None);
        assert_eq!(repl.env().get("part").as_scalar(), "abc");
    }

    #[test]
    fn exit_builtin_terminates_with_code() {
        let (_, exit, _) = drive(&[b"exit 5\n"]);
        assert_eq!(exit, Some(5));
    }

    #[test]
    fn ctrl_d_on_empty_buffer_ends_session() {
        // 0x04 = Ctrl-D. On an empty buffer the editor returns Eof.
        let (repl, exit, _) = drive(&[b"\x04"]);
        assert_eq!(exit, Some(repl.env().status()));
    }

    #[test]
    fn ctrl_c_discards_line_and_recovers() {
        // 0x03 = Ctrl-C: the partial edit is discarded (no exit); a fresh line
        // then evaluates normally -- proving the editor recovered.
        let (repl, exit, _) = drive(&[b"junk text", b"\x03", b"let after = ok\n"]);
        assert_eq!(exit, None);
        assert_eq!(repl.env().get("after").as_scalar(), "ok");
        // The cancelled line left nothing behind.
        assert!(!repl.env().defined("junk"));
    }

    #[test]
    fn redraw_emits_to_the_sink() {
        // A printable keystroke produces a Redraw; the loop must render the
        // prompt + buffer to the sink.
        let (_, exit, sink) = drive(&[b"a"]);
        assert_eq!(exit, None);
        assert!(!sink.is_empty(), "Redraw should have rendered the prompt+buffer");
    }

    #[test]
    fn parse_error_does_not_end_interactive_session() {
        // A malformed line surfaces a diagnostic but the session continues
        // (scripture 8.9), and a subsequent good line still evaluates.
        let (repl, exit, _) = drive(&[b")\n", b"let recovered = yes\n"]);
        assert_eq!(exit, None);
        assert_eq!(repl.env().get("recovered").as_scalar(), "yes");
    }
}
