// nora::syntax -- native lexer-based syntax highlighting (pure, host-testable).
//
// docs/KAUA.md section 12: "a regex/lexer-based highlighter ... pure Rust,
// no_std, tiny ... the standard pre-tree-sitter approach." A resilient per-line
// classifier: it never fails on half-typed input, and it emits comments (which
// libutopia's parser-lexer skips). UT (the Utopia shell language) is the v1
// language; Rust is a future `Lang` arm.
//
// Why nora-local rather than reusing libutopia's lexer: libutopia depends on
// libthyla-rs (aarch64-only), which would break nora's host tests; its
// parser-lexer also skips comments and returns Err (losing partial tokens) on
// malformed input -- neither suits a *live* highlighter (the buffer is invalid
// for most keystrokes). The UT keyword set below MIRRORS
// libutopia::parser::token::TokenKind::reserved_word -- keep the two in sync
// (the `keyword_set_is_pinned` test fixes nora's set; a shared no-libthyla lexer
// crate that would make the match a compile-time guarantee is the documented
// tree-sitter-toolchain follow-on, KAUA section 12 / #67).
//
// Imprecision is by design (KAUA section 12: "less precise than a real
// grammar"): per-line classification means a string or heredoc spanning lines
// does not carry its colour across the newline (a v1.x stateful refinement), and
// a lexical scan cannot know parse context. The high-value classes (comment,
// string, variable, keyword) are line-local and robust.

use alloc::vec;
use alloc::vec::Vec;

/// A highlight class -- the colouring category for a run of characters.
/// Language-agnostic; `nora::theme::syntax` maps each to a Bonfire hue.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HlClass {
    Text,
    Keyword,
    Str,
    Var,
    Comment,
    Number,
    /// Reserved for v1.x operator colouring; the UT scanner does not emit it yet
    /// (operators stay `Text` to avoid lexical false positives, e.g. `*`/`+`
    /// inside bare words and globs).
    Operator,
}

/// One classified run on a line: characters `[start, end)` carry `class`.
/// Coordinates are CHAR indices (the renderer paints one cell per char).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HlSpan {
    pub start: usize,
    pub end: usize,
    pub class: HlClass,
}

/// The language a buffer is highlighted as, derived from its filename.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Lang {
    None,
    Ut,
}

impl Lang {
    /// Choose a language from a filename. UT for a `*.ut` basename (e.g.
    /// `/bin/fun.ut`); nothing else is highlighted yet.
    pub fn from_filename(name: Option<&str>) -> Lang {
        let name = match name {
            Some(n) => n,
            None => return Lang::None,
        };
        let base = name.rsplit('/').next().unwrap_or(name);
        match base.rsplit('.').next() {
            Some("ut") if base.contains('.') => Lang::Ut,
            _ => Lang::None,
        }
    }

    /// Classify `line` into char-range spans (only the non-`Text` runs). Never
    /// fails; tolerates any input (an unterminated string runs to end-of-line).
    pub fn highlight_line(self, line: &str) -> Vec<HlSpan> {
        match self {
            Lang::Ut => ut_highlight(line),
            Lang::None => Vec::new(),
        }
    }

    /// Per-character classes for `line` (length == `line.chars().count()`); the
    /// renderer indexes this by char position. Gaps are `Text`.
    pub fn line_classes(self, line: &str) -> Vec<HlClass> {
        let n = line.chars().count();
        let mut classes = vec![HlClass::Text; n];
        for sp in self.highlight_line(line) {
            let end = sp.end.min(n);
            let mut i = sp.start.min(n);
            while i < end {
                classes[i] = sp.class;
                i += 1;
            }
        }
        classes
    }
}

/// The UT reserved words -- MIRRORS libutopia::parser::token::reserved_word.
/// Keep in sync (the `keyword_set_is_pinned` test fixes this set).
const KEYWORDS: [&str; 16] = [
    "fn", "let", "if", "else", "case", "for", "while", "in", "try", "catch", "return", "break",
    "continue", "on", "mask", "trace",
];

fn ut_highlight(line: &str) -> Vec<HlSpan> {
    let chars: Vec<char> = line.chars().collect();
    let n = chars.len();
    let mut spans = Vec::new();
    let mut i = 0;
    while i < n {
        let c = chars[i];
        if c == '#' {
            // A `#` begins a comment that runs to end-of-line.
            spans.push(HlSpan { start: i, end: n, class: HlClass::Comment });
            break;
        } else if c == '\'' {
            let start = i;
            i += 1;
            while i < n {
                if chars[i] == '\'' {
                    // rc convention: a doubled `''` is an escaped quote -- stay in.
                    if i + 1 < n && chars[i + 1] == '\'' {
                        i += 2;
                        continue;
                    }
                    i += 1;
                    break;
                }
                i += 1;
            }
            spans.push(HlSpan { start, end: i, class: HlClass::Str });
        } else if c == '"' {
            let start = i;
            i += 1;
            while i < n {
                if chars[i] == '\\' && i + 1 < n {
                    i += 2; // `\"` (and any `\X`) does not close the string
                    continue;
                }
                if chars[i] == '"' {
                    i += 1;
                    break;
                }
                i += 1;
            }
            spans.push(HlSpan { start, end: i, class: HlClass::Str });
        } else if c == '$' {
            // `$name`, `$#name`, `$"name`, or a bare `$`. The `(` of `$(cmd)` is
            // not a name char, so a substitution colours only the `$` (the inner
            // command lexes as ordinary text) -- acceptable v1 imprecision.
            let start = i;
            i += 1;
            if i < n && (chars[i] == '#' || chars[i] == '"') {
                i += 1;
            }
            while i < n && is_var_char(chars[i]) {
                i += 1;
            }
            spans.push(HlSpan { start, end: i, class: HlClass::Var });
        } else if is_word_char(c) {
            let start = i;
            while i < n && is_word_char(chars[i]) {
                i += 1;
            }
            // Classify the WHOLE shell word: `ifconfig` is not the `if` keyword.
            let class = classify_word(&chars[start..i]);
            if class != HlClass::Text {
                spans.push(HlSpan { start, end: i, class });
            }
        } else {
            // Whitespace / operators / other -> Text.
            i += 1;
        }
    }
    spans
}

fn is_var_char(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// Shell-word characters (mirrors libutopia's lexer: alphanumerics, `_`, and the
/// path/glob set). Deliberately includes `-`/`.`/`*`/`[`/`]` so flags, paths and
/// globs scan as one word rather than fragmenting into false operators.
fn is_word_char(c: char) -> bool {
    c.is_alphanumeric() || matches!(c, '_' | '-' | '.' | '/' | '+' | ':' | '@' | ',' | '*' | '[' | ']')
}

fn classify_word(w: &[char]) -> HlClass {
    if is_keyword(w) {
        HlClass::Keyword
    } else if !w.is_empty() && w.iter().all(|c| c.is_ascii_digit()) {
        HlClass::Number
    } else {
        HlClass::Text
    }
}

fn is_keyword(w: &[char]) -> bool {
    KEYWORDS.iter().any(|k| k.chars().eq(w.iter().copied()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ut(line: &str) -> Vec<HlSpan> {
        Lang::Ut.highlight_line(line)
    }

    fn has(spans: &[HlSpan], start: usize, end: usize, class: HlClass) -> bool {
        spans.iter().any(|s| s.start == start && s.end == end && s.class == class)
    }

    #[test]
    fn comment_runs_to_end_of_line() {
        assert!(has(&ut("# c"), 0, 3, HlClass::Comment));
        assert!(has(&ut("x # c"), 2, 5, HlClass::Comment));
    }

    #[test]
    fn keyword_recognized() {
        assert!(has(&ut("if"), 0, 2, HlClass::Keyword));
        assert!(has(&ut("fn f"), 0, 2, HlClass::Keyword));
    }

    #[test]
    fn keyword_is_not_a_substring() {
        // `iffy` is one shell word, not the `if` keyword.
        assert!(ut("iffy").iter().all(|s| s.class != HlClass::Keyword));
    }

    #[test]
    fn double_quoted_string() {
        assert!(has(&ut("\"hi\""), 0, 4, HlClass::Str));
    }

    #[test]
    fn single_quoted_string_with_doubled_escape() {
        // 'a''b' -- the inner '' is an escaped quote; the whole run is one string.
        assert!(has(&ut("'a''b'"), 0, 6, HlClass::Str));
    }

    #[test]
    fn unterminated_string_is_resilient() {
        // Never panics; the open string runs to end-of-line.
        assert!(has(&ut("\"oops"), 0, 5, HlClass::Str));
        assert!(has(&ut("'oops"), 0, 5, HlClass::Str));
    }

    #[test]
    fn variable_forms() {
        assert!(has(&ut("$foo"), 0, 4, HlClass::Var));
        assert!(has(&ut("$#n"), 0, 3, HlClass::Var));
        assert!(has(&ut("$"), 0, 1, HlClass::Var));
    }

    #[test]
    fn all_digit_word_is_a_number() {
        assert!(has(&ut("123"), 0, 3, HlClass::Number));
        // a mixed word is not a number
        assert!(ut("12a").iter().all(|s| s.class != HlClass::Number));
    }

    #[test]
    fn none_lang_highlights_nothing() {
        assert!(Lang::None.highlight_line("if x # c").is_empty());
    }

    #[test]
    fn from_filename_picks_ut() {
        assert_eq!(Lang::from_filename(Some("a.ut")), Lang::Ut);
        assert_eq!(Lang::from_filename(Some("/bin/fun.ut")), Lang::Ut);
        assert_eq!(Lang::from_filename(Some("a.rs")), Lang::None);
        assert_eq!(Lang::from_filename(Some("noext")), Lang::None);
        assert_eq!(Lang::from_filename(None), Lang::None);
    }

    #[test]
    fn line_classes_fill_and_length() {
        let cs = Lang::Ut.line_classes("if x");
        assert_eq!(cs.len(), 4);
        assert_eq!(cs[0], HlClass::Keyword);
        assert_eq!(cs[1], HlClass::Keyword);
        assert_eq!(cs[2], HlClass::Text); // space
        assert_eq!(cs[3], HlClass::Text); // x
    }

    #[test]
    fn keyword_set_is_pinned() {
        // Fixes nora's UT keyword set (mirror of libutopia reserved_word). If UT
        // gains/loses a keyword, update BOTH (see the module header).
        let mut got = KEYWORDS.to_vec();
        got.sort_unstable();
        let mut want = [
            "break", "case", "catch", "continue", "else", "fn", "for", "if", "in", "let", "mask",
            "on", "return", "trace", "try", "while",
        ]
        .to_vec();
        want.sort_unstable();
        assert_eq!(got, want);
    }
}
