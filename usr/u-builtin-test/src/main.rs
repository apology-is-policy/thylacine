// /u-builtin-test -- U-6e-a shell built-in dispatch + builtins probe.
//
// Runs PRE-pivot (devramfs root). Drives eval_source on a fresh Env per
// probe and asserts the observable contract of each built-in:
//
//   true / false      -- $status 0 / 1.
//   cd                -- validates a real (synthetic) devramfs dir,
//                        updates $cwd (field + the special-var bridge),
//                        normalizes `..`, toggles via `cd -`, fails 1 on
//                        a missing dir without changing cwd.
//   unset             -- removes a binding.
//   eval              -- evaluates a string in the current scope.
//   source            -- reads + evaluates /builtin-test.rc in the
//                        current scope (assignment + fn registration
//                        persist into the caller's Env).
//   type              -- reports a name's kind ($status 0).
//   exit              -- sets the pending-exit request + $status and
//                        unwinds the statement stack (Return), including
//                        out of a called function.
//
// The special-var bridge (scripture 8.5) is checked directly: $status /
// $errstr / $cwd read in-script resolve to their Env fields.
//
// joey gates the boot on this binary's status==0.

#![no_std]
#![no_main]

extern crate alloc;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;
use libutopia::eval::{eval_source, Env, StatementFlow};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // 1. true / false -> $status 0 / 1.
    {
        let mut e = fresh();
        if run(&mut e, "true") != 0 {
            return fail("true status");
        }
        if run(&mut e, "false") != 1 {
            return fail("false status");
        }
    }

    // 2. cd to a real synthetic dir; $cwd reflects it via both the field
    //    accessor and the in-script special-var bridge.
    {
        let mut e = fresh();
        if run(&mut e, "cd /srv") != 0 {
            return fail("cd /srv status");
        }
        if e.cwd() != "/srv" {
            return fail("cd /srv field");
        }
        // `let captured = $cwd` exercises the full eval -> env.get("cwd")
        // -> bridge path (not just the field accessor).
        let _ = eval_source(&mut e, "let captured = $cwd");
        if e.get("captured").as_scalar() != "/srv" {
            return fail("$cwd bridge");
        }
    }

    // 3. cd normalization + root clamp: /srv/../srv/.. -> "/".
    {
        let mut e = fresh();
        if run(&mut e, "cd /srv/../srv/..") != 0 {
            return fail("cd normalize status");
        }
        if e.cwd() != "/" {
            return fail("cd normalize field");
        }
    }

    // 4. cd - toggles between two real dirs (also exercises `cd /`,
    //    which validates without an fstat probe -- the root bypass).
    {
        let mut e = fresh();
        run(&mut e, "cd /srv"); // cwd=/srv oldpwd=/
        run(&mut e, "cd /"); // cwd=/   oldpwd=/srv
        if run(&mut e, "cd -") != 0 {
            // back to /srv
            return fail("cd - status");
        }
        if e.cwd() != "/srv" {
            return fail("cd - toggle");
        }
    }

    // 5. cd to a missing dir -> $status 1, $errstr set, cwd unchanged.
    {
        let mut e = fresh();
        run(&mut e, "cd /srv");
        if run(&mut e, "cd /no-such-dir-xyz") != 1 {
            return fail("cd missing status");
        }
        if e.cwd() != "/srv" {
            return fail("cd missing unchanged");
        }
        if e.get("errstr").as_scalar().is_empty() {
            return fail("$errstr bridge");
        }
    }

    // 6. unset removes a binding.
    {
        let mut e = fresh();
        run(&mut e, "let x = hello");
        if e.get("x").as_scalar() != "hello" {
            return fail("let x");
        }
        run(&mut e, "unset x");
        if !e.get("x").as_scalar().is_empty() {
            return fail("unset x");
        }
    }

    // 7. eval evaluates a string in the current scope. The script is
    //    single-quoted into ONE word (the eval idiom) so the outer parse
    //    hands bi_eval the whole `let y = 42` rather than splitting it.
    {
        let mut e = fresh();
        run(&mut e, "eval 'let y = 42'");
        if e.get("y").as_scalar() != "42" {
            return fail("eval scope");
        }
    }

    // 8. $status special-var bridge: after `false`, $status reads 1.
    {
        let mut e = fresh();
        run(&mut e, "false");
        if e.get("status").as_scalar() != "1" {
            return fail("$status bridge");
        }
    }

    // 9. type reports a name's kind (status 0; output to the UART).
    {
        let mut e = fresh();
        if run(&mut e, "type cd") != 0 {
            return fail("type status");
        }
    }

    // 10. source runs a file in the current scope: the assignment AND the
    //     fn definition in /builtin-test.rc persist into this Env.
    {
        let mut e = fresh();
        if run(&mut e, "source /builtin-test.rc") != 0 {
            return fail("source status");
        }
        if e.get("sourced_var").as_scalar() != "ok" {
            return fail("source var");
        }
        // The sourced fn is now callable.
        if run(&mut e, "sourced_fn") != 0 {
            return fail("source fn");
        }
    }

    // 11. exit sets the pending-exit request + $status; eval_block
    //     short-circuits to Return.
    {
        let mut e = fresh();
        match eval_source(&mut e, "exit 7") {
            Ok(StatementFlow::Return) => {}
            _ => return fail("exit flow"),
        }
        if e.exit_requested() != Some(7) {
            return fail("exit requested");
        }
        if e.status() != 7 {
            return fail("exit status");
        }
    }

    // 12. exit inside a called function unwinds + skips later statements.
    {
        let mut e = fresh();
        let _ = eval_source(&mut e, "fn quitter { exit 3 }\nquitter\nlet after = reached");
        if e.exit_requested() != Some(3) {
            return fail("exit unwind requested");
        }
        if !e.get("after").as_scalar().is_empty() {
            return fail("exit unwind ran-after");
        }
    }

    t_putstr("u-builtin-test: all OK\n");
    0
}

/// A fresh interactive Env (interactive suppresses implicit-fail so a
/// `false` just sets $status rather than propagating).
fn fresh() -> Env {
    let mut e = Env::new();
    e.interactive = true;
    e
}

/// Evaluate one source line and return the resulting $status, or -1 if
/// the eval itself errored.
fn run(env: &mut Env, src: &str) -> i32 {
    match eval_source(env, src) {
        Ok(_) => env.status(),
        Err(_) => -1,
    }
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-builtin-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
