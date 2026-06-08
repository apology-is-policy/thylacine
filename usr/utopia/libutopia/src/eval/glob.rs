// libutopia::eval::glob -- the rc/POSIX-shape glob pattern matcher.
//
// === Scope at U-6a ===
//
// At U-6a glob matching is used for two purposes ONLY:
//
//   1. `case $x { pat => ... }` -- pattern matching scrutinee against
//      arm patterns (scripture section 7.1).
//   2. `matches` operator -- e.g., `if ($var matches *.c) ...`
//      (scripture section 7.3).
//
// Both are PATTERN MATCH ONLY -- the matcher (`matches` / `has_meta`)
// does no filesystem I/O.
//
// === Filesystem expansion (U-6e-b-2) ===
//
// `expand` (below) is the argv-time filesystem walk (scripture 6.10:
// `*.rs` -> the list of files it names). It splits the pattern on `/`,
// walks the directory tree one segment at a time (read_dir + `matches`
// per level, descending only into directories for non-final segments),
// and returns the SORTED list of matching paths. A pattern matching
// nothing expands to the EMPTY list (rc nullglob), NOT the literal. It
// is invoked from `stmt::evaluate_argv` only for a bare unquoted word
// carrying a meta char; quoted words and `^`-concats never expand.
//
// `**` is NOT special-cased: a `**` segment behaves as `*` (matches one
// path component), so recursive descent is a v1.x refinement.
//
// === Pattern syntax ===
//
// Scripture 6.10:
//   * -- zero or more characters, NOT crossing `/`.
//   ? -- exactly one character, NOT crossing `/`.
//   [abc] -- character class.
//   [!abc] -- negated character class.
//   ** -- recursive (crosses `/`); only valid as a complete path
//         segment.
//
// For the case-arm and `matches` use cases, the scrutinee is
// typically NOT a path -- it's a string. In that context `*` simply
// matches any sequence (since the string has no `/`). `**` adds no
// expressive power. We implement the no-`/` exclusion correctly
// (so `*.c` won't match `dir/foo.c`) but don't special-case `**`
// at U-6a.
//
// === Algorithm ===
//
// Standard recursive backtracking glob matcher. The state is
// (pattern_bytes, input_bytes); each step:
//   - `*` -> try matching empty, otherwise advance input one byte
//     (refusing `/`) and retry.
//   - `?` -> match exactly one byte (refusing `/`).
//   - `[...]` -> match the character class.
//   - any other byte -> match literally.
//
// We operate on bytes, not codepoints, because:
//   - The pattern's `[a-z]` ranges are conventionally byte-level in
//     POSIX globs.
//   - UTF-8 is self-synchronizing; a partial-byte match cannot
//     occur if both sides are valid UTF-8 (and they are by
//     scripture 4.2).
//   - Bytewise simplifies the algorithm.

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::fs;

use super::env::Env;

/// Match a glob `pattern` against `input`. Returns true on full
/// match. Pattern and input are bytes (UTF-8 safe per module docs).
pub fn matches(pattern: &str, input: &str) -> bool {
    match_bytes(pattern.as_bytes(), input.as_bytes())
}

fn match_bytes(pat: &[u8], inp: &[u8]) -> bool {
    let mut pi = 0;
    let mut ii = 0;
    // For backtracking: when we encounter a `*`, remember the
    // pattern position AFTER the star and the input position at
    // which the star started consuming. On a literal mismatch we
    // retry by consuming one more input byte.
    let mut star_pat: Option<usize> = None;
    let mut star_inp: usize = 0;

    while ii < inp.len() {
        let p = if pi < pat.len() { Some(pat[pi]) } else { None };
        match p {
            Some(b'*') => {
                // Greedy: try to match zero chars first, then on
                // failure consume one more byte at a time.
                star_pat = Some(pi + 1);
                star_inp = ii;
                pi += 1;
            }
            Some(b'?') => {
                if inp[ii] == b'/' {
                    // `?` does NOT cross `/`.
                    if let Some(retry) = star_pat {
                        pi = retry;
                        star_inp += 1;
                        // The star may now need to extend further;
                        // but ii stays at star_inp; if star_inp
                        // crossed a `/` we lost (handled below).
                        ii = star_inp;
                        if ii < inp.len() && inp[ii] == b'/' {
                            // `*` cannot consume `/` either.
                            return false;
                        }
                        continue;
                    }
                    return false;
                }
                pi += 1;
                ii += 1;
            }
            Some(b'[') => {
                // Parse the character class: `[abc]` or `[!abc]`.
                let class_end = match find_class_end(pat, pi) {
                    Some(e) => e,
                    None => {
                        // Unterminated `[`; treat as literal `[`.
                        if inp[ii] == b'[' {
                            pi += 1;
                            ii += 1;
                            continue;
                        }
                        return false;
                    }
                };
                let class_body = &pat[pi + 1..class_end];
                let (negate, body) = if !class_body.is_empty() && class_body[0] == b'!' {
                    (true, &class_body[1..])
                } else {
                    (false, class_body)
                };
                let hit = char_class_match(body, inp[ii]);
                if hit ^ negate {
                    pi = class_end + 1;
                    ii += 1;
                } else if let Some(retry) = star_pat {
                    pi = retry;
                    star_inp += 1;
                    ii = star_inp;
                    if ii < inp.len() && inp[ii] == b'/' {
                        return false;
                    }
                } else {
                    return false;
                }
            }
            Some(c) => {
                if c == inp[ii] {
                    pi += 1;
                    ii += 1;
                } else if let Some(retry) = star_pat {
                    if inp[star_inp] == b'/' {
                        // `*` cannot extend across `/`.
                        return false;
                    }
                    pi = retry;
                    star_inp += 1;
                    ii = star_inp;
                } else {
                    return false;
                }
            }
            None => {
                // Pattern exhausted, input not. Maybe a prior `*`
                // can extend.
                if let Some(retry) = star_pat {
                    if inp[star_inp] == b'/' {
                        return false;
                    }
                    pi = retry;
                    star_inp += 1;
                    ii = star_inp;
                } else {
                    return false;
                }
            }
        }
    }

    // Input exhausted. Pattern must be exhausted too (or be only
    // `*` characters, which match the empty tail).
    while pi < pat.len() {
        if pat[pi] != b'*' {
            return false;
        }
        pi += 1;
    }
    true
}

fn find_class_end(pat: &[u8], start: usize) -> Option<usize> {
    // start points at `[`. Search for the matching `]`. The first
    // character after `[` (or after `[!`) is allowed to be `]`
    // literally per POSIX glob convention.
    let mut i = start + 1;
    if i < pat.len() && pat[i] == b'!' {
        i += 1;
    }
    if i < pat.len() && pat[i] == b']' {
        // `[]...]` -- the `]` at position i is literal.
        i += 1;
    }
    while i < pat.len() {
        if pat[i] == b']' {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn char_class_match(class: &[u8], c: u8) -> bool {
    let mut i = 0;
    let mut prev: Option<u8> = None;
    while i < class.len() {
        if class[i] == b'-' && prev.is_some() && i + 1 < class.len() {
            // Range: prev..=class[i+1].
            let lo = prev.unwrap();
            let hi = class[i + 1];
            if c >= lo.min(hi) && c <= lo.max(hi) {
                return true;
            }
            prev = None;
            i += 2;
            continue;
        }
        if class[i] == c {
            return true;
        }
        prev = Some(class[i]);
        i += 1;
    }
    false
}

/// Whether a string contains any glob meta characters. Used by argv
/// expansion (U-6e) to decide whether to invoke fs walk vs treat the
/// word as literal. At U-6a only exposed for callers that may want
/// to short-circuit pattern compilation.
pub fn has_meta(s: &str) -> bool {
    s.bytes().any(|b| matches!(b, b'*' | b'?' | b'['))
}

// ===========================================================================
// Filesystem expansion (U-6e-b-2)
// ===========================================================================

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

/// Match a glob pattern against a list of candidate strings,
/// returning a Vec of indices of matches. Useful for set membership
/// against a list literal. Currently unused (case-arm and `matches`
/// use single-string match); kept for U-6c+.
#[allow(dead_code)]
pub fn match_any(pattern: &str, candidates: &[alloc::string::String]) -> Vec<usize> {
    candidates
        .iter()
        .enumerate()
        .filter_map(|(i, c)| if matches(pattern, c) { Some(i) } else { None })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;

    #[test]
    fn literal_match() {
        assert!(matches("foo", "foo"));
        assert!(!matches("foo", "bar"));
        assert!(!matches("foo", "foobar"));
    }

    #[test]
    fn star_matches_anything() {
        assert!(matches("*", ""));
        assert!(matches("*", "anything"));
        assert!(matches("*", "with spaces"));
    }

    #[test]
    fn star_with_suffix() {
        assert!(matches("*.c", "foo.c"));
        assert!(matches("*.c", ".c"));
        assert!(!matches("*.c", "foo.cpp"));
        assert!(!matches("*.c", "foo.c.bak"));
    }

    #[test]
    fn star_does_not_cross_slash() {
        assert!(!matches("*", "a/b"));
        assert!(!matches("*.c", "dir/foo.c"));
        assert!(matches("dir/*.c", "dir/foo.c"));
    }

    #[test]
    fn question_mark() {
        assert!(matches("?", "a"));
        assert!(!matches("?", ""));
        assert!(!matches("?", "ab"));
        assert!(matches("a?b", "axb"));
        assert!(!matches("a?b", "a/b"));
    }

    #[test]
    fn char_class() {
        assert!(matches("[abc]", "a"));
        assert!(matches("[abc]", "b"));
        assert!(matches("[abc]", "c"));
        assert!(!matches("[abc]", "d"));
    }

    #[test]
    fn negated_class() {
        assert!(matches("[!abc]", "d"));
        assert!(!matches("[!abc]", "a"));
    }

    #[test]
    fn class_range() {
        assert!(matches("[a-z]", "k"));
        assert!(!matches("[a-z]", "K"));
        assert!(matches("[0-9]", "5"));
    }

    #[test]
    fn complex_pattern() {
        assert!(matches("*.[ch]", "foo.c"));
        assert!(matches("*.[ch]", "foo.h"));
        assert!(!matches("*.[ch]", "foo.cpp"));
    }

    #[test]
    fn match_any_via_helper() {
        let candidates: alloc::vec::Vec<alloc::string::String> = ["foo.c", "bar.rs", "baz.c"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let hits = match_any("*.c", &candidates);
        assert_eq!(hits, alloc::vec![0, 2]);
    }

    #[test]
    fn has_meta_detects() {
        assert!(!has_meta("plain"));
        assert!(has_meta("foo*"));
        assert!(has_meta("foo?"));
        assert!(has_meta("foo[abc]"));
    }
}
