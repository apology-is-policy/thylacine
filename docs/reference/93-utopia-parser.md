# 93 — libutopia::parser: the rc-shape parser stack for `ut` [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-07-utopia-parser-doc-absorb`).
`usr/utopia/libutopia/src/parser/` — the only path from "user typed a line" to
"AST the evaluator consumes": tokenizer (U-5a), statement grammar + AST (U-5b),
Pratt-style expression parser (U-5c), and pattern-matching / try-catch / trace /
on-mask / case-as-expression (U-5d). A pure-logic engine — no I/O, no syscalls.
Its content lives, code-verified and current, in:

    vault/system/userspace/shell-tui/sub-utopia-parser.md   (audit: none)

The dossier carries, as-built: the four entry points
(`tokenize`/`parse`/`parse_tokens`/`parse_expr_tokens`), the eight lexer
surfaces and where lexical disambiguation declines to the parser (`^` and `%`),
the queued-not-backtracked heredoc + one-shot-regex state, the structural (not
checked) UTF-8 span contract, substitution-bodies-re-parsed-not-in-place, the AST
shape, and the 29-variant error taxonomy. Most of what is interesting is the one
real hazard:

- **The three recursion bounds (RW-9).** A recursive-descent parser in a `no_std`
  program has no stack guard of its own, so a deep-enough parse is a guard-page
  fault that terminates the shell — which takes the user's session with it. Three
  separate bounds exist because the recursion has three shapes and **no single
  counter sees all of them**: bracket nesting (64, a flat pre-pass over the token
  stream), operator recursion (256, for `a**b**c` and `!!!!` chains no bracket
  counts — added after an audit round found the pre-pass blind to it), and re-lex
  depth (32, a process-global atomic, because a whole `$(…)` substitution is one
  token in the outer stream so the pre-pass sees depth 1). All trip to
  `RecursionLimit` — a message, never a dead shell.

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Clean redirect — the dossier is AHEAD of this doc.** Two grammar changes
  post-date the U-5a..d this doc froze at: the **AND-OR list** (`p && q || r`,
  scripture 8.6 — a `first` pipeline plus a `Vec<(AndOrOp, Pipeline)>`, built only
  when a connector is present) and the **`UnexpectedEqualInCommand` retirement** —
  this doc's "locked decision #1" (strict rc: `--key=value` is a parse error) is
  no longer true; `=` in argument position is now a literal word glued by
  span-adjacency (`-std=c++20`), and the error kind survives only for its
  `Display` arm. Read the dossier for the current grammar.
- **The host-test claim in the lexer header is false, and the dossier says so.**
  189 (measured 394) of this crate's parser tests cannot compile: the workspace
  pins a bare-metal target, the crate is unconditionally `no_std`, and libthyla-rs
  (depended on unconditionally) blocks the host-build escape. The parser is
  covered instead by an in-guest probe that drives the public entry points every
  boot (task #105).
- **Everything else was covered** — the token taxonomy, the Pratt precedence
  chain, the arith retokenization, the eager substitution-body lifting, the
  var-indexing, and the body-relative-span / orphaned-doc-comment / nesting-counter
  caveats are all as-built in the dossier. Zero code change.
