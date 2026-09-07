# 67 — Uniform-EL1h kernel execution model (P5-el1h-kernel) [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-el1h-doc-absorb`).
The uniform-EL1h kernel execution model (invariant **I-21**): the kernel executes
entirely at EL1h (`PSTATE.SPSel = 1`); `SP_EL1` is always the running thread's own
kernel stack; `SP_EL0` is exclusively the userspace stack. Its content lives,
code-verified and current, in:

- the **invariant itself** — the two clauses (a Thread runs on at most one CPU;
  the kernel is uniformly EL1h) and why the second makes the first checkable:

      vault/invariants/inv-i21.md   (strength: spec)

- the **model + the dual-mode fossil** — the uniform-EL1h `sp`, the two "current
  EL with `SP_EL0`" vector slots that are now the unreachable fossil of the old
  dual-mode design (`VEC_UNEXPECTED`), and `userland_enter`'s EL1h `msr sp_el0`:

      vault/system/kernel/entry/sub-kernel-exception.md   (audit: hard)

- the **thread kstacks + the CPU-pinned bootstrap threads** — every thread's own
  32 KiB guarded kstack; `kthread` / per-CPU idle born on the boot stack with no
  portable kstack, so `cpu_pinned` (the modern form of the F2 "`try_steal` skips
  `kstack_base == NULL`" rule) keeps them from migrating:

      vault/system/kernel/execution/sub-kernel-thread.md
      vault/system/kernel/scheduling/sub-kernel-sched-smp.md

- the **boot SPSel asserts + the secondary boot-stack guard (audit F1)** — each
  `g_secondary_boot_stacks` slot is now a 4 KiB no-access guard + 16 KiB usable,
  mapped by the page-table builder:

      vault/system/kernel/boot/sub-kernel-boot-entry.md
      vault/system/kernel/memory/sub-kernel-mmu.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold of a cross-cutting model doc.** Every
  atom is distributed and current: the model and the dual-mode-was-unsound history
  (the secondary-CPU silently dying on `msr SP_EL0` at `SPSel=0`, the three
  band-aids the conversion removed) in inv-i21 + sub-kernel-exception; the
  CPU-pinned bootstrap threads in sub-kernel-thread + sub-kernel-sched-smp; the F1
  secondary boot-stack guard in sub-kernel-sched-smp + sub-kernel-mmu; the spec
  (`sched_ctxsw.tla`, `CtxSwitchModeConsistent` clean vs `BuggyModeSwitch`) in
  inv-i21's validated-by.
- **The doc's own "supersedes 08-exception / 01-boot / 17-smp-bringup" note is the
  legacy-tree pointer** — those are frozen `docs/reference` files too. The one
  still-deferred item it names, a dedicated stack-overflow / SError handler stack
  (one SP bank means a kstack overflow faults recursively), is a known hardening
  seam, not a defect this absorption owes.
