// /u-job-test -- U-7a background-job boot probe.
//
// Runs PRE-pivot (flat devramfs root, where echo/seq/tr all spawn by name).
// Drives the REAL `&` background-launch + reap lifecycle through the public
// libutopia surface (eval_source + Env + the Repl prompt-cycle reaper),
// since the JobTable state is observable WITHOUT a terminal (the `[N] PID`
// + `[N]+ Done` chatter goes to the UART, but the assertions read the table
// directly). Covers, per UTOPIA-SHELL-DESIGN.md section 10.1 + 10.5:
//
//   1. `cmd &` registers a job (spec, cmd, one live pid).
//   2. `a | b &` registers ONE job tracking N pids (bash's model).
//   3. Reap-to-completion emits `[N]+ Done  cmd` + removes the job.
//   3b. Spec resets to 1 once the table drains (bash-like).
//   4. Foreground pid-demux: a fg command does NOT reap a coexisting bg job
//      (the U-7-pre by-pid `wait` is pid-precise) and its $status is correct.
//   5. A bg BUILTIN is rejected (`true &` -- needs fork, like a fg builtin
//      pipeline element).
//   6. The integrated Repl reaper (feed `cmd &` -> reap_jobs drains it).
//
// The harness cannot inject /dev/cons keystrokes (the A-4c constraint), so
// the interactive keystroke->job path is exercised via the fd-agnostic
// `Repl::feed` + the directly-driven reaper here. joey gates the boot on
// this binary's status == 0.
//
// The reap loops are bounded (echo/seq/tr exit in microseconds; the bound is
// a safety net, never the steady state) and yield via time::sleep between
// WNOHANG polls (U-7-pre F1: never hot-loop wait_pid_for).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::time::{self, Duration};
use libthyla_rs::{t_putstr, t_wait_pid_for, T_WAIT_WNOHANG};
use libutopia::eval::{eval_source, Env};
use libutopia::repl::Repl;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

/// One WNOHANG poll of every live bg pid in `env`'s table, feeding reaps
/// back via `mark_reaped`, then drain the `[N]+ Done` lines. Mirrors
/// `Repl::reap_jobs`'s ground-truth half (the probe drives a standalone Env,
/// not a Repl, for the deterministic registration assertions).
fn reap_once(env: &mut Env) -> Vec<String> {
    for pid in env.jobs().live_pids() {
        let mut st: i32 = 0;
        // SAFETY: t_wait_pid_for is the SYS_WAIT_PID SVC wrapper; &mut st is
        // a valid writable i32.
        let rc = unsafe { t_wait_pid_for(pid, T_WAIT_WNOHANG, &mut st as *mut i32) };
        if rc > 0 {
            env.jobs_mut().mark_reaped(pid, st);
        } else if rc < 0 {
            env.jobs_mut().mark_reaped(pid, 0);
        }
    }
    env.jobs_mut().take_done_notifications()
}

/// Reap a standalone Env's jobs to completion, collecting every `[N]+ Done`
/// line. Bounded (safety net) + yields between polls.
fn reap_collect(env: &mut Env) -> Vec<String> {
    let mut all = Vec::new();
    for _ in 0..2000 {
        all.extend(reap_once(env));
        if env.jobs().is_empty() {
            break;
        }
        let _ = time::sleep(Duration::from_millis(2));
    }
    all
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut env = Env::new();
    env.interactive = true;

    // 1. `cmd &` registers a job: spec 1, cmd "echo hi", one live pid,
    //    status 0. eval_source does NOT run a reaper, so the job is
    //    deterministically present right after the launch.
    if eval_source(&mut env, "echo hi &").is_err() {
        return fail("eval `echo hi &` errored");
    }
    if env.status() != 0 {
        return fail("`echo hi &` left non-zero status");
    }
    if env.jobs().len() != 1 {
        return fail("`echo hi &` did not register exactly one job");
    }
    {
        let job = env.jobs().iter().next().unwrap();
        if job.spec() != 1 {
            return fail("first bg job did not get spec [1]");
        }
        if job.cmd() != "echo hi" {
            return fail("bg job cmd not rendered as \"echo hi\"");
        }
    }
    if env.jobs().live_pids().len() != 1 {
        return fail("single-command bg job should track exactly one pid");
    }

    // 2. `a | b &` registers ONE job tracking N pids (a bg pipeline is one
    //    job). The table now holds two jobs; the new one has 2 live pids.
    if eval_source(&mut env, "echo hi | tr a-z A-Z &").is_err() {
        return fail("eval `echo hi | tr a-z A-Z &` errored");
    }
    if env.jobs().len() != 2 {
        return fail("bg pipeline did not register as one additional job");
    }
    {
        // Jobs iterate in registration order; the 2nd is the pipeline.
        let job2 = env.jobs().iter().nth(1).unwrap();
        if job2.spec() != 2 {
            return fail("second bg job did not get spec [2]");
        }
        if job2.cmd() != "echo hi | tr a-z A-Z" {
            return fail("bg pipeline cmd not rendered with ` | ` join");
        }
    }
    // Two jobs -> three live pids total (1 + 2).
    if env.jobs().live_pids().len() != 3 {
        return fail("bg pipeline should add two pids to the live set");
    }

    // 3. Reap both jobs to completion. Each emits exactly one `[N]+ Done`
    //    line carrying its rendered command, and the table drains.
    let done = reap_collect(&mut env);
    if !env.jobs().is_empty() {
        return fail("jobs did not drain to empty after reap-to-completion");
    }
    if done.len() != 2 {
        return fail("expected exactly two `[N]+ Done` notifications");
    }
    if !done.iter().any(|l| l.contains("Done") && l.contains("echo hi") && !l.contains('|')) {
        return fail("no Done line for the single `echo hi` job");
    }
    if !done.iter().any(|l| l.contains("Done") && l.contains("echo hi | tr a-z A-Z")) {
        return fail("no Done line for the `echo hi | tr a-z A-Z` pipeline job");
    }

    // 3b. Spec resets to [1] now that the table is empty (bash-like).
    if eval_source(&mut env, "echo hi &").is_err() {
        return fail("eval third `echo hi &` errored");
    }
    if env.jobs().iter().next().unwrap().spec() != 1 {
        return fail("spec did not reset to [1] after the table drained");
    }
    let _ = reap_collect(&mut env); // tidy up

    // 4. Foreground pid-demux. Launch a bg job, then run a FOREGROUND
    //    external command: the fg by-pid `wait` must reap ITS pid only,
    //    leaving the bg job tracked, and its $status must be correct.
    let mut env2 = Env::new();
    env2.interactive = true;
    if eval_source(&mut env2, "echo hi &").is_err() {
        return fail("demux: eval `echo hi &` errored");
    }
    let live_before = env2.jobs().live_pids().len();
    if live_before != 1 {
        return fail("demux: bg job not registered before the fg command");
    }
    // `seq` with no args is an EXTERNAL command that exits 1 (usage error to
    // stderr) -- a deterministic non-zero foreground status.
    if eval_source(&mut env2, "seq").is_err() {
        return fail("demux: eval foreground `seq` errored");
    }
    if env2.status() != 1 {
        return fail("demux: foreground `seq` status corrupted (expected 1)");
    }
    if env2.jobs().len() != 1 || env2.jobs().live_pids().len() != live_before {
        return fail("demux: foreground wait wrongly reaped the bg job");
    }
    let _ = reap_collect(&mut env2); // tidy up

    // 5. A bg BUILTIN is rejected: `true` is a shell builtin, which (like a
    //    builtin foreground pipeline element) has no spawned fd and needs
    //    fork -> NotImplemented. Must NOT register a job.
    let mut env3 = Env::new();
    env3.interactive = true;
    if eval_source(&mut env3, "true &").is_ok() {
        return fail("`true &` (bg builtin) unexpectedly succeeded");
    }
    if !env3.jobs().is_empty() {
        return fail("rejected bg builtin should register no job");
    }

    // 6. The integrated Repl reaper: feed `cmd &` through the fd-agnostic
    //    Repl (the real interactive path), then drive Repl::reap_jobs to
    //    completion. Verifies feed-registration + the integrated reaper.
    let mut repl = Repl::new();
    let mut sink: Vec<u8> = Vec::new();
    if repl.feed(b"echo hi &\n", &mut sink).is_some() {
        return fail("Repl::feed of `echo hi &` unexpectedly ended the session");
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
        return fail("Repl::reap_jobs did not drain the bg job");
    }

    t_putstr("u-job-test: all OK\n");
    0
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-job-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
