---
id: spec-poll-defer
type: spec
title: "poll_defer.tla"
models: [sub-kernel-poll, sub-kernel-ninep-client]
pins: [inv-i9, inv-i8]
cfgs:
  - "poll_defer.cfg -- clean: two holders, one REF and one WAIT owner, one hook each; every invariant incl. RefExact (5236 distinct states)"
  - "poll_defer_h2.cfg -- clean: two hooks per owner (75280)"
  - "poll_defer_liveness.cfg -- OwedReached + FreeEnds (5236)"
  - "poll_defer_buggy_no_get.cfg -- an owe on a REF owner takes no reference (NoUAF counterexample)"
  - "poll_defer_buggy_put_at_pop.cfg -- the reference dropped before the walk (NoUAF counterexample)"
  - "poll_defer_buggy_free_ignores_chain.cfg -- a WAIT owner's free waits for bookmarks only (NoUAF counterexample)"
  - "poll_defer_buggy_split_pop.cfg -- the chain field cleared and the bookmark listed in two holds (NoUAF counterexample)"
  - "poll_defer_buggy_split_owe.cfg -- the chain field tested and set in two holds (OneChain counterexample)"
  - "poll_defer_buggy_drain_inner.cfg -- every release drains, the inner one too (NoWalkUnderLock counterexample)"
  - "poll_defer_buggy_skip_drain.cfg -- no release drains (ChainDrained counterexample)"
  - "poll_defer_buggy_skip_drain_live.cfg -- the same, judged on OwedReached"
  - "poll_defer_buggy_skip_drain_free.cfg -- the same, judged on FreeEnds"
  - "poll_defer_buggy_owe_joins_walk.cfg -- (OwedCovered counterexample)"
gate: "any change to poll_defer_enter/exit, poll_wake_deferred, poll_deferred_await, a REF owner's tryget/put, a WAIT owner's free, or a deferring lock's wrapper -- specs/check-poll.sh"
created: 2026-10-28
updated: 2026-10-28
---
## Abstraction

Walks deferred past a DEFERRING lock: a 9P client's `c->lock` and
`g_dev9p_poll_lock`, which nests around it ([[dec-2026-10-27-locked-walks-out]]).
poll_owed.tla's owe-then-drain, with three differences:

- holders hold DIFFERENT locks (one per client) and one record can be owed
  under two of them (a Loom whose ops run on two clients), so a record's chain
  field is tested and set under its LIST's lock, and the drain clears it and
  lists the bookmark in the same hold; the chain's links belong to its thread
  and nothing cancels;
- nothing holds the owner across the drain, so a REF owner (a srvconn, a pipe
  ring) takes a reference per owe and drops it after the walk, and a WAIT owner
  (a Loom, a client, the test transports) is freed only once unreachable to
  every poster, its free waiting until the record is on no chain and no
  bookmark is listed;
- only the OUTERMOST release drains.

The model lets every holder interleave (no lock excludes one from another),
which only adds behaviours. The outermost release is STRONGLY fair: weak
fairness alone admits a thread that re-takes the inner lock forever inside one
outer hold, which no code path does (the first TLC run found that gap; the
clean liveness cfg failed until it was stated).

## What it pins

- **NoUAF**, **OneChain**, **ChainDrained**, **NoWalkUnderLock**,
  **OwedCovered**, **OwedIsListed**, **MarkIffWalking**, and **RefExact** (a REF
  owner's count is exactly its holders: transport, hooks, the chained record,
  each walk -- no leak, none missing).
- **OwedReached** + **FreeEnds** -- liveness: every owed hook is reached; a
  detached WAIT owner's free does not wait forever.

## What it cannot see

The same-CPU starvation of a preempted drainer by a never-preempted waiter (see
[[spec-poll-owed]]): runtime regression `poll.await_yields_to_a_drainer_on_its_cpu`.

## Binding

`specs/SPEC-TO-CODE.md::poll_defer.tla`: Lock / Unlock <-> `client_lock` /
`client_unlock` and `dpl_lock` / `dpl_unlock` (`poll_defer_enter` / `_exit`);
Event <-> `poll_wake_deferred`; Pop + Batch <-> the drain in `poll_defer_exit`;
Close <-> `p9_client_close`'s transport close; Detach + Free <-> `loom_free`
after its abandon loop, `p9_client_destroy`, the loopback/mq destroys, each
calling `poll_deferred_await`.
