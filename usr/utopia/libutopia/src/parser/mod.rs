// libutopia::parser -- the rc-shape parser stack for the `ut` shell.
//
// Per UTOPIA-SHELL-DESIGN.md sections 5-9 + 19, the parser stack is
// the pure-logic engine that consumes a `&str` buffer (typically
// produced by the line editor in libutopia::line_editor on Enter)
// and emits an AST. The U-5 sub-arc decomposition lifts the work
// into 4 sub-chunks:
//
//   U-5a (LANDED)      -- tokenizer (lexer.rs). Produces
//                          Vec<Token> with span tracking; covers
//                          words, strings, $-forms, substitutions,
//                          process subs, heredocs, regex literals,
//                          all operators + reserved words.
//
//   U-5b (LANDED)      -- parser core (parse.rs + ast.rs). Top-level
//                          command + pipeline + control-flow
//                          grammar producing AST nodes; expression
//                          contexts deferred as Vec<Token>
//                          placeholders.
//
//   U-5c (this commit) -- expression parser (expr.rs). Lifts every
//                          Vec<Token> placeholder into a proper
//                          Expr tree. Pratt-style precedence
//                          climbing for arith; recursive substitution
//                          body lifting; full match-operator surface
//                          for cond context.
//
//   U-5d               -- pattern matching + match operators +
//                          regex semantics + final glue.
//
// At U-5c the public surface is complete for non-pattern-matching
// shell expressions. The AST now has ONE canonical shape: every
// expression slot is an `Expr` (no raw `Vec<Token>` placeholders
// remain).

pub mod ast;
pub mod error;
pub mod expr;
pub mod lexer;
pub mod parse;
pub mod span;
pub mod token;

pub use ast::{
    AssignStmt, BinOp, Command, CommandKind, Expr, ExprContext, ExprKind, FnDecl, ForStmt, IfStmt,
    LetStmt, MatchOp, Pipeline, PipelineElement, Redirect, RedirectKind, Script, SimpleCommand,
    Statement, StatementKind, UnOp, WhileStmt, Word,
};
pub use error::{ParseError, ParseErrorKind, ParseResult};
pub use expr::parse_expr_tokens;
pub use lexer::tokenize;
pub use parse::{parse, parse_tokens};
pub use span::Span;
pub use token::{DqPart, Token, TokenKind};
