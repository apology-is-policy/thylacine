---
id: fnd-r15b-f234
type: fnd
round: adt-r15b-r1
severity: P2
status: fixed
title: "pipe_ring.ref was a plain -- : concurrent endpoint closes could double-free the ring"
surface: [sub-kernel-pipe]
threatens: []
fixed-by: chg-2026-05-14-r15b-atomic-refs
regression: "the ref-underflow extinction; the close-both-ends test"
created: 2026-08-01
---
## Prosecution

Two CPUs closing the two endpoints of one pipe concurrently ran
`r->ref--` as a plain RMW: a lost update leaks the ring forever, or
both closers observe the same pre-value and BOTH take the free path
— a double `kfree` of a live-shaped 4 KiB object, the allocator-
corruption class the AEGIS hunt taught the project to fear.

## Fix

`__atomic_fetch_sub` ACQ_REL; pre == 1 owns the free, pre <= 0
extincts (underflow is corruption, not an error). Init is relaxed —
the ring is unpublished until `pipe_create` returns. Fixed before
SMP was on; nobody ever debugged it live.
