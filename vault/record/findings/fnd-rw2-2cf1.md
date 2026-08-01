---
id: fnd-rw2-2cf1
type: fnd
round: adt-rw2-wake-r1
severity: P1
status: fixed
title: "A registered poll waiter outlives the obj ref — sibling-close mid-sleep frees the hook list"
surface: [sub-kernel-poll]
threatens: [inv-i9]
fixed-by: chg-2026-06-10-rw2-poll-retain
regression: "the retain is structural (held[] + the sweep order); the SMP gate is the witness"
created: 2026-08-01
---
## Prosecution

The dangerous window is not "between the two scans" — it is the
ENTIRE sleep: the register scan lists a STACK waiter on the polled
object's embedded `poll_waiter_list`, then the poller parks. Since
P6-pouch-threads, a SIBLING thread sharing the handle table can
`handle_close` the last handle to that object DURING the park —
`spoor_clunk`/`srvconn_unref` frees the object and the embedded list
with it. The still-listed stack waiter's `pw->list` now dangles: the
sweep spin-locks freed memory; a concurrent producer walks a freed
chain.

This is [[fnd-poll-r1-f3]]'s documented precondition, voided and
detonated — the class fix the P5 round deferred by documentation.

## Fix

`poll_scan_one` transfers the #844 `handle_get` obj snapshot (ref
HELD) into `held[i]` whenever it actually registered
(`pw->list != NULL`); `sys_poll_for_proc` releases every retained
ref only AFTER the unregister sweep. Transitively sufficient for
both real registering paths (pipe ring, devsrv conn — each frees its
embedded list only at the Spoor's last clunk). The KObj_Srv listener
gap is the round-2 finding ([[fnd-rw2-r2poll-f1]]).
