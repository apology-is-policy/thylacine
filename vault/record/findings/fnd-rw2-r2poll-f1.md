---
id: fnd-rw2-r2poll-f1
type: fnd
round: adt-rw2-wake-r1
severity: P3
status: documented
title: "The retain is INERT for KObj_Srv — listener-poll safety rests on the boot registry's immortality"
surface: [sub-kernel-poll]
threatens: []
seam: seam-poll-srv-registry-retain
regression: "none constructible today (the only registry is immortal); the seam is the tripwire"
created: 2026-08-01
---
## Prosecution

Round 2, prosecuting round 1's fix: poll.h claimed the retained ref
"keeps a SrvConn alive directly" — FALSE for the listener path.
`handle_acquire_obj`/`handle_release_obj` are NO-OPS for KObj_Srv,
so the `held[]` entry for a listener poll pins neither the
SrvService nor its registry. The only reason
`svc_listener_poll`'s hook list cannot be freed mid-sleep is that
the boot registry never dies (tombstones, never frees).

A mortal per-session registry — the A-5b direction, whose
`kfree(reg)` path already exists — revives round 1's UAF on exactly
this path.

## Disposition

The overclaiming comment fixed; the obligation tracked as
[[seam-poll-srv-registry-retain]]: whichever chunk makes a registry
mortal must add a real registry ref at register. The finding's value
is the tripwire — the inertness is invisible until the day it
detonates.
