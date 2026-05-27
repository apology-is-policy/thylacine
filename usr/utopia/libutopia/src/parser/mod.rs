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
//   U-5c (LANDED)      -- expression parser (expr.rs). Lifts every
//                          Vec<Token> placeholder into a proper
//                          Expr tree. Pratt-style precedence
//                          climbing for arith; recursive substitution
//                          body lifting; full match-operator surface
//                          for cond context.
//
//   U-5d (this commit) -- pattern matching + try/catch + trace +
//                          on note / mask note + case-as-expression.
//                          Closes the U-5 parser arc; the parser's
//                          public surface is now complete for U-6's
//                          evaluator to consume.
//
// At U-5d the public surface is complete. The AST has ONE canonical
// shape for both statements (every U-5b/U-5d statement kind is a
// `StatementKind` variant) and expressions (every U-5c/U-5d
// expression slot is an `Expr`, including `ExprKind::Case` for
// case-as-expression).

pub mod ast;
pub mod error;
pub mod expr;
pub mod lexer;
pub mod parse;
pub mod span;
pub mod token;

pub use ast::{
    AssignStmt, BinOp, CaseArm, CaseExpr, CaseExprArm, CaseStmt, Command, CommandKind, Expr,
    ExprContext, ExprKind, FnDecl, ForStmt, IfStmt, LetStmt, MaskNoteStmt, MatchOp, OnNoteStmt,
    Pipeline, PipelineElement, Redirect, RedirectKind, Script, SimpleCommand, Statement,
    StatementKind, TraceStmt, TryStmt, UnOp, WhileStmt, Word,
};
pub use error::{ParseError, ParseErrorKind, ParseResult};
pub use expr::parse_expr_tokens;
pub use lexer::tokenize;
pub use parse::{parse, parse_tokens};
pub use span::Span;
pub use token::{DqPart, Token, TokenKind};
