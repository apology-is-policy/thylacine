// lsp-probe -- the Stage 8e-2 LIVE LSP round-trip E2E (boot-fatal where gopls
// is baked).
//
// WHAT WAS MISSING. `parley::lsp` shipped with 19 unit tests, and every one of
// them feeds the client a message the TEST ITSELF wrote. `parley::transport`
// has none; its only E2E (`parley-probe`, 8e-1c) talks to `/parley-echo`, which
// echoes bytes and speaks no protocol. The gopls coverage that does exist (the
// joey go8d probe) drives gopls as a COMMAND -- `gopls check`, `gopls
// definition` -- never over the LSP stdio protocol.
//
// So the client had been validated exclusively against its own assumptions
// about the server. That is the shape that passes every test and fails on first
// contact: nothing had ever confirmed gopls ACCEPTS our `initialize` params,
// that the capability object it sends back is one we can read, that a
// server-initiated request gets the reply gopls blocks on, or that a real
// `publishDiagnostics` decodes to the position we planted.
//
// THE CHAIN, ALL LIVE:
//   1. write a module with a DELIBERATE undefined identifier at a known line;
//   2. spawn the real /goroot/bin/gopls over piped stdio, EXACTLY as nora does
//      (`Command::new(GOPLS)` bare -- no args, inherited env + caps), so this
//      probe fails if nora's invocation is wrong;
//   3. initialize -> Action::Ready (gopls parsed our params and we parsed its
//      capability reply);
//   4. initialized + didOpen the broken file;
//   5. wait for publishDiagnostics on OUR uri carrying OUR identifier at OUR
//      LINE -- the line is what proves the range decoded, rather than just that
//      some error-shaped thing arrived. An empty first publish is expected and
//      waited through, not a failure (gopls commonly publishes `[]` before the
//      type-check completes; a synthetic test would never model that).
//
// WHAT THIS DOES NOT COVER, measured rather than assumed. The PASS line prints
// `auto-replies=N`: server-initiated requests we answered. It reads 0, and that
// is CORRECT -- `Client::initialize` declares no `workspace.configuration` and
// no dynamic registration, so gopls has nothing to ask. The `Action::Send` arm
// is therefore wired and defensive but unexercised here; the counter is what
// makes that a fact in the boot log instead of a guess. Adding a capability
// that invites requests would light it up.
//
// Pure userspace: the kernel is byte-unchanged. joey spawns + reaps it and
// gates the boot on exit 0.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::io::Write;
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;
use libthyla_rs::time::Instant;
use libthyla_rs::{env, fs, t_exits, t_putstr};

use parley::json::Value;
use parley::lsp::{self, Action, Severity};
use parley::transport::{Mux, Server, Tag};

/// Where the Go toolchain ships in the default image (Stage 8d).
const GOPLS: &str = "/goroot/bin/gopls";

/// The probe's own workspace. Deliberately NOT go8d's `/tmp/gp`: a probe that
/// borrows another probe's fixture breaks the day that fixture is edited for
/// its own reasons.
const WS_DIR: &str = "/tmp/lspp";
const WS_MOD: &str = "/tmp/lspp/go.mod";
const WS_SRC: &str = "/tmp/lspp/main.go";

const MOD_TEXT: &str = "module lspp\n\ngo 1.25\n";

/// Line 3 (0-based) holds the undefined identifier. Keep the two in step: the
/// probe asserts the diagnostic lands exactly there, which is what proves the
/// range decode rather than just "some error arrived".
const SRC_TEXT: &str = "package lspp\n\nfunc Probe() int {\n\treturn lspProbeUndefined7431\n}\n";
const BAD_IDENT: &str = "lspProbeUndefined7431";
const BAD_LINE: u32 = 3;

/// Total budget for the whole session. gopls's first workspace load runs
/// `go list` on-device, which is the same heavy path the go8d probe allows 180s
/// for; this covers that plus the handshake and the type-check.
const BUDGET_MS: u64 = 240_000;
/// One poll wait. Short enough that the heartbeat below stays useful.
const POLL_MS: u32 = 5_000;
/// Print progress this often so a slow workspace load is distinguishable from a
/// hang in the boot log -- the difference between "waiting" and "wedged" should
/// never have to be guessed.
const HEARTBEAT_MS: u64 = 20_000;

const TAG_OUT: Tag = 1;
const TAG_ERR: Tag = 2;

/// Bail with a tagged reason. Deliberately does NOT reap gopls: exiting closes
/// our pipe fds, gopls sees stdin EOF and leaves, and a probe failure is
/// boot-fatal anyway (joey gates on it, so the guest is already going down).
/// If this probe is ever made non-fatal, this needs a kill+wait or every failed
/// boot leaks a ~25 MB child.
fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

/// Write `text` to `path`, replacing whatever is there.
///
/// /tmp is disk-backed and survives reboots, so a stale file from a previous
/// boot would otherwise be type-checked instead of this one.
fn write_file(path: &str, text: &str) -> bool {
    let _ = fs::remove_file(path);
    match fs::File::create(path) {
        Ok(mut f) => f.write_all(text.as_bytes()).is_ok(),
        Err(_) => false,
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("lsp-probe: starting (8e-2 live gopls LSP round-trip)\n");

    // Absent gopls is a legitimate image config (a non-bake build), and the
    // probe cannot manufacture a language server. Present gopls, on the other
    // hand, means the bake config -- where every step below MUST work, so from
    // here on nothing is skipped and nothing is soft.
    if !fs::exists(GOPLS) {
        t_putstr("lsp-probe: gopls absent -- skipping\n");
        return 0;
    }

    // 1. The workspace. gopls resolves its view from the CWD (the go8d probe
    //    established this the hard way -- without a module cwd it reports "no
    //    views" and type-checks nothing), so chdir before the spawn and let the
    //    child inherit it.
    if !fs::exists(WS_DIR) && fs::create_dir(WS_DIR).is_err() {
        fail("lsp-probe: FAIL -- mkdir /tmp/lspp\n");
    }
    if !write_file(WS_MOD, MOD_TEXT) {
        fail("lsp-probe: FAIL -- write go.mod\n");
    }
    if !write_file(WS_SRC, SRC_TEXT) {
        fail("lsp-probe: FAIL -- write main.go\n");
    }
    if env::set_current_dir(WS_DIR).is_err() {
        fail("lsp-probe: FAIL -- chdir /tmp/lspp\n");
    }

    // 2. Spawn gopls the way nora spawns it -- bare, no args, inheriting env
    //    (PATH, so gopls can LookPath `go`) and caps (CSPRNG_READ, which
    //    crypto/rand needs at init). Deviating here would prove a path nora
    //    does not take.
    let mut cmd = Command::new(GOPLS);
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail("lsp-probe: FAIL -- Server::spawn(gopls)\n"),
    };

    // 3. The handshake.
    let uri = lsp::path_to_uri(WS_SRC);
    let root_uri = lsp::path_to_uri(WS_DIR);
    let mut cl = lsp::Client::new();
    let init = cl.initialize(&root_uri);
    if srv.send(&init).is_err() {
        fail("lsp-probe: FAIL -- send initialize\n");
    }

    let mut mux = Mux::new();
    let start = Instant::now();
    let mut next_hb = HEARTBEAT_MS;
    let mut ready_seen = false;
    // Counters printed on the PASS line. They are not decoration: they are how
    // a future reader tells a round-trip that really happened from one that got
    // lucky, and how a regression that silently stops gopls talking to us shows
    // up in the boot log as a number rather than as nothing at all.
    let mut publishes: u32 = 0;
    let mut replies: u32 = 0;

    loop {
        let waited = start.elapsed().as_millis() as u64;
        if waited > BUDGET_MS {
            if !ready_seen {
                fail("lsp-probe: FAIL -- no initialize response within budget\n");
            }
            t_putstr("lsp-probe: FAIL -- handshake completed but no matching diagnostic; publishes=");
            print_u32(publishes);
            t_putstr("\n");
            let _ = srv.kill();
            let _ = srv.wait();
            unsafe { t_exits(1) }
        }
        if waited >= next_hb {
            next_hb += HEARTBEAT_MS;
            t_putstr("lsp-probe: waiting (");
            print_u32((waited / 1000) as u32);
            t_putstr("s, ready=");
            t_putstr(if ready_seen { "yes" } else { "no" });
            t_putstr(", publishes=");
            print_u32(publishes);
            t_putstr(")\n");
        }

        let fds = [(srv.stdout_fd(), TAG_OUT), (srv.stderr_fd(), TAG_ERR)];
        let ready = match mux.poll(&fds, PollTimeout::Millis(POLL_MS)) {
            Ok(r) => r,
            Err(_) => fail("lsp-probe: FAIL -- mux.poll\n"),
        };

        for r in &ready {
            match r.tag {
                // Drain and discard. A chatty server that fills its stderr pipe
                // BLOCKS -- which would present as gopls mysteriously going
                // quiet, not as an error.
                TAG_ERR => {
                    if r.readable {
                        let _ = srv.drain_stderr();
                    }
                }
                TAG_OUT => {
                    if r.readable {
                        match srv.pump() {
                            Ok(false) => {}
                            Ok(true) => fail("lsp-probe: FAIL -- gopls stdout EOF (server exited)\n"),
                            Err(_) => fail("lsp-probe: FAIL -- pump\n"),
                        }
                        loop {
                            let body = match srv.next_frame() {
                                Ok(Some(b)) => b,
                                Ok(None) => break,
                                Err(_) => fail("lsp-probe: FAIL -- frame decode (stream desync)\n"),
                            };
                            if dispatch(
                                &body,
                                &mut cl,
                                &mut srv,
                                &uri,
                                &mut ready_seen,
                                &mut publishes,
                                &mut replies,
                            ) {
                                // The planted diagnostic arrived and checked out.
                                t_putstr("lsp-probe: PASS -- gopls handshake + didOpen + publishDiagnostics (");
                                print_u32(start.elapsed().as_millis() as u32);
                                t_putstr("ms, publishes=");
                                print_u32(publishes);
                                t_putstr(", auto-replies=");
                                print_u32(replies);
                                t_putstr(")\n");
                                shutdown(&mut cl, &mut srv);
                                return 0;
                            }
                        }
                    } else if r.hup {
                        fail("lsp-probe: FAIL -- gopls stdout HUP\n");
                    }
                }
                _ => {}
            }
        }
    }
}

/// Handle one framed message. Returns true once the diagnostic we planted has
/// arrived and been verified.
fn dispatch(
    body: &[u8],
    cl: &mut lsp::Client,
    srv: &mut Server,
    uri: &str,
    ready_seen: &mut bool,
    publishes: &mut u32,
    replies: &mut u32,
) -> bool {
    // A message we cannot parse is skipped, not fatal: the framing is still in
    // sync (exactly Content-Length bytes were consumed), and a client that dies
    // on one unexpected message is a client that dies in the field.
    let value = match Value::parse(body) {
        Ok(v) => v,
        Err(_) => return false,
    };
    let msg = match parley::jsonrpc::classify(value) {
        Ok(m) => m,
        Err(_) => return false,
    };
    match cl.handle(msg) {
        // The auto-reply. gopls sends real requests during startup
        // (registerCapability, workspace/configuration) and WAITS on the
        // answer -- a client that ignores them stalls forever, which is
        // precisely the failure no synthetic test can produce.
        Action::Send(reply) => {
            *replies += 1;
            if srv.send(&reply).is_err() {
                fail("lsp-probe: FAIL -- send auto-reply (gopls stdin broken)\n");
            }
            false
        }
        Action::Ready => {
            *ready_seen = true;
            t_putstr("lsp-probe: initialize OK -- gopls accepted our capabilities\n");
            let note = cl.initialized();
            if srv.send(&note).is_err() {
                fail("lsp-probe: FAIL -- send initialized\n");
            }
            let open = cl.did_open(uri, "go", 1, SRC_TEXT);
            if srv.send(&open).is_err() {
                fail("lsp-probe: FAIL -- send didOpen\n");
            }
            false
        }
        Action::Diagnostics(published_uri) => {
            if published_uri != uri {
                // gopls publishes for every file in the package; ours is the
                // only one that counts.
                return false;
            }
            *publishes += 1;
            // An EMPTY publish is normal and NOT a failure: gopls commonly
            // publishes `[]` for a freshly-opened file before the type-check
            // finishes, then republishes with the real errors. Waiting through
            // that is the whole reason this loop is deadline-bounded rather
            // than first-publish-wins.
            for d in cl.diagnostics_for(uri) {
                if !d.message.contains(BAD_IDENT) {
                    continue;
                }
                if d.severity != Severity::Error {
                    fail("lsp-probe: FAIL -- planted error reported at non-Error severity\n");
                }
                // The line is what proves the RANGE decoded, not just that some
                // string arrived.
                if d.range.start.line != BAD_LINE {
                    t_putstr("lsp-probe: FAIL -- diagnostic line ");
                    print_u32(d.range.start.line);
                    t_putstr(", expected ");
                    print_u32(BAD_LINE);
                    t_putstr("\n");
                    unsafe { t_exits(1) }
                }
                t_putstr("lsp-probe: diagnostic OK -- \"");
                t_putstr(&d.message);
                t_putstr("\" at line ");
                print_u32(d.range.start.line);
                t_putstr("\n");
                return true;
            }
            false
        }
        // Everything else is server chatter (progress, log messages, an
        // unmatched response). Tolerated by design.
        _ => false,
    }
}

/// The orderly LSP goodbye, then make sure the child is really gone: a server
/// that ignores the protocol shutdown would otherwise outlive the probe as an
/// orphan holding the workspace.
fn shutdown(cl: &mut lsp::Client, srv: &mut Server) {
    let bye = cl.shutdown();
    let _ = srv.send(&bye);
    let exit = cl.exit();
    let _ = srv.send(&exit);
    srv.close_stdin();
    let _ = srv.kill();
    let _ = srv.wait();
}

/// Decimal print without `format!` -- this runs on the boot path, and the
/// console is shared.
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
    let s: &str = match core::str::from_utf8(&buf[i..]) {
        Ok(s) => s,
        Err(_) => return,
    };
    t_putstr(s);
}
