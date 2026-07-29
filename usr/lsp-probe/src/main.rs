// lsp-probe -- the LIVE LSP round-trip E2E, one session per baked language
// server (boot-fatal where a server is present).
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
// contact: nothing had ever confirmed the server ACCEPTS our `initialize`
// params, that the capability object it sends back is one we can read, that a
// server-initiated request gets the reply the server blocks on, or that a real
// `publishDiagnostics` decodes to the position we planted.
//
// THE CHAIN, ALL LIVE, per server:
//   1. write a workspace with a DELIBERATE undefined identifier at a known line;
//   2. spawn the real server over piped stdio, EXACTLY as nora does
//      (`Command::new(bin)` bare -- no args, inherited env + caps), so this
//      probe fails if nora's invocation is wrong;
//   3. initialize -> Action::Ready (the server parsed our params and we parsed
//      its capability reply);
//   4. initialized + didOpen the broken file;
//   5. wait for publishDiagnostics on OUR uri carrying OUR identifier at OUR
//      LINE -- the line is what proves the range decoded, rather than just that
//      some error-shaped thing arrived. An empty first publish is expected and
//      waited through, not a failure (servers commonly publish `[]` before the
//      analysis completes; a synthetic test would never model that).
//
// WHY A TABLE (CL-6). The chain above is protocol, not language: every step is
// the same for gopls and for clangd, and the only things that differ are which
// binary, which workspace, which `languageId`, and where the planted error is.
// Forking a second probe would have duplicated ~300 lines of session loop whose
// two copies could then drift -- and a drifted copy still passes its own gate,
// which is the failure mode that produced #100. So the differences live in
// `PROBES` and the loop runs once per present server.
//
// WHAT THIS DOES NOT COVER, measured rather than assumed. The PASS line prints
// `auto-replies=N`: server-initiated requests we answered. For gopls it reads
// 0, and that is CORRECT -- `Client::initialize` declares no
// `workspace.configuration` and no dynamic registration, so gopls has nothing
// to ask. The `Action::Send` arm is therefore wired and defensive but
// unexercised there; the counter is what makes that a fact in the boot log
// instead of a guess. Adding a capability that invites requests would light it
// up.
//
// SEPARATE CLAIMS, SEPARATE SESSIONS. The C++ side runs twice on purpose. An
// include-FREE source keeps the protocol claim clean: it needs no header search,
// so a failure there is unambiguously the round-trip and not a toolchain-config
// question. A second session then adds `#include <vector>` and asserts the
// ABSENCE of "file not found", because the first cannot detect that -- a clangd
// resolving NO header still reports the planted undefined identifier and would
// pass. Two claims, two failure modes, distinguishable in the boot log.
//
// Pure userspace: the kernel is byte-unchanged. joey spawns + reaps it and
// gates the boot on exit 0.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::io::{Read, Write};
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;
use libthyla_rs::time::Instant;
use libthyla_rs::{env, fs, t_exits, t_putstr};

use parley::json::Value;
use parley::lsp::{self, Action, Severity};
use parley::transport::{Mux, Server, Tag};

/// One language server's session: everything the protocol chain does NOT know.
struct ProbeSpec {
    /// Printed in every line this probe emits, so a failing boot log names the
    /// server rather than making the reader infer it from ordering.
    name: &'static str,
    /// Where the server ships in the default image. Absent = a non-bake build,
    /// which is a legitimate config and skips.
    bin: &'static str,
    /// The probe's own workspace. Deliberately NOT shared with another probe's
    /// fixture: borrowing one breaks the day it is edited for its own reasons.
    dir: &'static str,
    /// Support files written before the spawn -- the root markers the SERVER
    /// needs (`go.mod`, `compile_commands.json`). They are the same files
    /// `nora::lsp_host::workspace_root` searches for, but note this probe does
    /// NOT call that function: it passes `dir` as the root directly. Root
    /// DISCOVERY is therefore uncovered here; what is covered is that a server
    /// given a correctly-marked root does the right thing.
    /// (absolute path, contents)
    aux: &'static [(&'static str, &'static str)],
    /// The file that gets opened and must produce the diagnostic.
    src: &'static str,
    src_text: &'static str,
    /// The LSP `languageId`. Must match what `nora`'s ServerSpec sends, or this
    /// proves a path nora does not take.
    language_id: &'static str,
    /// The planted identifier and its 0-based line. Keep the two in step with
    /// `src_text`: the probe asserts the diagnostic lands exactly there, which
    /// is what proves the range decode rather than just "some error arrived".
    bad_ident: &'static str,
    bad_line: u32,
    /// If set, after the diagnostic verifies, issue `textDocument/hover` and
    /// `textDocument/definition` at this `(line, character)` and require BOTH
    /// to come back non-empty.
    ///
    /// CL-6's done-definition is "diagnostics/hover/def on a C++ file", not
    /// diagnostics alone. These are pure protocol -- the same `parley` code for
    /// every language -- so it is tempting to infer them from the Go side. That
    /// inference is exactly what "clangd will find its headers" was, and it was
    /// wrong. Cheaper to ask the server.
    ///
    /// Pointing at a name from an `#include` also proves something diagnostics
    /// cannot: that clangd INDEXED the header, not merely found it. A
    /// definition that resolves into libc++ is the evidence.
    intel_at: Option<(u32, u32)>,
    /// Diagnostic-message substrings that must NOT appear for this file.
    ///
    /// The planted-error assertion alone is satisfiable while the server is
    /// badly broken -- a clangd that resolves no `#include` at all still
    /// reports the undefined identifier, and would pass. Naming the messages
    /// that must be ABSENT is what turns "a diagnostic arrived" into "the
    /// server is actually configured", and it is checked BEFORE the planted
    /// match so a forbidden message cannot be masked by a successful one.
    forbid: &'static [&'static str],
}

/// One entry per session. A server that is absent is skipped, so this table is
/// the same in every image config; what changes is how many rows run.
///
/// Note the two `clangd` rows are deliberately NOT merged -- see the second
/// one's comment. `bad_line` is per-row precisely because the third source has
/// an `#include` above the error.
static PROBES: &[ProbeSpec] = &[
    ProbeSpec {
        name: "gopls",
        bin: "/goroot/bin/gopls",
        dir: "/tmp/lspp",
        aux: &[("/tmp/lspp/go.mod", "module lspp\n\ngo 1.25\n")],
        src: "/tmp/lspp/main.go",
        src_text: "package lspp\n\nfunc Probe() int {\n\treturn lspProbeUndefined7431\n}\n",
        language_id: "go",
        bad_ident: "lspProbeUndefined7431",
        bad_line: 3,
        // `Probe` on line 2 (`func Probe() int {`).
        intel_at: Some((2, 6)),
        forbid: &[],
    },
    ProbeSpec {
        name: "clangd",
        bin: "/clade/bin/clangd",
        dir: "/tmp/lspc",
        // clangd reads the compilation database to get this file's flags. It
        // PARSES the command string for flags; it never executes it. Without
        // this, clangd still diagnoses (fallback flags) -- but then the root
        // marker nora actually searches for would go untested.
        aux: &[(
            "/tmp/lspc/compile_commands.json",
            "[{\"directory\":\"/tmp/lspc\",\
              \"file\":\"/tmp/lspc/main.cpp\",\
              \"command\":\"clang++ -std=c++17 -c /tmp/lspc/main.cpp\"}]\n",
        )],
        src: "/tmp/lspc/main.cpp",
        src_text: "namespace lspc {\n\nint Probe() {\n\treturn lspProbeUndefined9317;\n}\n}\n",
        language_id: "cpp",
        bad_ident: "lspProbeUndefined9317",
        bad_line: 3,
        // Deliberately none: this spec's whole job is the protocol claim, and
        // an index-dependent assertion here would blur it with the next one.
        intel_at: None,
        forbid: &[],
    },
    // The toolchain-config claim, deliberately SEPARATE from the protocol claim
    // above. A clangd that resolves no header still reports the planted
    // undefined identifier, so the spec above would pass with `#include`
    // completely broken -- which is most of what makes an editor useful for
    // C++. This spec uses libc++ (`std::vector`) and asserts the ABSENCE of
    // "file not found", so the two failures are distinguishable in the boot
    // log: a broken protocol fails the spec above, a broken include path fails
    // this one.
    //
    // The flags mirror the REAL on-device C++ compile byte-for-byte -- they are
    // build.sh's "pouch C++ consumer flags" for /pouch-hello-cxx, which is the
    // one recipe in the tree that compiles a C++ TU AGAINST the installed
    // libc++ (rather than building libc++ itself).
    //
    // `-isystem .../c++/v1` is LOAD-BEARING and must not be dropped as
    // redundant: `-nostdlibinc` suppresses the standard *library* include dirs,
    // which includes the C++ ones, so `--sysroot` alone does NOT let the driver
    // find `<vector>`. This probe was first written without it on exactly that
    // reasoning and failed in-guest with "'vector' file not found" -- the
    // /storm Makefile is C-only and the libc++ CMake flags are for building
    // libc++ (headers supplied by CMake), so neither is evidence about the
    // consumer path. If a future edit "simplifies" these flags, this spec is
    // what fails.
    ProbeSpec {
        name: "clangd+headers",
        bin: "/clade/bin/clangd",
        dir: "/tmp/lspx",
        aux: &[(
            "/tmp/lspx/compile_commands.json",
            "[{\"directory\":\"/tmp/lspx\",\
              \"file\":\"/tmp/lspx/main.cpp\",\
              \"command\":\"clang++ --target=aarch64-thylacine -std=c++20 \
              -march=armv8-a -moutline-atomics -nostdlibinc -D_GNU_SOURCE=1 \
              -isystem /clade/sysroot/include/c++/v1 \
              -isystem /clade/sysroot/include \
              --sysroot=/clade/sysroot \
              -c /tmp/lspx/main.cpp\"}]\n",
        )],
        src: "/tmp/lspx/main.cpp",
        src_text: "#include <vector>\n\nint Probe() {\n\tstd::vector<int> v;\n\treturn lspProbeUndefined5521 + (int)v.size();\n}\n",
        language_id: "cpp",
        bad_ident: "lspProbeUndefined5521",
        bad_line: 4,
        // `vector` inside `std::vector<int> v;` on line 3 (tab = column 0, so
        // `vector` spans columns 6..12). A definition here must resolve INTO
        // libc++ -- which is the header-indexing proof.
        intel_at: Some((3, 8)),
        forbid: &["file not found", "'vector' file not found"],
    },
];

/// Where a session is in its chain. Diagnostics always come first (they are
/// what the planted error is for); hover and definition only run when the spec
/// asks, and each waits for its own reply before the next is sent so a stray
/// response cannot be mistaken for the one being awaited.
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    AwaitDiag,
    AwaitHover,
    AwaitDef,
}

/// Total budget for one session. gopls's first workspace load runs `go list`
/// on-device, which is the same heavy path the go8d probe allows 180s for; this
/// covers that plus the handshake and the type-check. clangd has no such
/// external step but pages in a 44 MB binary, so it shares the budget rather
/// than getting a guessed-tighter one.
const BUDGET_MS: u64 = 240_000;
/// One poll wait. Short enough that the heartbeat below stays useful.
const POLL_MS: u32 = 5_000;
/// Print progress this often so a slow workspace load is distinguishable from a
/// hang in the boot log -- the difference between "waiting" and "wedged" should
/// never have to be guessed.
const HEARTBEAT_MS: u64 = 20_000;

const TAG_OUT: Tag = 1;
const TAG_ERR: Tag = 2;

/// Bail with a tagged reason. Deliberately does NOT reap the server: exiting
/// closes our pipe fds, the server sees stdin EOF and leaves, and a probe
/// failure is boot-fatal anyway (joey gates on it, so the guest is already
/// going down). If this probe is ever made non-fatal, this needs a kill+wait or
/// every failed boot leaks a multi-MB child.
fn fail(spec: &ProbeSpec, msg: &str) -> ! {
    t_putstr("lsp-probe: FAIL [");
    t_putstr(spec.name);
    t_putstr("] -- ");
    t_putstr(msg);
    t_putstr("\n");
    unsafe { t_exits(1) }
}

/// Read one `name:`-prefixed unsigned field out of a `/proc/<pid>/status` body.
fn status_field(body: &str, name: &str) -> Option<u32> {
    for line in body.lines() {
        let rest = match line.strip_prefix(name) {
            Some(r) => r.trim(),
            None => continue,
        };
        let digits: alloc::string::String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
        if digits.is_empty() {
            return None;
        }
        return digits.parse::<u32>().ok();
    }
    None
}

/// The server's peak anon commit and the budget it is measured against, in
/// PAGES, from `/proc/<pid>/status`.
///
/// Taken while the server is still ALIVE, deliberately. The kernel's `peak` is
/// MONOTONIC (devproc.c: "a read taken any time after the peak reports it"), so
/// a live read cannot under-report work already done -- whereas reading after
/// the reap is impossible, because the reap frees the Proc and takes the number
/// with it. joey's `go4c_spawn_wait_hb_peak` solves the same problem by
/// deferring the reap until ZOMBIE; here the peak is wanted for a GRANDchild
/// joey never sees, so the probe reads it itself.
///
/// Why this is printed at all: CL-5 measured a 1959-byte template-heavy C++ TU
/// at 250 MiB through cc1 -- 97.8% of the 256 MiB default budget. clangd runs
/// the same frontend. This probe's source is deliberately trivial, so the number
/// it reports is a FLOOR, not a verdict on real files; a floor already near the
/// ceiling would mean real C++ needs a budget lift (task #100). Reporting it
/// every boot is what makes that a tracked number rather than a periodic
/// re-guess.
fn server_peak(pid: i32) -> Option<(u32, u32)> {
    let path = alloc::format!("/proc/{}/status", pid);
    let mut f = fs::File::open(&path).ok()?;
    let mut buf = [0u8; 1024];
    let mut total = 0usize;
    while total < buf.len() {
        match f.read(&mut buf[total..]) {
            Ok(0) => break,
            Ok(k) => total += k,
            Err(_) => break,
        }
    }
    let body = core::str::from_utf8(&buf[..total]).ok()?;
    Some((status_field(body, "peak:")?, status_field(body, "budget:")?))
}

/// Write `text` to `path`, replacing whatever is there.
///
/// /tmp is disk-backed and survives reboots, so a stale file from a previous
/// boot would otherwise be analyzed instead of this one.
fn write_file(path: &str, text: &str) -> bool {
    let _ = fs::remove_file(path);
    match fs::File::create(path) {
        Ok(mut f) => f.write_all(text.as_bytes()).is_ok(),
        Err(_) => false,
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("lsp-probe: starting (live LSP round-trip, one session per server)\n");

    let mut ran: u32 = 0;
    for spec in PROBES {
        // An absent server is a legitimate image config (a non-bake build), and
        // the probe cannot manufacture a language server. A PRESENT one, on the
        // other hand, means the bake config -- where every step below MUST
        // work, so from there on nothing is skipped and nothing is soft.
        if !fs::exists(spec.bin) {
            t_putstr("lsp-probe: ");
            t_putstr(spec.name);
            t_putstr(" absent (");
            t_putstr(spec.bin);
            t_putstr(") -- skipping\n");
            continue;
        }
        run_session(spec);
        ran += 1;
    }

    if ran == 0 {
        t_putstr("lsp-probe: no language servers present -- skipping\n");
    } else {
        t_putstr("lsp-probe: PASS -- ");
        print_u32(ran);
        t_putstr(" server session(s) completed\n");
    }
    0
}

/// One full LSP session against one server. Returns only on success; every
/// failure path exits the process (boot-fatal by design).
fn run_session(spec: &ProbeSpec) {
    t_putstr("lsp-probe: [");
    t_putstr(spec.name);
    t_putstr("] session starting\n");

    // 1. The workspace. gopls resolves its view from the CWD (the go8d probe
    //    established this the hard way -- without a module cwd it reports "no
    //    views" and type-checks nothing), so chdir before the spawn and let the
    //    child inherit it. clangd resolves from the compilation database, but
    //    the same cwd discipline costs nothing and keeps the two paths uniform.
    if !fs::exists(spec.dir) && fs::create_dir(spec.dir).is_err() {
        fail(spec, "mkdir workspace");
    }
    for (path, text) in spec.aux {
        if !write_file(path, text) {
            fail(spec, "write workspace support file");
        }
    }
    if !write_file(spec.src, spec.src_text) {
        fail(spec, "write source file");
    }
    if env::set_current_dir(spec.dir).is_err() {
        fail(spec, "chdir workspace");
    }

    // 2. Spawn the way nora spawns -- bare, no args, inheriting env (PATH, so
    //    gopls can LookPath `go`) and caps (CSPRNG_READ, which crypto/rand
    //    needs at init). Deviating here would prove a path nora does not take.
    let mut cmd = Command::new(spec.bin);
    let mut srv = match Server::spawn(&mut cmd) {
        Ok(s) => s,
        Err(_) => fail(spec, "Server::spawn"),
    };

    // 3. The handshake.
    let uri = lsp::path_to_uri(spec.src);
    let root_uri = lsp::path_to_uri(spec.dir);
    let mut cl = lsp::Client::new();
    let init = cl.initialize(&root_uri);
    if srv.send(&init).is_err() {
        fail(spec, "send initialize");
    }

    let mut mux = Mux::new();
    let start = Instant::now();
    let mut next_hb = HEARTBEAT_MS;
    let mut ready_seen = false;
    // Counters printed on the PASS line. They are not decoration: they are how
    // a future reader tells a round-trip that really happened from one that got
    // lucky, and how a regression that silently stops the server talking to us
    // shows up in the boot log as a number rather than as nothing at all.
    let mut publishes: u32 = 0;
    let mut replies: u32 = 0;
    let mut phase = Phase::AwaitDiag;

    loop {
        let waited = start.elapsed().as_millis() as u64;
        if waited > BUDGET_MS {
            if !ready_seen {
                fail(spec, "no initialize response within budget");
            }
            t_putstr("lsp-probe: FAIL [");
            t_putstr(spec.name);
            t_putstr("] -- handshake completed but no matching diagnostic; publishes=");
            print_u32(publishes);
            t_putstr("\n");
            let _ = srv.kill();
            let _ = srv.wait();
            unsafe { t_exits(1) }
        }
        if waited >= next_hb {
            next_hb += HEARTBEAT_MS;
            t_putstr("lsp-probe: [");
            t_putstr(spec.name);
            t_putstr("] waiting (");
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
            Err(_) => fail(spec, "mux.poll"),
        };

        for r in &ready {
            match r.tag {
                // Drain and discard. A chatty server that fills its stderr pipe
                // BLOCKS -- which would present as the server mysteriously
                // going quiet, not as an error. clangd is markedly chattier
                // than gopls here, so this is load-bearing, not hygiene.
                TAG_ERR => {
                    if r.readable {
                        let _ = srv.drain_stderr();
                    }
                }
                TAG_OUT => {
                    if r.readable {
                        match srv.pump() {
                            Ok(false) => {}
                            Ok(true) => fail(spec, "stdout EOF (server exited)"),
                            Err(_) => fail(spec, "pump"),
                        }
                        loop {
                            let body = match srv.next_frame() {
                                Ok(Some(b)) => b,
                                Ok(None) => break,
                                Err(_) => fail(spec, "frame decode (stream desync)"),
                            };
                            if dispatch(
                                spec,
                                &body,
                                &mut cl,
                                &mut srv,
                                &uri,
                                &mut ready_seen,
                                &mut publishes,
                                &mut replies,
                                &mut phase,
                            ) {
                                // The planted diagnostic arrived and checked out.
                                // Sample the peak BEFORE the shutdown below --
                                // once reaped, the Proc and its counter are gone.
                                let peak = server_peak(srv.pid());
                                t_putstr("lsp-probe: [");
                                t_putstr(spec.name);
                                t_putstr("] OK -- handshake + didOpen + publishDiagnostics (");
                                print_u32(start.elapsed().as_millis() as u32);
                                t_putstr("ms, publishes=");
                                print_u32(publishes);
                                t_putstr(", auto-replies=");
                                print_u32(replies);
                                if let Some((pk, budget)) = peak {
                                    t_putstr(", peak=");
                                    print_u32(pk / 256);
                                    t_putstr("MiB/");
                                    print_u32(budget / 256);
                                    t_putstr("MiB");
                                }
                                t_putstr(")\n");
                                shutdown(&mut cl, &mut srv);
                                return;
                            }
                        }
                    } else if r.hup {
                        fail(spec, "stdout HUP");
                    }
                }
                _ => {}
            }
        }
    }
}

/// Handle one framed message. Returns true once the diagnostic we planted has
/// arrived and been verified.
#[allow(clippy::too_many_arguments)]
fn dispatch(
    spec: &ProbeSpec,
    body: &[u8],
    cl: &mut lsp::Client,
    srv: &mut Server,
    uri: &str,
    ready_seen: &mut bool,
    publishes: &mut u32,
    replies: &mut u32,
    phase: &mut Phase,
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
        // The auto-reply. Servers send real requests during startup
        // (registerCapability, workspace/configuration) and WAIT on the
        // answer -- a client that ignores them stalls forever, which is
        // precisely the failure no synthetic test can produce.
        Action::Send(reply) => {
            *replies += 1;
            if srv.send(&reply).is_err() {
                fail(spec, "send auto-reply (server stdin broken)");
            }
            false
        }
        Action::Ready => {
            *ready_seen = true;
            t_putstr("lsp-probe: [");
            t_putstr(spec.name);
            t_putstr("] initialize OK -- server accepted our capabilities\n");
            let note = cl.initialized();
            if srv.send(&note).is_err() {
                fail(spec, "send initialized");
            }
            let open = cl.did_open(uri, spec.language_id, 1, spec.src_text);
            if srv.send(&open).is_err() {
                fail(spec, "send didOpen");
            }
            false
        }
        Action::Diagnostics(published_uri) => {
            if published_uri != uri {
                // Servers publish for every file they know about; ours is the
                // only one that counts.
                return false;
            }
            *publishes += 1;
            // Forbidden messages FIRST. Checked before the planted match
            // because a badly-misconfigured server reports both -- and if the
            // planted match returned first, the misconfiguration would never be
            // seen. This is the difference between "clangd answered" and
            // "clangd is usable".
            for d in cl.diagnostics_for(uri) {
                for bad in spec.forbid {
                    if d.message.contains(bad) {
                        t_putstr("lsp-probe: FAIL [");
                        t_putstr(spec.name);
                        t_putstr("] -- forbidden diagnostic: \"");
                        t_putstr(&d.message);
                        t_putstr("\"\n");
                        unsafe { t_exits(1) }
                    }
                }
            }
            // An EMPTY publish is normal and NOT a failure: servers commonly
            // publish `[]` for a freshly-opened file before the analysis
            // finishes, then republish with the real errors. Waiting through
            // that is the whole reason this loop is deadline-bounded rather
            // than first-publish-wins.
            for d in cl.diagnostics_for(uri) {
                if !d.message.contains(spec.bad_ident) {
                    continue;
                }
                if d.severity != Severity::Error {
                    fail(spec, "planted error reported at non-Error severity");
                }
                // The line is what proves the RANGE decoded, not just that some
                // string arrived.
                if d.range.start.line != spec.bad_line {
                    t_putstr("lsp-probe: FAIL [");
                    t_putstr(spec.name);
                    t_putstr("] -- diagnostic line ");
                    print_u32(d.range.start.line);
                    t_putstr(", expected ");
                    print_u32(spec.bad_line);
                    t_putstr("\n");
                    unsafe { t_exits(1) }
                }
                t_putstr("lsp-probe: [");
                t_putstr(spec.name);
                t_putstr("] diagnostic OK -- \"");
                t_putstr(&d.message);
                t_putstr("\" at line ");
                print_u32(d.range.start.line);
                t_putstr("\n");
                match spec.intel_at {
                    None => return true,
                    Some((line, character)) => {
                        // Diagnostics are done; ask for hover next. Sent from
                        // here rather than up-front because a request issued
                        // before the server finished its first analysis gets a
                        // null answer that says nothing about whether the
                        // index works.
                        let req = cl.hover(uri, lsp::Position { line, character });
                        if srv.send(&req).is_err() {
                            fail(spec, "send hover");
                        }
                        *phase = Phase::AwaitHover;
                        return false;
                    }
                }
            }
            false
        }
        Action::Hover(text) => {
            if *phase != Phase::AwaitHover {
                return false;
            }
            let body = match text {
                Some(t) if !t.is_empty() => t,
                // A server that answers "I know nothing about this name" at a
                // position it just type-checked is a real finding, not chatter.
                _ => fail(spec, "hover returned nothing at the probed position"),
            };
            t_putstr("lsp-probe: [");
            t_putstr(spec.name);
            t_putstr("] hover OK -- \"");
            t_putstr(first_line(&body));
            t_putstr("\"\n");
            let (line, character) = match spec.intel_at {
                Some(p) => p,
                None => return true,
            };
            let req = cl.definition(uri, lsp::Position { line, character });
            if srv.send(&req).is_err() {
                fail(spec, "send definition");
            }
            *phase = Phase::AwaitDef;
            false
        }
        Action::Definition(loc) => {
            if *phase != Phase::AwaitDef {
                return false;
            }
            let loc = match loc {
                Some(l) => l,
                _ => fail(spec, "definition returned nothing at the probed position"),
            };
            // The URI is the payload: for the with-headers spec it must land
            // INSIDE libc++, which is what proves clangd indexed the header
            // rather than merely finding it on disk.
            t_putstr("lsp-probe: [");
            t_putstr(spec.name);
            t_putstr("] definition OK -> ");
            t_putstr(&loc.uri);
            t_putstr(":");
            print_u32(loc.range.start.line);
            t_putstr("\n");
            true
        }
        // Everything else is server chatter (progress, log messages, an
        // unmatched response). Tolerated by design.
        _ => false,
    }
}

/// The orderly LSP goodbye, then make sure the child is really gone: a server
/// that ignores the protocol shutdown would otherwise outlive the probe as an
/// orphan holding the workspace -- and with two sessions in one process, an
/// orphaned first server would still be indexing while the second runs.
fn shutdown(cl: &mut lsp::Client, srv: &mut Server) {
    let bye = cl.shutdown();
    let _ = srv.send(&bye);
    let exit = cl.exit();
    let _ = srv.send(&exit);
    srv.close_stdin();
    let _ = srv.kill();
    let _ = srv.wait();
}

/// Hover bodies are markdown blocks; the boot log wants one informative line.
///
/// Skips blank lines and ``` fences, which are what both servers lead with --
/// printing the fence would put a literal "```cpp" in the boot log and say
/// nothing about whether the hover was real.
fn first_line(s: &str) -> &str {
    for line in s.split('\n') {
        let t = line.trim();
        if t.is_empty() || t.starts_with("```") {
            continue;
        }
        return t;
    }
    s
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
