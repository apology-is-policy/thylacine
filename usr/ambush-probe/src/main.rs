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
// wait-file/stop semantics, and (8c-2) the stop-of-a-sleeper that makes a
// multi-M Go target fully-stoppable. BOOT-FATAL: it asserts the inspect markers
// (a goroutine listing, the main.parkLoop bt frame, and the Sentinel value read
// from the target's memory) -- all three require a fully-stopped multi-M target.
// It gates on the markers, NOT ambush's exit code (Delve exits 1 on the EOF'd
// "kill? [Y/n]" prompt, a cosmetic artifact; the debug session succeeded).
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

// The full attach E2E below halts + inspects a MULTI-THREADED Go target -- which
// works because of 8c-2 (the stop-of-a-sleeper, DEBUG-FS-DESIGN 5c): a debugger
// stop now WAKES a Go M parked in an indefinite futex/torpor wait so it detours
// to park on its debug_rendez, so devproc_all_threads_parked is satisfied and the
// halt completes instead of hanging forever. ATTACH_ENABLED is a plain disable
// switch (leave it true); it flips false only to skip stage B for a fast
// iteration loop. See docs/DELVE-PORT-DESIGN.md 17-18 + DEBUG-FS-DESIGN.md 5c.
const ATTACH_ENABLED: bool = true;

// Stage C (launch E2E) drives `ambush exec /ambush-child` -- Ambush SPAWNS the
// child, stops it before main.main, sets a HARDWARE breakpoint at main.parkLoop,
// `continue`s into it, then inspects. This is the 8c-4 launch + the fork's 8c-2
// HW-breakpoint routing (DELVE-PORT-DESIGN section 6: every bp routes to the
// kernel hwbreak path; I-12/I-36 forbid a software BRK into shared RO+X text) +
// the kernel #95 focus-thread fix (the debug-fs reports the M that HIT the bp,
// not the head -- a Go bp fires on the migrated-goroutine's M).
const LAUNCH_ENABLED: bool = true;

// --- Stage B: attach E2E (the 8c-2 multi-thread-stop proof) ---
// Returns true on PASS or a legitimate SKIP (forks absent); false only when the
// attach reached the target but the inspect markers are missing (a real
// regression). With 8c-2 (the stop-of-a-sleeper), a multi-M Go target becomes
// fully-stopped, so `goroutines`/`bt`/`print` all complete.
fn attach_e2e() -> bool {
    if !ATTACH_ENABLED {
        t_putstr("ambush-probe: stage B DISABLED (ATTACH_ENABLED=false)\n");
        return true;
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
            return true;
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
            return true;
        }
    };
    // Close Ambush's stdin -> its REPL reads EOF after the init file -> exits +
    // detaches. Ambush exits status 1 on the EOF'd "kill the process? [Y/n]"
    // prompt (a cosmetic Delve exit artifact -- the debug session succeeded), so
    // this probe gates on the INSPECT MARKERS below, not ambush's exit code.
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

    // The 8c-2 inspect proof: `goroutines` listed them, `bt` unwound the stack
    // (the parkLoop frame), and `print main.Sentinel` read the target's memory --
    // ALL THREE require a FULLY-STOPPED multi-M Go target, which the stop-of-a-
    // sleeper (a detour-parked idle futex M) now delivers. SENTINEL is
    // 0x0AABB00DCAFE0001 == 768901734683508737 (see ambush-child).
    let saw_goroutine = contains(&out, b"Goroutine") || contains(&out, b"goroutine");
    // A REAL `bt` frame: Delve prints frames as `N  0xADDR in FUNC` (frame 0 is
    // `0  0x`). The prior `parkLoop` marker was a FALSE POSITIVE -- it matched the
    // `goroutines` listing's `Goroutine 1 - ... main.parkLoop`, NOT a bt frame (the
    // head thread's bt unwinds the M's runtime stack -- torpor_wake/mcall -- and
    // never contains "parkLoop"). `bt` reads /proc/<pid>/regs, the exact path the
    // #88 regression EPERM'd, so gating on a real frame is the honest bt proof.
    let saw_bt = contains(&out, b"0  0x");
    let saw_sentinel = contains(&out, b"768901734683508737");
    let saw_output = !out.is_empty();
    t_putstr(&format!(
        "ambush-probe: stage B markers -- output={} goroutine={} bt={} sentinel={}\n",
        saw_output as u8, saw_goroutine as u8, saw_bt as u8, saw_sentinel as u8
    ));

    reap_child(&mut kid);
    saw_goroutine && saw_bt && saw_sentinel
}

// --- Stage C: launch E2E (the 8c-4 + fork-8c-2 + kernel-#95 HW-breakpoint proof) ---
// `ambush exec /ambush-child`: Ambush SPAWNS the child (attach-first Launch),
// stops it before main.main, sets a HARDWARE breakpoint at main.parkLoop, then
// `continue` runs the target INTO the breakpoint (a HW code bp fires with PC ==
// the bp'd instruction, on whichever M runs the migrated goroutine -- kernel #95
// focuses that M). The inspect commands run against the bp-stopped multi-M
// target; stdin EOF exits the REPL (killing the launched child). This is the
// first end-to-end exercise of break + continue + the step-over-breakpoint dance.
// Returns true on PASS or a legitimate SKIP (forks absent); false only when
// Ambush ran but the bp/inspect markers are missing (a real regression).
fn launch_e2e() -> bool {
    if !LAUNCH_ENABLED {
        t_putstr("ambush-probe: stage C DISABLED (LAUNCH_ENABLED=false)\n");
        return true;
    }
    let mut amb = match Command::new("/ambush")
        .args([
            "exec",
            "/ambush-child",
            "--init",
            "/ambush-init-exec",
            "--allow-non-terminal-interactive=true",
        ])
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            t_putstr("ambush-probe: stage C SKIP (ambush not baked for exec)\n");
            return true;
        }
    };
    // Close Ambush's stdin -> the REPL reads EOF after the init file -> exits +
    // kills the launched child. Ambush exits status 1 on the EOF'd exit prompt (a
    // cosmetic Delve artifact), so this gates on the MARKERS below, not the code.
    let _ = amb.stdin.take();

    // Bounded wait (~18s): launch does spawn + attach + stop + bp-arm + a
    // `continue` that must reach main.parkLoop before the bp fires + re-stops.
    let mut exited = false;
    for _ in 0..180 {
        match amb.try_wait() {
            Ok(Some(s)) => {
                t_putstr(&format!(
                    "ambush-probe: stage C -- ambush exec exited status {}\n",
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
        t_putstr("ambush-probe: stage C -- ambush exec did not exit in ~18s; killing\n");
        let _ = amb.kill();
        let _ = amb.wait();
    }

    let out = drain(&mut amb.stdout);
    let err = drain(&mut amb.stderr);
    echo_block("ambush-probe: === ambush exec STDOUT ===\n", &out);
    echo_block("ambush-probe: === ambush exec STDERR ===\n", &err);

    // The HW-breakpoint proof: `break` set the bp (routed to the kernel hwbreak
    // path -- a routing failure errors here, so the marker is missing), `continue`
    // reached it, and `bt`/`print` inspected the bp-stopped target. A HW code bp
    // fires with PC == parkLoop (ELR, not yet executed), so frame 0 of the bt IS
    // main.parkLoop. SENTINEL is 0x0AABB00DCAFE0001 == 768901734683508737.
    let saw_bp_set = contains(&out, b"Breakpoint 1");
    let saw_bt = contains(&out, b"0  0x");
    let saw_parkloop = contains(&out, b"parkLoop");
    let saw_sentinel = contains(&out, b"768901734683508737");
    t_putstr(&format!(
        "ambush-probe: stage C markers -- bp_set={} bt={} parkloop={} sentinel={}\n",
        saw_bp_set as u8, saw_bt as u8, saw_parkloop as u8, saw_sentinel as u8
    ));

    saw_bp_set && saw_bt && saw_parkloop && saw_sentinel
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
    // Stage B: the 8c-2 attach + inspect proof (boot-fatal). A legitimate SKIP
    // (forks absent) returns true; a broken attach (markers missing) returns false.
    if !attach_e2e() {
        t_putstr(
            "ambush-probe: stage B FAIL -- attach reached the target but the inspect \
             markers (goroutines / parkLoop bt frame / Sentinel) are missing (an \
             8c-2 multi-thread-stop regression?)\n",
        );
        unsafe { t_exits(1) }
    }
    // Stage C: the 8c-4 launch + fork-8c-2 routing + kernel-#95 focus-thread proof
    // (boot-fatal). A legitimate SKIP (forks absent) returns true; a broken
    // break/continue (markers missing) returns false.
    if !launch_e2e() {
        t_putstr(
            "ambush-probe: stage C FAIL -- ambush exec ran but the HW-breakpoint \
             markers (Breakpoint set / parkLoop bt frame / Sentinel) are missing (a \
             break/continue regression -- did the bp reach the kernel hwbreak path, \
             and does the debug-fs focus the firing M? kernel #95)\n",
        );
        unsafe { t_exits(1) }
    }
    t_putstr(
        "ambush-probe: PASS (stage A: ambush runs; stage B: multi-thread Go attach + \
         goroutines + bt + print; stage C: launch + HW breakpoint + continue + bt + print)\n",
    );
    unsafe { t_exits(0) }
}
