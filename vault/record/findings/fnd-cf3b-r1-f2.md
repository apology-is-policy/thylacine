---
id: fnd-cf3b-r1-f2
type: fnd
title: "The role-wait conds' || eof term made a teardown-woken contender busy-spin against the unwinding holder"
round: adt-cf3b-r1
severity: P3
status: fixed
surface: [sub-kernel-srvconn]
threatens: []
fixed-by: chg-2026-07-08-cf3b-bulk-ring
created: 2026-07-31
---
## Prosecution

The role conds were `!role || eof`. A contender woken by teardown while
the unwinding holder still held the role found the cond instantly true →
tsleep returned AWOKEN without sleeping → register/re-check spin until
the holder got scheduled. Bounded (one slice, self-healing — no hang),
but a hot loop on the wake path.

## Disposition

Fixed in-commit: the role conds wait purely on role-free; liveness rests
on the holder's GUARANTEED release (teardown wakes the holder via
rendez/wrendez; every holder exit path releases; the release wakes the
contenders). The teardown-time role-list wake stays as defense in depth.
Deliberately the OPPOSITE call from the chan conds' kept `|| eof`
([[fnd-348-r1-f3]]) — the pairing is a standing prosecution item on
[[sub-kernel-srvconn]].
