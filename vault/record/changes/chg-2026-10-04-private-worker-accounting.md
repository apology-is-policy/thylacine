---
id: chg-2026-10-04-private-worker-accounting
type: chg
title: "Preserve private worker charge identity through exec and reap"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-proc]
established: []
closed: []
opened: []
mirrors-checked:
  - "kernel/include/thylacine/proc.h: internal eight-byte ticket only; no userspace ABI change"
depth: skeletal
---
Private worker budget helpers use exact creator/image admission and permanent
stripes for serialized refund. Actual-source sanitizer/race checks, eight intended
mutants and fresh native1830/1830 pass. No private runtime activation; owner,
retirement, shared-memory charging and broad qualification remain next.
