---
id: chg-2026-10-12-note-target
type: chg
title: "Every EL0 return delivers notes, and a caught note goes to a thread that can take it soon"
date: 2026-10-12
arc: arc-boosty
commits: ["d7728c93e", "99bcf632f", "abaa3f2b8", "76259d673", "5de52220b", "801d37a4a", "e61975590"]
touched:
  - sub-kernel-notes
  - sub-kernel-exception
  - sub-kernel-rendez
  - sub-kernel-thread
  - sub-kernel-death
  - sub-kernel-sched
  - sub-kernel-vivarium
  - spec-tail-order
established: [spec-caught-target]
closed: [seam-el0-irq-tail-no-notes]
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-12
---
Operator votes 2026-10-12 ([[dec-2026-10-12-note-target]]): "Prefer
running/main", native and phenotype alike; "#964 next, with the vote"; "Model
it", which re-enabled spec-first for this surface.

**What** ([[sub-kernel-exception]], [[sub-kernel-notes]]). The IRQ-from-EL0
vector slot branches into the synchronous slot's tail, `.Lel0_return`, so
every EL0 return runs the die-check, the stop leg and the notes leg: a thread
computing at EL0 without a syscall now takes its notes at the next tick or
kick (#964; closes [[seam-el0-irq-tail-no-notes]]). The handler frame saves the
whole interrupted context; SS and BTYPE are cleared on handler entry.

**Which thread** ([[sub-kernel-notes]], [[sub-kernel-rendez]],
[[sub-kernel-thread]], [[sub-kernel-sched]], [[sub-kernel-death]]). A posted
caught note designates one thread (`Thread.note_target`): the poster, main,
any thread not blocked, main blocked in a wait that unwinds, then any such
sleeper (L285: a worker's `ppoll` is no longer interrupted while main runs).
The designation is made under the pick's `wait_lock`, where every wait
publishes whether it unwinds or holds (`Thread.caught_wait`,
`sleep_caught_wait_enter`); a designee entering a wait it cannot unwind in, or
exiting, hands the designation on; a running designee is kicked
(`sched_kick_thread`); a stale pick is re-picked up to `CAUGHT_REPICKS` times
and then every unwinding sleeper is woken to race for the claim. A native
Proc's waits never unwind, so its walk only kicks. The designation becomes the
claim in `thread_caught_note_claim`; the tail's release drops both and its
walk skips the releaser. [[sub-kernel-vivarium]] records where this departs
from Linux's `complete_signal`, which prefers main whenever main can take the
signal.

**Model** ([[spec-caught-target]], new; [[spec-tail-order]], extended).
caught_target.tla: `OneClaimant` and `NoStrand`, clean 613245 distinct, 8 buggy
cfgs each violating its named property, 3 threads not finished (stopped at the disk floor after 12789190 distinct
states with both invariants holding; `NoStrand` not judged).
tail_order.tla gains `IrqEnter`, `RunMeets` and `BUGGY_IRQ_NO_NOTES`.

**Witnesses.** Kernel: `rendez.caught_note_prefers_awake`, `_poster_first`,
`_handoff` (sleep and tsleep), `_exit_drop`, `_holder_rewake`,
`_native_kicks_only`, `_repick_fallback`, and the rewritten `_one_unwind` and
`_release_wakes_peer`. Device: viv-pheno-probe L331-L333 (a main spinning
without a syscall takes a child's `SIGCHLD`; the polling peer sees POLLIN with
no EINTR) and jc-probe's spinint and spinnote rungs (Ctrl-C ends a native
spinner; a spinner's handler runs).

**Verification.** Fable 5.1 round 1 (start == end): 0 P0 / 0 P1 / 0 P2 / 6 P3,
closed: F1, F3, F4, F5 fixed; F2's comment fixed and its policy an operator
vote (OPEN-BUGS 2026-10-12 12:06Z); F6's tsleep hand-off and re-pick fallback
tested here, the stop-park restore and exec clear enqueued (12:06Z).
Suite 1984/1984; REDs, one build and boot each, red at their named assertions (k6 after its test gained the awake-main shape); check-caught-target.sh and check-tail-order.sh as claimed. Gates on e61975590: test-fault 8/8; ci-smp-gate N=10 50/50 (default smp1/4/8, UBSan smp4/8; 0 corruption); check-production --all PASS (KERNEL_TESTS off, where the forced-stale hook compiles out: the lean image builds, boots and logs in).
