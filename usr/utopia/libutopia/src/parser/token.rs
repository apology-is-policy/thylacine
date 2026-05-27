// libutopia::parser::token -- the token taxonomy produced by the
// U-5a lexer and consumed by the U-5b parser.
//
// === Design intent ===
//
// Per UTOPIA-SHELL-DESIGN.md sections 5-9, the rc-shape grammar
// carries an unusual amount of syntactic surface (compared to a
// minimal calculator parser): bare-word globs, two substitution
// forms, no-split variables, heredocs with three flavors, process
// substitution, regex literals after `=~`, arithmetic blocks,
// pattern-case arms, the `?` postfix operator, ...
//
// The token taxonomy below makes a few opinionated calls about
// what to recognize at lex time vs what to leave for the parser:
//
//   - Bare words include their glob meta chars (`*`, `?`, `[`, `]`,
//     `**`). The evaluator (U-6) does glob expansion. `?` carries
//     a special-case disambiguator (see lexer.rs::scan_word).
//
//   - Double-quoted strings are pre-parsed into a `Vec<DqPart>`
//     during lex. The parts are: literal text (with `\`-escapes
//     resolved), `$var`, `$#var`, and `$(cmd)`. Per scripture
//     section 6.5 we do NOT recognize `` `{cmd} `` inside DQ
//     strings (backtick is literal there) nor `$"var` (the `"`
//     closes the string).
//
//   - Substitution bodies (`$(cmd)` and `` `{cmd} ``) are stored
//     as RAW source strings (the bytes between the open and close
//     delimiters). The parser (U-5b) re-tokenizes them when it
//     descends into the substitution. This keeps the lexer's
//     control flow flat -- one scan, no nesting.
//
//   - Process substitution (`<(cmd)`, `>(cmd)`) follows the same
//     raw-body pattern.
//
//   - Heredoc bodies ARE collected by the lexer (the body sits on
//     a different line than the start; only the lexer has line-
//     position context). Bodies are parsed into a Vec<DqPart>
//     when `interp == true`, or a single Literal part when `interp
//     == false` (the `<<"TAG"` quoted-tag flavor).
//
//   - Regex literals after `=~` (per scripture section 7.3:
//     `$var =~ /^foo/`) are scanned as a special `Regex(String)`
//     token containing the regex body with `\/` escapes resolved.
//     The regex semantics itself is opaque at v1.0 (open
//     question #5 answer).
//
//   - Arithmetic blocks `(( expr ))` are NOT entered as a separate
//     lex mode at U-5a. `((` and `))` emit as DoubleLParen /
//     DoubleRParen, and the body lexes under normal command rules.
//     The parser at U-5c will re-interpret the resulting token
//     stream in arithmetic context (where `+`, `*`, etc. become
//     operators). This is a soft trade -- it keeps U-5a tight at
//     the cost of slightly awkward parser logic at U-5c.
//
// === Span discipline ===
//
// Every token carries a Span covering its source bytes. For
// compound tokens (DoubleQuoted, Subst, Backtick, ProcSubIn/Out,
// HeredocStart, HeredocBody, Regex), the outer span covers from
// the opening delimiter through the closing delimiter (inclusive
// of both). DqPart sub-spans are not tracked at U-5a; if U-5d
// needs them for finer-grained error reporting we add them then.

use alloc::string::String;
use alloc::vec::Vec;

use super::span::Span;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Token {
    pub kind: TokenKind,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TokenKind {
    // === Literals ===========================================================
    /// Bare word. Contains the raw source bytes (no escape resolution
    /// at lex; the evaluator's glob+expand path handles that). May
    /// include glob meta chars (`*`, `?`, `[`, `]`, `**`).
    Word(String),
    /// `'literal'`. Contents are the raw bytes between the quotes,
    /// with the rc-style `''` escape (doubled quote) resolved to a
    /// single `'`.
    SingleQuoted(String),
    /// `"interpolating $var $(cmd)"`. The Vec is the parsed structure
    /// of the string interior. See DqPart below.
    DoubleQuoted(Vec<DqPart>),

    // === Top-level variable refs ============================================
    //
    // These forms appear OUTSIDE quotes. Inside `"..."`, the same
    // forms are recognized but produce DqPart::Var / VarLen instead.
    //
    /// `$name`. The String is the name (without the leading `$`).
    Var(String),
    /// `$#name` -- length of the named variable.
    VarLen(String),
    /// `$"name` -- no-split form (join list elements with spaces).
    /// Per scripture section 6.8: rc's clean answer to bash's
    /// `"$@"` vs `$@` mess.
    VarNoSplit(String),

    // === Top-level substitutions ============================================
    //
    // The body is the raw source between the delimiters. The parser
    // re-tokenizes when it descends.
    //
    /// `$(cmd)` -- POSIX-shape substitution. String is the raw `cmd`
    /// bytes (between `(` and `)`).
    Subst(String),
    /// `` `{cmd} `` -- rc-traditional substitution. String is the raw
    /// `cmd` bytes (between `{` and `}`; the surrounding backticks
    /// are NOT in the stored body).
    Backtick(String),

    // === Process substitution ===============================================
    /// `<(cmd)`. String is the raw `cmd` bytes (between `(` and `)`).
    ProcSubIn(String),
    /// `>(cmd)`. String is the raw `cmd` bytes (between `(` and `)`).
    ProcSubOut(String),

    // === Heredocs ===========================================================
    //
    // Heredocs are a two-position construct: the `<<TAG` start
    // appears on one line; the body follows after that line's
    // newline, terminated by a line containing exactly `TAG`. The
    // lexer collects both during a single pass: HeredocStart is
    // emitted at the `<<TAG` position; HeredocBody is emitted
    // immediately after the corresponding Newline.
    //
    /// `<<TAG`, `<<-TAG`, or `<<"TAG"`. The flags control interp +
    /// leading-tab strip per scripture section 6.11.
    HeredocStart {
        tag: String,
        /// `<<TAG` and `<<-TAG`: true; `<<"TAG"`: false.
        interp: bool,
        /// `<<-TAG`: true; others: false.
        strip_tabs: bool,
    },
    /// The body of the immediately-preceding HeredocStart. Same
    /// DqPart shape as DoubleQuoted when `interp == true`; a single
    /// Literal part when `interp == false`.
    HeredocBody(Vec<DqPart>),

    // === Regex literal (after `=~`) =========================================
    /// `/pattern/`. String is the regex body with `\/` -> `/` escape
    /// resolved (other `\X` escapes are preserved verbatim -- the
    /// regex engine will interpret them).
    Regex(String),

    // === Punctuation =======================================================
    LBrace,        // {
    RBrace,        // }
    LParen,        // (
    RParen,        // )
    DoubleLParen,  // ((
    DoubleRParen,  // ))
    Semicolon,     // ;
    /// `\n`. Significant in command grammar (statement separator).
    /// Sequences of consecutive newlines are NOT collapsed at lex;
    /// the parser handles that.
    Newline,

    // === Pipe + logical / sequence ==========================================
    Pipe,          // |
    PipeTolerate,  // ?|
    AndAnd,        // &&
    OrOr,          // ||
    /// `&` -- background-job suffix (per scripture section 10.5)
    /// OR bitwise-and (inside `(( ))` per scripture section 6.13).
    Ampersand,

    // === Redirects =========================================================
    Less,            // <
    Greater,         // >
    /// `>>`. (`<<` is consumed as HeredocStart if a valid tag
    /// follows; otherwise it's a lex error at U-5a -- the
    /// shift-left arithmetic case is deferred to U-5c when arith
    /// mode lands.)
    GreaterGreater,

    // === Comparison / assignment ===========================================
    Equal,         // =
    DoubleEqual,   // ==
    NotEqual,      // !=
    LessEqual,     // <=
    GreaterEqual,  // >=
    /// `=~` -- regex match. Per scripture section 7.3 the immediately
    /// following token is a Regex literal (the lexer enters regex
    /// mode after emitting this).
    EqualTilde,
    /// `=>` -- case-arm separator. Per scripture section 7.1.
    FatArrow,

    // === Arithmetic-ish operators (also used by some non-arith forms) ======
    Plus,      // +
    Minus,     // -
    Star,      // *  (in normal context this would be inside a Word; emitted
               //     standalone only when it's not adjacent to word chars)
    Slash,     // /
    Percent,   // %
    /// `^` -- string concatenation in normal context; bitwise-xor
    /// in arithmetic context. Per scripture section 6.7.
    Caret,
    /// `~` -- legacy `~` match operator (`~ $var pattern` per
    /// scripture section 7.4) OR bitwise-not in arithmetic context.
    Tilde,
    /// `!` -- logical-not OR part of `!=`. The `!=` is consumed in
    /// a two-char read, so a standalone `!` reaches this token only
    /// when the next char is not `=`.
    Bang,

    // === Fail-propagate ====================================================
    /// `?` -- postfix fail-propagate (per scripture section 8.2).
    /// The lexer emits this only when `?` is NOT inside a word (see
    /// scan_word's disambiguator in lexer.rs).
    Question,

    // === Reserved words ====================================================
    //
    // Per scripture section 5.1. The lexer scans these as Words
    // first then promotes them after the scan completes. This
    // means that quoted forms like `'fn'` remain SingleQuoted
    // (not Fn) -- which is the desired behavior; the rc convention
    // is that quoting suppresses any keyword interpretation.
    //
    Fn,
    Let,
    If,
    Else,
    Case,
    For,
    While,
    In,
    Try,
    Catch,
    Return,
    Break,
    Continue,
    On,
    Mask,
    Trace,

    // === End ===============================================================
    /// Synthetic; emitted once at end of token stream.
    Eof,
}

/// One piece of a double-quoted (or interpolating-heredoc) string.
///
/// The order of parts in the parent Vec is the order they appear in
/// source. Adjacent literal parts MAY be coalesced by the lexer for
/// efficiency, but a parser MUST tolerate them being split.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DqPart {
    /// Bytes between expansions. Escapes have been resolved per
    /// scripture section 6.5 (`\n`, `\t`, `\\`, `\"`, `\$` become
    /// the corresponding char; unknown `\X` preserves both `\` and
    /// `X` -- rc-style).
    Literal(String),
    /// `$name` expansion. String is the name (no leading `$`).
    Var(String),
    /// `$#name` expansion.
    VarLen(String),
    /// `$(cmd)` substitution. String is the raw `cmd` body.
    Subst(String),
}

impl Token {
    pub fn new(kind: TokenKind, span: Span) -> Self {
        Self { kind, span }
    }

    pub fn is_eof(&self) -> bool {
        matches!(self.kind, TokenKind::Eof)
    }
}

impl TokenKind {
    /// Look up the reserved-word kind for a candidate word, if any.
    /// Returns None when the word is NOT a reserved keyword per
    /// scripture section 5.1.
    pub fn reserved_word(s: &str) -> Option<TokenKind> {
        match s {
            "fn" => Some(TokenKind::Fn),
            "let" => Some(TokenKind::Let),
            "if" => Some(TokenKind::If),
            "else" => Some(TokenKind::Else),
            "case" => Some(TokenKind::Case),
            "for" => Some(TokenKind::For),
            "while" => Some(TokenKind::While),
            "in" => Some(TokenKind::In),
            "try" => Some(TokenKind::Try),
            "catch" => Some(TokenKind::Catch),
            "return" => Some(TokenKind::Return),
            "break" => Some(TokenKind::Break),
            "continue" => Some(TokenKind::Continue),
            "on" => Some(TokenKind::On),
            "mask" => Some(TokenKind::Mask),
            "trace" => Some(TokenKind::Trace),
            _ => None,
        }
    }
}
