---
id: seam-9p-tag-block-on-full
type: seam
title: "ARCH 21.5 says block-on-tag-full; as-built alloc_tag clean-fails"
status: closed
surface: [sub-kernel-ninep-session]
opened-by: adt-rw4-r1
tracker: "RW-4 R3-F3 register (scripture-vs-impl; user call)"
created: 2026-08-01
updated: 2026-10-07
closed-by: chg-2026-10-07-tag-pool
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

## As of 2026-10-07

- Closed by [[chg-2026-10-07-tag-pool]] ([[dec-2026-10-07-tag-pool]]): the
  block-on-full contract was built, not amended away. A sync op that finds no
  free tag waits for one, killably, instead of failing `-P9_E_IO`; the table
  grows in 64-entry chunks to the 16-bit tag space; ops and async ops each
  have a share, and a Tflush always finds a tag (ARCH 21.11).
- The wait ends because every holder of the op share is an op the server
  owes a reply or one of at most `P9_ASYNC_MAX` async ops, and the reader
  applies every sync reply when it reads it, so a stopped thread holds no
  tag ([[spec-tag-pool]]). A server that never answers holds the waiter as it
  holds any op; a death or a caught note ends the wait.
