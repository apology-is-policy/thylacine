// dap-probe -- the Stage 8e-3 LIVE DAP round-trip E2E (boot-fatal where Ambush
// is baked).
//
// WHAT WAS MISSING. parley::dap + parley::dapc shipped with 22 unit tests, and
// every one feeds the client a message the TEST ITSELF wrote. The DAP protocol
// machinery was proven ON DEVICE only by `ambush dap-selftest` -- an IN-PROCESS
// round-trip (a dap.Server + a daptest.Client over a Go net.Pipe), which never
// crosses a real process boundary, never frames a byte over a pipe, and drives
// the session from Go, not from our client.
//
// So parley's DAP client had been validated exclusively against its own
// assumptions about the server. This probe closes that gap the way lsp-probe
// closed it for gopls: it spawns the REAL `/bin/ambush dap-stdio` over piped stdio
// (the parley::transport::Server the editor uses), then drives the canonical
// VS-Code launch sequence against `/bin/ambush-child` entirely through parley::dapc:
//
//   1. initialize            -> Action::Initialized (ambush accepted our caps)
//   2. launch(exec, child)   -> Action::ConfigureBreakpoints (the `initialized`
//                               event: ambush is ready for breakpoint config)
//   3. setFunctionBreakpoints[main.parkLoop] + configurationDone
//   4. stopped(entry) -> continue -> stopped, until the stack shows main.parkLoop
//      (a bounded retry -- a HW breakpoint may stop for another reason first,
//      exactly as the dap-selftest tolerates)
//   5. scopes -> variables -> evaluate(main.Sentinel), asserting the exact value
//      Ambush reads back from the target's memory (0x0AABB00DCAFE0001).
//   6. disconnect(terminateDebuggee) -> its response. Delve answers only after
//      its kill of the launched child returned and its blocking reap collected
//      the zombie, and pids are never reused, so the child's /proc entry must
//      already be gone; then ambush leaves by itself with status 0.
//   7. A second session, launched and then abandoned: ambush is killed with no
//      disconnect and no detach, the way a crashed debugger dies. The kernel's
//      EXITKILL (I-39 die-with-launcher, DEBUG-FS-DESIGN 5d) must end the
//      stopped child, so /proc/<pid>/status reads ZOMBIE with signal 9. Nothing
//      else can end it -- killgrp ends ambush's own thread group only, the
//      child parks forever, and the `stop` ambush sent at launch converted the
//      birth hold, whose orphan rule would kill it with the same signal -- so
//      that zombie is EXITKILL's. joey's orphan sweep reaps it after boot.
//
// Before 6 and 7 existed the probe killed ambush straight after sending the
// disconnect, so EXITKILL, unasserted, ended every launched child, and a broken
// teardown read exactly like a working one in the boot log.
//
// This proves parley::dap classifies real Ambush frames, parley::dapc sequences
// the launch handshake against a real backend, and the stdio transport carries
// DAP as faithfully as it carries LSP -- the substrate the Nora debugger rides.
//
// Pure userspace: the kernel is byte-unchanged. joey spawns + reaps it and gates
// the boot on exit 0 (a fork-absent build SKIPs).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::{String, ToString};

use libthyla_rs::err::Error;
use libthyla_rs::io::Read;
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::{Command, ExitStatus};
use libthyla_rs::time::{sleep, Duration, Instant};
use libthyla_rs::{fs, t_exits, t_putstr};

use parley::dap;
use parley::dapc::{self, Action};
use parley::transport::{Mux, Server, Tag};

/// The debugger + its target, both baked into the image (Stage 8c-1).
const AMBUSH: &str = "/bin/ambush";
const CHILD: &str = "/bin/ambush-child";

/// The non-inlined park function `main.parkLoop` -- a name-based breakpoint that
/// resolves to an entry PC and arms a kernel hardware breakpoint.
const BP_FUNC: &str = "main.parkLoop";
/// The target's known global, `var Sentinel int64 = 0x0AABB00DCAFE0001`.
const SENTINEL_DEC: &str = "768901734683508737";

/// Total budget for the whole session: launch spawns + REVENANT-execs the Go
/// child, ambush attaches + arms the HW breakpoint, continue runs into it, then
/// the inspect chain. The dap-selftest allows ~30s in-process; the stdio path
/// adds only pipe latency, but a generous ceiling keeps a slow boot from a false
/// fail.
const BUDGET_MS: u64 = 90_000;
/// One poll wait -- short enough that the heartbeat stays useful.
const POLL_MS: u32 = 2_000;
/// Print progress this often so a slow launch is distinguishable from a hang.
const HEARTBEAT_MS: u64 = 15_000;
/// How many times to `continue` looking for the parkLoop frame before giving up.
const MAX_CONTINUE: u32 = 4;
/// How often to re-read the abandoned debuggee's status while EXITKILL ends it.
const DEATH_POLL_MS: u64 = 10;

const TAG_OUT: Tag = 1;
const TAG_ERR: Tag = 2;

/// Where we are in the launch + inspect sequence.
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    /// Sent `initialize`, awaiting its response.
    Init,
    /// Sent `launch`, awaiting the `initialized` event.
    Launching,
    /// Sent breakpoints + `configurationDone`, awaiting the entry stop.
    Configuring,
    /// Sent `continue`, awaiting the next stop.
    RunningToBp,
    /// Sent `stackTrace`, awaiting the frames.
    Stack,
    /// Sent `scopes`, awaiting them.
    Scopes,
    /// Sent `variables`, awaiting them.
    Vars,
    /// Sent `evaluate(main.Sentinel)`, awaiting the value.
    Eval,
    /// The sentinel checked out.
    Done,
    /// Sent `disconnect(terminateDebuggee)`, awaiting its response.
    Disconnecting,
    /// ambush answered the disconnect.
    Disconnected,
    /// The abandoned session: the target is launched and stopped, and the
    /// session configures nothing more.
    Launched,
}

/// What one handled action did to the session.
enum Progress {
    /// Keep polling.
    Wait,
    /// A step failed for this reason (boot-fatal).
    Fail(String),
}

struct Session {
    client: dapc::Client,
    phase: Phase,
    /// Stop at `Launched` instead of setting the breakpoint (step 7).
    abandon: bool,
    /// The debuggee's pid, from ambush's `process` event; 0 until it arrives.
    child: i64,
    thread_id: i64,
    frame_id: i64,
    continues: u32,
}

impl Session {
    fn new(abandon: bool) -> Session {
        Session {
            client: dapc::Client::new(),
            phase: Phase::Init,
            abandon,
            child: 0,
            thread_id: 0,
            frame_id: 0,
            continues: 0,
        }
    }

    /// The inspect chain passed, so the debuggee's end is now expected.
    fn ending(&self) -> bool {
        matches!(self.phase, Phase::Done | Phase::Disconnecting | Phase::Disconnected)
    }

    /// Advance the sequence for one classified action. Returns whether to keep
    /// waiting, that we passed, or a fatal reason. Every `send` failure is fatal:
    /// a broken pipe to ambush means the session is dead.
    fn on_action(&mut self, act: Action, srv: &mut Server) -> Progress {
        match act {
            // A reverse-request we declined -- forward the reply so ambush does
            // not block on it (it should never send one; we advertised no
            // support).
            Action::Send(reply) => match srv.send(&reply) {
                Ok(()) => Progress::Wait,
                Err(_) => fatal("send reverse-request reply"),
            },

            Action::Initialized(_) if self.phase == Phase::Init => {
                t_putstr("dap-probe: initialize OK -- ambush accepted our capabilities\n");
                // Launch the prebuilt Go target, stopping at entry so we can
                // arm the breakpoint before it runs.
                let launch = self.client.launch("exec", CHILD, true);
                if srv.send(&launch).is_err() {
                    return fatal("send launch");
                }
                self.phase = Phase::Launching;
                Progress::Wait
            }

            // The launched debuggee's pid: the subject of the teardown checks.
            Action::Process { pid } => {
                if pid <= 0 || pid > i32::MAX as i64 {
                    return Progress::Fail(String::from("process event without a usable pid"));
                }
                self.child = pid;
                Progress::Wait
            }

            Action::ConfigureBreakpoints if self.phase == Phase::Launching && self.abandon => {
                self.phase = Phase::Launched;
                Progress::Wait
            }

            Action::ConfigureBreakpoints if self.phase == Phase::Launching => {
                // The server is ready for breakpoint configuration.
                let bps = self.client.set_function_breakpoints(&[BP_FUNC]);
                if srv.send(&bps).is_err() {
                    return fatal("send setFunctionBreakpoints");
                }
                let done = self.client.configuration_done();
                if srv.send(&done).is_err() {
                    return fatal("send configurationDone");
                }
                self.phase = Phase::Configuring;
                Progress::Wait
            }

            // The breakpoint's verification (an ack; does not advance the phase).
            Action::Breakpoints(bl) => {
                let verified = bl.iter().filter(|b| b.verified).count();
                t_putstr("dap-probe: breakpoints set -- verified=");
                print_u32(verified as u32);
                t_putstr("\n");
                Progress::Wait
            }

            Action::Stopped(s) => {
                t_putstr("dap-probe: stopped reason=\"");
                t_putstr(&s.reason);
                t_putstr("\" tid=");
                print_u32(s.thread_id as u32);
                t_putstr("\n");
                self.thread_id = s.thread_id;
                match self.phase {
                    // The entry stop: run to the breakpoint.
                    Phase::Configuring => self.send_continue(srv),
                    // A post-continue stop: read the stack to see if we are in
                    // parkLoop yet.
                    Phase::RunningToBp => {
                        let st = self.client.stack_trace(self.thread_id, 20);
                        if srv.send(&st).is_err() {
                            return fatal("send stackTrace");
                        }
                        self.phase = Phase::Stack;
                        Progress::Wait
                    }
                    // A stop we did not solicit -- ignore (a UI would repaint).
                    _ => Progress::Wait,
                }
            }

            Action::StackTrace(frames) if self.phase == Phase::Stack => {
                for f in &frames {
                    t_putstr("dap-probe: frame ");
                    t_putstr(&f.name);
                    t_putstr("\n");
                }
                if let Some(f) = frames.iter().find(|f| f.name.contains("parkLoop")) {
                    self.frame_id = f.id;
                    let sc = self.client.scopes(self.frame_id);
                    if srv.send(&sc).is_err() {
                        return fatal("send scopes");
                    }
                    self.phase = Phase::Scopes;
                    Progress::Wait
                } else {
                    // Not there yet -- a bounded retry (a HW breakpoint can stop
                    // for another reason first). parkLoop is an infinite park, so
                    // once reached the target stays there.
                    self.continues += 1;
                    if self.continues >= MAX_CONTINUE {
                        return fatal("main.parkLoop not reached after continue");
                    }
                    self.phase = Phase::RunningToBp;
                    self.send_continue(srv)
                }
            }

            Action::Scopes(scopes) if self.phase == Phase::Scopes => {
                if scopes.is_empty() {
                    return fatal("no scopes for the parkLoop frame");
                }
                let vs = self.client.variables(scopes[0].variables_reference);
                if srv.send(&vs).is_err() {
                    return fatal("send variables");
                }
                self.phase = Phase::Vars;
                Progress::Wait
            }

            Action::Variables { .. } if self.phase == Phase::Vars => {
                t_putstr("dap-probe: variables ok\n");
                let ev = self.client.evaluate("main.Sentinel", self.frame_id, "repl");
                if srv.send(&ev).is_err() {
                    return fatal("send evaluate");
                }
                self.phase = Phase::Eval;
                Progress::Wait
            }

            Action::Evaluate(er) if self.phase == Phase::Eval => {
                t_putstr("dap-probe: evaluate main.Sentinel -> ");
                t_putstr(&er.result);
                t_putstr("\n");
                if er.result.contains(SENTINEL_DEC) {
                    self.phase = Phase::Done;
                    Progress::Wait
                } else {
                    fatal("main.Sentinel value mismatch")
                }
            }

            // The debuggee's own output -- surface it, informational.
            Action::Output(o) => {
                t_putstr("dap-probe: [child ");
                t_putstr(&o.category);
                t_putstr("] ");
                t_putstr(&o.output);
                if !o.output.ends_with('\n') {
                    t_putstr("\n");
                }
                Progress::Wait
            }

            Action::Ack("disconnect") if self.phase == Phase::Disconnecting => {
                t_putstr("dap-probe: ack disconnect\n");
                self.phase = Phase::Disconnected;
                Progress::Wait
            }

            // A launch / configurationDone / continue ack -- log, do not advance.
            Action::Ack(cmd) => {
                t_putstr("dap-probe: ack ");
                t_putstr(cmd);
                t_putstr("\n");
                Progress::Wait
            }

            // A request failed: fatal (the probe drives a fixed, must-succeed
            // sequence).
            Action::Failed(msg) => {
                let mut s = String::from("request failed -- ");
                s.push_str(&msg);
                Progress::Fail(s)
            }

            // The target died before we finished -- fatal (it should park until
            // we disconnect).
            Action::Exited(code) if !self.ending() => {
                let mut s = String::from("child exited early, code ");
                s.push_str(&itoa(code));
                Progress::Fail(s)
            }
            Action::Terminated if !self.ending() => {
                Progress::Fail(String::from("session terminated early"))
            }

            // Everything else -- an unsolicited event, a stale ack, an unmatched
            // response -- is tolerated by design.
            _ => Progress::Wait,
        }
    }

    fn send_continue(&mut self, srv: &mut Server) -> Progress {
        let c = self.client.continue_(self.thread_id);
        if srv.send(&c).is_err() {
            return fatal("send continue");
        }
        self.phase = Phase::RunningToBp;
        Progress::Wait
    }
}

fn fatal(what: &str) -> Progress {
    let mut s = String::from("send/step failed -- ");
    s.push_str(what);
    Progress::Fail(s)
}

/// Fail (boot-fatal) with no ambush left to reap; `bail` reaps one first.
fn fail(msg: &str) -> ! {
    t_putstr("dap-probe: FAIL -- ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

/// Fail with ambush still running: kill it and reap it first. A launched child
/// it leaves behind is ended by EXITKILL and joey-reaped. After a refused kill
/// the wait would never return while our end of its stdin is open, so it fails
/// without one, and ambush leaves on the EOF our exit gives it.
fn bail(srv: &mut Server, msg: &str) -> ! {
    if srv.kill().is_ok() {
        let _ = srv.wait();
    }
    fail(msg)
}

/// The probe's one budget, and the heartbeat that tells a slow step from a hang.
struct Clock {
    start: Instant,
    next_hb: u64,
}

impl Clock {
    fn new() -> Clock {
        Clock { start: Instant::now(), next_hb: HEARTBEAT_MS }
    }

    /// `false` once the budget is spent; prints the heartbeat when it is due.
    fn tick(&mut self) -> bool {
        let waited = self.start.elapsed().as_millis() as u64;
        if waited > BUDGET_MS {
            return false;
        }
        if waited >= self.next_hb {
            self.next_hb += HEARTBEAT_MS;
            t_putstr("dap-probe: waiting (");
            print_u32((waited / 1000) as u32);
            t_putstr("s)\n");
        }
        true
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("dap-probe: starting (8e-3 live ambush DAP round-trip)\n");

    // A fork-absent build bakes neither -- a legitimate config the probe cannot
    // manufacture. Present, both must work, so from here nothing is soft.
    if !fs::exists(AMBUSH) || !fs::exists(CHILD) {
        t_putstr("dap-probe: /bin/ambush or /bin/ambush-child absent -- skipping\n");
        return 0;
    }

    let mut mux = Mux::new();
    let mut clock = Clock::new();
    orderly_session(&mut mux, &mut clock);
    abandoned_session(&mut mux, &mut clock);

    t_putstr("dap-probe: PASS -- ambush DAP launch + breakpoint + inspect + disconnect + EXITKILL (");
    print_u32(clock.start.elapsed().as_millis() as u32);
    t_putstr("ms)\n");
    0
}

/// Steps 1-6: launch, break, inspect, then the orderly goodbye.
fn orderly_session(mux: &mut Mux, clock: &mut Clock) {
    let mut sess = Session::new(false);
    let mut srv = start_session(&mut sess);
    drive(&mut sess, &mut srv, mux, clock, Phase::Done);
    t_putstr("dap-probe: inspect OK -- launch + breakpoint + main.Sentinel\n");
    let child = debuggee(&sess, &mut srv);
    let bye = sess.client.disconnect(true);
    if srv.send(&bye).is_err() {
        bail(&mut srv, "send disconnect");
    }
    sess.phase = Phase::Disconnecting;
    drive(&mut sess, &mut srv, mux, clock, Phase::Disconnected);
    match proc_status(child) {
        Status::Gone => {}
        Status::Present(st) => {
            let mut s = String::from("ambush answered the disconnect with its debuggee still present (state ");
            s.push_str(field(&st, "state:").unwrap_or("?"));
            s.push(')');
            bail(&mut srv, &s);
        }
        Status::Unreadable(e) => bail(&mut srv, &unreadable(child, e)),
    }
    t_putstr("dap-probe: disconnect OK -- ambush ended and reaped its debuggee pid=");
    print_u32(child as u32);
    t_putstr("\n");
    let st = await_exit(&mut srv, mux, clock);
    if !st.success() {
        let mut s = String::from("ambush exited status ");
        s.push_str(&itoa(st.raw() as i64));
        s.push_str(" after the disconnect");
        fail(&s);
    }
}

/// Step 7: a launched debuggee whose debugger dies without letting go.
fn abandoned_session(mux: &mut Mux, clock: &mut Clock) {
    let mut sess = Session::new(true);
    let mut srv = start_session(&mut sess);
    drive(&mut sess, &mut srv, mux, clock, Phase::Launched);
    let child = debuggee(&sess, &mut srv);
    // A refused kill must not reach the wait: this ambush waits for
    // configurationDone and would never exit. Failing closes our end of its
    // pipes instead, and it leaves on the EOF.
    if srv.kill().is_err() {
        fail("kill the abandoned session's ambush");
    }
    if srv.wait().is_err() {
        fail("reap the abandoned session's ambush");
    }
    await_exitkill(child, clock);
}

/// Spawn the real ambush over piped stdio (Server forces all three to Piped),
/// no args beyond `dap-stdio`, inheriting env + caps EXACTLY as the editor
/// spawns it -- launch/attach configuration comes over the wire, not argv --
/// and open the session.
fn start_session(sess: &mut Session) -> Server {
    let mut cmd = Command::new(AMBUSH);
    cmd.arg("dap-stdio");
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("Server::spawn(/bin/ambush dap-stdio)"),
    };
    let init = sess.client.initialize("go");
    if srv.send(&init).is_err() {
        bail(&mut srv, "send initialize");
    }
    srv
}

/// Run the session until it reaches `goal`. Every failure is boot-fatal: a step
/// that failed, ambush leaving, a broken stream, the budget spent.
fn drive(sess: &mut Session, srv: &mut Server, mux: &mut Mux, clock: &mut Clock, goal: Phase) {
    loop {
        // Decoded frames go first: the read that reached the last goal may have
        // carried more, and poll does not report bytes already read.
        loop {
            let body = match srv.next_frame() {
                Ok(Some(b)) => b,
                Ok(None) => break,
                Err(_) => bail(srv, "frame decode (stream desync)"),
            };
            if let Progress::Fail(msg) = dispatch(&body, sess, srv) {
                bail(srv, &msg);
            }
            if sess.phase == goal {
                return;
            }
        }
        if !clock.tick() {
            bail(srv, "no PASS within budget (session wedged or too slow)");
        }
        let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        let ready = match mux.poll(&fds, PollTimeout::Millis(POLL_MS)) {
            Ok(r) => r,
            Err(_) => bail(srv, "mux.poll"),
        };
        for r in &ready {
            match r.tag {
                // Drain + discard: a server that fills its stderr pipe BLOCKS,
                // presenting as ambush mysteriously going quiet.
                TAG_ERR => {
                    if r.readable {
                        let _ = srv.drain_stderr();
                    }
                }
                TAG_OUT => {
                    if r.readable {
                        match srv.pump() {
                            Ok(false) => {}
                            Ok(true) => {
                                let _ = srv.wait();
                                fail("ambush stdout EOF (server exited)");
                            }
                            Err(_) => bail(srv, "pump"),
                        }
                    } else if r.hup {
                        let _ = srv.wait();
                        fail("ambush stdout HUP");
                    }
                }
                _ => {}
            }
        }
    }
}

/// Classify + dispatch one framed DAP message.
fn dispatch(body: &[u8], sess: &mut Session, srv: &mut Server) -> Progress {
    // A message we cannot parse is skipped, not fatal: the framing is still in
    // sync (exactly Content-Length bytes were consumed).
    let value = match parley::json::Value::parse(body) {
        Ok(v) => v,
        Err(_) => return Progress::Wait,
    };
    let msg = match dap::classify(value) {
        Ok(m) => m,
        Err(_) => return Progress::Wait,
    };
    let act = sess.client.handle(msg);
    sess.on_action(act, srv)
}

/// The debuggee's pid, checked to be a live ambush-child whose parent is this
/// ambush. The positive control for the teardown checks: without it, an absent
/// /proc entry afterwards could mean a wrong pid rather than a reap.
fn debuggee(sess: &Session, srv: &mut Server) -> i32 {
    if sess.child == 0 {
        bail(srv, "ambush sent no process event");
    }
    let child = sess.child as i32;
    let st = match proc_status(child) {
        Status::Present(st) => st,
        Status::Gone => bail(srv, "the debuggee ambush named has no /proc entry"),
        Status::Unreadable(e) => bail(srv, &unreadable(child, e)),
    };
    let parent = itoa(srv.pid() as i64);
    if field(&st, "name:") != Some("ambush-child")
        || field(&st, "state:") != Some("ALIVE")
        || field(&st, "ppid:") != Some(parent.as_str())
    {
        bail(srv, "the debuggee ambush named is not its live ambush-child");
    }
    child
}

/// Let ambush leave by itself and reap it. Delve stops its server once a
/// disconnect ends the session; the closed stdin covers a server that waits for
/// EOF instead.
fn await_exit(srv: &mut Server, mux: &mut Mux, clock: &mut Clock) -> ExitStatus {
    srv.close_stdin();
    loop {
        if !clock.tick() {
            bail(srv, "ambush did not exit after the disconnect");
        }
        let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        let ready = match mux.poll(&fds, PollTimeout::Millis(POLL_MS)) {
            Ok(r) => r,
            Err(_) => bail(srv, "mux.poll"),
        };
        for r in &ready {
            match r.tag {
                TAG_ERR => {
                    if r.readable {
                        let _ = srv.drain_stderr();
                    }
                }
                TAG_OUT => {
                    if r.readable || r.hup {
                        match srv.pump() {
                            // The trailing `terminated` event: read and dropped.
                            Ok(false) => {
                                while let Ok(Some(_)) = srv.next_frame() {}
                            }
                            Ok(true) => match srv.wait() {
                                Ok(st) => return st,
                                Err(_) => fail("reap ambush after the disconnect"),
                            },
                            Err(_) => bail(srv, "pump"),
                        }
                    }
                }
                _ => {}
            }
        }
    }
}

/// Wait for the abandoned debuggee to die, then check that it died of a kill.
fn await_exitkill(child: i32, clock: &mut Clock) {
    loop {
        let st = match proc_status(child) {
            Status::Present(st) => st,
            Status::Gone => fail("the abandoned debuggee was reaped before it was seen dead"),
            Status::Unreadable(e) => fail(&unreadable(child, e)),
        };
        if field(&st, "state:") == Some("ZOMBIE") {
            if field(&st, "signal:") != Some("9") {
                let mut s = String::from("the abandoned debuggee died, but not of a kill (exit ");
                s.push_str(field(&st, "exit:").unwrap_or("?"));
                s.push_str(", signal ");
                s.push_str(field(&st, "signal:").unwrap_or("none"));
                s.push(')');
                fail(&s);
            }
            t_putstr("dap-probe: EXITKILL OK -- ambush died without detach and the kernel ended its debuggee pid=");
            print_u32(child as u32);
            t_putstr(" (ZOMBIE, signal 9; init reaps it)\n");
            return;
        }
        if !clock.tick() {
            fail("EXITKILL: the abandoned debuggee outlived its killed debugger");
        }
        let _ = sleep(Duration::from_millis(DEATH_POLL_MS));
    }
}

/// What `/proc/<pid>/status` says about a pid.
enum Status {
    Present(String),
    /// No Proc has the pid. Ids are never reused, so that one was reaped.
    Gone,
    Unreadable(String),
}

fn proc_status(pid: i32) -> Status {
    let mut path = String::from("/proc/");
    path.push_str(&itoa(pid as i64));
    path.push_str("/status");
    let mut f = match fs::File::open(&path) {
        Ok(f) => f,
        Err(Error::NotFound) => return Status::Gone,
        Err(e) => return Status::Unreadable(e.to_string()),
    };
    let mut buf = [0u8; 1024];
    let mut total = 0usize;
    while total < buf.len() {
        match f.read(&mut buf[total..]) {
            Ok(0) => break,
            Ok(k) => total += k,
            Err(e) => return Status::Unreadable(e.to_string()),
        }
    }
    match core::str::from_utf8(&buf[..total]) {
        Ok(s) => Status::Present(String::from(s)),
        Err(_) => Status::Unreadable(String::from("not UTF-8")),
    }
}

/// The value of the status line that starts with `key`.
fn field<'a>(status: &'a str, key: &str) -> Option<&'a str> {
    status.lines().find_map(|l| l.strip_prefix(key)).map(str::trim)
}

fn unreadable(pid: i32, e: String) -> String {
    let mut s = String::from("cannot read /proc/");
    s.push_str(&itoa(pid as i64));
    s.push_str("/status: ");
    s.push_str(&e);
    s
}

/// Decimal print without `format!` -- this runs on the boot path.
fn print_u32(mut v: u32) {
    let mut buf = [0u8; 12];
    let mut i = buf.len();
    if v == 0 {
        t_putstr("0");
        return;
    }
    while v > 0 {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
    }
    if let Ok(s) = core::str::from_utf8(&buf[i..]) {
        t_putstr(s);
    }
}

/// Signed decimal to String (for the failure messages).
fn itoa(v: i64) -> String {
    let mut s = String::new();
    if v < 0 {
        s.push('-');
    }
    let mut n = v.unsigned_abs();
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    if n == 0 {
        s.push('0');
        return s;
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    if let Ok(d) = core::str::from_utf8(&buf[i..]) {
        s.push_str(d);
    }
    s
}
