# `libutopia::parser` — the rc-shape parser stack for `ut` (U-5 arc)

**Status**: U-5a + U-5b LANDED — the tokenizer + the recursive-descent parser core (statement-level grammar + AST). U-5c/d are queued for the expression layer and pattern matching.

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

## 9. Status

- **U-5a LANDED**: tokenizer with span tracking, full reserved-word + operator coverage, single + double-quoted strings with parts, top-level + DQ-interior dollar forms, substitutions (`$()`, `` `{}` ``), process substitution, heredocs with body collection (interp + non-interp + strip-tabs), regex literal after `=~`. Reference impl at `usr/utopia/libutopia/src/parser/lexer.rs` (~900 LOC + ~560 LOC `#[cfg(test)]` tests).
- **U-5b LANDED**: parser core + AST. New `usr/utopia/libutopia/src/parser/ast.rs` (AST node types) + `parse.rs` (recursive-descent parser). Public entry: `parse(&str) -> ParseResult<Script>` (convenience that runs tokenize + parse_tokens) + `parse_tokens(Vec<Token>, source_len) -> ParseResult<Script>`. Grammar covers: Script, Statement (Pipeline / If / For / While / FnDecl / Let / Assign / Return / Break / Continue), Pipeline + PipelineElement (with `?|` -> `tolerate_failure` on LEFT element + trailing `&` -> `background`), Command + CommandKind (Simple / BraceBlock / Subshell / Arith — Arith body stored as raw tokens for U-5c), Word (Single token or `Concat(Vec<Token>)` for span-adjacent `^`-joined values), Redirect (Stdin / Stdout / Append / Heredoc-with-inline-body; redirects may interleave with words). Heredoc handling: parser pre-extracts all `HeredocBody` parts into a FIFO `VecDeque<Vec<DqPart>>` at construction time (taking ownership of the tokens via `core::mem::take` to avoid cloning); each `HeredocStart` pops the next body when parsing the redirect. Expression contexts (`if (cond)`, `for (var in expr)`, `while (cond)`, `let x = value`, bare assignment, `return value`) stored as raw `Vec<Token>` placeholders -- U-5c walks them into Expression trees. Locked design decisions per the U-5b session: strict rc (`--key=value` is `UnexpectedEqualInCommand` error); `^` requires span-adjacency on both sides (`UnexpectedCaret` otherwise -- actually emitted via `UnexpectedToken` since the loop just stops); postfix `?` requires a statement terminator (`UnexpectedTokenAfterFailPropagate` otherwise); `case` / `try` / `trace` / `on note` / `mask note` deferred to U-5d. ~900 LOC parser + ~250 LOC AST + ~500 LOC `#[cfg(test)]` tests (37 tests). Extended `flow_parser_core` `/u-test` probe (8 boot-time probes).
- **U-5c QUEUED**: expression parser — variable refs with index/slice (`$var(N)`, `$var(M N)`), arithmetic `(( ))` (the parser's stored `Arith(Vec<Token>)` body gets re-interpreted), concatenation operator semantics, string comparison, regex semantic interpretation.
- **U-5d QUEUED**: pattern matching + try/catch + trace + on/mask + final glue. `case $x { pat => body ... }`, `try { ... } catch { ... }`, `trace { ... }`, `on note 'name' { ... }`, `mask note 'name' { ... }`. Extends `StatementKind` with the deferred variants; finalizes the parser's public surface.

## 10. Known caveats / footguns

- **`<<` is always heredoc-start.** The lexer doesn't have arith-mode awareness at U-5a, so `(( 1 << 2 ))` would attempt to lex `<<2` as a heredoc with tag `2`, which would then fail to find a terminator. The U-5c parser will need to either re-enter arith-mode lexing OR re-process the token stream. The deferral is acceptable for U-5a because the rc-shape grammar uses `(( ))` arithmetic rarely (most logic is brace-block + `?` propagate).

- **`<` in arith context same gotcha.** `Less` / `LessEqual` are well-defined; the inner-paren-counting is the parser's job.

- **`<<` as shift-left or `>>` as shift-right inside arithmetic.** Not parsed at U-5a; deferred to U-5c arith mode. `>>` outside arith is the file-append redirect (`GreaterGreater`) and works.

- **The lexer doesn't enforce paren matching across the FULL token stream.** It only requires balance inside individual constructs (`$(cmd)`, `` `{cmd}` ``, `<(cmd)`, etc.). A standalone unmatched `(` in command position lexes as `LParen` and the parser will reject it.

- **`?` disambiguation is one-byte lookahead.** A `?` followed by a UTF-8 multi-byte word char's leading byte (≥ 0x80) is included in the word. This is correct (the multibyte sequence is a word char), but the lookahead doesn't decode the full char. Defensive only — UTF-8 sequences are always valid here since `source: &str` is guaranteed valid UTF-8.

- **Multiple heredocs on one line collect bodies in order.** `cat <<A <<B\nbody_a\nA\nbody_b\nB\n` — bodies collected at the first newline, in FIFO order (A's body first, then B's). This matches POSIX heredoc semantics.

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
