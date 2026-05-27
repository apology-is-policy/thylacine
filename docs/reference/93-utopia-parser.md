# `libutopia::parser` — the rc-shape parser stack for `ut` (U-5 arc)

**Status**: U-5a + U-5b + U-5c + U-5d LANDED — the U-5 parser arc is COMPLETE. The parser stack now covers the full rc-shape grammar: tokenizer (U-5a), statement-level grammar + AST (U-5b), expression parser with Pratt-style precedence climbing (U-5c), and pattern matching + try/catch + trace + on/mask + case-as-expression (U-5d). The parser's public surface is ready for U-6's evaluator to consume.

This is the as-built reference for the rc-shape parser used by the Utopia shell (`ut`). It lives at `usr/utopia/libutopia/src/parser/` and is the only path from "user typed a line" to "AST evaluator consumes". The line editor (U-4 arc) produces an `EditorAction::Accept(String)` when Enter is pressed on a balanced buffer; that string is fed to `tokenize()` here, then (U-5b onward) to `parse()` to produce an AST, then (U-6) to the evaluator.

The parser is a **pure-logic engine**. No I/O. No syscalls. Host-testable via `#[cfg(test)]` unit tests; runtime-validated via the `flow_parser_lexer` probe in `/u-test`. Same factoring as the line editor: a clean separation between the algorithm and its substrate.

---

## 1. Purpose

The parser stack consumes a Rust `&str` containing rc-shape source (per `docs/UTOPIA-SHELL-DESIGN.md` sections 5-9) and produces an AST. At U-5a, it produces a `Vec<Token>` — the tokenizer's output, the input to U-5b's recursive-descent parser.

The token taxonomy is opinionated: most syntactic surfaces get their own kind, but where lex-time disambiguation is genuinely impossible without parser context (e.g., `^` is concatenation in command position and bitwise-xor in arithmetic), the lexer emits the same token and lets the parser decide.

## 2. Public API (U-5a + U-5b)

```rust
// usr/utopia/libutopia/src/parser/mod.rs

pub mod ast;
pub mod error;
pub mod lexer;
pub mod parse;
pub mod span;
pub mod token;

// Re-exports
pub use ast::{
    AssignStmt, Command, CommandKind, FnDecl, ForStmt, IfStmt, LetStmt, Pipeline,
    PipelineElement, Redirect, RedirectKind, Script, SimpleCommand, Statement, StatementKind,
    WhileStmt, Word,
};
pub use error::{ParseError, ParseErrorKind, ParseResult};
pub use lexer::tokenize;
pub use parse::{parse, parse_tokens};
pub use span::Span;
pub use token::{DqPart, Token, TokenKind};
```

```rust
// U-5a tokenizer entry
pub fn tokenize(source: &str) -> ParseResult<Vec<Token>>;

// U-5b parser entries
pub fn parse(source: &str) -> ParseResult<Script>;
pub fn parse_tokens(tokens: Vec<Token>, source_len: usize) -> ParseResult<Script>;
```

**Contract** of `tokenize`:
- Returns `Ok(Vec<Token>)` ending with a synthetic `TokenKind::Eof` token whose span is the point `(source.len(), source.len())`.
- Returns `Err(ParseError)` on the first lex error (single-error-stop per `UTOPIA-SHELL-DESIGN.md` section 19 open question #2).
- Pure function: no global state; thread-safe; idempotent.
- UTF-8 aware: non-ASCII bytes are treated as word chars and preserved verbatim in `Word` / `SingleQuoted` / `DoubleQuoted::Literal` content.
- Every token carries a `Span` — byte offsets (not char offsets) into `source` such that `&source[span.start..span.end]` always returns valid UTF-8.

## 3. Token taxonomy

Lives at `usr/utopia/libutopia/src/parser/token.rs`. Every variant is documented inline; the major groupings:

**Literals** (concrete value-bearing tokens):
- `Word(String)` — bare word; may include glob meta chars (`*`, `?`, `[`, `]`, `**`); the `?` glyph is a word char only when followed by another word char (the standalone `cmd?` case yields `Word("cmd") + Question`).
- `SingleQuoted(String)` — `'literal'`; the only escape is the rc-style doubled quote `''`.
- `DoubleQuoted(Vec<DqPart>)` — `"interpolating"`; pre-parsed into parts during lex.

**Top-level variable refs**:
- `Var(String)` — `$name`.
- `VarLen(String)` — `$#name` (list length).
- `VarNoSplit(String)` — `$"name` (rc's clean answer to bash's `"$@"` per scripture section 6.8).

**Substitutions** (body stored as RAW source; re-tokenized by the parser when it descends):
- `Subst(String)` — `$(cmd)` POSIX-shape.
- `Backtick(String)` — `` `{cmd}` `` rc-shape (requires both `{` ... `}` AND the closing backtick).

**Process substitution**:
- `ProcSubIn(String)` — `<(cmd)`.
- `ProcSubOut(String)` — `>(cmd)`.

**Heredocs** (two-position constructs collected by the lexer):
- `HeredocStart { tag, interp, strip_tabs }` — `<<TAG`, `<<-TAG`, or `<<"TAG"`.
- `HeredocBody(Vec<DqPart>)` — emitted immediately after the newline that follows the `HeredocStart`; body parsed with `DqPart` shape when `interp == true`, single literal when `interp == false`.

**Regex literal** (after `=~`):
- `Regex(String)` — `/pattern/`; one-shot lex mode triggered by emitting `EqualTilde`; `\/` -> `/` escape resolved.

**Punctuation**:
- `LBrace` `RBrace` `LParen` `RParen` `DoubleLParen` `DoubleRParen` `Semicolon` `Newline`.

**Pipe + logical**:
- `Pipe` `PipeTolerate` (`?|` per section 8.4) `AndAnd` `OrOr` `Ampersand`.

**Redirects**:
- `Less` `Greater` `GreaterGreater`. (`<<` is consumed as `HeredocStart` if a valid tag follows; arith shift-left is deferred to U-5c.)

**Comparison / assignment**:
- `Equal` `DoubleEqual` `NotEqual` `LessEqual` `GreaterEqual` `EqualTilde` `FatArrow`.

**Arithmetic-ish** (also used by some non-arith forms):
- `Plus` `Minus` `Star` `Slash` `Percent` `Caret` `Tilde` `Bang`. (The lexer doesn't enter arith mode at U-5a; the parser at U-5c will re-interpret these in `(( ... ))` context.)

**Fail-propagate**:
- `Question` — postfix on a command per scripture section 8.2.

**Reserved words** (per scripture section 5.1):
- `Fn` `Let` `If` `Else` `Case` `For` `While` `In` `Try` `Catch` `Return` `Break` `Continue` `On` `Mask` `Trace`.

**End**:
- `Eof` — synthetic; emitted once at end of stream.

### `DqPart` — double-quoted string interior

```rust
pub enum DqPart {
    Literal(String),    // bytes between expansions; escapes resolved
    Var(String),        // $name expansion
    VarLen(String),     // $#name expansion
    Subst(String),      // $(cmd) substitution; raw body
}
```

Per scripture section 6.5, double-quoted strings recognize:
- `\n`, `\t`, `\\`, `\"`, `\$` as escapes (resolved to corresponding chars; `\X` for unknown X preserves both `\` and `X` verbatim — rc-style).
- `$var`, `$#var`, `$(cmd)` as interpolations.
- `\<newline>` as line continuation (consume both).
- `` `{cmd}` `` is NOT recognized inside `"..."` — the backtick is a literal there.
- `$"name` (no-split form) is NOT recognized inside `"..."` — the `"` closes the string.

## 4. Lexer state machine

Lives at `usr/utopia/libutopia/src/parser/lexer.rs`. Single-pass; recursive-descent-style top-level dispatcher; UTF-8-aware byte walker.

### 4.1 State

```rust
struct Lexer<'a> {
    source: &'a str,
    bytes: &'a [u8],
    pos: usize,
    tokens: Vec<Token>,
    pending_heredocs: Vec<HeredocSpec>,
    expect_regex_after: bool,
}

struct HeredocSpec {
    tag: String,
    interp: bool,
    strip_tabs: bool,
    start_span: Span,
}
```

- `pos` is the byte offset into `source`. Always advances by whole UTF-8 chars (never lands at a continuation byte).
- `pending_heredocs` is the FIFO queue of heredoc bodies awaiting collection. Each `<<TAG` adds an entry; each `\n` drains the queue.
- `expect_regex_after` is the one-shot lex-mode flag set by emitting `EqualTilde`; consumed by the next opening `/`.

### 4.2 Dispatch loop

The top-level `run()` loop:

1. Skip whitespace + line-continuation (`\<newline>`) + comments (`#` to EOL; the `\n` stays).
2. If at EOF: drain any remaining `pending_heredocs` (an error if non-empty — the start was orphaned), emit synthetic `Eof`, return.
3. If `expect_regex_after` and next byte is `/`: clear the flag, call `scan_regex()`, continue.
4. Else dispatch on the next byte. The dispatcher covers all explicit operator bytes, quotes, dollar, backtick; falls through to `scan_word()` for word chars (including backslash-escaped leaders); errors with `UnexpectedChar` on control chars / unrecognized bytes.

### 4.3 Word scan (`scan_word`)

- Continues while next byte is a word char OR `?` followed by a word char.
- Backslash escapes: `\<char>` includes `<char>` literally (so `\$path` is `Word("$path")`); `\<newline>` is line continuation (consumes both, continues word).
- Stops at the first separator (whitespace, `;`, `|`, `&`, `<`, `>`, `(`, `)`, `{`, `}`, `=`, `^`, `~`, `'`, `"`, `$`, `` ` ``, `#`, `!`, `?` at word-end).
- After scan, the text is matched against the reserved-word table; a hit emits the keyword token; a miss emits `Word(text)`.

### 4.4 String scanning

`scan_single_quoted`: scan until next `'`; doubled `''` resolves to a single `'`. Unterminated → `ParseErrorKind::UnterminatedSingleQuote`.

`scan_double_quoted`: two-phase. First, `find_dq_closing` locates the matching `"` while respecting `\"` escapes and `$(...)` body skipping. Then `parse_dq_or_heredoc_body` parses the interior into a `Vec<DqPart>`.

`parse_dq_or_heredoc_body` (shared between DQ-string and interp-heredoc-body):
- Walks bytes, resolving escapes (`\n \t \\ \" \$` + `\<nl>` continuation) and recognizing `$var` / `$#var` / `$(cmd)` interpolations.
- Coalesces literal runs into single `DqPart::Literal` entries.

### 4.5 Dollar forms (`scan_dollar`)

Dispatches on the byte after `$`:
- `#` → `VarLen(name)`.
- `"` → `VarNoSplit(name)`.
- `(` → `Subst(body)`; body via `find_balanced_paren_close`.
- name-start char (alphanumeric or `_`) → `Var(name)`.
- otherwise → `EmptyVarName` error.

### 4.6 Backtick form (`scan_backtick`)

Per scripture section 6.6: `` `{cmd}` ``. Requires `` ` `` `{` ... `}` `` ` ``. The `{...}` body uses `find_balanced_brace_close`. Closing backtick required — no closing `` ` `` after the `}` yields `UnterminatedBacktick`.

### 4.7 Heredoc start + body collection

`scan_heredoc_start` (triggered by `<<`):
- Optional `-` flag sets `strip_tabs`.
- Optional `"TAG"` wrap sets `interp = false`.
- Tag is alphanumeric + `_`.
- Pushes a `HeredocSpec` onto `pending_heredocs`; emits `HeredocStart` with the tag/flags.

`collect_heredoc_body` (triggered by `Newline`):
- Walks lines from current `pos` until a line matches `spec.tag` (after optional leading-tab strip if `spec.strip_tabs`).
- Body is the bytes from start-of-collection through the byte just before the terminator line.
- Body parsed via `parse_dq_or_heredoc_body` (interp) or wrapped in a single `Literal` (non-interp).
- Emits `HeredocBody(parts)`.
- Advances `pos` past the terminator's newline.

### 4.8 Regex literal (`scan_regex`)

Triggered when `expect_regex_after` is set and the next byte is `/`:
- Scans until next unescaped `/`.
- `\/` resolves to literal `/`; other `\X` is preserved verbatim (the regex engine interprets).
- Newline before close → `UnterminatedRegex`.

## 5. Span discipline

Every emitted `Token` carries a `Span { start, end }` where:
- `start` is the byte offset of the first byte of the token in `source` (inclusive).
- `end` is the byte offset just past the last byte (exclusive).
- For compound tokens (`DoubleQuoted`, `Subst`, `Backtick`, `ProcSubIn/Out`, `HeredocStart`, `HeredocBody`, `Regex`), the span covers from the opening delimiter through the closing delimiter (inclusive of both).
- `Eof` carries a point span `(source.len(), source.len())`.

`DqPart` sub-spans are NOT tracked at U-5a. If U-5d needs them for finer-grained error reporting inside string content (e.g., underlining a bad `$` in a `"..."`), they'll be added then.

## 6. Helper utilities

The lexer relies on two free helpers (used both directly by the lexer and by `parse_dq_or_heredoc_body`):

- `find_balanced_paren_close(s, body_start) -> Option<usize>` — locates the `)` that closes a paren opened at `body_start - 1`. Tracks nesting; respects single- and double-quoted strings (parens inside `'...'` or `"..."` don't count); respects `\` escapes.
- `find_balanced_brace_close(s, body_start) -> Option<usize>` — same shape for `{...}`.

Both walk by byte position and return None on EOF without close. Used by `$(cmd)` (paren), `<(cmd)` / `>(cmd)` (paren), `` `{cmd}` `` (brace).

## 7. Error types

```rust
pub struct ParseError {
    pub kind: ParseErrorKind,
    pub span: Span,
}

pub enum ParseErrorKind {
    UnterminatedSingleQuote,
    UnterminatedDoubleQuote,
    UnterminatedSubstitution,
    UnterminatedBacktick,
    UnterminatedProcSub,
    UnterminatedHeredoc { tag: String },
    UnterminatedHeredocTag,
    UnterminatedRegex,
    EmptyVarName,
    EmptyVarLenName,
    EmptyVarNoSplitName,
    InvalidHeredocStart,
    UnexpectedChar(char),
}
```

Single-error-stop per scripture section 19 open question #2 — the lexer reports one diagnostic and halts. The `span` field pins the offending byte range so the shell main loop (U-6) can underline it.

## 8. Tests

### 8.1 Host unit tests (`#[cfg(test)]`)

`usr/utopia/libutopia/src/parser/lexer.rs::tests` — 49 tests covering:
- Empty input / whitespace / EOF semantics.
- Single and multi-word bare words; glob metas preserved in words.
- `?` disambiguation: word-internal vs operator vs `?|`.
- All operators (one-char and two-char): `| || && ?| & ; ` `= == != <= >= =~ =>` `< > >> ( ) (( ))` `^ ~ ?`.
- Single-quoted strings with `''` escape.
- Double-quoted strings with literal, `$var`, `$#var`, `$(cmd)` parts; escape resolution; nested substitutions with paren balancing.
- Top-level dollar forms (`$var`, `$#var`, `$"var`, `$(cmd)`); nested substitutions; quote-respecting paren counting.
- Backtick form `` `{cmd}` ``; unterminated error.
- Process substitution `<(cmd)` and `>(cmd)`.
- Heredocs: plain interp; quoted non-interp; strip-tabs; unterminated.
- Regex literal: basic; escaped `\/`; unterminated; non-regex `/path` stays as Word.
- Reserved word recognition (all 16 keywords); quoted keywords stay strings.
- Comment skip; line continuation join.
- Backslash escape inside word; UTF-8 in word + DQ string.
- Redirects; pipeline with tolerate; case-arm pattern with fat arrow.
- Span coverage on Word, DoubleQuoted, Subst, Eof point span.
- Full canonical command from scripture section 6.5.

### 8.2 Boot-time `flow_parser_lexer` probe

`usr/u-test/src/main.rs::flow_parser_lexer` — 7 probes runtime-validated at every boot under the `aarch64-unknown-none` target with `ThylaAlloc`:

1. Empty input emits only synthetic `Eof`.
2. Multi-word command + `Pipe` operator + `Eof`.
3. Reserved word `Let` recognition + `Equal` operator.
4. `DoubleQuoted` with literal+var+literal+subst parts (canonical from scripture section 6.5).
5. Heredoc body collection: `<<EOF` body `EOF` produces `HeredocStart` + `Newline` + `HeredocBody(Literal)`.
6. Regex literal after `=~` (one-shot lex mode).
7. Error case (`UnterminatedSingleQuote`) + span tracking on a basic Word + Eof point span.

The probe's job is to validate that the lexer's heap allocation paths (`Vec<Token>`, `String`, `Vec<DqPart>`) work under `ThylaAlloc` (the libthyla-rs allocator backed by `SYS_BURROW_ATTACH`) and that UTF-8 char-walking produces identical output to the host. A regression in either would surface here.

## 8.5 AST + parser core (U-5b)

The U-5b chunk adds the recursive-descent parser over the U-5a token stream and the AST node types it produces.

### AST overview

```rust
pub struct Script {
    pub statements: Vec<Statement>,
    pub span: Span,
}

pub enum StatementKind {
    Pipeline(Pipeline),
    If(Box<IfStmt>),
    For(Box<ForStmt>),
    While(Box<WhileStmt>),
    FnDecl(Box<FnDecl>),
    Let(Box<LetStmt>),
    Assign(Box<AssignStmt>),
    Return(Option<Vec<Token>>),
    Break,
    Continue,
    // Case / Try / Trace / OnNote / MaskNote deferred to U-5d
}

pub struct Pipeline {
    pub elements: Vec<PipelineElement>,
    pub background: bool,
    pub span: Span,
}

pub struct PipelineElement {
    pub command: Command,
    pub tolerate_failure: bool,  // ?| preceded this element
    pub span: Span,
}

pub struct Command {
    pub kind: CommandKind,
    pub redirects: Vec<Redirect>,
    pub fail_propagate: bool,  // postfix ?
    pub span: Span,
}

pub enum CommandKind {
    Simple(SimpleCommand),
    BraceBlock(Vec<Statement>),
    Subshell(Vec<Statement>),
    Arith(Vec<Token>),  // body stored as raw tokens for U-5c
}

pub enum Word {
    Single(Token),
    Concat(Vec<Token>),  // span-adjacent ^-joined value tokens
}

pub enum RedirectKind {
    Stdin, Stdout, Append,
    Heredoc { interp: bool, strip_tabs: bool, body: Vec<DqPart> },
}
```

Control-flow node types (`IfStmt` / `ForStmt` / `WhileStmt` / `FnDecl` / `LetStmt` / `AssignStmt`) all store their expression contexts as `Vec<Token>` for U-5c to walk into proper Expression trees. The condition tokens of `if (cond)`, the list expression of `for (var in expr)`, the value of `let x = expr`, etc. all use the same placeholder pattern.

### Heredoc body resolution

The lexer emits `HeredocStart` at the source position of the `<<TAG` and `HeredocBody` *after* the next `Newline`. The parser pre-extracts all body parts into a FIFO `VecDeque<Vec<DqPart>>` at construction time (taking ownership of the tokens via `core::mem::take` to avoid cloning). When the parser walks a redirect and encounters `HeredocStart`, it pops the next body from the queue and inlines it into `RedirectKind::Heredoc`. `HeredocBody` tokens themselves are treated as separators (skipped) during statement walking.

This makes the body available exactly where the consumer wants it — on the Redirect — without the AST or evaluator needing to walk forward in the token stream.

### Parser dispatch

`parse_statement` dispatches on the current token:

- `If` / `For` / `While` / `Fn` / `Let` / `Return` / `Break` / `Continue` keywords → dedicated parsers
- `Word(IDENT)` followed by `Equal` (two-token lookahead) → assignment
- `Else` / `Catch` / `Case` / `Try` / `Trace` / `On` / `Mask` / `In` at statement-start → `InvalidStatement` error
- Otherwise → pipeline

`parse_pipeline` parses one or more `PipelineElement` joined by `|` or `?|`:
- `Pipe` is a plain join.
- `PipeTolerate` marks the LEFT element's `tolerate_failure = true` (per scripture section 8.4: "the preceding command's exit is ignored for pipefail purposes").

After the pipeline body, the parser checks for a trailing `&` → `background = true`.

`parse_command` dispatches by the first non-separator token:
- `LBrace` → BraceBlock
- `LParen` → Subshell (with statements parsed inside)
- `DoubleLParen` → Arith (body kept as raw tokens; `(( ` and `))` balanced with nested paren counting)
- Otherwise → SimpleCommand (argv + redirects)

After the command body, if a `Question` token follows, set `fail_propagate = true`. Per the U-5b lock: postfix `?` requires a statement terminator next (`Newline` / `;` / `|` / `?|` / `&` / `}` / `)` / EOF), otherwise `UnexpectedTokenAfterFailPropagate`.

`parse_simple_command` collects words + redirects in source order. Words can be `Word::Single(token)` or `Word::Concat(Vec<Token>)` — the Concat path is engaged when a span-adjacent `Caret` is followed by a span-adjacent value token. If `Equal` appears in command-arg position (after at least one word), the parser emits `UnexpectedEqualInCommand` (the U-5b strict-rc lock).

### Locked design decisions (this session)

| # | Decision | Behavior |
|---|---|---|
| 1 | `--key=value` arg | Strict rc: `UnexpectedEqualInCommand` parse error. Users quote: `'--key=value'`. |
| 2 | `^` concat | Requires span-adjacency on both sides. `$a ^ $b` (with spaces) is three separate tokens; the orphan Caret terminates the simple command and the outer terminator check errors. |
| 3 | `case` placement | Deferred to U-5d (per scripture section 19). U-5b emits `InvalidStatement` if `case` appears at statement-start. |
| 4 | Postfix `?` | Must be followed by a statement terminator. `cmd ? arg` is `UnexpectedTokenAfterFailPropagate`. |
| 5 | Empty pipeline element | Per scripture section 17.10: `cmd1 \| \| cmd2` is `EmptyPipelineElement` error. |
| 6 | Redirect placement | Anywhere in command (before / interleaved with / after words). All redirects stored in `Command.redirects` in source order. |
| 7 | `fn name args { ... }` | Args are space-separated bare-word idents between name and `{`. Strict positional only at v1.0. |
| 8 | Trailing `;` | Allowed. `cmd1;` parses as one statement. |
| 9 | List literal vs subshell | At U-5b, both `(a b c)` and `(stmt1; stmt2)` appearing in **value** position (right of `=`, inside `if (...)`, etc.) are stored as raw `Vec<Token>`; U-5c parses lists. `(...)` in **command** position is a Subshell. |
| 10 | Reserved keyword position | `'if'` (quoted) is SingleQuoted (not the If keyword token). Only the bare `If` token is the keyword. |

### Span discipline

Every AST node carries a `Span` from its first token's start through its last token's end. Statement spans include their terminating brace (for blocks). For the `Script` itself, the span is `(0, source.len())`.

### Implementation file map

```
usr/utopia/libutopia/src/parser/
├── mod.rs       extends with ast + parse re-exports
├── span.rs      unchanged from U-5a
├── token.rs     unchanged from U-5a
├── error.rs     extended with U-5b parser-level error kinds
├── lexer.rs     unchanged from U-5a
├── ast.rs       U-5b NEW: AST node types
└── parse.rs     U-5b NEW: recursive-descent parser
```

## 8.6 Expression parser (U-5c)

The U-5c chunk lifts every `Vec<Token>` placeholder left by U-5b into a proper `Expr` tree. After U-5c the AST has ONE canonical shape: every expression slot is an `Expr` (no raw `Vec<Token>` placeholders remain).

### AST extensions

```rust
pub struct Expr { pub kind: ExprKind, pub span: Span }

pub enum ExprKind {
    // Atoms
    Word(String) | SingleQuoted(String) | DoubleQuoted(Vec<DqPart>)
    | Integer(i64)        // arith context
    | Var(String) | VarLen(String) | VarNoSplit(String)
    | VarIndex(String, Box<Expr>)             // $var(N)
    | VarSlice(String, Box<Expr>, Box<Expr>)  // $var(M N)
    | Subst(Box<Script>) | Backtick(Box<Script>)
    | ProcSubIn(Box<Script>) | ProcSubOut(Box<Script>)
    | Regex(String)
    // Composites
    | List(Vec<Expr>)     // (a b c) literal
    | Concat(Vec<Expr>)   // span-adjacent ^-joined values
    // Operators
    | BinOp(BinOp, Box<Expr>, Box<Expr>)
    | UnOp(UnOp, Box<Expr>)
    | Match(MatchOp, Box<Expr>, Box<Expr>)  // matches / in / =~
}

pub enum BinOp {
    Add, Sub, Mul, Div, Mod, Pow,
    BitAnd, BitOr, BitXor, Shl, Shr,
    Lt, Le, Gt, Ge, Eq, Ne,
    And, Or,
}

pub enum UnOp { Neg, BitNot, Not }
pub enum MatchOp { Glob, In, Regex }

pub enum ExprContext { Value, Cond, Arith, List, Return }
```

`IfStmt.cond`, `IfStmt.elif_branches[].0`, `ForStmt.list_expr`, `WhileStmt.cond`, `LetStmt.value`, `AssignStmt.value`, `StatementKind::Return`, `CommandKind::Arith` all hold `Expr` (was `Vec<Token>` at U-5b).

### Public API

```rust
pub fn parse_expr_tokens(
    tokens: Vec<Token>,
    source_len: usize,
    ctx: ExprContext,
) -> ParseResult<Expr>;
```

Called by `parse.rs` at each placeholder site:

| Site | Context |
|---|---|
| `parse_if` cond, elif conds | `Cond` |
| `parse_for` list expression | `List` |
| `parse_while` cond | `Cond` |
| `parse_let` value | `Value` |
| `parse_assign` value | `Value` |
| `parse_return` value | `Return` (same shape as Value) |
| `parse_arith_command` body | `Arith` |

### Pratt-style precedence climbing

The arith path implements precedence climbing for the integer math operators. Precedence (lowest to highest, in the chain of mutually-recursive methods `parse_or → parse_and → parse_bit_or → parse_bit_xor → parse_bit_and → parse_eq → parse_rel → parse_shift → parse_add → parse_mul → parse_pow → parse_unary → parse_primary`):

| Level | Operators | Associativity | Active in |
|---|---|---|---|
| 1 | `\|\|` | left | Cond + Arith |
| 2 | `&&` | left | Cond + Arith |
| 3 | `\|` (bit OR) | left | Arith |
| 4 | `^` (bit XOR) | left | Arith |
| 5 | `&` (bit AND) | left | Arith |
| 6 | `==` `!=` `matches` `in` `=~` | left | Cond (eq + match); Arith (eq only) |
| 7 | `<` `<=` `>` `>=` | left | Cond + Arith |
| 8 | `<<` `>>` | left | Arith (structural; lex doesn't emit at v1) |
| 9 | `+` `-` | left | Arith |
| 10 | `*` `/` `%` | left | Arith |
| 11 | `**` | right | Arith |
| 12 (unary) | `! - ~` | right | `!` Cond+Arith; `- ~` Arith |
| 13 (primary) | atoms + `(...)` grouping | n/a | always |

### Arith retokenization

The lexer at U-5a treats `+ - * /` as word chars (so `1+2` is one Word, and `1 + 2` is three Words: Word("1") Word("+") Word("2")). The arith parser receives these tokens and the Pratt climbing needs to see proper operator tokens.

Solution: when an `ExprParser` is constructed with `ExprContext::Arith`, the entire token stream is preprocessed by `retokenize_arith_stream`. For each non-integer Word token, the helper `retokenize_arith_word` walks the text and emits per-char operator tokens (`+` → Plus, `-` → Minus, `*` → Star, `/` → Slash, `%` → Percent) interleaved with digit-runs (each digit run remains a Word that the integer-literal path parses to `Integer(N)`). Pure integer Words pass through unchanged so the primary path consumes them as integer literals; Words that contain non-digit non-operator chars (e.g., `"hello"`, `"1.5"`) pass through unchanged and the primary path errors with `InvalidArithLiteral`.

**The retokenization is purely a robustness aid.** It is documented as a v1.0 expedient; arith-mode lex awareness (a separate lex mode entered at `((` and exited at `))` that emits proper operator tokens directly) is the v1.x target.

### Concat semantics

The U-5b `Word::Concat` path handles concat in command-arg position. U-5c adds a parallel `ExprKind::Concat` for concat in expression contexts (`let x = $a^$b^$c`). The span-adjacency check is identical: a `^` token only binds when both sides are span-adjacent value tokens.

### Substitution body lifting

`$(cmd)`, `` `{cmd}` ``, `<(cmd)`, `>(cmd)` carry their bodies as raw `String` from the lexer. The U-5c expression parser **eagerly parses** these bodies as sub-scripts (recursive `parse(&body)` call), wrapping the resulting `Script` in `ExprKind::Subst(Box<Script>)` / `Backtick` / `ProcSubIn` / `ProcSubOut`. The AST after U-5c is fully resolved -- the evaluator (U-6) sees a uniform shape.

Cost: small (substitution bodies are typically short). Benefit: parse errors surface at parse time; the evaluator doesn't need a fallback for unparsed bodies.

### Var indexing

`$var(N)` and `$var(M N)` per scripture 6.9. The parser recognizes a span-adjacent `LParen` following a `Var` token (the `(` IMMEDIATELY follows the var name, no whitespace) and switches to index-parsing mode:

1. Collect tokens until the matching `RParen` (with paren-depth tracking).
2. Split the body on top-level whitespace boundaries (using span-gap detection: adjacent tokens with `prev.end != next.start` at depth 0 indicate a whitespace split).
3. One body piece → `VarIndex(name, Box<index>)`; two pieces → `VarSlice(name, Box<m>, Box<n>)`; zero or three+ pieces → `InvalidVarIndex` error.

Each piece is parsed in `Arith` context (the index/slice operands are integer-valued).

### Locked U-5c design decisions

| # | Decision | Behavior |
|---|---|---|
| 1 | In-place vs separate pass | **In-place lifting**: `parse_expr_tokens` called from each placeholder site in `parse.rs`. The AST has ONE canonical shape; no separate `lift_exprs(&mut Script)` pass. (Closes the U-5b handoff open question.) |
| 2 | Substitution bodies | **Eager parse**: `$(cmd)` body parsed at U-5c into `Subst(Box<Script>)`. No lazy/raw path. |
| 3 | Top-level operators in Value context | **Disallowed**: `let x = 5 + 3` is `TrailingTokensInExpr`. User writes `let x = (( 5 + 3 ))`. Concat (`^`) and var indexing remain valid at top level. |
| 4 | Arith retokenization | **Run upfront at construction**: `retokenize_arith_stream` runs once when an `ExprParser` is built with `ExprContext::Arith`. Splice-on-demand was abandoned mid-impl after the lifetime tangle became apparent. |
| 5 | Implicit list in List context | **Allowed**: `for (f in a b c)` builds a 3-element `List`. Same in `for (f in $files)` (single value); same in `for (f in (a b c))` (explicit list). |
| 6 | Empty list literal | **Allowed**: `let xs = ()` parses to `List(vec![])`. |
| 7 | Var index out-of-range | **Validated at evaluation time**, not parse time. Parser only checks that 1 or 2 integer expressions appear inside the parens. |
| 8 | `**` (Pow) recognition | Requires SPAN-ADJACENT `Star Star` after retokenization. `(( 2**3 ))` works; `(( 2 ** 3 ))` retokenizes the `**` Word to two adjacent Stars and works the same. |

### Error kinds (new)

```rust
EmptyExpression                          // empty expression context
UnexpectedTokenInExpr { expected }       // token doesn't belong in expr context
InvalidArithLiteral                      // arith Word that's not an integer
InvalidVarIndex                          // wrong shape inside $var(...)
TrailingTokensInExpr                     // tokens remain after a complete value
```

### Impl file map

```
usr/utopia/libutopia/src/parser/
├── mod.rs       extends with expr re-exports
├── span.rs      unchanged
├── token.rs     unchanged
├── error.rs     extends with U-5c expression-level error kinds
├── lexer.rs     unchanged
├── ast.rs       extends with Expr/ExprKind/BinOp/UnOp/MatchOp/ExprContext;
│                IfStmt.cond / ForStmt.list_expr / WhileStmt.cond /
│                LetStmt.value / AssignStmt.value / StatementKind::Return /
│                CommandKind::Arith change from Vec<Token> to Expr
├── parse.rs     extends to call parse_expr_tokens at each placeholder
└── expr.rs      U-5c NEW: Pratt-style expression parser
```

## 8.7 Pattern matching + try/catch + trace + on/mask (U-5d)

U-5d closes the U-5 parser arc. The U-5b parser core emitted `InvalidStatement` for `case` / `try` / `trace` / `on` / `mask` at statement-start; U-5d removes that error path and adds dedicated parsers for each construct.

### AST extensions

`StatementKind` gains five variants:

```rust
pub enum StatementKind {
    // ... existing ...
    Case(Box<CaseStmt>),        // statement-form case
    Try(Box<TryStmt>),
    Trace(Box<TraceStmt>),
    OnNote(Box<OnNoteStmt>),
    MaskNote(Box<MaskNoteStmt>),
}

pub struct CaseStmt {
    pub scrutinee: Expr,
    pub arms: Vec<CaseArm>,
    pub span: Span,
}

pub struct CaseArm {
    pub patterns: Vec<Expr>,    // 1+ patterns (space-separated); `*` is just Word("*")
    pub body: Statement,         // single statement; use a brace block for multi
    pub span: Span,
}

pub struct TryStmt {
    pub body: Vec<Statement>,
    pub catch: Vec<Statement>,
    pub span: Span,
}

pub struct TraceStmt {
    pub body: Vec<Statement>,
    pub span: Span,
}

pub struct OnNoteStmt {
    pub note_name: Expr,         // value-context Expr; evaluator resolves to string
    pub body: Vec<Statement>,
    pub span: Span,
}

pub struct MaskNoteStmt {
    pub note_name: Expr,
    pub body: Vec<Statement>,
    pub span: Span,
}
```

`ExprKind` gains the `Case` variant for case-as-expression (scripture 7.2):

```rust
pub enum ExprKind {
    // ... U-5c variants ...
    Case(Box<CaseExpr>),
}

pub struct CaseExpr {
    pub scrutinee: Box<Expr>,
    pub arms: Vec<CaseExprArm>,
    pub span: Span,
}

pub struct CaseExprArm {
    pub patterns: Vec<Expr>,
    pub value: Expr,             // single value Expr (Value context)
    pub span: Span,
}
```

### Public surface

No new public functions; `parse(&str) -> ParseResult<Script>` and `parse_tokens(...)` now accept the five new statement forms and the `Case` expression form. The `parse_expr_tokens(...)` entry recognizes `Case` at every context (Value, Cond, Arith, List, Return) — the case-as-expression dispatch lives at the top of `parse_value_top` and `parse_list_top` (so the Value/Return/List paths reach it) and inside `parse_primary` (so the Pratt operator chain in Cond/Arith reaches it).

### Pattern collection

Pattern collection inside a case arm uses the same span-gap whitespace-split algorithm as the U-5c var-index splitter. The helper `split_value_tokens_on_whitespace(Vec<Token>) -> Vec<Vec<Token>>` (previously `split_index_body`, renamed and exposed `pub(super)` so `parse.rs` can reuse it) walks adjacent tokens at top-level depth 0, splitting when `prev_token.span.end != next_token.span.start` (i.e., the user wrote whitespace). Each resulting slice is parsed via `parse_expr_tokens(slice, source_len, ExprContext::Value)` — pattern position disallows top-level operators (consistent with U-5c's Value-context rules).

The catch-all `*` pattern is just `ExprKind::Word("*")`; the evaluator's glob engine handles it as "match anything." No special parser handling needed.

### Locked U-5d design decisions

| # | Decision | Behavior |
|---|---|---|
| 1 | Arm body is a single Statement | `case $x { *.c => echo C }` -- the body after `=>` is one Statement. For multi-statement arms, users wrap in a brace block: `*.c => { build $x ; install $x }`. Simpler than detecting next-arm-start. |
| 2 | catch is REQUIRED after try | `try { a }` (no catch) emits `UnexpectedToken { expected: "`catch`" }`. Scripture 7.7 always pairs them. |
| 3 | `on` / `mask` require literal `note` | `on note 'snare:int' { ... }`. `note` is NOT a reserved keyword (per the U-5a token taxonomy); the parser matches it by text. `on banana { ... }` errors with `UnexpectedToken { expected: "`note`" }`. |
| 4 | Note name is an Expr | `on note 'name'`, `on note "name"`, and `on note bare-name` all parse to `OnNoteStmt.note_name: Expr` in Value context. The evaluator resolves to a string at registration time. |
| 5 | case-as-expression in all contexts | `parse_primary` (Pratt path) AND `parse_value_top` / `parse_list_top` (non-Pratt paths) all dispatch to `parse_case_expr` when the next token is `Case`. The evaluator decides what to do with the resulting value. |
| 6 | Empty pattern arm errors | `case $x { => body }` emits `EmptyCasePattern`. (The `collect_case_arm_patterns` walk also rejects `=>` at offset 0 with `UnexpectedToken { expected: "`=>`" }` if it hits the FatArrow before any tokens.) |
| 7 | Pattern list terminator is `=>` | Patterns are collected until `FatArrow` at depth 0. Top-level `Newline` / `Semicolon` / `RBrace` inside a pattern list is a parse error (a missing `=>`). |
| 8 | Trace body is general | `trace { body }` accepts any Vec<Statement>; the evaluator wraps each command's execution with the trace pre-print. |

### Error kinds (new)

```rust
EmptyCasePattern    // `case $x { => body }` -- no patterns before `=>`
```

Other case/try/trace/on/mask shape errors use the existing `UnexpectedToken { expected }` and `UnexpectedEof { expected }` variants with appropriate labels.

### Impl file map

```
usr/utopia/libutopia/src/parser/
├── mod.rs       extends re-exports with new AST types
├── span.rs      unchanged
├── token.rs     unchanged
├── error.rs     extends with `EmptyCasePattern`
├── lexer.rs     unchanged (case/try/catch/trace/on/mask are already reserved words)
├── ast.rs       extends with Case/Try/Trace/OnNote/MaskNote StatementKind variants,
│                Case ExprKind variant, and the supporting struct types
├── parse.rs     extends parse_statement dispatch + parse_case_stmt /
│                parse_try / parse_trace / parse_on_note / parse_mask_note +
│                collect_case_arm_patterns + collect_tokens_until_lbrace helpers
└── expr.rs      extends parse_primary + parse_value_top + parse_list_top with
                 Case dispatch; new parse_case_expr + parse_case_expr_arm + supporting
                 collect_* helpers
```

## 9. Status

- **U-5a LANDED**: tokenizer with span tracking, full reserved-word + operator coverage, single + double-quoted strings with parts, top-level + DQ-interior dollar forms, substitutions (`$()`, `` `{}` ``), process substitution, heredocs with body collection (interp + non-interp + strip-tabs), regex literal after `=~`. Reference impl at `usr/utopia/libutopia/src/parser/lexer.rs` (~900 LOC + ~560 LOC `#[cfg(test)]` tests).
- **U-5b LANDED**: parser core + AST. New `usr/utopia/libutopia/src/parser/ast.rs` (AST node types) + `parse.rs` (recursive-descent parser). Public entry: `parse(&str) -> ParseResult<Script>` (convenience that runs tokenize + parse_tokens) + `parse_tokens(Vec<Token>, source_len) -> ParseResult<Script>`. Grammar covers: Script, Statement (Pipeline / If / For / While / FnDecl / Let / Assign / Return / Break / Continue), Pipeline + PipelineElement (with `?|` -> `tolerate_failure` on LEFT element + trailing `&` -> `background`), Command + CommandKind (Simple / BraceBlock / Subshell / Arith — Arith body stored as raw tokens for U-5c), Word (Single token or `Concat(Vec<Token>)` for span-adjacent `^`-joined values), Redirect (Stdin / Stdout / Append / Heredoc-with-inline-body; redirects may interleave with words). Heredoc handling: parser pre-extracts all `HeredocBody` parts into a FIFO `VecDeque<Vec<DqPart>>` at construction time (taking ownership of the tokens via `core::mem::take` to avoid cloning); each `HeredocStart` pops the next body when parsing the redirect. Expression contexts (`if (cond)`, `for (var in expr)`, `while (cond)`, `let x = value`, bare assignment, `return value`) stored as raw `Vec<Token>` placeholders -- U-5c walks them into Expression trees. Locked design decisions per the U-5b session: strict rc (`--key=value` is `UnexpectedEqualInCommand` error); `^` requires span-adjacency on both sides (`UnexpectedCaret` otherwise -- actually emitted via `UnexpectedToken` since the loop just stops); postfix `?` requires a statement terminator (`UnexpectedTokenAfterFailPropagate` otherwise); `case` / `try` / `trace` / `on note` / `mask note` deferred to U-5d. ~900 LOC parser + ~250 LOC AST + ~500 LOC `#[cfg(test)]` tests (37 tests). Extended `flow_parser_core` `/u-test` probe (8 boot-time probes).
- **U-5c LANDED**: expression parser. New `usr/utopia/libutopia/src/parser/expr.rs` (~1100 LOC parser + ~500 LOC `#[cfg(test)]` tests; 35 tests). Public entry: `parse_expr_tokens(Vec<Token>, source_len: usize, ctx: ExprContext) -> ParseResult<Expr>`. Pratt-style precedence climbing over an owned token stream; eagerly parsed substitution bodies; full operator surface for arith + cond contexts; upfront arith retokenization that splits Word tokens containing operator chars (`+ - * /`) into proper Plus/Minus/Star/Slash tokens. The AST now has ONE canonical shape: every expression slot is an `Expr` (no `Vec<Token>` placeholders remain). `IfStmt.cond` / `IfStmt.elif_branches[].0` / `ForStmt.list_expr` / `WhileStmt.cond` / `LetStmt.value` / `AssignStmt.value` / `StatementKind::Return(Option<Expr>)` / `CommandKind::Arith(Expr)` all hold `Expr`. New `ExprKind` variants cover Atoms (Word/SingleQuoted/DoubleQuoted/Integer/Var/VarLen/VarNoSplit/VarIndex/VarSlice/Subst/Backtick/ProcSubIn/ProcSubOut/Regex), Composites (List/Concat), Operators (BinOp/UnOp/Match). New `ParseErrorKind` variants: `EmptyExpression`, `UnexpectedTokenInExpr`, `InvalidArithLiteral`, `InvalidVarIndex`, `TrailingTokensInExpr`. Extended `flow_parser_expr` `/u-test` probe (10 boot-time probes + 1 bonus, runtime-validated under `ThylaAlloc`). Reference doc 93 extended with section 8.6 covering AST extensions, public API, Pratt precedence chain, arith retokenization, concat semantics, substitution body lifting, var indexing, locked design decisions, error kinds, impl file map.
- **U-5d LANDED**: pattern matching + try/catch + trace + on/mask + case-as-expression. New statement-level constructs: `case $x { pat => body ... }` (`CaseStmt` + `CaseArm`), `try { ... } catch { ... }` (`TryStmt`), `trace { ... }` (`TraceStmt`), `on note 'name' { ... }` (`OnNoteStmt`), `mask note 'name' { ... }` (`MaskNoteStmt`). New expression-level construct: `case $x { pat => value ... }` in any context (`CaseExpr` + `CaseExprArm`; reached via `parse_primary` for Cond/Arith and via `parse_value_top` / `parse_list_top` for Value/Return/List). Each arm of a statement-form case has a single Statement body (use a brace block for multi-statement). `catch` is required after `try`. `on` / `mask` require the literal `Word("note")` after the keyword. Pattern collection reuses the U-5c span-gap whitespace-split (`split_value_tokens_on_whitespace`, formerly `split_index_body`, now `pub(super)`). New `ParseErrorKind::EmptyCasePattern` for empty-pattern arms; other shape errors use existing `UnexpectedToken { expected }`. ~500 LOC parse.rs additions + ~250 LOC expr.rs additions + ~100 LOC ast.rs additions + cfg(test) tests (20 new tests in parse.rs + 4 in expr.rs). Extended `/u-test` with new `flow_parser_full` (12 boot-time probes runtime-validated under `ThylaAlloc`). Reference doc 93 extended with section 8.7. The U-5 parser arc is COMPLETE; the parser's public surface is ready for U-6's evaluator.

## 10. Known caveats / footguns

- **`<<` is always heredoc-start.** The lexer doesn't have arith-mode awareness at U-5a, so `(( 1 << 2 ))` would attempt to lex `<<2` as a heredoc with tag `2`, which would then fail to find a terminator. The U-5c parser will need to either re-enter arith-mode lexing OR re-process the token stream. The deferral is acceptable for U-5a because the rc-shape grammar uses `(( ))` arithmetic rarely (most logic is brace-block + `?` propagate).

- **`<` in arith context same gotcha.** `Less` / `LessEqual` are well-defined; the inner-paren-counting is the parser's job.

- **`<<` as shift-left or `>>` as shift-right inside arithmetic.** Not parsed at U-5a; deferred to U-5c arith mode. `>>` outside arith is the file-append redirect (`GreaterGreater`) and works.

- **The lexer doesn't enforce paren matching across the FULL token stream.** It only requires balance inside individual constructs (`$(cmd)`, `` `{cmd}` ``, `<(cmd)`, etc.). A standalone unmatched `(` in command position lexes as `LParen` and the parser will reject it.

- **`?` disambiguation is one-byte lookahead.** A `?` followed by a UTF-8 multi-byte word char's leading byte (≥ 0x80) is included in the word. This is correct (the multibyte sequence is a word char), but the lookahead doesn't decode the full char. Defensive only — UTF-8 sequences are always valid here since `source: &str` is guaranteed valid UTF-8.

- **Multiple heredocs on one line collect bodies in order.** `cat <<A <<B\nbody_a\nA\nbody_b\nB\n` — bodies collected at the first newline, in FIFO order (A's body first, then B's). This matches POSIX heredoc semantics.

- **(U-5c) Arith Words containing `%` cannot be lexed.** `%` is neither a word char nor a top-level operator the lexer recognizes; `(( 5 % 2 ))` lexes as `Word("5")` then `UnexpectedChar('%')`. The `%` (Mod) operator is structurally supported in the Pratt parser; the lexer side is v1.x. Workaround: rewrite `%` arith expressions in terms of `/` and `-` until the lexer surface lands.

- **(U-5c) Arith Words can't carry alphabetic or `_` chars.** `(( hello ))` produces `InvalidArithLiteral`. Bare identifiers are not primaries in arith; variables must use `$` prefix (`(( $hello ))`).

- **(U-5c) Substitution bodies parse with body-relative spans.** A `$(cmd)` body's sub-Script reports parse errors with spans relative to the body bytes, not the outer source. Translation to outer coordinates is a v1.x lift (the outer span anchors the offset).

- **Regex mode is a one-shot.** Only the IMMEDIATELY following opening `/` (after `=~` + whitespace) is treated as regex start. A pattern like `$x =~ $regex` (where `$regex` holds the pattern) does not engage regex mode; instead `$regex` is just a `Var`. The parser at U-5c/d will route the regex string through whatever runtime path the evaluator uses.

- **`#` is NOT a comment marker inside `"..."` or `'...'`.** Quotes preserve `#` literally. The skip-comment logic only fires at top-level dispatch position.

- **Backslash escape semantics inside bare words preserves the next char literally.** `\<space>` is a one-char Word containing a space. `\$path` is a single Word containing `$path`. The shell convention is: backslash escapes the IMMEDIATELY following char in bare-word context; for special chars (`$`, `"`, `'`, `` ` ``, etc.) this suppresses their syntactic meaning.

## 11. Naming rationale

`libutopia::parser` rather than `libutopia::shell` or `libutopia::rc` because the parser is the **substrate** that the shell (the evaluator + main loop in `usr/utopia/shell/`) consumes. The libutopia/shell split keeps the parser host-testable as a standalone library; future Utopia surfaces (a hypothetical scripting CLI, a syntax-highlighting helper for an editor plugin) could depend on `libutopia::parser` without dragging the whole shell.

No thylacine-themed renaming at U-5a; the parser pieces (`tokenize`, `Token`, `Lexer`, `ParseError`) follow standard Rust/compiler conventions because that's what readers will expect. Future thematic candidates: the visitor pattern that walks the AST in U-6 might pick up a name like `tracker` (apex predator following the AST limb) — held for U-6.

## 12. References

- `docs/UTOPIA-SHELL-DESIGN.md` — design scripture (sections 5-9 for grammar; section 19 for sub-chunk plan).
- `docs/UTOPIA.md` — top-level Utopia milestone.
- `docs/reference/91-utopia.md` — `libutopia` skeleton (U-3).
- `docs/reference/92-utopia-line-editor.md` — `libutopia::line_editor` (U-4 arc; produces the `&str` that this parser consumes).
- `usr/utopia/libutopia/src/parser/` — the parser stack source.
- `usr/u-test/src/main.rs::flow_parser_lexer` — boot-time probe.
