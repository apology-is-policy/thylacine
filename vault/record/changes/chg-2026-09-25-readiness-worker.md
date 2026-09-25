---
id: chg-2026-09-25-readiness-worker
type: chg
title: "Add bounded native readiness worker and repair its runtime harness"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-libthyla-rs, sub-kernel-syscall-abi, sub-substrate-machine, sub-substrate-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Adds a standalone native worker with owned descriptor duplicates, one-shot
readiness, generation and worker identity checks, bounded metadata and joined
shutdown. Real Pi/KVM descriptor tests and named disarming/owner-check negative
controls pass. The worker is not yet connected to the Halcyon service loop.

Qualification also repairs absent per-attempt pools when snapshots are unavailable,
pins modern MMIO transport in the QEMU launcher, and waits for complete failure
diagnostics. Unchanged-image controls and Linux/macOS fixture checks distinguish
those causes from the new worker. Exact results and activation obligations live
in the interaction status; no live clipboard is claimed.
