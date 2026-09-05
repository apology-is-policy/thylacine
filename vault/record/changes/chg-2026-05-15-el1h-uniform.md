---
id: chg-2026-05-15-el1h-uniform
type: chg
title: "P5-el1h-kernel: the kernel runs uniformly at EL1h"
date: 2026-05-15
arc: arc-corvus-srv
commits: ["e2784c28"]
touched: [sub-kernel-sched-smp]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

Made the kernel's execution mode a constant. Before: normal kernel code
ran at **EL1t** (`SPSel = 0`, sp = `SP_EL0`), and EL1h was entered only
transiently inside exception handlers. After: EL1h everywhere, `SP_EL0`
exclusively the user stack, every kernel exception frame built on the
running thread's own kernel stack.

## Why

Root-caused during corvus bring-up, as an SMP secondary-CPU crash.
`cpu_switch_context` saves and restores SP but **not `SPSel`**, so a
thread resumed in whatever mode the *outgoing* thread had left the CPU
in. Under work-stealing a thread could resume the exception-exit path in
the wrong mode, where `msr SP_EL0` writes against the currently-selected
stack pointer instead of the intended one.

The fix considered and rejected was "save `SPSel` too". Removing the
variable is stronger than tracking it: with one mode there is no
mismatch to detect, and the property becomes checkable rather than
merely maintained.

## Consequence

This is the clause of [[inv-i21]] that makes the other clause meaningful:
if every frame lands on the thread's own stack, then "two CPUs writing
one context" and "two CPUs writing one stack" are the same violation, so
the `on_cpu` protocol guards both. [[spec-sched-ctxsw]] pins it, with the
dual-mode model as its counterexample.

Also why `g_exception_stacks` is allocated and asserted but unused at
runtime — it became a reserved landing pad for a future dedicated
overflow/SError stack rather than the live exception stack.
