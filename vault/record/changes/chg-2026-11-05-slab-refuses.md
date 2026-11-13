---
id: chg-2026-11-05-slab-refuses
type: chg
title: "A slab shortfall refuses: the four panic caches go"
date: 2026-11-05
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-kernel-mm-slub
  - sub-kernel-territory
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-05
---
I-32 says fail clean, never extinct, and four caches said otherwise: thread, proc, vma and territory carried a flag turning a slab shortfall into an extinction, under allocations any user can reach (a fork, a map, an unmap that cuts a mapping). Every caller already handled NULL, so the flag only kept those unwinds from ever running. It goes, and with it the only flag defined. Boot's own allocations keep their explicit extinctions.

The unwinds had never run, so `slab_refusal.*` runs them: a lever refuses one allocation from a named cache on the calling thread, after a chosen count. Not booted when written.
