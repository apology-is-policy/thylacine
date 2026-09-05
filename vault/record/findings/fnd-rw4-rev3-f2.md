---
id: fnd-rw4-rev3-f2
type: fnd
title: "RW-4 R3-F2: the monotonic fid allocator never reclaims — a bounded-uptime assumption"
round: adt-rw4-r1
severity: P3
status: deferred
surface: [sub-kernel-ninep-client]
threatens: []
seam: seam-fid-monotonic-reclaim
created: 2026-08-01
---
## Prosecution

`p9_client_alloc_fid` never reclaims clunked or abandoned numbers:
pathological churn reaches 2^32 in ~a day on the durable Stratum mount,
then fails SAFE (-EIO wedge; remount recovers). The 47-9p-client doc
claimed "in practice never" — an unbounded-uptime overclaim.

## Disposition

Registered (H3), not fixed in-arc: the v1.x fix is a Plan 9
devmnt-style fid free-list. The doc claim was softened to the
bounded-uptime assumption at the close. Same seam as the stalk-2-era
[[fnd-stalk2-r1-f2]] (which drives the abandonment path on failed
crosses); G2's dirfid cache later recycled walk-fresh DIRECTORY fids —
a partial mitigation, the general allocator unchanged
(grep-re-verified monotonic at the vault sweep).
