---
id: chg-2026-06-01-811-death-interruptible
type: chg
title: "#811: universal death-interruptible sleep — the cascade becomes total"
date: 2026-06-01
arc: arc-holotype-rw
commits: ["7e65532b", "c111f4a6"]
touched: [sub-kernel-death, inv-i9]
established: []
closed: []
opened: [seam-death-cascade-smp-harness]
mirrors-checked: []
depth: rich
no-dossier-change: "retro backfill -- sub-kernel-death is established in this same sweep commit"
---
## What

Every rendez sleep in the kernel becomes death-interruptible. `sleep` and
`tsleep` gain a register-then-observe of the group-exit flag under a NEW
per-Thread `wait_lock` (the Plan 9 `p->rlock` analog) and a `*_INTR` return;
`proc_group_terminate` gains the universal death-wake — walk `p->threads`,
take each peer's `wait_lock`, read `rendez_blocked_on`, `wakeup()` it — and
gains its lock precondition. Nine blocking sites grow an INTR arm. The
trampoline gains a first-entry die-check (#809-audit F3).

## Why

#809's cascade woke futex sleepers and kicked running peers, but an
indefinitely-blocked one — `poll(-1)`, a pipe read, `devnotes_read` — was
never woken at all. The audit's F1 corrected the original framing: the
residual was not "the thread dies a bit late", it was a **non-reaping
HANG**. The Proc never drives its live count to zero, so it never zombies,
so its parent's wait never returns.

Two design points carry the weight:

**`rendez_blocked_on` is the only record** of "Thread T sleeps on Rendez R",
and only the owner writes it, always under its own `wait_lock`. The cascade
only READS it, under that same lock. That read-only waker→sleeper edge is
what keeps the lock graph acyclic.

**Option A — `wait_lock` held ACROSS `wakeup()`** — was chosen over dropping
it first because `rendez_blocked_on` can point into a sleeping peer's KERNEL
STACK FRAME (a torpor waiter's `w.rendez`). Holding the lock pins the peer:
its own resume must re-acquire `wait_lock` before returning, so the frame
cannot be popped out from under the waker.

## Verification

[[adt-811-r1]] — the mandated dirty-class follow-up to the #809 round,
which returned CLEAN (0/0/0/3). Its verified-sound set is the
do-not-re-prosecute preamble for this surface. Later pinned retroactively by
[[spec-death-wake]], written model-first-in-spirit against shipped code
precisely because this lineage carried none.
