---
id: chg-2026-09-07-utopia-parser-doc-absorb
type: chg
title: "absorb docs/reference/93-utopia-parser (the rc-shape parser stack): clean redirect -- sub-utopia-parser is fresh + ahead"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
libutopia::parser (the ut rc-shape lexer/parser/expression stack, U-5a..d).
quaestor owner: usr/utopia/libutopia/src/parser/{lexer,parse,expr,ast,...}.rs ->
sub-utopia-parser (audit:none, fresh 2026-09-06). ALL 4 paths OWNED. Verified
atom-by-atom.

ALREADY COVERED (verified, dossier fresh + deep): the four entry points; the
eight lexer surfaces + the lexer-declines-to-parser cases (^ and %); queued
heredoc + one-shot regex state; the structural UTF-8 span contract; substitution-
bodies-re-parsed; the AST shape + 29-variant error taxonomy; and -- the load-
bearing atom -- the THREE RW-9 recursion bounds (bracket-nesting 64 pre-pass /
operator-recursion 256 field / re-lex-depth 32 atomic; no single counter sees all
three; the middle bound added after an audit found the pre-pass blind to a**b**c),
all tripping to RecursionLimit (a message, never a dead shell -- the DoS hazard in
a panic=abort no_std shell). Also the #105 stranded-tests + #104 body-relative-span
caveats.

CLEAN REDIRECT, zero fold: the dossier is AHEAD of the doc. Two post-U-5 grammar
changes the doc froze before: the scripture-8.6 AND-OR list (p && q || r) and the
UnexpectedEqualInCommand RETIREMENT (the doc's locked decision #1 "strict rc:
--key=value is a parse error" is no longer true -- = in arg position is now a
literal word glued by span-adjacency, -std=c++20; the error kind survives only for
Display). Redirect stub notes both. Zero code change.
