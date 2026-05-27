// libutopia::parser::error -- the single error type produced by the
// rc-shape parser stack (lexer at U-5a; parser/expression/pattern
// layers at U-5b/c/d).
//
// Per UTOPIA-SHELL-DESIGN.md section 19 open question #2: v1.0 picks
// single-error-stop. The lexer / parser report ONE diagnostic and
// halt; the shell main loop (U-6) prints it and redraws the prompt.
// Panic-mode recovery + multi-error collection is a v1.x decision
// once we have a feel for how often users want to see all errors at
// once vs the first one.
//
// `ParseError` is the type the lexer + parser both produce. The
// `kind` enum covers lexer-level lex failures here at U-5a; later
// chunks extend it with parser-level grammar failures. The `span`
// pins the offending source location; the shell main loop uses it
// to underline the bad byte range during interactive use.

use alloc::string::String;
use core::fmt;

use super::span::Span;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ParseError {
    pub kind: ParseErrorKind,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ParseErrorKind {
    // === Lexer-level diagnostics (U-5a) ===
    /// A `'` opened but no closing `'` was found before EOF.
    UnterminatedSingleQuote,
    /// A `"` opened but no closing `"` was found before EOF.
    UnterminatedDoubleQuote,
    /// `$(` opened but the matching `)` was not found before EOF.
    UnterminatedSubstitution,
    /// `` `{ `` opened but the matching `}` and closing `` ` `` were
    /// not found.
    UnterminatedBacktick,
    /// `<(` or `>(` opened but the matching `)` was not found.
    UnterminatedProcSub,
    /// `<<TAG` opened but no terminator line containing exactly
    /// `TAG` (after optional leading-tab strip) appeared before EOF.
    UnterminatedHeredoc { tag: String },
    /// `<<"TAG` opened but the matching closing `"` was not found
    /// before end-of-line.
    UnterminatedHeredocTag,
    /// `=~ /pat` opened but the matching `/` was not found before
    /// EOF or newline.
    UnterminatedRegex,
    /// `$` was followed by no name at top-level scan position.
    EmptyVarName,
    /// `$#` with no name following.
    EmptyVarLenName,
    /// `$"` with no name following.
    EmptyVarNoSplitName,
    /// `<<` was emitted but the next non-whitespace char wasn't a
    /// valid heredoc tag start (alphanumeric, `_`, `-`, or `"`).
    InvalidHeredocStart,
    /// A control char / non-recognized byte appeared at top-level
    /// dispatch. Most legal characters (alphanumeric, operators,
    /// quotes, ...) get explicit treatment; this is the
    /// last-resort catch-all.
    UnexpectedChar(char),
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match &self.kind {
            ParseErrorKind::UnterminatedSingleQuote => {
                f.write_str("unterminated single-quoted string")
            }
            ParseErrorKind::UnterminatedDoubleQuote => {
                f.write_str("unterminated double-quoted string")
            }
            ParseErrorKind::UnterminatedSubstitution => {
                f.write_str("unterminated $(...) substitution")
            }
            ParseErrorKind::UnterminatedBacktick => {
                f.write_str("unterminated `{...} substitution")
            }
            ParseErrorKind::UnterminatedProcSub => {
                f.write_str("unterminated process substitution")
            }
            ParseErrorKind::UnterminatedHeredoc { tag } => {
                write!(f, "unterminated heredoc (expected `{}` line)", tag)
            }
            ParseErrorKind::UnterminatedHeredocTag => {
                f.write_str("unterminated heredoc tag (missing closing `\"`)")
            }
            ParseErrorKind::UnterminatedRegex => f.write_str("unterminated regex literal"),
            ParseErrorKind::EmptyVarName => f.write_str("empty variable name after `$`"),
            ParseErrorKind::EmptyVarLenName => f.write_str("empty variable name after `$#`"),
            ParseErrorKind::EmptyVarNoSplitName => f.write_str("empty variable name after `$\"`"),
            ParseErrorKind::InvalidHeredocStart => f.write_str("invalid heredoc start after `<<`"),
            ParseErrorKind::UnexpectedChar(c) => write!(f, "unexpected character `{}`", c),
        }
    }
}

pub type ParseResult<T> = core::result::Result<T, ParseError>;
