---
id: fnd-c10-r1-f3
type: fnd
round: adt-c10-r1
severity: P3
status: fixed
title: "Witnesses were missing for x0's round trip, a frame straddling the guard, the IRQ slot and a secondary dying through the vector"
surface: [sub-kernel-exception]
threatens: []
fixed-by: chg-2026-11-04-c10-r1
regression: "fault variants kstack_overflow_frame, kstack_overflow_irq, kstack_overflow_peer; sabotages K18, K19"
created: 2026-11-04
---
## Prosecution

Those shapes had never been provoked.

## Disposition

New fault variants, each turned red by a sabotage. Item (e), that `thread.kstack_release_invalidates_a_warm_entry` cannot go red under TCG, is half read: K02 went red under HVF by a corruption the kernel caught at an earlier test, so the test's own line has not been seen red. It runs alone at C8b's lease.
