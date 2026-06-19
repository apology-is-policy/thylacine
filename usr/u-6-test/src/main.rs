// /u-6-test -- the U-6 evaluator arc CUMULATIVE integration smoke.
//
// Where the per-sub-chunk probes prove each U-6 surface in ISOLATION --
// u-builtin-test (U-6e-a), u-glob-test (U-6e-b), u-subst-test (U-6f),
// u-repl-test (U-6g), and u-test's flow_eval_* blocks (U-6a..U-6d-b) --
// this probe proves they COMPOSE: realistic multi-feature scripts in one
// evaluation pass, weaving expr eval + control flow + external spawn +
// pipelines + redirection + builtins + glob + command substitution, and
// (the capstone) the whole stack driven through the real read-parse-eval
// loop via libutopia::repl::Repl::feed.
//
// Runs PRE-pivot (flat devramfs root) where echo/seq/tr spawn by name and
// the u-*/hello-rs/pipe-* fixtures + the /srv synthetic dir all resolve.
// At v1.0 a spawned command's stdout goes to a dropped pipe (no terminal-
// backed fd 1 until U-PTY), so the only observables are $status, Env
// state, and -- the key integration lever -- command substitution, which
// CAPTURES a real spawned/piped/globbed command's stdout into a variable
// we can then assert on. Every flow lands there.
//
// Flows (each composes >=3 U-6 features):
//   1. subst-capture -> for -> case -> arith   (classify + count files)
//   2. glob -> spawn(echo) -> subst-capture     (enumerate the u-* binaries)
//   3. var-interp -> pipeline(echo|tr) -> subst (transform + field-split)
//   4. cd builtin -> $status/$cwd -> if -> Dq   (act, branch, report)
//   5. var -> heredoc-interp -> redirect -> spawn (var-fed heredoc to a sink)
//   6. Repl::feed end-to-end                    (the loop: var/subst/pipe/Dq)
//   7. Repl::feed parse-error survival + subst  (interactive recovery, 8.9)
//
// joey gates the boot on this binary's status==0.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;
use libutopia::eval::{eval_source, Env};
use libutopia::repl::Repl;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    if let Err(rc) = flow_subst_for_case() {
        return rc;
    }
    if let Err(rc) = flow_glob_spawn_capture() {
        return rc;
    }
    if let Err(rc) = flow_pipeline_capture() {
        return rc;
    }
    if let Err(rc) = flow_builtin_branch() {
        return rc;
    }
    if let Err(rc) = flow_tilde_expand() {
        return rc;
    }
    if let Err(rc) = flow_heredoc_var_redirect() {
        return rc;
    }
    if let Err(rc) = flow_repl_session() {
        return rc;
    }
    if let Err(rc) = flow_repl_recovery() {
        return rc;
    }
    if let Err(rc) = flow_script_mode() {
        return rc;
    }

    t_putstr("u-6-test: all OK\n");
    0
}

// Flow 1 -- the canonical "classify a list of filenames" idiom: a command
// substitution captures + splits echo's output into a 4-element list; a
// `for` iterates it; a `case` matches the *.c arm; arith counts the
// matches. Proves subst-capture + for + case + arith + re-`let` all nest
// and share scope (each is proven alone elsewhere; here they compose).
fn flow_subst_for_case() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;

    let src = "let files = $(echo a.c b.rs c.c d.h)\n\
               let ccount = 0\n\
               for (f in $files) { case $f { *.c => { let ccount = (( $ccount + 1 )) } } }";
    if eval_source(&mut env, src).is_err() {
        return fail("flow 1: classify-and-count script errored");
    }
    let files = env.get("files").0;
    if files.len() != 4 || files[0] != "a.c" || files[3] != "d.h" {
        return fail("flow 1: $(echo ...) did not capture+split into 4 files");
    }
    if env.get("ccount").as_scalar() != "2" {
        return fail("flow 1: for/case/arith did not count 2 *.c files");
    }
    t_putstr("u-6-test: subst + for + case + arith OK\n");
    Ok(())
}

// Flow 2 -- glob composes with spawn + capture: `u-*` expands in echo's
// argv (the evaluate_argv fs-walk), echo prints the matches, the
// substitution captures + splits them into a list. Proves the full
// glob -> argv -> external spawn -> stdout -> capture -> field-split chain
// (each half proven alone in u-glob-test + u-subst-test; never together).
fn flow_glob_spawn_capture() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;

    if eval_source(&mut env, "let bins = $(echo u-*)").is_err() {
        return fail("flow 2: $(echo u-*) errored");
    }
    let bins = env.get("bins").0;
    // The flat root holds the whole u-*-test family; assert on peers that
    // are unconditionally packed, plus this binary itself (the nice
    // self-reference: u-6-test is in the same ramfs the glob walks).
    if !list_has(&bins, "u-glob-test") || !list_has(&bins, "u-subst-test") {
        return fail("flow 2: glob-via-echo missing a known u-* peer");
    }
    if !list_has(&bins, "u-6-test") {
        return fail("flow 2: glob-via-echo missing self (u-6-test)");
    }
    if !bins.iter().all(|s| s.as_bytes().starts_with(b"u-")) {
        return fail("flow 2: glob-via-echo yielded a non-u- name");
    }
    t_putstr("u-6-test: glob + spawn + subst capture OK\n");
    Ok(())
}

// Flow 3 -- a substitution whose body is a PIPELINE with a variable
// interpolated into the upstream argv: `$(echo hello $phrase | tr a-z
// A-Z)`. Proves var-interp (in a spawned argv, inside a substitution) +
// 2-stage pipeline (two distinct externals) + capture + multi-word split
// all compose into one captured list.
fn flow_pipeline_capture() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;

    let src = "let phrase = world\n\
               let shout = $(echo hello $phrase | tr a-z A-Z)";
    if eval_source(&mut env, src).is_err() {
        return fail("flow 3: pipeline-substitution script errored");
    }
    let shout = env.get("shout").0;
    if shout.len() != 2 || shout[0] != "HELLO" || shout[1] != "WORLD" {
        return fail("flow 3: echo|tr capture did not split into (HELLO WORLD)");
    }
    t_putstr("u-6-test: var + pipeline + subst capture OK\n");
    Ok(())
}

// Flow 4 -- a builtin's side effect drives control flow through the
// special-var bridge: `cd /srv` sets $status + $cwd; an `if ($status ==
// 0)` branches on the bridge; the taken arm builds a message via "...$cwd"
// double-quote interpolation. Proves builtin + $status + if + $cwd + Dq
// interp compose (the bridge is read in both Cond and Dq contexts).
fn flow_builtin_branch() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;

    let src = "cd /srv\n\
               if ($status == 0) { let loc = \"now at $cwd\" } else { let loc = 'cd failed' }";
    if eval_source(&mut env, src).is_err() {
        return fail("flow 4: cd-and-report script errored");
    }
    if env.cwd() != "/srv" {
        return fail("flow 4: cd /srv did not update cwd");
    }
    if env.get("loc").as_scalar() != "now at /srv" {
        return fail("flow 4: $status branch / $cwd Dq-interp did not compose");
    }
    t_putstr("u-6-test: builtin + special-vars + if + Dq OK\n");
    Ok(())
}

// Flow tilde -- `~` expansion in command-word evaluation. A *leading* `~`
// (`~`, `~/path`) in a spawned command's argv expands to $home (an ordinary
// var, the same store login's `--home` forward writes via set_home); a
// mid-word `~` (`a~b`) stays literal. The expansion happens in ut's eval
// BEFORE the spawn, so `$(echo ~/foo)` captures the already-expanded path
// (stdout is dropped pre-PTY, so the substitution capture is the observable).
fn flow_tilde_expand() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;

    let src = "let home = /home/test\n\
               let sub = docs\n\
               let a = $(echo ~)\n\
               let b = $(echo ~/foo)\n\
               let c = $(echo a~b)\n\
               let d = $(echo ~user)\n\
               let e = $(echo ~/$sub)";
    if eval_source(&mut env, src).is_err() {
        return fail("flow tilde: tilde-expansion script errored");
    }
    if env.get("a").as_scalar() != "/home/test" {
        return fail("flow tilde: bare ~ did not expand to $home");
    }
    if env.get("b").as_scalar() != "/home/test/foo" {
        return fail("flow tilde: ~/foo did not expand to $home/foo");
    }
    if env.get("c").as_scalar() != "a~b" {
        return fail("flow tilde: mid-word a~b did not stay literal");
    }
    if env.get("d").as_scalar() != "~user" {
        return fail("flow tilde: ~user (no name service) did not stay literal");
    }
    if env.get("e").as_scalar() != "/home/test/docs" {
        return fail("flow tilde: ~/$sub did not expand + join the var suffix");
    }
    t_putstr("u-6-test: tilde (~) command-word expansion OK\n");
    Ok(())
}

// Flow 8 (D2) -- `ut SCRIPT` non-interactive execution: Repl::run_script binds
// the positional parameters (the names "0"/"1"/"2"/"*") at the script's global
// scope, switches to script mode (fail-fast, scripture 8.9), and returns the
// exit code (an explicit `exit N` wins). The positionals are bound for parity
// with invoke_function; the `$N` / `$*` *reference* syntax is a separate Utopia
// language item (the lexer's var-name-start is [a-zA-Z_], so a digit cannot
// start a `$`-reference today -- functions use named args, not `$1`), tracked
// as a follow-up. The shebang FS-peek half is covered by host unit tests
// (parse_shebang_line) + the interactive `./script.ut` E2E; here we prove the
// script-mode evaluator path runs in QEMU.
fn flow_script_mode() -> Result<(), i64> {
    let mut repl = Repl::new();
    let code = repl.run_script(
        "/s.ut",
        &[String::from("foo"), String::from("bar")],
        "exit 9\n",
    );
    if code != 9 {
        return fail("flow 8: run_script did not propagate `exit 9`");
    }
    if repl.env().interactive {
        return fail("flow 8: run_script did not switch to script mode");
    }
    if repl.env().get("0").as_scalar() != "/s.ut"
        || repl.env().get("1").as_scalar() != "foo"
        || repl.env().get("2").as_scalar() != "bar"
    {
        return fail("flow 8: run_script did not bind $0/$1/$2 positionals");
    }
    // A script that runs statements + completes with status 0 returns 0.
    let mut r2 = Repl::new();
    let rc = r2.run_script("/s.ut", &[], "let a = 1\nlet b = 2\n");
    if rc != 0 || r2.env().get("b").as_scalar() != "2" {
        return fail("flow 8: a clean script did not evaluate + return 0");
    }
    // run_script syncs $cwd to the real kernel cwd (so a `#!`-spawned script,
    // which gets no --home, reports the inherited cwd, not the unset `/`).
    if let Ok(cwd) = libthyla_rs::env::current_dir() {
        if r2.env().cwd() != cwd {
            return fail("flow 8: run_script did not sync $cwd to the kernel cwd");
        }
    }
    t_putstr("u-6-test: ut SCRIPT mode (positional + script-mode + exit) OK\n");
    Ok(())
}

// Flow 5 -- a variable defined earlier in the SAME script feeds an
// interpolated heredoc body that is delivered, via a `<<` redirect, to a
// real spawned child's fd 0. pipe-sink exits 0 iff it reads exactly the
// 13-byte "PIPE-DATA-OK\n"; with mid="DATA" the rendered body is that
// payload. Proves var + heredoc-interp + redirect + external spawn
// compose end-to-end (resolve -> pipe -> spawn -> feed body -> child read).
fn flow_heredoc_var_redirect() -> Result<(), i64> {
    let mut env = Env::new();
    env.interactive = true;

    let src = "let mid = DATA\n\
               pipe-sink << EOF\n\
               PIPE-$mid-OK\n\
               EOF\n";
    if eval_source(&mut env, src).is_err() {
        return fail("flow 5: var-fed heredoc script errored");
    }
    if env.status() != 0 {
        return fail("flow 5: heredoc payload did not reach pipe-sink intact");
    }
    t_putstr("u-6-test: var + heredoc + redirect + spawn OK\n");
    Ok(())
}

// Flow 6 -- the capstone: a realistic multi-statement session driven
// through the actual read-parse-eval loop (Repl::feed), not eval_source
// directly. Each line is Accepted by the line editor, parsed, and
// evaluated; the substitution spawns a real echo|tr pipeline and captures
// it; a later line interpolates the captured value into a double-quoted
// word; `exit 0` terminates the session. Proves the WHOLE U-6 stack runs
// through the U-6g entrypoint (editor -> parser -> evaluator -> subst/pipe
// -> Dq), with state carried across feeds.
fn flow_repl_session() -> Result<(), i64> {
    let mut repl = Repl::new();
    let mut sink: alloc::vec::Vec<u8> = alloc::vec::Vec::new();

    if repl.feed(b"let name = thylacine\n", &mut sink).is_some() {
        return fail("flow 6: the first line unexpectedly ended the session");
    }
    if repl.feed(b"let up = $(echo $name | tr a-z A-Z)\n", &mut sink).is_some() {
        return fail("flow 6: the subst/pipe line unexpectedly ended the session");
    }
    if repl.feed(b"let tag = \"[$up]\"\n", &mut sink).is_some() {
        return fail("flow 6: the Dq-interp line unexpectedly ended the session");
    }
    if repl.env().get("name").as_scalar() != "thylacine" {
        return fail("flow 6: name not assigned through the REPL");
    }
    if repl.env().get("up").as_scalar() != "THYLACINE" {
        return fail("flow 6: echo|tr capture not carried through the REPL");
    }
    if repl.env().get("tag").as_scalar() != "[THYLACINE]" {
        return fail("flow 6: Dq interp of the captured value did not compose");
    }
    // Session termination composes: `exit 0` returns Some(0) from feed.
    match repl.feed(b"exit 0\n", &mut sink) {
        Some(0) => {}
        Some(_) => return fail("flow 6: exit returned the wrong code"),
        None => return fail("flow 6: exit did not end the session"),
    }
    t_putstr("u-6-test: REPL session (var + subst + pipe + Dq + exit) OK\n");
    Ok(())
}

// Flow 7 -- interactive resilience composes with real evaluation: a parse
// error at the prompt draws a diagnostic but does NOT end the session
// (scripture 8.9), and a subsequent line that uses the full substitution
// pipeline still evaluates. Proves error survival + subst-capture compose
// (u-repl-test proves bare recovery; here the recovery line does real work).
fn flow_repl_recovery() -> Result<(), i64> {
    let mut repl = Repl::new();
    let mut sink: alloc::vec::Vec<u8> = alloc::vec::Vec::new();

    if repl.feed(b")\n", &mut sink).is_some() {
        return fail("flow 7: a parse error unexpectedly ended the session");
    }
    if repl.feed(b"let ok = $(echo recovered)\n", &mut sink).is_some() {
        return fail("flow 7: the post-error subst line unexpectedly ended the session");
    }
    if repl.env().get("ok").as_scalar() != "recovered" {
        return fail("flow 7: substitution did not work after a parse error");
    }
    t_putstr("u-6-test: REPL error-survival + subst OK\n");
    Ok(())
}

fn list_has(v: &[String], want: &str) -> bool {
    v.iter().any(|s| s == want)
}

fn fail(tag: &str) -> Result<(), i64> {
    t_putstr("u-6-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    Err(1)
}
