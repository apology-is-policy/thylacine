---
id: seam-death-cascade-smp-harness
type: seam
title: "No deterministic test reaches the 3-way SMP death interleaving"
status: open
surface: [sub-kernel-death]
opened-by: chg-2026-06-01-811-death-interruptible
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A deterministic harness for the full cascade at `-smp > 1`: the walk plus
the broadcast IPI plus the last-out reap, with a genuine 3-way interleaving
— a cascade walking while a remote peer is mid-`thread_exit_self` blocking
on [[lock-proc-table]] while a third resumes and clears `on_cpu`.

The in-kernel harness cannot produce it: the broadcast wakes idle
secondaries and races `thread_free` under a deterministic single-CPU
regime, so `rendez.death_interrupts_sleep` deliberately reproduces only the
per-sleeper effect.

## What closes it

The same class of harness owed across the tree for concurrency
interleavings — see [[seam-841-mi-harness]], which owes the multi-in-flight
9P equivalent. Both want a way to STAGE an interleaving rather than hope for
it. A TSan pass over a stress run is the other candidate and was flagged
explicitly at the #811 close.

## Risk while open

The #811 prosecutor named this its lowest-confidence-by-static-reading area,
while finding the multi-CPU path sound (every mutator serializes on the
table lock). Coverage today is [[gate-smp]] plus the boot path — empirical,
repeated, and unable to distinguish "correct" from "did not happen to race".
