---
id: fnd-c8b-timer-bases-r1-f3
type: fnd
round: adt-c8b-timer-bases-r1
severity: P3
status: fixed
title: "The timer base's accessors indexed per-CPU arrays with an unchecked CPU index"
surface: [sub-kernel-rendez]
threatens: []
fixed-by: chg-2026-11-03-c8b-timer-bases
regression: "none: unreachable"
created: 2026-11-03
---
## Prosecution

Every sibling guards smp_cpu_idx_self(); these did not. Unreachable today. Fixed: one guarded accessor, timer_base_of_this_cpu.
