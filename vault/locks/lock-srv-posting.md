---
id: lock-srv-posting
type: lock
title: "Proc posting transaction and death latch"
kind: spin-irqsave
orders-before: [lock-srv-registry-lock]
guards: "Proc.srv_posts_closed and the deduplicated registry-membership list; publication against poster death."
created: 2026-10-01
updated: 2026-10-01
---
## Discipline

Posting allocates its candidate membership before locking, then holds this
lock through registry reservation, listener-table installation and commit.
Registry and handle-table locks are entered separately, never nested together.
A new membership takes a covering registry ref before unlock; unused candidate
storage and rollback refs are released after unlock. Death closes the latch
and detaches the whole list while locked, then drains each registry and drops
its references after unlock. No new post can escape that detached list.
Neither mutable namespace lookup nor the process-table lock participates.
See [[sub-kernel-devsrv]] and [[sub-kernel-death]].
