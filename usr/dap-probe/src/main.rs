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
// closed it for gopls: it spawns the REAL `/ambush dap-stdio` over piped stdio
// (the parley::transport::Server the editor uses), then drives the canonical
// VS-Code launch sequence against `/ambush-child` entirely through parley::dapc:
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

use alloc::string::String;

use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;
use libthyla_rs::time::Instant;
use libthyla_rs::{fs, t_exits, t_putstr};

use parley::dap;
use parley::dapc::{self, Action};
use parley::transport::{Mux, Server, Tag};

/// The debugger + its target, both baked into the image (Stage 8c-1).
const AMBUSH: &str = "/ambush";
const CHILD: &str = "/ambush-child";

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
}

/// What one handled action did to the session.
enum Progress {
    /// Keep polling.
    Wait,
    /// The whole round-trip PASSED.
    Pass,
    /// A step failed for this reason (boot-fatal).
    Fail(String),
}

struct Session {
    client: dapc::Client,
    phase: Phase,
    thread_id: i64,
    frame_id: i64,
    continues: u32,
}

impl Session {
    fn new() -> Session {
        Session {
            client: dapc::Client::new(),
            phase: Phase::Init,
            thread_id: 0,
            frame_id: 0,
            continues: 0,
        }
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

            Action::Variables(_) if self.phase == Phase::Vars => {
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
                    Progress::Pass
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
            Action::Exited(code) if self.phase != Phase::Done => {
                let mut s = String::from("child exited early, code ");
                s.push_str(&itoa(code));
                Progress::Fail(s)
            }
            Action::Terminated if self.phase != Phase::Done => {
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

/// Bail (boot-fatal). Does NOT reap ambush: exiting closes our pipe fds, ambush
/// sees stdin EOF and leaves, and a probe failure is boot-fatal anyway. A killed
/// session's launched child is joey-reaped.
fn fail(msg: &str) -> ! {
    t_putstr("dap-probe: FAIL -- ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("dap-probe: starting (8e-3 live ambush DAP round-trip)\n");

    // A fork-absent build bakes neither -- a legitimate config the probe cannot
    // manufacture. Present, both must work, so from here nothing is soft.
    if !fs::exists(AMBUSH) || !fs::exists(CHILD) {
        t_putstr("dap-probe: /ambush or /ambush-child absent -- skipping\n");
        return 0;
    }

    // Spawn the real ambush over piped stdio (Server forces all three to Piped),
    // no args beyond `dap-stdio`, inheriting env + caps EXACTLY as the editor
    // spawns it. launch/attach configuration comes over the wire, not argv.
    let mut cmd = Command::new(AMBUSH);
    cmd.arg("dap-stdio");
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("Server::spawn(/ambush dap-stdio)"),
    };

    let mut sess = Session::new();
    let init = sess.client.initialize("go");
    if srv.send(&init).is_err() {
        let _ = srv.kill();
        let _ = srv.wait();
        fail("send initialize");
    }

    let mut mux = Mux::new();
    let start = Instant::now();
    let mut next_hb = HEARTBEAT_MS;

    loop {
        let waited = start.elapsed().as_millis() as u64;
        if waited > BUDGET_MS {
            let _ = srv.kill();
            let _ = srv.wait();
            fail("no PASS within budget (session wedged or too slow)");
        }
        if waited >= next_hb {
            next_hb += HEARTBEAT_MS;
            t_putstr("dap-probe: waiting (");
            print_u32((waited / 1000) as u32);
            t_putstr("s)\n");
        }

        let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        let ready = match mux.poll(&fds, PollTimeout::Millis(POLL_MS)) {
            Ok(r) => r,
            Err(_) => {
                let _ = srv.kill();
                let _ = srv.wait();
                fail("mux.poll");
            }
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
                            Err(_) => {
                                let _ = srv.kill();
                                let _ = srv.wait();
                                fail("pump");
                            }
                        }
                        loop {
                            let body = match srv.next_frame() {
                                Ok(Some(b)) => b,
                                Ok(None) => break,
                                Err(_) => {
                                    let _ = srv.kill();
                                    let _ = srv.wait();
                                    fail("frame decode (stream desync)");
                                }
                            };
                            match dispatch(&body, &mut sess, &mut srv) {
                                Progress::Wait => {}
                                Progress::Pass => {
                                    t_putstr("dap-probe: PASS -- ambush DAP launch + breakpoint + inspect (");
                                    print_u32(start.elapsed().as_millis() as u32);
                                    t_putstr("ms)\n");
                                    shutdown(&mut sess, &mut srv);
                                    return 0;
                                }
                                Progress::Fail(msg) => {
                                    let _ = srv.kill();
                                    let _ = srv.wait();
                                    fail(&msg);
                                }
                            }
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

/// The orderly DAP goodbye + make sure ambush is gone. disconnect terminates the
/// debuggee; closing stdin gives ambush its EOF; the kill+wait guarantees no
/// orphan holding the child.
fn shutdown(sess: &mut Session, srv: &mut Server) {
    let bye = sess.client.disconnect(true);
    let _ = srv.send(&bye);
    srv.close_stdin();
    let _ = srv.kill();
    let _ = srv.wait();
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
