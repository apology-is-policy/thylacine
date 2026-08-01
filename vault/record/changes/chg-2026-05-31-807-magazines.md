---
id: chg-2026-05-31-807-magazines
type: chg
title: "#807: per-CPU magazines for real — the SMP double-alloc race"
date: 2026-05-31
arc: arc-deep-smp-review
commits: ["ce49d45c", "e6ef963d"]
touched: [sub-kernel-mm-phys]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

Pre-#807 the magazine layer's per-CPU-ness was nominal: `NCPUS` was
pinned at 1 and `my_cpu()` returned 0, so under `-smp > 1` every CPU
shared ONE stack and raced the non-atomic `count` RMW — two CPUs
could pop the same page (double allocation → cross-owner corruption).
Dormant until work-stealing let joey's allocations run on secondary
CPUs.

The fix: one magazine set per CPU indexed by `MPIDR_EL1.Aff0`, the
whole fast path under a bare IRQ mask (`spin_lock_irqsave(NULL)`) —
pinning CPU identity across the op and making the set non-reentrant —
plus the loud count-corruption `ASSERT_OR_DIE` as the regression
tripwire.

## The close's one finding worth a note

[[adt-807-r1]] closed 0/0/0/2; its F1 ([[fnd-807-f1]]) named the
KERNEL-WIDE assumption the fix leans on: Aff0 == the dense logical
CPU index — shared with sched/gic/fault/halls, false on clustered
big.LITTLE where Aff0 repeats per cluster and two CPUs fold onto one
slot, quietly reopening this exact race. [[seam-sparse-mpidr]] is
the standing obligation; this chg is why the magazines are on its
blast radius.

## Precursor context

This and [[chg-2026-05-31-808-directmap-pagemap]] are the
same-week SMP-soundness precursors of the deep-smp-review — the
corruption hunt (#806 lineage) that a week later became the
model-first scheduler redesign.
