---
id: chg-2026-11-04-seat-takeover-fenced
type: chg
title: "Trusted takeover and restore wait for no fenced chain"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-04
---
Trusted takeover and restore no longer wait for a client's fenced chain (vote 50, [[dec-2026-11-03-scoped-retirement-and-friends]]). `Seat::step` phase 1 and `exclude_all_outputs` ask `Gpu::takeover_clear`, an idle cursor queue and no synchronous chain outstanding; `Device::restore` asks `Gpu::synchronous_clear`, the second half. Three sites, because restoring with the old rule would let the trusted scene up and then strand it behind the same poison. A stalled engine still holds the SAK off. Reproduction and witness: `ls-graphical-sak-poison`, which also fails now on a seat that refuses the takeover.
