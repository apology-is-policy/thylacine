---
id: chg-2026-11-03-sigpipe-row
type: chg
title: "Pouch's SIGPIPE default is a row that ignores"
date: 2026-11-03
arc: arc-phase6-pouch
commits: []
touched:
  - sub-pouch-signal
established: [dec-2026-11-03-sigpipe-row]
closed: []
opened: []
depth: skeletal
created: 2026-11-03
---
Operator vote 25, on [[fnd-note-wait-r1-f6]]. The default was a mask bit, which the one restore at a handler's return undid when set in a handler. `SIG_DFL` and `SIG_IGN` now write the same ignore row, the constructor writes it, and the dispatcher writes it back after a one-shot. In patch 0071. seam-pouch-sigmask-per-thread closes at the chunk's landing, once `pipe-thread` has run.
