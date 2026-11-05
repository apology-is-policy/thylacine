---
id: chg-2026-11-05-as4-r7-shell
type: chg
title: "AS-4 audit round 7, the shell's half"
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
Round 7, Opus on Opus: 0 P0, 0 P1, 1 P2, 9 P3, read from the code. It found no way the merged tree fails to boot. Eight findings fall here.

- **F1 (P2):** round 6's heredoc wait exposed `balance` reading a body as shell, so a body holding `it's` or `f() {` could never submit. Bodies are removed first, using spans the lexer records before it parses a body.
- **F2:** a connector after a heredoc opener now skips the body.
- **F3:** when the parser accepts a line, only `[` and a trailing `\` still wait. A refused line that the walk misreads still waits until Ctrl-C; that is the stated remainder.
- **F4:** `try { a }` at the end of input waits for its catch.
- **F5:** the mark on `$errstr` moved from each line to each statement.
- **F6:** flow 10 has 24 groups, and both flows' reds name what they saw.
- **F7:** the reserve's stated reason was wrong. A statement can reach it, and a function's old body can drop there.
- **F10:** a function call polls for an interrupt on the loop's stride. The first test could not fail, because `eval_block` stops on a pending interrupt before any call runs. Its replacement counts the call that stops, and ls-5 gains a Ctrl-C leg through the real note queue.
