---
id: chg-2026-11-04-ut-stack
type: chg
title: "The shell asks the stack, and left-nested arithmetic parses"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-utopia-parser, sub-utopia-eval, sub-kernel-elf, sub-kernel-syscall-abi]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-04
---
Three recursion bounds multiplied past the 8 MiB stack: 10047 KiB to parse a 90 KB line, measured on a host build at the guest's opt-level. Bare `if` chains, from the same day's change, cost 1.33 KiB a level with no bound at all. A statement count now covers the chain, and a check on the real stack address holds every product, with the base read from AT_STACK_BASE. Each check has a line only it stops. A glob's walk became a worklist, so it needs no check. `((((1 + 2) * 3) + 4))` parses. Round 5's shell findings are here too: `~`, `matches` and `case` take a list as one pattern per element, Enter continues on a statement that wants its next line, `$1`, `$*` and `$#*` lex, an operator chain is bounded, `if ((a) && (b))` parses, `(x = 1)` parses, and a reserved word is refused in arithmetic. Host-tested; the stack and statement checks with mutants both ways, round 5's rows owed; `u-6-test`'s flows 8 and 10 are written, not booted.
