---
id: seam-fid-monotonic-reclaim
type: seam
title: "The 9P fid allocator is monotonic — abandoned numbers never reclaim"
status: open
surface: [sub-kernel-ninep-client]
opened-by: fnd-stalk2-r1-f2
tracker: "RW-4 R3-F2 register"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`p9_client_alloc_fid` is purely monotonic. A failed clone-walk or a
partial walk abandons the reserved fid NUMBER (benign server-side — a
failed Twalk binds nothing there), and every mount cross's
`clone_walk_zero` drives the failure path on OOM. RW-4 R3-F2 bounded the
burn: ~1 day of pathological churn to 2^32 on the durable Stratum mount,
failing SAFE (-EIO wedge, remount recovers). The proper fix is a Plan 9
devmnt-style fid free-list (recycle reserved-but-unbound fids) — a
band-aid `p9_client_clunk` is WRONG (clunking an unbound fid).

## What closes it

The free-list hygiene chunk on `p9_client`. G2's dirfid cache
([[chg-2026-07-13-g2-dirfid]]) already recycles walk-fresh DIRECTORY
fids indefinitely for the by-name flow — a partial mitigation that
shrinks the steady-state burn but leaves the general allocator
monotonic.

## Risk while open

A bounded-uptime assumption on long-lived sessions (the 47-9p-client
caveat softened at RW-4 from "in practice never"); no leak, no
corruption, fail-safe wedge at exhaustion.
