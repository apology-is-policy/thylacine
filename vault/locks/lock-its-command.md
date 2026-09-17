---
id: lock-its-command
type: lock
title: "ITS command ring"
kind: spin-irqsave
guards: "one controller's command producer/consumer accounting and ring writes"
orders-before: []
created: 2026-09-17
updated: 2026-09-17
---
## Discipline

Submit at most four commands, preserving one empty slot. CREADR is sampled once
under the lock; no hardware-completion wait occurs while held. Waiters poll
progress in separate short critical sections and fail after 100ms. A stalled,
impossible or timed-out read faults the controller. Ring memory is boot-lifetime.
