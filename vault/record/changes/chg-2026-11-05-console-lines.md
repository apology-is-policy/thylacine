---
id: chg-2026-11-05-console-lines
type: chg
title: "console: a cooked read ends at its line, a control byte echoes as a caret"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-kernel-cons
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
The console had the two shapes `ptyfs` just lost. `cons_input_read` drained past a newline, so a reader taking one line lost the next, and `cons_echo_stage` echoed a control byte as itself, which drives the terminal instead of showing what is in the line. A canonical read now ends at its line, and a control byte in a canonical line echoes as `^X` and erases as two columns. Raw reads and raw echo are unchanged. The console's line limit stays at 256, its ring 512: raising them is a separate question, and only `ptyfs` was asked to.

The one test that read two lines at once is rewritten. `cons.read_ends_at_its_line` and `cons.control_byte_echoes_as_caret` pin it, five mutants in `sabotage-i32.py`, not yet run.
