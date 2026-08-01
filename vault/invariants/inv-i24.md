---
id: inv-i24
type: inv
title: "I-24 — group termination is atomic, exactly-once, and total"
number: I-24
guards: [sub-kernel-death, sub-kernel-torpor]
validated-by: [spec-death-wake, gate-smp]
strength: spec
created: 2026-08-01
updated: 2026-08-01
---
## Statement

Terminating a multi-Thread Proc is **atomic** (one decision, not a
per-Thread race), **exactly-once** (the Proc reaches ZOMBIE once, with one
status), and **total** (no Thread of a terminating Proc executes at EL0
after the Proc is ZOMBIE, and no Thread is left asleep forever instead of
reaching its checkpoint).

The three clauses fail in different ways and are worth separating: losing
atomicity gives two statuses; losing exactly-once gives a double reap;
losing totality gives a **hang** — the Proc never drives its live count to
zero, so it never zombies and its parent's `wait` never returns.

## Enforcement

`kernel/proc.c`. The model is flag-and-self-terminate (Plan 9 / Linux /
Zircon convergent; the seL4 synchronous stall was rejected):

- **Atomic**: `proc_group_terminate` publishes `group_exit_msg` with a
  set-once CAS (RELEASE). First message wins; a racing second terminate is a
  flag no-op that still re-runs the wake and kick. Every caller holds
  [[lock-proc-table]], so terminations are serialized outright — the CAS
  guards idempotency, not a genuine race.
- **Exactly-once**: the ZOMBIE transition happens only in
  `proc_become_zombie_locked`, reached only by the thread that observes
  `proc_count_live_peers_locked(p, t) == 0` under the table lock. That
  observer is unique by contradiction: for two threads to both count zero,
  each must observe the other EXITING, but EXITING commits *after* the
  committer's own count under the same lock.
- **Total**: every Thread dies at its own EL0-return die-check
  (`el0_return_die_check`, on both the sync and IRQ-from-EL0 tails), and
  the three delivery vehicles guarantee it gets there — the #811 universal
  death-wake for rendez sleepers, `torpor_wake_all_for_proc` for futex
  sleepers ([[sub-kernel-torpor]] — whose post-register die-pending
  re-check under `torpor_lock` closes the register-after-walk race on
  the futex leg), and a broadcast `smp_resched_others` (with the
  periodic tick as the floor) for peers running at EL0.

The status a terminating Proc reaps with is read from the recorded
`group_exit_msg`, not from the last Thread's own exit — which is what makes
a killed multi-thread Proc report the *kill's* reason rather than `"ok"`.

## Validation

[[spec-death-wake]]: `ZombieImpliesAllDead` is the totality clause's safety
form, `EventuallyReaps` its liveness witness, and the
`BUGGY_OBSERVE_BEFORE_REGISTER` cfg is the executable counterexample — the
#809-audit F1 non-reaping hang. [[gate-smp]] is the empirical backstop for
the multi-CPU cascade.

**blind-to:** the model abstracts the cascade to one flag and one walk. It
does not model the *close window* that now precedes the ZOMBIE flip
(#68/#926), the two stop owners that can park a thread on the way to its
checkpoint, or the orphan rule that fires inside the chokepoint. Those rest
on prose + the focused audits ([[adt-68-r3]] converged clean over three
rounds). The full 3-way SMP interleaving — a cascade walking while a remote
peer is mid-`thread_exit_self` while a third resumes clearing `on_cpu` —
remains [[seam-death-cascade-smp-harness]].
