---
id: fnd-thread-usr-r1-f1
type: fnd
round: adt-thread-usr-r1
severity: P1
status: fixed
title: "The new full-ring test expected one slot too few and would fail on correct code"
surface: [sub-kernel-notes]
threatens: [inv-i19]
fixed-by: chg-2026-11-02-usr-notes
regression: "the test's corrected count, green on lease 2; K17 and K22 red on it"
created: 2026-11-02
---
## Prosecution

`notes.every_direct_family_fits_past_a_full_ring` filled the ring with interrupts naming sender `p`, then expected the moved interrupt to coalesce. A move coalesces only with a sender-0 entry (`notes_find_locked(q, name, 0u)`), so it takes a spare slot and the count is DEPTH + families. The kernel's bound is right and the expectation was the author's misreading. Found by reading, before the first boot.
