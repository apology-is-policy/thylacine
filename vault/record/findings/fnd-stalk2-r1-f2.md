---
id: fnd-stalk2-r1-f2
type: fnd
title: "A failed clone-walk abandons a monotonic fid number"
round: adt-stalk2-r1
severity: P3
status: deferred
surface: [sub-kernel-ninep-dev9p]
threatens: []
seam: seam-fid-monotonic-reclaim
created: 2026-08-01
---
## Prosecution

`dev9p_walk`'s failure branches (both `nname == 0` and `nname > 0`)
return without releasing the freshly-allocated fid NUMBER, and
`p9_client_alloc_fid` is purely monotonic. Pre-existing (the create
path and the 0-component SYS_OPEN already hit it); stalk-2 newly DRIVES
it on every mount cross via `clone_walk_zero`. Benign server-side (a
failed Twalk binds no server fid) — the cost is one number from a u32
space per failure.

## Disposition

Deferred → [[seam-fid-monotonic-reclaim]]: the proper fix is a Plan 9
devmnt-style fid free-list; a band-aid `p9_client_clunk` is WRONG
(clunking an unbound fid). RW-4 R3-F2 later re-registered the same
allocator with the burn-rate bound ([[fnd-rw4-rev3-f2]]); G2's dirfid
cache recycles walk-fresh DIRECTORY fids, a partial mitigation.
