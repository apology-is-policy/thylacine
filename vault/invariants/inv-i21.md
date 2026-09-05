---
id: inv-i21
type: inv
title: "I-21 — one CPU per Thread; the kernel is uniformly EL1h"
number: I-21
guards: [sub-kernel-sched-smp, sub-kernel-thread, sub-kernel-exception]
validated-by: [spec-sched-alpha, spec-sched-oncpu, spec-sched-ctxsw, gate-smp]
strength: spec
created: 2026-08-01
updated: 2026-08-02
---
## Statement

Two clauses that are really one:

1. **A Thread runs on at most one CPU at a time**, and its saved context
   and kernel stack are never written by two CPUs concurrently.
2. **The kernel executes uniformly at EL1h** (`SPSel = 1`), so `SP_EL0`
   is exclusively the user stack and every kernel exception frame is
   built on the running thread's own kernel stack.

The second clause is what makes the first *checkable*: if every frame
lands on the thread's own stack, then "two CPUs writing one context"
and "two CPUs writing one stack" are the same violation.

## Enforcement

**The claim protocol.** `t->on_cpu` is the cross-CPU "this context is
claimed" flag. It is set before the switch and RELEASE-cleared by
whoever resumes on that CPU afterward, and it is a **one-way** signal: a
reader may act on `false`, and must only ever *wait* on `true`. Three
readers rely on it — the wait/wake spin in `wake_rendez_waiter`,
`thread_free`'s gate (#788), and `timerwait_tick`'s pre-filter — plus
the two run-tree asserts that fail loud if a tree ever holds a
non-RUNNABLE or claimed thread.

**Pinning.** `cpu_pinned` threads never migrate: every per-CPU idle and
`kthread`. They run on a static boot/idle stack that belongs to one
specific CPU, and under the uniform-EL1h model migrating one would build
its frames on a stack its origin CPU still owns. `cpu_pinned` is the
single clean predicate that replaced a `kstack_base != NULL &&
cand != g_bootcpu_idle` gate whose exception *was* #860.

**The steal claim.** A victim is claimed under the *peer's* lock before
that lock is released (#801-F1), closing the window in which a stolen
thread sits out-of-tree and unclaimed and a concurrent `thread_free`
could reclaim its context.

**The MPIDR identity check.** `per_cpu_main` asserts that the PSCI
context id equals `smp_cpu_idx_self()`. On a sparse or cluster-MPIDR
board they diverge, and a CPU then aliases another's live per-CPU slot —
whose single-slot switch handoffs would be written by two CPUs. Bounds
checks cannot detect aliasing; this equality check fails loud instead.

**Uniform EL1h.** `cpu_switch_context` saves and restores SP but *not*
`SPSel`, so a thread resumed in whatever mode the outgoing thread left
the CPU in. Under the pre-P5 dual-mode kernel (EL1t normally, EL1h inside
handlers) a work-stolen thread could resume the exception-exit path in
the wrong mode, where `msr SP_EL0` writes the wrong stack pointer. Making
the kernel uniformly EL1h removes the variable rather than tracking it.

The clause is also enforced *structurally*, in [[sub-kernel-exception]]: the two
vector slots for "current EL with `SP_EL0`" are unreachable under this model, so
they are wired to the unexpected-vector diagnostic. An exception arriving there
means the mode bit was somehow cleared — which now extincts loudly, naming the
slot, instead of silently writing the wrong stack-pointer bank. The fossil is
load-bearing: under the pre-P5 dual-mode kernel those same two slots were the
live kernel-exception entries.

## Validation

[[spec-sched-alpha]] carries the safety set the redesign is gated on:
`NoSimultaneousRun`, `OwnerUnique`, `OnCpuMeansOwned`,
`RunningImpliesOnCpu`, `RunqOnCpuSafe`, `NoDoubleEnqueue`,
`IdleStaysHome`. [[spec-sched-oncpu]] is the diagnostic sibling that
reproduced #860 by *re-introducing* the mechanism `scheduler.tla`
abstracts away. [[spec-sched-ctxsw]] pins the EL1h clause with
`CtxSwitchModeConsistent` and a buggy cfg for the dual-mode model.
[[gate-smp]] (default + UBSan × smp4/smp8) is the empirical backstop.

**blind-to:** the models reason about the protocol, not about the
memory-ordering of the RELAXED set / RELEASE clear pair, which rests on
the documented atomics contract. And the whole family assumes a dense
MPIDR.Aff0 — the assumption the `per_cpu_main` assertion converts from
silent corruption into a loud boot failure, and which is otherwise
[[seam-sparse-mpidr]].
