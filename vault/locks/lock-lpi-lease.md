---
id: lock-lpi-lease
type: lock
title: "LPI leases and properties"
kind: spin-irqsave
guards: "LPI lease state, generation identity and property-byte updates"
orders-before: [lock-its-command]
created: 2026-09-17
updated: 2026-09-17
---
## Discipline

State transitions and property stores/cleaning only. IRQ-safe enable/disable
may enqueue an asynchronous INV under the command lock; it never waits for
completion. Allocation and retirement publish BUILDING or RETIRING, then drop
the lock before command waits and CPU IRQ barriers. Failure publishes quarantine;
successful retirement publishes FREE only after all hardware/CPU proofs.
