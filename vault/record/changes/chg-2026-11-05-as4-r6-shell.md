---
id: chg-2026-11-05-as4-r6-shell
type: chg
title: "AS-4 audit round 6, the shell's half"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched: [sub-utopia-parser, sub-utopia-eval, sub-utopia-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
Round 6, Opus on Opus, 1 P0 + 3 P2 + 9 P3, all read from the code. The P0 was flow 10 itself, which could not pass: errstr held a Debug form and the probe asked for the Display text, so every probe image would have failed its boot, and this was caught before any boot. Fixed: errstr carries the Display message; a positional reads only the innermost frame a call bound `*` in; Enter reports what no further line can finish, by a new kind for the end of a closed group, and waits on a trailing connector and a heredoc; `balance` knows `$#` and `$"`; a digit run past i64 is an invalid literal rather than a recursion; a group opens at a later `((` and inline arithmetic inside arithmetic parses; a later error prints its own kind; `^` stops at 64 MiB of product. Corrected the reserve's stated reason: no statement runs beneath an expression, so no tree is dropped there. 19 mutant rows red, one unbounded mutant reset the Pi, so those run capped now. Flows 10 and 11 still owe a boot, and round 7 is owed after it.
