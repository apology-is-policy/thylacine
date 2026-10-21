---
id: chg-2026-10-12-loom-sq-release-after-copy
type: chg
title: "The SQ head is published only after the copy (SQ-1)"
date: 2026-10-12
arc: arc-boosty
commits: ["2c038fce1", "512383b31"]
touched:
  - sub-kernel-loom
  - spec-loom-sq
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-12
---
Found, specified, fixed and audited by Corona on corona/async-memory
(aab7cd512, 46f07edc2, 559edc720); ported to main as 22c343232 and landed by
main, stacked on groupstatus (cherry-picked as 2c038fce1).

**What** ([[sub-kernel-loom]], [[spec-loom-sq]]). `loom_drain_sq` published
`sq_head` (the producer's permission to reuse both the index slot and its SQE)
before it read the index and copied the SQE. Under SQPOLL the kernel drains on
another CPU while the producer runs, so a producer that refilled the position
in that window had the kernel copy the newer SQE under the old submission: one
op executed twice and one lost (I-29). I-30 held: the kernel validated its own
copy. `loom_sq_release` (advance the private head and release-store the
published one, under `l->lock`) is now the only writer of either, called after
the index read and the copy or drop; the dropped count is an atomic add.

**Why a new spec.** `loom.tla` gives every op its own slot and copies
atomically at `Consume`, so it cannot reach a position handed back and refilled
between the publication and the copy. `specs/loom_sq.tla` models one
well-behaved producer gated on the published head and two consumers under the
lock; its counterexample for the old order is an out-of-range entry's position
refilled under the stale claim and consumed as valid.

**Verification.** Corona: Fable 5.1 audit r1 0/0/1/4, all fixed; TLC 14/14 and the guest mutants on corona/async-memory. Main: Stacked tip 512383b31: suite 1975/1975 (main's 1972 plus the three SQ-1 tests). Corona's mutants, re-taken in main's tree: M1 (release before the copy, main's old order) fails exactly sq_release_after_copy, sq_release_after_index and sq_release_after_copy_chain at Corona's named assertions (1972/1975); M2 (the chain path re-reads the shared SQE) fails only the chain test (1974/1975). TLC: loom_sq clean 188 distinct, liveness 188, buggy_release_before_copy and buggy_unlocked_claim violate FifoFidelity, buggy_unlocked_claim_exclusive violates ClaimExclusive; loom.tla unchanged: clean 2429, liveness 1457, all 7 buggy cfgs violate their named invariants. One number differed from Corona's table: unlocked_claim stopped at 110 distinct states, not 104; at one worker it stops at 51. The count before a violation depends on the worker count; the verdict does not. ci-smp-gate N=10 50/50, test-fault 8/8, and the production shape (KERNEL_TESTS off, where the new test hook compiles out) builds and boots.
