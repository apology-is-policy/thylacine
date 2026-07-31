---
id: fnd-stalk3a-r1-f4
type: fnd
title: "The per-Proc srv_conn_count decrement must move with the open=connect migration"
round: adt-stalk3a-r1
severity: P3
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-06-03-stalk3b-open-connect
created: 2026-07-31
---
## Prosecution

A forward note at 3a: when stalk-3b lifted `SRV_CONN_PER_PROC_MAX`, the
per-Proc counter's increment (in the connect core) and decrement (in
`handle_close`) had to move or generalize together — silently dropping
only the decrement leaves the counter climbing until the Proc can never
connect again.

## Disposition

Closed by stalk-3b-D taking the OTHER exit: the per-Proc cap (and the
counter, and both its mutation sites) was REMOVED entirely — a
deliberate decision (a session needs corvus AND its stratum-fs
concurrently), collapsing `srv_conn_count` to a reserved pad with
`struct Proc`'s size + offsets asserted unchanged. The fairness
consequence of cap removal is [[fnd-stalk3b-r1-f3]].
