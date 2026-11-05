---
id: fnd-c10-r1-f6
type: fnd
round: adt-c10-r1
severity: P3
status: fixed
title: "A body test-fault matches had been reworded"
surface: [abi-boot-banner]
threatens: []
fixed-by: chg-2026-11-04-c10-r1
regression: "fault variants kstack_overflow and overflow_stack_guard; sabotages K08, K11"
created: 2026-11-04
---
## Prosecution

Rewording a matched extinction body is a format break.

## Disposition

The match went back to the prefix `EXTINCTION: kernel stack overflow`, and the new wording is a separate required string.
