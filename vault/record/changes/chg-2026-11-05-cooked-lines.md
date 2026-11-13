---
id: chg-2026-11-05-cooked-lines
type: chg
title: "ptyfs: a cooked read ends at its line, a control byte echoes as a caret"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-ptyfs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
Three of AS-4 audit round 3's P3s, fixed where the fix is local. A cooked read took every line in the ring, so `head -n 1; next` lost the second; it ends at its line now, as Linux does. A control byte in a cooked window went to the terminal raw, so an arrow key moved the cursor and Alt-] left a vt in an OSC; it echoes as `^X` and erases as two columns. The line bound goes from 256 to N_TTY's 4095, so a machine-typed line landing in a cooked window reaches its end far more often. The kernel console keeps its 256, and its reads are unbounded by line too, enqueued. The battery at ptyfs's start grows steps 10 and 11. Not booted when written.
