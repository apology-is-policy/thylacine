---
id: chg-2026-06-10-ls5c-widen
type: chg
title: "LS-5c: torpor's early-exit predicate widens to death-or-terminate"
date: 2026-06-10
arc: arc-life-support
commits: ["9886704d"]
touched: [sub-kernel-torpor]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

The post-register re-check in `torpor_wait` widened from the #809
death predicate to `thread_die_pending` — death OR a pending
terminate-disposition `interrupt` (the LS-5 Ctrl-C leg). For the
interrupt leg the check is conservative-prompt only: the interrupt
waker does not take `torpor_lock`, so the airtight closure lives one
layer down in tsleep's register-then-observe under
[[lock-wait]] — a nuance the dossier
([[sub-kernel-torpor]]) records verbatim because it looks like a
missing-lock bug and is not.
