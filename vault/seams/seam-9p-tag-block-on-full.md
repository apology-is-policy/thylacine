---
id: seam-9p-tag-block-on-full
type: seam
title: "ARCH 21.5 says block-on-tag-full; as-built alloc_tag clean-fails"
status: open
surface: [sub-kernel-ninep-session]
opened-by: adt-rw4-r1
tracker: "RW-4 R3-F3 register (scripture-vs-impl; user call)"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A scripture reconcile, not a code fix per se: ARCH §21.5 commits "block
until a slot frees" when all 64 tags are outstanding; as-built
`alloc_tag` returns -1 → the op fails -EIO (clean-fail, the
#841-F3/SRVCONN_RING_CAP envelope). v1.0 never reaches 64 in-flight;
a heavily multi-in-flight workload would hand the 65th op a spurious
-EIO. Registered at RW-4 as a USER/scripture decision: either build
block-on-full (a new park/wake leg on the tag table) or amend §21.5 to
the clean-fail contract.

## What closes it

The user's vote, then either the amendment chg or the park/wake chunk
(which would be audit-bearing on the wait/wake lineage). Note the
adjacent machinery has since grown: #52/#53 added `abort_unsent` +
the pre-send free-tag drain for clunks — the classification refinements
sit BELOW this contract question and do not decide it.

## Risk while open

A spurious per-op -EIO under >64 concurrent in-flight ops on one
session — unreached by any current workload; fail-safe when reached.
