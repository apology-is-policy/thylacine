// parley-probe -- the Stage 8e-1c transport E2E (boot-fatal).
//
// Proves the persistent-child transport end to end in the guest, exercising
// exactly the machinery nora's LSP/DAP loop will use (minus the LSP protocol):
//
//   1. spawn /bin/parley-echo as a persistent server (piped stdin/stdout/stderr);
//   2. build a JSON-RPC request (parley::jsonrpc) and frame-send it to stdin;
//   3. Mux-poll the echo's stdout+stderr in one poll(2) -- the multi-fd,
//      tag-dispatched wait that will hold {fd0, gopls, ambush};
//   4. pump the readable bytes through the streaming frame Decoder;
//   5. assert the echoed body is byte-identical to what we sent AND re-parses +
//      classifies back into our request;
//   6. graceful shutdown (close stdin -> the echo EOFs -> blocking reap, no
//      zombie);
//   7. a server that closes stderr and keeps serving: stderr leaves the poll set
//      at its EOF, the next poll sleeps, and the server still answers;
//   8. a server that stops reading: send returns with the message queued, a full
//      pipe does not wake the poll, and the outbox refuses at its bound;
//   9. a message many pipes long: it moves on each writable wake and comes back
//      whole, and a close with bytes still queued delivers them first.
//
// A pure-userspace probe: the kernel is byte-unchanged. joey spawns + reaps it
// and gates the boot on exit 0 (the "parley-probe: PASS" marker is for the
// console log; the exit code is the gate).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::ToString;
use alloc::vec::Vec;
use libthyla_rs::err::{Error, Result};
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;
use libthyla_rs::service_worker::{Control, ServiceWorker};
use libthyla_rs::{t_exits, t_putstr};
use parley::json::Value;
use parley::jsonrpc::{self, Incoming};
use parley::transport::{Mux, Server};

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

// Mux tags: the echo's two output streams. (nora will tag fd 0 + each server's
// stdout/stderr the same way.)
const TAG_OUT: u32 = 1;
const TAG_ERR: u32 = 2;
const TAG_IN: u32 = 3;

// Many pipes long: a pipe holds 4 KiB.
const BIG: usize = 64 * 1024;

// A send that blocks on a stopped reader would hang the boot with no line at all.
const BLOCKED_MS: u32 = 20_000;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("parley-probe: starting (8e-1c transport E2E)\n");

    // 1. Spawn the echo server. Root-anchored name (baked in the cpio, like
    //    /bin/stack-child); Server forces all three stdio slots to Piped.
    let mut cmd = Command::new("/bin/parley-echo");
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("parley-probe: FAIL -- Server::spawn(/bin/parley-echo)\n"),
    };

    // 2. Build a JSON-RPC request and frame-send it. The echo returns the framed
    //    wire verbatim, so the decoded body must equal our serialized request.
    let params = match Value::parse(br#"{"textDocument":{"uri":"file:///w/main.go"},"position":{"line":41,"character":8}}"#)
    {
        Ok(v) => v,
        Err(_) => fail("parley-probe: FAIL -- params parse\n"),
    };
    let req = jsonrpc::request(7, "textDocument/definition", params);
    let sent = req.to_string();
    if srv.send(&req).is_err() {
        fail("parley-probe: FAIL -- send\n");
    }

    // 3-4. Multiplex + pump + decode until the echoed frame arrives. The 2-fd
    //      set (stdout + stderr) exercises the real multi-fd, tag-dispatched
    //      poll; only stdout ever fires for the echo.
    let mut mux = Mux::new();
    let mut got: Option<Vec<u8>> = None;
    let mut spins: u32 = 0;
    'wait: while got.is_none() {
        spins += 1;
        if spins > 1000 {
            fail("parley-probe: FAIL -- no echo after 1000 poll cycles\n");
        }
        let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        let ready = match mux.poll(&fds, PollTimeout::Millis(2000)) {
            Ok(r) => r,
            Err(_) => fail("parley-probe: FAIL -- mux.poll\n"),
        };
        if ready.is_empty() {
            fail("parley-probe: FAIL -- poll timed out (no echo)\n");
        }
        for r in &ready {
            match r.tag {
                TAG_OUT => {
                    if r.readable {
                        match srv.pump() {
                            Ok(false) => {}
                            Ok(true) => fail("parley-probe: FAIL -- echo stdout EOF before reply\n"),
                            Err(_) => fail("parley-probe: FAIL -- pump\n"),
                        }
                        // One framed reply is all the echo sends; a partial
                        // pump yields None -> fall through and re-poll.
                        match srv.next_frame() {
                            Ok(Some(body)) => {
                                got = Some(body);
                                break 'wait;
                            }
                            Ok(None) => {}
                            Err(_) => fail("parley-probe: FAIL -- frame decode\n"),
                        }
                    } else if r.hup {
                        fail("parley-probe: FAIL -- echo stdout HUP before reply\n");
                    }
                }
                TAG_ERR => {
                    if r.readable || r.hup {
                        // The echo writes no stderr; drain defensively so a
                        // hypothetical byte can never wedge it.
                        let _ = srv.drain_stderr();
                    }
                }
                _ => {}
            }
        }
    }

    // 5. The echoed body must be byte-identical to what we sent, and must
    //    re-parse + classify as our request (method + id echoed).
    let body = match got {
        Some(b) => b,
        None => fail("parley-probe: FAIL -- no body\n"),
    };
    if body != sent.as_bytes() {
        fail("parley-probe: FAIL -- echoed body mismatch\n");
    }
    let parsed = match Value::parse(&body) {
        Ok(v) => v,
        Err(_) => fail("parley-probe: FAIL -- echoed body reparse\n"),
    };
    match jsonrpc::classify(parsed) {
        Ok(Incoming::Request { id, method, .. }) => {
            if id != Value::Int(7) || method != "textDocument/definition" {
                fail("parley-probe: FAIL -- classify wrong fields\n");
            }
        }
        _ => fail("parley-probe: FAIL -- classify not a request\n"),
    }

    // 6. Graceful shutdown: close stdin -> the echo reads EOF and exits 0 -> a
    //    blocking reap collects it (no zombie leak).
    srv.close_stdin();
    match srv.wait() {
        Ok(st) if st.success() => {}
        Ok(_) => fail("parley-probe: FAIL -- echo child nonzero exit\n"),
        Err(_) => fail("parley-probe: FAIL -- reap echo child\n"),
    }

    closed_stderr(&mut mux, &req, sent.as_bytes());
    stopped_reader(&mut mux);
    large_message(&mut mux);

    t_putstr("parley-probe: PASS\n");
    0
}

fn closed_stderr(mux: &mut Mux, req: &Value, sent: &[u8]) {
    let mut cmd = Command::new("/bin/parley-echo");
    cmd.arg("close-stderr");
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("parley-probe: FAIL -- Server::spawn(close-stderr)\n"),
    };
    let mut spins: u32 = 0;
    while srv.stderr_fd() >= 0 {
        spins += 1;
        if spins > 1000 {
            fail("parley-probe: FAIL -- stderr still in the poll set after 1000 polls\n");
        }
        let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        let ready = match mux.poll(&fds, PollTimeout::Millis(2000)) {
            Ok(r) => r,
            Err(_) => fail("parley-probe: FAIL -- mux.poll (stderr EOF)\n"),
        };
        if ready.is_empty() {
            fail("parley-probe: FAIL -- poll timed out (no stderr EOF)\n");
        }
        for r in &ready {
            if r.tag == TAG_ERR && (r.readable || r.hup) {
                let _ = srv.drain_stderr();
            }
        }
    }
    let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
    match mux.poll(&fds, PollTimeout::Millis(50)) {
        Ok(r) if r.is_empty() => {}
        Ok(_) => fail("parley-probe: FAIL -- a closed stderr still wakes the poll\n"),
        Err(_) => fail("parley-probe: FAIL -- mux.poll (idle)\n"),
    }
    if srv.send(req).is_err() {
        fail("parley-probe: FAIL -- send (close-stderr)\n");
    }
    let mut body: Option<Vec<u8>> = None;
    let mut spins: u32 = 0;
    while body.is_none() {
        spins += 1;
        if spins > 1000 {
            fail("parley-probe: FAIL -- no echo with stderr closed\n");
        }
        let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        match mux.poll(&fds, PollTimeout::Millis(2000)) {
            Ok(r) if !r.is_empty() => {}
            _ => fail("parley-probe: FAIL -- poll (echo with stderr closed)\n"),
        }
        match srv.pump() {
            Ok(false) => {}
            _ => fail("parley-probe: FAIL -- pump (stderr closed)\n"),
        }
        match srv.next_frame() {
            Ok(b) => body = b,
            Err(_) => fail("parley-probe: FAIL -- frame decode (stderr closed)\n"),
        }
    }
    if body.as_deref() != Some(sent) {
        fail("parley-probe: FAIL -- echoed body mismatch (stderr closed)\n");
    }
    srv.close_stdin();
    match srv.wait() {
        Ok(st) if st.success() => {}
        _ => fail("parley-probe: FAIL -- reap (close-stderr)\n"),
    }
}

fn big_request(id: i64) -> Value {
    let mut json = Vec::with_capacity(BIG + 16);
    json.extend_from_slice(br#"{"pad":""#);
    json.resize(json.len() + BIG, b'a');
    json.extend_from_slice(br#""}"#);
    match Value::parse(&json) {
        Ok(params) => jsonrpc::request(id, "pad", params),
        Err(_) => fail("parley-probe: FAIL -- big params parse\n"),
    }
}

fn blocked_watchdog(_: &(), control: &Control) -> Result<()> {
    control.ready()?;
    let stop = [(control.stop_fd(), 0)];
    match Mux::new().poll(&stop, PollTimeout::Millis(BLOCKED_MS)) {
        Ok(r) if !r.is_empty() => Ok(()),
        _ => fail("parley-probe: FAIL -- a send blocked on a server that stopped reading\n"),
    }
}

fn stopped_reader(mux: &mut Mux) {
    let mut dog = match ServiceWorker::new((), blocked_watchdog) {
        Ok(d) => d,
        Err(_) => fail("parley-probe: FAIL -- the watchdog thread\n"),
    };
    let mut cmd = Command::new("/bin/parley-echo");
    cmd.arg("stop-reading");
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("parley-probe: FAIL -- Server::spawn(stop-reading)\n"),
    };
    let req = big_request(8);
    if srv.send(&req).is_err() {
        fail("parley-probe: FAIL -- send (stopped reader)\n");
    }
    if srv.stdin_pending_fd() < 0 {
        fail("parley-probe: FAIL -- a stopped reader left nothing queued\n");
    }
    let reads = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
    let writes = [(srv.stdin_pending_fd(), TAG_IN)];
    match mux.poll_rw(&reads, &writes, PollTimeout::Millis(50)) {
        Ok(r) if r.is_empty() => {}
        _ => fail("parley-probe: FAIL -- a full pipe wakes the poll\n"),
    }
    let mut sends: u32 = 0;
    loop {
        match srv.send(&req) {
            Ok(()) => {}
            Err(Error::NoSpace) => break,
            Err(_) => fail("parley-probe: FAIL -- send past the first (stopped reader)\n"),
        }
        sends += 1;
        if sends > 200 {
            fail("parley-probe: FAIL -- the outbox has no bound\n");
        }
    }
    if srv.kill().is_err() || srv.wait().is_err() {
        fail("parley-probe: FAIL -- reap (stopped reader)\n");
    }
    if dog.shutdown().is_err() {
        fail("parley-probe: FAIL -- the watchdog's end\n");
    }
}

// One framed reply, flushing and draining on the way. This is the shape of Nora's
// loop, so a transport that needs anything more from its host fails here.
fn serve_until_frame(srv: &mut Server, mux: &mut Mux) -> Vec<u8> {
    let mut spins: u32 = 0;
    loop {
        spins += 1;
        if spins > 10_000 {
            fail("parley-probe: FAIL -- no frame after 10000 polls (large message)\n");
        }
        let reads = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        let writes = [(srv.stdin_pending_fd(), TAG_IN)];
        let ready = match mux.poll_rw(&reads, &writes, PollTimeout::Millis(2000)) {
            Ok(r) => r,
            Err(_) => fail("parley-probe: FAIL -- mux.poll_rw\n"),
        };
        if ready.is_empty() {
            fail("parley-probe: FAIL -- poll timed out (large message)\n");
        }
        for r in &ready {
            match r.tag {
                TAG_IN => {
                    if srv.flush().is_err() {
                        fail("parley-probe: FAIL -- flush\n");
                    }
                }
                TAG_ERR => {
                    if r.readable || r.hup {
                        let _ = srv.drain_stderr();
                    }
                }
                _ => {
                    if !r.readable {
                        fail("parley-probe: FAIL -- stdout hung up before the frame\n");
                    }
                    if !matches!(srv.pump(), Ok(false)) {
                        fail("parley-probe: FAIL -- pump (large message)\n");
                    }
                    match srv.next_frame() {
                        Ok(Some(body)) => return body,
                        Ok(None) => {}
                        Err(_) => fail("parley-probe: FAIL -- frame decode (large message)\n"),
                    }
                }
            }
        }
    }
}

fn large_message(mux: &mut Mux) {
    let mut cmd = Command::new("/bin/parley-echo");
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("parley-probe: FAIL -- Server::spawn(large message)\n"),
    };
    let req = big_request(9);
    let sent = req.to_string();
    if srv.send(&req).is_err() {
        fail("parley-probe: FAIL -- send (large message)\n");
    }
    if srv.stdin_pending_fd() < 0 {
        fail("parley-probe: FAIL -- a message many pipes long left nothing queued\n");
    }
    if serve_until_frame(&mut srv, mux) != sent.as_bytes() {
        fail("parley-probe: FAIL -- a large message came back changed\n");
    }
    if srv.stdin_pending_fd() >= 0 {
        fail("parley-probe: FAIL -- the outbox did not drain\n");
    }
    if srv.send(&req).is_err() {
        fail("parley-probe: FAIL -- second send (large message)\n");
    }
    if srv.stdin_pending_fd() < 0 {
        fail("parley-probe: FAIL -- the second message left nothing queued\n");
    }
    srv.close_stdin();
    if serve_until_frame(&mut srv, mux) != sent.as_bytes() {
        fail("parley-probe: FAIL -- a close lost the bytes queued behind it\n");
    }
    match srv.wait() {
        Ok(st) if st.success() => {}
        _ => fail("parley-probe: FAIL -- reap (large message)\n"),
    }
}
