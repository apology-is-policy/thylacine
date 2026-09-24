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
//
// The pattern arrives as the word was written, escapes and all (`glob`'s
// header): a segment with no unescaped meta names one place and is read as a
// value, `my\ dir` as `my dir`; a segment that globs is matched with its
// escapes honoured, so `a\*b*` matches names beginning `a*b`.

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::fs;

use super::env::Env;
use super::glob::{leading_dot, matches, path_pattern};

/// Expand a glob `pattern` against the filesystem, returning the SORTED
/// list of matching paths. The result preserves the pattern's shape: an
/// absolute pattern yields absolute matches; a relative pattern yields
/// matches relative to `env.cwd()`.
///
/// Returns the EMPTY vector when nothing matches (rc nullglob, scripture
/// 6.10) -- the caller (`evaluate_argv`) contributes no argv element in
/// that case rather than falling back to the literal.
///
/// PRECONDITION: the caller gates on `has_unescaped_meta(pattern)`, so at
/// least one `/`-separated segment carries a live meta char. A pattern with
/// no such segment expands to nothing (it is never reached in practice).
pub fn expand(env: &Env, pattern: &str) -> Vec<String> {
    // Everything decided before the first read_dir -- the segments, where the
    // walk starts, what the literal start directory is -- is `path_pattern`'s
    // and host-tested there.
    let Some(p) = path_pattern(pattern) else {
        return Vec::new(); // no meta segment (precondition violated) -> nothing
    };

    let mut out: Vec<String> = Vec::new();
    walk(env.cwd(), p.absolute, &p.start, &p.walk, &mut out);
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
    segs: &[String],
    out: &mut Vec<String>,
) {
    let seg = match segs.first() {
        Some(s) => s.as_str(),
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
    // A leading-dot name matches only a segment that itself begins with a
    // literal `.` (POSIX). `.`/`..` are not emitted by any Dev's readdir, so
    // this rule does not need to special-case them.
    let seg_dot = leading_dot(seg);
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
