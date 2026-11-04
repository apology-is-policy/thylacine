---
id: chg-2026-11-05-ut-tree-walks
type: chg
title: "A parsed tree is shared, not copied, and its operators drop without recursing"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched: [sub-utopia-parser, sub-utopia-eval]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
The stack check bounded the parser's and the evaluator's own recursion, and left two the compiler writes. A derived Clone copied a function's body at declaration and at every call, 14 MiB of stack for a tree parsed within 8. Drop recursed over a left-deep chain that cost the parser no stack, and the per-parser count of round 5 could not bound that because a group starts a new parser. No type in the tree is Clone now, a function and a note handler are Rc's shared with it, an operator's child drops with a worklist, and the reserve is 1 MiB because the deepest body measured takes 511 KiB to drop. Red before this: a 48 KB function declared at 8 MiB, ten substitutions of chains dropped at 8 MiB. Ten mutant rows red; flow 11 owes its first boot.
