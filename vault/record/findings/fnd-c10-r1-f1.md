---
id: fnd-c10-r1-f1
type: fnd
round: adt-c10-r1
severity: P2
status: fixed
title: "The crash dump faulted on its two commonest shapes, the overflow path and every EL0-source extinction"
surface: [sub-kernel-halls]
threatens: []
fixed-by: chg-2026-11-04-c10-r1
regression: "thread.kstack_dump_stops_at_the_top; fault variants kstack_overflow and kstack_dump_at_top; sabotages K13, K14, K17"
created: 2026-11-04
---
## Prosecution

A dump that walks past where a stack ends reads the guard or the slot above and faults inside the dump, and the report it was printing is lost.

## Disposition

`halls_translates`, `halls_stack_dump_extent`, the frame-pointer walk asks too, and `exception_kstack_overflow` hands the dump the thread's SP. Self-found as well.
