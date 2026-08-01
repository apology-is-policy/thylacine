---
id: spec-sched-ctxsw
type: spec
title: "sched_ctxsw.tla"
models: [sub-kernel-sched-smp]
pins: [inv-i21]
cfgs:
  - "sched_ctxsw.cfg -- clean: CtxSwitchModeConsistent under the uniform-EL1h model"
  - "sched_ctxsw_buggy.cfg -- the pre-P5 EL1t/EL1h dual-mode model: the invariant fails"
gate: "any change to the kernel's execution mode, SPSel handling, or what cpu_switch_context saves"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

The relationship between a CPU's live `SPSel` and the execution mode the
running thread requires — a tiny model of a single mismatch.

## What it pins

The EL1h clause of [[inv-i21]]: `CtxSwitchModeConsistent`.

The bug it memorializes, root-caused during corvus bring-up: the pre-P5
kernel ran normal kernel code at **EL1t** (`SPSel = 0`, sp = `SP_EL0`) and
entered **EL1h** only transiently inside exception handlers.
`cpu_switch_context` saves and restores SP but **not** `SPSel`, so a
thread resumed in whatever mode the *outgoing* thread had left the CPU
in. Under work-stealing, a thread could resume the exception-exit path in
the wrong mode, where `msr SP_EL0` writes against the currently-selected
stack pointer instead of the intended one.

The fix was not to save `SPSel` — it was to make the mode a constant.
Uniform EL1h means there is no mode to get wrong, `SP_EL0` is
exclusively the user stack, and every kernel exception frame builds on
the running thread's own kernel stack. The buggy cfg models the
dual-mode kernel and shows the invariant failing; the clean cfg shows
the uniform model keeps it by construction.

## Why it is a separate module

Because it is a *property of the machine model*, not of the scheduler's
data structures, and folding it into [[spec-sched-alpha]] would mean
carrying an `SPSel` variable through every migration action for a
property that is now constant. It is small, it is checked, and it is the
formal record of why the kernel does not have two modes.

## Gate

Both cfgs on any change to the kernel's execution mode, `SPSel` handling
in the vectors, or what `cpu_switch_context` saves and restores. In
practice that means: this is the module that says no when someone
proposes re-introducing EL1t for anything.
