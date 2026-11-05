---
id: fnd-c10-r1-f7
type: fnd
round: adt-c10-r1
severity: P3
status: deferred
title: "Stack addresses are predictable from creation order"
surface: [sub-kernel-thread]
threatens: []
seam: seam-kstack-va-predictable
created: 2026-11-04
---
## Prosecution

The range is fixed and slots go lowest first. No invariant is breached: I-16 is the image base, and raw stack addresses are capability-tiered.

## Disposition

Not done in C10. A random start breaks a word's first bit naming its table unless aligned to 64 slots, and lowest-first would need a wrap. Tracked in OPEN-BUGS.
