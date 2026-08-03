---
id: sub-utopia-parser
type: sub
parent: moc-userspace-shell-tui
title: "The ut parser — an rc-shape grammar, three recursion bounds, and 188 tests that cannot compile"
code:
  - usr/utopia/libutopia/src/parser/mod.rs
  - usr/utopia/libutopia/src/parser/lexer.rs
  - usr/utopia/libutopia/src/parser/token.rs
  - usr/utopia/libutopia/src/parser/parse.rs
  - usr/utopia/libutopia/src/parser/expr.rs
  - usr/utopia/libutopia/src/parser/ast.rs
  - usr/utopia/libutopia/src/parser/error.rs
  - usr/utopia/libutopia/src/parser/span.rs
audit: none
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design:
  - "docs/UTOPIA-SHELL-DESIGN.md sections 5-9"
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

Text to AST for `ut`, the Utopia shell. A `&str` — normally one line handed
over by the line editor on Enter — becomes a `Script`, and nothing else
happens: no syscall, no filesystem access, no evaluation. It is the only part
of the shell that touches nothing outside its own arguments.

Which is why it still needs a dossier. **Purity buys the parser exactly one
safety property, and not the one people assume.** A mis-parse cannot escalate
anything — but a *deep enough* parse can exhaust the stack of a `no_std`
program that has no guard page of its own, and a shell that dies takes its
user's session with it. That hazard, and the three separate mechanisms built to
bound it, is most of what is interesting here.

## Contract

Four entry points, all returning `ParseResult`:

- `tokenize(&str)` — a `Vec<Token>` ending in a synthetic end-of-file token
  whose span is a point past the last byte.
- `parse(&str)` — tokenize then parse, producing a `Script`.
- `parse_tokens(tokens, source_len)` — for a caller that already has tokens.
- `parse_expr_tokens(tokens, source_len, context)` — the expression layer,
  parameterized by context because the same token means different things in
  command position and in arithmetic position.

Every token and every AST node carries a `Span`: an inclusive-start,
exclusive-end pair of **byte** offsets. The grammar is rc-shaped rather than
POSIX-shaped — braces delimit blocks, conditions are parenthesized, and the
value model is a list rather than a string.

## Mechanism

### The lexer's job is to make the parser's job easy, and it says where it gives up

A single-pass byte scanner over eight syntactic surfaces: whitespace and
comments, bare words, quoted strings, variable references, substitutions,
process substitutions, heredocs, and regex literals. Every unambiguous surface
gets its own token kind.

Where lexical disambiguation is genuinely impossible without parse context, the
lexer declines rather than guessing — `^` is concatenation in command position
and exclusive-or in arithmetic, so it emits one token and lets the parser
decide. The same admission appears for `%`, which is a word character so that a
job specification and a literal percent both lex as words, with the expression
layer re-splitting the word's text when it turns out to be arithmetic.

Two pieces of state make the scanner not quite context-free, both queued rather
than backtracked: heredoc bodies are collected at the *next newline* after the
tag that requested them, drained first-in-first-out; and a regex literal is
recognized only through a one-shot flag set when the match operator is emitted.

### UTF-8 correctness is structural, not checked

The span contract — that slicing the source by any span yields valid UTF-8 —
holds because every advance is by a whole character. The scanner uses a
character-length helper wherever it copies text, and the sites that advance by
a single byte have already matched an ASCII byte. Non-ASCII is admitted
deliberately: the word-character test returns true for any byte at or above
0x80, so a multi-byte character starts a word and is copied through verbatim.

### Substitution bodies are re-parsed, not parsed in place

A `$(...)` body is stored raw by the lexer as one token, then tokenized and
parsed as an independent source when the expression layer descends into it.
That keeps the outer grammar simple, and it has one consequence that returns in
Caveats: **the sub-script's spans are offsets into the body, not into the
line.**

### The one real hazard is stack depth, and it has three shapes

A recursive-descent parser in a `no_std` program has no stack guard of its own;
overflowing the EL0 stack is a guard-page fault, which terminates the shell.
Three separate bounds exist because the recursion has three shapes and **no
single counter sees all of them**:

| bound | value | what it catches | why the others miss it |
|---|---:|---|---|
| bracket nesting | 64 | `(((…`, `{{{…` | a flat pre-pass over the token stream, run before any recursion starts |
| operator recursion | 256 | `a**b**c**…`, `!!!!…` | right-associative and prefix chains are not brackets, so the pre-pass counts nothing |
| re-lex depth | 32 | `$($($(…)))` | a whole substitution is *one token* in the outer stream, so the pre-pass sees depth 1 |

The middle row is the interesting one, because it was added after the fact: the
bracket pre-pass shipped first, and an audit round found that a chain of
exponentiation operators recurses with no bracket to count. The comment
introducing the fix names that history rather than just the constant.

The bracket pre-pass runs inside the token-stream entry point, so it re-runs on
every re-lexed substitution body — the two outer bounds compose rather than
overlap. The re-lex counter is a process-global atomic, justified by the shell
parsing one line at a time and argued to fail safe under a hypothetical
concurrent parse (it would trip earlier, never later), and it is decremented on
both the success and error paths so a top-level parse always restores it.

## Data structures

- **`Span`** — two byte offsets, with `join` (widen to cover both), `contains`,
  and a `slice` that indexes the source. `slice` has no callers.
- **`Token` / `TokenKind`** — the lexeme plus its span. There are no bracket
  tokens for `[` and `]`: those are glob metacharacters and lex as word
  characters, which is why the nesting pre-pass's three-pair set is complete
  rather than partial.
- **`DqPart`** — the pieces of an interpolated string, so a double-quoted run
  keeps its literal and substituted segments distinct instead of being
  re-scanned later.
- **The AST** — a `Script` of `Statement`s; a statement is a pipeline or one of
  the control forms; a pipeline holds elements holding commands; a command is
  simple, a brace block, a subshell, or arithmetic. Expressions are a separate
  tree reached from every expression slot, with one node kind for
  case-as-an-expression so `case` is available in both positions.
- **`ParseErrorKind`** — 29 variants, each naming a specific malformation
  rather than a generic parse failure.

## Concurrency

None. The parser holds no locks and shares no state, with a single exception:
the re-lex depth counter is a process-global atomic rather than a field,
because the recursion it bounds crosses a re-entrant tokenize-and-parse call
that has no parser instance to hang it on. The reasoning for that choice is
written where the counter is declared, and its failure direction is stated — a
concurrent parse would trip the bound early, never late.

## Invariants enforced

**None from the enumerated set.** No syscall, no capability, no lifetime.

The parser does hold one property the rest of the shell depends on, and it is
worth naming because it is on no list: **no input, however hostile, recurses
the parser deep enough to fault.** That is a liveness property of the shell
rather than a soundness property of the system, which is exactly why it rests
on three hand-written counters and nothing structural.

## Error paths

Every failure is a `ParseError` carrying a kind and a span; no partial AST is
returned and there is no panicking path in normal operation. The 29 error kinds
are specific enough that the taxonomy is itself a description of the grammar —
unterminated heredoc, empty case pattern, invalid variable index, recursion
limit, and so on.

Depth exhaustion is reported through the same channel as any syntax error,
which is the whole design goal: a pathological input is a *message*, not a dead
shell.

## Performance

Not a measured surface. One pass over the input for the lexer, one over the
tokens for the parser, plus one additional lex-and-parse per substitution body.
Nothing here is on a hot path — it runs once per line typed.

## Prosecution

- **A new nesting construct must reach one of the three bounds.** Add a grammar
  form that recurses and ask which counter sees it: does it pass through a
  counted bracket, is it an operator chain that needs the depth field, or is it
  a re-entrant parse that needs the re-lex counter? The second bound exists
  because that question was once answered wrong.
- **A new bracket-like token pair must join the pre-pass's match arms**, or the
  pre-pass silently stops being complete.
- **A new advance must move by a whole character**, or use the length helper.
  The span contract has no runtime check behind it.
- **A new sub-parse must choose its coordinate system explicitly** — see
  Caveats; there is already one place where the answer leaks.
- **A new error kind carries a span that indexes the source it was parsed
  from.** Mixing the two coordinate systems is a failure this code already has
  an instance of.

## Seams

- **Span coordinates are never translated across a sub-parse.** The
  body-relative choice is deliberate and documented; translation is described
  as future work, and the outer anchor needed to do it is already passed in.
- **`Span::slice` exists and is unused.** The helper that would let a
  diagnostic show the user the offending text has no callers, so the operation
  the whole span apparatus was built to enable is performed nowhere.
- **Case-as-an-expression exists in the AST** with its own node kind, so the
  grammar admits `case` in both statement and expression position — a wider
  surface than most shells offer, and correspondingly more to keep working.

## Caveats

- **188 of this parser's tests cannot compile, in the file that claims to be
  host-testable.** The lexer's header says "Pure logic; no I/O; host-testable."
  As configured it is none of the third: the workspace pins a bare-metal target
  for every build, that target has no test harness, and the crate is
  unconditionally `no_std`, so the test module fails to find the test crate
  before a single assertion runs. The escape — building for the host — is
  blocked one level down, in the runtime crate whose inline assembly will not
  assemble for a host target. This is not local to the parser: about 878 test
  functions across six native crates are in the same state, and the pattern
  that fixes it is proven in-tree by a seventh crate, which runs its 7 tests
  successfully. The parser *is* covered — but by a separate in-guest test
  binary that drives it through the public entry points on every boot. Two
  independently written bodies of test intent, of which the older and more
  granular one is dead. Task #105.

- **A substitution body's errors are reported in the body's coordinate system,
  on the one path that forgot to re-anchor.** Spans inside a `$(...)` index the
  body, and the evaluator renders spans as bare byte offsets with nothing
  marking which source they belong to. The evaluator re-anchors deliberately on
  eight paths out of nine — the parse-failure path goes so far as to discard the
  inner error entirely to avoid the problem — and the ninth is a bare `?` that
  propagates a body-relative span unchanged. Task #104.

- **The only written record of that hazard is attached to the wrong item.** The
  paragraph explaining that sub-script coordinates are not translated is an
  orphaned doc comment: the function it once described has moved, and with no
  blank line to stop it the text now documents the re-lex depth constant, whose
  rendered documentation therefore opens "Eagerly parse a substitution body …
  into a sub-Script". It also names a parameter, `_outer_span`, that neither
  item has. So a reader of the sub-parse function — the person who needs the
  warning — never sees it, and a reader of the constant sees a function
  description.

- **The nesting pre-pass's counter is not clamped at zero.** A closing bracket
  decrements unconditionally, so a stream beginning with unmatched closers
  drives the count negative and buys that many extra levels before the bound
  trips. Not reachable in practice — the parser rejects the unmatched closers
  long before the extra depth could be spent — but the counter measures *net*
  rather than *maximum* nesting, which is not the quantity the bound wants.

## Provenance

[[chg-2026-08-03-utopia-parser-sweep]].
