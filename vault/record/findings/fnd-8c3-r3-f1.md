---
id: fnd-8c3-r3-f1
type: fnd
title: "The fix's own doc-rot: comments described the removed re-read"
round: adt-8c3-r3
severity: P3
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-07-17-8c3-reader-role
created: 2026-07-31
---
## Prosecution

Two comments (sched.c + thread.h) still described the REMOVED
client_stop_pending re-read classification -- a reintroduction hazard for
the exact race the latch closed.

## Disposition

Fixed: both reworded to the stop_unwound latch; an optional symmetry clear
added to client_wait's dying branch. Behaviorally inert (rebuild + suite
confirm).
