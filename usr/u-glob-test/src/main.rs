// /u-glob-test -- U-6e-b-2 glob-argv-expansion boot probe.
//
// Runs PRE-pivot (flat devramfs root). Two layers:
//
//   A. The load-bearing fs-walk -- libutopia::eval::pathname::expand directly
//      against the boot ramfs. argv echoes to a dropped pipe at v1.0
//      (no terminal-backed fd 1 until U-PTY), so the expansion itself is
//      asserted here on the returned Vec rather than via command output:
//        - prefix star (`u-*`), bare star (`*`), single-char (`?`),
//          char-class (`[vw]*`), absolute (`/u-*`);
//        - sortedness, single-level containment (no `/`, no dotfile leak),
//          rc nullglob (no-match -> EMPTY list);
//        - escapes honoured by the walk (`versio\n*`, `v*\*`).
//
//   B. The evaluate_argv wiring -- observed via $status (the one signal
//      that survives the dropped-pipe stdio): a BARE glob matching nothing
//      nullglobs to an empty command -> status 0; the SAME pattern QUOTED
//      is taken literally and spawned (NotFound) -> status 127. The delta
//      proves the bare branch globbed and the quoted branch did not. The
//      same pattern with its star ESCAPED must behave like the quoted one:
//      a word keeps its `\` through lexing, and `\*` is a star, never a
//      wildcard (before that, `rm \*` removed every file).
//
//   C. Escaped metas in `case` arms and `matches`, by the same signal: an
//      arm (or an `if` body) that runs a missing command leaves 127 when it
//      fires and 0 when nothing does. Each escaped case has a twin that
//      must fire, so a 0 cannot come from a pattern that never matches.
//
// joey gates the boot on this binary's status==0.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;
use libutopia::eval::{eval_source, pathname, Env};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // cwd defaults to "/" -- relative patterns resolve against the root.
    let env = Env::new();

    // A1. Prefix star: `u-*` names the u-prefixed binaries on the flat root.
    let u = pathname::expand(&env, "u-*");
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
    let all = pathname::expand(&env, "*");
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
    let q = pathname::expand(&env, "versio?");
    if !contains(&q, "version") {
        return fail("versio? missing version");
    }

    // A4. Char class: `[vw]*` matches both version and welcome.
    let cc = pathname::expand(&env, "[vw]*");
    if !contains(&cc, "version") {
        return fail("[vw]* missing version");
    }
    if !contains(&cc, "welcome") {
        return fail("[vw]* missing welcome");
    }

    // A5. rc nullglob (scripture 6.10): no match -> EMPTY list.
    let none = pathname::expand(&env, "no-match-prefix-zzz-*");
    if !none.is_empty() {
        return fail("nullglob expanded to a non-empty list");
    }

    // A6. Absolute pattern: `/u-*` -> "/u-..." display (resolve_fs absolute
    //     branch + the join_display root branch).
    let abs = pathname::expand(&env, "/u-*");
    if !contains(&abs, "/u-glob-test") {
        return fail("/u-* missing /u-glob-test");
    }
    if !abs.iter().all(|s| s.as_bytes().starts_with(b"/u-")) {
        return fail("/u-* yielded a non-/u- path");
    }

    // A7. Escapes inside a pattern the walker matches: an escaped ordinary
    //     character still matches itself, and an escaped star is not a
    //     wildcard -- no name on this root ends in a literal `*`.
    let esc = pathname::expand(&env, "versio\\n*");
    if !contains(&esc, "version") {
        return fail("versio\\n* missing version");
    }
    let lit = pathname::expand(&env, "v*\\*");
    if !lit.is_empty() {
        return fail("v*\\* matched a name without a literal star");
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
    if eval_source(&mut e2, "no-match-prefix-zzz-\\*").is_err() {
        return fail("escaped-glob eval errored");
    }
    if e2.status() != 127 {
        return fail("escaped star globbed (expected literal-spawn status 127)");
    }
    // ...and a word's value drops its escapes: `c\d` runs the `cd` builtin
    // (status 0), where a kept backslash would spawn `c\d` (127).
    if eval_source(&mut e2, "c\\d /").is_err() {
        return fail("escaped command name eval errored");
    }
    if e2.status() != 0 {
        return fail("escaped command name kept its backslash");
    }

    // C. Escaped metas in patterns (see the header).
    for (src, want, tag) in [
        (
            "case ab { a\\* => no-such-cmd-zzz }",
            0,
            "case: escaped star matched ab",
        ),
        (
            "case 'a*' { a\\* => no-such-cmd-zzz }",
            127,
            "case: escaped star missed a*",
        ),
        (
            "case ab { a* => no-such-cmd-zzz }",
            127,
            "case: live star missed ab",
        ),
        (
            "if (ab matches a\\*) { no-such-cmd-zzz }",
            0,
            "matches: escaped star matched ab",
        ),
        (
            "if ('a*' matches a\\*) { no-such-cmd-zzz }",
            127,
            "matches: escaped star missed a*",
        ),
        // The expression form of `case` reads its patterns the same way.
        (
            "let k = case ab { a\\* => yes ; * => no }; if ($k == yes) { no-such-cmd-zzz }",
            0,
            "case expr: escaped star matched ab",
        ),
        (
            "let k = case 'a*' { a\\* => yes ; * => no }; if ($k == yes) { no-such-cmd-zzz }",
            127,
            "case expr: escaped star missed a*",
        ),
        // An expression word's value drops its escapes too.
        (
            "if (a\\ b == 'a b') { no-such-cmd-zzz }",
            127,
            "expr: escaped space kept its backslash",
        ),
        // A quoted pattern's backslash is a backslash, as it always was.
        (
            "case 'C:\\dir' { 'C:\\dir' => no-such-cmd-zzz }",
            127,
            "case: quoted backslash escaped",
        ),
    ] {
        if eval_source(&mut e2, src).is_err() {
            return fail(tag);
        }
        if e2.status() != want {
            return fail(tag);
        }
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
