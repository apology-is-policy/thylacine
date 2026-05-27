# `libutopia::parser` — the rc-shape parser stack for `ut` (U-5 arc)

**Status**: U-5a LANDED — the tokenizer. U-5b/c/d are queued for the parser core, expression layer, and pattern matching.

This is the as-built reference for the rc-shape parser used by the Utopia shell (`ut`). It lives at `usr/utopia/libutopia/src/parser/` and is the only path from "user typed a line" to "AST evaluator consumes". The line editor (U-4 arc) produces an `EditorAction::Accept(String)` when Enter is pressed on a balanced buffer; that string is fed to `tokenize()` here, then (U-5b onward) to `parse()` to produce an AST, then (U-6) to the evaluator.

The parser is a **pure-logic engine**. No I/O. No syscalls. Host-testable via `#[cfg(test)]` unit tests; runtime-validated via the `flow_parser_lexer` probe in `/u-test`. Same factoring as the line editor: a clean separation between the algorithm and its substrate.

---

## 1. Purpose

The parser stack consumes a Rust `&str` containing rc-shape source (per `docs/UTOPIA-SHELL-DESIGN.md` sections 5-9) and produces an AST. At U-5a, it produces a `Vec<Token>` — the tokenizer's output, the input to U-5b's recursive-descent parser.

The token taxonomy is opinionated: most syntactic surfaces get their own kind, but where lex-time disambiguation is genuinely impossible without parser context (e.g., `^` is concatenation in command position and bitwise-xor in arithmetic), the lexer emits the same token and lets the parser decide.

## 2. Public API (U-5a)

```rust
// usr/utopia/libutopia/src/parser/mod.rs

pub mod error;
pub mod lexer;
pub mod span;
pub mod token;

// Re-exports
pub use error::{ParseError, ParseErrorKind, ParseResult};
pub use lexer::tokenize;
pub use span::Span;
pub use token::{DqPart, Token, TokenKind};

// usr/utopia/libutopia/src/parser/lexer.rs
pub fn tokenize(source: &str) -> ParseResult<Vec<Token>>;
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

## 9. Status

- **U-5a LANDED**: tokenizer with span tracking, full reserved-word + operator coverage, single + double-quoted strings with parts, top-level + DQ-interior dollar forms, substitutions (`$()`, `` `{}` ``), process substitution, heredocs with body collection (interp + non-interp + strip-tabs), regex literal after `=~`. Reference impl at `usr/utopia/libutopia/src/parser/lexer.rs` (~900 LOC + ~560 LOC `#[cfg(test)]` tests).
- **U-5b QUEUED**: parser core — top-level commands, pipelines, control flow (`if`/`for`/`while`/`fn`/`try`/`catch`/`case`/brace blocks/subshells), AST node types.
- **U-5c QUEUED**: expression parser — variable refs with index/slice, arithmetic `(( ))` (the lexer's existing tokens get re-interpreted in arith context), concatenation, string comparison.
- **U-5d QUEUED**: pattern matching + try/catch + trace + fn declarations; regex semantics (currently the token is opaque); finalize the public `parse()` entry.

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
