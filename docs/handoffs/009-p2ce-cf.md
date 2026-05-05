# Handoff 009 — P2-Ce + P2-Cf landed (SMP scheduler end-to-end functional)

**Date**: 2026-05-05
**Tip**: `0df6680` (P2-Cf hash fixup)
**Phase**: Phase 2 in progress. P2-A through P2-Cf landed. The SMP scheduler is end-to-end functional at the C/asm layer: per-CPU run trees, work-stealing, finish_task_switch handoff, on_cpu wait/wake race close, GIC SGI cross-CPU IPIs. **P2-Cg (scheduler.tla SMP refinement) is the next sub-chunk** — it lifts the implementation discipline to the spec level (per-CPU runqueues + Steal action + IPI ordering invariant I-18) and closes the P2-C section.

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework. Mandatory.
2. **This file** — canonical pickup at P2-Cf close.
3. `docs/handoffs/008-p2cc-cd-cdc.md` — predecessor (P2-Cc..Cdc).
4. `docs/handoffs/007-p2bb-bc-ca-cb.md` — P2-Bb..Cb.
5. `docs/VISION.md` + `docs/ARCHITECTURE.md` + `docs/ROADMAP.md` — binding scripture.
6. `docs/phase2-status.md` — Phase 2 sub-chunk plan with all landed rows + cumulative trip-hazards (#1-77).
7. `docs/REFERENCE.md` snapshot + `docs/reference/15-scheduler.md` (per-CPU dispatch + work-stealing + finish_task_switch sections) + `17-smp-bringup.md`.
8. `specs/scheduler.tla` + `specs/SPEC-TO-CODE.md` — formal spec. **P2-Cg is the next chunk that touches these.**
9. `memory/project_active.md` — quick state summary.

---

## What landed in this session (5 sub-chunks, 11 commits — including handoff 008)

### P2-Cc — per-CPU exception stacks via SPSel=0

`8eb4aee` substantive + `a9757db` hash fixup.

- `g_exception_stacks[DTB_MAX_CPUS][4096]` (32 KiB BSS); `EXCEPTION_STACK_SIZE` macro; `smp_cpu_idx_self()` reads MPIDR_EL1.Aff0.
- start.S `_real_start` step 4.6 + `secondary_entry`: SPSel-dance splits SP into SP_EL0 = boot stack + SP_EL1 = per-CPU exception stack (`msr sp_el0` allowed at EL1; `msr sp_el1` is UNDEFINED; use `mov sp` while SPSel=1).
- step 8.5 + post-`mmu_program_this_cpu`: SP_EL1 re-anchored to HIGH VA via `kaslr_high_va_addr` + same dance.
- vectors.S keeps both Current-EL/SP_EL0 (0x000+0x080) AND SP_ELx (0x200+0x280) Sync+IRQ slots LIVE — kernel transits through SPSel=1 transiently after sched-from-IRQ + thread_trampoline daifclr.
- timer_irq_handler captures `&local` once per CPU into `g_exception_stack_observed[]` for runtime verification.
- New test smp.exception_stack_smoke. Closes vectors.S P1-F "KNOWN LIMITATION" — kernel stack overflow detection actually works.

### P2-Cd — per-CPU run trees + idle threads (Cda + Cdb)

`a604cd7` substantive + `af38884` hash fixup.

- `struct CpuSched { run_tree[BANDS]; vd_counter; initialized; idle; }`. `g_cpu_sched[DTB_MAX_CPUS]` BSS.
- sched(), ready(), sched_tick(), preempt_check_irq(), sched_remove_if_runnable(), sched_runnable_count() index by smp_cpu_idx_self().
- `g_need_resched` is now `volatile bool[DTB_MAX_CPUS]`.
- sched_init takes cpu_idx. main.c calls sched_init(0); per_cpu_main calls sched_init(idx).
- `thread_init_per_cpu_idle(cpu_idx)` allocates idle Thread (no kstack — uses per-CPU boot stack).
- per_cpu_main wired: VBAR + idle thread + TPIDR_EL1 + sched_init + g_cpu_alive + WFI loop.
- New test smp.per_cpu_idle_smoke. New `sched_idle_thread()` accessor.

### P2-Cdc — GIC SGI infrastructure + IPI_RESCHED

`9b8ecd0` substantive + `2648053` hash fixup.

- `gic_init_secondary(cpu_idx)` per-CPU GIC redistributor + CPU interface (refactored `redist_init_cpu0` → `redist_init_cpu(idx)` with per-CPU base = `g_redist_base + idx*0x20000`).
- `gic_send_ipi(target, sgi)` writes ICC_SGI1R_EL1 with TargetList encoding for QEMU virt's flat-Aff0 cluster.
- `IPI_RESCHED = SGI 0`. `g_ipi_resched_count[]`. `ipi_resched_handler` increments per-CPU counter.
- `smp_cpu_ipi_init(cpu_idx)` from per_cpu_main. per_cpu_main idle loop: pure WFI → `for(;;){sched();wfi;}`.
- **Bug fix**: gic_enable_irq for SGI/PPI was hardcoded to CPU 0's redist; refactored to use `cpu_redist_base(smp_cpu_idx_self())`.
- New test smp.ipi_resched_smoke. New banner: `ipi: GICv3 SGIs live (...)`.

### P2-Ce — work-stealing + finish_task_switch handoff

`fa6ded0` substantive + `e9de6c1` hash fixup.

- Real spinlocks via `__atomic_exchange_n` with acquire/release (LSE `swpa` or LL/SC fallback). spin_trylock for cross-CPU. spin_lock_irqsave/irqrestore thread NULL through gracefully.
- Per-CPU run-tree lock added to `struct CpuSched`. ready()/sched()/sched_remove_if_runnable take it.
- `try_steal(cs)` scans peers via spin_trylock, takes one runnable thread (highest non-empty band), rebases vd_t to caller's clock. Called in sched()'s no-peer path.
- `cs->pending_release_lock` finish_task_switch handoff: prev sets it before cpu_switch_context; resume path reads from THIS CPU's slot (destination after migration), releases, clears. Handles cross-CPU thread migration via stealing.
- `sched_finish_task_switch` C helper called from thread_trampoline (asm) on fresh thread first-run. NULL-guard lets thread_switch (legacy direct-switch) coexist.
- New test smp.work_stealing_smoke: boot creates N threads, ready()s on its tree, sends IPI_RESCHED to secondaries; verifies at least one secondary's per-CPU counter incremented.

### P2-Cf — on_cpu flag + SMP wait/wake race close

`6cdfc8a` substantive + `0df6680` hash fixup.

- `volatile bool on_cpu` per Thread (size 216→224 B; `_Static_assert` updated). True while running on some CPU.
- `cs->prev_to_clear_on_cpu` handoff: prev's sched/thread_switch sets it; destination CPU's resume path clears prev's on_cpu via `__atomic_store_n(..., __ATOMIC_RELEASE)`.
- `wakeup()` spin-waits for `__atomic_load_n(&t->on_cpu)` to be false before transitioning waiter to RUNNABLE + ready(). **Closes P2-Bb trip-hazard #16**: a peer can't pick a half-saved ctx mid-cpu_switch_context.
- New public helpers `sched_arm_clear_on_cpu(prev)` + `sched_finish_task_switch()` so thread_switch (legacy) participates in the protocol.
- thread_init / thread_create / thread_init_per_cpu_idle initialize on_cpu deterministically.
- Pattern matches Linux's `task->on_cpu`.

---

## Current state at handoff

- **Tip**: `0df6680`.
- **Phase**: Phase 2 in progress. P2-A through P2-Cf landed. **P2-Cg (scheduler.tla SMP refinement) is the next sub-chunk.**
- **Working tree**: clean (only `docs/estimate.md` + `loc.sh` untracked, pre-existing).
- **`tools/test.sh`**: PASS. 22/22 in-kernel tests; ~224 ms boot (production), ~250 ms (UBSan). 4/4 cpus online via PSCI HVC + per-CPU sched + idle threads + IPIs + work-stealing + on_cpu serialization.
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (3/3).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**: `scheduler.tla` TLC-clean at 283 distinct states (BUGGY=FALSE). `scheduler_buggy.cfg` produces missed-wakeup counterexample. **Spec UNCHANGED at P2-Cc/Cd/Cdc/Ce/Cf** — all SMP infrastructure is at the impl level. P2-Cg lifts to spec.
- **LOC**: ~6950.
- **Kernel ELF**: ~308 KB debug.
- **In-kernel tests**: 22. New this session: smp.exception_stack_smoke (P2-Cc), smp.per_cpu_idle_smoke (P2-Cd), smp.ipi_resched_smoke (P2-Cdc), smp.work_stealing_smoke (P2-Ce). P2-Cf added no new test (covered by existing rendez + work_stealing tests under on_cpu serialization).
- **Open audit findings**: 0. R5 audit deferred to Phase 2 close.

## What's NEXT

### Option A (recommended): P2-Cg → P2-D

**P2-Cg: scheduler.tla SMP refinement** (~150 LOC TLA+). Lift the per-CPU + Steal + IPI discipline from impl to spec. Concretely:

- Extend `scheduler.tla` with a `Cpu` constant or set (try `{0, 1}` first for tractability).
- Per-CPU runqueues: `runq[c]` rather than a single `runq`.
- Per-CPU `current[c]`, `vd_counter[c]`.
- `Steal(stealer, victim)` action: stealer with empty runq pulls one thread from victim's runq.
- `IPI_RESCHED(src, dst)` action: src marks dst's need_resched and "delivers" the wake.
- IPI ordering invariant (ARCH §28 I-18): IPIs from CPU A to CPU B are processed in send order. Encode as a per-(src,dst) pair FIFO queue; `IPI_DELIVER` action processes the head.
- Update `NoMissedWakeup` to per-CPU; check across all CPUs.
- Update `SPEC-TO-CODE.md` mapping each new action to its impl site (sched.c try_steal, gic.c gic_send_ipi, vectors.S IRQ slot, ipi_resched_handler).
- Run TLC at small bounds (2 CPUs, 2 threads). Aim for state count under ~10k for fast iteration.
- Add `scheduler_buggy_steal.cfg` showing what breaks if Steal violates an invariant (e.g., stealing without locking → thread in two runqueues).

Best-case ~50-80k tokens; iterative case 150k+ if TLC surfaces modeling issues.

**P2-D: rfork + exits + wait** (~300 LOC). The actual process model. `kernel/proc.c` rfork(flags) implementing the resource-share / clone matrix per ARCH §7.4. exits() terminate. wait() reap. Address-space tear-down on exit. Thread cleanup. Child re-parenting to PID 1 (init).

After: P2-E namespace + bind/mount, P2-F handles + VMO, P2-G ELF + init, P2-H closing audit.

### Option B: defer P2-Cg, jump to P2-D

P2-D is independent of spec refinement. Could land before P2-Cg. But spec-first policy in CLAUDE.md says invariant-bearing impl lands WITH spec — P2-Ce/Cf added work-stealing + cross-CPU race close without spec refinement, which is a deviation. P2-Cg formally closes the deviation.

### Recommendation: A (close P2-Cg first)

Spec-first discipline. Closes the SMP scheduler section cleanly. Then P2-D introduces multi-process semantics on a fully-realized + fully-specified SMP scheduler.

---

## Trip-hazards summary (cumulative as of P2-Cf)

77 trip-hazards documented in `docs/phase2-status.md`. Highlights from this session:

**P2-Cc (#42-49)**: msr/mrs sp_el1 UNDEFINED at EL1; both vector groups live; SP_EL1 LOW PA → HIGH VA after MMU; g_exception_stack_observed slots 1+ remain 0 at v1.0 (closed at P2-Cdc); EXCEPTION_STACK_SIZE = 4096; smp_cpu_idx_self flattens MPIDR; stack-overflow detection works.

**P2-Cd (#50-57)**: per-CPU run trees + vd_counter + need_resched + initialized; sched_init takes cpu_idx; thread_init_per_cpu_idle for secondaries; sched_remove_if_runnable scans all CPUs; sched_runnable_count aggregates; per-CPU initialized gates sched.

**P2-Cdc (#58-63)**: gic_enable_irq SGI/PPI now per-CPU, redist_init_cpu walks per-CPU frame, ICC_SGI1R_EL1 encoding pinned for flat-Aff0, IPI_RESCHED = SGI 0, per_cpu_main idle loop = sched()+wfi, smp_cpu_ipi_init order before g_cpu_alive flip.

**P2-Ce (#64-70)**: real spinlocks via atomic exchange-acquire/release; per-CPU run-tree lock; cs->pending_release_lock finish_task_switch handoff; sched_finish_task_switch NULL-guard for thread_switch coexistence; try_steal scans peers in CPU order (rotate-start at Cg); prev == next after steal; boot-time regression to ~195 ms.

**P2-Cf (#71-77)**: struct Thread.on_cpu (volatile bool); cs->prev_to_clear_on_cpu handoff; wakeup spins on waiter's on_cpu; sched_arm_clear_on_cpu + sched_finish_task_switch helpers; on_cpu init in kthread/idle/thread_create; struct Thread size 224 B; same-thread wakeup deadlock corner case (misuse).

---

## Verify on session pickup

```bash
git log --oneline -3
# Expect: 0df6680 P2-Cf: hash fixup / 6cdfc8a P2-Cf: on_cpu flag + ... / e9de6c1 P2-Ce: hash fixup

git status
# Expect: clean (only docs/estimate.md + loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 22/22 PASS, ~224 ms boot, full SMP banner with smp:/exception:/ipi: lines

tools/test.sh --sanitize=undefined
# Expect: 22/22 PASS

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
- **scheduler.tla SMP refinement**: P2-Cg (next sub-chunk) — Steal action + IPI ordering invariant I-18.
- **try_steal rotate-start**: scan peers starting at a rotating CPU index per call to spread load. v1.0 P2-Ce uses fixed CPU order; refinement at P2-Cg or post-v1.0.
- **IPI_TLB_FLUSH/HALT/GENERIC**: deferred until use-cases arrive (TLB shootdown for namespace rebind in Phase 5+; shutdown for clean halt; generic callback delivery).
- **Pi 5 / multi-cluster Aff{1,2,3}**: Phase 7 hardening pass — current ICC_SGI1R_EL1 encoding pinned for QEMU virt's flat-Aff0 cluster.

---

## Naming holds (cumulative)

- **Plan 9 idiom names KEPT**: `sleep`, `wakeup`, `Rendez`. (P2-Bb)
- **Linux/Plan 9 standard preempt names KEPT**: `sched_tick`, `need_resched`, `preempt_check_irq`. (P2-Bc)
- **Standard SMP names KEPT**: `smp_init`, `smp_cpu_count`, `secondary_entry`, `g_cpu_online`, `g_cpu_alive`, `smp_cpu_idx_self`. (P2-Ca/Cd)
- **PSCI names pinned by Arm DEN 0022D**: PSCI_CPU_ON_64, PSCI_SUCCESS, etc. (P2-Ca)
- **GIC names pinned by ARM IHI 0069**: GICR_*, GICD_*, ICC_SGI1R_EL1, IPI_RESCHED. (P2-Cdc)
- **Standard ARM names KEPT**: `g_exception_stacks`, `EXCEPTION_STACK_SIZE`. (P2-Cc)
- **Linux finish_task_switch idiom KEPT**: `sched_finish_task_switch`, `pending_release_lock`, `prev_to_clear_on_cpu`, `on_cpu`. (P2-Ce/Cf) — cross-OS vocabulary; renaming would break the mental model that anyone familiar with Linux's scheduler brings.

Held for explicit signoff:
- `_hang` → `_torpor` (marsupial deep-sleep state).
- Audit-prosecutor agent → stays "prosecutor" for Stratum continuity.

The user has invited more thematic suggestions ("don't be shy"). This session held existing names — finish_task_switch, on_cpu, Steal, work-stealing, ICC_SGI1R_EL1, GIC redistributor are all pinned by Linux/ARM vocabulary. Renaming would create a translation barrier without adding clarity.

---

## Closing notes

This session closed the SMP scheduler at the impl level. Five sub-chunks landed (P2-Cc, P2-Cd, P2-Cdc, P2-Ce, P2-Cf), 11 commits (10 implementation + handoff 008 mid-session), ~1500 LOC C+asm + spec maintenance, ~6950 LOC total. The kernel now has:

- Per-CPU exception stacks via SPSel=0 split (kernel stack overflow detection actually works).
- Per-CPU run trees + vd_counters + need_resched + idle threads + sched_init(cpu_idx).
- Cross-CPU IPI delivery via GICv3 SGIs (IPI_RESCHED).
- Work-stealing via try_steal in sched()'s no-peer path; cross-CPU thread placement live.
- finish_task_switch handoff via cs->pending_release_lock — migrated threads release the destination CPU's lock cleanly.
- on_cpu flag + wakeup() spin-on-on_cpu — closes the SMP wait/wake race (P2-Bb #16).

Remaining for Phase 2 SMP closure: **P2-Cg** lifts the discipline to scheduler.tla (per-CPU runqueues + Steal action + IPI ordering invariant I-18). Then **P2-D** kicks off the full process model (rfork + exits + wait).

Posture is excellent: 22/22 tests + UBSan-clean + fault matrix + KASLR distinct all PASS; spec TLC-clean; 0 audit findings open. Next session has a clean foundation.

The thylacine runs on 4 CPUs that share work and respect each other's saves.
