---
id: chg-2026-09-22-arch81-scripture
type: chg
title: "ARCH 8.12: the design for syscall bodies with interrupts on, landed as scripture before any code"
date: 2026-09-22
arc: arc-arch81
commits: ["3bbd3bf3"]
touched: [sub-kernel-sched, sub-kernel-syscall-abi, sub-kernel-poll]
established: []
closed: []
opened: [seam-viv-tier2-frame]
mirrors-checked: []
depth: rich
created: 2026-09-22
---
## Synthesis

The design half of [[arc-arch81]], landed alone per scripture-before-code. Its
value is that THREE reconnaissance results replaced assumptions the chunk had
been sized against, and two of the three shrank it while the third grew it.

The lock sweep -- the work everyone expected -- measured a **clean negative**:
zero sites take a plain lock on a syscall path that a same-CPU IRQ handler also
takes. So the discipline already holds and only its ARGUMENT changes.

`preempt_count` cannot carry the marker, and **three** live assertions forbid it
rather than the one previously found; the decisive one is #361's
`el0_return_die_check`, where a syscall-wide count is definitionally the
"counted spinlock leaked to EL0 return" it extincts on.

The kernel stack had **never been measured**, and measuring it pulled a
prerequisite forward ([[seam-viv-tier2-frame]]): the Linux-phenotype path
already sits at 86% of 16 KiB, and the IRQ frame this chunk adds would take it
to 96.7%.

Full reasoning, the rejected alternatives and the placement constraint live in
the commit message and ARCH 8.12; the decision record is
[[dec-2026-09-22-arch81-design]].
