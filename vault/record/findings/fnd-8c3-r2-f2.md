---
id: fnd-8c3-r2-f2
type: fnd
title: "Comments claimed t->proc == NULL for kproc threads (wrong immunity)"
round: adt-8c3-r2
severity: P3
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-07-17-8c3-reader-role
created: 2026-07-31
---
## Prosecution

Three comments justified kproc stop-immunity via "t->proc == NULL" -- false
(a kproc thread's proc is kproc(), non-NULL). The REAL immunity is
kproc-undebuggability: proc_debug_stop_deliver rejects kproc, so its
debug_stop_req is always 0.

## Disposition

Fixed (three comments). A guard justified by the wrong mechanism invites a
refactor that preserves the stated reason while breaking the real one.
