---
id: fnd-8c3-r2-f1
type: fnd
title: "The boundary stop classifier races an async proc_debug_resume"
round: adt-8c3-r2
severity: P1
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-07-17-8c3-reader-role
regression: "seam-841-mi-harness (kproc-infeasible deterministically; SMP gate + model + code-trace are the durable coverage)"
created: 2026-07-31
---
## Prosecution

The detour returns SLEEP_INTR (byte-identical to a death-interrupt and a
transport error), so the client re-derived "was it a stop?" by re-reading
debug_stop_req. But proc_debug_resume clears that flag under
g_proc_table_lock -- NOT c->lock -- so a debugger dying mid-stop in the
recv-return -> classify window turns a benign stop-unwind into
client_mark_dead_locked of the shared SYSTEM session (whole-FS DoS).
Reachable across all three EL0 recv classifiers.

## Disposition

Fixed: the stable per-Thread stop_unwound latch -- set by the detour's
unwind branch, reset at reader_recv_frame ENTRY, read+cleared by the same
reader thread. Owner-only access closes the race by construction; the
loop-top park checks keep the racy read (benign there: a resumed park
returns immediately).
