---
id: sub-utopia-parser
type: sub
parent: moc-userspace-shell-tui
title: "The ut parser — an rc-shape grammar, three recursion bounds, and the tests that could not compile until they found six defects"
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
updated: 2026-09-23
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

Tab completion reads the word under the cursor with a second scanner over the
same grammar, for the incomplete input the lexer is not built for; it shares the
lexer's character predicates (`is_word_char_byte`, `is_var_name_start_byte`,
`is_var_name_byte`, `pub(crate)` for that reason), and a test pins the two to
one reading of complete input ([[sub-utopia-interactive]]).

### A word keeps its backslashes, and where it lands decides what they mean

A bare word's `\<char>` makes the character part of the word whatever it is, and
since 2026-09-23 the word's text KEEPS the backslash; only a `\<newline>`
continuation is removed at lex time. That is what `TokenKind::Word`'s own doc
had always said -- raw source bytes, escapes left to the evaluator -- while the
scanner resolved every escape, so a word's text could not tell `\*` from `*`.
The meaning now depends on where the word is read:

| where the word is read | reads | so `\*` is |
|---|---|---|
| a value: an argv word, an expression atom, a redirect target | `lexer::unescape(text)` | a `*` character |
| arithmetic | unescaped before the arithmetic re-split | the operator (`2\*3` is 6, as before) |
| an argv glob, a `case` arm, a `matches` | the text as written | a literal star, never a wildcard |
| keyword and identifier tests | the text as written | -- (`\if` is a word, `\x=1` no assignment) |

`unescape` lives with the lexer because the lexer defines the escape.
`unescape_gives_the_value_the_lexer_used_to_resolve` pins that `unescape` of
every kept word equals what the lexer used to produce -- every printable ASCII
escape at three positions, doubled and trailing backslashes, a continuation,
multi-byte characters -- so no value changed, and only globs and patterns see the
difference. The matcher's half is [[sub-utopia-eval]].

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
- **The AST** — a `Script` of `Statement`s; a statement is a pipeline, a
  short-circuit AND-OR list (`p && q || r`, scripture 8.6 — a `first` pipeline
  plus a `Vec<(AndOrOp, Pipeline)>`, built only when a connector is present so a
  lone pipeline keeps its `Pipeline` shape; left-associative, `&&`/`||` at equal
  precedence), or one of the control forms; a pipeline holds elements holding
  commands; a command is
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

One of the 29 is now vestigial: `UnexpectedEqualInCommand` is kept for its
`Display` arm but no longer raised. An `=` in argument position is a literal
word — `parse_word` glues a span-adjacent `=` and its value into one argv
element (`-std=c++20`), the same span-adjacency rule that already fused
`~/path` — and a statement-start assignment is caught earlier by
`is_assignment_start`, so nothing reaches the old raise.

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

- **FIXED 2026-09-23: a backslash-escaped glob character no longer globs.**
  `scan_word` turned `\*` into a bare `*` inside `Word(text)`, so eval could not
  tell it from a wildcard: by reading `evaluate_argv`, `rm \*` removed every file
  in the directory, and `grep a\*b f`, finding no file named like `a*b`, lost the
  argument under rc's no-match-is-empty rule. The same loss made an escaped `*` a
  wildcard in a `case` arm or a `matches`, and made `\if` the keyword. Fixed by
  keeping the escape (Mechanism, above) rather than by a side channel on the
  token, which would have left two spellings of every word to keep in step. The
  "hundred match sites" an early estimate feared were mostly tests: nineteen
  outside them read a word's kind or text, and each is classed as a value, a
  pattern, or the word as written. The side effects are each POSIX's reading and
  each an edge a script must go looking for: an escaped reserved word is an
  ordinary word; an escaped name is no identifier, so `\x=1` is not an assignment
  and `fn \f` is refused; and `~\/x` is not the home form, since the
  tilde-prefix ends at the first UNQUOTED slash. Scripture still documents
  backslash-in-a-word nowhere (UTOPIA-SHELL-DESIGN.md 6.4-6.5 name only the two
  quotes); the semantics above are proposed to the operator for 6.4. Tab
  completion still spells names with single quotes ([[sub-utopia-interactive]]).
- **FIXED 2026-09-22: this parser's tests run.** They had never compiled — the
  crate depended on libthyla-rs unconditionally, whose inline assembly will not
  assemble for a host target, so the workspace's bare-metal pin had no escape
  and the lexer's header claim ("Pure logic; no I/O; host-testable") was true of
  the code and false of the build. `libutopia` now carries the tree's standard
  `backend` feature, so `--no-default-features` leaves the pure half —
  `parser`, `line_editor`, `ansi`, `path`, `palette`, `eval::{jobs, error,
  value}` — host-buildable. **296 tests ran where 0 did** at the split, of 399
  declared. At `53c51671` the crate runs **312** distinct tests of **403**
  declared, and **91 stay stranded** in the gated modules (`eval::expr` 29,
  `repl` 23, `eval::stmt` 14, `eval::glob` 11, `completion` 9, `eval::env` 5),
  which `tools/test-rust.sh` now counts per crate ([[sub-substrate-gates]]).
  Cargo reported 313 until 2026-09-23 because
  `equal_is_assignment_at_statement_start_and_literal_after_a_word` carried TWO
  `#[test]` attributes -- a leftover of the UT-PARSE-2 withdrawal -- so libtest
  registered and ran it twice. rustc's `duplicate_macro_attributes` warning said
  so on every build, and nothing printed it.

  **What the first run found is the reason this caveat is worth reading rather
  than deleting.** Asking never-compiled tests to compile produced 20 build
  errors (all one missing `use alloc::vec`), and then 49 failures. 41 were a
  single stale helper in `parser::expr::tests` — `lex()` handed
  `parse_expr_tokens` the lexer's synthetic trailing `Eof`, which that function
  documents itself as not taking and which every production caller in `parse.rs`
  strips. The remaining **8 are 6 genuine defects**, quarantined with
  `#[ignore = "UT-PARSE-n"]` reasons so the gate keeps its signal for new
  breakage while each debt stayed greppable. **All six are now closed, and the
  split is the interesting part: four were real and two were the tests being
  wrong.** FIXED: UT-PARSE-1 (reserved words off the command word), -3 (`))`
  split), -4 (truncation reports Eof), -5 (the backtick form, operator-
  ratified). WITHDRAWN: UT-PARSE-2 asserted `cmd =arg` is two words, which
  contradicts `UTOPIA-SHELL-DESIGN.md` 6.1's documented `x = value` form, so
  the parser was right; UT-EDIT-1 asserted `ESC ESC` returns to Ground, and the
  editor deliberately restarts the sequence as the VT machine does. A test that
  has never run has never had a chance to be right either, so each one was
  settled from the CONTRACT -- scripture, the heritage, or the documented state
  machine -- rather than by editing whichever side was cheaper.
  **UT-PARSE-4 was investigated and downgraded** -- truncated input reports
  `UnexpectedToken` where `UnexpectedEof` is expected, and the theory that this
  could break the REPL's line-continuation is FALSE: `line_editor` decides
  submission with its own `balance(buffer)` tracker (*"intentionally
  lightweight; the U-5 parser is authoritative"*), and nothing outside
  `parser/` consumes `UnexpectedEof` at all. Its blast radius is the diagnostic
  a truncated script file prints.

- **Running out of input says so** (UT-PARSE-4, fixed 2026-09-22).
  `expect_kind`'s `UnexpectedEof` arm keyed on `peek_kind() == None` and was
  therefore UNREACHABLE: `tokenize` always appends a synthetic `Eof` TOKEN, so
  exhausted input arrives as `Some(Eof)` and fell through to the general
  wrong-token arm. Every expect site in the parser was discarding the fact that
  the input had ended, reporting `{ a; b` as "unexpected token, expected `}`"
  pointing at a token the user never typed. An explicit `Some(Eof)` arm
  restores it. Not a continuation bug -- the line editor decides submission
  with its own brace tracker -- so the blast radius is the diagnostic a
  truncated script file prints. The test carries the control that matters: a
  complete-but-malformed input must still NOT report Eof, which is the failure
  mode of the careless version of this fix.

- **A reserved word is reserved only in COMMAND-WORD position** (UT-PARSE-1,
  fixed 2026-09-22). Before this, the sixteen words in
  `TokenKind::reserved_word` -- `if`, `in`, `for`, `case`, `fn`, `let`, `while`,
  `try`, `catch`, `return`, `break`, `continue`, `on`, `mask`, `trace`, `else`
  -- could not be used as an argument or a filename ANYWHERE. `echo if`,
  `cd in`, `cat case`, `echo a in b` and `cmd < in` were all parse errors,
  because the lexer reserves on word text and `is_value_token` admits no
  keyword. The finding arrived looking like a redirect bug; measuring it
  returned eighteen refused forms.

  `Parser::demote_reserved_word` rewrites the token in place to the `Word` it
  spells, keeping its span, at exactly two sites: `parse_simple_command`'s loop
  **guarded on `!words.is_empty()`**, and `parse_redirect_target`
  unconditionally (a redirect target is never a command word). The guard is the
  whole of POSIX rule 1 and is load-bearing -- without it a pipeline element
  beginning with a keyword would silently become a command named `if`. This is
  also rc's answer, which its lexer reaches with a last-token flag; doing it in
  the parser instead means the lexer stays context-free and the demotion only
  has to be right where a word is already what the grammar asks for. Nothing
  downstream learns of it: the result is an ordinary `TokenKind::Word`.

  `TokenKind::reserved_word_text` is the hand-written inverse of
  `reserved_word`, so `reserved_word_round_trips` walks every word in the
  forward table and fails if it does not come back with the same spelling --
  a keyword added to one table and not the other would otherwise quietly become
  unusable as a filename again.

- **`` `{cmd} `` has NO closing backtick** (UT-PARSE-5, fixed 2026-09-22,
  operator-ratified). `scan_backtick` required a trailing backtick, and its own
  comment claimed scripture 6.6 said so -- 6.6 says `` `{cmd} ``, which is rc's
  actual form. The deviation defeated the form's only stated purpose, since a
  real rc script's `` `{ls} `` failed to lex; three more comments (the lexer's
  file header, `ParseErrorKind::UnterminatedBacktick`, `TokenKind::Backtick`)
  carried the same wrong claim and are corrected. The `}` now ends the form, so
  a backtick after it OPENS THE NEXT substitution -- which is also why both
  forms are not accepted: `` `{a}`{b} `` would be ambiguous.

- **`))` is split at the parse site, not lexed in context** (UT-PARSE-3, fixed
  2026-09-22). The lexer is context-free and emits `DoubleRParen` for any `))`,
  which is right for `$(( ))` and wrong for `(a; (b; c))`, where two subshells
  close with nothing between them. `Parser::split_double_rparen` rewrites that
  token in place into two `RParen`s carrying the two halves of its span, and is
  called from `parse_block_statements_until` only when the end token IS
  `RParen` -- so an arithmetic `))`, which `parse_arith_command`'s own depth
  accounting consumes before this is reached, is untouched. It is called twice
  per iteration and both sites are load-bearing: once before looking for the
  end token, and once before JUDGING the statement terminator, because the
  inner subshell's last statement finishes with `))` current and the terminator
  check is what fires first.

  **The opening side is deliberately NOT symmetric.** `((` is the arithmetic
  opener, so `((a; b); c)` is read as arithmetic and fails on `a` -- exactly as
  it does in sh, with the same remedy, a space. That is pinned by an assertion
  rather than left as current behaviour, so a future lexer mode cannot silently
  turn it into a subshell.

  The separate in-guest binary that drives the public entry points on every boot
  remains, so the parser now has BOTH bodies of test intent live — and the
  granular one, dead since it was written, is what found the six. Task #105.

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

[[chg-2026-08-03-utopia-parser-sweep]] · [[chg-2026-09-06-utopia-parser-andor-eqliteral]].
