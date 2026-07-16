// /ambush-probe -- the Stage-8c-1 Ambush (Delve port) in-guest E2E.
//
// Stage A (version smoke, GATED): prove the cross-built Ambush binary EXECUTES
// -- `ambush version` drives the whole Go runtime + the cobra command tree (710
// vendored deps) + an fd-1 write + exit_group. Isolated from the backend so a
// failure pins "binary does not run" vs "backend is wrong".
//
// Stage B (attach E2E, SOFT -- iteration 1): the backend exercise. Spawn the
// parking /ambush-child (a known global + a named park loop), attach Ambush
// non-interactively (`ambush attach <pid> /ambush-child --init /ambush-init` ->
// goroutines/bt/print, then stdin EOF exits the REPL), bounded-wait it, and drain
// + LOG its stdout/stderr verbatim. This is the first exercise of the
// proc_thylacine backend against a real M-threaded Go target: the non-PIE
// EntryPoint==0, the exe-path DWARF load, the iscgo/tpidr Go-g recovery, and the
// wait-file/stop semantics. Kept SOFT (rich logging, boot never gated on it) for
// the first iteration so the boot completes and the actual Delve output is
// captured to ground-truth what works; it tightens to a hard assertion once the
// output shape is known.
//
// Ambush is OPTIONAL infra (baked only when both forks were present at build). An
// unbaked /ambush makes stage A SKIP (exit 0) so a fork-absent build still boots;
// a present-but-broken `ambush version` FAILS the boot (the 8c regression
// sentinel). joey spawns /ambush-probe boot-fatally.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use alloc::vec::Vec;
use libthyla_rs::fs::File;
use libthyla_rs::io::Read;
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{t_exits, t_putstr};

// Substring search over captured bytes (no_std, no regex).
fn contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if hay.len() < needle.len() {
        return false;
    }
    hay.windows(needle.len()).any(|w| w == needle)
}

// Read a piped child's fd to EOF (bounded). The fd EOFs once the child closes its
// write end -- on exit (the #68 last-thread-out fd close) or when killed.
fn drain(f: &mut Option<File>) -> Vec<u8> {
    let mut v: Vec<u8> = Vec::new();
    if let Some(file) = f.as_mut() {
        let mut buf = [0u8; 512];
        loop {
            match file.read(&mut buf) {
                Ok(0) => break,
                Ok(n) => {
                    if v.len() < 32 * 1024 {
                        v.extend_from_slice(&buf[..n]);
                    }
                }
                Err(_) => break,
            }
        }
    }
    v
}

fn echo_block(tag: &str, bytes: &[u8]) {
    t_putstr(tag);
    if let Ok(s) = core::str::from_utf8(bytes) {
        t_putstr(s);
        if !s.ends_with('\n') {
            t_putstr("\n");
        }
    } else {
        t_putstr("<non-utf8 output>\n");
    }
}

// --- Stage A: version smoke (gated) ---
fn version_smoke() -> Result<(), &'static str> {
    let mut child = match Command::new("/ambush")
        .arg("version")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            t_putstr("ambush-probe: SKIP (ambush not baked -- Go/Ambush fork absent at build)\n");
            unsafe { t_exits(0) }
        }
    };
    let out = drain(&mut child.stdout);
    let st = child
        .wait()
        .map_err(|_| "ambush-probe: FAIL -- reap ambush version\n")?;
    echo_block("ambush-probe: ambush version output follows:\n", &out);
    if !st.success() {
        return Err("ambush-probe: FAIL -- ambush version exited non-zero (Go throw / snare?)\n");
    }
    if !contains(&out, b"Version") {
        return Err("ambush-probe: FAIL -- ambush version banner missing 'Version'\n");
    }
    Ok(())
}

// The full attach E2E below is written + PROVEN to REACH the target -- Delve
// logs "attaching to pid <N>" (captured with --log) -- but it then HANGS at the
// halt on a MULTI-THREADED Go target: an idle Go M parked in a futex/torpor wait
// never reaches the EL0-return stop checkpoint, so kernel devproc_all_threads_
// parked (rendez_blocked_on == &debug_rendez for EVERY thread) is never satisfied
// and `ctl stop` blocks forever. This is a DELIBERATE v1.x kernel seam
// (kernel/proc.c::proc_debug_stop_deliver: "a thread blocked in a syscall SLEEP
// is deliberately NOT interrupted ... a v1.x refinement" -- the Plan 9
// non-preemptive stop). It is the #811-analog for debug-stop: an explicit
// stop-of-a-sleeper (wake interruptible sleepers -> drive them to the checkpoint
// -> restart the syscall on resume). Until that lands, the attach is GATED OFF so
// it does not hang every boot; flip ATTACH_ENABLED + turn the markers into a
// gated assertion once the seam closes. See docs/DELVE-PORT-DESIGN.md 17.
const ATTACH_ENABLED: bool = false;

// --- Stage B: attach E2E (gated on the multi-thread-stop seam) ---
fn attach_e2e() {
    if !ATTACH_ENABLED {
        t_putstr(
            "ambush-probe: stage B GATED -- the attach E2E reaches the target but \
             halt hangs on a multi-thread Go target (a sleeping Go M never reaches \
             the debug-stop checkpoint; kernel v1.x seam). Stage A proves Ambush runs.\n",
        );
        return;
    }
    // The parking Go target. Piped stdio (it prints nothing, parks); Piped keeps
    // it from cloning this fd-less probe's absent 0/1/2.
    let mut kid = match Command::new("/ambush-child")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            t_putstr("ambush-probe: stage B SKIP (ambush-child not baked)\n");
            return;
        }
    };
    let pid = kid.pid();
    t_putstr(&format!("ambush-probe: stage B -- ambush-child pid {}\n", pid));
    // Let it get past runtime init into parkLoop (the global is PC-independent,
    // but attach wants a settled target).
    let _ = sleep(Duration::from_millis(400));

    let pid_s = format!("{}", pid);
    let mut amb = match Command::new("/ambush")
        // --allow-non-terminal-interactive: our isatty shim returns false
        // (Thylacine has no termios tty), so Delve's non-terminal guard fires
        // before attach; this flag drives it from a pipe (the init file + EOF).
        .args([
            "attach",
            pid_s.as_str(),
            "/ambush-child",
            "--init",
            "/ambush-init",
            "--allow-non-terminal-interactive=true",
        ])
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            t_putstr("ambush-probe: stage B SKIP (ambush not baked for attach)\n");
            reap_child(&mut kid);
            return;
        }
    };
    // Close Ambush's stdin -> its REPL reads EOF after the init file -> exits +
    // detaches (no `exit` command, which would prompt to kill the target).
    let _ = amb.stdin.take();

    // Bounded wait (~12s): guards a hang (an EOF-exit that never fires, or a
    // backend stall). try_wait with WNOHANG reaps on exit.
    let mut exited = false;
    for _ in 0..120 {
        match amb.try_wait() {
            Ok(Some(s)) => {
                t_putstr(&format!(
                    "ambush-probe: stage B -- ambush attach exited status {}\n",
                    s.raw()
                ));
                exited = true;
                break;
            }
            Ok(None) => {
                let _ = sleep(Duration::from_millis(100));
            }
            Err(_) => break,
        }
    }
    if !exited {
        t_putstr("ambush-probe: stage B -- ambush attach did not exit in ~25s; killing\n");
        let _ = amb.kill();
        let _ = amb.wait();
    }

    // Drain + log Ambush's attach output (stdout has the command results; stderr
    // has any Delve error). Both fds EOF now that ambush exited/was-killed.
    let out = drain(&mut amb.stdout);
    let err = drain(&mut amb.stderr);
    echo_block("ambush-probe: === ambush attach STDOUT ===\n", &out);
    echo_block("ambush-probe: === ambush attach STDERR ===\n", &err);

    // Soft report: which expected markers appeared (goroutine listing / the
    // parkLoop frame / a non-empty result). NOT gated this iteration.
    let saw_goroutine = contains(&out, b"Goroutine") || contains(&out, b"goroutine");
    let saw_park = contains(&out, b"parkLoop");
    let saw_output = !out.is_empty();
    t_putstr(&format!(
        "ambush-probe: stage B markers -- output={} goroutine={} parkLoop={}\n",
        saw_output as u8, saw_goroutine as u8, saw_park as u8
    ));

    reap_child(&mut kid);
}

// killgrp + reap the parking target (its loop is unbounded).
fn reap_child(kid: &mut Child) {
    let _ = kid.kill();
    let _ = kid.wait();
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("ambush-probe: starting (8c-1 Ambush E2E: version smoke + attach)\n");
    match version_smoke() {
        Ok(()) => {
            t_putstr("ambush-probe: stage A PASS (ambush runs; version banner ok)\n");
        }
        Err(msg) => {
            t_putstr(msg);
            unsafe { t_exits(1) }
        }
    }
    // Stage B is SOFT this iteration: log-rich, never gates the boot.
    attach_e2e();
    t_putstr("ambush-probe: DONE (stage A gated PASS; stage B soft -- see attach output above)\n");
    unsafe { t_exits(0) }
}
