---
id: chg-2026-08-01-sched-sweep
type: chg
title: "Vault sweep: the scheduler area (dispatch, the SMP protocol, the wait/wake primitive)"
date: 2026-08-01
arc: arc-vault
commits: []
touched: [sub-kernel-sched, sub-kernel-sched-smp, sub-kernel-rendez]
established:
  - moc-kernel-scheduling
  - sub-kernel-sched
  - sub-kernel-sched-smp
  - sub-kernel-rendez
  - inv-i8
  - inv-i17
  - inv-i18
  - inv-i21
  - spec-scheduler
  - spec-sched-oncpu
  - spec-sched-alpha
  - spec-sched-ctxsw
  - spec-sched-tickless
  - spec-sched-rebalance
  - spec-tsleep
  - lock-wait
  - lock-timerwait
  - lock-rendez
  - lock-runq
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

Batch 8 of the vault sweep. Establishes `vault/system/kernel/scheduling/`
as its own area — three dossiers along the code's own seams — from a full
read of `kernel/sched.c` (2632 lines), `kernel/smp.c`,
`kernel/include/thylacine/sched.h`, `rendez.h`, and the six `sched_*`
spec modules plus `tsleep.tla`.

Four invariants registered (I-8, I-17, I-18, I-21), seven specs, four
locks forming the wait chain, seven seams, and the Record-plane history
of the surface: two new arcs, ten retro chgs, six audits, ten findings.

## Why its own area, not a child of execution

[[moc-kernel-execution]] frames itself narrowly and correctly — the
Proc/Thread pair and the death path, with death as its centre of gravity.
The scheduler is ~3900 lines with its own invariant set and the largest
spec family in the tree. Folding it in would blur both. Adjacency is
carried by the MOCs' cross-links, which is what they are for.

`sleep`/`tsleep`/`wakeup` are placed **here** rather than in the (still
empty) `ipc-wake` area, because they are implemented in `sched.c` and are
the scheduler's blocking side. `ipc-wake` becomes the *consumers* — poll,
pipe, torpor, notes — which is the honest layering.

## The finding

**Three consecutive sweeps have now found the same staleness mode, and
this one is the clearest instance of the mechanism.**

`docs/reference/15-scheduler.md` is 1075 lines, of which lines 303–1075
are current, detailed and well-maintained — sections appended
chronologically as each chunk landed. Lines 1–302 are frozen at P2-Ba.

Nothing was ever *revised*; sections were only ever *added*. So the head —
Purpose, Public API, Implementation, Data structures, Tests, Status,
Known caveats — describes a single-CPU, non-preemptive scheduler that
stopped existing in May, and a reader arriving at the top gets that,
with 772 lines of immaculate current prose below it as evidence the page
is maintained.

The sharpest instance is inside one table. The **Status** table lists, as
adjacent rows, `Scheduler-tick preemption (timer IRQ -> sched) | P2-Bc`
(i.e. future work) and a bolded, current row for the #866 SMP-redesign
audit close. Batch 7 found a contradiction four lines apart; this one is
four *rows* apart, in the same table.

Full list in the absorbed stub. Two entries are worth naming here:

- **`thread_block` / `thread_wake`** — phantom function names that do not
  exist (the primitives are `sleep` / `wakeup`). The **same** phantom
  pair batch 7 found in `14-process-model.md`. That makes it a
  *propagated* error, not two independent ones.
- **The `sched()` step-list** (line 82) gives five steps with **no IRQ
  mask, no lock, no `on_cpu`, no handoff**. That is the pre-#104 body —
  and #104 was a permanent SMP deadlock. Batch 7 found step-lists
  documenting the pre-fix bodies of #788 and #101; this is the third.

## Also fixed

`arc-vault`'s own `chunks:` list had drifted two sweeps behind (the
stalk, netd, territory and proc/thread sweeps all set `arc: arc-vault`
but were never appended). Appended here, along with this chunk — an arc
whose chunk list is not the index it claims to be is the same failure
mode in miniature.
