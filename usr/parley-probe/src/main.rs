// parley-probe -- the Stage 8e-1c transport E2E (boot-fatal).
//
// Proves the persistent-child transport end to end in the guest, exercising
// exactly the machinery nora's LSP/DAP loop will use (minus the LSP protocol):
//
//   1. spawn /parley-echo as a persistent server (piped stdin/stdout/stderr);
//   2. build a JSON-RPC request (parley::jsonrpc) and frame-send it to stdin;
//   3. Mux-poll the echo's stdout+stderr in one poll(2) -- the multi-fd,
//      tag-dispatched wait that will hold {fd0, gopls, ambush};
//   4. pump the readable bytes through the streaming frame Decoder;
//   5. assert the echoed body is byte-identical to what we sent AND re-parses +
//      classifies back into our request;
//   6. graceful shutdown (close stdin -> the echo EOFs -> blocking reap, no
//      zombie).
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
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;
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

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("parley-probe: starting (8e-1c transport E2E)\n");

    // 1. Spawn the echo server. Root-anchored name (baked in the cpio, like
    //    /stack-child); Server forces all three stdio slots to Piped.
    let mut cmd = Command::new("/parley-echo");
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("parley-probe: FAIL -- Server::spawn(/parley-echo)\n"),
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
                    if r.readable {
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

    t_putstr("parley-probe: PASS\n");
    0
}
