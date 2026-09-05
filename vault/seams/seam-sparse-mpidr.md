---
id: seam-sparse-mpidr
type: seam
title: "A dense MPIDR.Aff0 is assumed; a sparse/cluster board fails loud"
status: open
surface: [sub-kernel-sched-smp]
opened-by: chg-2026-06-05-863-smp-soundness-core
tracker: "the DTB MPIDR -> dense-logical-index map"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A real MPIDR → dense-logical-index map derived from the DTB. Today
`smp_cpu_idx_self()` is literally `mpidr & 0xff` — MPIDR.Aff0 — and the
whole per-CPU array is indexed by it.

## The hazard

Two different numbers name a CPU's slot:

- the **PSCI context id** (`cpu_idx`), used for the boot stack slot,
  `sched_init`, and the per-CPU idle;
- **MPIDR.Aff0**, used by every *runtime* per-CPU access —
  `this_cpu_sched`, `sched_tick`, `preempt_check_irq`, `asid_resolve`.

On a dense-Aff0 board they agree. On a sparse or cluster-MPIDR board
(Aff1 = cluster, Aff0 restarting at 0 per cluster) they **diverge**: the
CPU initializes slot `cpu_idx` and then resolves every runtime access to
a *different* slot — silently aliasing another CPU's live `CpuSched`,
whose single-slot switch handoffs (`pending_release_lock`,
`prev_to_clear_on_cpu`) would then be written by two CPUs. That is the
#860 class of corruption, arriving by a different door.

## What stands in for it

`per_cpu_main` asserts `(unsigned)cpu_idx == smp_cpu_idx_self()` and
extincts on mismatch. **A bounds check cannot detect aliasing** — both
indices are in range — so the equality check is the only thing that
converts silent cross-CPU corruption into a loud boot failure on the
first board that breaks the assumption.

Dormant on every v1.0 target: QEMU virt and RPi are both dense-Aff0.

## Cost of leaving it

None until the first non-dense board, and then a clean refusal to boot
rather than a memory-corruption hunt. The assertion is the deliberate
trade: fail loud now, build the map when a board needs it.
