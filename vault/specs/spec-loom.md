---
id: spec-loom
type: spec
title: "loom.tla"
models: [sub-kernel-loom]
pins: [inv-i29, inv-i30]
cfgs:
  - "loom.cfg -- clean: the sixteen-conjunct safety set, every buggy flag FALSE"
  - "loom_liveness.cfg -- every admitted op eventually posts its completion"
  - "loom_buggy_live_sqe_reread.cfg -- BUGGY_LIVE_SQE_REREAD: re-read a ring field after validating (ArgPinnedToSnapshot counterexample)"
  - "loom_buggy_recheck_at_completion.cfg -- BUGGY_RECHECK_AT_COMPLETION: re-resolve rights at completion instead of submit (ActedUnderAdmittedRights)"
  - "loom_buggy_double_post.cfg -- BUGGY_DOUBLE_POST: two completions for one op (NoDoubleCompletion)"
  - "loom_buggy_lost_on_full.cfg -- BUGGY_LOST_ON_FULL_CQ: overwrite an unreaped completion (CqNeverOverfull)"
  - "loom_buggy_stale_after_teardown.cfg -- BUGGY_STALE_AFTER_TEARDOWN: post into a torn-down ring (NoStaleCompletion)"
  - "loom_buggy_cqwait_no_wake.cfg -- BUGGY_CQWAIT_NO_WAKE: publish without waking the wait-list (NoMissedCqWake)"
  - "loom_buggy_cqwait_check_early.cfg -- BUGGY_CQWAIT_CHECK_EARLY: sample before registering (the same, from the waiter's side)"
gate: "any change to submission admission, the pin/rights snapshot, completion posting, or the completion wait-list"
created: 2026-08-02
updated: 2026-08-02
---
## Abstraction

One ring, a set of operations, a bounded completion queue, and a set of objects
an operation may name. Submission, dispatch, reply and reaping are separate
steps, so the interleavings that matter — a re-registration between resolve and
act, a teardown between reply and post, a wake between sample and sleep — are all
reachable.

The model was written before the dispatch implementation and gates it. That
ordering is the point: the two properties it pins are the ones whose violations
are famous elsewhere, and cheap to find in a model rather than in a driver.

## What it pins

- **[[inv-i30]] as two conjuncts.** `ArgPinnedToSnapshot` — the kernel acts on
  the copy it took, never on the shared slot. `ActedUnderAdmittedRights` — the
  rights checked at submit are the rights the work runs under, so replacing a
  table entry afterwards cannot redirect it. That second one is the io_uring
  credential-versus-work vulnerability class, stated as an invariant and given a
  counterexample.
- **[[inv-i29]] as the completion set.** Exactly one completion per operation
  (`NoDoubleCompletion`), none invented (`NoSpuriousCompletion`), none written
  over an unreaped one (`CqNeverOverfull`), none posted into a torn-down ring
  (`NoStaleCompletion`), and the in-flight count bounded.
- **The wait-list's register-then-observe**, as three conjuncts rather than one:
  the flag tracks the queue, no wake is missed, no waiter strands. The two
  matching buggy configurations attack it from opposite ends — the producer that
  publishes without waking, and the waiter that samples before it registers.

The full-queue behaviour is where the model shaped the implementation rather
than merely checking it: `CqNeverOverfull` forbids the obvious overwrite, and
what satisfies it is refusing to *consume* a submission whose completion has no
reserved room. The back-pressure moved to the front door because the invariant
would not permit it anywhere else.

## What it cannot see

Multishot, ordering and device-death each needed their own module rather than an
extension here, because this one's completion queue is a *set* of operations —
one completion per operation is baked into the state's shape, and a stream of
completions cannot be expressed in it. Widening it would have invalidated the
eight configurations already landed against it. So [[spec-loom-multishot]],
[[spec-loom-order]] and [[spec-loom-devgone]] are siblings, following the
precedent set when the scheduler's on-CPU protocol got its own module rather
than a rewrite of the original.

The teardown-wakes-waiters action is modelled and the implementation realizes it
literally — but *vacuously*, because a waiter holds a ring reference for its
whole call, so teardown cannot run while one sleeps. The code keeps the empty
walk as a defence if that reference discipline ever weakens.

Below the model: object lifetime, the borrow guard, the poll thread's join
handshake, and the concurrent-admitter over-reservation window.

## Binding

`specs/SPEC-TO-CODE.md::loom.tla`. Submit ↔ the drain's per-slot claim and copy;
the snapshot ↔ the entry copied into kernel memory; the pin and rights snapshot ↔
the resolve under the ring lock; post ↔ the completion write from the private
tail; the wake ↔ the wait-list walk after the lock drops.
