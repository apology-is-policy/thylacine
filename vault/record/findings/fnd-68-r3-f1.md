---
id: fnd-68-r3-f1
type: fnd
title: "handle.c's lockless-safety justification went stale under the fix that needed it"
round: adt-68-r3
severity: P3
status: fixed
surface: [sub-kernel-death]
threatens: []
fixed-by: chg-2026-07-14-68-last-thread-out-close
regression: "none -- a comment"
created: 2026-08-01
---
## Prosecution

`handle_table_free`'s lockless-safety argument rested on a `thread_count <= 1`
premise and an enumeration that said "SINGLE-thread exit". The #68 change
invalidated both: `thread_count` may now exceed 1 at the close, and the
close now runs on multi-thread and killed paths too.

The file is the one carrying the #66c cross-Proc FOOTGUN warning — i.e. the
place a future author goes to learn whether a cross-Proc `/proc/<pid>/fd`
reader would be safe. A stale premise there is worse than a stale premise
elsewhere.

## Disposition

FIXED: reworded to the live_peers formulation — `thread_count` may exceed 1;
exactly ONE live thread performs the close; EXITING tails never touch the
table.

Worth noting the pattern rather than the instance: the fix's own soundness
argument lived in a different file from the fix, and only a third round
found the drift.
