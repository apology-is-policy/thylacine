---
id: lock-gic-msi
type: lock
title: "gic-msi"
kind: spin-irqsave
guards: "MSI vector allocation states, generations and quota"
orders-before: []
created: 2026-09-17
updated: 2026-09-17
---
## Discipline

Allocation and retirement state transitions only. Controller drain runs after
publishing RETIRING and dropping the lock; completion or quarantine is then
published under the lock. No device/controller wait or endpoint wake while held.
