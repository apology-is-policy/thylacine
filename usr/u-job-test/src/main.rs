// /u-job-test -- U-7a/U-7b job-control boot probe.
//
// Runs PRE-pivot (flat devramfs root, where echo/seq/tr all spawn by name).
// Drives the REAL `&` background-launch + reap lifecycle + the U-7b job-
// control builtins through the public libutopia surface (eval_source + Env +
// the Repl prompt-cycle reaper), since the JobTable state is observable
// WITHOUT a terminal (the `[N] PID` + `[N]+ Done` chatter goes to the UART,
// but the assertions read the table directly). Covers, per
// UTOPIA-SHELL-DESIGN.md section 10.1 + 10.5 + 10.6:
//
//   U-7a:
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
//   U-7b:
//   7. `wait %N` blocks + reaps a specific job; a following `jobs` lists it
//      Done and consumes it (exactly-once reporting).
//   8. `wait` (no args) blocks on ALL jobs.
//   9. `fg %N` blocks to completion + removes the job silently (no Done line).
//   10. `bg %N` reports the already-running job (status 0), no removal.
//   11. `kill` error paths (usage / no-such-job / snare:* reserved).
//   12. `kill %N` posts a note to a LIVE child's pid (the %N -> pid mapping).
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
use alloc::vec;
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::process::{Command, Stdio};
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

    // === U-7b: job-control builtins (jobs / fg / bg / wait / kill) ===

    // 7. `wait %N` blocks on a specific job + reaps it (live_pids drains) and
    //    leaves it complete-but-not-removed; a following `jobs` lists it Done
    //    and CONSUMES it (so the prompt cycle would not re-announce -- exactly
    //    once, whether observed by `jobs` or the prompt).
    let mut env_w = Env::new();
    env_w.interactive = true;
    if eval_source(&mut env_w, "echo hi &").is_err() {
        return fail("wait: `echo hi &` errored");
    }
    if eval_source(&mut env_w, "wait %1").is_err() {
        return fail("wait: `wait %1` errored");
    }
    if env_w.status() != 0 {
        return fail("wait %1 left non-zero status");
    }
    if !env_w.jobs().live_pids().is_empty() {
        return fail("wait %1 did not reap the job's pid");
    }
    if env_w.jobs().len() != 1 {
        return fail("wait %1 must leave the completed job for the prompt-cycle drain");
    }
    if eval_source(&mut env_w, "jobs").is_err() {
        return fail("wait: `jobs` errored");
    }
    if !env_w.jobs().is_empty() {
        return fail("`jobs` did not consume the completed job");
    }

    // 8. `wait` (no args) blocks on ALL background jobs; both reaped, status 0.
    let mut env_wa = Env::new();
    env_wa.interactive = true;
    let _ = eval_source(&mut env_wa, "echo a &");
    let _ = eval_source(&mut env_wa, "echo b &");
    if env_wa.jobs().live_pids().len() != 2 {
        return fail("wait-all: two bg jobs not registered");
    }
    if eval_source(&mut env_wa, "wait").is_err() {
        return fail("wait-all: `wait` errored");
    }
    if env_wa.status() != 0 || !env_wa.jobs().live_pids().is_empty() {
        return fail("wait-all did not reap every job");
    }
    let _ = reap_collect(&mut env_wa); // consume the Done entries (no zombies)

    // 9. `fg %N` blocks on the job to completion AND removes it (NO Done line):
    //    the job is consumed as the foreground command. $status is its exit.
    let mut env_fg = Env::new();
    env_fg.interactive = true;
    if eval_source(&mut env_fg, "echo hi &").is_err() {
        return fail("fg: `echo hi &` errored");
    }
    if eval_source(&mut env_fg, "fg %1").is_err() {
        return fail("fg: `fg %1` errored");
    }
    if env_fg.status() != 0 {
        return fail("fg %1 left non-zero status");
    }
    if !env_fg.jobs().is_empty() {
        return fail("fg %1 did not remove the foregrounded job (no Done line owed)");
    }

    // 10. `bg %N` reports the already-running job (status 0) and does NOT
    //     remove it (v1.0 has no stopped jobs to resume). Its bg child must
    //     then be reaped -- else it orphans to joey -- so `wait %1` follows.
    let mut env_bg = Env::new();
    env_bg.interactive = true;
    if eval_source(&mut env_bg, "echo hi &").is_err() {
        return fail("bg: `echo hi &` errored");
    }
    if eval_source(&mut env_bg, "bg %1").is_err() {
        return fail("bg: `bg %1` errored");
    }
    if env_bg.status() != 0 {
        return fail("bg %1 should report status 0");
    }
    if env_bg.jobs().len() != 1 {
        return fail("bg %1 must not remove the job");
    }
    let _ = eval_source(&mut env_bg, "wait %1"); // reap the bg child
    let _ = reap_collect(&mut env_bg);

    // 11. `kill` error paths -- deterministic, no live child:
    //     bad usage, no-such-job, and the snare:* reservation.
    let mut env_ke = Env::new();
    env_ke.interactive = true;
    let _ = eval_source(&mut env_ke, "kill");
    if env_ke.status() == 0 {
        return fail("`kill` with no args should fail");
    }
    let _ = eval_source(&mut env_ke, "kill %99");
    if env_ke.status() == 0 {
        return fail("`kill %99` (no such job) should fail");
    }
    let _ = eval_source(&mut env_ke, "kill 1 snare:term");
    if env_ke.status() == 0 {
        return fail("`kill ... snare:term` should be rejected (kernel-reserved)");
    }

    // 12. `kill %N` SUCCESS to a LIVE child. Spawn `tr` with a piped stdin we
    //     hold open -> it blocks reading (cannot exit) -> register it as job
    //     [1] -> `kill %1` posts `interrupt` to its pid (send must succeed) ->
    //     EOF its stdin so it exits regardless of the note -> reap by-pid
    //     (bounded; never hangs). Proves the %N -> live-pid mapping + send.
    // All three slots are Piped (the shell's v1.0 no-terminal convention --
    // Stdio::Inherit cannot resolve without a terminal-backed fd 0/1/2). We
    // HOLD stdin's write end so `tr` blocks reading (stays alive) and drop the
    // stdout/stderr parent ends.
    let mut env_k = Env::new();
    env_k.interactive = true;
    match Command::new("tr")
        .arg("a-z")
        .arg("A-Z")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(mut child) => {
            drop(child.stdout.take());
            drop(child.stderr.take());
            let pid = child.pid();
            env_k.jobs_mut().add(vec![pid], String::from("tr a-z A-Z"));
            if eval_source(&mut env_k, "kill %1").is_err() {
                return fail("kill: `kill %1` errored");
            }
            if env_k.status() != 0 {
                return fail("kill %1 to a live child should report success");
            }
            // EOF the child's stdin so it exits even if it ignored the note,
            // then reap by-pid (bounded -- never hangs).
            drop(child.stdin.take());
            let _ = child.wait();
        }
        Err(_) => return fail("kill: could not spawn the `tr` blocker"),
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
