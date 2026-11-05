---
id: chg-2026-11-05-as4-r7-shell-corrected
type: chg
title: "AS-4 round 7's shell chg, corrected"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched: [sub-utopia-eval]
established: []
closed: []
opened: []
mirrors-checked: []
supersedes: chg-2026-11-05-as4-r7-shell
depth: skeletal
created: 2026-11-05
---
Two statements in the note this supersedes, and in its commit 9ae7a94ae, were wrong. The rest of that note stands.

- The first test of F10 passed because `eval_command` runs no command while an interrupt is pending (eval/stmt.rs, the check marked SA-1). The note named `eval_block`, which only checks after a statement.
- The drop's largest composite, 431 KiB, is 60 nested `if` around a 60-level `case` value, in each of 32 substitutions. The commit called it 60 arms.
