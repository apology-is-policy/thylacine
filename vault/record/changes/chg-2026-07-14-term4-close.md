---
id: chg-2026-07-14-term4-close
type: chg
title: "term-4 audit close: G2 fid_gen before the take + the wstat comment"
date: 2026-07-14
arc: arc-go-build
commits: ["65ef4675"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The close commit of the batched term-4 round ([[adt-term4-r1]] over
G1/G2/G3/G4 + the Stratum A-1/A-2 buffered-reader chunks): the
self-found SA-P3 — the G2 consume path snapshotted `fid_gen` AFTER
`dirfid_take` (and two allocations), so an invalidation landing in the
serve→take window fell OUTSIDE the donate gate's scan (the era's code
leaned on the create/mkdir drop-hook backstop for exactly that window;
both prosecutors traced the backstop sound). Snapshot moved BEFORE the
take: a window event now yields at worst a false-stale clunk (the
fail-safe direction) — the gate airtight at its own layer. Plus F3: the
wstat invalidation comment overclaimed "truncate/size" (the handler
never sets a size; content truncate is OTRUNC-routed, which drops pages
— the attr-only invalidate was correct, the comment was not). The
Stratum-side F1/F2/F4 (the 64 KiB reader-buffer leak on the
thread-exhaustion teardown; two contract comments) closed on the Stratum
branch.
