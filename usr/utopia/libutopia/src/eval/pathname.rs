// libutopia::eval::pathname -- pathname expansion: `*.rs` -> the files it names.
//
// POSIX calls this "pathname expansion" (XCU 2.13.3); scripture 6.10 is the
// rc-shaped form. It is the ONE part of globbing that asks the filesystem,
// which is why it lives apart from `glob`: the matcher is pure and runs its
// tests on the host, and this walk needs `fs::read_dir` and the shell's cwd,
// so it rides the `backend` feature with the rest of the syscall half.
//
// `expand` splits the pattern on `/`, walks the directory tree one segment at
// a time (read_dir + `glob::matches` per level, descending only into
// directories for non-final segments), and returns the SORTED list of
// matching paths. A pattern matching nothing expands to the EMPTY list (rc
// nullglob), NOT the literal. It is invoked from `stmt::evaluate_argv` only
// for a bare unquoted word carrying a meta char; quoted words and
// `^`-concats never expand.
//
// `**` is NOT special-cased: a `**` segment behaves as `*` (matches one path
// component), so recursive descent is a v1.x refinement.

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::fs;

use super::env::Env;
use super::glob::{has_meta, matches};

/// Expand a glob `pattern` against the filesystem, returning the SORTED
/// list of matching paths. The result preserves the pattern's shape: an
/// absolute pattern yields absolute matches; a relative pattern yields
/// matches relative to `env.cwd()`.
///
/// Returns the EMPTY vector when nothing matches (rc nullglob, scripture
/// 6.10) -- the caller (`evaluate_argv`) contributes no argv element in
/// that case rather than falling back to the literal.
///
/// PRECONDITION: the caller gates on `has_meta(pattern)`, so at least one
/// `/`-separated segment carries a meta char. A pattern with no meta
/// segment expands to nothing (it is never reached in practice).
pub fn expand(env: &Env, pattern: &str) -> Vec<String> {
    let leading_slash = pattern.as_bytes().first() == Some(&b'/');
    // Drop empty segments so `//`, a leading `/`, and a trailing `/` all
    // normalize away. (A trailing-slash "directories only" refinement is
    // a v1.x item.)
    let segs: Vec<&str> = pattern.split('/').filter(|s| !s.is_empty()).collect();

    // The leading run of meta-free segments is the literal start directory
    // (we don't readdir-match it -- it names exactly one place). The walk
    // begins at the first segment carrying a meta char.
    let walk_start = segs
        .iter()
        .position(|s| has_meta(s))
        .unwrap_or(segs.len());
    if walk_start >= segs.len() {
        return Vec::new(); // no meta segment (precondition violated) -> nothing
    }
    let (prefix_segs, walk_segs) = segs.split_at(walk_start);

    let start_display = if leading_slash {
        let mut s = String::from("/");
        s.push_str(&prefix_segs.join("/"));
        s
    } else {
        prefix_segs.join("/")
    };

    let mut out: Vec<String> = Vec::new();
    walk(env.cwd(), leading_slash, &start_display, walk_segs, &mut out);
    // bash sorts the final expansion as whole strings; do that once over
    // the full result (a per-level sort would diverge around the `/`
    // boundary, e.g. "a" vs "a.b").
    out.sort();
    out
}

/// Walk one pattern segment against the directory named by `dir_display`,
/// recursing into matching subdirectories for the non-final segments and
/// pushing matched paths for the final one. Recursion depth is bounded by
/// the segment count (each call consumes one segment), independent of the
/// tree's depth -- there is no unbounded descent.
fn walk(
    cwd: &str,
    leading_slash: bool,
    dir_display: &str,
    segs: &[&str],
    out: &mut Vec<String>,
) {
    let seg = match segs.first() {
        Some(s) => *s,
        None => return,
    };
    let last = segs.len() == 1;
    let dir_fs = resolve_fs(cwd, dir_display, leading_slash);
    let rd = match fs::read_dir(dir_fs.as_str()) {
        Ok(rd) => rd,
        // An unreadable directory (missing, not a dir, mount with no
        // readdir) contributes no matches -- nullglob for this branch.
        Err(_) => return,
    };
    // A leading-dot name matches only a segment that itself begins with `.`
    // (POSIX). `.`/`..` are not emitted by any Dev's readdir, so this rule
    // does not need to special-case them.
    let seg_dot = seg.as_bytes().first() == Some(&b'.');
    for entry in rd {
        let entry = match entry {
            Ok(e) => e,
            // A mid-stream readdir error stops this directory; keep what we
            // already collected.
            Err(_) => break,
        };
        let is_dir = entry.is_dir();
        let name = entry.into_file_name();
        if name.as_bytes().first() == Some(&b'.') && !seg_dot {
            continue;
        }
        if !matches(seg, &name) {
            continue;
        }
        if last {
            out.push(join_display(dir_display, &name));
        } else if is_dir {
            let child = join_display(dir_display, &name);
            walk(cwd, leading_slash, &child, &segs[1..], out);
        }
        // else: a non-final segment matched a non-directory -- cannot
        // descend, so this candidate yields nothing.
    }
}

/// Append `name` to a directory display path, preserving the path's
/// relative/absolute shape (empty dir = relative first level; `/` = the
/// root).
fn join_display(dir: &str, name: &str) -> String {
    if dir.is_empty() {
        String::from(name)
    } else if dir == "/" {
        let mut s = String::with_capacity(1 + name.len());
        s.push('/');
        s.push_str(name);
        s
    } else {
        let mut s = String::with_capacity(dir.len() + 1 + name.len());
        s.push_str(dir);
        s.push('/');
        s.push_str(name);
        s
    }
}

/// Map a pattern-shaped display path to the filesystem path to `read_dir`.
/// An absolute display is used as-is (already starts with `/`); a relative
/// display is joined onto `cwd` (an empty relative display is the cwd
/// itself).
fn resolve_fs(cwd: &str, display: &str, leading_slash: bool) -> String {
    if leading_slash {
        if display.is_empty() {
            String::from("/")
        } else {
            String::from(display)
        }
    } else if display.is_empty() {
        String::from(cwd)
    } else if cwd == "/" {
        let mut s = String::with_capacity(1 + display.len());
        s.push('/');
        s.push_str(display);
        s
    } else {
        let mut s = String::with_capacity(cwd.len() + 1 + display.len());
        s.push_str(cwd);
        s.push('/');
        s.push_str(display);
        s
    }
}
