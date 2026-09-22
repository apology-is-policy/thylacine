---
id: spec-syscall-irqs
type: spec
title: "syscall_irqs.tla"
models: [sub-kernel-sched, sub-kernel-syscall-abi]
pins: []
cfgs:
  - "syscall_irqs.cfg -- clean, a user thread: NoInvoluntarySwitchInBody + EretWindowMasked + TailTookItsPreempt (18 states)"
  - "syscall_irqs_liveness.cfg -- SpecLive: CpuGetsItsInterrupts (11)"
  - "syscall_irqs_kthread.cfg -- THE CONTROL, not a bug: no marker, so KthreadGetsPreempted must be VIOLATED (12)"
  - "syscall_irqs_buggy_marker_ignored.cfg -- preempt_check_irq never consults the marker (NoInvoluntarySwitchInBody)"
  - "syscall_irqs_buggy_unmask_before_mark.cfg -- the body unmasks one step before the marker is set (NoInvoluntarySwitchInBody)"
  - "syscall_irqs_buggy_late_remask.cfg -- the re-mask lands after ELR/SPSR are installed: #713's shape (EretWindowMasked)"
  - "syscall_irqs_buggy_marker_never_cleared.cfg -- the tail defers instead of taking the boundary preempt (TailTookItsPreempt)"
  - "syscall_irqs_buggy_masked_body.cfg -- the AS-BUILT model under noise (CpuGetsItsInterrupts)"
gate: "any change to the SVC entry mask, the marker's set/clear placement, preempt_check_irq's gate, or the KERNEL_EXIT eret window -- specs/check-syscall-irqs.sh"
created: 2026-09-22
updated: 2026-09-22
---
## Abstraction

One CPU, one thread, and one question: where does the mask change, where is
the marker consulted, and what does a deferred reschedule do. Written
MODEL-FIRST for [[arc-arch81]] -- the code does not exist yet.

## What it pins

- **NoInvoluntarySwitchInBody** -- an interrupt taken inside a syscall body
  runs on the interrupted thread's own kernel stack and returns to it. The
  tick's `need_resched` waits for the EL0 boundary.
- **EretWindowMasked** -- `ctx => masked`. KERNEL_EXIT installs ELR/SPSR and
  `eret`s under an INHERITED mask, the one surviving #713-class window that
  does not mask locally, so the re-mask must precede it.
- **TailTookItsPreempt** -- the #107 syscall-return preempt fires AT the
  boundary rather than being deferred past it by a leaked marker. Stated as
  `tailchk # "deferred"`, over a variable the tail's preempt check WRITES,
  not as `(pc = "eret") => ~resched`. The predecessor was TAUTOLOGICAL: it
  gated the eret window on `(~resched \/ Defers)`, which with the marker clear
  is literally the invariant's own text, so a model in which the check were
  simply ABSENT would have passed it. It is now its own step, the window
  requires it to have RUN, and skipping it wedges the model instead --
  measured, by deleting the action: 14 distinct states and "Deadlock reached"
  against the clean 18.
- **CpuGetsItsInterrupts** -- `[]<>(~masked)`. What the chunk buys. This is
  the obligation [[spec-poll-cpu]] carries today (`[]<>Open` there): the same
  sentence about the same CPU.

## The negative invariant carries its own control

"No involuntary switch" is satisfied in full by a model that cannot switch at
all, which is the shape of a green that means nothing. `syscall_irqs_kthread`
runs the SAME machinery with no marker and must VIOLATE
`KthreadGetsPreempted` -- one variable away, opposite verdict. It is also a
real obligation: kthreads stay preemptible or #810 is lost. The gate script's
header says at length that this row's failure IS its pass, because reading it
as a defect and fixing it would delete the evidence.

The gate's own discrimination is MEASURED, not assumed: two sabotages of the
module (making the late-remask arm inert; making the marker never consulted)
each turn `check-syscall-irqs.sh` red, on the rows they should.

## What it cannot see

**Locks are absent**, and that is a finding rather than a gap: the
reconnaissance measured ZERO sites where a plain lock is taken by both a
syscall path and a same-CPU IRQ handler, so a lock model would have nothing to
discriminate.

**Stack depth is absent.** The kernel-stack bound is a MEASUREMENT
(`-fstack-usage` over a call graph whose indirect edges resolve through the
DWARF layout of `struct Dev`), not a model property, and its runtime guard is
the watermark [[arc-arch81]] adds. See [[seam-viv-tier2-frame]].

## Three weaknesses the audit round found in this module

All three were in the parts the module's own prose leaned on hardest, which is
the instructive part -- the prose asserted properties the model did not have.

**`VoluntarySleep` modelled no switch.** Its comment claimed it existed "so the
invariant is not trivially satisfied by a model in which no switch of any kind
occurs in the body", and the action moved no `pc` and touched no `ksw`: it
cleared a flag. The module's non-triviality was carried ENTIRELY by the
`KTHREAD` control, which is genuine, but the stated second source did not
exist. It now moves to a real `"blocked"` state and back, so the body DOES
leave the CPU and `NoInvoluntarySwitchInBody` is a claim about the KIND of
switch rather than about switches existing. Its `resched` precondition is gone
too -- a blocking syscall sleeps whether or not a reschedule is pending, and
requiring one made the leg unreachable in exactly the runs where nothing had
ticked.

**`Tick` was gated on `~masked`.** A local timer IRQ does need the unmask, but
it is not the only producer: `ready_on`'s cross-CPU arm sets a PEER's
`need_resched` regardless of that peer's mask. The gate modelled a system in
which the flag cannot appear during a masked window, and let the module prove
a tail property STRONGER than the code guarantees.

**`TailTookItsPreempt` was tautological** -- see above.

And the gate that runs it had a matching hole: it passed TLC's `-deadlock`
flag, which DISABLES deadlock checking, so the wedge that now catches a skipped
tail check was invisible. Measured by sabotage: with the flag, deleting
`TailPreemptCheck` PASSED. The clean cfgs now run deadlock-checked; the
must-fail cfgs keep the flag, because their job is to violate a NAMED property
and a deadlock reported first would mask which one.

## A property withdrawn, and why that is recorded

A first cut asserted that a pending reschedule always eventually fires. TLC
refuted it in five steps -- a tick, a syscall entry, and a producer that keeps
the body looping. The counterexample was CORRECT: a non-preemptible kernel
defers the switch for as long as the syscall runs, and what 8.1 buys is that
interrupts are SERVICED, not that a reschedule is prompt.

That is the same category error [[arc-arch81]] exists to repair -- "defer
preemption" heard as "mask interrupts" -- caught one layer down, inside the
spec written to prevent it. The reschedule guarantee is therefore stated only
where it is true: at the boundary, as `TailTookItsPreempt`.

## Supersession

[[spec-poll-cpu]] is RETIRED IN PLACE, not deleted: its module
`specs/poll_cpu.tla` and its four cfgs are gone, but the note stays because the
append-only record plane names it (an audit round scopes it, a change
established it, a finding lists it as a surface). History keeps its referents.

That module's stated premise WAS the masked syscall body, so this chunk did not
make it wrong -- it made it VACUOUS. A model whose adversary cannot exist
proves nothing, so it went with `sched_preempt_point`. The obligation it
carried is `CpuGetsItsInterrupts` here, and `syscall_irqs_buggy_masked_body`
reproduces the old defect under noise, so the discrimination round-6 S1 earned
is not lost.

`poll.tla` keeps everything else it models; only `Point` / `atpoint` /
`IrqLatencyBounded` and `poll_buggy_no_point.cfg` left it. Its clean runs
measure 2146 / 944 states, down 48 -- exactly the `atpoint` states removed.

## Binding

`specs/SPEC-TO-CODE.md::syscall_irqs.tla` carries the action table with its
impl column OWED -- deliberately unfilled, because the code does not exist and
an invented mapping is worse than an absent one.
