// libutopia::parser -- the rc-shape parser stack for the `ut` shell.
//
// Per UTOPIA-SHELL-DESIGN.md sections 5-9 + 19, the parser stack is
// the pure-logic engine that consumes a `&str` buffer (typically
// produced by the line editor in libutopia::line_editor on Enter)
// and emits an AST (planned). The U-5 sub-arc decomposition lifts
// the work into 4 sub-chunks:
//
//   U-5a (this commit) -- tokenizer (lexer.rs). Produces
//                          Vec<Token> with span tracking; covers
//                          words, strings, $-forms, substitutions,
//                          process subs, heredocs, regex literals,
//                          all operators + reserved words.
//
//   U-5b               -- parser core. Top-level command +
//                          pipeline + control-flow (if/for/while/
//                          fn/try/catch/case) grammar producing
//                          AST nodes.
//
//   U-5c               -- expression parser. Variable refs,
//                          interpolation, substitution context,
//                          concatenation, arithmetic.
//
//   U-5d               -- pattern matching + match operators +
//                          regex semantics + final glue.
//
// At U-5a the public surface is small: `tokenize()` + the types
// (Token, TokenKind, DqPart, Span, ParseError). U-5b extends this
// with AST types + a `parse()` entry; U-5c/d add expression and
// pattern nodes; U-5d completes the public surface.

pub mod error;
pub mod lexer;
pub mod span;
pub mod token;

pub use error::{ParseError, ParseErrorKind, ParseResult};
pub use lexer::tokenize;
pub use span::Span;
pub use token::{DqPart, Token, TokenKind};
