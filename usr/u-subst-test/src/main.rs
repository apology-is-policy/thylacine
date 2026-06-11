// /u-subst-test -- U-6f command-substitution boot probe.
//
// Runs PRE-pivot (flat devramfs root, where echo/seq/tr/true/false all
// spawn by name). Drives command substitution through the public
// libutopia::eval surface (eval_source + Env), since the in-process
// CAPTURE is observable WITHOUT a terminal (unlike argv echo, which goes
// to a dropped pipe at v1.0). Covers, per scripture 6.6 + 6.5 + 8.7:
//
//   1. Capture + trailing-newline trim       -- $(echo hi) -> "hi"
//   2. Bare-context whitespace field-split    -- $(echo a b c) -> (a b c)
//   3. Newline-separated output also splits    -- $(seq 1 3)  -> (1 2 3)
//   4. Inside "..." inserted verbatim (no split) -- "[$(echo hi)]" -> "[hi]"
//   5. Pipeline-body capture                   -- $(echo hello | tr a-z A-Z)
//   6. rc-traditional backtick form            -- `{echo hi} -> "hi"
//   7. $status carries the inner exit (8.7)    -- $(seq)->1, $(echo)->0
//   7c. A builtin in a substitution is NotImplemented -- $(true)
//   8. Process substitution stays NotImplemented -- <(echo hi)
//   9. Script-mode implicit-fail propagation (8.7) -- let q = $(seq)
//
// joey gates the boot on this binary's status==0.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;
use libutopia::eval::{eval_source, Env, StatementFlow};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Interactive mode: a non-zero $status does NOT early-return, so every
    // sequential check below runs (scripture 8.7 propagation is verified
    // separately with a fresh script-mode env in step 9).
    let mut env = Env::new();
    env.interactive = true;

    // 1. Capture + trailing-newline trim. echo writes "hi\n" to fd 1 (the
    //    capture pipe); the trim drops the newline. A successful echo also
    //    leaves $status == 0.
    if eval_source(&mut env, "let x = $(echo hi)").is_err() {
        return fail("eval `let x = $(echo hi)` errored");
    }
    if env.get("x").as_scalar() != "hi" {
        return fail("$(echo hi) did not capture \"hi\"");
    }
    if env.status() != 0 {
        return fail("$(echo hi) left non-zero status");
    }

    // 2. Bare-word context whitespace-splits the result into a list.
    if eval_source(&mut env, "let xs = $(echo a b c)").is_err() {
        return fail("eval `let xs = $(echo a b c)` errored");
    }
    let xs = env.get("xs").0;
    if xs.len() != 3 || xs[0] != "a" || xs[1] != "b" || xs[2] != "c" {
        return fail("$(echo a b c) did not split into (a b c)");
    }

    // 3. Newline-separated output splits the same way (seq prints one per line).
    if eval_source(&mut env, "let ns = $(seq 1 3)").is_err() {
        return fail("eval `let ns = $(seq 1 3)` errored");
    }
    let ns = env.get("ns").0;
    if ns.len() != 3 || ns[0] != "1" || ns[1] != "2" || ns[2] != "3" {
        return fail("$(seq 1 3) did not split into (1 2 3)");
    }

    // 4. Inside "..." a substitution is inserted verbatim -- one scalar word.
    if eval_source(&mut env, "let d = \"[$(echo hi)]\"").is_err() {
        return fail("eval quoted-substitution errored");
    }
    let d = env.get("d");
    if d.0.len() != 1 || d.as_scalar() != "[hi]" {
        return fail("\"[$(echo hi)]\" did not inline to \"[hi]\"");
    }

    // 5. A pipeline body: the LAST element's stdout is captured.
    if eval_source(&mut env, "let p = $(echo hello | tr a-z A-Z)").is_err() {
        return fail("eval pipeline-substitution errored");
    }
    if env.get("p").as_scalar() != "HELLO" {
        return fail("$(echo .. | tr ..) pipeline capture wrong");
    }

    // 6. rc-traditional backtick form `{cmd}` (closing backtick required by
    //    the lexer), identical capture semantics to $(cmd).
    if eval_source(&mut env, "let b = `{echo hi}`").is_err() {
        return fail("eval backtick-substitution errored");
    }
    if env.get("b").as_scalar() != "hi" {
        return fail("`{echo hi} did not capture \"hi\"");
    }

    // 7. $status carries the inner command's exit (scripture 8.7). We use
    //    EXTERNAL commands: `seq` with no args prints a usage error to
    //    STDERR and exits 1 (captured stdout is empty); `echo` with no args
    //    prints just a newline and exits 0. (`true`/`false` are shell
    //    BUILTINS -- see 7c -- not external, so they can't be captured.)
    if eval_source(&mut env, "let f = $(seq)").is_err() {
        return fail("eval `let f = $(seq)` errored");
    }
    if env.status() != 1 {
        return fail("$(seq) [usage error] did not set status 1");
    }
    if !env.get("f").as_scalar().is_empty() {
        return fail("$(seq) captured non-empty stdout (usage goes to stderr)");
    }
    if eval_source(&mut env, "let t = $(echo)").is_err() {
        return fail("eval `let t = $(echo)` errored");
    }
    if env.status() != 0 {
        return fail("$(echo) did not set status 0");
    }

    // 7c. A BUILTIN inside a substitution is NotImplemented at v1.0 (an
    //     in-process command has no spawned stdout to capture without fork;
    //     the documented limitation). `true` is a builtin -> eval errors.
    if eval_source(&mut env, "let u = $(true)").is_ok() {
        return fail("$(builtin) unexpectedly succeeded (should be NotImplemented)");
    }

    // 8. Process substitution is NotImplemented (needs /proc/self/fd/N); it
    //    must NOT succeed (parse-error or NotImplemented are both fine).
    if eval_source(&mut env, "let z = <(echo hi)").is_ok() {
        return fail("<(cmd) process substitution unexpectedly succeeded");
    }

    // 9. Scripture 8.7 implicit-fail: in SCRIPT mode (interactive=false), a
    //    failed substitution in an assignment propagates -- the block
    //    returns, and $status is the inner exit.
    let mut script_env = Env::new();
    match eval_source(&mut script_env, "let q = $(seq)") {
        Ok(StatementFlow::Return) => {}
        Ok(_) => return fail("failed substitution did not propagate in script mode"),
        Err(_) => return fail("script-mode substitution eval errored"),
    }
    if script_env.status() != 1 {
        return fail("propagated substitution left wrong status");
    }

    // 10. RW-9 R2-F1 regression: a command substitution whose NON-FINAL
    //     element fails to spawn must NOT hang. Pre-fix the capture write end
    //     was orphaned in the parent (the never-spawned last element never
    //     took it), so read_to_end blocked forever -- an un-interruptible
    //     shell hang. The fix drops the un-consumed pipe ends before draining.
    //     Reaching the assertions at all is the regression (pre-fix the boot
    //     hangs here); we also pin the empty capture + 127 status.
    if eval_source(&mut env, "let h = $(no-such-binary-xyz123 | echo done)").is_err() {
        return fail("non-final-spawn-fail substitution errored unexpectedly");
    }
    if !env.get("h").as_scalar().is_empty() {
        return fail("non-final-spawn-fail substitution captured non-empty output");
    }
    if env.status() != 127 {
        return fail("non-final-spawn-fail substitution did not set status 127");
    }

    // 11. RW-9 R2-F2 regression: unbounded eval recursion (a self-calling
    //     function) must yield a graceful error, NOT a 256 KiB EL0 stack
    //     overflow -> snare:segv -> shell death. Pre-fix this crashed the
    //     Proc; post-fix eval hits EVAL_MAX_DEPTH and returns Err.
    let mut rec_env = Env::new();
    if eval_source(&mut rec_env, "fn rec_probe { rec_probe }\nrec_probe").is_ok() {
        return fail("self-recursive function did not hit the eval recursion bound");
    }

    // 12. RW-9 R1-F1/SA-1 regression: deeply nested parser input must yield a
    //     graceful error, NOT a parse-time stack overflow. check_token_nesting
    //     trips at PARSE_MAX_NESTING; 300 nested levels would overflow the EL0
    //     stack pre-fix (the recursive-descent had no depth cap).
    let mut deep = String::new();
    let mut depth_i = 0;
    while depth_i < 300 {
        deep.push('(');
        depth_i += 1;
    }
    let mut deep_env = Env::new();
    if eval_source(&mut deep_env, &deep).is_ok() {
        return fail("deeply nested parser input did not hit the nesting bound");
    }

    t_putstr("u-subst-test: all OK\n");
    0
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-subst-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
