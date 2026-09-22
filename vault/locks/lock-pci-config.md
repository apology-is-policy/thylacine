---
id: lock-pci-config
type: lock
title: "pci-config"
kind: spin-irqsave
guards: "PCI Command/MSI-X controls and terminal owner revoke"
orders-before: []
created: 2026-09-17
updated: 2026-09-17
---
## Discipline

IRQ-safe leaf for config reads and writes. All Command mutations use 16-bit accesses and readback plus device barrier. Quiescence drops this lock before acquiring the IRQ domain lock; endpoint operations acquire it after domain and rendez locks.
