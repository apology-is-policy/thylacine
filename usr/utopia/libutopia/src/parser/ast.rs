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
// === Expression contexts (U-5c) ===
//
// Per scripture section 19 + the U-5c session lift: every expression
// context in a statement is fully parsed at construction time. The
// statement parser (parse.rs) collects the raw token stream that
// belongs to an expression context (between `(` and `)` for an `if`
// condition, between `=` and the statement terminator for an
// assignment, etc.), then immediately calls into `expr::parse_expr
// _tokens()` to lift those tokens into a proper `Expr` tree. The
// AST therefore has ONE canonical shape: every expression slot is
// an `Expr`, never a raw `Vec<Token>` placeholder.
//
// Eager parsing trades a slightly larger parse-time cost for AST
// uniformity. The evaluator (U-6) sees a single AST shape; spec /
// audit / refactor work is simpler. The cost is small because
// expressions are typically short, and parse errors surface at
// parse time (not at first execution).
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
// the closing `}`). For expressions, the span covers from the first
// constituent token through the last. For compound nodes, the span
// covers the whole construct including delimiters.

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
    /// `return` optionally followed by an expression (U-5c-lifted).
    Return(Option<Expr>),
    /// `break`
    Break,
    /// `continue`
    Continue,
    /// `case $x { pat1 pat2 => body ; pat3 => body ; * => body }` -- the
    /// statement form of pattern matching (scripture 7.1). First match
    /// wins; no fallthrough. Each arm's body is one Statement (use a
    /// BraceBlock for multi-statement arms).
    Case(Box<CaseStmt>),
    /// `try { body } catch { catch_body }` -- error handling
    /// (scripture 7.7). The catch block runs if any command in the
    /// try body exits non-zero. `$errstr` is set by the evaluator
    /// before the catch runs.
    Try(Box<TryStmt>),
    /// `trace { body }` -- block-scoped tracing (scripture 7.8).
    /// Each command in the body is printed to stderr before execution
    /// (set -x equivalent, scoped to the block).
    Trace(Box<TraceStmt>),
    /// `on note 'name' { body }` -- note handler registration
    /// (scripture 9.5 + 10.7). The handler runs in the main poll
    /// loop, not in an async signal context.
    OnNote(Box<OnNoteStmt>),
    /// `mask note 'name' { body }` -- note suppression block
    /// (scripture 9.5 + 10.8). Notes are deferred while the body
    /// runs; the registered handler (if any) fires immediately
    /// after the block exits.
    MaskNote(Box<MaskNoteStmt>),
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
    /// `(( body ))` -- arithmetic expression context. Body lifted to
    /// `Expr` (U-5c).
    Arith(Expr),
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
///
/// Word at the command-argv level remains token-shaped at U-5c -- it
/// is fed through glob/expansion at evaluation time. The Expr-level
/// concat (inside expression contexts) is `ExprKind::Concat` below.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Word {
    /// A single value-producing token.
    Single(Token),
    /// Span-adjacent value tokens fused into one word. Two fusers:
    /// the `^` operator (one or more `Caret` operators between the
    /// values; the Caret tokens are NOT in the AST -- their presence
    /// is implicit), and a `~` (which glues its span-adjacent
    /// neighbours so `~/foo` and `a~b` are each one word; the `Tilde`
    /// tokens ARE in the AST so eval can expand a *leading* `~` to
    /// $home while keeping a non-leading `~` literal). The Vec holds
    /// the value tokens in source order.
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
    /// Condition expression -- `Expr` (U-5c lifted from `Vec<Token>`).
    pub cond: Expr,
    pub then_branch: Vec<Statement>,
    /// `else if (cond2) { body2 }` chain. Each entry is (cond_expr,
    /// body) for one elif arm. Empty Vec means no elif.
    pub elif_branches: Vec<(Expr, Vec<Statement>)>,
    /// `else { body }` at the end. `None` means no else.
    pub else_branch: Option<Vec<Statement>>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ForStmt {
    /// The loop variable name (bare identifier from `for (NAME in ...)`).
    pub var_name: String,
    /// The list expression -- `Expr` (U-5c lifted; typically a Var,
    /// a List, or a Concat).
    pub list_expr: Expr,
    pub body: Vec<Statement>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WhileStmt {
    /// Condition expression -- `Expr` (U-5c lifted).
    pub cond: Expr,
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
    /// Value expression -- `Expr` (U-5c lifted).
    pub value: Expr,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AssignStmt {
    pub name: String,
    /// Value expression -- `Expr` (U-5c lifted).
    pub value: Expr,
    pub span: Span,
}

// ---------------------------------------------------------------------
// Pattern matching, try/catch, trace, note handlers (U-5d)
// ---------------------------------------------------------------------

/// `case $x { pat => body ... }` -- the statement form. Each arm's
/// body is one Statement; users wrap multi-statement bodies in a
/// brace block.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CaseStmt {
    /// The scrutinee: the value being matched. Parsed as a
    /// `Value`-context Expr -- typically a Var, SingleQuoted, or
    /// Concat.
    pub scrutinee: Expr,
    pub arms: Vec<CaseArm>,
    pub span: Span,
}

/// One arm of a statement-form `case`. Multi-pattern arms are
/// permitted (scripture 7.1: "Multi-pattern per branch
/// (space-separated)").
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CaseArm {
    /// One or more patterns separated by whitespace. Each pattern is
    /// parsed as a `Value`-context Expr -- typically a Word with
    /// glob metas (`*.c`), a quoted literal, or a Concat
    /// (`*.$ext`). The `*` catch-all is just `Word("*")` and the
    /// evaluator's glob engine handles it as "match anything".
    pub patterns: Vec<Expr>,
    /// The arm body: one Statement. Use a brace block (`{ ... }`)
    /// for multi-statement bodies.
    pub body: Statement,
    pub span: Span,
}

/// `try { body } catch { catch }` (scripture 7.7). `catch` is
/// required; the parser emits `UnexpectedToken { expected: "`catch`" }`
/// if absent.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TryStmt {
    pub body: Vec<Statement>,
    pub catch: Vec<Statement>,
    pub span: Span,
}

/// `trace { body }` (scripture 7.8).
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TraceStmt {
    pub body: Vec<Statement>,
    pub span: Span,
}

/// `on note <name> { body }` (scripture 9.5 + 10.7). The note name
/// is parsed as a `Value`-context Expr so that `'snare:int'`,
/// `"snare:int"`, and bare `snare:int` all work; the evaluator
/// resolves to a string at registration time.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OnNoteStmt {
    pub note_name: Expr,
    pub body: Vec<Statement>,
    pub span: Span,
}

/// `mask note <name> { body }` (scripture 9.5 + 10.8). Same shape
/// as `OnNote`; the evaluator masks delivery of the named note
/// class while the body runs.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MaskNoteStmt {
    pub note_name: Expr,
    pub body: Vec<Statement>,
    pub span: Span,
}

/// `case $x { pat => value ... }` -- the expression form
/// (scripture 7.2). Distinct from `CaseStmt` because each arm
/// produces a single VALUE (Expr), not a Statement. Lives in
/// `ExprKind::Case` so the AST has ONE canonical expression shape.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CaseExpr {
    pub scrutinee: Box<Expr>,
    pub arms: Vec<CaseExprArm>,
    pub span: Span,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CaseExprArm {
    pub patterns: Vec<Expr>,
    /// The arm value: a `Value`-context Expr.
    pub value: Expr,
    pub span: Span,
}

// ---------------------------------------------------------------------
// Expression (U-5c)
// ---------------------------------------------------------------------

/// An expression node -- the result of `expr::parse_expr_tokens()`.
///
/// At U-5c every statement that previously held a `Vec<Token>` for
/// its expression slot now holds a proper `Expr`. The shape is
/// recursive: composite expressions (BinOp, UnOp, Match, List,
/// Concat, VarIndex, VarSlice, Subst, etc.) contain boxed children.
///
/// The `span` covers from the first token of the expression through
/// the last. For a single-atom expression (e.g., `Var($x)`), the
/// span is exactly the atom's span. For a binary operation
/// (`a + b`), the span covers `a`, the operator, and `b`.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

/// The kind of an expression node.
///
/// Atoms map directly to lexer tokens (Word -> ExprKind::Word, etc.)
/// with one exception: in arithmetic context, Word("42") lifts to
/// ExprKind::Integer(42).
///
/// Composite expressions are produced by the parser:
/// - `Concat(parts)` -- span-adjacent `^`-joined values in non-arith
///   contexts (scripture 6.7).
/// - `List(parts)` -- explicit `(a b c)` list literal in value
///   context (scripture 6.3).
/// - `BinOp/UnOp` -- arithmetic and logical operators (scripture
///   6.13 + 6.14 + 7.3).
/// - `Match` -- the `matches`/`in`/`=~` match operators (scripture
///   7.3).
/// - `VarIndex/VarSlice` -- `$var(N)` element / `$var(M N)` slice
///   (scripture 6.9).
/// - `Subst/Backtick/ProcSubIn/ProcSubOut` -- substitutions; body
///   eagerly parsed as a sub-Script at U-5c.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ExprKind {
    // === Atoms (single-token-ish) ===
    /// Bare word; an unquoted argv-like value. Glob meta chars
    /// preserved; the evaluator (U-6) does glob expansion.
    Word(String),
    /// `'literal'`. Single-quoted; literal text.
    SingleQuoted(String),
    /// `"interp"`. Body unchanged from the lexer.
    DoubleQuoted(Vec<DqPart>),
    /// Integer literal. Emitted only in arith context (or when an
    /// atom that began as a digit-only Word is consumed in an arith
    /// sub-expression).
    Integer(i64),
    /// `$name` -- list-valued variable reference.
    Var(String),
    /// `$#name` -- length of named variable.
    VarLen(String),
    /// `$"name` -- no-split form (scripture 6.8).
    VarNoSplit(String),
    /// `$var(N)` -- element index (1-indexed). N is itself an
    /// expression (parsed in arith context).
    VarIndex(String, Box<Expr>),
    /// `$var(M N)` -- inclusive slice. Both bounds are integer
    /// expressions.
    VarSlice(String, Box<Expr>, Box<Expr>),
    /// `$(cmd)` -- substitution; body parsed as a sub-Script.
    Subst(Box<Script>),
    /// `` `{cmd}` `` -- rc-shape substitution; same as Subst.
    Backtick(Box<Script>),
    /// `<(cmd)` -- process substitution (input).
    ProcSubIn(Box<Script>),
    /// `>(cmd)` -- process substitution (output).
    ProcSubOut(Box<Script>),
    /// `/pattern/` -- regex literal. The string is the body
    /// (escapes already resolved by the lexer). Semantics deferred
    /// to U-5d's regex engine.
    Regex(String),

    // === Composites ===
    /// `(a b c)` -- list literal. Used in value position
    /// (scripture 6.3). An empty list `()` is `List(vec![])`.
    List(Vec<Expr>),
    /// `a^b^c` -- span-adjacent `^`-joined concatenation in
    /// non-arith contexts (scripture 6.7). The Vec holds two or
    /// more value sub-expressions; a single-atom Concat collapses
    /// to that atom at parse time.
    Concat(Vec<Expr>),

    // === Operators ===
    /// `a OP b` -- binary operator. Arithmetic context exercises
    /// the full set; conditional context uses comparison and
    /// logical only.
    BinOp(BinOp, Box<Expr>, Box<Expr>),
    /// `OP a` -- unary operator (`- ~ !`).
    UnOp(UnOp, Box<Expr>),
    /// `a matches glob`, `a in list`, `a =~ /regex/` -- match
    /// operators (scripture 7.3). Right-hand side carries the
    /// glob pattern (Word/DoubleQuoted/SingleQuoted), the list
    /// (Var/List), or the regex literal.
    Match(MatchOp, Box<Expr>, Box<Expr>),

    // === Pattern matching as expression (U-5d) ===
    /// `case $x { pat => value ... }` -- the expression form
    /// (scripture 7.2). The chosen branch's `value` becomes the
    /// case expression's value. Distinct from `StatementKind::Case`
    /// because each arm produces a single Expr (not a Statement).
    Case(Box<CaseExpr>),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BinOp {
    // Arithmetic
    Add, // +
    Sub, // -
    Mul, // *
    Div, // /
    Mod, // %
    Pow, // **  (right-associative)
    // Bitwise (arith)
    BitAnd, // &
    BitOr,  // |
    BitXor, // ^
    Shl,    // <<
    Shr,    // >>
    // Comparison
    Lt, // <
    Le, // <=
    Gt, // >
    Ge, // >=
    Eq, // ==
    Ne, // !=
    // Logical
    And, // &&
    Or,  // ||
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UnOp {
    Neg,    // -x
    BitNot, // ~x  (arith)
    Not,    // !x  (logical)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MatchOp {
    /// `matches` -- glob match (scripture 7.3).
    Glob,
    /// `in` -- list membership (scripture 7.3).
    In,
    /// `=~` -- regex match (scripture 7.3); evaluator wiring at U-5d.
    Regex,
}

// ---------------------------------------------------------------------
// Context discriminator for the expression parser
// ---------------------------------------------------------------------

/// The syntactic context an expression is being parsed in. Determines
/// which operators are recognized and how certain tokens are
/// interpreted.
///
/// - `Value` / `Return`: a single value or a list literal. Top-level
///   operators are NOT recognized (use `(( ... ))` for math, use the
///   conditional context for booleans). Concat (`^`) and var
///   indexing are recognized.
/// - `Cond`: a boolean expression. Logical (`&& || !`), comparison
///   (`== != < <= > >=`), and match (`matches in =~`) operators are
///   recognized.
/// - `Arith`: a full arithmetic expression with all 18+ operators.
///   Integer-typed; vars auto-deref.
/// - `List`: the right-hand side of `for (var in expr)`. Accepts a
///   single value (Var/Concat/atom) or an implicit / explicit list.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExprContext {
    Value,
    Cond,
    Arith,
    List,
    Return,
}
