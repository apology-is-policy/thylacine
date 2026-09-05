---
id: lock-burrow
type: lock
title: "Burrow.lock — the dual-refcount and free-decision guard"
kind: spin
orders-before: [lock-buddy-zone]
guards: "one Burrow's handle_count, mapping_count, the both-counts-zero free decision, and the sparse per-page slot array"
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

A per-Burrow spinlock guarding the dual refcount and — the part that matters —
**the free decision computed from it**. Before it existed the two counts were
plain `++`/`--` and the `handle_count == 0 && mapping_count == 0` test appeared
twice, in the handle-drop path and the mapping-drop path, unsynchronized: two
Threads of one Proc dropping the last handle and the last mapping concurrently
could both observe zero-zero and free twice, or interleave their decrements and
neither observe it and leak.

The discipline is therefore narrow and total: **decrement and read the free
decision under the lock, then free outside it.** The `should_free` boolean
crossing the unlock is the whole mechanism — exactly one of the racing droppers
carries a true out.

`burrow_free_internal` runs **outside** the lock, and must, because it reaches
for the buddy allocator, the hardware-object refcounts, and the 9P Spoor clunk.
That is the leaf discipline: order is `vma_lock -> Burrow.lock -> buddy zone`,
and freeing under the Burrow lock would nest the buddy lock inside it on the
free path while the map path nests it outside — the cycle the split avoids.

Inbound it sits below the per-Proc handle table lock (a handle acquire or dup
takes the table lock first, then this), and below [[lock-vma]]. Never the
reverse.

Two guards live under the lock beyond the counts, and both are UAF defenses
rather than bookkeeping: a **both-counts-zero check on every acquire**, which
catches resurrection of an already-freed identity, and a **per-type liveness
switch** reading the backing field for the Burrow's own type. The liveness
switch was originally outside the lock — safe only while the sole caller held a
handle, which stopped being an argument once a sibling Thread could free the
backing resource concurrently.

The sparse slot array used by the file-backed and lazy-anon types is also under
this lock for read and install, with the blocking part of a page-in done
outside and the result installed on re-entry. See [[sub-kernel-burrow]].
