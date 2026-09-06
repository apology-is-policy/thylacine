---
id: chg-2026-09-06-utopia-repl-doc-absorb
type: chg
title: "absorb docs/reference/108-utopia-repl (ut REPL loop): zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/108-utopia-repl.md -> ABSORBED

Absorbed the 357-line ut-REPL reference doc into a multi-redirect stub. The
three-tier split (editor/REPL/eval) maps to the three utopia dossiers, all
current: the REPL loop + pure line editor + prompt + U-7 job-reaping ->
sub-utopia-interactive (owns repl.rs); the parse half -> sub-utopia-parser; the
eval half (run_line) -> sub-utopia-eval. Zero fold.

100 -> 101 absorbed of 157. lint 0-fail.
