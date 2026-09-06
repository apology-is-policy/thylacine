---
id: chg-2026-09-06-el1h-doc-absorb
type: chg
title: "absorb docs/reference/67-el1h-kernel (uniform-EL1h model, I-21): clean multi-redirect to inv-i21 + exception + thread + sched-smp + boot-entry + mmu"
date: 2026-09-06
arc: arc-vault
commits: ["3851e2e4"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The uniform-EL1h kernel execution model (I-21). A cross-cutting model doc;
verified atom-by-atom across the distributed homes.

WHERE EACH ATOM LIVES (verified, not assumed):
- The invariant (two clauses: one CPU per Thread + uniform EL1h; the second makes
  the first checkable) -> inv-i21 (note exists, strength: spec).
- The model + the dual-mode fossil (uniform-EL1h sp; the two "current EL with
  SP_EL0" slots as the unreachable VEC_UNEXPECTED fossil; userland_enter's EL1h
  msr sp_el0) -> sub-kernel-exception (:45 uniform-EL1h, :52 the dual-mode fossil,
  :260 the inv-i21 enforcement).
- The dual-mode-was-unsound history (cpu_switch_context doesn't save SPSel; SMP
  work-steal resumed KERNEL_EXIT at wrong mode; msr SP_EL0 at SPSel=0 traps
  UNDEFINED -> secondary CPU silently died; the 3 band-aids removed) -> inv-i21 +
  sub-kernel-exception.
- The thread kstacks + CPU-pinned bootstrap threads (own 32 KiB guarded kstack;
  kthread/idle on the boot stack, cpu_pinned = the modern form of F2's
  try_steal-skips-kstack_base==NULL) -> sub-kernel-thread (:30-33, :157-158) +
  sub-kernel-sched-smp (:107-108, :273).
- The F1 secondary boot-stack guard (4 KiB no-access + 16 KiB usable) ->
  sub-kernel-sched-smp:273 + sub-kernel-mmu.
- The spec (sched_ctxsw.tla, CtxSwitchModeConsistent clean vs BuggyModeSwitch) ->
  inv-i21 validated-by.

Zero-fold. The doc's "supersedes 08/01/17" note is the legacy-tree pointer; the
deferred dedicated-overflow-stack item is a known hardening seam. Multi-redirect
stub.
