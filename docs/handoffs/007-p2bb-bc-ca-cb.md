# Handoff 007 — P2-Bb + P2-Bc + P2-Ca + P2-Cb landed

**Date**: 2026-05-05
**Tip**: `13fff49` (P2-Cb hash fixup)
**Phase**: Phase 2 in progress. P2-A through P2-Cb landed. P2-Cc (per-CPU exception stacks) and P2-Cd (per-CPU run trees + IPIs + idle threads) are the next sub-chunks.

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework. Mandatory.
2. **This file** — canonical pickup at P2-Cb close.
3. `docs/handoffs/006-p2a-process-model.md` — P2-A close (predecessor).
4. `docs/handoffs/005-phase1-close.md` — Phase 1 close.
5. `docs/VISION.md` + `docs/ARCHITECTURE.md` + `docs/ROADMAP.md` — binding scripture.
6. `docs/phase2-status.md` — Phase 2 sub-chunk plan with all landed rows + 41 trip-hazards.
7. `docs/REFERENCE.md` + `docs/reference/15-scheduler.md` + `16-rendez.md` + `17-smp-bringup.md` — as-built per-subsystem.
8. `specs/scheduler.tla` + `specs/SPEC-TO-CODE.md` — formal spec + impl mapping.
9. `memory/project_active.md` — quick state summary.

---

## What landed in this session (8 commits)

### P2-Bb: Plan 9 wait/wake — sleep + wakeup over Rendez

Commits: `4e108a8` (substantive) + `c798381` (hash fixup).

- New `kernel/include/thylacine/rendez.h` — `struct Rendez { spin_lock_t lock; struct Thread *waiter; }`. Plan 9 single-waiter convention.
- `sleep(r, cond, arg)` + `wakeup(r)` impl in `kernel/sched.c` (~110 LOC). Maps to scheduler.tla `WaitOnCond` / `WakeAll`. spin_lock_irqsave bracketing for IRQ-mask discipline.
- `sched()` refactored to respect pre-set `prev->state`: RUNNING → yield-insert; SLEEPING/EXITING → leave alone.
- struct Thread extended (200→208 B) with `rendez_blocked_on`.
- Defensive `prev == next` re-insert+return guard for SMP race window (P2-C closes via finish_task_switch).
- Tests: rendez.{sleep_immediate_cond_true, basic_handoff, wakeup_no_waiter}.
- Reference doc 16-rendez.md.

### P2-Bc: scheduler-tick preemption + IRQ-mask discipline

Commits: `518d294` (substantive) + `374c687` (hash fixup).

- `sched_tick()` (decrements current's slice; on expiry sets g_need_resched + replenishes) called from `timer_irq_handler`.
- `preempt_check_irq()` called from `arch/arm64/vectors.S` IRQ-return slot. Calls sched() if need_resched + initialized + valid current.
- struct Thread extended (208→216 B) with `slice_remaining`. THREAD_DEFAULT_SLICE_TICKS = 6 (Linux EEVDF default at 1 kHz).
- vectors.S IRQ slot: `bl preempt_check_irq` after `bl exception_irq_curr_el` (28 of 32 instructions used).
- thread_trampoline `msr daifclr, #2` to unmask IRQs at first thread entry (subsequent re-entries inherit DAIF via sched()'s irqrestore).
- sched() and ready() bracket run-tree mutations with `spin_lock_irqsave(NULL)` (closes P2-A trip-hazard #3).
- Test: scheduler.preemption_smoke (CPU-bound threads share CPU; 10× fairness check).
- Reference doc 15-scheduler.md preamble extended.

### P2-Ca: SMP secondary bring-up via PSCI

Commits: `0ffe554` (substantive) + `0fb1832` (hash fixup).

- DTB cpu/psci enumeration in `lib/dtb.c`: `dtb_cpu_count`, `dtb_cpu_mpidr`, `dtb_psci_method`. `DTB_MAX_CPUS = 8`.
- `arch/arm64/psci.{h,c}` (new): PSCI 0.2+ standard function IDs. HVC/SMC SMCCC dispatch via inline asm with x0..x3 register pinning.
- `secondary_entry` asm trampoline in `arch/arm64/start.S` (P2-Ca minimal): validate idx, flip g_cpu_online[idx], dsb sy, WFI loop.
- `mmu_enable` refactored: split into `mmu_program_this_cpu` + `mmu_enable` (build_page_tables + program).
- `kernel/smp.{h,c}` (new): `g_cpu_online[]` BSS, `smp_init` with PSCI bringup loop, 100-tick timeout per secondary, dmb ish polling.
- `kernel/main.c`: psci_init() then smp_init() after sched_init. Banner: `smp:  N/M cpus online (boot + K secondaries via PSCI HVC)`. `cpus:` line bumped from hardcoded 1 to DTB-reported.
- Test: smp.bringup_smoke verifies all online flags.
- Reference doc 17-smp-bringup.md.

### P2-Cb: per-CPU init at high VA (PAC + MMU + VBAR + TPIDR)

Commits: `a9d1a82` (substantive) + `13fff49` (hash fixup).

- PAC keys refactored: `pac_derive_keys` + `pac_apply_this_cpu` asm functions in `arch/arm64/start.S`. Both leaf, no paciasp.
- `g_pac_keys[8]` BSS — 8 u64 key halves shared across CPUs (cross-CPU PAC consistency required for thread migration at P2-Ce).
- `g_secondary_boot_stacks[7][16384]` = 112 KiB BSS for per-secondary 16 KiB boot stacks.
- Extended `secondary_entry`: validate idx → save callee-saved → set SP from per-CPU stack → flip g_cpu_online (early signal) → bl pac_apply_this_cpu → bl mmu_program_this_cpu → adrp/add per_cpu_main + bl kaslr_high_va_addr → blr to high VA per_cpu_main(idx).
- `per_cpu_main(int idx)` in `kernel/smp.c`: noreturn. Sets VBAR_EL1 to _exception_vectors, TPIDR_EL1 = NULL (no per-CPU current_thread at P2-Cb), flips g_cpu_alive[idx], idle WFI loop.
- `smp_init` wait now watches `g_cpu_alive` (stricter signal — catches PAC/MMU/VBAR/TPIDR failures).
- Test extended to verify both g_cpu_online + g_cpu_alive.
- Reference doc 17-smp-bringup.md extended.

---

## Current state at handoff

- **Tip**: `13fff49`.
- **Phase**: Phase 2 in progress. P2-A through P2-Cb landed.
- **Working tree**: clean (only untracked: `docs/estimate.md` + `loc.sh`, pre-existing).
- **`tools/test.sh`**: PASS. 18/18 in-kernel tests; ~112 ms boot (production), ~143 ms (UBSan). 4/4 cpus online via PSCI HVC; secondaries reach high-VA per_cpu_main.
- **`tools/test-fault.sh`**: PASS (3/3 protections fire under attack). Primary's PAC keys now derived once + applied via the same `pac_apply_this_cpu` used by secondaries — no regression in primary's PAC.
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **Specs**: `scheduler.tla` TLC-clean at 283 distinct states. `scheduler_buggy.cfg` produces missed-wakeup counterexample at depth 4 (29 states). Spec UNCHANGED at P2-Bc + P2-Ca + P2-Cb (preempt path maps to existing Yield action; PSCI/per-CPU-init touch no kernel concurrency invariant). Cross-CPU spec work begins at P2-Cd (IPI ordering ARCH §28 I-18).
- **LOC**: ~6200. Kernel ELF: ~281 KB debug (production); ~305 KB (UBSan).
- **In-kernel tests**: 18. New this session: rendez.{sleep_immediate_cond_true, basic_handoff, wakeup_no_waiter}, scheduler.preemption_smoke, smp.bringup_smoke.
- **Open audit findings**: 0. R5 audit deferred to Phase 2 close (will cover P2-Ba + P2-Bb + P2-Bc + P2-Ca + P2-Cb + P2-Cc + P2-Cd + P2-Ce + P2-Cf + P2-Cg cumulative — single audit covers all sub-chunks of P2-B + P2-C).

## What's NEXT

**P2-Cc + P2-Cd are the next big steps.** Could land as one chunk or two — recommend two for tractability.

### P2-Cc: per-CPU exception stacks (~100 LOC)

**Scope**: Each CPU gets its own exception-context stack. Closes P1-F shared-stack limitation (per phase2-status.md trip-hazard #2 + #21).

**Why**: Currently when an IRQ fires on a secondary, KERNEL_ENTRY saves the exception context onto whatever SP is current — the secondary's per-CPU boot stack at P2-Cb. Multiple nested exceptions (rare but possible) on the same CPU would clobber. A dedicated per-CPU exception stack switched-to via SP_EL1 reset on entry is the standard pattern.

**Approach**:
- Allocate `g_exception_stacks[NCPUS][EXC_STACK_SIZE]` BSS (e.g., 4 KiB per CPU = 32 KiB total).
- vectors.S KERNEL_ENTRY macro switches SP to per-CPU exception stack at entry, restores at exit.
- Per-CPU exception-stack base lookup: by MPIDR or per-CPU storage.

**Trip-hazards landed**: phase2-status.md #2 + #21 closed.

### P2-Cd: per-CPU run trees + idle threads + GIC SGI infrastructure + IPIs (~500 LOC)

**Scope**: The biggest remaining piece of SMP. Covers:

1. **Per-CPU run trees**: `g_run_tree` becomes `g_run_tree[NCPUS][BANDS]`. Per-CPU `g_vd_counter`. `smp_cpu_idx()` accessor (from MPIDR or per-CPU storage, e.g. tpidr_el1 high bits or a separate tpidr_el2 if at EL2 — at EL1 use a per-CPU pointer trick).
2. **Per-CPU sched_init**: each CPU initializes its own run tree at per_cpu_main entry.
3. **Per-CPU idle threads**: each secondary creates its own idle Thread, sets TPIDR_EL1 to it. Secondary's per_cpu_main becomes the idle thread's entry.
4. **GIC SGI infrastructure**: Software Generated Interrupts (SGI) for cross-CPU coordination. GIC_INTID_SGI_RANGE = 0..15. `gic_send_sgi(cpu_mask, intid)`.
5. **IPI types**: `IPI_RESCHED` (peer should rerun scheduler), `IPI_TLB_FLUSH` (peer should flush TLB), `IPI_HALT` (peer should halt at shutdown), `IPI_GENERIC` (peer should run callback). Per ARCH §20.4.
6. **Per-CPU IRQ routing**: secondary CPU's GIC redistributor initialized; secondary's CPU interface enabled. Timer SGI per CPU.
7. **Spec refinement**: scheduler.tla per-CPU runqueues + IPI ordering invariant (I-18).

**This is genuinely big.** Could split further into Cda/Cdb/Cdc.

### P2-Ce: work-stealing (~200 LOC)

When a CPU's run tree is empty, steal from a peer's tree. Cross-CPU lock acquisition pattern. ARCH §8.4. Spec: `Steal` action.

### P2-Cf: finish_task_switch pattern (~100 LOC)

Closes P2-Bb trip-hazard #16 (SMP wait/wake race). Drop run-queue lock only AFTER context switch completes. Linux's pattern.

### P2-Cg: scheduler.tla SMP refinement (~150 LOC TLA+)

Per-CPU runqueues + Steal action + IPI ordering invariant (I-18).

### After P2-C

P2-D (rfork) → P2-E (namespace) → P2-F (handles + VMO) → P2-G (ELF + init) → P2-H (closing audit + spec finalization). The Phase 2 close audit (R5) covers all P2 sub-chunks cumulative.

---

## Trip-hazards summary (cumulative as of P2-Cb)

41 trip-hazards documented in `docs/phase2-status.md`. Highlights:

**P2-A (1-14)**: TPIDR_EL1 init, no thread guard pages, no IRQ mask around thread_switch (P2-Bc closed), thread_switch unconditionally sets RUNNABLE (P2-Bb closed via state-respecting sched), thread_trampoline halts on entry-return, scheduler.tla sketch at P2-A (P2-A R4 added wait/wake), struct Context size pinned, kstack_base PA-cast, tools/test.sh tail-20, magazines drain pre-baseline, U-30 (kstack zeroing on free), struct Proc/Thread sizes pinned, magic at offset 0, BTI BTYPE values.

**P2-Bb (15-20)**: sched() respects prev->state, SMP wait/wake race window (P2-Cf closes), sleep keeps IRQ mask across sched, single-waiter Rendez convention, struct Thread size 208 B, sched()'s deadlock detection assumes UP-no-other-CPU.

**P2-Bc (21-27)**: thread_trampoline IRQ unmask asymmetric, g_need_resched volatile bool (SMP needs atomic), sched_tick state guard, slice replenish on RUNNABLE→RUNNING, struct Thread size 216 B, vectors.S IRQ slot 28/32, LatencyBound spec deferred.

**P2-Ca (28-33)**: secondaries inert beyond trampoline (P2-Cb mostly closed for execution; P2-Cd closes for scheduling), g_cpu_online cache coherence, secondary entry PA formula dependency on _kernel_start, PSCI conduit caching, DTB_MAX_CPUS hardcoded throughout, mmu_enable split.

**P2-Cb (34-41)**: PAC keys MUST be identical across CPUs (cross-CPU thread migration), pac_apply_this_cpu MUST stay leaf (init keys without paciasp), per-CPU stack sizing (16 KiB × 7 = 112 KiB BSS), per_cpu_main noreturn, g_cpu_online vs g_cpu_alive distinction, VBAR_EL1 shared across CPUs, TPIDR_EL1 NULL on secondaries, cross-CPU PAC + thread migration interaction.

---

## Verify on session pickup

Before starting work, verify:

```bash
git log --oneline -3
# Expect: 13fff49 P2-Cb: hash fixup  /  a9d1a82 P2-Cb: per-CPU init at high VA  /  ...

git status
# Expect: clean (only docs/estimate.md + loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 18/18 PASS, ~112 ms boot, "smp:  4/4 cpus online (boot + 3 secondaries via PSCI HVC)"

tools/test.sh --sanitize=undefined
# Expect: 18/18 PASS, ~143 ms boot

tools/test-fault.sh
# Expect: 3/3 PASS

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct

export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler.cfg scheduler.tla 2>&1 | tail -3
# Expect: 1413 states generated, 283 distinct states found
```

If any of these fails on a clean checkout, something regressed since the handoff — investigate before proceeding.

---

## Open follow-ups

- **U-30 (kstack zeroing on free)**: deferred at P2-A R4 F48. KP_ZERO is the documented escape hatch. Phase 3's user-facing allocators must use KP_ZERO consistently.
- **R5 audit**: covers P2-B + P2-C cumulative. Run at Phase 2 close (after P2-Cg lands).
- **LatencyBound liveness spec**: deferred to Phase 2 close refinement of scheduler.tla. Requires weak fairness annotations + explicit Slice variable.
- **Full EEVDF math** (vd_t = ve_t + slice × W_total / w_self): deferred — meaningful only with weight ≠ 1, which is Phase 5+ when sched_setweight is exposed.
- **Per-thread guard page**: Phase 2 close (per phase2-status.md trip-hazard #2).
- **finish_task_switch pattern**: P2-Cf (closes P2-Bb trip-hazard #16).

---

## Naming holds (cumulative)

- **Plan 9 idiom names KEPT**: `sleep`, `wakeup`, `Rendez`. (P2-Bb)
- **Linux/Plan 9 standard preempt names KEPT**: `sched_tick`, `need_resched`, `preempt_check_irq`. (P2-Bc)
- **Standard SMP names KEPT**: `smp_init`, `smp_cpu_count`, `secondary_entry`, `g_cpu_online`. (P2-Ca)
- **PSCI names pinned by Arm DEN 0022D spec**: PSCI_CPU_ON_64, PSCI_SUCCESS, etc. (P2-Ca)

Held for explicit signoff:
- `_hang` → `_torpor` (marsupial deep-sleep state).
- Audit-prosecutor agent → stays "prosecutor" for Stratum continuity.

---

## Closing notes

This session pushed Phase 2 forward through 4 sub-chunks (P2-Bb, P2-Bc, P2-Ca, P2-Cb), 8 commits, ~1800 LOC C+asm, ~6200 LOC total. The kernel now has:
- Plan 9 wait/wake live (sleep + wakeup over Rendez).
- Timer-IRQ-driven preemption with IRQ-mask discipline.
- SMP secondaries brought up via PSCI, reaching the high-VA C runtime with PAC + MMU + VBAR + TPIDR all configured.

Remaining for Phase 2: P2-Cc (per-CPU exception stacks), P2-Cd (the BIG one — per-CPU run trees + idle threads + GIC SGI + IPIs), P2-Ce (work-stealing), P2-Cf (finish_task_switch), P2-Cg (scheduler.tla SMP refinement). Then P2-D/E/F/G/H to wrap Phase 2.

Posture is excellent: 18/18 tests + UBSan-clean + fault matrix + KASLR distinct all PASS; spec TLC-clean; 0 audit findings open. Next session has a clean foundation.

The thylacine runs on 4 CPUs.
