---
id: seam-loom-sqpoll-owner-unbackstopped
type: seam
title: "The SQPOLL owner pointer rests on a backstopped argument without the backstop"
status: closed
surface: sub-kernel-loom
opened-by: chg-2026-08-16-loom-charge-ledger
tracker: "unfiled -- yip to main 2026-08-16"
created: 2026-08-16
updated: 2026-08-16
closed-by: chg-2026-08-16-loom-backstop-closed
---
## Owed

A ring stores the creating Proc twice — once for the page ledger, once for the
thread ledger — and both rest on the **same** lifetime argument: the ring handle
is non-transferable and non-dup-able, so a ring is reachable only through its
creator's handle table, which is torn down while that Proc's structure is still
allocated.

The page-ledger pointer does **not** trust that argument. It is validated at
every use against the Proc's magic word and a stored pid, and the code says why
in as many words: this is an argument, not an enforced invariant, so a future
violation should degrade into a skipped refund rather than a write through a
dangling pointer.

The thread-ledger pointer is dereferenced bare, at the same call site, inside
the same teardown function, about forty lines away — and its use is a **write**
(a decrement of the Proc's poll-thread count under the process-table lock), not
a read.

## What closes it

Route the thread-ledger settle through the same liveness check the page ledger
already uses. It is a static helper in the same file and the value is already
the same Proc, so this is a substitution, not a mechanism.

It must remain a **separate stored pointer**. The two are bound at opposite ends
of setup on purpose — the page owner last so rollbacks cannot double-refund, the
thread owner first so teardown settles what rollbacks deliberately do not. A fix
that merges them to remove the duplication breaks one of the two ledgers.

## Risk while open

None reachable today: the lifetime argument holds, and the argument is the same
one the page ledger is *also* relying on for its primary path.

The exposure is what happens when the argument stops holding — a ring made
transferable, a dup path admitted, a teardown reordered past the Proc release.
Then the page ledger degrades to an inert skipped refund and the thread ledger
decrements a recycled Proc's counter: an under-count on a Proc that never
charged, inflating its effective thread budget. That is the [[inv-i32]]-breaking
direction, and it is the one every other tie in this mechanism is deliberately
broken away from.

The shape worth naming: **defense-in-depth applied to one of two identical
arguments makes the unprotected one look considered.** A reader who finds the
magic-and-pid check on the first pointer will reasonably assume the second was
examined and judged safe, when the difference is that only one was written while
thinking about it.
