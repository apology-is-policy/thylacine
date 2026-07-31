---
id: fnd-stalk3a-r1-f1
type: fnd
title: "devsrv roots carried no per-instance devno — every registry root had mount-key identity (s,0,0)"
round: adt-stalk3a-r1
severity: P3
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-06-02-stalk3a-registry
created: 2026-07-31
---
## Prosecution

`devsrv_attach_registry` minted roots without stamping a `devno`, so
every devsrv root — the boot registry's AND any future per-session
one — shared the mount-key identity `(dc='s', devno=0, qid.path=0)`.
Dormant with one registry (the mount table keys on the mount-POINT), but
the exact stalk-2 collision class lying in wait for the first second
registry: two indistinguishable roots defeat `(dc, devno, qid.path)`
keying and the mount-cycle source check.

## Disposition

Fixed in the audit close: `c->devno = spoor_next_devno()` per attach
(mirroring `dev9p_attach_client`) — devsrv became a multi-instance Dev
the day it grew multiple instances, not the day the collision fired.
Clones inherit the devno. Matrix re-verified green after the fix.
