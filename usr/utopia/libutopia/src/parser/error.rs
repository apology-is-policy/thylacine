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

    // === Parser-level diagnostics (U-5b) ============================
    //
    // The parser produces these when token-stream structure violates
    // the rc-shape grammar. Each carries a span pinning the
    // offending token range.
    //
    /// An unexpected token appeared where the parser was expecting a
    /// specific construct. `expected` is a short human-readable
    /// label (e.g., "`(`", "identifier", "`{` or pipeline").
    UnexpectedToken { expected: &'static str },
    /// The token stream ended mid-construct (after an `if (`, after
    /// `let`, etc.). `expected` describes what was being parsed.
    UnexpectedEof { expected: &'static str },
    /// A bare identifier was required (after `let`, after `fn`, as
    /// the loop variable in `for (NAME in ...)`) but the token was
    /// something else (a keyword, an operator, a quoted string, ...).
    ExpectedIdent,
    /// Empty pipeline element per scripture section 17.10: `cmd1 |
    /// | cmd2` -- the parser refuses to accept a pipe with no
    /// command on at least one side.
    EmptyPipelineElement,
    /// `cmd?` was followed by something other than a statement
    /// terminator. Per the U-5b design lock: postfix `?` requires a
    /// terminator (Newline / Semicolon / Pipe / PipeTolerate /
    /// Ampersand / RBrace / RParen / EOF).
    UnexpectedTokenAfterFailPropagate,
    /// The parser found a token that doesn't fit any statement
    /// production (e.g., a bare RBrace at top level, or a stray
    /// FatArrow outside a case arm).
    InvalidStatement,
    /// An assignment was attempted with a name that isn't a valid
    /// identifier ([a-zA-Z_][a-zA-Z0-9_]*).
    InvalidAssignmentName,
    /// `=` appeared in command-arg position. Per the U-5b design
    /// lock (strict rc): `--key=value` is a parse error; users
    /// quote.
    UnexpectedEqualInCommand,
    /// A `^` (Caret) appeared where Concat semantics couldn't apply
    /// -- either the surrounding tokens aren't span-adjacent
    /// (`$a ^ $b`) or one side isn't a value-producing token. Per
    /// the U-5b design lock (rc-strict): `^` requires span
    /// adjacency to both sides.
    UnexpectedCaret,
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

            ParseErrorKind::UnexpectedToken { expected } => {
                write!(f, "unexpected token; expected {}", expected)
            }
            ParseErrorKind::UnexpectedEof { expected } => {
                write!(f, "unexpected end of input; expected {}", expected)
            }
            ParseErrorKind::ExpectedIdent => f.write_str("expected an identifier"),
            ParseErrorKind::EmptyPipelineElement => {
                f.write_str("empty pipeline element (a `|` or `?|` with no command on one side)")
            }
            ParseErrorKind::UnexpectedTokenAfterFailPropagate => {
                f.write_str("unexpected token after postfix `?`; expected a statement terminator")
            }
            ParseErrorKind::InvalidStatement => f.write_str("invalid statement"),
            ParseErrorKind::InvalidAssignmentName => f.write_str("invalid assignment name"),
            ParseErrorKind::UnexpectedEqualInCommand => {
                f.write_str("unexpected `=` in command-arg position; quote it or use spaces around `=` for assignment")
            }
            ParseErrorKind::UnexpectedCaret => {
                f.write_str("unexpected `^`; concat requires both sides span-adjacent to value tokens")
            }
        }
    }
}

pub type ParseResult<T> = core::result::Result<T, ParseError>;
