// /u-7-test -- the U-7 job-control arc CUMULATIVE composition smoke.
//
// Where the per-sub-chunk probe (/u-job-test, 19 scenarios) proves each U-7
// surface in ISOLATION -- the `&` background launch + the job table, the
// jobs/fg/bg/wait/kill builtins, `on note`/`mask note` runtime delivery, and
// the interruptible foreground wait (Ctrl-C forward) -- this probe proves they
// COMPOSE, the way a real interactive session weaves them: a background job
// surviving a foreground command's by-pid demux and then drained by the
// builtins; ONE `interrupt` handler observed in two contexts (forwarded to a
// running foreground child vs fired by the shell at the idle prompt); a
// `mask note` body that performs a job-control reap mid-defer; and (the
// capstone) the whole stack driven through the real prompt cycle via
// libutopia::repl::Repl::feed (run-line -> reap_jobs -> deliver_notes).
//
// Runs PRE-pivot (flat devramfs root, where echo/seq/tr spawn by name). The
// harness cannot inject /dev/cons keystrokes (the A-4c constraint), so a
// foreground Ctrl-C is exercised by self-posting the `interrupt` the kernel
// console owner would post; the observables are $status, the JobTable, and the
// Env note state, all read directly (the `[N] PID` / `[N]+ Done` chatter goes
// to the UART). Every spawned child is reaped -- an orphan would re-parent to
// joey and trip its wrong-pid reap (joey.c).
//
// Flows (each composes >=2 U-7 surfaces):
//   1. bg-demux + builtins  -- two `&` jobs survive a foreground `seq`, then
//                              `wait` + `jobs` drain them (U-7a + U-7b). Runs
//                              with no note queue: the foreground wait takes
//                              the DEGRADE (plain blocking by-pid) path.
//   2. forward-vs-handle    -- one `on note 'interrupt'` handler + a counter,
//                              observed firing at the prompt (U-7c-a delivery)
//                              but NOT during a foreground wait, where the same
//                              note is forwarded to the child (U-7c-b).
//   3. mask + reap          -- a `mask note 'interrupt'` body runs `wait %1`
//                              (a job-control reap) while the note is deferred;
//                              the handler fires only at block exit, AFTER the
//                              reap (U-7c-a mask + U-7a/U-7b interleaved).
//   4. integrated REPL      -- Repl::feed drives note registration + delivery
//                              + a backgrounded job + the integrated reaper +
//                              `exit`, all through the real prompt cycle.
//
// joey gates the boot on this binary's status == 0. The reap loops are bounded
// (echo/seq exit in microseconds; the bound is a safety net, never the steady
// state) and yield via time::sleep between WNOHANG polls (U-7-pre F1: never
// hot-loop wait_pid_for).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::notes::{self, NoteTarget};
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::time::{self, Duration};
use libthyla_rs::t_putstr;
use libutopia::eval::{deliver_pending_notes, eval_source, wait_pids_interruptible, Env};
use libutopia::repl::Repl;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    if let Err(rc) = flow_bg_survives_fg() {
        return rc;
    }
    if let Err(rc) = flow_forward_vs_handle() {
        return rc;
    }
    if let Err(rc) = flow_mask_with_reap() {
        return rc;
    }
    if let Err(rc) = flow_repl_integrated() {
        return rc;
    }

    t_putstr("u-7-test: all OK\n");
    0
}

// Flow 1 -- the canonical "a build runs in the background while I run a quick
// command" idiom. Two `&` jobs are registered; a FOREGROUND external (`seq`
// with no args exits 1) is then run: the U-7-pre by-pid foreground wait reaps
// ITS pid only, so both background jobs survive the demux and the foreground
// $status is the command's own, not a background job's. `wait` (no args) then
// blocks on + reaps every background job, and `jobs` consumes the now-Done
// entries exactly once, draining the table. Composes U-7a (bg register + the
// pid-demux) + U-7b (`wait` + `jobs`). No note queue is opened, so the
// foreground wait takes the DEGRADE path (a plain blocking by-pid reap -- the
// host / boot-check posture), exercising that arm under a real bg-coexistence.
fn flow_bg_survives_fg() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;

    if eval_source(&mut env, "echo one &").is_err() {
        return fail("flow 1: `echo one &` errored");
    }
    if eval_source(&mut env, "echo two &").is_err() {
        return fail("flow 1: `echo two &` errored");
    }
    if env.jobs().len() != 2 || env.jobs().live_pids().len() != 2 {
        return fail("flow 1: two background jobs were not both registered");
    }

    // `seq` with no args is an EXTERNAL command that exits 1 (usage error to
    // stderr) -- a deterministic non-zero foreground status that cannot be
    // confused with a background job's exit 0.
    if eval_source(&mut env, "seq").is_err() {
        return fail("flow 1: foreground `seq` errored");
    }
    if env.status() != 1 {
        return fail("flow 1: foreground $status clobbered (expected 1)");
    }
    if env.jobs().len() != 2 || env.jobs().live_pids().len() != 2 {
        return fail("flow 1: the foreground wait wrongly reaped a background job");
    }

    if eval_source(&mut env, "wait").is_err() {
        return fail("flow 1: `wait` errored");
    }
    if env.status() != 0 || !env.jobs().live_pids().is_empty() {
        return fail("flow 1: `wait` did not reap every background job");
    }
    if eval_source(&mut env, "jobs").is_err() {
        return fail("flow 1: `jobs` errored");
    }
    if !env.jobs().is_empty() {
        return fail("flow 1: `jobs` did not consume the completed jobs");
    }

    t_putstr("u-7-test: bg demux survives fg + wait/jobs drain OK\n");
    Ok(())
}

// Flow 2 -- the forward-vs-handle distinction, the heart of the U-7c
// composition: ONE `on note 'interrupt'` handler incrementing one counter,
// observed in the two contexts a real session produces.
//
//   (a) At the idle prompt (no foreground job), an `interrupt` runs the
//       shell's handler -- the U-7c-a delivery path (deliver_pending_notes).
//   (b) During a foreground command, an `interrupt` is FORWARDED to the
//       running child and is NEVER dispatched to the shell handler (scripture
//       10.2 + U-7c-b). `wait_pids_interruptible` has no `on note` dispatch at
//       all, so the counter is unchanged across the wait regardless of which
//       timing arm runs (the immediate reap, or the poll-and-forward).
//
// Proves U-7c-a + U-7c-b compose on the SAME handler, distinguished purely by
// foreground presence.
fn flow_forward_vs_handle() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;
    match notes::Notes::open_self() {
        Ok(nq) => env.set_notes(Some(nq)),
        Err(_) => return fail("flow 2: could not open the self note queue"),
    }
    deliver_pending_notes(&mut env); // clear leftover child_exit notes (flow 1)

    if eval_source(&mut env, "let fires = 0").is_err() {
        return fail("flow 2: `let fires = 0` errored");
    }
    if eval_source(&mut env, "on note 'interrupt' { let fires = (( $fires + 1 )) }").is_err() {
        return fail("flow 2: registering the interrupt handler errored");
    }

    // (a) At the prompt: the handler fires on delivery.
    if notes::send(NoteTarget::SelfProc, "interrupt").is_err() {
        return fail("flow 2: self-post of the prompt interrupt failed");
    }
    deliver_pending_notes(&mut env);
    if env.get("fires").as_scalar() != "1" {
        return fail("flow 2: the handler did not fire at the prompt");
    }

    // (b) During a foreground command: the interrupt is forwarded, NOT handled.
    match Command::new("echo")
        .arg("x")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(mut child) => {
            drop(child.stdin.take());
            drop(child.stdout.take());
            drop(child.stderr.take());
            let pid = child.pid();
            if notes::send(NoteTarget::SelfProc, "interrupt").is_err() {
                return fail("flow 2: self-post of the foreground interrupt failed");
            }
            let statuses = wait_pids_interruptible(&mut env, &[pid]);
            if statuses.len() != 1 || statuses[0] != 0 {
                return fail("flow 2: the foreground wait did not reap `echo x`");
            }
        }
        Err(_) => return fail("flow 2: could not spawn the foreground `echo`"),
    }
    // The load-bearing assertion: the shell handler did NOT run inside the
    // foreground wait. Deterministic in both arms (queued-and-untouched, or
    // drained-and-forwarded) -- `fires` stays 1.
    if env.get("fires").as_scalar() != "1" {
        return fail("flow 2: the shell handler fired DURING the foreground wait (must forward)");
    }

    // Cleanup: a fast-reap arm may have left the foreground interrupt queued;
    // draining it now may run the handler (fires -> 2) or find nothing (still
    // 1) -- both correct, so the post-drain count is not asserted.
    deliver_pending_notes(&mut env);

    t_putstr("u-7-test: interrupt forward (fg) vs handle (prompt) OK\n");
    Ok(())
}

// Flow 3 -- a note-mask body interleaved with a job-control reap. A background
// job is registered; an `interrupt` is queued; a `mask note 'interrupt'` block
// then runs `wait %1`, reaping the background job WHILE the interrupt is held
// back by the kernel Thread mask. The deferred handler fires only at the
// post-block drain (scripture 10.8) -- after the reap completed inside the
// masked region. Proves U-7c-a (mask defer + drain-at-exit) composes with
// U-7a/U-7b (a `&` job reaped via `wait %N`) in one evaluation.
fn flow_mask_with_reap() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;
    match notes::Notes::open_self() {
        Ok(nq) => env.set_notes(Some(nq)),
        Err(_) => return fail("flow 3: could not open the self note queue"),
    }
    deliver_pending_notes(&mut env); // clear leftovers (flow 2)

    if eval_source(&mut env, "on note 'interrupt' { let mfired = yes }").is_err() {
        return fail("flow 3: registering the interrupt handler errored");
    }
    if eval_source(&mut env, "echo bgm &").is_err() {
        return fail("flow 3: `echo bgm &` errored");
    }
    if env.jobs().live_pids().len() != 1 {
        return fail("flow 3: the background job was not registered");
    }

    if notes::send(NoteTarget::SelfProc, "interrupt").is_err() {
        return fail("flow 3: self-post of `interrupt` failed");
    }
    // No delivery point has passed (eval_source does not drain), so the handler
    // must not have fired yet.
    if env.defined("mfired") {
        return fail("flow 3: the handler fired before any delivery point");
    }

    // The masked body performs the job-control reap; the interrupt is deferred
    // across it and fires at block exit.
    if eval_source(&mut env, "mask note 'interrupt' { wait %1 }").is_err() {
        return fail("flow 3: the masked `wait %1` block errored");
    }
    if !env.jobs().live_pids().is_empty() {
        return fail("flow 3: the masked `wait %1` did not reap the background job");
    }
    if env.get("mfired").as_scalar() != "yes" {
        return fail("flow 3: the deferred handler did not fire at mask-block exit");
    }

    // The reaped job is left Done for the prompt cycle; `jobs` consumes it.
    if eval_source(&mut env, "jobs").is_err() {
        return fail("flow 3: `jobs` errored");
    }
    if !env.jobs().is_empty() {
        return fail("flow 3: `jobs` did not consume the reaped job");
    }

    t_putstr("u-7-test: mask-note body reaps a job mid-defer OK\n");
    Ok(())
}

// Flow 4 -- the capstone: the whole U-7 stack driven through the REAL prompt
// cycle via Repl::feed (each accepted line runs run_line -> reap_jobs ->
// deliver_notes). `open_notes` arms the integrated note-delivery half; a
// handler is registered, an `interrupt` posted, and the NEXT feed's prompt
// cycle delivers it. A `&` job is then backgrounded through feed and drained
// by the integrated reaper. `exit 0` ends the session through the same path.
// Proves U-7a (bg register + reap) + U-7c (note register + delivery) all run
// through the U-6g entrypoint, with state carried across feeds.
fn flow_repl_integrated() -> Result<(), i64> {
    let mut repl = Repl::new();
    let mut sink: Vec<u8> = Vec::new();

    // Arm the integrated note-delivery half, then drain the child_exit notes
    // the earlier flows' reaped children left in this Proc's shared queue
    // (no handler -> dropped).
    repl.open_notes();
    repl.deliver_notes();

    if repl
        .feed(b"on note 'interrupt' { let rang = yes }\n", &mut sink)
        .is_some()
    {
        return fail("flow 4: the `on note` line unexpectedly ended the session");
    }
    // Post an interrupt; the NEXT accepted line's prompt cycle (reap_jobs +
    // deliver_notes) fires the handler -- delivery integrated into feed.
    if notes::send(NoteTarget::SelfProc, "interrupt").is_err() {
        return fail("flow 4: self-post of `interrupt` failed");
    }
    if repl.feed(b"let probe = 1\n", &mut sink).is_some() {
        return fail("flow 4: the probe line unexpectedly ended the session");
    }
    if repl.env().get("rang").as_scalar() != "yes" {
        return fail("flow 4: the integrated prompt cycle did not deliver the note");
    }
    if repl.env().get("probe").as_scalar() != "1" {
        return fail("flow 4: the probe assignment did not take through the REPL");
    }

    // Background a job through feed, then drive the integrated reaper to
    // completion (bounded; echo exits in microseconds, the bound is a net).
    if repl.feed(b"echo capjob &\n", &mut sink).is_some() {
        return fail("flow 4: the `echo capjob &` line unexpectedly ended the session");
    }
    let mut drained = false;
    for _ in 0..2000 {
        repl.reap_jobs();
        if repl.env().jobs().is_empty() {
            drained = true;
            break;
        }
        let _ = time::sleep(Duration::from_millis(2));
    }
    if !drained {
        return fail("flow 4: the integrated reaper did not drain the bg job");
    }

    // `exit 0` ends the session through the same feed path.
    match repl.feed(b"exit 0\n", &mut sink) {
        Some(0) => {}
        Some(_) => return fail("flow 4: exit returned the wrong code"),
        None => return fail("flow 4: exit did not end the session"),
    }

    t_putstr("u-7-test: integrated Repl (note delivery + bg reap + exit) OK\n");
    Ok(())
}

fn fail(tag: &str) -> Result<(), i64> {
    t_putstr("u-7-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    Err(1)
}
