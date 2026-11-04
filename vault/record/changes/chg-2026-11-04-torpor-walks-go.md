---
id: chg-2026-11-04-torpor-walks-go
type: chg
title: "The two torpor walks and the bucket-lock die check go"
date: 2026-11-04
arc: arc-phase6-pouch
commits: [3e2adf489]
touched:
  - sub-kernel-torpor
  - sub-kernel-death
established:
  - dec-2026-11-04-torpor-walks-go
closed: []
opened: []
depth: skeletal
created: 2026-11-04
---
`proc_death_wake_locked` and `proc_stop_wake_sleepers_locked` keep only the per-peer rendez wake, and
WAIT goes straight from linking to `tsleep_noteintr`. ARCH 7.9.1 and the lock order in 7.3.1 say so.
`specs/torpor.tla` models a stop's wake without naming who sends it, so only its mapping moved.
