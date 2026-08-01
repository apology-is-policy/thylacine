---
id: chg-2026-07-04-360-preempt-count
type: chg
title: "#359/#360: a plain spinlock hold makes the holding thread non-preemptible"
date: 2026-07-04
arc: arc-go-build
commits: ["ce7bd352", "587972a6"]
touched: [sub-kernel-sched]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

`Thread.preempt_count`: incremented before a plain `spin_lock` acquire,
decremented after the release store. `preempt_check_irq` refuses to
preempt a thread holding one; `sched()` extincts if entered with a
non-zero count. Plus (#361) an EL0-return detector for a count that
leaked all the way back to userspace.

## The bug

A parallel on-device `go build` wedged the whole guest, syscall-silent,
about one boot in 1.5. A **preemptible context** — the fresh-thread exec
thunk; latently any kproc kthread or the exit handle-close path — was
descheduled *mid-hold* on the shared dev9p client's lock, and IRQ-masked
syscall spinners then occupied every CPU waiting for a holder that could
never run again.

Root-caused by QMP live-corpse autopsy.

## The fix is the rule, not the instance

It supersedes and **reverts** an interim fix that masked IRQs in the exec
thunk — which closed only the dominant trigger. That interim fix was the
#359 audit's own F1.

## Why per-THREAD, not per-CPU

The first cut was per-CPU, the Linux x86 shape, and had an unfixable
tear: an IRQ landing mid-RMW read the pre-increment 0, passed the gate,
the thread migrated, and the store then poisoned the **old** CPU's slot
permanently non-preemptible. Reproduced as a livelock with the kernel
still breathing and twin garbled extinctions. A per-thread count travels
with the migration, so the gate and the RMW always name the same object.

## Three details that carry

- **`preempt_check_irq` defers without consuming `need_resched`.** The
  flag may be the once-set cross-CPU placement kick ([[fnd-866-r1-f1]]);
  consuming it would lose the placed thread. The deferred preempt fires
  within a tick.
- **`spin_lock_raw` exists for exactly one caller** — `sched()`'s
  run-queue lock, the kernel's one cross-thread lock handoff (prev
  acquires, the resuming thread releases). One raw acquire, three raw
  releases; the first cut released one of them counted, and the underflow
  probe caught it.
- **Both diagnostics are permanent**, because each caught a real bug
  during bring-up: the underflow extinction is the *only* detector of the
  counted/raw mismatch class, and the #361 EL0-return check is the only
  detector of a leak in a CPU-bound thread that never sleeps.

## What the assert found immediately

One pre-existing lock-across-sleep: `p9_client_handshake` held a fresh
client's lock across the NOTAG Tversion exchange's **blocking recv**. A
new invariant check earning its keep on the first run.
