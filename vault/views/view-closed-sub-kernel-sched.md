---
id: view-closed-sub-kernel-sched
type: view
title: "Do-not-re-report preamble — sub-kernel-sched"
query: closed:sub-kernel-sched
---
# Do-not-re-report preamble — sub-kernel-sched

Generated from `fnd-*` notes (`quaestor render`; also emitted on-demand
by `quaestor closed sub-kernel-sched`). Paste or transclude into a
prosecutor prompt as the closed-findings preamble.

Read it WITH the verified-sound set in [[adt-866-r1]]. On this surface
that set is unusually load-bearing, because it records a **byte-identical
behavior-preservation diff** and a **deadlock-freedom argument for
`ready_on`** that a fresh round would otherwise re-derive from scratch —
and because every finding here lived on a path the runtime matrix
**cannot execute** (the cross-CPU and declared-heterogeneous branches are
inert on uniform hardware), so "the tests pass" is not evidence about any
of it.

<!-- generated:begin -->
8 closed findings on [[sub-kernel-sched]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-107-r1-f1]] [P2] ready_on carried the identical per-CPU TOCTOU class as sched() (fixed)
- [[fnd-33-r1-f2]] [P3] Three imprecise claims in the yield fast path's own justification (fixed)
- [[fnd-866-r1-f1]] [P1] Cross-CPU placement never set the target's need_resched, so a busy target ignored the placed thread for a full slice (fixed)
- [[fnd-866-r1-f3]] [P3] Per-CPU capacities were published with no release barrier and read with no acquire (fixed)
- [[fnd-866-r1-f4]] [P3] ready_on lost the out-of-range guard on its OWN cpu index during the policy/mechanism split (fixed)
- [[fnd-b0poll-r6-s1]] [P1] Two masked pollers on one CPU hand it to each other through sched() inside the masked syscall, so a per-thread sleep bound never unmasks the CPU (fixed) — FIXED by [[chg-2026-09-22-poll-preemption-point]]: the backstop is deleted and
- [[fnd-b0poll-r7-f3]] [P2] Both point tests stay GREEN with sched_preempt_point's body replaced by `return;` -- the test pollers are kthreads, so the unmask is inert (fixed) — FIXED by `sched.preempt_point_takes_a_pending_irq`: mask with
- [[fnd-b0poll-r7-s4]] [P3] The isb sabotage PASSES: the prose had upgraded the point into a per-pass delivery guarantee the architecture never gives, while the model stated it correctly (documented) — FIXED at `0434a4bc`: the emitted instruction sequence is unchanged (the `isb`
<!-- generated:end -->
