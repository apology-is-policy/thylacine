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
// does no filesystem I/O, which is what lets this module run its tests on
// the host. The argv-time filesystem walk (scripture 6.10: `*.rs` -> the
// files it names) is `super::pathname`, gated with the syscall half.
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
//   - `\x` -> match `x` literally, whatever it is (inside a class too).
//   - any other byte -> match literally.
//
// === Escapes ===
//
// A pattern is a bare word AS WRITTEN: the lexer keeps a word's `\` and the
// evaluator decides what it means where the word lands. In a value it is
// removed (`parser::lexer::unescape`); in a glob or a pattern it makes the
// next character literal, so `\*` names a star and never globs -- POSIX
// fnmatch's rule, and the only reading under which `rm \*` is safe. Evaluated
// parts of a pattern (quotes, variables, substitutions) enter through
// `escape_backslashes`, which keeps their backslashes literal as they always
// were; `has_meta` stays the check for a LITERAL string (a file name being
// quoted), `has_unescaped_meta` the check for a pattern.
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

use crate::parser::lexer::unescape;

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
                // `\x` matches `x` itself, meta or not; a lone `\` at the
                // end of the pattern matches a `\`.
                let (c, width) = if c == b'\\' && pi + 1 < pat.len() {
                    (pat[pi + 1], 2)
                } else {
                    (c, 1)
                };
                if c == inp[ii] {
                    pi += width;
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
        match pat[i] {
            b']' => return Some(i),
            // An escaped `]` is a member, not the end.
            b'\\' => i += 2,
            _ => i += 1,
        }
    }
    None
}

fn char_class_match(class: &[u8], c: u8) -> bool {
    let mut i = 0;
    while i < class.len() {
        let (lo, next) = class_member(class, i);
        // `lo-hi` is a range when the `-` is unescaped and not the last byte.
        if next + 1 < class.len() && class[next] == b'-' {
            let (hi, after) = class_member(class, next + 1);
            if c >= lo.min(hi) && c <= lo.max(hi) {
                return true;
            }
            i = after;
            continue;
        }
        if lo == c {
            return true;
        }
        i = next;
    }
    false
}

/// The class member at `i` -- a byte, or the byte an escape names -- and the
/// index past it.
fn class_member(class: &[u8], i: usize) -> (u8, usize) {
    if class[i] == b'\\' && i + 1 < class.len() {
        (class[i + 1], i + 2)
    } else {
        (class[i], i + 1)
    }
}

/// Whether a LITERAL string -- a file name, not a pattern -- contains a glob
/// meta character, so that writing it bare would glob. A pattern's escapes
/// need `has_unescaped_meta`.
pub fn has_meta(s: &str) -> bool {
    s.bytes().any(|b| matches!(b, b'*' | b'?' | b'['))
}

/// Whether a pattern -- a bare word as written, escapes and all -- carries a
/// meta character its own `\` does not escape: the test for whether the word
/// globs at all.
pub fn has_unescaped_meta(pattern: &str) -> bool {
    let b = pattern.as_bytes();
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'\\' => i += 2,
            b'*' | b'?' | b'[' => return true,
            _ => i += 1,
        }
    }
    false
}

/// `value` as a pattern whose backslashes match themselves. Only a bare
/// word's own `\` escapes; a value -- a quoted string, a variable, a
/// substitution -- has always matched its backslashes literally, and its
/// glob metas stay live.
pub fn escape_backslashes(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for c in value.chars() {
        if c == '\\' {
            out.push('\\');
        }
        out.push(c);
    }
    out
}

/// A path pattern's `/`-separated segments, empty ones dropped (so `//` and
/// a leading or trailing `/` normalize away). An escaped `/` separates too,
/// as in POSIX: no path component can hold one, and keeping the `\` would
/// leave a segment ending in a lone backslash.
pub fn pattern_segments(pattern: &str) -> Vec<String> {
    let mut segs = Vec::new();
    let mut cur = String::new();
    let mut chars = pattern.chars();
    while let Some(c) = chars.next() {
        match c {
            '\\' => match chars.next() {
                Some('/') => take_segment(&mut cur, &mut segs),
                Some(n) => {
                    cur.push('\\');
                    cur.push(n);
                }
                None => cur.push('\\'),
            },
            '/' => take_segment(&mut cur, &mut segs),
            _ => cur.push(c),
        }
    }
    take_segment(&mut cur, &mut segs);
    segs
}

fn take_segment(cur: &mut String, segs: &mut Vec<String>) {
    if !cur.is_empty() {
        segs.push(core::mem::take(cur));
    }
}

/// Whether a pattern segment begins with a literal `.` -- the only thing that
/// may match a name's leading dot (POSIX 2.13.3). A bracket expression never
/// does.
pub fn leading_dot(segment: &str) -> bool {
    segment.starts_with('.') || segment.starts_with("\\.")
}

/// A path pattern read for the filesystem walk (`pathname::expand`), which
/// is gated: everything decided before the first `read_dir` is here, where
/// the host tests it.
#[derive(Debug, PartialEq, Eq)]
pub struct PathPattern {
    /// It names a path from the root.
    pub absolute: bool,
    /// The directory its leading meta-free segments name -- a value, so
    /// their escapes are removed (`my\ dir` is `my dir`).
    pub start: String,
    /// The segments to match from `start` on, as written, escapes kept for
    /// the matcher.
    pub walk: Vec<String>,
}

/// Read `pattern` for the walk; `None` when no segment carries a live meta.
pub fn path_pattern(pattern: &str) -> Option<PathPattern> {
    // An escaped `/` separates too (`pattern_segments`), so it leads.
    let absolute = pattern.starts_with('/') || pattern.starts_with("\\/");
    let mut segs = pattern_segments(pattern);
    let at = segs.iter().position(|s| has_unescaped_meta(s))?;
    let walk = segs.split_off(at);
    let prefix: Vec<String> = segs.iter().map(|s| unescape(s)).collect();
    let mut start = String::new();
    if absolute {
        start.push('/');
    }
    start.push_str(&prefix.join("/"));
    Some(PathPattern {
        absolute,
        start,
        walk,
    })
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

    #[test]
    fn an_escaped_meta_matches_only_itself() {
        assert!(matches("a\\*b", "a*b"));
        assert!(!matches("a\\*b", "aXb"));
        assert!(matches("\\*", "*"));
        assert!(!matches("\\*", "x"));
        assert!(matches("\\?", "?"));
        assert!(!matches("\\?", "x"));
        assert!(matches("\\[a]", "[a]"));
        assert!(!matches("\\[a]", "a"));
    }

    #[test]
    fn an_escape_mixes_with_live_metas() {
        // `a\*b*`: the first star is a character, the second a wildcard.
        assert!(matches("a\\*b*", "a*b"));
        assert!(matches("a\\*b*", "a*bcd"));
        assert!(!matches("a\\*b*", "aXbcd"));
        // An escaped character after a star, reached by backtracking.
        assert!(matches("*\\*x", "a*b*x"));
        assert!(matches("*\\*", "ab*"));
        assert!(!matches("*\\*", "ab"));
        // A pattern ending in an escaped star is not a trailing wildcard.
        assert!(!matches("a\\*", "a"));
    }

    #[test]
    fn a_backslash_escapes_itself_and_a_lone_one_is_itself() {
        assert!(matches("a\\\\", "a\\"));
        assert!(!matches("a\\\\", "a\\\\"));
        assert!(matches("a\\", "a\\"));
        assert!(matches("\\a", "a"));
    }

    #[test]
    fn an_escape_inside_a_class_is_a_member() {
        assert!(matches("[\\]a]", "]"));
        assert!(matches("[\\]a]", "a"));
        assert!(!matches("[\\]a]", "\\"));
        assert!(matches("[a\\-z]", "-"));
        assert!(!matches("[a\\-z]", "m"));
        assert!(matches("[a-z]", "m"));
    }

    #[test]
    fn has_unescaped_meta_skips_escaped_metas() {
        assert!(!has_unescaped_meta("\\*"));
        assert!(!has_unescaped_meta("a\\?b\\[c"));
        assert!(has_unescaped_meta("a\\*b*"));
        assert!(has_unescaped_meta("\\\\*"));
        assert!(!has_unescaped_meta("plain"));
        assert!(!has_unescaped_meta("tail\\"));
        // has_meta reads a literal string: the star is there either way.
        assert!(has_meta("\\*"));
    }

    #[test]
    fn a_value_escaped_for_a_pattern_matches_itself() {
        for v in ["C:\\dir", "a\\b\\", "\\", "plain", "tr\\*ail"] {
            let p = escape_backslashes(v);
            assert!(matches(&p, v), "{:?} as {:?}", v, p);
        }
        assert!(!matches(&escape_backslashes("C:\\dir"), "C:dir"));
        // Only the backslashes are made literal; a value's metas stay live.
        assert!(matches(&escape_backslashes("a*"), "abc"));
    }

    #[test]
    fn pattern_segments_split_on_every_slash() {
        assert_eq!(pattern_segments("my\\ dir/*.txt"), ["my\\ dir", "*.txt"]);
        assert_eq!(pattern_segments("/a//b*/"), ["a", "b*"]);
        assert_eq!(pattern_segments("a\\/b*"), ["a", "b*"]);
        assert_eq!(pattern_segments("tail\\"), ["tail\\"]);
        assert_eq!(pattern_segments("caf\\\u{e9}/*"), ["caf\\\u{e9}", "*"]);
    }

    #[test]
    fn a_path_pattern_starts_at_the_directory_its_literal_segments_name() {
        let read = |p: &str| path_pattern(p).map(|p| (p.absolute, p.start, p.walk));
        let v = |xs: &[&str]| xs.iter().map(|x| x.to_string()).collect::<Vec<_>>();
        assert_eq!(
            read("my\\ dir/*.txt"),
            Some((false, "my dir".into(), v(&["*.txt"])))
        );
        assert_eq!(read("/a\\/b/c*"), Some((true, "/a/b".into(), v(&["c*"]))));
        assert_eq!(read("\\/u-*"), Some((true, "/".into(), v(&["u-*"]))));
        // Past the first glob every segment is matched, escapes and all.
        assert_eq!(
            read("*/x\\ y"),
            Some((false, "".into(), v(&["*", "x\\ y"])))
        );
        assert_eq!(read("a\\*b"), None);
        assert_eq!(read("plain/path"), None);
    }

    #[test]
    fn only_a_literal_dot_leads() {
        assert!(leading_dot(".b*"));
        assert!(leading_dot("\\.b*"));
        assert!(!leading_dot("[.]b*"));
        assert!(!leading_dot("*b"));
        assert!(!leading_dot("\\\\.b"));
    }
}
