---
id: seam-kstack-va-predictable
type: seam
title: "Kernel stack addresses are predictable from creation order"
status: open
surface: [sub-kernel-thread]
opened-by: fnd-c10-r1-f7
tracker: "OPEN-BUGS 2026-11-03T13:06Z"
created: 2026-11-04
updated: 2026-11-04
---
## Owed

The stack range is at a fixed address and slots go lowest first, so a thread's stack address follows from how many threads came before it. No invariant is breached: I-16 covers the image base, and raw stack addresses are capability-tiered.

## What closes it

A random start for the slot scan, aligned to 64 slots so a word's first bit still names its table, with a wrap in place of lowest-first. Or a decision that this hardening is not wanted, stated here.
