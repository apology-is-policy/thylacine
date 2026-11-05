---
id: chg-2026-11-04-ut-language
type: chg
title: "The shell's documented forms parse, and its stranded tests run"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-utopia-parser, sub-utopia-eval, sub-substrate-gates, sub-corvus, sub-kernel-syscall-abi]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-04
---
libthyla-rs gained `host-test`, libutopia's last 69 tests and corvus's 3 now run on a host, and the first run found documented syntax that did not parse. Closers follow their openers in the lexer, an arithmetic `<<` token, `in` takes a written list, a braceless `if`, an index in a command word, rc's `~`, and a reserved word as a value. `=~` stays refused. Audit round 4's F5 and F9 are here. Host-tested only, not booted.
