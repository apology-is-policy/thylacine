---
id: adt-torpor8-r1
type: adt
title: "torpor sub-chunk 8 — the futex's embedded round"
date: 2026-05-23
scope: [sub-kernel-torpor]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: clean
counts: {p0: 0, p1: 0, p2: 2, p3: 10}
findings: [fnd-torpor8-r1-f1, fnd-torpor8-r1-f2]
round-of: chg-2026-05-23-torpor
created: 2026-08-01
---
## Scope

The whole primitive at birth: WAIT/WAKE state machines, the
stack-waiter lifetime, the uaccess primitive, the prose I-9 proof —
this was the chunk that broadened the spec-to-code suspension, so
the round WAS the formal rigor.

## Verdict

Clean: both P2s dispositioned (one fixed, one documented), ten P3s.
The audit's stated verdict on the prose validation: the I-9
specialization "captures the canonical no-lost-wakeup case correctly
and soundly", with the spinlock acquire/release pairing supplying
the happens-before edges — and it demanded the timeout-then-WAKE
case (c) be added to the proof, which it was (F3).

The P3 set shaped the surface more than most P2 sets do: F4's
`p == current` extinction assert, F7's no-barrier `WAKE(count=0)`,
F10's load-bearing alignment check each became permanent contract
lines.

## What this round could not see

Everything that came later on the same lock: the R-5 blocking-fault
hazard did not exist yet (no file-backed pages), the #343 osyield
storm had no Go runtime to generate it, and the stop-cascade misuse
(#19) had no job control to trigger it. The round was sound FOR ITS
TREE — the dossier's provenance chain is the reminder that three
subsequent eras each re-opened this surface.
