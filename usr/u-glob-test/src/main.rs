// /u-glob-test -- U-6e-b-2 glob-argv-expansion boot probe.
//
// Runs PRE-pivot (flat devramfs root). Two layers:
//
//   A. The load-bearing fs-walk -- libutopia::eval::glob::expand directly
//      against the boot ramfs. argv echoes to a dropped pipe at v1.0
//      (no terminal-backed fd 1 until U-PTY), so the expansion itself is
//      asserted here on the returned Vec rather than via command output:
//        - prefix star (`u-*`), bare star (`*`), single-char (`?`),
//          char-class (`[vw]*`), absolute (`/u-*`);
//        - sortedness, single-level containment (no `/`, no dotfile leak),
//          rc nullglob (no-match -> EMPTY list).
//
//   B. The evaluate_argv wiring -- observed via $status (the one signal
//      that survives the dropped-pipe stdio): a BARE glob matching nothing
//      nullglobs to an empty command -> status 0; the SAME pattern QUOTED
//      is taken literally and spawned (NotFound) -> status 127. The delta
//      proves the bare branch globbed and the quoted branch did not.
//
// joey gates the boot on this binary's status==0.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;
use libutopia::eval::{eval_source, glob, Env};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // cwd defaults to "/" -- relative patterns resolve against the root.
    let env = Env::new();

    // A1. Prefix star: `u-*` names the u-prefixed binaries on the flat root.
    let u = glob::expand(&env, "u-*");
    if !contains(&u, "u-glob-test") {
        return fail("u-* missing self");
    }
    if !contains(&u, "u-readdir-test") {
        return fail("u-* missing u-readdir-test");
    }
    if !u.iter().all(|s| s.as_bytes().starts_with(b"u-")) {
        return fail("u-* yielded a non-u- name");
    }
    if !is_sorted(&u) {
        return fail("u-* not sorted");
    }

    // A2. Bare star: enumerates the whole flat root. Single-level (no `/`),
    //     no leading-dot leak, sorted, plausibly many entries.
    let all = glob::expand(&env, "*");
    if all.len() < 10 {
        return fail("* count implausibly low");
    }
    if !is_sorted(&all) {
        return fail("* not sorted");
    }
    if all.iter().any(|s| s.as_bytes().first() == Some(&b'.')) {
        return fail("* leaked a dotfile");
    }
    if all.iter().any(|s| s.as_bytes().iter().any(|&b| b == b'/')) {
        return fail("* crossed a slash on a flat dir");
    }
    if !contains(&all, "version") {
        return fail("* missing version");
    }
    if !contains(&all, "srv") {
        return fail("* missing srv");
    }

    // A3. Single-char wildcard: `versio?` -> version.
    let q = glob::expand(&env, "versio?");
    if !contains(&q, "version") {
        return fail("versio? missing version");
    }

    // A4. Char class: `[vw]*` matches both version and welcome.
    let cc = glob::expand(&env, "[vw]*");
    if !contains(&cc, "version") {
        return fail("[vw]* missing version");
    }
    if !contains(&cc, "welcome") {
        return fail("[vw]* missing welcome");
    }

    // A5. rc nullglob (scripture 6.10): no match -> EMPTY list.
    let none = glob::expand(&env, "no-match-prefix-zzz-*");
    if !none.is_empty() {
        return fail("nullglob expanded to a non-empty list");
    }

    // A6. Absolute pattern: `/u-*` -> "/u-..." display (resolve_fs absolute
    //     branch + the join_display root branch).
    let abs = glob::expand(&env, "/u-*");
    if !contains(&abs, "/u-glob-test") {
        return fail("/u-* missing /u-glob-test");
    }
    if !abs.iter().all(|s| s.as_bytes().starts_with(b"/u-")) {
        return fail("/u-* yielded a non-/u- path");
    }

    // B. evaluate_argv wiring via $status. A bare glob matching nothing
    //    nullglobs to an empty command (status 0); the same pattern quoted
    //    is a literal arg -> spawn NotFound -> status 127.
    let mut e2 = Env::new();
    if eval_source(&mut e2, "no-match-prefix-zzz-*").is_err() {
        return fail("bare-nullglob eval errored");
    }
    if e2.status() != 0 {
        return fail("bare nullglob did not yield empty-command status 0");
    }
    if eval_source(&mut e2, "'no-match-prefix-zzz-*'").is_err() {
        return fail("quoted-glob eval errored");
    }
    if e2.status() != 127 {
        return fail("quoted glob expanded (expected literal-spawn status 127)");
    }

    t_putstr("u-glob-test: all OK\n");
    0
}

fn contains(v: &[String], want: &str) -> bool {
    v.iter().any(|s| s == want)
}

fn is_sorted(v: &[String]) -> bool {
    v.windows(2).all(|w| w[0] <= w[1])
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-glob-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
