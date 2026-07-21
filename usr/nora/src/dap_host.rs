// nora's Ambush session (Stage 8e-3e) -- the binary-side glue between
// `parley::dapc` (the pure DAP client) and `parley::transport` (the persistent
// child). The DAP twin of `lsp_host.rs`: everything protocol-shaped lives in
// parley, everything terminal-shaped lives in main.rs, this file owns the
// debugger process lifetime and turns DAP state into a status line / scratch
// buffer.
//
// HEADLESS BY DESIGN (NORA-IDE-UX section 9): the debugger is driven from `:`
// commands and reports through the status line (state, stops, evaluate results)
// and a `*backtrace*` scratch buffer (the call stack). The dashboard -- the
// Variables / Call-Stack / Console tiles -- is 8f; this sub-chunk proves the
// debugger LOOP inside the editor first.
//
// DESIGN NOTES a reader will want:
//
//   * The editor NEVER blocks on Ambush. A `:cont` fires a request and returns;
//     the `stopped` event lands on a later poll-wake and marks the frame dirty.
//     There is no tick, so an event nothing polls for never repaints -- the DAP
//     pipe fds are registered in the SAME poll(2) as fd 0 (main.rs), exactly
//     like gopls's.
//
//   * The launch is a HANDSHAKE, not a single call: initialize -> (Initialized)
//     -> launch(stopOnEntry) -> (the `initialized` EVENT) -> breakpoints +
//     configurationDone -> the entry stop. The user's `:break`/`:cont` verbs
//     interleave with it; a `:break` issued before the session can accept
//     breakpoints is QUEUED and sent at the `initialized` event.
//
//   * On every stop the host auto-fetches the call stack: it gives both the
//     "stopped at <func>" status and the frame id that `:print` evaluates in,
//     and caches the frames for `:bt`. No live-watch faster than a step
//     (NORA-IDE-UX section 7): the stack refreshes on stop, never continuously.

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use libthyla_rs::fs;
use libthyla_rs::process::Command;

use parley::dap;
use parley::dapc::{self, Action, StackFrame};
use parley::transport::{Ready, Server, Tag};

use nora::editor::{DapRequest, Editor};

/// Where the Ambush debugger ships in the default image (Stage 8e-3e) -- baked
/// to the pool beside the Go toolchain (`/goroot/bin`, on the login PATH), like
/// gopls: a post-login dev tool coupled to the toolchain it debugs, disk-backed
/// so its bytes never bloat the memory-resident initrd.
const AMBUSH_PATH: &str = "/goroot/bin/ambush";

/// The DAP adapter type reported to Ambush (`"go"`).
const ADAPTER: &str = "go";

/// How many stack frames to fetch on a stop -- enough for a full `:bt` on any
/// realistic Go stack, bounded so a runaway unwinder cannot flood the pipe.
const MAX_FRAMES: i64 = 64;

/// The scratch buffer `:bt` writes the call stack into.
const BACKTRACE_BUF: &str = "*backtrace*";

/// Poll tags. fd 0 is TAG_STDIN (0) and gopls owns 1/2 (lsp_host); the DAP pipes
/// take 3/4 so all four sources share one poll(2).
pub const TAG_DAP_OUT: Tag = 3;
pub const TAG_DAP_ERR: Tag = 4;

/// Where the session is in the launch + run cycle.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    /// Sent `initialize`, awaiting its response.
    Initializing,
    /// Sent `launch`, awaiting the `initialized` event.
    Launching,
    /// Sent `configurationDone`, awaiting the entry stop.
    Configuring,
    /// Continued or stepping, awaiting the next stop.
    Running,
    /// The target is halted; inspect / step / continue.
    Stopped,
    /// The debuggee finished; `:debug` restarts.
    Exited,
}

/// A live Ambush session.
pub struct Dap {
    srv: Server,
    cl: dapc::Client,
    phase: Phase,
    /// The debuggee path, for status messages.
    program: String,
    /// True once the `initialized` event arrived -- breakpoints may be sent.
    /// Before it, a `:break` is queued in `func_bps` and flushed then.
    configured: bool,
    /// The stopped thread and its top frame (valid in `Phase::Stopped`).
    thread_id: i64,
    frame_id: i64,
    /// The frames from the last stop, for `:bt` (refreshed each stop).
    frames: Vec<StackFrame>,
    /// The reason from the last `stopped` event, held until the auto-stack lands
    /// so the status can read "stopped: <reason> at <func>".
    last_reason: String,
    /// The expression a pending `:print` asked about (evaluate carries the
    /// result, not the request), so the reply can read "<expr> = <value>".
    print_expr: Option<String>,
    /// The accumulated FUNCTION breakpoints. DAP `setFunctionBreakpoints`
    /// REPLACES the whole set each call, so the host keeps the list and re-sends
    /// it in full on every change.
    func_bps: Vec<String>,
    /// Source-line breakpoints as `(file, line)`. `setBreakpoints` replaces a
    /// FILE's set, so a new line breakpoint re-sends all of that file's lines.
    line_bps: Vec<(String, i64)>,
    /// The session died / its stream broke: stop polling it and drop it. The
    /// editor keeps working.
    dead: bool,
}

impl Dap {
    /// Spawn Ambush for `program` and fire `initialize`. Sets the editor status
    /// either way, so the caller does not.
    ///
    /// `None` (a reported state, not an error to dismiss) when the debugger is
    /// not installed or the spawn/handshake could not start -- editing is
    /// unaffected.
    pub fn start(program: &str, ed: &mut Editor) -> Option<Dap> {
        if !fs::exists(AMBUSH_PATH) {
            ed.set_status(format!(
                "debugger not installed ({} absent)",
                AMBUSH_PATH
            ));
            return None;
        }
        let mut cmd = Command::new(AMBUSH_PATH);
        cmd.arg("dap-stdio");
        // No env plumbing: Ambush inherits nora's environment wholesale (v1.0
        // Command has no envp), which is what we want -- Ambush needs the same
        // PATH + CAP_CSPRNG_READ gopls needs, all arriving login->ut->nora by
        // inheritance (the 8d finding).
        let srv = match Server::spawn(&mut cmd) {
            Ok(s) => s,
            Err(_) => {
                ed.set_status(String::from("could not start the debugger"));
                return None;
            }
        };
        let mut cl = dapc::Client::new();
        let init = cl.initialize(ADAPTER);
        let mut d = Dap {
            srv,
            cl,
            phase: Phase::Initializing,
            program: program.to_string(),
            configured: false,
            thread_id: 0,
            frame_id: 0,
            frames: Vec::new(),
            last_reason: String::new(),
            print_expr: None,
            func_bps: Vec::new(),
            line_bps: Vec::new(),
            dead: false,
        };
        if d.send(&init).is_err() {
            d.shutdown();
            ed.set_status(String::from("debugger handshake failed"));
            return None;
        }
        ed.set_status(format!("debugging {} ...", program));
        Some(d)
    }

    /// The `(fd, tag)` pairs to register this round -- empty once dead, so a
    /// dead session's fds are never polled again (safe by construction: the mux
    /// rebuilds its poll set each round).
    pub fn poll_fds(&self) -> Vec<(i32, Tag)> {
        if self.dead {
            return Vec::new();
        }
        alloc::vec![
            (self.srv.stdout_fd(), TAG_DAP_OUT),
            (self.srv.stderr_fd(), TAG_DAP_ERR),
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
                self.dead = true;
                Err(())
            }
        }
    }

    /// Handle a readiness report for one of the DAP fds. Returns true when the
    /// editor should repaint. Mirrors `Lsp::on_ready`.
    pub fn on_ready(&mut self, r: &Ready, ed: &mut Editor) -> bool {
        match r.tag {
            TAG_DAP_ERR => {
                if r.readable {
                    let _ = self.srv.drain_stderr();
                }
                false
            }
            TAG_DAP_OUT => {
                let mut dirty = false;
                if r.readable {
                    match self.srv.pump() {
                        Ok(false) => {}
                        // EOF or a read error: the debugger exited.
                        Ok(true) | Err(_) => {
                            ed.set_status(String::from("debugger exited"));
                            self.reap();
                            return true;
                        }
                    }
                    loop {
                        match self.srv.next_frame() {
                            Ok(Some(body)) => dirty |= self.dispatch(&body, ed),
                            Ok(None) => break,
                            // A malformed stream is unrecoverable -- we cannot
                            // find the next frame boundary; tear it down.
                            Err(_) => {
                                ed.set_status(String::from("debugger stream error"));
                                self.reap();
                                return true;
                            }
                        }
                    }
                }
                if r.hup && !self.dead {
                    ed.set_status(String::from("debugger exited"));
                    self.reap();
                    dirty = true;
                }
                dirty
            }
            _ => false,
        }
    }

    /// Parse + classify one framed DAP message and act on it.
    fn dispatch(&mut self, body: &[u8], ed: &mut Editor) -> bool {
        let value = match parley::json::Value::parse(body) {
            Ok(v) => v,
            // Framing is still in sync (exactly Content-Length bytes consumed);
            // skip one unparseable message rather than resynchronizing.
            Err(_) => return false,
        };
        let msg = match dap::classify(value) {
            Ok(m) => m,
            Err(_) => return false,
        };
        let act = self.cl.handle(msg);
        self.on_action(act, ed)
    }

    /// Advance the session for one classified action. Returns repaint-worthy.
    fn on_action(&mut self, act: Action, ed: &mut Editor) -> bool {
        match act {
            // A reverse-request we declined -- forward the reply so Ambush does
            // not block (it should never send one; we advertised no support).
            Action::Send(reply) => {
                let _ = self.send(&reply);
                false
            }

            // The `initialize` RESPONSE: launch the target, stopping at entry so
            // breakpoints can be armed before it runs.
            Action::Initialized(_) => {
                let launch = self.cl.launch("exec", &self.program, true);
                if self.send(&launch).is_ok() {
                    self.phase = Phase::Launching;
                }
                false
            }

            // The `initialized` EVENT: the server is ready for configuration.
            // Flush any queued breakpoints, then configurationDone.
            Action::ConfigureBreakpoints => {
                self.configured = true;
                if !self.func_bps.is_empty() {
                    // Direct field access so the borrow checker sees `func_bps`
                    // (read) and `cl` (mutated) are disjoint fields.
                    let refs: Vec<&str> = self.func_bps.iter().map(|s| s.as_str()).collect();
                    let msg = self.cl.set_function_breakpoints(&refs);
                    let _ = self.send(&msg);
                }
                self.send_all_line_breakpoints();
                let done = self.cl.configuration_done();
                let _ = self.send(&done);
                self.phase = Phase::Configuring;
                false
            }

            // A breakpoint set was verified.
            Action::Breakpoints(bl) => {
                let ok = bl.iter().filter(|b| b.verified).count();
                ed.set_status(format!("breakpoints: {}/{} verified", ok, bl.len()));
                true
            }

            Action::Stopped(s) => {
                self.thread_id = s.thread_id;
                self.last_reason = s.reason.clone();
                self.phase = Phase::Stopped;
                // Auto-fetch the stack: it yields the stop location + the frame
                // id `:print` needs, and caches the frames for `:bt`.
                let st = self.cl.stack_trace(self.thread_id, MAX_FRAMES);
                let _ = self.send(&st);
                // A preliminary status in case the stack fetch turns up nothing.
                ed.set_status(format!("stopped: {}", s.reason));
                true
            }

            Action::StackTrace(frames) => {
                // The auto-stack after a stop. If the user continued in the tiny
                // window before it landed (phase is now Running), it is stale --
                // drop it so a "stopped at ..." status never paints over a
                // running target; the next stop re-fetches.
                if self.phase != Phase::Stopped {
                    return false;
                }
                self.frames = frames;
                if let Some(top) = self.frames.first() {
                    self.frame_id = top.id;
                    ed.set_status(format!(
                        "stopped: {} at {}{}",
                        self.last_reason,
                        top.name,
                        location_suffix(top)
                    ));
                }
                true
            }

            Action::Evaluate(er) => {
                let expr = self.print_expr.take().unwrap_or_default();
                ed.set_status(format!("{} = {}", expr, er.result));
                true
            }

            // The debuggee's own output -- surface it, clearly labelled and
            // one line (the Program console is 8f).
            Action::Output(o) => {
                let line: String = o.output.trim_end().chars().take(200).collect();
                if !line.is_empty() {
                    ed.set_status(format!("[program] {}", line));
                    return true;
                }
                false
            }

            Action::Exited(code) => {
                self.phase = Phase::Exited;
                ed.set_status(format!("debuggee exited (code {})", code));
                true
            }

            Action::Terminated => {
                ed.set_status(String::from("debug session ended"));
                self.reap();
                true
            }

            Action::Failed(msg) => {
                ed.set_status(format!("debug: {}", msg));
                true
            }

            // continued / thread / scopes / variables / threads / acks: no
            // headless surface consumes them at 8e-3e.
            _ => false,
        }
    }

    /// Act on a user `:` debug command. `Launch` is handled by the binary (it
    /// creates the session); everything here operates on the live one.
    pub fn command(&mut self, req: DapRequest, ed: &mut Editor) {
        match req {
            // Handled at the binary level; a defensive no-op if it reaches here.
            DapRequest::Launch(_) => {}

            DapRequest::Break(spec) => self.set_breakpoint(&spec, ed),

            DapRequest::Continue => self.resume("continue", ed, |c, t| c.continue_(t)),
            DapRequest::Next => self.resume("stepping over", ed, |c, t| c.next(t)),
            DapRequest::Step => self.resume("stepping into", ed, |c, t| c.step_in(t)),
            DapRequest::StepOut => self.resume("stepping out", ed, |c, t| c.step_out(t)),

            DapRequest::Backtrace => {
                if self.frames.is_empty() {
                    ed.set_status(String::from("no stack (run to a breakpoint first)"));
                } else {
                    ed.open_scratch(BACKTRACE_BUF, &self.format_backtrace());
                }
            }

            DapRequest::Print(expr) => {
                if self.phase != Phase::Stopped || self.frames.is_empty() {
                    ed.set_status(String::from("not stopped (nothing to evaluate)"));
                    return;
                }
                let msg = self.cl.evaluate(&expr, self.frame_id, "repl");
                if self.send(&msg).is_ok() {
                    self.print_expr = Some(expr);
                }
            }

            DapRequest::Kill => {
                self.shutdown();
                ed.set_status(String::from("debug session ended"));
            }
        }
    }

    /// Add + arm a breakpoint. A `<file>:<line>` spec is a source-line
    /// breakpoint; anything else is a function-name breakpoint (the E2E-verified
    /// path -- Thylacine resolves these to entry PCs and arms kernel hardware
    /// breakpoints).
    fn set_breakpoint(&mut self, spec: &str, ed: &mut Editor) {
        if self.dead || self.phase == Phase::Exited {
            ed.set_status(String::from("no live debuggee (:debug to start)"));
            return;
        }
        if let Some((file, line)) = parse_file_line(spec) {
            if !self.line_bps.iter().any(|(f, l)| f == file && *l == line) {
                self.line_bps.push((file.to_string(), line));
            }
            if self.configured {
                self.send_line_breakpoints_for(file);
                ed.set_status(format!("breakpoint set: {}", spec));
            } else {
                ed.set_status(format!("breakpoint queued: {}", spec));
            }
            return;
        }
        if !self.func_bps.iter().any(|f| f == spec) {
            self.func_bps.push(spec.to_string());
        }
        if self.configured {
            let refs: Vec<&str> = self.func_bps.iter().map(|s| s.as_str()).collect();
            let msg = self.cl.set_function_breakpoints(&refs);
            let _ = self.send(&msg);
            ed.set_status(format!("breakpoint set: {} ({} total)", spec, self.func_bps.len()));
        } else {
            ed.set_status(format!("breakpoint queued: {}", spec));
        }
    }

    /// Issue an execution-control request; requires a stopped target.
    fn resume(
        &mut self,
        verb: &str,
        ed: &mut Editor,
        build: impl FnOnce(&mut dapc::Client, i64) -> parley::json::Value,
    ) {
        if self.phase != Phase::Stopped {
            ed.set_status(String::from("not stopped (nothing to run)"));
            return;
        }
        let msg = build(&mut self.cl, self.thread_id);
        if self.send(&msg).is_ok() {
            self.phase = Phase::Running;
            ed.set_status(verb.to_string());
        }
    }

    /// Re-send `setBreakpoints` for `file` with all its accumulated lines (DAP
    /// replaces a file's whole set each call).
    fn send_line_breakpoints_for(&mut self, file: &str) {
        let lines: Vec<i64> =
            self.line_bps.iter().filter(|(f, _)| f == file).map(|(_, l)| *l).collect();
        let msg = self.cl.set_breakpoints(file, &lines);
        let _ = self.send(&msg);
    }

    /// Flush every file's line breakpoints (at the `initialized` event).
    fn send_all_line_breakpoints(&mut self) {
        let files: Vec<String> = {
            let mut fs: Vec<String> = self.line_bps.iter().map(|(f, _)| f.clone()).collect();
            fs.dedup();
            fs
        };
        for f in files {
            self.send_line_breakpoints_for(&f);
        }
    }

    /// Format the cached stack for the `*backtrace*` scratch buffer.
    fn format_backtrace(&self) -> String {
        let mut s = format!("Backtrace ({}):\n\n", self.program);
        for (i, f) in self.frames.iter().enumerate() {
            s.push_str(&format!("#{:<3} {}{}\n", i, f.name, location_suffix(f)));
        }
        s
    }

    /// Reap a session that already exited or whose stream broke.
    fn reap(&mut self) {
        if self.dead {
            return;
        }
        self.dead = true;
        let _ = self.srv.kill();
        let _ = self.srv.wait();
    }

    /// Orderly shutdown (`:kill` / nora exit): DAP disconnect+terminate, close
    /// stdin so Ambush sees EOF, then make sure it is gone. `kill`+`wait` are
    /// unconditional -- a debugger ignoring the goodbye must not outlive the
    /// editor holding its debuggee.
    pub fn shutdown(&mut self) {
        if !self.dead {
            let bye = self.cl.disconnect(true);
            let _ = self.send(&bye);
            self.srv.close_stdin();
        }
        self.dead = true;
        let _ = self.srv.kill();
        let _ = self.srv.wait();
    }
}

/// `" (main.go:12)"` for a frame with source, else `""`. The path is reduced to
/// its basename to fit the one-line status.
fn location_suffix(f: &StackFrame) -> String {
    match &f.source_path {
        Some(p) => format!(" ({}:{})", basename(p), f.line),
        None => String::new(),
    }
}

/// The final path component (`/x/main.go` -> `main.go`).
fn basename(path: &str) -> &str {
    match path.rfind('/') {
        Some(i) => &path[i + 1..],
        None => path,
    }
}

/// Parse a `file:line` breakpoint spec: `Some((file, line))` when the text after
/// the LAST colon is a positive integer and a non-empty file precedes it, else
/// `None` (a function-name breakpoint). `main.parkLoop` -> None (no digits after
/// the colon); `main.go:12` -> Some(("main.go", 12)).
fn parse_file_line(spec: &str) -> Option<(&str, i64)> {
    let (file, num) = spec.rsplit_once(':')?;
    if file.is_empty() || num.is_empty() || !num.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let line: i64 = num.parse().ok()?;
    if line <= 0 {
        return None;
    }
    Some((file, line))
}
