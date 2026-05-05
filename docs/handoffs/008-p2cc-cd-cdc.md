# Handoff 008 — P2-Cc + P2-Cd + P2-Cdc landed

**Date**: 2026-05-05
**Tip**: `2648053` (P2-Cdc hash fixup)
**Phase**: Phase 2 in progress. P2-A through P2-Cdc landed. SMP fully online with cross-CPU IPI delivery. P2-Ce (work-stealing) is the next sub-chunk.

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework. Mandatory.
2. **This file** — canonical pickup at P2-Cdc close.
3. `docs/handoffs/007-p2bb-bc-ca-cb.md` — predecessor (P2-Bb..Cb).
4. `docs/handoffs/006-p2a-process-model.md` — P2-A close.
5. `docs/handoffs/005-phase1-close.md` — Phase 1 close.
6. `docs/VISION.md` + `docs/ARCHITECTURE.md` + `docs/ROADMAP.md` — binding scripture.
7. `docs/phase2-status.md` — Phase 2 sub-chunk plan with all landed rows + cumulative trip-hazards (#1-63).
8. `docs/REFERENCE.md` + `docs/reference/15-scheduler.md` (per-CPU dispatch section) + `17-smp-bringup.md` (per-CPU exception stack + per-CPU GIC + IPI sections).
9. `specs/scheduler.tla` + `specs/SPEC-TO-CODE.md` — formal spec + impl mapping.
10. `memory/project_active.md` — quick state summary.

---

## What landed in this session (6 commits)

### P2-Cc: per-CPU exception stacks via SPSel=0

Commits: `8eb4aee` (substantive) + `a9757db` (hash fixup).

- New `g_exception_stacks[DTB_MAX_CPUS][4096]` (32 KiB BSS) in kernel/smp.c with `EXCEPTION_STACK_SIZE` + `smp_cpu_idx_self()` accessor (reads MPIDR_EL1.Aff0).
- start.S `_real_start` step 4.6 (primary) + `secondary_entry` (secondaries) split SP into SP_EL0 = boot stack + SP_EL1 = per-CPU exception stack via the SPSel-dance: `msr sp_el0` (allowed at EL1 regardless of SPSel) then `mov sp` while still SPSel=1 (so sp refers to SP_EL1; `msr sp_el1` from EL1 is UNDEFINED), then `msr SPSel #0 + isb`. Step 8.5 + post-mmu_program_this_cpu re-anchor SP_EL1 to HIGH VA via `kaslr_high_va_addr` + the same SPSel-dance.
- vectors.S keeps BOTH Current-EL/SP_EL0 (0x000+0x080) AND Current-EL/SP_ELx (0x200+0x280) Sync+IRQ slots LIVE — kernel transits through SPSel=1 transiently after sched-from-IRQ + thread_trampoline daifclr unmask.
- timer_irq_handler captures `&local` once per CPU into `g_exception_stack_observed[]` for runtime verification.
- New test smp.exception_stack_smoke.
- Closes vectors.S P1-F "KNOWN LIMITATION" — kernel stack overflow detection now actually works.

### P2-Cd: per-CPU run trees + per-CPU idle threads (Cda + Cdb)

Commits: `a604cd7` (substantive) + `af38884` (hash fixup).

- `struct CpuSched { run_tree[BANDS]; vd_counter; initialized; idle; }` in sched.c. `g_cpu_sched[DTB_MAX_CPUS]` BSS.
- sched(), ready(), sched_tick(), preempt_check_irq(), sched_remove_if_runnable(), sched_runnable_count() all index by smp_cpu_idx_self().
- `g_need_resched` is now `volatile bool[DTB_MAX_CPUS]` — each CPU's IRQ writes its own slot.
- sched_init takes a cpu_idx. main.c calls sched_init(0); per_cpu_main calls sched_init(idx).
- New `thread_init_per_cpu_idle(cpu_idx)` in thread.{h,c}: allocates a Thread descriptor for a secondary CPU's idle. Doesn't own a kstack — runs on the per-CPU boot stack assigned by start.S. State = THREAD_RUNNING; band = SCHED_BAND_IDLE.
- per_cpu_main wired: VBAR + idle thread + TPIDR_EL1 + sched_init + g_cpu_alive + (initially pure WFI at this commit; updated to sched()+wfi at P2-Cdc).
- New test smp.per_cpu_idle_smoke.
- New `sched_idle_thread()` accessor.

### P2-Cdc: GIC SGI infrastructure + IPI_RESCHED

Commits: `9b8ecd0` (substantive) + `2648053` (hash fixup).

- New `gic_init_secondary(cpu_idx)` per-CPU GIC bring-up (refactored `redist_init_cpu0` → `redist_init_cpu(idx)` walking g_redist_base + idx*0x20000) + CPU interface system regs.
- New `gic_send_ipi(target, sgi)` writes ICC_SGI1R_EL1 with TargetList encoding (1 << target_cpu_idx, Aff{1,2,3}=0 for QEMU virt).
- `IPI_RESCHED = SGI 0` in `<thylacine/smp.h>`. `g_ipi_resched_count[]` BSS counter. `ipi_resched_handler` increments per-CPU counter.
- `smp_cpu_ipi_init(cpu_idx)` from per_cpu_main: gic_init_secondary + gic_attach + gic_enable_irq + msr daifclr #2.
- per_cpu_main idle loop: pure WFI → `for(;;){sched();wfi;}`.
- **Bug fix**: gic_enable_irq for SGI/PPI was hardcoded to CPU 0's redist; refactored to use cpu_redist_base(smp_cpu_idx_self()).
- New test smp.ipi_resched_smoke.
- New banner: `ipi: GICv3 SGIs live (...)`.

---

## Current state at handoff

- **Tip**: `2648053`.
- **Phase**: Phase 2 in progress. P2-A through P2-Cdc landed. **P2-Ce (work-stealing) is the next sub-chunk.**
- **Working tree**: clean (only `docs/estimate.md` + `loc.sh` untracked, pre-existing).
- **`tools/test.sh`**: PASS. 21/21 in-kernel tests; ~104 ms boot (production), ~140 ms (UBSan). 4/4 cpus online via PSCI HVC + per-CPU sched + idle thread + IPI delivery.
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (3/3).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**: `scheduler.tla` TLC-clean at 283 distinct states; `scheduler_buggy.cfg` produces missed-wakeup counterexample. Spec UNCHANGED at P2-Cc/Cd/Cdc — per-CPU run trees + IPIs are infrastructure with no kernel concurrency invariant impact at these sub-chunks. Cross-CPU spec work begins at P2-Cg (Steal action + IPI ordering invariant I-18).
- **LOC**: ~6700.
- **Kernel ELF**: ~292 KB debug.
- **In-kernel tests**: 21. New this session: smp.exception_stack_smoke, smp.per_cpu_idle_smoke, smp.ipi_resched_smoke.
- **Open audit findings**: 0. R5 audit deferred to Phase 2 close.

## What's NEXT — decision tree

### Option A (recommended): P2-Ce → P2-Cf → P2-Cg

**P2-Ce: work-stealing** (~200 LOC). When this CPU's run tree is empty AND a peer CPU has runnable work, steal a thread from the peer's tree. ARCH §8.4. Per-CPU run-tree spinlock (currently NULL no-op). Lock-acquisition pattern for cross-CPU access. Spec: `Steal` action. Send IPI_RESCHED to target CPU when placing cross-CPU.

**P2-Cf: finish_task_switch** (~100 LOC). Closes P2-Bb trip-hazard #16 (SMP wait/wake race). Drop run-queue lock only AFTER context switch completes. Linux's pattern.

**P2-Cg: scheduler.tla SMP refinement** (~150 LOC TLA+). Per-CPU runqueues + Steal action + IPI ordering invariant (I-18).

### Option B: defer P2-Ce/Cf/Cg, tackle P2-D (rfork)

P2-D (rfork + exits + wait) is independent of work-stealing. Could land before P2-Ce — gives the actual "process" model. Then return to P2-C for the SMP scheduler integration.

**Risk**: thread migration design depends on work-stealing, so P2-D's rfork would need a placeholder for "current CPU's run tree" until P2-Ce lands. Less coherent.

### Recommendation: A (continue P2-C linearly to closure)

P2-Ce + Cf are small-ish and naturally extend the per-CPU sched work just landed. P2-Cg closes the spec side. Then P2-D introduces multi-process semantics on a fully-realized SMP scheduler.

---

## Trip-hazards summary (cumulative as of P2-Cdc)

63 trip-hazards documented in `docs/phase2-status.md`. Highlights:

**P2-A (1-14)**: TPIDR_EL1 init, no thread guard pages, IRQ mask discipline (closed by P2-Bc), sched-state respect (closed by P2-Bb), thread_trampoline halt, scheduler.tla sketch, struct sizes pinned, kstack PA-cast, tools/test.sh tail-20, magazines drain, kstack zeroing, struct sizes, magic at offset 0, BTI BTYPE values.

**P2-Bb (15-20)**: sched() respects prev->state, SMP wait/wake race window (P2-Cf closes), sleep keeps IRQ mask, single-waiter Rendez, struct Thread 208 B, deadlock detection assumes UP.

**P2-Bc (21-27)**: thread_trampoline IRQ unmask asymmetric, g_need_resched per-CPU at v1.0 P2-Cd, sched_tick state guard, slice replenish on RUNNABLE→RUNNING, struct Thread 216 B, vectors.S IRQ slot 28/32, LatencyBound spec deferred.

**P2-Ca (28-33)**: secondaries inert beyond trampoline (closed by P2-Cb/Cd), g_cpu_online cache coherence, secondary entry PA formula, PSCI conduit caching, DTB_MAX_CPUS hardcoded, mmu_enable split.

**P2-Cb (34-41)**: PAC keys MUST be identical across CPUs, pac_apply_this_cpu MUST stay leaf, per-CPU stack sizing, per_cpu_main noreturn, g_cpu_online vs g_cpu_alive, VBAR_EL1 shared, TPIDR_EL1 NULL on secondaries, cross-CPU PAC + thread migration.

**P2-Cc (42-49)**: msr/mrs sp_el1 UNDEFINED at EL1, both vector groups live, SP_EL1 LOW PA → HIGH VA after MMU, g_exception_stack_observed slots 1+ remain 0 at v1.0 P2-Cdc closes this, EXCEPTION_STACK_SIZE = 4096, smp_cpu_idx_self flattens MPIDR, stack-overflow detection works.

**P2-Cd (50-57)**: per-CPU run trees + vd_counter + need_resched + initialized; sched_init takes cpu_idx; thread_init_per_cpu_idle; sched_remove_if_runnable scans all CPUs; sched_runnable_count aggregates; per-CPU initialized gates sched.

**P2-Cdc (58-63)**: gic_enable_irq SGI/PPI now per-CPU, redist_init_cpu walks per-CPU frame, ICC_SGI1R_EL1 encoding pinned for flat-Aff0, IPI_RESCHED = SGI 0, per_cpu_main idle loop = sched()+wfi, smp_cpu_ipi_init order before g_cpu_alive flip.

---

## Verify on session pickup

```bash
git log --oneline -3
# Expect: 2648053 P2-Cdc: hash fixup / 9b8ecd0 P2-Cdc: GIC SGI infrastructure + IPI_RESCHED / af38884 P2-Cd: hash fixup

git status
# Expect: clean (only docs/estimate.md + loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 21/21 PASS, ~104 ms boot, "smp:  4/4 cpus online (boot + 3 secondaries via PSCI HVC)"
# + "exception: per-CPU SP_EL1 ..." + "ipi:  GICv3 SGIs live ..."

tools/test.sh --sanitize=undefined
# Expect: 21/21 PASS, ~140 ms boot

tools/test-fault.sh
# Expect: 3/3 PASS

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct

export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler.cfg scheduler.tla 2>&1 | tail -3
# Expect: 1413 states generated, 283 distinct states found
```

If any fails on a clean checkout, something regressed since the handoff — investigate before proceeding.

---

## Open follow-ups

- **U-30 (kstack zeroing on free)**: deferred at P2-A R4 F48. KP_ZERO is the documented escape hatch.
- **R5 audit**: covers P2-B + P2-C cumulative. Run at Phase 2 close.
- **LatencyBound liveness spec**: deferred to Phase 2 close. Requires weak fairness + explicit Slice variable.
- **Full EEVDF math**: deferred — meaningful only with weight ≠ 1 (Phase 5+).
- **Per-thread guard page**: Phase 2 close.
- **finish_task_switch pattern**: P2-Cf (closes P2-Bb #16).
- **Cross-CPU thread placement + IPI_RESCHED on placer side**: P2-Ce.
- **scheduler.tla SMP refinement**: P2-Cg (Steal action + IPI ordering invariant I-18).
- **IPI_TLB_FLUSH/HALT/GENERIC**: deferred until use-cases arrive (TLB shootdown for namespace rebind in Phase 5+; shutdown for clean halt; generic callback delivery).

---

## Naming holds (cumulative)

- **Plan 9 idiom names KEPT**: `sleep`, `wakeup`, `Rendez`. (P2-Bb)
- **Linux/Plan 9 standard preempt names KEPT**: `sched_tick`, `need_resched`, `preempt_check_irq`. (P2-Bc)
- **Standard SMP names KEPT**: `smp_init`, `smp_cpu_count`, `secondary_entry`, `g_cpu_online`, `g_cpu_alive`, `smp_cpu_idx_self`. (P2-Ca/Cd)
- **PSCI names pinned by Arm DEN 0022D**: PSCI_CPU_ON_64, PSCI_SUCCESS, etc. (P2-Ca)
- **GIC names pinned by ARM IHI 0069**: GICR_*, GICD_*, ICC_SGI1R_EL1, IPI_RESCHED. (P2-Cdc)
- **Standard ARM names KEPT**: `g_exception_stacks`, `EXCEPTION_STACK_SIZE`. (P2-Cc)

Held for explicit signoff:
- `_hang` → `_torpor` (marsupial deep-sleep state).
- Audit-prosecutor agent → stays "prosecutor" for Stratum continuity.

The user has invited more thematic suggestions ("don't be shy"). P2-Cc/Cd/Cdc held existing names — ARM/Linux/GIC vocabulary is loadbearing across the ecosystem; renaming would create a translation barrier without adding clarity.

---

## Closing notes

This session pushed Phase 2 forward through 3 sub-chunks (P2-Cc, P2-Cd, P2-Cdc), 6 commits, ~500 LOC C+asm + ~80 LOC TLA+/spec maintenance docs, ~6700 LOC total. The kernel now has:
- Per-CPU exception stacks via SPSel=0 (kernel stack overflow detection actually works now).
- Per-CPU run trees, vd_counters, need_resched flags, sched_init with cpu_idx, idle threads on every CPU.
- Cross-CPU IPI delivery via GICv3 SGIs (IPI_RESCHED = SGI 0).
- Boot CPU can wake any secondary's WFI; secondaries process their own run trees on each wake.

Remaining for Phase 2 SMP closure: P2-Ce (work-stealing — cross-CPU thread placement using IPI_RESCHED), P2-Cf (finish_task_switch — closes P2-Bb wait/wake race), P2-Cg (scheduler.tla SMP refinement: per-CPU runqueues + Steal action + IPI ordering invariant I-18). Then P2-D/E/F/G/H.

Posture is excellent: 21/21 tests + UBSan-clean + fault matrix + KASLR distinct all PASS; spec TLC-clean; 0 audit findings open. Next session has a clean foundation.

The thylacine runs on 4 CPUs, and they can talk to each other.
