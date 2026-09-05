---
id: lock-runq
type: lock
title: "CpuSched.lock — the per-CPU run-queue lock"
kind: spin-irqsave (raw in sched(); trylock across CPUs)
guards: "one CPU's run_tree[3] and vd_counter, the pending_release_lock and prev_to_clear_on_cpu handoff slots, and the wake-preempt decision (which reads current_thread() and cs->idle where they are stable)"
orders-before: []
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

One per CPU, in `struct CpuSched`. File-private to `kernel/sched.c` — it
is not reachable from any other translation unit, which is why no
external caller can arrive already holding one, and why the deadlock
argument for `ready_on` is short.

The **innermost** lock of the wait chain:

    lock-proc-table -> lock-wait -> lock-timerwait -> lock-rendez -> lock-runq

Three acquisition disciplines, deliberately different:

- **Own CPU, `sched()`** — acquired **raw** (`spin_lock_raw`), i.e.
  uncounted by the #360 preempt count. This is the one cross-thread lock
  handoff in the kernel: `prev` acquires it, and the *resuming* thread
  (or a fresh thread's trampoline, via `sched_finish_task_switch`)
  releases it through `cs->pending_release_lock`. A per-thread count
  cannot balance an acquire and release in different threads. It is
  sound because the hold is IRQ-masked from `sched()`'s entry mask
  through the release, so it is non-preemptible by masking rather than by
  counting.
- **Own CPU, elsewhere** (`ready_on`, `sched_remove_if_runnable`,
  `sched_in_cpu_tree`) — ordinary `spin_lock_irqsave`, counted.
- **A peer's CPU** — `spin_trylock` only, in `try_steal`. A peer
  mid-mutation is skipped, never waited on. The single exception is
  `sched_remove_if_runnable`, which takes each CPU's lock in turn with a
  full acquire because `thread_free` needs unconditional cleanup — but
  one at a time, so it still cannot cycle.

## Held across

The **whole multi-step switch**, from `sched()`'s pick through
`cpu_switch_context` to whoever resumes on that CPU. That is unusually
long for a spinlock and it is the point: it is what makes a peer's
`spin_trylock` fail against a mid-switch CPU, and therefore what makes
the run tree hold only RUNNABLE, unclaimed threads at every instant a
peer can observe it. `RunqOnCpuSafe` in [[spec-sched-alpha]] is that
property; the two `ASSERT_OR_DIE`s in `pick_next` and `try_steal` are its
runtime enforcement.

Never held across a sleep — the thread that yields under it does not
*return* under it; the lock is handed off.

## Prosecution

- **Mask before you name the CPU.** `sched()` and `ready_on` both mask
  IRQs before reading the per-CPU index. Reading it unmasked lets a
  migration in the window leave the caller holding — and then leaking —
  a *foreign* CPU's lock, which is #104: a later `sched()` on the origin
  CPU spins on it forever. A loud-fail assert compares
  `cs - g_cpu_sched` to `smp_cpu_idx_self()` as the durable regression
  for a race that is otherwise timing-only.
- **The raw/counted split is exactly one acquire and three releases.**
  Releasing the raw acquire counted underflows the count (an extinction,
  and the first cut did it); releasing a counted acquire raw silently
  poisons the preempt gate.
- **The resume path must re-read `this_cpu_sched()`.** After
  `cpu_switch_context` the thread may be on a different CPU; the `cs` in
  its own frame names the wrong lock. Both resume sites (the `sched()`
  tail and `sched_finish_task_switch`) do, and both NULL-guard the slot
  because the test-only `thread_switch` primitive arms no handoff.
- **`ready_on` holds exactly one of these locks and never nests.** That
  is the entire deadlock argument for cross-CPU placement; a change that
  makes it hold two re-derives the argument from scratch.
