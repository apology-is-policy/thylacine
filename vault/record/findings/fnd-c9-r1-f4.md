---
id: fnd-c9-r1-f4
type: fnd
round: adt-c9-r1
severity: P3
status: fixed
title: "I-31 and its spec mapping name a TTBR0 write that may not happen"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c9-r1
regression: "none: wording"
created: 2026-11-04
---
## Prosecution

ARCH section 28 and SPEC-TO-CODE order the publish before the TTBR0 write, and a same-space switch now writes nothing. The property holds: the hardware already holds the value published.

## Disposition

Both say the switch writes or keeps it.
