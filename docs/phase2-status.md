# Phase 2 — status and pickup guide

Authoritative pickup guide for **Phase 2: Process model + scheduler + handles + VMO + first formal specs**.

## TL;DR

Phase 2 brings up the process / thread model, EEVDF scheduler, handle table, VMO manager, and the first three formal specs (`scheduler.tla`, `namespace.tla`, `handles.tla`). Multi-process concurrent execution; preemptive EEVDF with three priority bands; SMP-aware on 4 vCPUs; rfork / exec / exits / wait fully working; init runs a UART debug shell. **Spec-first discipline binding from this phase forward** — no invariant-bearing implementation merges without the corresponding TLA+ refinement.

Per `ROADMAP.md §5`. Phase 2 entry chunk **P2-A** (process-model bootstrap) has landed.

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| *(pending)* | **P2-Db**: 1000-iter rfork+exits+wait stress. `proc.rfork_stress_100` → `proc.rfork_stress_1000` (10× iterations) with batched 8-rfork → 8-reap pattern that fans out the run tree's depth before yielding. Per-CPU run counters track placement (sum-must-equal-ITERS verifies no double-reap or missed run). Cross-CPU placement assertion deferred: secondaries don't run their own timer IRQs at this phase, and rfork doesn't IPI_RESCHED peers, so children placed in the local run tree are typically picked by the same CPU before stealing fires. Per-thread guard pages (kstack underflow detection) deferred to P2-Dc — requires L2-block → L3-table demotion in `arch/arm64/mmu.c` (kstacks live in 2 MiB-block-mapped RAM; per-page no-access protection needs 4 KiB granularity). Phase tag P2-Da → P2-Db. | 25/25 tests; ~232 ms boot (production, +36 ms vs P2-Da for the 900 extra stress iterations); ~270 ms UBSan; fault matrix 3/3; KASLR 5/5 distinct; scheduler.tla TLC-clean (10188 states). |
| `4465408` | **P2-Da**: rfork(RFPROC) + exits + wait. struct Proc grew 24→80 bytes (P2-A baseline 24 + state 4 + exit_status 4 + parent 8 + children 8 + sibling 8 + exit_msg 8 + child_done Rendez 16); `_Static_assert` updated. New `enum proc_state { INVALID=0, ALIVE=1, ZOMBIE=2 }`. proc_init_fields sets state=ALIVE + initializes child_done. proc_free now requires state==ZOMBIE (lifecycle gate). New `rfork(unsigned flags, void (*entry)(void *), void *arg)` in `kernel/proc.c` (~50 LOC); only RFPROC supported at v1.0 (other flags extinct loudly — RFNAMEG at P2-E, RFFDG at P2-F, RFMEM at P2-G). Allocates child Proc + initial Thread via `thread_create_with_arg`, links to parent's children list, ready()s the thread. New `exits(msg)` (~30 LOC): re-parents orphan children to kproc; sets exit_status (0 for "ok", 1 otherwise) + exit_msg + state=ZOMBIE; marks calling Thread EXITING; wakes parent's child_done; yields via sched(). Marked __attribute__((noreturn)). New `wait_pid(int *status_out)` (~50 LOC): scans children for ZOMBIE; if found, unlinks + reaps (thread_free + proc_free) + returns PID; if no zombie + has live children, sleeps on child_done Rendez with cond predicate "any child zombie OR no children." Returns -1 if no children at all. New `thread_create_with_arg` extends thread_create signature; both share thread_create_internal helper. thread_trampoline asm extended with `mov x0, x20` before `blr x21` — backward-compatible since x20 defaults to 0 via KP_ZERO. proc_link_child / proc_unlink_child / proc_reparent_children helpers. New tests `proc.rfork_basic_smoke` + `proc.rfork_exits_status` + `proc.rfork_stress_100` (100-iter lifecycle stress). Phase tag P2-Cg → P2-Da. | 25/25 tests; ~196 ms boot (production); ~226 ms UBSan; fault matrix 3/3; KASLR 5/5 distinct; scheduler.tla TLC-clean (10188 states; spec untouched — proc lifecycle uses existing scheduler.tla WaitOnCond/WakeAll discipline via Rendez child_done). |
| `8b8516a` | **P2-Cg**: scheduler.tla SMP refinement. Spec-only chunk. `specs/scheduler.tla` extended (~389 → ~620 LOC TLA+) with three new actions: `Steal(stealer, victim)` (atomic cross-CPU transfer modeling `kernel/sched.c::try_steal`), `IPI_Send(src, dst)` + `IPI_Deliver(src, dst)` (per-(src,dst) FIFO queue with monotonic seq numbers, modeling `arch/arm64/gic.c::gic_send_ipi` + `kernel/smp.c::ipi_resched_handler`). Three new buggy actions (`BuggySteal`, `BuggyIPI_Deliver`) gated by orthogonal flags (`BUGGY_STEAL`, `BUGGY_IPI_ORDER`). Two new invariants: `NoDoubleEnqueue` (thread in ≤1 runq) and `IPIOrdering` (head of any non-empty queue equals next-expected delivery seq). Three new buggy configs: `scheduler_buggy_steal.cfg` (counterexample at depth 4 / 38 states), `scheduler_buggy_ipi.cfg` (depth 6 / 470 states). Existing `scheduler_buggy.cfg` updated with new constants; same NoMissedWakeup behavior (245 states / depth 6). New constant `MaxIPIs` bounds state space. `specs/SPEC-TO-CODE.md` updated mapping each new action + invariant to its impl site. `docs/reference/15-scheduler.md` Spec cross-reference + Build sections refreshed. Closes the spec-first deviation introduced at P2-Ce/Cf — SMP discipline is now pinned in TLC-checkable form. ARCH §28 I-18 (FIFO IPI ordering) and §8.4 (work-stealing safety) both formally specified. Kernel impl untouched. | 22/22 tests; ~224 ms boot (no impl change); UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; `scheduler.cfg` TLC-clean (10188 distinct states / depth 16); 3 buggy configs each produce their counterexample. |
| `df78693` | **P2-A**: process-model bootstrap. struct Proc + struct Thread + struct Context + cpu_switch_context + thread_trampoline + thread_switch + current_thread (via TPIDR_EL1). proc_init (kproc PID 0) + thread_init (kthread TID 0). SLUB caches. specs/scheduler.tla sketch (TLC-clean, 99 states). 2 new tests (context.create_destroy + context.round_trip). Banner: kproc + kthread lines. Phase tag bumped to P2-A. Collateral bug fixes: alloc_smoke + leak_10k drain magazines pre-baseline; verify-kaslr.sh reads full log. Reference doc 14-process-model.md. | 11/11 tests; ~42 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; spec TLC-clean. |
| `a212038` | **P2-Ba**: scheduler dispatch. New `kernel/include/thylacine/sched.h` + `kernel/sched.c` (~150 LOC) + `kernel/test/test_sched.c`. struct Thread extended (168→200 bytes; `_Static_assert` updated) with `vd_t` (s64) + `weight` (u32) + `band` (u32) + `runnable_next`/`runnable_prev`. Plan 9 idiom: `sched()` (yield) + `ready(t)` (mark runnable + insert into per-band run tree). 3 priority bands (INTERACTIVE/NORMAL/IDLE). Run tree: per-band sorted-by-vd_t doubly-linked list (head = min vd_t). Pick: highest non-empty band; min vd_t. On yield: prev's vd_t advances past tree max via `g_vd_counter++`; prev inserts at back of band; next removes from band; switch. `thread_free` calls `sched_remove_if_runnable` before unlink. `main.c` calls `sched_init` after `thread_init`. Banner adds `sched:` line. Phase tag bumped to P2-Ba. New reference doc `docs/reference/15-scheduler.md`. **Simplified EEVDF at P2-Ba**: monotonic vd_t advance gives FIFO-equivalent rotation; full math (weight + slice + latency bound) lands at P2-Bc. Spec EEVDF refinement deferred to P2-Bc. | 13/13 tests; ~42 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (still 283 states — spec untouched at P2-Ba). New tests: `scheduler.dispatch_smoke` (3-thread round-robin via `sched()`), `scheduler.runnable_count` (counter sanity). |
| `bbd7cbf` | **P2-A R4 audit close**: 1 P1 + 3 P2 + 5 P3 fixed; 1 P3 deferred-with-rationale. **F40 (P1)**: KP_ZERO on all proc/thread kmem_cache_alloc (defends future struct growth against UMR). **F41 (P2)**: BTI BTYPE comments corrected per ARM ARM D24.2.1 (BL→00, BR→01, BLR→10) in context.S + start.S + reference doc. **F42 (P2)**: u64 magic field at offset 0 of struct Proc (PROC_MAGIC) and struct Thread (THREAD_MAGIC); SLUB freelist write naturally clobbers magic on free, so subsequent free reads clobbered value and extincts with explicit double-free diagnostic. **F43 (P2)**: THREAD_STATE_INVALID checks in thread_switch + thread_free. **F44+F45 (P3)**: msr tpidr_el0/el1, xzr in start.S step 4.5 (closes U-25). **F46 (P3)**: _Static_asserts for THREAD_STATE_INVALID==0, sizeof(struct Proc)==24, sizeof(struct Thread)==168, magic at offset 0. **F47 (P3)**: closed-universe modeling assumption documented in scheduler.tla preamble. **F48 (P3)** deferred to U-30 (kstack zeroing on free). Plus **P2-B-1 (not a finding)**: scheduler.tla extended with wait/wake atomicity (cond + waiters + WaitOnCond + BuggyCheck/BuggySleep + WakeAll + NoMissedWakeup); scheduler_buggy.cfg produces missed-wakeup counterexample. struct Proc grew 16→24 bytes; struct Thread grew 160→168 bytes. | 11/11 tests; ~39 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; scheduler.tla TLC-clean (283 states); scheduler_buggy.cfg counterexample at depth 4. |
| `4e108a8` | **P2-Bb**: Plan 9 wait/wake. New `kernel/include/thylacine/rendez.h` + sleep/wakeup impl in `kernel/sched.c` (~110 LOC). struct Thread extended (200→208 B; new field `rendez_blocked_on`). `sched()` refactored to respect pre-set `prev->state`: RUNNING → yield (RUNNABLE+insert); SLEEPING/EXITING → don't insert (caller already transitioned). Defensive `prev==next` re-insert+return guard for the SMP race window between sleep's `spin_unlock(&r->lock)` and `sched()` entry (P2-C closes properly via finish_task_switch pattern). spin_lock_irqsave on `r->lock` for IRQ-mask discipline at UP — P2-Bc's timer-IRQ scheduler tick exercises the path. Single-waiter Rendez (Plan 9 convention); structurally a special case of `scheduler.tla`'s multi-waiter spec. New tests: rendez.{sleep_immediate_cond_true, basic_handoff, wakeup_no_waiter}. New reference doc `docs/reference/16-rendez.md`. Phase tag bumped P2-Ba → P2-Bb. | 16/16 tests; ~42 ms boot; UBSan-clean (~70 ms); fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (283 states — spec unchanged at P2-Bb; impl maps to existing actions); `scheduler_buggy.cfg` counterexample at depth 4. |
| `a9d1a82` | **P2-Cb**: per-CPU init at high VA. PAC keys refactored — `pac_derive_keys` + `pac_apply_this_cpu` asm functions in start.S; `g_pac_keys[8]` BSS holds shared keys derived once on primary; each CPU calls `pac_apply_this_cpu` to load (cross-CPU PAC consistency required for thread migration). `g_secondary_boot_stacks[7][16384]` = 112 KiB BSS for per-secondary 16 KiB boot stacks. Extended `secondary_entry` trampoline: validate idx + set SP from per-CPU stack + `bl pac_apply_this_cpu` + `bl mmu_program_this_cpu` + long-branch via `kaslr_high_va_addr` to high VA `per_cpu_main(idx)`. New `per_cpu_main` C function: sets VBAR_EL1 to `_exception_vectors`, TPIDR_EL1 to NULL (per-CPU current_thread at P2-Cd or later), flips `g_cpu_alive[idx]` (the "fully initialized at high VA" signal), enters idle WFI loop. `smp_init` wait loop now watches `g_cpu_alive` instead of `g_cpu_online` — stricter signal catches PAC/MMU/VBAR/TPIDR failures that would leave a secondary stuck mid-init. `g_cpu_online` still set by trampoline as diagnostic. Test extended to verify both flags. | 18/18 tests; ~112 ms boot (production); UBSan-clean (~143 ms); fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (283 states — untouched at P2-Cb). |
| `0ffe554` | **P2-Ca**: SMP secondary bring-up via PSCI. New DTB cpu/psci enumeration (`dtb_cpu_count`/`dtb_cpu_mpidr`/`dtb_psci_method` + `DTB_MAX_CPUS = 8`); new `arch/arm64/psci.{h,c}` (PSCI HVC/SMC calling, `psci_cpu_on` for PSCI_CPU_ON_64); new `secondary_entry` asm trampoline in `arch/arm64/start.S` (validates idx, flips `g_cpu_online[idx]`, dsb sy, WFI loop); new `kernel/smp.{h,c}` with `smp_init` (iterates DTB cpus, brings each up via PSCI pointing at trampoline PA = `kaslr_kernel_pa_start() + (secondary_entry - _kernel_start)`, waits for online flag with 100-tick timeout); `mmu_enable` refactored into `mmu_program_this_cpu` + `mmu_enable` (build+program) for future per-CPU MMU re-program at P2-Cb. New banner line: `smp:  N/N cpus online (boot + N-1 secondaries via PSCI HVC)`. New test `smp.bringup_smoke` verifies all online flags after smp_init. New reference doc `docs/reference/17-smp-bringup.md`. **Minimal trampoline**: secondaries have NO MMU, NO PAC, NO exception vectors, NO per-CPU stack — they only flip the flag + WFI. P2-Cb adds full per-CPU init bringing them into scheduling. | 18/18 tests; ~112 ms boot (production); UBSan-clean (~145 ms); fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (283 states — spec untouched at P2-Ca; spec work begins at P2-Cd for IPI ordering). |
| `6cdfc8a` | **P2-Cf**: on_cpu flag + SMP wait/wake race close. New `volatile bool on_cpu` field in struct Thread (size 216 → 224 B; `_Static_assert` updated). Set true in sched()/thread_switch when picking next; cleared false in resume path on the destination CPU AFTER cpu_switch_context completed (via per-CPU `cs->prev_to_clear_on_cpu` handoff, mirroring the pending_release_lock pattern). New public helpers `sched_arm_clear_on_cpu(prev)` + `sched_finish_task_switch()` so non-sched callers (thread_switch) can participate. Thread_init/thread_create/thread_init_per_cpu_idle initialize on_cpu deterministically (kthread + per-CPU idles start true; thread_create starts false). wakeup() now spins on `__atomic_load_n(&t->on_cpu)` before transitioning the waiter to RUNNABLE, closing the P2-Bb trip-hazard #16 race: a peer's wakeup can't insert a still-running thread into a runqueue mid-cpu_switch_context-save (would let another peer pick + run a half-saved ctx). Pattern matches Linux's `task->on_cpu`. | 22/22 tests; ~224 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; scheduler.tla TLC-clean (283 states; spec untouched — atomic on_cpu transitions add cross-CPU memory ordering, P2-Cg lifts to spec-level `Steal` action + IPI ordering invariant I-18). |
| `fa6ded0` | **P2-Ce**: work-stealing + finish_task_switch. Real spin_lock primitives in `<thylacine/spinlock.h>` (was UP-no-op stub) using `__atomic_exchange_n` with acquire/release. spin_trylock for cross-CPU access. spin_lock_irqsave threads NULL through (existing API contract). New `pending_release_lock` field in `struct CpuSched` — set by prev's sched right before cpu_switch_context, read+cleared by next's resume path (in C sched()) OR by `sched_finish_task_switch` (called from thread_trampoline asm for fresh threads). Handles thread migration: after cpu_switch_context the destination CPU's pending_release_lock is the lock to release (set by whoever switched-to-us on the destination CPU). New `try_steal(cs)` scans peers via spin_trylock, takes one runnable thread from highest-band non-empty tree, rebases its vd_t into the calling CPU's clock. Called in sched() when pick_next returns NULL, before falling back to "yield with no peer." sched_remove_if_runnable now lock-aware (full spin_lock per CPU). NULL-guard in sched_finish_task_switch lets thread_switch (legacy direct-switch) coexist. New test smp.work_stealing_smoke: boot creates N test threads, ready()s them on its tree, sends IPI_RESCHED to each secondary; secondaries' WFI wakes, sched() pulls work via try_steal, threads run on secondaries (verified via per-CPU counter). Test threads sleep on per-thread Rendezes after exit signal so thread_free can safely reap them (SLEEPING state). | 22/22 tests; ~195 ms boot (production — added 50+20 tick busy-waits in the smoke test); UBSan-clean (~250 ms with finish_task_switch instrumentation overhead); fault matrix 3/3; KASLR 5/5 distinct; scheduler.tla TLC-clean (283 states — spec untouched at P2-Ce; cross-CPU Steal action lands at P2-Cg). |
| `9b8ecd0` | **P2-Cdc**: GIC SGI infrastructure + IPI_RESCHED. New `gic_init_secondary(cpu_idx)` brings up each secondary's GIC redistributor + CPU interface (refactored `redist_init_cpu0` → `redist_init_cpu(idx)` with per-CPU base = `g_redist_base + idx * 0x20000`). New `gic_send_ipi(target, sgi_intid)` writes ICC_SGI1R_EL1 with TargetList encoding (1 << target_cpu_idx, Aff{1,2,3}=0 for QEMU virt). New `IPI_RESCHED = SGI 0` + `g_ipi_resched_count[DTB_MAX_CPUS]` BSS. New `ipi_resched_handler` increments per-CPU receive counter. New `smp_cpu_ipi_init(cpu_idx)` called from per_cpu_main: gic_init_secondary + gic_attach(IPI_RESCHED) + gic_enable_irq(IPI_RESCHED) + msr daifclr #2. per_cpu_main idle loop now `for(;;){sched();wfi;}` — IPI_RESCHED wakes WFI. **Bug fix**: gic_enable_irq for SGI/PPI was hardcoded to `g_redist_base` (CPU 0's frame); refactored to use `cpu_redist_base(smp_cpu_idx_self())` so secondaries enable their own banked SGIs. New test `smp.ipi_resched_smoke` (boot CPU sends IPI_RESCHED to each secondary; verifies counter increments). | 21/21 tests; ~104 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; scheduler.tla TLC-clean (283 states — spec untouched: cross-CPU IPI ordering invariant I-18 lands at P2-Cg with the SMP refinement). |
| `a604cd7` | **P2-Cd (Cda+Cdb without Cdc)**: per-CPU run trees + per-CPU idle threads. struct CpuSched (run_tree[BANDS], vd_counter, initialized, idle); g_cpu_sched[DTB_MAX_CPUS] BSS. sched(), ready(), sched_tick(), preempt_check_irq() refactored to index by smp_cpu_idx_self(). g_need_resched is now a per-CPU array (each CPU's IRQ writes its own slot). sched_init takes cpu_idx; called from main.c (boot) and per_cpu_main (secondaries). New `thread_init_per_cpu_idle(cpu_idx)` in thread.{h,c} allocates a Thread descriptor for a secondary CPU's idle (uses the per-CPU boot stack from secondary_entry; no kstack of its own). per_cpu_main on secondaries: install VBAR_EL1 → allocate idle thread → set TPIDR_EL1 → sched_init → set g_cpu_alive → enter idle WFI loop. `sched_idle_thread(cpu_idx)` accessor exposes per-CPU idle pointer. New test smp.per_cpu_idle_smoke verifies each CPU's idle is registered (CPU 0 = kthread; secondaries = fresh thread in IDLE band). Phase tag P2-Cc → P2-Cd. **GIC SGI infrastructure + IPIs deferred to P2-Cdc** — secondaries' WFI loop is currently a pure park (no IRQs reach them yet); idle infrastructure is in place to wake on IPI_RESCHED once Cdc lands. Boot-time regression (1078 ms) traced to a tight sched()+WFI loop on secondaries (WFI returns on architectural events even with masked IRQs); reverted to pure WFI to keep boot at ~100 ms — the loop body comes back at P2-Cdc when IPIs make it productive. | 20/20 tests; ~100 ms boot (production); UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (283 states — spec untouched: per-CPU run trees are infrastructure with no kernel concurrency invariant impact at this sub-chunk; cross-CPU spec work begins at P2-Cg). |
| `8eb4aee` | **P2-Cc**: per-CPU exception stacks via SPSel=0. New `g_exception_stacks[DTB_MAX_CPUS][4096]` BSS (32 KiB total) in `kernel/smp.c`; declared in `<thylacine/smp.h>` with `EXCEPTION_STACK_SIZE` macro + `smp_cpu_idx_self()` accessor. start.S `_real_start` step 4.6 splits boot SP into SP_EL0 = boot stack + SP_EL1 = exception stack via the SPSel-dance (msr sp_el0 then mov sp while SPSel=1, then msr SPSel #0). Step 8.5 re-anchors SP_EL1 to HIGH VA after MMU enable. `secondary_entry` mirrors both: SPSel-dance for SP_EL0/SP_EL1 at LOW PA, then re-anchor SP_EL1 to HIGH VA after `mmu_program_this_cpu`. vectors.S keeps both Current-EL/SP_EL0 (0x000 + 0x080) AND Current-EL/SP_ELx (0x200 + 0x280) Sync+IRQ slots LIVE — kernel transits through SPSel=1 transiently after sched-from-IRQ + thread_trampoline unmask path; both groups dispatch to the same handlers. timer_irq_handler instrumented to capture `&local` once per CPU into `g_exception_stack_observed[]` for runtime verification. New test `smp.exception_stack_smoke` verifies SPSel=0 in normal kernel mode + g_exception_stacks layout + observed IRQ-entry SP falls in g_exception_stacks[0] range. New banner line `exception: per-CPU SP_EL1 (4096 B/CPU; SPSel=0 kernel mode)`. Closes vectors.S P1-F "KNOWN LIMITATION" — kernel stack overflow no longer recursively faults inside KERNEL_ENTRY (SP_EL1 = exception stack with full headroom regardless of SP_EL0 state). Phase tag P2-Cb → P2-Cc. | 19/19 tests; ~112 ms boot (production); ~143 ms (UBSan); fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (283 states — spec untouched at P2-Cc; per-CPU exception stacks are a hardware/cooperation concern with no concurrency invariant impact). |
| `518d294` | **P2-Bc**: scheduler-tick preemption + IRQ-mask discipline. New `sched_tick()` + `preempt_check_irq()` in `kernel/sched.c` (~50 LOC). struct Thread extended (208→216 B; new field `slice_remaining` s64). `THREAD_DEFAULT_SLICE_TICKS = 6` (Linux EEVDF default at 1 kHz). Timer IRQ handler decrements current's slice; on expiry (≤ 0), sets `g_need_resched` and replenishes. `vectors.S` IRQ slot inserts `bl preempt_check_irq` after `exception_irq_curr_el` (5 instructions of slot used; 28-instruction total well under 32 budget). `preempt_check_irq` calls `sched()` if need_resched + scheduler initialized + current valid. `sched()` and `ready()` bracket run-tree mutations with `spin_lock_irqsave(NULL)` for IRQ-mask discipline (closes P2-A trip-hazard #3 fully). `thread_trampoline` adds `msr daifclr, #2` at first entry — fresh threads need IRQ unmasked once running so the timer-IRQ preempt path can fire (subsequent re-entries inherit DAIF via sched()'s irqrestore). New test: `scheduler.preemption_smoke` (two CPU-bound busy-loop threads + boot share the CPU under timer-IRQ-driven preemption; both counters > 0 + within 10× fairness; without preemption this would deadlock). Spec mapping: timer-IRQ-driven preempt path maps to existing `Yield(cpu)` action (non-deterministic in TLC; observably indistinguishable from cooperative yield); LatencyBound liveness deferred to Phase 2 close refinement. Phase tag bumped P2-Bb → P2-Bc. | 17/17 tests; ~120 ms boot (45 ticks of preempt_smoke wait + ~75 ms of init/tests/banner); UBSan-clean (~134 ms); fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (283 states — spec docs updated, no model changes); `scheduler_buggy.cfg` counterexample at depth 4. |

## Remaining work

Sub-chunk plan (refined as Phase 2 progresses):

1. ✅ **P2-A: process-model bootstrap.** Landed. struct Proc + struct Thread + cpu_switch_context + thread_switch + scheduler.tla sketch.
2. ✅ **P2-Ba: scheduler dispatch.** Landed. sched() + ready() + 3-band run tree.
3. ✅ **P2-Bb: wait/wake.** Landed. Rendez + sleep + wakeup. scheduler.tla WaitOnCond / WakeAll mapping (spec was extended at P2-A R4; impl lands here).
4. ✅ **P2-Bc: scheduler-tick preemption + IRQ-mask discipline.** Landed. Per-thread slice + sched_tick + preempt_check_irq (vectors.S wired). Full EEVDF math (vd_t = ve_t + slice × W_total / w_self with weighted virtual time advance) deferred — meaningful only with weight ≠ 1 which is Phase 5+. LatencyBound (I-17) liveness spec deferred to Phase 2 close refinement.
5. ✅ **P2-Ca: SMP secondary bring-up via PSCI.** Landed. DTB cpu/psci enumeration + PSCI HVC/SMC primitives + minimal asm trampoline + smp_init. Secondaries flip per-CPU online flag + park at WFI; no MMU/PAC/vectors yet. P2-Cb..Cg add the rest of SMP (per-CPU init, IPIs, work-stealing, finish_task_switch).
6. ✅ **P2-Cb: per-CPU init at high VA.** Landed. PAC keys refactored (derive once, apply per CPU). Per-CPU boot stacks. Extended trampoline: SP + PAC + MMU + long-branch to per_cpu_main. per_cpu_main sets VBAR + TPIDR + alive flag + idle WFI. Secondaries now reach C runtime at high VA — ready for per-CPU run trees + SGIs at P2-Cd.
7. ✅ **P2-Cc: per-CPU exception stacks via SPSel=0**. Landed. Hardware-driven SP_EL0/SP_EL1 split; per-CPU exception stack BSS; SPSel-dance setup in start.S; both vector groups live; observability hook + smoke test.
8. ✅ **P2-Cd: per-CPU run trees + idle threads + GIC SGI + IPI_RESCHED**. Landed in two parts: P2-Cd (Cda+Cdb) for per-CPU CpuSched + idle Thread descriptors, then P2-Cdc for GIC SGI infrastructure + IPI_RESCHED. Secondaries now receive cross-CPU IPIs and wake from WFI. `IPI_TLB_FLUSH`, `IPI_HALT`, `IPI_GENERIC` per ARCH §20.4 deferred — not needed at v1.0 P2-Cd; lands when use-cases arrive (e.g., TLB shootdown for namespace rebind in Phase 5+).
9. ✅ **P2-Ce: work-stealing + finish_task_switch**. Landed. Real spinlocks (atomic exchange-acquire/release) + per-CPU run-tree lock + try_steal in sched()'s no-peer path + finish_task_switch handoff via cs->pending_release_lock for cross-CPU resume after migration.
10. ✅ **P2-Cf: on_cpu flag + SMP wait/wake race close**. Landed. `volatile bool on_cpu` per Thread; transitions across cpu_switch_context via per-CPU prev_to_clear_on_cpu handoff. wakeup() spins on waiter's on_cpu before state transition, closing the P2-Bb trip-hazard #16 race.
11. ✅ **P2-Cg: scheduler.tla SMP refinement**. Landed. Per-CPU runqueue model already present from P2-A; added cross-CPU `Steal(stealer, victim)` action + per-(src,dst) FIFO IPI queues with `IPI_Send(src, dst)` / `IPI_Deliver(src, dst)` actions + `NoDoubleEnqueue` invariant + `IPIOrdering` invariant (ARCH §28 I-18). Three buggy configs each produce their counterexample. Closes the spec-first deviation introduced at P2-Ce/Cf.
12. ✅ **P2-Da: rfork(RFPROC) + exits + wait**. Landed. `rfork(flags, entry, arg)` only RFPROC (other flags extinct loudly until their phase lands — RFNAMEG at P2-E, RFFDG at P2-F, RFMEM at P2-G). exits(msg) marks Proc ZOMBIE + wakes parent's child_done Rendez. wait_pid(*out) reaps zombie children. Single-thread Procs (multi-thread coordination at Phase 5+). Orphans re-parent to kproc (PID 1 / init at Phase 5+). 3 new tests: rfork_basic_smoke, rfork_exits_status, rfork_stress_100.
13. ✅ **P2-Db: 1000-iter rfork+exits+wait stress**. Landed. Stress validates lifecycle integrity at 10× scale; batched 8-rfork→8-reap pattern stresses the run tree depth + wait_pid Rendez under sustained load. Per-CPU run counters track placement; sum-must-equal-ITERS verifies no double-reap. Cross-CPU placement assertion deferred (needs IPI_RESCHED on rfork OR per-CPU timer init). Per-thread guard pages deferred to P2-Dc.
14. **P2-Dc: per-thread guard pages**. Closes P2-A trip-hazard #1 (kstack overflow detection). Allocate kstack as 8 pages (32 KiB), use upper 4 (16 KiB) as the actual stack, mark lower 4 as no-access in TTBR1. Stack overflow → page fault → extinction. Requires L2-block → L3-table demotion in `arch/arm64/mmu.c` because kstacks come from 2 MiB-block-mapped RAM and per-page no-access needs 4 KiB granularity. New deliberate-fault test variant `kstack_overflow` for `tools/test-fault.sh`.
15. **P2-Dd: cross-CPU work distribution (IPI_RESCHED on rfork OR per-CPU timer init)**. Either approach lets rfork'd children actually run on secondaries. IPI_RESCHED on rfork is the lighter-weight option (sends one SGI when ready() puts a thread in the local tree, prompting peers to wake-and-steal). Per-CPU timer init is heavier (configures the ARM generic timer on each secondary so they wake periodically) but more uniform. Audit-bearing change to scheduler / rfork. New cross-CPU stress test once it works.
16. **P2-E: namespace + bind/mount**. `kernel/namespace.c` — `Pgrp`, `bind`, `mount` (stub: ramfs only at this phase), `unmount`. Namespace cloned on rfork(RFPROC); shared on rfork(RFPROC | RFNAMEG). New spec `namespace.tla`: cycle-freedom (I-3), isolation (I-1).
17. **P2-F: handle table + VMO**. `kernel/handle.c` — typed kernel object handles per ARCH §18 (Process / Thread / VMO / MMIO / IRQ / DMA / Chan / Interrupt); rights monotonicity (I-2, I-6); transfer-via-9P invariant (I-4) — placeholder pending Phase 4's 9P client; hardware-handle non-transferability (I-5). `kernel/vmo.c` — VMO manager per ARCH §19; refcount + mapping lifecycle (I-7). New spec `handles.tla` (mandatory; Phase 2 close).
18. **P2-G: ELF loader + minimal init**. `arch/arm64/elf.c` — ELF loader; reject RWX segments per W^X invariant. `init/init-minimal.c` — first userspace process; UART debug shell.
19. **P2-H: Phase 2 closing audit + spec finalization**. Round-4 audit on the entire Phase 2 surface; spec mappings in `specs/SPEC-TO-CODE.md` populated and CI-checked. ROADMAP §5.2 exit criteria all met.

## Exit criteria status

(Copy from `ROADMAP.md §5.2`; tick as deliverables complete.)

- [ ] Two processes run concurrently on a single CPU; timer preemption works. (P2-B)
- [ ] Four processes run concurrently on 4 vCPUs; EEVDF latency bound holds. (P2-C + P2-B)
- [ ] `rfork(RFPROC)` + `exits()` + `wait()` lifecycle works without leak (1000-iteration stress test). (P2-D)
- [ ] `exec()` loads and runs a static ELF from ramfs; rejects RWX segments. (P2-G)
- [ ] init starts a UART shell; `echo hello` works via pipe. (P2-G)
- [ ] Page fault handler allocates demand pages; stack growth works. (P2-D / P2-F + Phase 3 dependency)
- [ ] Handle table: 10,000 handles open/close cycle without leak; rights reduction enforced; hardware-handle transfer attempt panics cleanly. (P2-F)
- [ ] VMO: create, map, write, read, unmap, close cycle correct; pages freed on last-handle-close + last-mapping-unmap. (P2-F)
- [ ] Stress: 1000 `rfork`/`exits`/`wait` cycles across 4 CPUs without leak or panic. (P2-D + P2-C)
- [ ] Wakeup atomicity: 1000 producer/consumer pairs across 4 CPUs in tight loop; no missed wakeups (verified by counter). (P2-B + P2-C)
- [ ] Work-stealing: 4-CPU test with imbalanced load; load redistributes within 5ms. (P2-C)
- [ ] **TSan clean** on the SMP test suite. (P2-C)
- [ ] `specs/scheduler.tla`, `specs/namespace.tla`, `specs/handles.tla` clean under TLC. (P2-A landed scheduler sketch; refinement at P2-B + P2-C; namespace + handles at P2-E + P2-F)
- [ ] `SPEC-TO-CODE.md` for all three specs maintained. (Stubbed at P2-A; populated through Phase 2)
- [ ] No P0/P1 audit findings. (Closing audit at P2-H)

## Build + verify commands

```bash
# Build the kernel ELF (build/kernel/thylacine.elf, ~227 KB debug)
tools/build.sh kernel
make kernel

# Run interactively
tools/run-vm.sh
make run

# Integration test: 11/11 in-kernel tests + boot banner within 10s
tools/test.sh
make test

# UBSan build (build/kernel-undefined; isolated cache)
tools/build.sh kernel --sanitize=undefined
tools/test.sh --sanitize=undefined

# Deliberate-fault matrix (regression check for Phase 1 hardening)
tools/test-fault.sh

# KASLR variance
tools/verify-kaslr.sh -n 10

# All TLA+ specs (1 spec at P2-A: scheduler.tla)
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && for s in *.tla; do
    echo "== $s =="
    java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
        -config "${s%.tla}.cfg" "$s" 2>&1 | tail -3
done
```

Reference output of a successful boot (banner subset):

```
Thylacine v0.1.0-dev booting...
  arch: arm64
  el-entry: EL1 (direct)
  cpus: 1 (P1-C-extras; SMP at P1-F)
  mem:  2048 MiB at 0x0000000040000000
  ...
  gic:  v3 dist=0x0000000008000000 redist=0x00000000080a0000
  timer: 1000000 kHz freq, 1000 Hz tick (PPI 14 / INTID 30)
  kproc:   pid=0 threads=1
  kthread: tid=0 state=RUNNING (current_thread = kthread)
  tests:
    [test] kaslr.mix64_avalanche ... PASS
    ...
    [test] context.create_destroy ... PASS
    [test] context.round_trip ... PASS
  tests: 11/11 PASS
  ticks: 25 (kernel breathing)
  boot-time: 42.0 ms (target < 500 ms per VISION §4)
  phase: P2-A
Thylacine boot OK
```

## Trip hazards

(Cumulative from Phase 1; new at Phase 2 marked NEW.)

### NEW at P2-A

1. ~~**TPIDR_EL1 reset value is UNKNOWN per ARM ARM**~~. **CLOSED at P2-A R4 (F44)**: start.S step 4.5 now emits `msr tpidr_el1, xzr` AND `msr tpidr_el0, xzr` after BSS clear, making pre-`thread_init` reads of `current_thread()` deterministic-NULL.
2. **No guard page below `thread_create` kstacks**. The boot kthread (TID 0) inherits the boot stack which has a guard from P1-C-extras Part A; threads created via `thread_create` get a 16 KiB stack from `alloc_pages(2, KP_ZERO)` with NO guard page. Stack overflow in a P2-A kthread silently corrupts neighbor memory. **Phase 2 close** lands the per-thread guard page.
3. **No IRQ masking around `thread_switch`**. The only IRQ source live at v1.0 is the timer; its handler increments `g_ticks` and returns — does not touch thread state. Reentrancy is trivially safe at P2-A. **P2-B** adds the IRQ-mask discipline once scheduler-tick preemption becomes possible (timer IRQ → `thread_block` / dispatch).
4. **`thread_switch` unconditionally sets prev's state to `THREAD_RUNNABLE`**. Correct for "yield to next" semantics. For "block on a condition" (sleep), prev must be `THREAD_SLEEPING`. **P2-B** adds the separate `thread_block(cond)` primitive.
5. **`thread_trampoline` halts on entry-return (WFE)**. No `thread_exit` plumbing yet — if a thread's entry returns, the descriptor isn't reclaimed and the kstack remains allocated. Mitigated at P2-A: test entries don't return (call `thread_switch(kt0)` instead). **Phase 2 close** lands real `thread_exit` + reap.
6. **`scheduler.tla` is a sketch — `_buggy.cfg` deferred to P2-B**. The CLAUDE.md "executable documentation" pattern (primary `.cfg` clean + `_buggy.cfg` showing a specific invariant violation) is unfulfilled at P2-A because the relevant bug-classes (missed wakeups, deadline starvation, IPI reordering) are introduced by P2-B's actions. P2-B lands `scheduler_buggy.cfg` showing the missed-wakeup race when wait/wake atomicity is violated.
7. **`struct Context` size pinned at 112 bytes**. Adding a field (e.g., FPSIMD state at Phase 5 for userspace) requires updating `_Static_assert`s in `context.h` AND offsets / stp/ldp/str/ldr immediates in `context.S` together. The build break is loud but multi-step.
8. **`kstack_base` is PA-cast-to-void* at v1.0** (consistent with `kpage_alloc`). Phase 2 introduces the kernel direct map; both `kpage_alloc` and `thread_create` convert to high-VA pointers in the same chunk.
9. **`tools/test.sh` truncates the log to tail-20**. P2-A added kproc + kthread banner lines, pushing the KASLR offset line off the tail. `tools/verify-kaslr.sh` was updated to read `build/test-boot.log` directly instead of test.sh stdout. Future banner growth doesn't break the verifier; but a future debugger looking at test.sh stdout might miss earlier banner lines.
10. **`phys.alloc_smoke` + `phys.leak_10k` now drain magazines BEFORE baseline**. Phase 1's tests didn't need this — no SLUB allocations happened pre-test so the magazine was empty. P2-A's proc_init + thread_init each `kmem_cache_create` + `kmem_cache_alloc`, leaving ~5 pages resident in the order-0 magazine. Without the pre-drain, the post-drain comparison reads HIGHER than baseline by the resident count → false drift. The pre-drain is the durable fix.
11. **U-30 (NEW at P2-A R4 F48)**: `thread_free` returns kstack pages with stale stack-frame contents to buddy. At P2-A there's no userspace + only kernel callers of subsequent allocations + `KP_ZERO` is the documented escape hatch. Phase 3's user-facing allocators must use `KP_ZERO` consistently. Deferred — no immediate fix at P2-A.
12. **`struct Proc` size pinned at 24 bytes; `struct Thread` size pinned at 168 bytes (P2-A R4 F46)**. `_Static_assert`s in proc.h/thread.h. Adding a field breaks the build until the assert is bumped; the bump must be deliberate.
13. **`magic` field at offset 0 of struct Proc + Thread (P2-A R4 F42)**. SLUB's freelist write on `kmem_cache_free` naturally clobbers it; subsequent `proc_free`/`thread_free` reads the clobbered value and extincts with explicit double-free diagnostic. The contract is "magic is at offset 0 SO that the SLUB clobber kills it" — moving the field elsewhere defeats the defense.
14. **BTI BTYPE values correctly stated everywhere (P2-A R4 F41)**. Per ARM ARM D24.2.1: BL → BTYPE=0b00, BR → BTYPE=0b01, BLR → BTYPE=0b10. The previous comments in `context.S` + `start.S` + reference doc had values swapped or wrong. Now consistent.

### NEW at P2-Bb

15. **`sched()` now respects pre-set `prev->state`** (P2-Bb refactor). Caller protocol: set `prev->state = THREAD_SLEEPING` (or `EXITING`) BEFORE calling `sched()` to block; leave `prev->state == THREAD_RUNNING` to yield. `sched()` sees the state and dispatches accordingly (yield-insert vs leave-alone). Any callers that forget to set state will get yield semantics by default — safe but wrong for blockers (the thread becomes runnable again immediately). The `sleep` impl is the canonical example; future block paths (timer-IRQ preemption that puts a thread to sleep on its own slice expiry; postnote stop) follow the same pattern.

16. **SMP wait/wake race window (deferred to P2-C)**. Between `sleep`'s `spin_unlock(&r->lock)` and entering `sched()`, on SMP a peer wakeup can fire on another CPU, transition `current` → RUNNABLE, ready() it (insert into a runqueue). When `current` then enters `sched()`, `pick_next` may pull `current` out of its own runqueue. The current `prev == next` defensive check (re-insert + return-without-switch) is a soft mitigation; the proper fix is the `finish_task_switch` pattern at P2-C — drop the runqueue lock only AFTER the context switch completes. UP at v1.0 P2-Bb: race cannot fire (single CPU + IRQ-mask held throughout sleep).

17. **`sleep` keeps the IRQ mask held across `sched()`**. Per the pattern in 9front and Linux: the IRQ mask saved at sleep entry is restored only after the loop exits. Between `spin_unlock(&r->lock)` and `sched()`, IRQ is masked locally — what makes the wait/wake protocol robust against IRQ-context wakeups at UP. The next thread switched in inherits "live" IRQ state for the duration of its own critical sections (which are themselves balanced via irqsave/irqrestore). P2-Bc's scheduler-tick preemption tests this discipline at scale.

18. **Single-waiter Rendez (Plan 9 convention)**. At most one thread per Rendez at a time; second sleeper extincts. Multi-waiter wait queues land at Phase 5 (poll, futex). Today's kernel use cases (timer expiry, basic producer/consumer) fit the single-waiter pattern naturally; userspace primitives that need many waiters (futexes for pthread cond vars) get a wait-queue generalization.

19. **`struct Thread` size pinned at 208 bytes (P2-Bb)**. P2-Ba's 200 + 8 (rendez_blocked_on). `_Static_assert` updated. Future Thread fields grow this further; deliberate per-bump.

20. **`sched()`'s deadlock detection assumes UP-no-other-CPU**. `sched: deadlock — current is blocking, no runnable peer` extincts when prev is SLEEPING + run tree is empty. At v1.0 P2-Bb single-CPU UP, this is the correct behavior — only an IRQ could wake us, and IRQs are masked. P2-C lands SMP idle-WFI: an idle CPU with no runnable thread parks at WFI, gets woken via IPI / IRQ. The extinction path then narrows to genuine deadlocks (all CPUs sleeping with no waker scheduled).

### NEW at P2-Bc

21. **`thread_trampoline` unmasks IRQs at first entry** (P2-Bc). The very first time a fresh thread runs (after thread_create + ready + sched picks it + cpu_switch_context.ret), it lands at thread_trampoline — NOT inside sched()'s frame. So sched()'s irqrestore (which restores DAIF on resumption) doesn't run for fresh threads. We explicitly `msr daifclr, #2` at the start of thread_trampoline to unmask IRQs. Subsequent re-entries (when a preempted thread is resumed) go through sched()'s frame and inherit irqrestore correctly. Don't add a second irqsave/irqrestore around the trampoline body — it would conflict with sched()'s discipline.

22. **`g_need_resched` is `volatile bool`, not atomic** (P2-Bc). UP single-CPU at v1.0: no race (writer is timer IRQ handler, reader is preempt_check_irq from same CPU's IRQ-return path; both serialized via the IRQ mask). P2-C SMP needs `_Atomic(bool)` with release/acquire ordering for cross-CPU need_resched signals (or a per-CPU need_resched + IPI_RESCHED).

23. **`sched_tick` skips when `t->state != THREAD_RUNNING`** (P2-Bc). Defensive: if current's state somehow became RUNNABLE/SLEEPING between IRQ entry and sched_tick's read, don't decrement. Shouldn't happen at v1.0 UP (the only writers to state are sched()/sleep/wakeup, all with IRQ masked) but the read is cheap.

24. **Slice replenish on RUNNABLE → RUNNING** (P2-Bc). `sched()` sets `next->slice_remaining = THREAD_DEFAULT_SLICE_TICKS` on the pick path. NOT on `ready()`. This is correct: the slice represents "time remaining this quantum"; a thread that hasn't run yet hasn't consumed anything. ALSO replenished when sched() is called with no other runnable peer (the yield-with-no-other-runnable path) — keeps boot's slice topped up so the test path's `timer_busy_wait_ticks` doesn't continually re-fire need_resched.

25. **`struct Thread` size pinned at 216 bytes (P2-Bc)**. P2-Bb's 208 + 8 (slice_remaining s64). `_Static_assert` updated.

26. **`vectors.S` IRQ slot uses 28 of 32 instructions** (P2-Bc). KERNEL_ENTRY (24) + mov + 2×bl + b = 28. Adding a third bl would push past the 32-instruction (0x80-byte) slot budget; further IRQ-return work moves to .Lexception_return or to a dedicated trampoline section (per the existing P1-G pattern for KERNEL_EXIT factoring).

27. **Phase 2 close LatencyBound spec deferred** (P2-Bc). The C impl provides the latency bound (slice × N rotation under preemption — empirically tested in scheduler.preemption_smoke), but the spec-level proof requires weak fairness annotations + an explicit Slice variable. Phase 2 close adds the full liveness refinement.

### NEW at P2-Ca

28. **Secondaries cannot execute kernel code beyond the trampoline at P2-Ca**. They have no MMU, no PAC, no exception vectors, no per-CPU stack, no per-CPU TPIDR_EL1. Any IRQ delivered to a secondary would fault into the (unset) vector table → undefined behavior. Currently all device IRQs are routed to the boot CPU (default GIC routing); P2-Cd's per-CPU IRQ routing closes this. Until P2-Cb lands the full per-CPU init, secondaries are observably alive but inert.

29. **`g_cpu_online` cache coherence assumption** (P2-Ca). The asm trampoline writes the flag with caches off (Device-nGnRnE per ARM ARM B2.7). The boot CPU reads via TTBR1's Normal-WB cacheable mapping — `volatile + dmb ish` is sufficient on QEMU virt. Bare-metal hardware where coherency might not include CPUs in pre-MMU state may need an explicit `dc ivac` on the boot side. Phase 7 hardening (Pi 5 bring-up) revisits.

30. **Secondary entry PA computation depends on `_kernel_start` matching the linker script's BASE** (P2-Ca). `kaslr_kernel_pa_start() + (secondary_entry - _kernel_start)` is the formula. If the linker script changes the kernel image's base symbol (currently `_kernel_start`), this needs to be updated alongside.

31. **PSCI conduit cached at psci_init time** (P2-Ca). Don't change conduits at runtime — the SMCCC dispatch in `arch/arm64/psci.c::smccc_call` reads `g_conduit` once per call but the binary contains both HVC and SMC paths. A runtime conduit change is unsupported; reboot to switch.

32. **`DTB_MAX_CPUS = 8` is hard-coded throughout** (P2-Ca). The DTB walker's stack arrays, asm trampoline's bounds check (`mov x9, #8` in start.S `secondary_entry`), `g_cpu_online` array, all depend on this. Bumping requires updating each site. v1.0 cap per ARCH §20.7.

33. **`mmu_enable` split into `mmu_program_this_cpu` + `mmu_enable`** (P2-Ca). Primary still calls `mmu_enable` which builds tables and programs MMU. Secondaries (at P2-Cb) will call `mmu_program_this_cpu` directly to re-program their MMU using the primary's already-built tables. The split is preparation; no behavior change at P2-Ca.

### NEW at P2-Cf

71. **`struct Thread.on_cpu` field** (P2-Cf). volatile bool, true while a thread is actively executing on some CPU. Set by sched()/thread_switch when picking next; cleared by the destination CPU's resume path (sched()'s post-cpu_switch_context block, OR sched_finish_task_switch from thread_trampoline). Read with __atomic_load_n acquire by wakeup() spin loops. Linux's task->on_cpu equivalent.

72. **`cs->prev_to_clear_on_cpu` handoff** (P2-Cf). Mirrors cs->pending_release_lock from P2-Ce. Set by prev's sched/thread_switch right before cpu_switch_context to point at prev (the thread being switched away from on this CPU). Read+cleared by the destination CPU's resume path: `__atomic_store_n(&prev_to_clear->on_cpu, false, __ATOMIC_RELEASE)` — this is the synchronization point that lets a peer wakeup proceed.

73. **wakeup() spins on waiter's on_cpu** (P2-Cf). Before transitioning waiter from SLEEPING to RUNNABLE + ready(), wakeup spin-waits for `t->on_cpu == false`. Without this, a peer can be mid-cpu_switch_context-save when wakeup races, transitions state, ready() inserts into another CPU's tree, that CPU picks t and loads ctx — but ctx isn't fully saved yet. on_cpu serializes with cpu_switch_context completion.

74. **`sched_arm_clear_on_cpu(prev)` + `sched_finish_task_switch()` exposed** (P2-Cf). Non-sched-internal callers (thread_switch) need the same on_cpu handoff as sched() proper. Two helpers expose the per-CPU CpuSched manipulation: arm sets prev_to_clear; finish_task_switch consumes it (and pending_release_lock) on resume.

75. **on_cpu init in thread_init / thread_create / thread_init_per_cpu_idle** (P2-Cf). kthread starts on_cpu=true (it's the running boot thread). Per-CPU idles start on_cpu=true (they'll be set as TPIDR_EL1 on their CPU before sched_init records them as the CPU's idle, and they remain "running" until first sched-out). thread_create starts on_cpu=false (not running until picked).

76. **struct Thread size pinned at 224 bytes** (P2-Cf). P2-Bc baseline 216 + on_cpu (1 byte + tail-padding to 8). _Static_assert updated.

77. **Same-thread wakeup deadlock corner case** (P2-Cf). If a thread T calls wakeup(R) while R->waiter == T (thread waking itself), wakeup spins on t->on_cpu forever — t IS the wakeup caller and remains on_cpu=true. This is a misuse of the API (a thread can't wake itself when it's the waiter); we extinct on the state validation later (state should be SLEEPING but caller can mutate). Not a v1.0 concern; document.

### NEW at P2-Ce

64. **Real spinlocks via `__atomic_exchange_n`** (P2-Ce). spin_lock_t.value is now used (was `_stub` no-op). `spin_lock` is test-and-set with acquire ordering + spin-on-load fast path. `spin_unlock` is a release store. `spin_trylock` is acquire-exchange returning the previous value. Compiles to ARMv8.1 LSE (`swpa`) under `-mcpu=cortex-a72+lse` or LL/SC (`ldaxr`/`stxr`) on ARMv8.0. spin_lock_irqsave/irqrestore now thread the NULL lock pointer through gracefully — passing NULL skips the spin part (still does IRQ mask/restore), preserving the prior API contract for callers that use it as "IRQ mask only."

65. **Per-CPU run-tree lock** (P2-Ce). `struct CpuSched.lock` protects the band-indexed run tree against cross-CPU work-stealing. Held with IRQs masked across the entire sched() body (including cpu_switch_context); local re-entry via timer IRQ is gated by the IRQ mask, cross-CPU access via try_steal is gated by the lock. ready() takes the lock too. sched_remove_if_runnable takes each CPU's lock as it scans (full spin_lock — thread_free expects unconditional cleanup).

66. **`cs->pending_release_lock` finish_task_switch handoff** (P2-Ce). Set by prev's sched right before cpu_switch_context: "the lock the resuming thread should release." Read by next's resume path: a thread that was migrated (stolen) reads the destination CPU's pending_release_lock — set by whoever switched-to-us on this CPU. Cleared to NULL after consumption (hygiene). Pattern mirrors Linux's finish_task_switch.

67. **`sched_finish_task_switch` for thread_trampoline** (P2-Ce). thread_trampoline (asm) calls this C helper before unmasking IRQs and calling the entry function. NULL-guard handles the legacy thread_switch path (P2-A direct-switch primitive used by context tests) which doesn't acquire the per-CPU run-tree lock; sched_finish_task_switch sees pending_release_lock=NULL and skips the unlock.

68. **`try_steal` scans peers in CPU order** (P2-Ce). For each peer with `initialized` true: spin_trylock; if acquired, pick the highest-priority runnable thread from peer's tree, unlink it, drop peer's lock, return. Stolen thread's vd_t is rebased to the calling CPU's `vd_counter++` so it sorts at the back of the local rotation. The CPU-order scan is a v1.0 simplification — production should rotate the start CPU per call to spread load (P2-Cg refinement).

69. **`prev == next` after steal handles cross-CPU race** (P2-Ce). With work-stealing, prev's resume path may find pick_next gave NULL but try_steal pulled prev itself back from a peer — only possible under a future cross-CPU-placer (e.g., a wakeup that places prev on a different CPU's tree, then we steal back). The `prev == next` re-insert + return path covers this without context-switching to self.

70. **Boot-time regression to ~195 ms in production** (P2-Ce). The smp.work_stealing_smoke test deliberately busy-waits 50+20 ticks to give secondaries time to steal + drain. UBSan adds further overhead (~250 ms). Both still under the 500 ms budget per VISION §4. Future tuning: shorten the wait windows once steal latency is well-characterized.

### NEW at P2-Cdc

58. **`gic_enable_irq` for SGI/PPI now uses the calling CPU's redistributor** (P2-Cdc). Previously hardcoded to `g_redist_base` (CPU 0's frame). After P2-Cdc, callers from secondary CPUs get their own banked GICR_ISENABLER0 — necessary for IPI_RESCHED to actually be enabled on the target. SPI path unchanged (uses distributor's GICD_ISENABLERn). The fix does NOT affect existing callers from CPU 0 (gic_init's timer-PPI enable in main.c) because cpu_redist_base(0) == g_redist_base.

59. **`redist_init_cpu(cpu_idx)` walks per-CPU frame at offset `cpu_idx * 0x20000`** (P2-Cdc). QEMU virt's redistributor region is contiguous; CPU N's frame base is `g_redist_base + N * 0x20000`. ARM IHI 0069 §12.3.1 requires this; the entire region is mmu_map_device'd at gic_init time so all per-CPU frames are accessible without re-mapping.

60. **ICC_SGI1R_EL1 encoding pinned at v1.0** (P2-Cdc). Per ARM ARM C5.2.18: INTID at bits [27:24], TargetList at bits [15:0] (bitmap within Aff{1,2,3} cluster). On QEMU virt all CPUs share Aff{1,2,3}=0 with Aff0=cpu_idx, so `(sgi_intid << 24) | (1 << target_cpu_idx)` is correct. Pi 5 / multi-cluster hardware needs Aff1/2/3 fields populated — Phase 7 hardening pass.

61. **IPI_RESCHED = SGI 0** (P2-Cdc). Reserved INTID for cross-CPU resched signal. Future IPIs (TLB_FLUSH, HALT, GENERIC per ARCH §20.4) get later SGI INTIDs.

62. **per_cpu_main idle loop is `for(;;){sched();wfi;}`** (P2-Cdc). Previously pure WFI park (P2-Cd before Cdc). With IRQs unmasked + IPI infrastructure live, sched() runs on each WFI wake to process anything placed on this CPU's tree. P2-Ce work-stealing introduces the actual cross-CPU placement; until then the IPI_RESCHED handler just increments g_ipi_resched_count for observability.

63. **`smp_cpu_ipi_init` order matters** (P2-Cdc). per_cpu_main calls it BEFORE flipping g_cpu_alive[cpu_idx] — so by the time the boot CPU's smp_init wait observes alive, this secondary is fully ready to receive IPIs (GIC redist + CPU interface up; IRQs unmasked; handler attached + SGI 0 enabled). Test `smp.ipi_resched_smoke` runs after smp_init returns and relies on this ordering.

### NEW at P2-Cd

50. **`g_run_tree`/`g_vd_counter`/`g_sched_initialized` are now per-CPU** (P2-Cd). Replaced by `struct CpuSched g_cpu_sched[DTB_MAX_CPUS]`. Each CPU dispatches against `g_cpu_sched[smp_cpu_idx_self()]`. ready() inserts into THIS CPU's run tree; sched() picks from THIS CPU's tree; sched_remove_if_runnable scans all CPUs' trees (since at v1.0 a thread is on at most one tree). Cross-CPU placement is a P2-Ce work-stealing concern.

51. **`g_need_resched` is now a per-CPU `volatile bool[DTB_MAX_CPUS]`** (P2-Cd). Each CPU's timer IRQ writes its own slot via `sched_tick()`; `preempt_check_irq()` reads its own slot. No cross-CPU access — writer and reader are always the same CPU. The atomic-bool requirement listed in P2-Bc trip-hazard #22 is therefore deferred to P2-Cdc IPI_RESCHED implementation, where one CPU may set another CPU's need_resched.

52. **`sched_init` now takes a `cpu_idx`** (P2-Cd). main.c calls `sched_init(0)` for the boot CPU after thread_init. per_cpu_main on each secondary calls `sched_init(idx)` after parking its idle thread in TPIDR_EL1. Calling sched_init twice for the same cpu_idx extincts.

53. **Per-CPU idle threads via `thread_init_per_cpu_idle`** (P2-Cd). The boot CPU's idle is `kthread` (set up by thread_init); secondaries allocate a fresh Thread descriptor that does NOT own a kstack — it runs on the per-CPU boot stack already in use by per_cpu_main. Idle threads live in `SCHED_BAND_IDLE` so they sort below NORMAL/INTERACTIVE in the run tree. Their state is `THREAD_RUNNING` from creation (the caller is "running" as this thread).

54. **Secondaries WFI without sched() at v1.0 P2-Cd** (P2-Cd). per_cpu_main's idle loop is `for(;;) wfi;` — NOT `for(;;){sched();wfi;}`. Reason: WFI on QEMU returns on architectural events even with masked IRQs, so a sched()-then-WFI loop on secondaries hammers their per-CPU sched data (no contention but wasted cycles), inflating total boot time from ~100 ms to >1000 ms. Without IPIs to deliver work to secondaries (P2-Cdc), the sched() call has nothing to switch to — we'd just yield-with-no-peer immediately. The sched_init + idle thread infrastructure is in place; the loop body lands at P2-Cdc when IPIs make it productive.

55. **`sched_remove_if_runnable` walks every CPU's tree** (P2-Cd). At v1.0 a thread is in at most one CPU's tree, so the first match wins. P2-Ce work-stealing introduces thread migration between trees — the same scan covers that case.

56. **`sched_runnable_count` aggregates across all CPUs** (P2-Cd). Diagnostic-only; not a hot path. Test `scheduler.runnable_count` continues to work because tests run on the boot CPU and only insert into CPU 0's tree.

57. **`g_cpu_sched[i].initialized` is the gate for sched-on-CPU-i** (P2-Cd). sched() / sched_tick / preempt_check_irq all check the per-CPU initialized flag — secondaries that haven't reached sched_init yet (during the smp_init bring-up window) silently no-op rather than touching uninitialized state. The boot CPU's flag is set early in main.c; secondaries' flags are set in per_cpu_main before g_cpu_alive[idx] is published.

### NEW at P2-Cc

42. **`msr sp_el1, ...` is UNDEFINED at EL1** (P2-Cc; ARM ARM B6.2). The only legal way to write `SP_EL1` from EL1 is `mov sp, ...` while `SPSel = 1` (so `sp` refers to SP_EL1). `msr sp_el0, ...` IS allowed at EL1 regardless of SPSel. The SPSel-dance pattern (`msr SPSel, #1; isb; mov sp, x0; msr SPSel, #0; isb`) is the canonical way to update SP_EL1 from kernel mode after init.

43. **`mrs sp_el1, ...` is also UNDEFINED at EL1** (P2-Cc). Tests cannot read `SP_EL1` directly to verify exception-stack discipline. The observability mechanism is `g_exception_stack_observed[cpu]`, written by `timer_irq_handler` capturing `&local` on first invocation per CPU. The test indexes by cpu_idx and verifies the captured address falls in `g_exception_stacks[cpu]`'s range.

44. **Both Current-EL vector groups (SP_EL0 + SP_ELx) MUST stay live** (P2-Cc). Although the kernel runs in SPSel=0 mode in steady state (so SP_EL0 group fires for IRQ-from-kernel), the kernel transits through SPSel=1 transiently after sched() context-switches out of an IRQ handler: cpu_switch_context's `mov sp` writes SP_EL1, then thread_trampoline unmasks IRQs while still in SPSel=1 mode. The next IRQ on that CPU thus dispatches to SP_ELx group. P2-Cd or later might collapse this with finish_task_switch + proper eret-driven SPSel restoration; until then, both groups must dispatch to the same handlers.

45. **SP_EL1 is set to LOW PA initially, re-anchored to HIGH VA after MMU enable** (P2-Cc). start.S sets SP_EL1 in step 4.6 BEFORE mmu_enable; at that point PC is at LOW PA and adrp+add resolves to LOW PA. After mmu_enable, step 8.5 calls kaslr_high_va_addr + SPSel-dance to re-anchor SP_EL1 at HIGH VA so exception handlers run on addresses matching the rest of the post-MMU kernel. Secondaries do the equivalent in secondary_entry. If a future change skips the re-anchor, the test's address comparison will fail (observed PA ≠ runtime HIGH VA of g_exception_stacks).

46. **g_exception_stack_observed[] is populated incrementally by timer_irq_handler** (P2-Cc). Slot 0 (boot CPU) is populated within the first few ticks after `msr daifclr, #2` in main.c. Slots 1..N-1 (secondaries) remain 0 at v1.0 P2-Cc — secondaries don't unmask IRQs until P2-Cd lands per-CPU GIC + idle threads. The test only verifies slot 0; it must not require slots 1+ to be populated.

47. **EXCEPTION_STACK_SIZE = 4096 (one page)** (P2-Cc). Sized to fit cleanly in linker alignment + cover EXCEPTION_CTX_SIZE (288 B) × ~14 nesting levels. If exception nesting under v1.0 ever exceeds this, the in-handler fault would clobber the slot below in g_exception_stacks. Sizing up doubles BSS cost (+ 32 KiB per doubling); not worth it until a use case demands it.

48. **`smp_cpu_idx_self()` reads MPIDR_EL1.Aff0** (low 8 bits) (P2-Cc). On QEMU virt this is exactly the CPU index 0..N-1. On bare-metal hardware where MPIDR has hierarchical fields (Aff0 = core, Aff1 = cluster), this would conflate cores across clusters. v1.0 P2-Cc accepts this limitation — Pi 5 bring-up at Phase 7 may need a hierarchical decode. Trip-hazard for cross-Aff hardware.

49. **Stack-overflow detection now works** (P2-Cc benefit, not a trip hazard but worth documenting). Pre-P2-Cc, a kernel stack overflow recursively faulted inside KERNEL_ENTRY because SP was in the guard region. Post-P2-Cc, KERNEL_ENTRY runs on SP_EL1 = per-CPU exception stack (clean), so `sub sp, sp, #EXCEPTION_CTX_SIZE` succeeds and exception_sync_curr_el's "kernel stack overflow" diagnostic is reachable. The vectors.S P1-F "KNOWN LIMITATION" is closed.

### NEW at P2-Cb

34. **PAC keys MUST be identical across all CPUs** (P2-Cb). `pac_derive_keys` runs ONCE on primary (derives 8 key halves from cntpct_el0 + ROR chain, stores to `g_pac_keys[8]` BSS). Each CPU calls `pac_apply_this_cpu` to load the SAME shared keys. Per-CPU random keys would break thread migration (P2-Ce work-stealing): a thread's signed return address on its kstack must auth-validate against APIA on whichever CPU resumes it. Don't refactor pac_derive_keys to run per-CPU.

35. **`pac_apply_this_cpu` is a leaf asm function with NO paciasp emission** (P2-Cb). It MUST stay leaf — the function is what enables PAC on the secondary; if its own prologue emitted paciasp signing with uninitialized APIA, the autiasp at epilogue would fail authentication. Don't add bl calls inside pac_apply_this_cpu, don't convert to a non-leaf C function.

36. **Per-CPU boot stacks at v1.0 P2-Cb are 16 KiB each, 7 secondaries** = 112 KiB BSS (P2-Cb). PSCI bring-up is serialized by smp_init (one secondary at a time), so per-CPU stack assignment is race-free. Future per-CPU idle threads (P2-Cd or later) will replace these once each CPU has its own scheduler context.

37. **`per_cpu_main` is `noreturn`** (P2-Cb). The trampoline's blr to per_cpu_main never returns — defensive halt loop in trampoline catches the impossible case. Don't remove the halt loop; it's the safety net.

38. **`g_cpu_alive` distinct from `g_cpu_online`** (P2-Cb). Trampoline sets g_cpu_online with caches off (Device-nGnRnE strb + dsb sy). per_cpu_main sets g_cpu_alive with caches on (cacheable store + dsb sy). Both must be observable to primary. The pair distinguishes "PSCI succeeded + trampoline ran" from "PAC + MMU + VBAR + TPIDR all worked" — useful for diagnosing per-CPU init failures.

39. **VBAR_EL1 shared across all CPUs at P2-Cb** (no per-CPU vector table yet). All CPUs install the same `_exception_vectors` address. Per-CPU vector tables (e.g., for IRQ affinity) deferred to P2-Cd if needed; not required for v1.0.

40. **TPIDR_EL1 = NULL on secondaries at P2-Cb**. `current_thread()` on a secondary returns NULL until P2-Cd assigns per-CPU idle threads. Don't call `current_thread()` from per_cpu_main or any subsequent secondary code expecting a non-NULL Thread; check or extinct on NULL.

41. **Cross-CPU function returns rely on shared APIA** (P2-Cb consequence). When a thread migrates (P2-Ce), its on-stack signed lr was signed by the source CPU's APIA. The destination CPU's autiasp must authenticate against the SAME APIA. Shared `g_pac_keys` ensures this. If a future feature derives per-CPU PAC keys (e.g., for cross-CPU ASLR), thread migration must be re-engineered (e.g., re-sign on migration boundary).

### Carried from Phase 1 (selected — see `docs/handoffs/005-phase1-close.md` for full list)

- Boot banner ABI (`Thylacine boot OK` + `EXTINCTION:`) is kernel-tooling contract.
- `KASLR_LINK_VA = 0xFFFFA00000080000` lives in TWO places (kaslr.h + kernel.ld); linker `ASSERT()` enforces equality.
- `volatile` on `g_kernel_pa_*` in kaslr.c is load-bearing under `-O2 -fpie -mcmodel=tiny`.
- TTBR0 stays active post-KASLR for low-PA access; Phase 2 retires when user namespaces move there (P2-D).
- `kfree` requires head pointer (not interior); enforced in P1-I-D.
- `kmem_cache_destroy` extincts on `nr_full != 0`; Phase 2 callers MUST drain caches before destroy.
- `spin_lock_irqsave` actually masks IRQs; don't revert to no-op (P1-I F30).
- `mmu_map_device` rejects ranges hitting kernel-image L3 block.

## Known deltas from ARCH

- ARCH §7.2 documents `struct Proc` with namespace, addr_space, fds, handles, cred, parent/children, notes — P2-A lands only `pid + thread_count + threads` because that's what P2-A's bringup uses. The struct grows by appending across sub-chunks (P2-B through P2-G); existing offsets stay stable.
- ARCH §7.3 documents `struct Thread` with errstr, EEVDF data, etc. — P2-A lands the minimal subset (tid, state, proc, ctx, kstack, list links). The rest lands at P2-B / Phase 2 close.
- ARCH §8 specifies EEVDF + per-CPU run trees + work-stealing — P2-A has neither. P2-B adds EEVDF + per-CPU; P2-C adds SMP + work-stealing.
- `scheduler.tla` at P2-A is a sketch — TLC-clean at small bounds but doesn't yet model EEVDF deadline math, wait/wake atomicity, or IPI ordering. ARCH §25.2 mandates the full spec by Phase 2 close; P2-B/P2-C/Phase 2 close land the refinements.

## References

- `docs/ARCHITECTURE.md §7` (process and thread model), `§8` (scheduler), `§9` (namespace), `§18` (handles), `§19` (VMO), `§20` (per-core SMP), `§28` (invariants — I-1 through I-9, I-17, I-18 land at this phase).
- `docs/ROADMAP.md §5` (Phase 2 deliverables, exit criteria, risks).
- `docs/handoffs/005-phase1-close.md` (Phase 1 close pickup pointer).
- `docs/reference/14-process-model.md` (P2-A as-built).
- `specs/scheduler.tla` + `specs/scheduler.cfg` (P2-A sketch).
- `specs/SPEC-TO-CODE.md` (action ↔ source location mapping; populated through Phase 2).
- `CLAUDE.md` (operational framework, audit-trigger surfaces, spec-first policy).

## Audit-trigger surfaces introduced this phase

| Surface | Files | Why | Landed at |
|---|---|---|---|
| Process model | `kernel/proc.c`, `kernel/include/thylacine/proc.h` | Lifecycle, rfork semantics (P2-D) | P2-A (descriptor only); P2-D (lifecycle) |
| Thread model | `kernel/thread.c`, `kernel/include/thylacine/thread.h` | Lifecycle, current_thread (TPIDR_EL1) | P2-A |
| Context switch | `arch/arm64/context.S`, `kernel/include/thylacine/context.h` | Register state save/load correctness; layout pinning | P2-A |
| Scheduler | `kernel/sched.c` (P2-B), `kernel/eevdf.c` (P2-B), `kernel/run_tree.c` (P2-B), `arch/arm64/ipi.c` (P2-C) | EEVDF correctness, SMP, wakeup atomicity (I-8, I-9, I-17, I-18) | (planned) |
| Namespace | `kernel/namespace.c` (P2-E) | Cycle-freedom (I-3), isolation (I-1) | (planned) |
| Handle table | `kernel/handle.c` (P2-F) | Rights, transfer, type discipline (I-2, I-4, I-5, I-6) | (planned) |
| VMO | `kernel/vmo.c` (P2-F) | Refcount, mapping lifecycle (I-7) | (planned) |
| Page fault | `arch/arm64/fault.c` (P2-D) | Lifetime, COW | (planned) |
| ELF loader | `arch/arm64/elf.c` (P2-G) | RWX rejection | (planned) |
