---
id: spec-loom-order
type: spec
title: "loom_order.tla"
models: [sub-kernel-loom]
pins: [inv-i29]
cfgs:
  - "loom_order.cfg -- clean: the seven-conjunct safety set"
  - "loom_order_liveness.cfg -- EverySubmittedPosts: no op strands; it runs, or a failed predecessor cancels it"
  - "loom_order_buggy_link_reorder.cfg -- BUGGY_LINK_REORDER: a linked successor starts before its predecessor finished (LinkOrdered)"
  - "loom_order_buggy_drain_jumps_ahead.cfg -- BUGGY_DRAIN_JUMPS_AHEAD: a barrier runs with prior work outstanding (DrainOrdered)"
  - "loom_order_buggy_cancel_skips.cfg -- BUGGY_CANCEL_SKIPS: a post-failure successor is never cancelled and strands"
  - "loom_order_buggy_cancel_no_cqe.cfg -- BUGGY_CANCEL_NO_CQE: a cancelled op posts nothing (EveryDoneOpPosted)"
gate: "any change to the link gate, the barrier gate, the cancel cascade, or chain admission"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

Submission order, two ordering flags, and an admission pass. An operation is
*held* until its gates open: a linked successor waits for its predecessor to
finish successfully; a barrier waits for everything submitted before it. A failed
link cancels the rest of its group.

Deliberately a separate module from [[spec-loom]] for the same reason as its
multishot sibling — but here the reason is subtler. Ordering is not a property of
one operation's lifecycle; it is a property of the *relation between* operations,
and expressing it needed a submission-order successor relation the base module
does not carry.

## What it pins

- **`LinkOrdered` and `DrainOrdered`** — the two gates, stated as what may not be
  observed rather than as the algorithm. A linked successor never starts before
  its predecessor is done; a barrier never starts with prior work outstanding.
- **`EveryDoneOpPosted` and `AtMostOneCqe`** — every operation that reaches a
  terminal state, *including a cancelled one*, posts exactly one completion. This
  is the property that makes cancellation observable: a consumer waiting on a
  token must learn its operation was cancelled, and the only channel is a
  completion.
- **`NoOrphanCancel`** — a cancellation is always attributable to a failed linked
  predecessor, never spontaneous.

`EveryDoneOpPosted` is also why the implementation *rejects* the flag that would
suppress a successful operation's completion: suppression would violate this
invariant directly, so it needs a carve-out in the model before it can exist in
the code. That is the spec constraining the feature set rather than describing
it.

The barrier gate is where the model earned its keep. The implementation's first
version admitted a barrier while a live multishot stream sat in its deferred
re-arm state — momentarily *not* counted as in flight, because the count drops at
the shot and rises again at the re-issue. Their sum is the invariant, and the
gate had to consult both.

## What it cannot see

Concurrency between admitters. The model has a single admission pass; the
implementation assumes effectively one, and the residual — two concurrent drivers
over-reserving the completion queue — lives below it. The cancellation leg was
hardened against it explicitly (revert and retry rather than lose the
cancellation), the dispatch leg's residual is documented and owed.

Mixing ordering with multishot: rejected in the implementation, unmodelled here.

Chain-entry reclamation, which must not free a terminal predecessor while a held
successor could still consult it, is an implementation-level lifetime argument
below the abstraction.

## Binding

`specs/SPEC-TO-CODE.md::loom_order.tla`. Held ↔ the chain enqueue during the
drain; the gates ↔ the admission pass's link and barrier tests; the claim ↔ the
in-flight state written under the ring lock so a concurrent pass cannot
double-dispatch; cancel ↔ the head-to-tail walk in which a just-cancelled victim
becomes the next iteration's failed predecessor.
