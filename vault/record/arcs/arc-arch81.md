---
id: arc-arch81
type: arc
status: active
title: "ARCH 8.1 as written: syscall bodies with interrupts on, still non-preemptible"
design: ["docs/ARCHITECTURE.md 8.12", "docs/ARCHITECTURE.md 8.1"]
chunks:
  - chg-2026-09-22-arch81-scripture
follow-ons: []
created: 2026-09-22
---
## Goal

Repair an accident rather than add a feature. Phase 0 deferred kernel
PREEMPTION to Phase 7; P3-Ec (`48dfc5c4`) wired the SVC path under the mask
exception entry sets and never lifted it, so the deferral of *preemption* was
BUILT as *interrupts off* -- a strictly stronger and different property, which
nothing recorded. Later races were closed by masking more (#713, #104), and
#359 wrote the result into ARCH 8.11 as a fact of the design.

The cost: a syscall that LOOPS holds its CPU's interrupts for as long as it
loops, and making one loop takes no privilege ([[fnd-b0poll-r5-f1]]). poll has
a preemption point today ([[chg-2026-09-22-poll-preemption-point]]);
`pipe_block_locked` and `chan_role_acquire` have the same shape and no point.
Points do not scale -- each is a patch on one instance of a diagnosed class.

Build ARCH 8.1's line as written: preemptive at the EL0<->EL1 boundary, the
kernel itself non-preemptible, interrupts SERVICED throughout.

## Shape

- **Design** ([[chg-2026-09-22-arch81-scripture]], ARCH 8.12) -- the marker,
  the unmask placement, and the three measurements that forced them
  ([[dec-2026-09-22-arch81-design]]).
- **Spec** (owed) -- spec-first is re-enabled for this surface. The property is
  NOT the lock sweep, which measured clean: it is "no involuntary switch inside
  a syscall body" + "the re-mask precedes the eret window".
- **Code** (owed) -- the per-thread marker, the unmask/re-mask placement,
  [[seam-viv-tier2-frame]]'s prerequisite fix, the interrupt-state assert, the
  kernel-stack watermark, and the deletion of `sched_preempt_point` +
  `specs/poll_cpu.tla`.
- **Doc sweep** (owed) -- ~20 code comments and 9 spec/doc sites state the
  masking as premise. The load-bearing one is
  `kernel/include/thylacine/spinlock.h:41-43`, which IS the #360 rationale.
- **Audit + SMP gate + fleet** (owed).

## What makes this arc dangerous

It edits the exception entry path -- #713's home. `vectors.S:126-131`
(KERNEL_EXIT) sets ELR/SPSR and `eret`s under an INHERITED mask, the one
surviving #713-class window that does not mask locally. An unmask that leaks
into the return tail resurrects a year-long corruption hunt whose profile was
3-13% of boots and never at `-smp 1`.

The compensating facts, all measured rather than assumed: the lock sweep is a
clean negative (0 sites), the kernel-stack headroom is sufficient once
[[seam-viv-tier2-frame]] is fixed (75.5% of 16 KiB with the IRQ frame), and the
guard page turns any residual overflow into a fault rather than corruption.

## Exit criteria

- No involuntary switch inside a syscall body, modelled and gated.
- The re-mask provably precedes the `eret` window.
- `sched_preempt_point` and `specs/poll_cpu.tla` deleted, with poll's tests
  still green.
- Kernel-stack watermark corroborates the static bound under the fleet.
- Audit round clean; SMP gate 40/40; the interactive fleet green.
