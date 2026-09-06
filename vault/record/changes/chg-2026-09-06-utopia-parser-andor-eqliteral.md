---
id: chg-2026-09-06-utopia-parser-andor-eqliteral
type: chg
title: "sub-utopia-parser de-stale: the && / || AND-OR list grammar (scripture 8.6) and = as a literal command argument (UnexpectedEqualInCommand retired as a raise)"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-utopia-parser
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-utopia-parser]] (updated 2026-08-03) missed exactly two settled shell-grammar
commits -- verified purely main, no aux (topological `ea93d8b7~1..HEAD` on
`parser/`, both author-flagged "Vault surfaces ... dossier notes owed"):

- **AND-OR lists** (`ea93d8b7`, scripture 8.6). New `StatementKind::AndOr(Box<AndOrList>)`
  + `enum AndOrOp { And, Or }` + `struct AndOrList { first: Pipeline, rest:
  Vec<(AndOrOp, Pipeline)>, span }` in `ast.rs`; `parse_pipeline_statement` now
  parses `pipeline (( && | || ) pipeline)*` (a `loop`, so no new recursion shape --
  the three bounds are untouched), building `AndOr` only when a connector is
  present so a lone pipeline keeps its `Pipeline` shape. Left-associative, equal
  precedence; a newline is allowed after the connector. (The short-circuit
  EVALUATION -- `eval_and_or`, the `should_propagate_failure` arm -- is `stmt.rs`,
  the [[sub-utopia-eval]] dossier's half, still owed.) Folded into Data structures.
- **`=` as a literal command argument** (`fd4c59ae`). `parse_simple_command`'s
  `Equal` arm parses a literal word instead of raising `UnexpectedEqualInCommand`;
  `parse_word` glues a span-adjacent `=` and its value into one argv element
  (`-std=c++20`), the same span-adjacency rule that fused `~/path`. A statement-
  start assignment is still caught earlier by `is_assignment_start` (IDENT Equal
  two-token lookahead -> `parse_assign`), so any `=` reaching argument gathering is
  literal. `UnexpectedEqualInCommand` is now VESTIGIAL -- kept for its `Display`
  arm, never raised. Folded into Error paths.

MEASURED counts corrected: parser `#[test]` 188 -> **189** (`fd4c59ae` removed
`equal_in_command_position_errors`, added two: net +1) -- the title and the
Caveats opening; and the Caveats' cross-crate stranded figure "this one's 385"
-> **394** (the libutopia crate total now, moved by this + the eval-side shell
arc), so "389 across two crates" -> **398** (394 + tapestryd's 4, unchanged).
`ParseErrorKind` stays **29** (the vestigial variant is kept, not removed).

`updated:` -> 2026-09-06. Stale backlog 33 -> 32.
