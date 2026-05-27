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
// Both are PATTERN MATCH ONLY, not file expansion. No filesystem
// walks happen here. Filesystem glob expansion (scripture section
// 6.10: `*.rs` expanding to a list of files) lands at U-6c or U-6e
// alongside argv expansion.
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

use alloc::vec::Vec;

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
