// libutopia::parser::ast -- the rc-shape AST produced by the U-5b
// recursive-descent parser and consumed by the U-6 evaluator.
//
// === Design intent ===
//
// The AST is enum-of-structs (standard Rust idiom). Each large variant
// is boxed where the data is genuinely larger than a few words so the
// `StatementKind` discriminant stays small. Every node carries a
// `Span` so the U-6 evaluator can attach runtime errors to source
// locations.
//
// === Expression contexts at U-5b ===
//
// Per scripture section 19 + the U-5b handoff (memory/project_next_
// session.md): U-5b is statement-level grammar. Expression contexts
// (condition of `if (...)`, list expression of `for (... in ...)`,
// condition of `while (...)`, value of `let x = ...`, value of bare
// assignment, return value, arithmetic body of `(( ... ))`) are
// stored as RAW `Vec<Token>` placeholders. The U-5c expression
// parser walks those into proper Expression trees later.
//
// This trade keeps U-5b focused on statement structure (the meat of
// the parse) while leaving expression semantics for the dedicated
// U-5c chunk. The runtime evaluator (U-6) can stub expression
// evaluation against the placeholder Vec<Token> for early bring-up;
// U-5c lifts that to a proper Expression node.
//
// === Deferred to U-5d ===
//
// Per scripture section 19: case (pattern matching), try/catch,
// trace, on note, mask note. These constructs use the rc-shape
// grammar's most distinctive features (case arms with `=>` separators,
// match operators inside expressions); they belong with U-5d's
// pattern-matching pass.
//
// === Span discipline ===
//
// Every node carries its source span. For statements, the span covers
// from the first token of the statement through the last (e.g., for
// `if (cond) { body }`, the span runs from the `if` keyword through
// the closing `}`). For compound nodes, the span covers the whole
// construct including delimiters.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use super::span::Span;
use super::token::{DqPart, Token};

// ---------------------------------------------------------------------
// Top-level
// ---------------------------------------------------------------------

/// A parsed rc-shape script -- the top-level AST node produced by
/// `parse(&str) -> Script`.
///
/// The `statements` vector holds the statements in source order.
/// Empty scripts (zero statements) are valid; the evaluator treats
/// them as no-ops.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Script {
    pub statements: Vec<Statement>,
    pub span: Span,
}

// ---------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Statement {
    pub kind: StatementKind,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum StatementKind {
    /// A pipeline (one or more commands joined by `|` or `?|`,
    /// optionally backgrounded with `&`).
    Pipeline(Pipeline),
    /// `if (cond) { body } else if (cond) { body } else { body }`
    If(Box<IfStmt>),
    /// `for (var in expr) { body }`
    For(Box<ForStmt>),
    /// `while (cond) { body }`
    While(Box<WhileStmt>),
    /// `fn name { body }` or `fn name args { body }`
    FnDecl(Box<FnDecl>),
    /// `let name = value`
    Let(Box<LetStmt>),
    /// `name = value` (bare assignment; no `let`)
    Assign(Box<AssignStmt>),
    /// `return` optionally followed by an expression.
    /// At U-5b the value is stored as raw `Vec<Token>`; U-5c parses
    /// it into an Expression.
    Return(Option<Vec<Token>>),
    /// `break`
    Break,
    /// `continue`
    Continue,
    // === Deferred to U-5d ===
    // Case(Box<CaseStmt>)
    // Try(Box<TryStmt>)
    // Trace(Vec<Statement>)
    // OnNote(Box<OnNoteStmt>)
    // MaskNote(Box<MaskNoteStmt>)
}

// ---------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------

/// A pipeline: a sequence of commands joined by `|` or `?|`, with an
/// optional trailing `&` background suffix.
///
/// Per scripture section 8.4: `?|` is "the preceding command's exit
/// is ignored for pipefail purposes" -- it sets the `tolerate_failure`
/// flag on the LEFT element of the pipe (i.e., the element BEFORE
/// the `?|`). A `|` (plain pipe) sets no tolerate flag.
///
/// Per scripture section 10.5: `&` backgrounds the WHOLE pipeline,
/// not just the rightmost command. The parser sets `background: true`
/// when it consumes a trailing `Ampersand` token after the pipeline.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Pipeline {
    pub elements: Vec<PipelineElement>,
    pub background: bool,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PipelineElement {
    pub command: Command,
    /// `?|` preceded this element's command? (i.e., this command's
    /// non-zero exit doesn't count toward pipefail per scripture
    /// section 8.4).
    pub tolerate_failure: bool,
    pub span: Span,
}

// ---------------------------------------------------------------------
// Command
// ---------------------------------------------------------------------

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Command {
    pub kind: CommandKind,
    /// Redirects in source order. Per scripture, redirects can
    /// interleave with command words and apply to the whole command
    /// at runtime. The parser preserves source order.
    pub redirects: Vec<Redirect>,
    /// Postfix `?` per scripture section 8.2 -- if the command's
    /// exit is non-zero, the enclosing function returns with that
    /// exit code (implicit fail-propagate).
    pub fail_propagate: bool,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CommandKind {
    /// A simple command: argv as a sequence of Words.
    Simple(SimpleCommand),
    /// `{ stmts }` -- a brace block. Runs in the current shell
    /// (no fork).
    BraceBlock(Vec<Statement>),
    /// `( stmts )` -- a subshell. Forks per scripture section 5.6;
    /// the parent waits.
    Subshell(Vec<Statement>),
    /// `(( body ))` -- arithmetic expression context. The body is
    /// stored as raw tokens; U-5c parses it as arithmetic.
    Arith(Vec<Token>),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SimpleCommand {
    pub words: Vec<Word>,
    pub span: Span,
}

// ---------------------------------------------------------------------
// Word
// ---------------------------------------------------------------------

/// A word is one argv element. Most words are a single value-
/// producing token (Word, SingleQuoted, DoubleQuoted, Var, VarLen,
/// VarNoSplit, Subst, Backtick, ProcSubIn, ProcSubOut). The
/// `Concat` variant captures the `^` concatenation operator
/// (scripture section 6.7) joining two or more value-producing
/// tokens into a single argv element.
///
/// Per the U-5b design decision (locked in this session): `^` only
/// concats when the surrounding tokens are SPAN-ADJACENT (no
/// whitespace between them). `$a^$b` -> Concat; `$a ^ $b` -> three
/// separate words (with `Caret` as an unexpected token -> parser
/// error in command-arg position).
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Word {
    /// A single value-producing token.
    Single(Token),
    /// Span-adjacent value tokens joined by `^` (one or more
    /// `Caret` operators between them). The Vec holds the value
    /// tokens in order; the Caret tokens themselves are NOT in the
    /// AST (their presence is implicit in the Concat variant).
    Concat(Vec<Token>),
}

impl Word {
    /// The source span this word covers (from the first token's
    /// start through the last token's end).
    pub fn span(&self) -> Span {
        match self {
            Word::Single(t) => t.span,
            Word::Concat(parts) => {
                let first = parts.first().expect("Concat has >= 2 parts");
                let last = parts.last().expect("Concat has >= 2 parts");
                Span::new(first.span.start, last.span.end)
            }
        }
    }
}

// ---------------------------------------------------------------------
// Redirect
// ---------------------------------------------------------------------

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Redirect {
    pub kind: RedirectKind,
    /// The redirect's target word. `Some(word)` for Stdin / Stdout /
    /// Append (the file path); `None` for Heredoc (the body is
    /// inline in `kind`).
    pub target: Option<Word>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum RedirectKind {
    /// `< file`
    Stdin,
    /// `> file`
    Stdout,
    /// `>> file`
    Append,
    /// `<< TAG` (or `<<-TAG` or `<<"TAG"`). The body comes from the
    /// matching `HeredocBody` token in the source -- the parser
    /// pre-scans the token stream to pair HeredocStart with
    /// HeredocBody and inlines the body parts here.
    Heredoc {
        interp: bool,
        strip_tabs: bool,
        body: Vec<DqPart>,
    },
}

// ---------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IfStmt {
    /// Condition tokens (between the `(` and `)` of the `if`).
    /// At U-5b: raw tokens; U-5c parses into Expression.
    pub cond: Vec<Token>,
    pub then_branch: Vec<Statement>,
    /// `else if (cond2) { body2 }` chain. Each entry is (cond_tokens,
    /// body) for one elif arm. Empty Vec means no elif.
    pub elif_branches: Vec<(Vec<Token>, Vec<Statement>)>,
    /// `else { body }` at the end. `None` means no else.
    pub else_branch: Option<Vec<Statement>>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ForStmt {
    /// The loop variable name (bare identifier from `for (NAME in ...)`).
    pub var_name: String,
    /// The list expression tokens (between `in` and `)`).
    /// At U-5b: raw tokens; U-5c parses into Expression.
    pub list_expr: Vec<Token>,
    pub body: Vec<Statement>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WhileStmt {
    /// Condition tokens (between the `(` and `)` of the `while`).
    /// At U-5b: raw tokens; U-5c parses into Expression.
    pub cond: Vec<Token>,
    pub body: Vec<Statement>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FnDecl {
    pub name: String,
    /// Optional positional argument names per scripture section 5.5
    /// (the `fn name args { body }` form). Empty Vec when no args
    /// are declared (`fn name { body }`).
    pub args: Vec<String>,
    pub body: Vec<Statement>,
    pub span: Span,
}

// ---------------------------------------------------------------------
// Assignment
// ---------------------------------------------------------------------

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LetStmt {
    pub name: String,
    /// Value tokens (everything after `=` up to the statement
    /// terminator). At U-5b: raw tokens; U-5c parses into Expression.
    pub value: Vec<Token>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AssignStmt {
    pub name: String,
    /// Value tokens (everything after `=` up to the statement
    /// terminator). At U-5b: raw tokens; U-5c parses into Expression.
    pub value: Vec<Token>,
    pub span: Span,
}
