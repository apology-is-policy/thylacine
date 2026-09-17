---
id: lock-pci-irq-domain
type: lock
title: "pci-irq-domain"
kind: spin-irqsave
guards: "PCI endpoint weak membership and permanent GIC domains"
orders-before: [lock-rendez, lock-pci-config, lock-lpi-lease]
created: 2026-09-17
updated: 2026-09-17
---
## Discipline

IRQ-safe in create/dispatch/arm/complete/revoke/free. Never sleep while held. Dispatch increments in-flight pins before dropping the lock, then wakes and drops pins. Destruction first unlinks, then drains pins outside the lock.
