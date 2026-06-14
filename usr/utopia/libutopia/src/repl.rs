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

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::io::Read as IoRead;
use libthyla_rs::io::Write as IoWrite;
use libthyla_rs::t_putstr;

use crate::ansi;
use crate::completion::ShellCompletionSource;
use crate::eval::{builtin, deliver_pending_notes, eval_source, Env, Value};
use crate::line_editor::{EditorAction, LineEditor};
use crate::palette::Role;

/// The Utopia read-parse-eval loop driver.
pub struct Repl {
    env: Env,
    editor: LineEditor,
    /// #115a: the cached `/bin` external-command scan (the #58 exec namespace,
    /// static for the session). Merged with the live builtins / aliases / funcs
    /// to form the Tab-completion command index. Empty until `install_completion`.
    bin_commands: Vec<String>,
    /// #115a: whether `install_completion` has run. `refresh_command_index` is a
    /// no-op until then, so host tests + the bare-spawn boot check (which never
    /// install) keep the inert-Tab behaviour.
    completion_installed: bool,
    /// #115b: the `~/.ut_history` path once `install_history` has resolved it
    /// from `$home`. `None` keeps history in-memory only (a bare-spawned `ut`
    /// with no home, host tests) -- no append happens.
    history_path: Option<String>,
    /// D4: whether a completion-menu candidate strip is currently drawn below
    /// the prompt. Set on `MenuShow`; any other editor action first clears the
    /// strip line, so a stale strip never lingers below the prompt.
    menu_shown: bool,
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
            bin_commands: Vec::new(),
            completion_installed: false,
            history_path: None,
            menu_shown: false,
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

    /// #94-B-b: record the inherited `/dev/consctl` fd that login forwarded
    /// (`--consctl-fd N`). A session `ut` -- not login -- owns the console line
    /// discipline for the session; LS-7's editor dance reads it from the Env
    /// (set cooked/raw around a foreground child). No effect beyond holding the
    /// fd until then; a fd-less boot check / host test never calls this.
    pub fn set_consctl_fd(&mut self, fd: i32) {
        self.env.consctl_fd = Some(fd);
    }

    /// #94-B-b: establish ut's prompt-mode line discipline through the
    /// inherited consctl fd -- raw byte-at-a-time, no kernel echo (the line
    /// editor draws its own), ISIG so Ctrl-C cooks to the `interrupt` note the
    /// shell services, no CR/NL translation. Returns true iff the mode write
    /// was accepted; the `ut` binary turns that into the boot-log witness that
    /// the inherited-fd consctl reach extends to a user-identity shell (the
    /// #94-B end of the gate drop). Inert false when no consctl fd was
    /// forwarded -- the shell simply runs in whatever mode login left.
    pub fn console_apply_default(&self) -> bool {
        match self.env.consctl_fd {
            // The ONE prompt-mode vocabulary lives in `eval::console` (shared with
            // the LS-7 raw-mode dance, whose restore must be byte-identical to this
            // -- defining it once removes the drift hazard).
            Some(fd) => crate::eval::console::set_mode(fd, crate::eval::console::PROMPT_MODE),
            None => false,
        }
    }

    /// #113: record the session user's home directory (login forwards it via
    /// `--home <path>`, since there is no kernel envp). Sets `$home` -- which
    /// `cd` with no argument resolves to, and the prompt abbreviates to `~` --
    /// and chdirs into it so a session `ut` starts in the user's home, syncing
    /// `$cwd` to the kernel cwd on success. A home that cannot be entered
    /// (absent / no search permission) leaves `ut` at `/` rather than failing
    /// startup; a bare-spawned `ut` (the boot check) is given no `--home` and
    /// never calls this, so it runs unchanged at `/`.
    pub fn set_home(&mut self, path: String) {
        self.env.assign("home", Value::scalar(path.clone()));
        if libthyla_rs::env::set_current_dir(&path).is_ok() {
            self.env.cwd_set(path);
        }
    }

    /// #115a: install namespace-driven Tab completion. Scans `/bin` ONCE (the
    /// #58 exec namespace -- the external-command set, static for a session) and
    /// builds the initial command index, then installs the production
    /// `ShellCompletionSource` into the editor. Called by the consumer ON-TARGET
    /// (gated on a live console, like `open_notes`) -- `new()` stays syscall-free
    /// so host tests + the bare-spawn boot check pay nothing. A failed `/bin`
    /// scan degrades to builtins + aliases + funcs only (Tab still completes
    /// those); it never fails startup. Idempotent (a re-call re-scans).
    pub fn install_completion(&mut self) {
        self.bin_commands.clear();
        if let Ok(rd) = libthyla_rs::fs::read_dir("/bin") {
            for ent in rd.flatten() {
                if ent.is_file() {
                    self.bin_commands.push(ent.into_file_name());
                }
            }
        }
        self.completion_installed = true;
        self.refresh_command_index();
    }

    /// Rebuild the Tab-completion command index (builtins + aliases + funcs +
    /// the cached `/bin` scan) and re-install it into the editor's completion
    /// source. Cheap -- no syscall (the `/bin` scan is cached in `bin_commands`),
    /// just two small map walks + a sort. Run after each accepted line so an
    /// interactively-defined `fn` / alias becomes completable at the next prompt.
    /// A no-op until `install_completion` has run.
    fn refresh_command_index(&mut self) {
        if !self.completion_installed {
            return;
        }
        let mut names: Vec<String> = Vec::new();
        for b in builtin::builtin_names() {
            names.push(String::from(*b));
        }
        for a in self.env.alias_names() {
            names.push(String::from(a));
        }
        for f in self.env.fn_names() {
            names.push(String::from(f));
        }
        names.extend(self.bin_commands.iter().cloned());
        names.sort();
        names.dedup();
        // #115c: the SAME sorted index drives command-line validity coloring.
        self.editor.set_known_commands(names.clone());
        self.editor
            .set_completion_source(Box::new(ShellCompletionSource::new(names)));
    }

    /// #115b: load + enable on-disk command history at `~/.ut_history` (the
    /// per-user encrypted home, A-5b -- private by construction). Reads any
    /// existing file into the editor's in-memory history (capped at the
    /// editor's HISTORY_CAP via `push_history` eviction, newest kept) and
    /// records the path so subsequent accepted lines are appended. Called
    /// ON-TARGET after `set_home` (gated on a live console, like
    /// `install_completion`); a home that is unset or unreadable leaves history
    /// in-memory only (no path recorded -> no append) and never fails startup.
    pub fn install_history(&mut self) {
        let path = match self.history_file_path() {
            Some(p) => p,
            None => return,
        };
        // Load existing entries (best-effort; an absent file = empty history).
        if let Ok(mut f) = libthyla_rs::fs::File::open(&path) {
            let mut text = String::new();
            if f.read_to_string(&mut text).is_ok() {
                for line in text.lines() {
                    if !line.is_empty() {
                        self.editor.push_history(String::from(line));
                    }
                }
            }
        }
        self.history_path = Some(path);
    }

    /// The history file path (`$home/.ut_history`), or `None` when `$home` is
    /// unset (a bare-spawned `ut`).
    fn history_file_path(&self) -> Option<String> {
        let mut p = self.env.get("home").as_scalar();
        if p.is_empty() {
            return None;
        }
        if !p.ends_with('/') {
            p.push('/');
        }
        p.push_str(".ut_history");
        Some(p)
    }

    /// #115b: append one accepted command line to the history file. Single-line
    /// commands only -- a multi-line command's embedded newlines would split
    /// into separate entries on reload (v1.x escapes them). A missing path
    /// (history disabled / no home) or any write failure is silently ignored:
    /// history is a convenience, never load-bearing. Append-only, so a torn
    /// write loses at most the trailing line (the file is never rewritten).
    fn append_history(&mut self, line: &str) {
        if line.contains('\n') {
            return;
        }
        let path = match &self.history_path {
            Some(p) => p,
            None => return,
        };
        if let Ok(mut f) = libthyla_rs::fs::OpenOptions::new()
            .write(true)
            .append(true)
            .create(true)
            // Owner-only (0600): command history can contain sensitive args
            // (matches bash's HISTFILE). The encrypted home (A-5b) already
            // gates access, but a per-file 0600 is the defense-in-depth norm.
            .mode(0o600)
            .open(path)
        {
            let mut buf = String::with_capacity(line.len() + 1);
            buf.push_str(line);
            buf.push('\n');
            let _ = f.write_all(buf.as_bytes());
        }
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
        // #118: abbreviate a leading $home to `~` (the canonical Pale Fire
        // display, UTOPIA-VISUAL.md 3.1). `$home` is empty for a bare-spawned
        // `ut` (no `--home`), where abbreviate_home returns the cwd unchanged.
        let home = self.env.get("home").as_scalar();
        let shown = crate::path::abbreviate_home(cwd, &home);
        let mut p = ansi::fg(Role::Path, &shown);
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
            // D4: a completion-menu strip is drawn on the line BELOW the prompt.
            // Any action that leaves the menu must clear that strip first, so it
            // never lingers under a fresh prompt / accepted line. Save cursor
            // (DECSC) -> down + clear line -> restore (DECRC), leaving the cursor
            // on the prompt line. MenuShow redraws it in place; NoChange means
            // the editor stayed in the menu (e.g. an unknown CSI byte) -- both
            // keep the strip.
            if self.menu_shown
                && !matches!(
                    action,
                    EditorAction::MenuShow { .. } | EditorAction::NoChange
                )
            {
                let _ = out.write_all(b"\x1b7\r\n\x1b[K\x1b8");
                self.menu_shown = false;
            }
            match action {
                EditorAction::NoChange => {}
                EditorAction::Redraw => {
                    self.emit_prompt(out);
                }
                EditorAction::Accept(line) => {
                    // The user pressed Enter; move the terminal past the
                    // edited line before any eval output or the next prompt.
                    let _ = out.write_all(b"\r\n");
                    // R3-F1: drain notes BEFORE evaluating the line. A Ctrl-C
                    // pressed while idle at the prompt queues an `interrupt`
                    // note; left in the queue, the next command's
                    // wait_pids_interruptible would forward that stale note to
                    // the just-spawned child and spuriously kill it. Draining
                    // here fires any `on note` handler for the idle note and
                    // discards an unhandled idle interrupt (benign at a sync
                    // point), so the new command starts from a clean queue.
                    self.deliver_notes();
                    if !line.trim().is_empty() {
                        self.editor.push_history(line.clone());
                        // #115b: persist to ~/.ut_history (no-op until
                        // install_history; single-line commands only).
                        self.append_history(&line);
                    }
                    self.run_line(&line, out);
                    // #115a: a `fn` / alias the line just defined should be
                    // completable at the next prompt -- rebuild the index (cheap;
                    // no-op until completion is installed).
                    self.refresh_command_index();
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
                    // handlers whose note arrived during the just-run command.
                    // LS-8c also delivers notes ASYNCHRONOUSLY while idle at the
                    // prompt (`on_notes_ready`); this post-command sync-point
                    // drain still catches notes that arrived while a foreground
                    // command held the loop. An idle interrupt drained here is
                    // discarded (benign at a sync point) -- the bool is ignored.
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
                EditorAction::MenuShow {
                    candidates,
                    selected,
                } => {
                    // D4: the editor has applied candidates[selected] to the
                    // buffer. Redraw the prompt+buffer line, then draw a
                    // one-line candidate strip below it (selected highlighted),
                    // restoring the cursor to the prompt line. On the next
                    // MenuShow (cycle) this redraws in place; on any other
                    // action the strip is cleared (above).
                    self.emit_prompt(out);
                    let strip = render_menu_strip(&candidates, selected);
                    let _ = out.write_all(b"\x1b7\r\n\x1b[K");
                    let _ = out.write_all(strip.as_bytes());
                    let _ = out.write_all(b"\x1b8");
                    let _ = out.flush();
                    self.menu_shown = true;
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

    /// Evaluate a whole script `src` non-interactively (the `ut SCRIPT` mode,
    /// D2). Binds the positional parameters ($0 = `arg0`, $1.. = `args`, $* =
    /// all args) at the script's global scope, switches the Env to script mode
    /// (fail-fast on a non-zero $status, scripture 8.9), evaluates the source,
    /// and returns the exit code: an explicit `exit N` wins, else the last
    /// statement's $status. A parse/eval error is reported to the UART and
    /// yields a non-zero code (the script's `$status` if it already failed,
    /// else 1). No line editor / prompt / notes loop -- a script reads no fd 0.
    pub fn run_script(&mut self, arg0: &str, args: &[String], src: &str) -> i32 {
        self.env.interactive = false;
        self.bind_positionals(arg0, args);
        if let Err(e) = eval_source(&mut self.env, src) {
            let mut msg = String::from("ut: ");
            if self.env.errstr().is_empty() {
                let _ = core::fmt::write(&mut FmtSink(&mut msg), format_args!("{:?}", e.kind));
            } else {
                msg.push_str(self.env.errstr());
            }
            msg.push('\n');
            t_putstr(&msg);
            if self.env.status() == 0 {
                self.env.status_set(1);
            }
        }
        self.exit_code()
    }

    /// Bind a script's positional parameters at the current (global) scope,
    /// mirroring `invoke_function`'s function-scope binding: $0 = the script
    /// path, $1..$N = `args`, $* = the whole args list.
    fn bind_positionals(&mut self, arg0: &str, args: &[String]) {
        self.env.let_set("0", Value::scalar(String::from(arg0)));
        for (i, a) in args.iter().enumerate() {
            let mut name = String::new();
            let _ = core::fmt::write(&mut FmtSink(&mut name), format_args!("{}", i + 1));
            self.env.let_set(name, Value::scalar(a.clone()));
        }
        self.env.let_set("*", Value::list(args.to_vec()));
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
        // R2-F3: a Ctrl-C that broke a runaway loop set the one-shot
        // interrupt flag, which unwound the eval to here. Clear it so the
        // next command starts clean, and report it in $status (130 = 128 +
        // interrupt, the shell convention).
        if self.env.take_interrupt() {
            self.env.status_set(130);
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
    /// arrived since the last prompt cycle (U-7c). Returns true iff an
    /// unhandled `interrupt` was drained (the LS-8c reactive-cancel signal):
    /// the sync-point callers in `feed` ignore it; `on_notes_ready` uses it.
    /// No-op until `open_notes` runs. Public so a probe can drive delivery
    /// directly.
    pub fn deliver_notes(&mut self) -> bool {
        deliver_pending_notes(&mut self.env)
    }

    /// The raw note-fd index, if the shell's note queue is open (`open_notes`).
    /// The `ut` binary adds this to its poll set (LS-8c) so an async note -- a
    /// finished background job, a Ctrl-C while idle -- wakes the loop. `None` on
    /// the bare-spawn boot check / host paths that never opened the queue.
    pub fn notes_fd(&self) -> Option<i32> {
        use libthyla_rs::poll::AsFd;
        self.env.notes().map(|n| n.as_raw_fd())
    }

    /// Service an async wake of the shell's note fd while idle at the prompt
    /// (LS-8c). The `ut` binary's multi-fd poll loop calls this when poll()
    /// reports the note fd ready and no foreground command is running (that
    /// path is `wait_pids_interruptible`, which forwards interrupts to the
    /// child). It reaps any finished background job (`[N]+ Done`), drains the
    /// note queue (firing `on note` handlers), and on an UNHANDLED `interrupt`
    /// abandons the in-progress edit -- the same disposition as the editor's
    /// own 0x03 Cancel -- before repainting the prompt + preserved buffer. A
    /// caught interrupt (an `on note interrupt` body) fires the handler and
    /// leaves the edit intact (the LS-5 catchable-interrupt semantics). The
    /// leading `\r\n` mirrors the Cancel/Accept idiom: it moves off the
    /// in-progress line so the notification + fresh prompt land cleanly.
    pub fn on_notes_ready(&mut self, out: &mut dyn IoWrite) {
        let _ = out.write_all(b"\r\n");
        // Reap finished bg jobs first (so an `on note child_exit` handler, if
        // any, observes current job state), matching the prompt-cycle order.
        self.reap_jobs();
        let interrupted = self.deliver_notes();
        if interrupted {
            self.editor.reset();
        }
        self.emit_prompt(out);
    }
}

/// D4: render the completion-menu candidate strip (one line, below the prompt).
/// The `selected` candidate is reverse-video highlighted. Candidates join with
/// two spaces; if they would exceed a conservative column budget, a contiguous
/// window AROUND `selected` is shown with `<`/`>` truncation markers so the
/// current pick is always visible (the editor/REPL do not know the real
/// terminal width -- 80-col is the safe assumption).
fn render_menu_strip(cands: &[String], selected: usize) -> String {
    const BUDGET: usize = 76;
    if cands.is_empty() {
        return String::new();
    }
    let sel = selected.min(cands.len() - 1);
    let widths: Vec<usize> = cands.iter().map(|c| ansi::visible_width(c)).collect();
    // Grow a window [lo, hi) outward from `sel` while it fits the budget.
    let mut lo = sel;
    let mut hi = sel + 1;
    let mut used = widths[sel];
    loop {
        let mut grew = false;
        if hi < cands.len() && used + 2 + widths[hi] <= BUDGET {
            used += 2 + widths[hi];
            hi += 1;
            grew = true;
        }
        if lo > 0 && used + 2 + widths[lo - 1] <= BUDGET {
            lo -= 1;
            used += 2 + widths[lo];
            grew = true;
        }
        if !grew {
            break;
        }
    }
    let mut out = String::new();
    if lo > 0 {
        out.push_str("< ");
    }
    for (n, i) in (lo..hi).enumerate() {
        if n > 0 {
            out.push_str("  ");
        }
        if i == sel {
            out.push_str("\x1b[7m"); // reverse video
            out.push_str(&cands[i]);
            out.push_str("\x1b[0m"); // reset (self-contained so DECRC is clean)
        } else {
            out.push_str(&cands[i]);
        }
    }
    if hi < cands.len() {
        out.push_str(" >");
    }
    out
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
    fn console_apply_default_is_inert_without_a_forwarded_fd() {
        // #94-B-b: a fd-less `ut` (the bare-spawn boot check, or any host test)
        // never received a consctl fd. console_apply_default MUST report false
        // WITHOUT touching the SVC -- the `None` arm returns before any t_write
        // -- so the shell silently runs in whatever mode it started in (no
        // panic, no false `ut: consctl ok` witness). A freshly-built Repl has
        // no consctl fd (set_consctl_fd is the only path to a `Some`). The
        // `Some(fd)` write path is exercised by the boot-log E2E witness (a host
        // test cannot drive the SVC).
        let repl = Repl::new();
        assert!(!repl.console_apply_default());
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

    #[test]
    fn prompt_abbreviates_home_to_tilde() {
        // #118: with $home set, the prompt's path segment renders ~-relative
        // (the Pale Fire display, UTOPIA-VISUAL.md 3.1), not the literal cwd.
        let mut repl = Repl::new();
        repl.env_mut()
            .assign("home", Value::scalar(String::from("/home/cora")));
        repl.env_mut().cwd_set("/home/cora/src");
        let p = repl.prompt();
        assert!(p.contains("~/src"), "prompt should abbreviate $home: {:?}", p);
        assert!(!p.contains("/home/cora/src"));
    }

    #[test]
    fn prompt_without_home_shows_cwd() {
        // A bare-spawned ut has no $home: the cwd renders verbatim (no ~).
        let mut repl = Repl::new();
        repl.env_mut().cwd_set("/etc");
        let p = repl.prompt();
        assert!(p.contains("/etc"));
        assert!(!p.contains('~'));
    }

    // LS-8c: `on_notes_ready` is the idle-prompt wake of the multi-fd poll loop.
    // Driven on the host via the deferred-note path (no kernel queue), the
    // canonical in-QEMU coverage is u-job-test (real per-Proc note queue).
    fn note(name: &str) -> libthyla_rs::notes::Note {
        libthyla_rs::notes::Note {
            name: alloc::string::String::from(name),
            arg: 0,
            from: None,
            timestamp_ns: 0,
        }
    }

    #[test]
    fn on_notes_ready_interrupt_cancels_the_edit() {
        // The note-path analogue of ctrl_c_discards_line_and_recovers: an
        // UNHANDLED `interrupt` delivered while a partial line is in the editor
        // cancels the edit; a fresh line then evaluates cleanly.
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        let _ = repl.feed(b"let before = bad", &mut sink);
        repl.env_mut().defer_note(note("interrupt"));
        repl.on_notes_ready(&mut sink);
        let _ = repl.feed(b"let after = ok\n", &mut sink);
        assert_eq!(repl.env().get("after").as_scalar(), "ok");
        assert!(!repl.env().defined("before"));
    }

    #[test]
    fn on_notes_ready_child_exit_preserves_the_edit() {
        // A non-interrupt async wake (`child_exit` -- a finished bg job) leaves
        // the in-progress edit intact: the partial line completes normally.
        let mut repl = Repl::new();
        let mut sink: Vec<u8> = Vec::new();
        let _ = repl.feed(b"let kept = ye", &mut sink);
        repl.env_mut().defer_note(note("child_exit"));
        repl.on_notes_ready(&mut sink);
        let _ = repl.feed(b"s\n", &mut sink);
        assert_eq!(repl.env().get("kept").as_scalar(), "yes");
    }

    // ----- D2: `ut SCRIPT` non-interactive execution --------------------

    #[test]
    fn run_script_binds_positional_params() {
        // $0 = script path, $1.. = args -- bound at the script's global scope
        // (read directly, independent of the `$1` reference syntax).
        let mut repl = Repl::new();
        let code = repl.run_script(
            "/s.ut",
            &[String::from("foo"), String::from("bar")],
            "let x = ok\n",
        );
        assert_eq!(code, 0);
        assert_eq!(repl.env().get("0").as_scalar(), "/s.ut");
        assert_eq!(repl.env().get("1").as_scalar(), "foo");
        assert_eq!(repl.env().get("2").as_scalar(), "bar");
    }

    #[test]
    fn run_script_returns_exit_builtin_code() {
        // An explicit `exit N` wins the script's exit code.
        let mut repl = Repl::new();
        assert_eq!(repl.run_script("/s.ut", &[], "exit 7\n"), 7);
    }

    #[test]
    fn run_script_switches_to_script_mode() {
        // Script mode (fail-fast on a non-zero $status, scripture 8.9) is the
        // opposite of the interactive REPL's policy.
        let mut repl = Repl::new();
        let _ = repl.run_script("/s.ut", &[], "let x = 1\n");
        assert!(!repl.env().interactive);
    }

    #[test]
    fn run_script_parse_error_is_nonzero_without_panic() {
        // A malformed script reports + yields a non-zero code; it must not
        // panic (no_std panic=abort would kill the shell).
        let mut repl = Repl::new();
        let code = repl.run_script("/s.ut", &[], ")\n");
        assert_ne!(code, 0);
    }

    // ----- D4: the completion-menu candidate strip --------------------------

    #[test]
    fn menu_strip_highlights_selected() {
        let c = [
            String::from("apple"),
            String::from("application"),
            String::from("apparatus"),
        ];
        let r = render_menu_strip(&c, 1);
        assert!(r.contains("\x1b[7mapplication\x1b[0m"), "highlight: {:?}", r);
        assert!(r.contains("apple") && r.contains("apparatus"));
        // All three fit the budget -> no truncation markers.
        assert!(!r.starts_with("< ") && !r.ends_with(" >"));
    }

    #[test]
    fn menu_strip_windows_around_selected_when_overflowing() {
        // Many wide candidates: the selected one stays visible + markers appear.
        let c: Vec<String> = (0..20)
            .map(|i| {
                let mut s = String::from("candidate-number-");
                if i < 10 {
                    s.push('0');
                }
                let mut n = String::new();
                let _ = core::fmt::write(&mut FmtSink(&mut n), format_args!("{}", i));
                s.push_str(&n);
                s
            })
            .collect();
        let r = render_menu_strip(&c, 15);
        assert!(
            r.contains("\x1b[7mcandidate-number-15\x1b[0m"),
            "selected visible: {:?}",
            r
        );
        assert!(r.starts_with("< "), "left truncation marker: {:?}", r);
    }
}
