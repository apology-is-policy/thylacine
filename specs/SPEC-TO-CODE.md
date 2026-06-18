# Spec-to-code mapping

For each TLA+ spec, this file maps each action / invariant to a source location. **Honest status (RW-10, 2026-06-11):** the CI currency-gate planned at Phase-2 close was never built — `make specs` runs each module's default clean cfg and fails on TLC failure, but nothing verifies these mappings automatically and nothing runs the buggy-cfg set; both remain manual discipline (the tiered spec-gate runner is a tracked task). Line numbers in older sections drift; each section's **Currency note** records what moved since it was written.

Per `CLAUDE.md` Spec-first policy: if the four (spec, technical reference, code, user reference) disagree, **the spec wins**.

---

## scheduler.tla — P2-Cg/H impl mapping (SMP discipline lifted to spec)

> **Currency note (RW-10, 2026-06-11).** The P4-Ic6 subsections below describe
> `g_bootcpu_idle` + its deadlock-path dispatch as landed — that mechanism was
> **RETIRED** by the SMP redesign (#863, deep-smp-review 2026-06-05): the
> off-tree boot-CPU idle was the #860 root cause and is replaced by the
> per-CPU **pinned in-tree idle** (`cpu_pinned`; `kernel/sched.c` keeps the
> history in comments at :396/:817/:973). The as-built migration/idle model is
> pinned by `sched_alpha.tla` (the gating model) + `sched_oncpu.tla` (the
> diagnostic) — see their sections at the end of this file. Line numbers in
> this section are P2/P4-era (`try_steal` is now ~:788). scheduler.tla's
> wait/wake + IPI-ordering actions still map as written.

Status: **wait/wake proven; SMP runqueue + IPI ordering proven; LatencyBound liveness proven (P2-H, minimal universe); full-universe per-thread fairness deferred to Phase 5+**. Models thread state machine + per-CPU dispatch + cross-CPU work-stealing (`Steal`) + per-(src,dst) FIFO IPI delivery (`IPI_Send` / `IPI_Deliver`) with state-consistency, wait/wake-atomicity, no-double-enqueue, FIFO-ordering, and "every runnable thread eventually runs" invariants. Proves `NoMissedWakeup` (ARCH §28 I-9), `NoDoubleEnqueue` (ARCH §8.4), `IPIOrdering` (ARCH §28 I-18), and `LatencyBound` (ARCH §28 I-17) under the correct primitives + fairness assumptions; produces a counterexample for each under the corresponding buggy primitive (or, for LatencyBound, fairness drop). Does NOT yet model EEVDF deadline math (post-P2-Cg, deferred to Phase 5+).

**P2-H spec change**: `Steal` precondition tightened — `current[stealer] = NULL` added so only an idle CPU steals. Without it, the spec admitted a steal-back-and-forth lasso (two busy CPUs trade a thread) that doesn't occur in the impl (since `try_steal` is only called from `pick_next` inside `sched()`, which runs only when the calling CPU is between releasing prev and picking next). The refinement closes the spurious lasso at the spec level. Safety state-space numbers below shifted slightly post-refinement.

Safety: TLC-clean at `Threads = {t1, t2, t3}, CPUs = {c1, c2}, MaxIPIs = 2` — 10188 distinct states explored; depth 17.
Liveness: TLC-clean at `Threads = {t1, t2}, CPUs = {c1}, MaxIPIs = 1` — 23 distinct states; depth 5.

| Config | Flag | Invariant / Property | Result | Distinct |
|---|---|---|---|---|
| `scheduler.cfg`              | safety-only             | `Invariants`          | clean (3T × 2C) | 15840 |
| `scheduler_liveness.cfg`     | Spec_Live + SF          | `Invariants` + `LatencyBound` | clean (2T × 1C) | 23 |
| `scheduler_liveness_wfi.cfg` (P3-G; P4-Ic6-cfg) | Spec_Live_Wfi + SF on WFI path | `Invariants` + `LatencyBound` | clean (1T × 2C) | 1008 |
| `scheduler_liveness_hwake.cfg` (P4-Ic6-spec) | Spec_Live_Hwake + SF on Wake | `Invariants` + `LatencyBound` + `HardwareWakeProgress` | clean (2T × 1C) | 23 |
| `scheduler_buggy.cfg`        | `BUGGY = TRUE`          | `NoMissedWakeup`     | violation (depth 6) | 101 |
| `scheduler_buggy_steal.cfg`  | `BUGGY_STEAL = TRUE`    | `NoDoubleEnqueue`    | violation (depth 4) | 40 |
| `scheduler_buggy_ipi.cfg`    | `BUGGY_IPI_ORDER = TRUE`| `IPIOrdering`        | violation (depth 6) | 442 |
| `scheduler_buggy_starve.cfg` | Spec (no fairness)      | `LatencyBound`       | stuttering (depth 3) | 23 |
| `scheduler_buggy_wfi.cfg` (P3-G) | Spec_Live (no WFI fairness) | `LatencyBound` | stuttering (depth ~16) | 3888 |
| `scheduler_buggy_hwake.cfg` (P4-Ic6-spec) | Spec_Live (no SF on Wake) | `HardwareWakeProgress` | stuttering (depth 4) | 23 |

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/proc.c::proc_init` + `kernel/thread.c::thread_init` + `kernel/sched.c::sched_init(0)` | Bootstrap: kproc PID 0 + kthread TID 0 RUNNING on CPU0. cond=FALSE, waiters={} initial. P2-Cd: per-CPU `g_cpu_sched[]` initialized one CPU at a time as secondaries come online. |
| `Yield(cpu)` | `kernel/sched.c::sched()` (RUNNING-state branch). **Also: timer-IRQ-driven preempt path** (P2-Bc): `arch/arm64/timer.c::timer_irq_handler` → `sched_tick` → sets `g_need_resched[cpu_idx]` when slice expires → `preempt_check_irq` (called from `arch/arm64/vectors.S` IRQ-return) → `sched()` performs the same RUNNING → RUNNABLE+insert+pick-next transition. The spec's Yield is non-deterministic; cooperative yield (sched() called from kernel code) and involuntary preempt (sched() called from preempt_check_irq) are observably indistinguishable. The atomicity that matters for NoMissedWakeup is preserved by sleep()'s spin_lock_irqsave bracketing the WaitOnCond body — preempt cannot fire mid-WaitOnCond. |
| `Block(cpu)` | `kernel/sched.c::sched()` (SLEEPING-state branch) | Caller-set state SLEEPING; sched() leaves prev out of run tree. P2-Bb path is via sleep(); generic Block usable for postnote stop / etc. at Phase 2 close. |
| `Wake(t)` | (deferred — generic non-cond wake) | At P2-Bb, all wakes go through wakeup(r) → ready(t). Generic Wake(t) lands when notes / signals at Phase 5. |
| `Resume(cpu)` | implicit in `kernel/sched.c::pick_next` + `per_cpu_main` idle loop | Idle CPU pick-up. P2-Cdc: secondaries' `for(;;){sched(); wfi;}` loop is the explicit Resume — sched() picks any runnable thread from the per-CPU run tree (pick_next) or steals from a peer (try_steal), then runs. WFI returns on IPI_RESCHED (or any architectural event) and the loop body re-enters sched(). |
| `WaitOnCond(cpu)` | `kernel/sched.c::sleep(r, cond, arg)` | **The atomic protocol.** Under `r->lock`: `cond(arg)` check; if FALSE, `r->waiter = current` + `current->state = THREAD_SLEEPING` + `current->rendez_blocked_on = r`; drop spinlock; `sched()` (which sees state SLEEPING, leaves prev out of run tree). Resume reacquires `r->lock` and re-checks. P2-Cf adds the on_cpu spin in wakeup() to close the SMP wait/wake race; the spec abstracts this via atomic actions (no half-saved intermediate state). |
| `WakeAll` | `kernel/sched.c::wakeup(r)` | **The atomic publish.** Under `r->lock`: clear `r->waiter`, `current->rendez_blocked_on = NULL`, transition `THREAD_SLEEPING → THREAD_RUNNABLE`, `ready(t)`. Producer must have set cond TRUE before calling (or rely on wakeup's lock acquisition for the happens-before edge). Single-waiter version of multi-waiter spec. P2-Cf: wakeup() spin-waits on waiter's `on_cpu` flag before transitioning state — closes the SMP race where a peer could pick a half-saved ctx mid-`cpu_switch_context`. The spin is impl-level enforcement of the spec's atomic action. |
| `BuggyCheck(cpu)` | (none — bug class statically prevented) | The bug requires splitting cond check from sleep transition; in `sleep()` both happen inside one `while (!cond)` body under `r->lock`. |
| `BuggySleep(cpu)` | (none) | Same as above. |
| **`Steal(stealer, victim)`** *(P2-Cg)* | `kernel/sched.c::try_steal` | **Cross-CPU work-stealing.** Stealer holds its own `cs->lock`; calls `spin_trylock` on each peer's `cs->lock` in turn. On success, walks bands top-down, picks first non-empty band's thread, calls `unlink(peer, stolen)`, releases peer's lock, rebases `stolen->vd_t` to caller's `cs->vd_counter++`. Atomic at the spec level; the impl's two-lock window is observably indistinguishable from a single atomic step because no other CPU can read victim's tree mid-unlink (peer's spin_trylock fails). Spec omits vd_t (deferred). |
| **`BuggySteal(stealer, victim)`** *(P2-Cg)* | (none — bug class statically prevented) | Bug = adding to stealer's runq without removing from victim. The impl's `try_steal` always calls `unlink(peer, stolen)` between read and rebase; no code path exists that adds without removing. The static prevention is the lexical structure of `try_steal`. |
| **`IPI_Send(src, dst)`** *(P2-Cg)* | `arch/arm64/gic.c::gic_send_ipi` | **GIC SGI send.** Writes `ICC_SGI1R_EL1` with TargetList encoding `(sgi_intid << 24) | (1 << target_cpu_idx)`. Sequence-number tracking is conceptual — the GIC has per-(SGI ID, recipient) pend bits with edge-trigger semantics, which collapse multiple sends of the same SGI to one delivery. The spec models per-pair monotonic seq-numbers because IPI ordering is what matters at the protocol level; whether the underlying transport collapses or queues is an impl detail. v1.0 callers: `kernel/test/test_smp.c::smp.ipi_resched_smoke`, `smp.work_stealing_smoke`. P5+ callers: scheduler wakeup() to a sleeping CPU, TLB shootdown, etc. |
| **`IPI_Deliver(src, dst)`** *(P2-Cg)* | `kernel/smp.c::ipi_resched_handler` (per-type handler dispatched by `arch/arm64/gic.c::gic_dispatch`) | **GIC SGI delivery.** `gic_dispatch` reads `ICC_IAR1_EL1` to get INTID, looks up the registered handler by INTID, invokes it, then `ICC_EOIR1_EL1`. For IPI_RESCHED at v1.0, the handler increments a per-CPU counter and returns. The "consume head of FIFO" semantics is enforced by the GIC's edge-trigger + arbitration: SGIs of the same INTID + same priority are processed in send order per ARM IHI 0069 §11.2.3. |
| **`BuggyIPI_Deliver(src, dst)`** *(P2-Cg)* | (none — bug class statically prevented) | Bug = handler processes IPIs out of send order. The GIC SGI hardware enforces FIFO per (src, INTID, dst) at equal priority; impl-level handler does not buffer or reorder. If future IPI types are introduced at distinct priorities (TLB shootdown urgent, etc.), this static prevention degrades to "FIFO per (src, type, dst)" rather than "FIFO per (src, dst)" — the spec would need refinement to per-type queues at that point. |

| Spec invariant | Source enforcement |
|---|---|
| `StateConsistency` (RUNNING ⇔ some CPU's current) | `sched()` + `sleep()` + `wakeup()` update state under their respective locks. P2-Cd: per-CPU run-tree lock serializes per-CPU updates. P2-Cf: `on_cpu` flag closes the cross-CPU half-saved race. |
| `NoSimultaneousRun` (RUNNING on ≤1 CPU) | One thread is RUNNING ⇒ it's some CPU's `current_thread()` (TPIDR_EL1). The TPIDR_EL1 register is per-CPU; cross-CPU "I'm running this thread" claims are mutually exclusive by hardware. Steal does not violate this — stolen threads are RUNNABLE in the victim's runq, not RUNNING. |
| `RunnableInQueue` (RUNNABLE ⇔ in some runq) | `ready()` is the only entry to RUNNABLE; sets state + inserts into per-CPU run tree. `sched()` pick_next removes prev RUNNING-→-RUNNABLE-→-tree (via insert_sorted) and pulls next out (RUNNABLE-→-RUNNING). |
| `SleepingNotInQueue` (SLEEPING ⇒ no runq + no current) | `sleep()` sets state SLEEPING ONLY after `sched()` has switched the thread off-CPU and pick-next has not re-inserted (the SLEEPING-state branch of sched() is the no-op insert). |
| `NoMissedWakeup` (cond=TRUE ⇒ waiters={}) | `sleep()`'s cond check + waiter-add + sleep transition all happen inside one `while (!cond)` body under `r->lock`. `wakeup()`'s waiter-clear + cond is set by caller, both observable to sleep's resume re-check. The atomicity defeats the missed-wakeup race — proven in TLC, enforced at compile time by the lexical structure of `sleep()`. |
| **`NoDoubleEnqueue`** (thread in ≤1 runq) *(P2-Cg)* | `try_steal`'s `unlink(peer, stolen)` call between read and rebase ensures the thread leaves victim's tree before joining caller's. The unlink is unconditional in the success path; no code path adds-without-removing. Plus, `ready()` is the only public insertion entry; it does not check "already in some other runq" — soundness rests on callers (sched, wakeup, try_steal) maintaining the invariant. |
| **`IPIOrdering`** (head of queue = next-expected delivery seq) *(P2-Cg)* | GIC SGI hardware: per-(src, dst, sgi_intid) pend bit; edge-trigger; same-priority SGIs are arbitrated FIFO per ARM IHI 0069 §11.2.3. The `gic_dispatch` ACK-and-call path consumes one pending SGI at a time; subsequent SGIs are processed in arbitration order. v1.0 has only IPI_RESCHED, so the invariant is trivially per-pair FIFO. P5+ multi-type IPIs would refine to per-(src, type, dst) FIFO. |
| **`LatencyBound`** (every RUNNABLE thread eventually runs) *(P2-H, ARCH §28 I-17)* | Three-layer impl enforcement: (1) **EEVDF deadline math** — `kernel/sched.c::insert_sorted` orders the per-CPU run tree by `vd_t`, so the next pick is always the deadline-tightest thread; v1.0 uses uniform weight=1, so `vd_t` advances as `cs->vd_counter++` per dispatch (effectively round-robin). (2) **Timer-driven preempt** — `arch/arm64/timer.c::timer_irq_handler` → `kernel/sched.c::sched_tick` decrements `slice_remaining` and sets `g_need_resched[cpu]` when slice expires; `arch/arm64/exception.c` IRQ-return path calls `preempt_check_irq` → `sched()` to honor the flag. Bounds wall-clock latency for any single thread to `THREAD_DEFAULT_SLICE_TICKS` × tick_period × N. (3) **WFI loop in idle CPUs** — `kernel/sched.c::per_cpu_main` re-enters `sched()` on every IPI_RESCHED, so a runnable thread on a peer's runq triggers steal-and-dispatch on the next idle wake. The spec's SF-on-Resume + SF-on-Yield + SF-on-WakeAll abstracts this layered impl-side enforcement to "fire if enabled inf often." |

### P4-Ic6-impl landed (this chunk; R12-sched kernel-side resolution)

- **`g_bootcpu_idle`** — a dedicated boot-CPU idle Thread allocated in `boot_main` via `thread_create(kproc(), bootcpu_idle_main)`, registered via `sched_set_bootcpu_idle()`. **Off-tree** by design: NOT `ready()`'d into BAND_IDLE's run tree. Used solely as the deadlock-path fallback in `sched()` when prev is SLEEPING/EXITING and no peer is runnable system-wide.
- **`bootcpu_idle_main()`** — runs the WFI idle loop body symmetric to `kernel/smp.c::per_cpu_main`'s tail loop on secondaries (IRQ-mask + idle_in_wfi flag + sched + wfi + clear-flag).
- **`sched_set_bootcpu_idle()`** — set-once registration with magic + band validation. Stored in static `g_bootcpu_idle` in sched.c (read-only after init).
- **`sched()` deadlock path** — replaces `extinction("sched: deadlock — current is blocking, no runnable peer system-wide")` with: when `prev->state != THREAD_RUNNING` AND pick_next + try_steal both NULL AND `smp_cpu_idx_self() == 0` AND `g_bootcpu_idle != NULL`, set `next = g_bootcpu_idle`. Defensive extinction kept for secondaries (their per_cpu_main idle is in BAND_IDLE's tree after first yield; the path is unreachable in practice).
- **Why off-tree?** preempt_check_irq runs at SpSel=1 (hardware-set on exception entry). If bootcpu_idle were in BAND_IDLE's tree, preempt's `pick_next` would switch to it via `cpu_switch_context`, which does `mov sp, x9` — at SpSel=1 this writes to SP_EL1 (the per-CPU exception stack pointer), clobbering it with bootcpu_idle's kstack address. By restricting bootcpu_idle to the deadlock path (which fires only from VOLUNTARY sched() callers at SpSel=0), `cpu_switch_context`'s SP write naturally targets SP_EL0, and the per-CPU exception stack stays intact.
- **`on_cpu` protocol unchanged** — Direction B reuses the existing P2-Cf wakeup discipline. The wakeup() spin on `t->on_cpu` works correctly because the deadlock-path switch is a normal `cpu_switch_context(prev, g_bootcpu_idle)` at SpSel=0; prev's on_cpu is cleared via the standard `prev_to_clear_on_cpu` handoff in the resume path. No new sub-states (no THREAD_HALTED); no new cross-CPU rules.
- **Plan 9 heritage alignment** — bootcpu_idle is the Thylacine analog of Plan 9's per-CPU `runproc()` idle path. Plan 9 has no notion of "deadlock — current is blocking, no runnable peer system-wide" because `runproc()` calls `idlehands()` (architecture-specific WFI/HLT) when the runq is empty, treating idle as the natural empty-runq state rather than an error. This chunk brings Thylacine's boot CPU to the same model (post-impl, the boot CPU's "no work" state is bootcpu_idle's WFI loop, not extinction).
- **Test conversion**: `kernel/test/test_virtio_blk_probe.c` yield-poll loop replaced with `wait_pid`. The R12-sched workaround (kthread busy-yielding to keep itself RUNNABLE while child slept on hardware IRQ) is no longer needed — kthread now sleeps on the child_done rendez; deadlock-path switches to bootcpu_idle; IRQ wakes child; child exits; exit wakeup wakes kthread.
- **Closes R12-sched + R12-sched-impl deferred audits.** Trip hazards #181 (R12-sched original) + #185 (R12-sched-impl architectural choice) CLOSED.
- **Direction A (refactor on_cpu) explicitly NOT taken** — would have introduced a THREAD_HALTED sub-state with cross-CPU steal-skip rules. Direction B is architecturally cleaner (reuses existing mechanism), matches Plan 9 heritage, and Linux per-CPU swapper pattern.

### P4-Ic6-cfg landed (R12-wfi-cfg resolution)

- **`Wake(t)`** refined: now atomically clears `wfi[cpu]` of its chosen target. Models the impl invariant that an IRQ which places work into a wfi'd CPU's runq either (a) was delivered to that CPU directly — hardware lifts WFI when the IRQ becomes pending — or (b) was delivered to a peer CPU which then IPIs the target (P3-G's NotifyWFIPeer mechanism) and the IPI eventually delivers, clearing wfi[target]. Both impl paths produce identical post-state (RUNNABLE thread in runq[target] with ~wfi[target]); collapsing into a single atomic spec step is a sound abstraction for liveness. Without this refinement, the spec admitted a Wake-enqueues-into-wfi'd-runq deadlock with no corresponding impl trace.
- **`WakeAll`** refined: same wfi-clear pattern for cpu0 (the CHOOSE'd recipient of waiters), conditional on `waiters # {}` to avoid spurious-wake traces when WakeAll is a no-op.
- **`scheduler_liveness_wfi.cfg`** reparameterized from 2T × 2C → 1T × 2C. At 2T × 2C the cfg has been violating LatencyBound since the P3-G commit at `7a61651` (verified by TLC replay at that commit) due to a CHOOSE-determinism artifact: t1 alternates RUNNING/SLEEPING via Block/Wake while t2 gets Steal'd between c1 and c2 indefinitely, never picked by Resume (which always sees t1 in its target runq instead). The artifact is the same one documented in scheduler_liveness.cfg's comment (the spec lacks per-thread Resume fairness; the deferred Phase 5+ fix is `Resume(cpu, t)` parameterization with `SF_vars(Resume(cpu, t))` per (cpu, t) pair, bundled with EEVDF weight math). At 1T × 2C the runq is always a singleton or empty, so CHOOSE has no degree of freedom and the artifact disappears. The cfg's original F77/F78 intent (verify NotifyWFIPeer fairness prevents WFI starvation) was not actually being exercised at 2T × 2C — the lasso never reached the F77/F78 scenario shape. The buggy companion (`scheduler_buggy_wfi.cfg`, still 2T × 2C) verifies F77/F78 negatively (drops WFI fairness; produces stuttering counterexample). The 1T × 2C reparameterization preserves the WFI dynamics check (EnterWFI / NotifyWFIPeer / IPI_Deliver fairness; Wake's wfi-clear) while eliminating the artifact.
- **R12-wfi-cfg deferred audit CLOSED**: previously tracked as "cfg parameters too tight" but the root cause turned out to be the spec-modeling gap in Wake/WakeAll (work could land in wfi'd runqs with no escape mechanism) AND the pre-existing CHOOSE-determinism artifact at 2T × 2C. Both addressed.
- **State count effects**: `scheduler.cfg` distinct states 25416 → 15840 (Fix A constrains Wake/WakeAll's post-state — fewer reachable states). `scheduler_buggy_wfi.cfg` 5760 → 3888 (same effect through Wake/WakeAll's tighter semantics). Other cfgs unchanged (no Wake/WakeAll fires under their action restrictions).

### P4-Ic6-spec landed (R12-sched fairness extension)

- **`HardwareWakeProgress`** temporal property added: `\A t : [](state[t] = "SLEEPING" /\ t \notin waiters => <>(state[t] # "SLEEPING"))`. Captures the spec-level invariant that every non-cond-waiter SLEEPING thread (i.e., a thread blocked on a hardware-IRQ Rendez in the impl) must eventually leave SLEEPING. The exclusion `t \notin waiters` matches `Wake(t)`'s precondition: cond-waiters are woken by `WakeAll`, not `Wake`, so they fall outside this property's scope. **Distinct from `LatencyBound`**: LatencyBound is a RUNNABLE-only property and vacuously holds when threads stay SLEEPING. HardwareWakeProgress is the property that *fails* when hardware-blocked threads starve — exactly what R12-sched is about.
- **`Liveness_Hwake`**: `Liveness_Wfi /\ \A t : SF_vars(Wake(t))`. SF on Wake encodes "hardware IRQs eventually deliver" — the external-event guarantee independent of any kernel scheduling decision.
- **`Spec_Live_Hwake == Init /\ [][Next]_vars /\ Liveness_Hwake`**.
- **`scheduler_liveness_hwake.cfg`** (2T × 1C, MaxIPIs=2): correct, 23 distinct states, Invariants + LatencyBound + HardwareWakeProgress all hold.
- **`scheduler_buggy_hwake.cfg`** (2T × 1C, MaxIPIs=2): SPECIFICATION uses `Spec_Live` (no SF on Wake). HardwareWakeProgress violated at depth 4 — two consecutive Block(c1) actions leave both threads in SLEEPING, then state stutters indefinitely. Models the R12-sched impl bug (`kernel/sched.c::sched()` extincts on "no runnable peer system-wide" before hardware can deliver an IRQ-driven wakeup).
- **`Wake(t)`** semantic now documented in spec header comments as the abstract model of `kernel/irqfwd.c::kobj_irq_dispatch` → `kernel/sched.c::wakeup()` for a thread blocked on a hardware-IRQ Rendez. The spec models only one shared `cond` while the impl has many Rendez; threads blocked on a hardware Rendez are NOT in the spec's `waiters` set (which the spec reserves for cond-waiters), so Wake(t)'s precondition `t \notin waiters` is exactly the abstraction boundary that matches "hardware-blocked thread".
- **~~Impl mapping deferred~~** — CLOSED at P4-Ic6-impl via Direction B. See P4-Ic6-impl landed section above. Direction A (refactor on_cpu) NOT taken; Direction B (separate boot-CPU idle thread) chosen for Plan 9 heritage alignment + minimal mechanism reuse.
- **Pre-existing finding (CLOSED at P4-Ic6-cfg)**: `scheduler_liveness_wfi.cfg` (P3-G) had been violating LatencyBound since the P3-G commit at `7a61651`. Initially diagnosed as "cfg parameters too tight" during P4-Ic6-spec; deeper investigation at P4-Ic6-cfg revealed two distinct issues: (1) a real spec-modeling gap where Wake/WakeAll could place work into a wfi'd CPU's runq with no escape mechanism (closed by the Wake-clears-wfi + WakeAll-clears-wfi refinement landed at P4-Ic6-cfg); (2) a pre-existing CHOOSE-determinism artifact at 2T × 2C unrelated to WFI (resolved by reparameterizing the cfg to 1T × 2C, matching scheduler_liveness.cfg's same workaround for the same artifact). See P4-Ic6-cfg landed section above for the resolution detail.

### P3-G landed

- **`wfi: [CPUs -> BOOLEAN]`** variable added: TRUE while CPU is halted in WFI awaiting IPI. Maps to `kernel/sched.c::struct CpuSched::idle_in_wfi`.
- **`EnterWFI(cpu)`** action: pre `current[cpu]=NULL ∧ runq[cpu]={} ∧ ~wfi[cpu] ∧ Cardinality(CPUs)>1`. Effect: `wfi[cpu]=TRUE`. Maps to `kernel/smp.c::per_cpu_main`'s set-and-WFI sequence in the idle loop.
- **`NotifyWFIPeer(src, dst)`** action: pre `wfi[dst] ∧ ∃c: runq[c]≠{}`. Sends IPI; structurally identical to `IPI_Send` with stronger precondition. Maps to `kernel/sched.c::sched_notify_idle_peer` called from `ready()`.
- **`Resume(cpu)`** + **`Steal(stealer, victim)`** preconditions extended with `~wfi`. The impl-side analogue: a CPU in WFI is halted; can't run `sched()` and can't `try_steal` until IPI wakes it.
- **`IPI_Deliver(src, dst)`** + **`BuggyIPI_Deliver`**: also clear `wfi[dst]=FALSE`. Models the GIC SGI delivery exiting WFI on the dst CPU.
- **`Liveness_Wfi`**: extends `Liveness` with `\A pair : SF_vars(NotifyWFIPeer(pair[1],pair[2])) ∧ \A pair : SF_vars(IPI_Deliver(pair[1],pair[2]))`. Strong fairness on both clauses ensures a WFI'd CPU never indefinitely starves a runnable thread.
- **`Spec_Live_Wfi == Init ∧ [][Next] ∧ Liveness_Wfi`**.
- **`scheduler_liveness_wfi.cfg`** (1T × 2C since P4-Ic6-cfg; was 2T × 2C 2026-05-11 and earlier, reparameterized to avoid CHOOSE-determinism artifact — see P4-Ic6-cfg section above), MaxIPIs=2: correct, 1008 distinct states, LatencyBound holds.
- **`scheduler_buggy_wfi.cfg`** (2T × 2C, MaxIPIs=2): SPECIFICATION uses `Spec_Live` (no WFI fairness). LatencyBound violated — TLC produces counterexample showing CPU stuck in WFI while a thread starves on a peer's runq. Models the F78 impl bug (ready/wakeup don't IPI a WFI'd peer). State count 3888 distinct (was 5760 pre-P4-Ic6-cfg; Wake/WakeAll Fix A constrains some Wake post-states).
- Existing `scheduler.cfg` state count grew 10188 → 25416 due to the wfi variable. All existing buggy configs (`scheduler_buggy.cfg` / `_steal.cfg` / `_ipi.cfg` / `_starve.cfg`) still produce their original counterexamples; the wfi addition is orthogonal.
- Closes R5-H **F77** (try_steal contention sentinel + sched retry; impl-only, no spec change required) + **F78** (WFI machinery + NotifyWFIPeer fairness).
- ARCH §28 **I-9** (NoMissedWakeup) and **I-18** (IPIOrdering) preserved by the extension. **I-17** (LatencyBound) proven across two universes: 1C cooperative-only (`scheduler_liveness.cfg`, 23 states) AND 1T × 2C with WFI/IPI (`scheduler_liveness_wfi.cfg`, 1008 states; reparameterized at P4-Ic6-cfg — see that section).

### P2-H landed (prior)

- `LatencyBound` temporal property (`\A t : [](state[t] = "RUNNABLE" => <>(state[t] = "RUNNING"))`).
- `Liveness == /\ \A cpu : SF_vars(Resume(cpu)) /\ \A cpu : SF_vars(Yield(cpu)) /\ SF_vars(WakeAll)`.
- `Spec_Live == Init /\ [][Next]_vars /\ Liveness`.
- `Steal` precondition tightened: `current[stealer] = NULL` added — only an idle CPU steals (matches impl: `try_steal` is called from `pick_next` inside `sched()`, when calling CPU is between releasing prev and picking next). Closes a spurious steal-back-and-forth lasso. Safety state-space numbers shifted slightly post-refinement.
- `scheduler_liveness.cfg` (2T × 1C, MaxIPIs=1): `Spec_Live` + invariants + `LatencyBound` clean at 23 distinct states. Liveness check at minimal universe deliberately — full-universe per-thread fairness requires `Yield(cpu, t)` parameterization (see deferred section in scheduler.tla).
- `scheduler_buggy_starve.cfg` (2T × 1C): `Spec` (no fairness) + `LatencyBound` produces stuttering counterexample at depth 3. Documents "without fairness, the scheduler does not satisfy I-17."
- ARCH §28 I-17 promoted from "in spec" to **"proven in spec at minimal universe + counterexample documented"**. Full-universe refinement deferred to Phase 5+ when sched_setweight introduces meaningful weight asymmetry.

### P2-Cg landed

- `Steal(stealer, victim)` action (~10 lines TLA+): models cross-CPU work-stealing as atomic transfer. Maps to `kernel/sched.c::try_steal` (line 273). Spec omits vd_t rebasing.
- `BuggySteal(stealer, victim)` action: models forgotten unlink → thread in two runqs. Counterexample in `scheduler_buggy_steal.cfg` at depth 4.
- `IPI_Send(src, dst)` + `IPI_Deliver(src, dst)` actions: per-pair FIFO queue with monotonic seq numbers. Maps to `arch/arm64/gic.c::gic_send_ipi` + `kernel/smp.c::ipi_resched_handler`.
- `BuggyIPI_Deliver(src, dst)` action: pops arbitrary index, advances deliver_seq. Counterexample in `scheduler_buggy_ipi.cfg` at depth 6.
- `NoDoubleEnqueue` invariant: thread in at most one runq.
- `IPIOrdering` invariant: head of any non-empty queue equals next-expected delivery seq.
- `MaxIPIs` constant bounds state space (queue length AND total sends per pair). At MaxIPIs=2 with 3 threads + 2 CPUs, scheduler.cfg explores 10188 distinct states in <1 s.

### P2-Bc landed (preempt mapping; carried from prior chunk)

- Timer-IRQ-driven preemption path mapped to spec's existing Yield action — non-deterministic Yield in the spec covers both cooperative and involuntary yields. NoMissedWakeup atomicity preserved by sleep()'s spin_lock_irqsave bracketing.
- Per-thread slice + replenish-on-RUNNING modeled implicitly: the spec's Yield can fire at any state, which corresponds to "preempt-when-slice-expires" being an arbitrary scheduler decision.

### Deferred (post-P2-H)

- **Full-universe LatencyBound** with per-thread fairness: requires parameterizing Yield/Block by both `(cpu, thread)` and adding SF for each pair. State space + fairness-clause cardinality both grow; meaningful when EEVDF weights differ (since equal-weight round-robin emerges naturally from CHOOSE rotation at minimal universe). Phase 5+ when `sched_setweight` is exposed.
- **Quantitative LatencyBound** (slice × N step bound): requires explicit `Slice` variable + step counter. Today's qualitative ("eventually") form is what TLA+ liveness checks naturally; quantitative bounds need a refinement mapping. Defer to whenever a numeric bound at the spec level becomes load-bearing (e.g., real-time scheduler work post-v1.0).
- **Full EEVDF math**: `vd_t = ve_t + slice × W_total / w_self` with weighted virtual time advance. Becomes meaningful when weights differ — Phase 5+ when sched_setweight is exposed. v1.0 weight=1 always; current `g_vd_counter++` advance is a valid instantiation.
- **`PickIsMinDeadline` invariant**: implies the impl's pick-min-vd_t is correct. Currently provable manually (the impl is mechanically pick-min) but not modeled.
- **Per-IPI-type ordering**: when multiple IPI types coexist (P5+ TLB shootdown, halt, generic), the ordering invariant should refine to per-(src, type, dst) FIFO. Today's per-(src, dst) version is sound only when all IPIs are equal-priority + same-type.
- **`finish_task_switch` ordering invariant**: P2-Ce `pending_release_lock` handoff + P2-Cf `prev_to_clear_on_cpu` handoff maintain "destination CPU completes prev's release before resuming." The spec's atomic-action model already enforces this implicitly; an explicit invariant would require modeling intermediate save states.
- **`Spawn(t)` action**: open-universe thread creation (current spec is closed-universe). P2-D rfork lands the impl side; spec extension follows.

---

## territory.tla — P2-Ea spec + P5-attach-mount extension (impl at P2-Eb + P5-attach-mount)

> **Currency note (RW-10, 2026-06-11).** The Mount/Unmount rows below model a
> PATH-keyed mount table; **stalk-2 re-keyed `PgrpMount` to the full
> mount-point Spoor identity `(dc, devno, qid.path)`** (`kernel/territory.c`
> mount/unmount/mount_lookup; STALK-DESIGN §5 + ARCH §9.6.7), and the impl
> additionally carries a mount-graph cycle check (`territory.c:587`) the spec
> does not model (the spec's `NoCycle` covers the bind graph only). The
> refcount-consistency invariant the spec pins is key-shape-independent and
> still holds as mapped; the re-key + the second cycle check are unmodeled
> strengthenings, not violations. RW-4 added the per-Territory `ns_lock`
> (SA-F1) — also unmodeled (the spec's actions are atomic).

Status: **cycle-freedom proven; isolation structural; mount-refcount consistency proven; impl landed at P2-Eb + P5-attach-mount.** Models the Plan 9 territory primitives — `bind` as a directed graph with cycle-freedom (I-3) the primary state invariant, AND `mount` as a (path, Spoor) graft table with `MountRefcountConsistency` the per-Spoor refcount invariant per ARCH §9.6.6. Isolation (I-1) is structural. RFNAMEG (shared territory) is deliberately not modeled at this phase. The user-visible `mount` syscall is deferred until fd-syscall infrastructure exists (P5-fd-syscalls); the kernel-internal C API is the v1.0 deliverable.

TLC verdicts at `Procs = {p1, p2}, Paths = {a, b, c}, Spoors = {s1, s2}`:

| Config | Flag | Verdict | Depth | Distinct states |
|---|---|---|---|---|
| `territory.cfg`                         | (all FALSE)                       | clean (no error)              | full  | 2,560,000 |
| `territory_buggy.cfg`                   | `BUGGY_CYCLE = TRUE`              | NoCycle violated              | 4     | 106 |
| `territory_buggy_mount_no_refbump.cfg`  | `BUGGY_MOUNT_NO_REFBUMP = TRUE`   | MountRefcountConsistency violated | 2 | 172 |
| `territory_buggy_unmount_no_refdrop.cfg`| `BUGGY_UNMOUNT_NO_REFDROP = TRUE` | MountRefcountConsistency violated | 3 | 884 |
| `territory_buggy_destroy_leak.cfg`      | `BUGGY_DESTROY_LEAK = TRUE`       | MountRefcountConsistency violated | 3 | 855 |

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/territory.c::territory_init` (P2-Eb) | Empty bindings + empty mounts per Proc; refcount = 0 per Spoor. |
| `Bind(p, src, dst)` | `kernel/territory.c::bind` (P2-Eb) | The cycle-checked bind primitive. Verify-then-insert. |
| `Unbind(p, src, dst)` | `kernel/territory.c::unbind` (P5-attach-mount; renamed from `unmount`) | Removes a bind edge. The verb `unmount` was repurposed at P5-attach-mount for the mount-table primitive; this function is the bind-edge remover. |
| `Mount(p, s, path)` | `kernel/territory.c::mount` (P5-attach-mount) | Idempotent on `(target, source)` equality; bumps `spoor_ref(source)` per new entry. MREPL replaces an existing entry at the same target. |
| `Unmount(p, s, path)` | `kernel/territory.c::unmount` (P5-attach-mount) | Removes ONE entry per call; drops `spoor_unref(source)`. The impl looks up by `target_path` (first match); the spec quantifies over `s` because at the spec level the entry is a `(path, s)` pair. |
| `ForkClone(parent, child)` | `kernel/territory.c::territory_clone` (called by `kernel/proc.c::rfork`) | Deep-copies bindings AND mounts; for each cloned mount entry, calls `spoor_ref(source)`. Precondition (spec): child's territory must be in Init state — mirrors the impl's "alloc-then-clone" sequence. |
| `BuggyBind` / `BuggyMountNoRefbump` / `BuggyUnmountNoRefdrop` / `BuggyDestroyLeak` | (none — bug classes statically prevented by impl discipline) | Each clean action's refcount update is mechanical and inseparable from the table mutation; a caller that bypasses `mount()` / `unmount()` / `territory_clone()` / `territory_unref()` would re-introduce the bug. |

| Spec invariant | Source enforcement |
|---|---|
| `NoCycle` (no path is reachable from its own bindings via transitive closure) | `kernel/territory.c::bind`'s cycle-check before insert. |
| `MountRefcountConsistency` (per-Spoor refcount equals cardinality of mount entries referencing it across all Territories) | spoor_ref / spoor_unref discipline at every mount-table mutation site: `mount` bumps before insert; `unmount` drops after remove; `territory_clone` bumps per cloned entry; `territory_unref` final-release drops per remaining entry BEFORE `kmem_cache_free`. |
| `MountRefcountNonNegative` | `spoor_unref`'s underflow extinct guarantees the counter never drops below zero. |

### P2-Ea landed (this chunk)

- `bindings` variable + Reachable transitive-closure helper.
- `Bind` / `BuggyBind` / `Unbind` / `ForkClone` actions.
- `NoCycle` invariant + `WouldCreateCycle` precondition helper.
- Two configs: territory.cfg (clean) + namespace_buggy.cfg (counterexample at depth 4).
- Isolation documented as structural (per-Proc function values; no shared mutable state at this phase).

### P2-Eb impl targets (next chunk)

- `kernel/include/thylacine/territory.h` — Territory struct (refcount + mount table).
- `kernel/territory.c` — `bind`, `unmount`, `territory_init`, `territory_clone`, lookup helpers.
- `struct Proc` extended with `struct Territory *territory`.
- `rfork` extended to call `territory_clone` for RFPROC (without RFNAMEG).
- Cycle-detection in `bind`: DFS over the existing bind graph.
- New tests: `territory.bind_smoke`, `territory.cycle_rejected`, `territory.fork_isolated`.

### Deferred

- **RFNAMEG (shared territory) at Phase 5+**: requires a separate Territory indirection layer in the spec (multiple Procs pointing at the same Territory). Isolation invariant becomes meaningful as a state predicate (Procs sharing a Territory see the same bindings; Procs with separate Territories don't). v1.0 P2-Eb extincts on RFNAMEG.
- **Walk determinism**: ARCH §9.1 lists "walk determinism" alongside cycle-freedom + isolation. The spec's binding graph is deterministic by construction (functional state); walk determinism is structurally satisfied. A separate state invariant could explicitly check "the lookup function is total + single-valued" — deferred.
- **Mount union semantics (MBEFORE / MAFTER / MREPL flags)**: the spec models `bindings[p][dst]` as a SET (no ordering). The impl's union-list ordering matters for lookup priority but doesn't affect cycle-freedom. Phase 5+ extension can refine this.
- **`mount` (9P-server-attaching variant)**: structurally identical to bind for the cycle-freedom + isolation invariants. Phase 4 (9P client) lands the impl.

## handles.tla — P2-Fa + P4-Ib + P4-Ic3 + P4-Ic5b1b spec (impl at P2-Fc + P4-Ib + P4-Ic3 + P4-Ic5b1b)

> **Currency note (RW-10, 2026-06-11).** Since this section was written:
> (a) **KOBJ_LOOM** landed as a FOURTH non-transferable partition
> (`handle.h:106-122` — anywhere below saying "three partitions"/"three
> asserts" is one short); (b) the **#844 handle-lifetime pass** rewrote the
> table internals (per-Proc table lock; `handle_get` returns a BY-VALUE
> snapshot with the obj refcount held, paired with `handle_put`) — the spec's
> abstract alloc/dup/transfer actions still map, but line numbers drifted;
> (c) any mention of a `handle_transfer_via_9p` `-ENOTSUP` stub is wrong —
> **no transfer codepath was ever built** (I-4 holds vacuously; the positive
> 9P-transfer path remains future work).

Status: **rights ceiling proven; hardware-handle non-transferability proven; transfer-only-via-9P proven; capability monotonicity proven; hw-handle non-duplicability proven; hw-resource exclusivity proven; hw-handle-implies-cap proven; rfork-capability-grant ceiling proven; elevation-only-cap axis + rfork-strip discipline proven (P5-hostowner); KObj_Srv non-transferability + walk-derivation proven (P5-corvus-srv); impl landed at P2-Fc + P4-Ib + P4-Ic3 + P4-Ic5b1b + P5-corvus-srv-impl-a1 (the `KObj_Srv` kind + its non-transferable partition; the `devsrv` walk-derivation lands in -impl-a3).** Models the kernel handle table with typed kobjs partitioned into transferable + hardware sets, per-handle provenance tags, abstract 9P sessions, per-Proc coarse capabilities, and dynamic per-Proc capability ceilings (rfork-time inheritance). ARCH §28 invariants pinned: I-2 (capability monotonic reduction), I-4 (transfer only via 9P), I-5 (hardware handles non-transferable — covers MMIO + IRQ + DMA), I-6 (rights monotonic on transfer). I-7 (BURROW refcount + mapping lifecycle) is OUT OF SCOPE — covered by `burrow.tla`.

P4-Ic5b1b adds no new spec state, actions, or invariants — the existing `HwKObjs` partition + the 5 hw-kobj invariants (HwHandlesAtOrigin, NoHwDup, HwResourceExclusive, HwHandleImpliesCap, RightsCeiling) apply uniformly to KObj_DMA. PA stability is a structural impl property (no kernel code path mutates KObj_DMA's `pa` after `kobj_dma_create`). The kernel-allocates-PA discipline (vs MMIO's user-specifies-PA) is a syscall-surface design choice that doesn't surface in the abstract handle-table model. See the `HandleAlloc` row below for the DMA-side impl cross-reference.

The main `handles.cfg` keeps `SrvKObjs = {}`, so the P5-corvus-srv additions (the `WalkDerive` / `BuggySrvWalkTx` actions never fire) leave its reachable state graph — and TLC verdict — unchanged: TLC-clean at `Procs = {p1, p2}, TxKObjs = {kx}, HwKObjs = {kh}, SrvKObjs = {}, Rights = {R, T}, Caps = {C, HW}, CapHwCreate = HW, CapHostowner = C` — **51,744,096 distinct states / depth 28; ~56 min on 8 cores**. P5-corvus-srv's `KObj_Srv` walk-derivation mechanism is proven in a focused clean cfg `handles_srv.cfg` (`SrvKObjs = {ks1, ks2}`, Tx/Hw partitions empty) — clean, **26,784 distinct / depth 12 / 2s**. Buggy configs:

| Config | Flag | Invariant violated | Depth | Distinct states |
|---|---|---|---|---|
| `handles_buggy_elevate.cfg`        | `BUGGY_ELEVATE = TRUE`           | `RightsCeiling`        | 4 | 75 |
| `handles_buggy_hw.cfg`             | `BUGGY_HW_TRANSFER = TRUE`       | `HwHandlesAtOrigin`    | 5 | 523 |
| `handles_buggy_direct.cfg`         | `BUGGY_DIRECT_TRANSFER = TRUE`   | `OnlyTransferVia9P`    | 5 | 73 |
| `handles_buggy_caps.cfg`           | `BUGGY_CAPS_ELEVATE = TRUE`      | `CapsCeiling`          | 4 | 80 |
| `handles_buggy_hw_dup.cfg`         | `BUGGY_HW_DUP = TRUE`            | `NoHwDup`              | 4 | 131 |
| `handles_buggy_hw_overlap.cfg`     | `BUGGY_HW_OVERLAP = TRUE`        | `HwResourceExclusive`  | 4 | 124 |
| `handles_buggy_hw_nocap.cfg`       | `BUGGY_HW_CREATE_NO_CAP = TRUE`  | `HwHandleImpliesCap`   | 4 | 169 |
| `handles_buggy_rfork_elevate.cfg`  | `BUGGY_RFORK_ELEVATE = TRUE`     | `CapsCeiling` (dynamic ceiling) | 5 | 534 |
| `handles_buggy_spawn_fds_elevate.cfg` | `BUGGY_SPAWN_FDS_ELEVATE = TRUE` | `SpawnFdsRightsMonotonic` | 3 | 133 |
| `handles_buggy_rfork_hostowner.cfg` | `BUGGY_RFORK_HOSTOWNER = TRUE` | `CapsCeiling` (elevation-only strip) | 5 | 553 |
| `handles_buggy_srv_walk_tx.cfg`     | `BUGGY_SRV_WALK_TX = TRUE`       | `WalkChildIsSrv`       | 3 | 330 |

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/handle.c::handle_init` (P2-Fc) + `kernel/proc.c::proc_init` (kproc.caps = CAP_ALL at line 199) | Per-Proc handle table allocated empty; kproc (= ProcRoot) starts with `caps = CAP_ALL` and `proc_ceiling = Caps`. Every other Proc starts with `caps = {}` and `proc_ceiling = {}` (via `KP_ZERO` in `proc_alloc`). |
| `HandleAlloc(p, k, granted)` | `kernel/handle.c::handle_alloc(struct Proc *, kobj_kind, kobj_t, rights_t)` (P2-Fc) + cap-gated entries in `kernel/syscall.c::sys_mmio_create_handler` / `sys_irq_create_handler` (P4-Ib) / `sys_dma_create_handler` (P4-Ic5b1b) | Kernel allocates a fresh kobj k of statically-typed kind, grants `granted` rights to proc p. Sets origin_rights[k] = granted permanently. The P4-Ib hw-kobj precondition `CapHwCreate \in proc_caps[p]` is enforced syscall-side via `__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE`. P4-Ic5b1b: the DMA-side `sys_dma_create_handler` allocates buddy pages via `alloc_pages` and wraps the resulting contiguous PA range in a `KObj_DMA` via `kobj_dma_create(size)` before calling `handle_alloc(p, KOBJ_DMA, rights, k)`. Distinct from MMIO: the PA is chosen by the kernel (not the caller), guaranteeing freshness; the buddy allocator's per-allocation partitioning is the structural enforcement of `HwResourceExclusive` for DMA kobjs (no shared global claim table needed). |
| `HandleClose(p, h)` | `kernel/handle.c::handle_close(struct Proc *, hidx_t)` (P2-Fc) | Releases h from p's table. Decrements kobj's refcount; cascades to burrow_unref / kobj_mmio_unref / kobj_irq_unref / kobj_dma_unref (P4-Ic5b1b) if last handle. |
| `HandleDup(p, h, new_rights)` | `kernel/handle.c::handle_dup(struct Proc *, hidx_t, rights_t new_rights)` (P2-Fc + P4-Ib hw reject) | Creates a fresh handle in p's table sharing h's kobj with rights ⊆ h.rights. Rejects elevated rights with -EINVAL; **rejects hw-kobj kinds entirely** (NoHwDup, P4-Ib). |
| `BuggyDupElevate(p, h, new_rights)` | (none — bug class statically prevented) | The impl's `handle_dup` checks `(new_rights & h->rights) == new_rights` (subset test) before insert. A future caller that bypasses this check would re-introduce the bug. |
| `OpenSession / CloseSession` | (Phase 4: `kernel/9p_client.c::9p_attach / 9p_clunk`) | At v1.0 P2-Fc the spec models sessions abstractly; no impl callers. Phase 4's 9P client wires actual session lifecycle. |
| `HandleTransferVia9P(src, dst, h, new_rights)` | (Phase 4: `kernel/handle.c::handle_transfer_via_9p`) | The transfer codepath is **defined** at P2-Fc as a stub (returns -ENOTSUP at v1.0); the policy gates (TxKObjs ∈ TransferableTypes, RightTransfer ∈ rights, session open, new_rights ⊆ rights) are coded against the spec actions. Phase 4 wires the actual 9P payload extraction. |
| `BuggyHwTransfer(src, dst, h, new_rights)` | (none — bug class statically prevented) | The transfer switch in §18.3 has NO case for KObj_MMIO/IRQ/DMA/Interrupt. `_Static_assert` over the kobj_kind enum ensures every kind is accounted for; a future addition that's transferable must be added explicitly. |
| `BuggyDirectTransfer(src, dst, h, new_rights)` | (none — bug class statically prevented) | No syscall exists in the impl that transfers a handle directly between procs. The only cross-proc handle path is via 9P (Phase 4). |
| `ReduceCaps(p, lost)` | (Phase 5+: future cap-drop syscall) | At v1.0 there is no in-kernel caller of capability reduction. The spec's precondition `CapHwCreate \in lost => no hw handles in p` (P4-Ib refinement) is forward-looking — the future syscall must enumerate `p->handles` and reject if any has `kobj_kind_is_hw(kind)` (R9 F150 implementer note in caps.h). |
| `BuggyCapsElevate(p, gained)` | (none — bug class statically prevented) | No impl syscall ever ORs in elevated bits to `p->caps`. Catch surface narrowed at P4-Ic3 to "bits beyond ceiling" (not "bits not currently held") — see F162 commentary in `handles.tla` near `BuggyCapsElevate`. |
| `BuggyHwDup(p, h, new_rights)` | (none — bug class statically prevented at P4-Ib) | `handle_dup` rejects when `h->obj->kind` is hw via the new `handle_kind_is_hw_kobj()` check. A future case that misses the hw kinds would re-introduce the bug. |
| `BuggyHwOverlap(p, k, granted)` | (none — bug class statically prevented at P4-Ib + P4-Ic2) | `kobj_mmio_create` rejects via `g_mmio_claims` overlap scan; `kobj_irq_create` rejects via `g_intid_claimed` bitmap. P4-Ic2 added kernel-MMIO-reservation (`kobj_mmio_reserve_kernel_ranges`) so userspace can't claim GIC/UART/ECAM/virtio-mmio either. |
| `BuggyHwCreateNoCap(p, k, granted)` | (none — bug class statically prevented at P4-Ib) | Syscall handlers gate on `__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE` before allocating. R9 F146 hardened the read to acquire-fence. |
| `RforkWithCaps(parent, child, granted)` | `kernel/proc.c::rfork_internal` (P4-Ic3, lines 469-538) + `rfork_with_caps` (lines 545-548) | Kernel-internal capability grant primitive. `child->caps = __atomic_load_n(&parent->caps, __ATOMIC_ACQUIRE) & caps_mask` enforces `granted \subseteq proc_caps[parent]` regardless of caller-supplied mask. The plain `rfork` is a delegate with `caps_mask = CAP_NONE`. **P5-hostowner**: the spec's `RforkWithCaps` now also strips `ElevationOnly` from the child's caps and ceiling (`granted \ ElevationOnly`); the impl strip — `& ~CAP_ELEVATION_ONLY` in `rfork_internal` — lands in P5-hostowner-b. |
| `BuggyRforkElevate(parent, child, gained)` | (none — bug class statically prevented at P4-Ic3) | The AND with `parent_caps` at line 499 of `rfork_internal` makes it impossible for the caller to elevate the child's caps above the parent's. A future refactor that replaces `&` with `=` or `\|` would re-introduce the bug. |
| `HostownerGrant(p)` | `kernel/devcap.c` use-file redemption (landed P5-hostowner-b; the A-4a clearance redeem shares the locked kind-branched core) | Models the `cap` device's `use`-file redemption: a console-attached Proc redeems a pending grant and the kernel ORs `CAP_HOSTOWNER` into its `caps` (the spec also raises `proc_ceiling`, modeling the legitimate authorization growth). corvus's passphrase + console gates are modeled in `corvus.tla::AdminElevate`; this action is the cap arithmetic only. The SOLE action admitting `CapHostowner` into any `proc_caps`. |
| `BuggyRforkNoStrip(parent, child, granted)` | (P5-hostowner-b — statically prevented once `rfork_internal` ANDs the child's caps with `~CAP_ELEVATION_ONLY`) | The bug: `rfork` omits the elevation-only strip, so an elevated parent leaks `CAP_HOSTOWNER` to a forked child that lacks `PROC_FLAG_CONSOLE_ATTACHED`. Caught by `CapsCeiling` — the child's ceiling is correctly stripped, its caps are not. |
| `WalkDerive(p, h, k)` | (P5-corvus-srv-impl-a3 — `kernel/devsrv.c` walk op) | Walking a `KObj_Srv` handle yields a fresh child Spoor; because `/srv` is its own Dev, the child kobj is structurally `KObj_Srv` (allocated in the `SrvKObjs` partition), via="walk", inheriting the parent handle's rights as its ceiling. |
| `BuggySrvWalkTx(p, h, k)` | (none — bug class prevented once `devsrv`'s walk op mints the child Spoor with the `KObj_Srv` kind) | The bug: the walk op constructs the child Spoor with a transferable kobj kind (or routes `/srv` through `dev9p`), so the child can 9P-transfer between Procs. Caught by `WalkChildIsSrv`. |

| Spec invariant | Source enforcement |
|---|---|
| `RightsCeiling` (every handle's rights ⊆ origin_rights of its kobj) | `handle_dup`'s subset check before insert; `handle_transfer_via_9p`'s subset check (Phase 4 wire-up). The kobj's origin_rights is set once at `handle_alloc` and never modified. |
| `HwHandlesAtOrigin` (every hw-typed handle held by origin proc) | `handle_transfer_via_9p`'s switch statement omits the hw kinds entirely. `_Static_assert(KIND_COUNT == ...)` ensures every kind is enumerated; missing case is a compile error. |
| `OnlyTransferVia9P` (no handle has via="direct") | The impl has no direct-transfer syscall. The only public handle-cross-proc API is `handle_transfer_via_9p`. |
| `CapsCeiling` (every proc's caps ⊆ proc_ceiling[p]) | **At P4-Ic3 this is dynamic**: ProcRoot's ceiling is `Caps` at Init; non-root Procs start with ceiling `{}` (via `KP_ZERO`) and have their ceiling set by `RforkWithCaps` to `proc_caps[parent]` at fork time. Impl-side, `rfork_internal`'s AND-with-parent_caps clamps the child's caps to its newly-established ceiling. P5-hostowner adds one sanctioned post-fork growth — `HostownerGrant` (the `cap` device) — which raises `proc_caps[p]` AND `proc_ceiling[p]` together for the same Proc, so `CapsCeiling` still holds. `CapsCeiling` now also catches `BuggyRforkNoStrip`: an rfork that fails to strip the elevation-only cap leaves the child holding `CapHostowner` in its caps while its (correctly stripped) ceiling does not. |
| `NoHwDup` (no hw-kobj handle has via="dup") | `handle_dup` rejects when `h->obj->kind` is hw-kobj-kind via `handle_kind_is_hw_kobj()`. P4-Ib. |
| `HwResourceExclusive` (at most one alive handle per hw kobj across all procs) | `kobj_mmio_create`'s `g_mmio_claims` overlap scan + `kobj_irq_create`'s `g_intid_claimed` bitmap. P4-Ib + P4-Ic2 kernel-MMIO-reservation. **DMA case (P4-Ic5b1b)**: `kobj_dma_create` calls `alloc_pages` which returns a fresh contiguous page chunk per allocation; the buddy allocator's free-list partitioning structurally prevents two live `KObj_DMA` from sharing PA. No explicit claim table is needed because the page allocator IS the claim layer. |
| `HwHandleImpliesCap` (every hw-handle holder has CAP_HW_CREATE) | Syscall handlers gate on `__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE` before allocating. `ReduceCaps` (spec; future syscall) refuses to drop CAP_HW_CREATE while holding hw handles (R9 F150 implementer note). |
| `WalkChildIsSrv` (every via="walk" handle points at a `KObj_Srv` kobj) | `devsrv`'s walk op allocates the child Spoor with the `KObj_Srv` kind; `/srv` being its own Dev makes this structural. Pins CORVUS-DESIGN.md C-22 (the connection's kernel-stamped identity is unforgeable across a 9P walk). P5-corvus-srv-impl-a3 (the `devsrv` walk op). |
| `SrvHandlesAtOrigin` (every `KObj_Srv` handle held by its origin proc) | `KObj_Srv` is non-transferable *structurally*: absent from `KOBJ_KIND_TRANSFERABLE_MASK` (so `kobj_kind_is_transferable(KOBJ_SRV)` is false), placed in the third partition `KOBJ_KIND_SRV_MASK`; three pairwise-disjoint + one completeness `_Static_assert` over the kobj_kind masks pin the partition. `handle_dup` rejects it via `!kobj_kind_is_transferable` (NoSrvDup). Any cross-Proc transfer path gates on `kobj_kind_is_transferable`, which excludes it. **Landed P5-corvus-srv-impl-a1** (the `KObj_Srv` kind + the masks + the `handle_dup` rejection). |

### P2-Fa landed

- `handles.tla` (~530 LOC TLA+) + 5 cfg files (1 clean + 4 buggy variants).
- Kobj universe partitioned into `TxKObjs` ∪ `HwKObjs` (set-based, no function-valued constants — clean for TLC).
- Provenance tags `via \in {"orig", "dup", "9p", "direct"}` capture handle origin; only the buggy variant produces "direct".
- Per-handle `origin_proc` field tracks where a handle was first granted; preserved by all transfer paths.
- Per-kobj `origin_rights` ceiling set at `HandleAlloc` and never modified.
- Per-Proc `proc_caps` modeled with ProcRoot ceiling = Caps and other Procs' ceiling = {}; `BuggyCapsElevate` violates only on non-root procs.
- Abstract 9P session set; OpenSession/CloseSession lifecycle.
- Five state invariants: `TypeOk`, `RightsCeiling`, `HwHandlesAtOrigin`, `OnlyTransferVia9P`, `CapsCeiling`.

### P2-Fc impl targets (next chunk)

- `kernel/include/thylacine/handle.h` — Handle struct (kobj_kind + rights + kobj pointer).
- `kernel/handle.c` — `handle_init`, `handle_alloc`, `handle_close`, `handle_dup`, `handle_get`. Per-Proc fixed-size handle table at v1.0 (growable Phase 5+).
- `struct Proc` extended with `struct HandleTable handles`.
- New tests: `handles.alloc_close_smoke`, `handles.rights_monotonic`, `handles.dup_elevate_rejected`, `handles.full_table_oom`.
- `_Static_assert(KIND_COUNT == ...)` over the kobj_kind enum to pin the type set at compile time.

### Deferred

- **9P session lifecycle (Phase 4)**: spec's OpenSession/CloseSession actions abstract the actual `9p_attach` / `9p_clunk` impl. P2-Fc's `handle_transfer_via_9p` is a stub returning -ENOTSUP; Phase 4 wires the wire-format payload.
- **rfork capability mask (Phase 5+)**: `proc_caps` and ReduceCaps actions are forward-looking. v1.0 rfork only supports RFPROC; capability mask via syscall surface in Phase 5+.
- **Per-Proc handle-table growth (Phase 5+)**: v1.0 P2-Fc has a fixed-size table (PROC_HANDLE_MAX); Phase 5+ refactors to growable. The spec is agnostic to the table representation — handles[p] is just a set.
- **Cross-Proc handle accounting under transfer (Phase 4)**: the spec's HandleTransferVia9P creates a fresh dst handle without affecting src; impl-side, the kobj's refcount is incremented per fresh handle. Phase 4 audit verifies the refcount math.

## burrow.tla — P2-Fb spec (impl at P2-Fd; finalized at P4-N)

Status: **BURROW refcount + mapping lifecycle proven across ANON + MMIO + DMA; impl finalized at P4-N (R13 audit close).** Models the dual-refcount discipline for Virtual Memory Objects per ARCH §19. Pins ARCH §28 invariant I-7 (pages live until last handle closed AND last mapping unmapped). The dual-count semantics are type-independent; the spec models the abstract domain that all three backing types (ANON / MMIO / DMA) share — each type's distinct backing-resource discipline (alloc_pages / KObj_MMIO / KObj_DMA) is impl-level and converges on the same NoUseAfterFree invariant at the spec level.

TLC-clean at `VmoIds = {v1, v2}, MaxRefs = 2` — 100 distinct states explored; depth 9. Buggy configs (state counts re-verified at P4-N with TLC2 v1.8.0):

| Config | Flag | Invariant violated | Distinct states |
|---|---|---|---|
| `burrow_buggy_free_on_close.cfg` | `BUGGY_FREE_ON_HANDLE_CLOSE = TRUE` | `NoUseAfterFree` (premature) | 66 |
| `burrow_buggy_free_on_unmap.cfg` | `BUGGY_FREE_ON_UNMAP = TRUE`        | `NoUseAfterFree` (premature) | 54 |
| `burrow_buggy_never_free.cfg`    | `BUGGY_NEVER_FREE = TRUE`           | `NoUseAfterFree` (delayed)   | 43 |

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/burrow.c::burrow_init` (P2-Fd) | SLUB cache for struct Burrow. No VMOs alive at boot. |
| `VmoCreate(v)` | `kernel/burrow.c::burrow_create_anon(size)` (P2-Fd) <br> `kernel/burrow.c::burrow_create_mmio(struct KObj_MMIO *)` (P4-Ic1) <br> `kernel/burrow.c::burrow_create_dma(struct KObj_DMA *)` (P4-Ic5b1b) | Three type-specific constructors; all set `handle_count = 1` + `mapping_count = 0` + `magic = VMO_MAGIC`. ANON allocates buddy pages; MMIO + DMA each `kobj_*_ref` the underlying hw-backed kobj for the Burrow's lifetime. Returns the new BURROW; the caller's `handle_alloc` wraps it in a Handle. |
| `HandleOpen(v)` | `kernel/burrow.c::burrow_ref(struct Burrow *)` (P2-Fd) | Increments handle_count. Called from `handle_acquire_obj` + `handle_dup` (intra-Proc). Type-agnostic. Single-CPU at v1.0 (Phase 5+ adds atomics per `handles.tla`'s ConcurrencyExtension). |
| `HandleClose(v)` | `kernel/burrow.c::burrow_unref(struct Burrow *)` (P2-Fd) | Decrements handle_count. If both counts reach 0, calls `burrow_free_internal` which type-dispatches: ANON → `free_pages`; MMIO → `kobj_mmio_unref`; DMA → `kobj_dma_unref`. Clobbers `magic = 0` before `kmem_cache_free` (R9 F148 discipline; closed at R13 F213 / P4-N). |
| `BuggyFreeOnHandleClose(v)` | (none — bug class statically prevented) | The impl's `burrow_unref` checks BOTH counts before calling `burrow_free_internal`: `if (h_count == 0 && m_count == 0)`. `burrow_free_internal` additionally asserts both counts == 0 on entry (extincts on violation). A future caller that bypasses the check fails fast at the inner assertion. |
| `MapVmo(v)` | `kernel/burrow.c::burrow_acquire_mapping(struct Burrow *)` (P3-Db) | Increments mapping_count. Called from `vma_alloc` (which is called by `burrow_map` — the high-level entry from `SYS_MMIO_MAP` / `SYS_DMA_MAP` / `exec.c`). Type-dispatched liveness check ensures the backing resource (pages / kobj_mmio / kobj_dma) is still alive. |
| `UnmapVmo(v)` | `kernel/burrow.c::burrow_release_mapping(struct Burrow *)` (P3-Db) | Decrements mapping_count. Called from `vma_free`. Same dual-check on free as `burrow_unref`. |
| `BuggyFreeOnUnmap(v)` | (none — bug class statically prevented) | Same dual-check as BuggyFreeOnHandleClose; structural guarantee. |
| `BuggyNoFreeHandleClose(v) / BuggyNoFreeUnmap(v)` | (none — bug class statically prevented) | The impl's `burrow_unref` and `burrow_release_mapping` always evaluate the dual-check at decrement time; no codepath skips the free transition when both counts reach 0. |

| Spec invariant | Source enforcement |
|---|---|
| `RefcountConsistent` (counts = 0 if not in vmos) | The impl's struct Burrow is allocated/freed via SLUB; counts are part of the struct. Before allocation: no struct Burrow exists, so counts are not addressable; "not in vmos" maps to "no Burrow struct allocated yet." After `burrow_free_internal` (counts reach 0 + magic clobbered), subsequent `burrow_ref` / `burrow_acquire_mapping` extincts via the magic check (`v->magic != VMO_MAGIC`), pinning the "freed Burrows must not be referenced" half of `RefcountConsistent`. |
| `NoUseAfterFree` (pages alive iff at least one count > 0) | `burrow_unref` and `burrow_release_mapping` both check `(handle_count == 0 AND mapping_count == 0)` after their decrement; if true, free via `burrow_free_internal`. The dual check is the runtime enforcement of I-7. `burrow_free_internal` additionally asserts both counts == 0 at entry — defense-in-depth against a hypothetical bypassing caller. The type-dispatched switch in `burrow_free_internal` releases the per-type backing resource (free_pages / kobj_mmio_unref / kobj_dma_unref) symmetrically with `burrow_create_*`'s ref-acquire. |

### Files

- `specs/burrow.tla` (~310 LOC TLA+) + 4 cfg files (1 clean + 3 buggy variants).
- `kernel/include/thylacine/burrow.h` — `struct Burrow` (magic + type + size + page_count + handle_count + mapping_count + pages + order + kobj_mmio + kobj_dma + pa); `enum burrow_type` (ANON / MMIO / DMA / INVALID).
- `kernel/burrow.c` — `burrow_init`, `burrow_create_anon`, `burrow_create_mmio`, `burrow_create_dma`, `burrow_ref`, `burrow_unref`, `burrow_acquire_mapping`, `burrow_release_mapping`, `burrow_map`, `burrow_unmap`, `burrow_free_internal` (static; clobbers magic before `kmem_cache_free`), diagnostic accessors.
- `kernel/test/test_burrow.c` + `kernel/test/test_burrow_mmio.c` + `kernel/test/test_burrow_map_proc.c` — 30+ tests covering refcount lifecycle, map/unmap, dual-count interleavings, magic-check rejection paths, MMIO/DMA type dispatch.

### Deferred / Phase 5+

- **File-backed VMOs (VMO_FILE) post-v1.0**: integration with Stratum's page cache. Spec extension: VmoCreate variants per backing type; refcount mechanics unchanged.
- **Concurrent map/unmap and ref/unref (Phase 5+)**: at v1.0 the impl runs effectively single-CPU per Proc; Phase 5+ adds a per-Burrow lock OR atomic-only operations. The spec is single-stepping (atomic transitions); Phase 5+ refinement may need finer-grained atomicity modeling. Cross-reference: `handles.tla`'s ConcurrencyExtension section.



## 9p_client.tla — P5-spec (Phase 5 entry; spec-first)

> **Currency note (RW-10, 2026-06-11).** Three impl arcs moved under this
> section after it was written: **#841** (the elected-reader restoration —
> lock never held across recv; multi-in-flight tag demux; `be_reader`
> election + hand-off; ARCH §21.10), **#845** (Tflush-on-abandon — the
> `awaiting_flush` tag state sits DIRECTLY on the modeled I-10 surface: an
> abandoned tag is not reusable until its Rflush), and **Loom-2b** (the
> pluggable-completion seam — `p9_rpc.on_complete` WAKE_RENDEZ vs POST_CQE).
> The spec's tag-uniqueness + fid-lifecycle invariants still hold over the
> as-built (the #841/#845 audits + the 4 buggy cfgs are the evidence); the
> action↔site rows below pre-date the restructure and their line numbers
> drifted.

Pins ARCH §28 invariants:

- **I-10** — Per-9P-session tag uniqueness.
- **I-11** — Per-9P-session fid identity is stable for fid's open lifetime.

Plus two composition-layer properties from ROADMAP §7 + Stratum's
`OS-INTEGRATION.md` §3 (pipelined client correctness):

- **OutOfOrderCorrectness** — Rmessages match Tmessages by tag, not by
  arrival order. Captured via the `TagAndOpAccounting` invariant: the
  set of op_ids in `outstanding[]` equals `sent_ops \ completed_ops`.
  A misordered receive (BuggyOOOReceive) breaks the bijection.
- **FlowControl** — Outstanding-request cardinality bounded by
  `MaxWindow`. Back-pressure surfaces as Send-side blocking
  (precondition refusal), never as silent drop. Captured via
  `BoundedOutstanding`.

### Bug classes covered (executable counterexamples)

| Buggy cfg | Bug class | Invariant that catches |
|---|---|---|
| `9p_client_buggy_tag_collision.cfg` | Tag reuse before Rmsg arrival (alloc_tag returns in-use tag) | `TagAndOpAccounting` |
| `9p_client_buggy_fid_after_clunk.cfg` | IO on fid not in `bound_fids` (use after Tclunk) | `FidStability` (I-11) |
| `9p_client_buggy_ooo_match.cfg` | Rmsg with tag T paired with `outstanding[fake_t]`'s op (wrong-tag mis-match) | `TagAndOpAccounting` |
| `9p_client_buggy_unbounded.cfg` | Send fires past `MaxWindow` (no back-pressure) | `BoundedOutstanding` |

### TLC posture

| cfg | Verdict | States generated / distinct / depth |
|---|---|---|
| `9p_client.cfg` (correct) | **Model checking completed. No error has been found.** | 462 / 197 / 9 |
| `9p_client_buggy_tag_collision.cfg` | Invariant violated (expected) | 50 / 42 / depth 5 |
| `9p_client_buggy_fid_after_clunk.cfg` | Invariant violated (expected) | 68 / 52 / depth 6 |
| `9p_client_buggy_ooo_match.cfg` | Invariant violated (expected) | 60 / 45 / depth 5 |
| `9p_client_buggy_unbounded.cfg` | Invariant violated (expected) | 64 / 47 / depth 5 |

Bounded model: `TagIds = {t1, t2, t3}` (3 tags); `FidIds = {root, f1}`
(2 fids); `RootFid = root`; `MaxWindow = 2`; `MaxOps = 3`. State space is
small (<200 distinct) but every bug class reaches its violation in ≤ 6 steps
— the spec's invariants catch each bug at the buggy-action step.

### State machine

| Spec action | Wire-level event | Impl |
|---|---|---|
| `OpenSession` | `Tversion` + `Tattach` (handshake) | `kernel/9p_session.c::p9_session_send_version` + `p9_session_send_attach` + `p9_session_dispatch_rmsg` (Rversion → VERSIONED; Rattach → OPEN + bind root_fid). **Landed at P5-session.** |
| `CloseSession` | connection close + per-Proc cleanup | `kernel/9p_session.c::p9_session_close` (refuses while Inflight ≠ {}). **Landed at P5-session.** |
| `SendIO(t, fid)` — open / create | Send `Tlopen` / `Tlcreate` on tag `t` for `fid` (server-side fid state mutation; exclusive at the wire layer) | `kernel/9p_session.c::p9_session_send_lopen` + `p9_session_send_lcreate` (use `p9_build_tlopen` / `p9_build_tlcreate`). Send-time precondition: fid bound + no other in-flight op on fid. Dispatch handlers in `p9_session_dispatch_rmsg` for `op->kind ∈ {TLOPEN, TLCREATE}` populate `out->open_qid` + `out->open_iounit`. **Landed at P5-wire-io.** |
| `SendIO(t, fid)` — read / write | Send `Tread(offset, count)` / `Twrite(offset, count, data)` on tag `t` for `fid` (explicit-offset; concurrent ops permitted) | `kernel/9p_session.c::p9_session_send_read` + `p9_session_send_write` (use `p9_build_tread` / `p9_build_twrite`). Send-time precondition: fid bound. Dispatch handlers for `op->kind ∈ {TREAD, TWRITE}`; Tread surfaces zero-copy `out->read_data` + `out->read_count` (R111 caller-cap-bound applied against `s->negotiated_msize - 11`); Twrite surfaces `out->write_count`. **Landed at P5-wire-io.** |
| `SendIO(t, fid)` — getattr / statfs / readdir / fsync | Send `Tgetattr(request_mask)` / `Tstatfs` / `Treaddir(offset, count)` / `Tfsync(datasync)` (read-shaped; concurrent ops permitted) | `kernel/9p_session.c::p9_session_send_getattr` / `_statfs` / `_readdir` / `_fsync` (use the matching `p9_build_*`). Send-time precondition: fid bound. Dispatch handlers populate `out->attr` (Rgetattr, full statx record), `out->statfs` (Rstatfs), `out->readdir_count` + `out->readdir_data` zero-copy (Rreaddir, R111-bounded), or no payload (Rfsync). **Landed at P5-wire-meta.** |
| `SendIO(t, fid)` — setattr | Send `Tsetattr(valid, mode, uid, gid, size, atime, mtime)` on tag `t` for `fid` (mutation-shaped; fid-exclusive) | `kernel/9p_session.c::p9_session_send_setattr` (uses `p9_build_tsetattr`). Send-time precondition: fid bound + no other in-flight op on fid. Dispatcher: Rsetattr is body-empty header validation. **Landed at P5-wire-meta.** |
| `SendIO(t, fid)` — symlink / mknod / link / mkdir / unlinkat / renameat | Send `Tsymlink` / `Tmknod` / `Tlink` / `Tmkdir` / `Tunlinkat` / `Trenameat` (read-or-mutation-shaped; concurrent ops on same dfid permitted) | `kernel/9p_session.c::p9_session_send_symlink` / `_mknod` / `_link` / `_mkdir` / `_unlinkat` / `_renameat` (use the matching `p9_build_*`). Send-time precondition: dfid bound (+ fid bound for link). Dispatcher: qid-shape Rmsgs populate `out->created_qid`; empty-body Rmsgs are header-only validation. **Landed at P5-wire-mutation.** |
| `SendIO(t, fid)` — readlink | Send `Treadlink` on tag `t` for `fid` (read-shaped; concurrent permitted) | `kernel/9p_session.c::p9_session_send_readlink` (uses `p9_build_treadlink`). Send-time precondition: fid bound. Dispatcher: surfaces zero-copy `out->readlink_target` + `out->readlink_target_len`. **Landed at P5-wire-mutation.** |
| `SendIO(t, fid)` — rename | Send `Trename(fid, dfid, name)` on tag `t` (mutation-shaped; fid-exclusive — server-side identity moves) | `kernel/9p_session.c::p9_session_send_rename` (uses `p9_build_trename`). Send-time precondition: fid + dfid bound + no other in-flight op on fid. Dispatcher: Rrename is body-empty header validation. **Landed at P5-wire-mutation.** |
| `SendIO(t, fid)` — other families | `Tlock` / `Tsync` / `Treflink` / `Tfallocate` / `Tfadvise` / xattr family on tag `t` for `fid` | Wire builders deferred to P5-wire-lock / -xattr / -stratum-ext; session-side dispatch handlers extend `p9_session_dispatch_rmsg` at each landing. |
| `SendWalk(t, src, new)` | Send `Twalk(fid=src, newfid=new, n_names=N)` on tag `t` | `kernel/9p_session.c::p9_session_send_walk` (uses `p9_build_twalk`). Send-time precondition: src bound, new not bound, new ≠ root, no other in-flight op on new. **Landed at P5-session.** |
| `SendClunk(t, fid)` | Send `Tclunk(fid)` on tag `t`; Send-time unbind | `kernel/9p_session.c::p9_session_send_clunk` (uses `p9_build_tclunk`). Send-time-unbinds fid from `bound_fids` BEFORE storing the outstanding entry. Send-time precondition: fid bound, fid ≠ root, no other in-flight op on fid. **Landed at P5-session.** |
| `ReceiveOp(t)` | Receive `Rmsg.tag == t`; apply mutation per stored op kind | `kernel/9p_session.c::p9_session_dispatch_rmsg` (uses `p9_peek_header` + per-type `p9_parse_*`). Tag-indexed lookup pairs Rmsg with the correct outstanding op; Rlerror handled as a generic error response without fid mutation; type-mismatch rejected. **Landed at P5-session.** |

### Modeling abstractions

- One 9P session modeled. Cross-session isolation is the server's
  responsibility (Stratum's `namespace.tla` + `fid.tla`); Thylacine's
  client has nothing to enforce there.
- Op kinds collapsed to `{walk, clunk, io}`. The 9P2000.L baseline ops
  (`Tlopen`, `Tlcreate`, `Tsymlink`, `Tmknod`, `Trename`, `Treaddir`,
  `Tstatfs`, `Tgetattr`, `Tsetattr`, `Treadlink`, `Tlock`, `Tgetlock`,
  `Tlink`, `Tmkdir`, `Trenameat`, `Tunlinkat`) plus Stratum extensions
  (`Tsync`, `Treflink`, `Tbind`/`Tunbind`, `Txattrwalk` family) all
  collapse into the `io` kind for spec purposes — the invariants don't
  depend on per-op semantics.
- `SendClunk` requires no other in-flight op on the same fid (canonical
  client discipline). Tclunk's Send-time unbind models the client's
  intent: no further ops on this fid even while the Rmsg is in flight.
- Tracking per-op `op_id` (monotonic, distinct across all sends) lets
  the spec's invariants distinguish two ops on the same fid + tag-slot
  reuse pattern.

### Files

| Spec | File |
|---|---|
| Module | `specs/9p_client.tla` |
| Correct cfg | `specs/9p_client.cfg` |
| Buggy: tag collision | `specs/9p_client_buggy_tag_collision.cfg` |
| Buggy: fid after clunk | `specs/9p_client_buggy_fid_after_clunk.cfg` |
| Buggy: out-of-order match | `specs/9p_client_buggy_ooo_match.cfg` |
| Buggy: unbounded outstanding | `specs/9p_client_buggy_unbounded.cfg` |

### Phase 5 impl progress

- ✅ **P5-wire** (landed `1aa4826` / `42e87ed`) — `kernel/9p_wire.c` 9P2000.L codec (handshake + navigation + clunk bring-up subset). 15 unit tests.
- ✅ **P5-session** (landed `16d6c80` / `a8eddd0`) — `kernel/9p_session.c` state machine: tag pool (bitmap over `outstanding[64]`), fid table (linear array, swap-with-last unbind), state-machine INIT→VERSIONED→OPEN→CLOSED, Send-side preconditions enforcing every spec discipline, Receive-side tag-indexed dispatch with type-match validation + Rlerror surfacing. 17 unit tests covering bug-class shapes that match the spec's 4 buggy cfgs (TagCollision attacked indirectly via flow-control; FidAfterClunk + OOO-match + UnboundedOutstanding directly).
- ✅ **P5-wire-io** (landed `3aa508c` / `f06fe12`) — codec extension: Tlopen/Rlopen, Tlcreate/Rlcreate, Tread/Rread, Twrite/Rwrite. Session-level send APIs (`p9_session_send_lopen` / `_lcreate` / `_read` / `_write`) with fid-exclusivity rules (lopen/lcreate exclusive; read/write concurrent-permitted). R111 caller-cap-bound applied at `p9_parse_rread`. 15 new unit tests (7 wire + 8 session); test count 276 → 291.
- ✅ **P5-wire-meta** (landed `c95b194` / `c44970d`) — codec extension: Tgetattr/Rgetattr (statx-shape), Tsetattr/Rsetattr, Treaddir/Rreaddir, Tstatfs/Rstatfs, Tfsync/Rfsync. New structs `p9_attr`, `p9_setattr`, `p9_statfs`. New `p9_unpack_dirent` for consumer-side dirent-stream traversal. Session-level send APIs with fid-exclusivity rules (getattr/statfs/readdir/fsync concurrent-permitted; setattr exclusive). R111 caller-cap-bound applied at `p9_parse_rreaddir`. 15 new unit tests (7 wire + 8 session); test count 291 → 306.
- ✅ **P5-transport** (landed `3f7c0e2` / `6a81bb7`) — `kernel/9p_transport.c` + `kernel/9p_transport_loopback.c`: frame-aware byte pipe + in-memory loopback backend + session composition helper. Vtable-driven; backends provide raw read/write semantics; transport core handles framing + partial-read aggregation. State machine INIT → OPEN → CLOSED + ERROR sink. `p9_transport_exchange` ties session.send + transport.send + transport.recv + session.dispatch into one call. **No new spec module** — the transport's invariants (FramingIntegrity, RequestResponseOrdering, NoMessageLoss) are mechanical at the request-response synchronous level; session-level invariants (`specs/9p_client.tla` I-10, I-11, FlowControl, OutOfOrderCorrectness) compose unchanged. When the first async backend lands (Phase 5+), the spec may be extended with connection states + multi-message-in-flight ordering. 9 new unit tests (lifecycle / round-trip / framing rejection / partial-read aggregation / backend-error transition / close idempotency / end-to-end session handshake / end-to-end session walk); test count 306 → 315.
- ✅ **P5-wire-mutation** (landed `c7daae7` / `97cc73f`) — codec extension completing the standard 9P2000.L surface: Tsymlink/Rsymlink, Tmknod/Rmknod, Trename/Rrename, Treadlink/Rreadlink, Tlink/Rlink, Tmkdir/Rmkdir, Trenameat/Rrenameat, Tunlinkat/Runlinkat. Three response shapes (empty / qid-only / string) share `parse_empty_body` + `parse_qid_body` helpers. Session-level send APIs with fid-exclusivity rules (Trename exclusive; other 7 concurrent-permitted). struct p9_dispatch_result extended with `created_qid` (symlink/mknod/mkdir) + `readlink_target` zero-copy. 19 new unit tests (8 wire + 11 session); test count 315 → 334. **No spec extension** — mutation ops collapse to SendIO at spec level.
- ✅ **P5-client** (landed `fc4f2ce` / `e1648ef`) — `kernel/9p_client.c` + header: 25 high-level op wrappers (lifecycle + handshake + path + IO + metadata + mutation) consolidating wire + session + transport into one callable surface. Each wrapper: `session.send → transport.exchange → result extraction → error mapping`. Signed-errno convention (0 / -EINVAL / -EBUSY / -EIO / -ecode). Internal field-by-field copy helpers replace implicit memcpy (kernel doesn't link libc). 13 new unit tests; test count 334 → 347. **No spec extension** — pure composition over spec'd layers.
- ✅ **P5-attach-dev** (landed `f7b2d6c` / `7786bf0`) — `kernel/dev9p.{c,h}`: Dev vtable proxying to the kernel 9P client; the bridge that realizes ARCH §9.6 "filesystem-as-Spoor". Walk → `p9_client_alloc_fid` + `p9_client_walk`; open / read / write / close all route through the client. Per-Spoor state in `Spoor->aux` is a `struct dev9p_priv { magic, client *, fid, fid_owned }`. Root Spoors don't own root_fid; walk-derived Spoors do. New `p9_client_alloc_fid` fid allocator on the client API. 9 new unit tests; test count 347 → 356. **No spec extension** — pure composition.
- ✅ **P5-attach-create** (landed `fa31e51` / `0e1a339`) — `kernel/9p_attach.{c,h}`: kernel-internal mount creation primitive; the body of the future `attach_9p` syscall. `struct p9_attached` heap-allocates the p9_client + recv_buf; create runs Tversion + Tattach; destroy clunks root_fid + tears down. Used by tests + the future P5-stratumd boot path. 4 new unit tests; test count 356 → 360. **No spec extension** — pure composition.
- ✅ **P5-attach-mount** (landed this chunk) — `kernel/territory.c` Territory mount table + `mount` / `unmount` kernel primitives + `specs/territory.tla` extension. Per ARCH §9.6.6: every filesystem entity is a Spoor; `mount` grafts one in a Territory holding one refcount per entry; `unmount` removes one entry. Spec extension adds Mount / Unmount / BuggyMountNoRefbump / BuggyUnmountNoRefdrop / BuggyDestroyLeak actions over `mounts: [Procs -> SUBSET (Paths X Spoors)]` + `refcount: [Spoors -> Nat]`; `MountRefcountConsistency` is the global invariant. 1 clean cfg + 4 buggy cfgs (cycle / mount-no-refbump / unmount-no-refdrop / destroy-leak); TLC verdicts all match expected. C-side `struct PgrpMount` (16 B) + extended Territory (80 B → 216 B); rename `unmount(territory, src, dst) → unbind(territory, src, dst)` to free the verb for the mount-table primitive. `territory_clone` deep-copies mounts + `spoor_ref` per entry; `territory_unref` final-release drops per-entry refs before kmem_cache_free. 7 new unit tests; test count 360 → 367. **User-visible `mount` syscall is DEFERRED** to P5-fd-syscalls — the kernel-internal C API is the v1.0 deliverable; ARCH §9.6 mapping is unchanged.
- ✅ **P5-attach-syscall** (landed this chunk) — `SYS_ATTACH_9P = 13` SVC handler in kernel/syscall.c. Takes tx_fd + rx_fd KOBJ_SPOOR pair + aname; drives Tversion + Tattach handshake; returns a KOBJ_SPOOR fd for the 9P tree's root. dev9p_priv extended with `attached_owner` + `adapter_to_free`; dev9p_close on a Spoor with attached_owner set tears down the entire attach session (destroy attached + unref transport spoors + kfree adapter). Userspace libt stub `t_attach_9p` matches kernel ABI. Kernel-internal rejection-path test covers handle_get pre-checks; happy-path integration test deferred to P5-attach-probe (needs a 9P-responder companion). **No spec extension** — pure composition over specs/9p_client.tla + specs/handles.tla + specs/pipe.tla.
- ✅ **P5-mount-syscall** (landed `bb778e6` / `fb4c2cd`) — `SYS_MOUNT = 14` + `SYS_UNMOUNT = 15` SVC handlers in `kernel/syscall.c`. Thin wrappers over `kernel/territory.c::mount` / `::unmount`. ABI: `SYS_MOUNT(source_spoor_fd, target_path_id, flags) → 0/-1`; `SYS_UNMOUNT(target_path_id) → 0/-1`. target_path_id is `path_id_t` (u32) at v1.0; string-path resolution waits on the future walk subsystem. Rights gate: source handle needs RIGHT_READ. Companion fix: `kernel/spoor.c::spoor_clunk` refactored to Plan 9 cclose semantics (dev->close runs ONLY when ref hits 0), with 5 site-changes in territory.c + dev9p.c switching `spoor_unref` → `spoor_clunk` at last-drop sites. Userspace libt stubs `t_mount` + `t_unmount` + flag macros. 9 new kernel-internal tests including the end-to-end mount-after-close lifecycle. **No spec extension** — `specs/territory.tla::MountRefcountConsistency` is preserved structurally; the spoor_clunk fix is an implementation refinement orthogonal to the spec.
- ✅ **P5-attach-probe** (landed this chunk) — userspace integration test of SYS_ATTACH_9P + SYS_MOUNT + SYS_UNMOUNT through a real EL0 Proc against a kernel-thread 9P responder. First Phase 5 test driving the full SVC dispatch + wire codec + dev9p + spoor-transport + pipe stack without loopback shortcuts. `/attach-probe` ELF binary (`usr/attach-probe/attach-probe.c`) + responder kthread + test framework (`kernel/test/test_attach_probe.c`). Probe expects fd 0 (tx) + fd 1 (rx) pre-installed in its handle table; responder owns the other ends of the two pipe pairs; loop reads framed Tmsgs → builds Rmsgs → writes back. Test asserts ≥ 2 messages handled (Tversion + Tattach) — Tclunk-on-root_fid rejected at session layer by design (specs/9p_client.tla::SendClunk precondition; root fids are session-scoped). 410/410 PASS × default + UBSan. Two-userspace-Proc design deferred until rfork(RFFDG) lands. **No spec extension** — pure composition.
- ✅ **P5-spoor-transport** (landed `0f45fc6` / `28b5cbb`) — `kernel/9p_spoor_transport.{c,h}`: adapter wrapping a Spoor pair into `struct p9_transport_ops`. send routes through `tx_spoor->dev->write` (loops on short writes); recv routes through `rx_spoor->dev->read` (single call; transport core aggregates partial frames); close clunks both Spoors iff `owns_spoors`. tx and rx may be the same Spoor (duplex) or distinct (pipe model). 9 unit tests including full end-to-end Tversion + Tattach composition through real Spoor I/O — until this chunk, every 9P test ran through the loopback-fn shortcut. **No spec extension** — pure plumbing.
- ✅ **P5-pipe** (landed this chunk) — `kernel/pipe.{c,h}`: Plan 9 `pipe(fd[2])` primitive at the kernel layer. Connected Spoor pair over a shared 4 KiB ring buffer with FIFO byte semantics. Non-blocking at v1.0 (read returns 0 on empty; write returns 0 on full; wrong-end → -1). Ring shared across two Spoors with per-endpoint priv; ring freed when both Spoors clunked. Production backend for P5-spoor-transport — the canonical e2e test runs full Tversion + Tattach handshake through two pipe pairs wired into a `p9_spoor_transport` adapter. dc='\|' matches Plan 9 9front + shell pipe glyph. **No spec extension** — ring correctness is local + structural; the missed-wakeup hazard (I-9) enters scope at the blocking variant. 10 new unit tests.
- ✅ **P5-pipe-blocking** (landed `431db3a` / `92be8b6`) — `specs/pipe.tla` + extended `kernel/pipe.c`. Spec models the wait/wake protocol with 8 clean actions (ReadDrain/ReadEof/ReadSleep/WriteAppend/WriteEpipe/WriteSleep/CloseWrite/CloseRead) + 4 buggy variants that elide the wake-after-mutation step. Invariants `NoStuckReader` + `NoStuckWriter` specialize ARCH §28 I-9 to the pipe's two-direction state machine. Composes with `specs/scheduler.tla::NoMissedWakeup` (atomic cond-check + sleep transition at the rendez API surface). C-side: per-pipe spin lock + 2 rendez wait queues + 2 EOF flags. devpipe_read / devpipe_write sleep when blocked; devpipe_close sets EOF + wakes the other side. 4 new multi-thread unit tests; 2 existing tests repurposed. TLC clean cfg + 4 buggy cfgs (verdicts match expected).
- ✅ **P5-fd-pipe** (landed this chunk) — SYS_PIPE SVC handler + KOBJ_SPOOR release/acquire wiring in `kernel/handle.c`. First userspace consumer of `kernel/pipe.c`. No-args SVC returns rd fd in x0 / wr fd in x1; -1 on failure. KOBJ_SPOOR is now reference-counted on the handle table (third kind after BURROW + hw): handle_close + proc_free's handle_table_free both tear down the underlying Spoor via spoor_clunk. 3 new kernel-internal tests covering: 2 distinct handle alloc; proc_free end-to-end release; one-end close keeps other alive. **No spec extension** — pure composition over specs/handles.tla + specs/pipe.tla.
- ✅ **P5-fd-rw** (landed this chunk) — SYS_READ = 9, SYS_WRITE = 10 SVC handlers + `uaccess_store_u8` primitive. Each handler validates user-VA range + handle rights (READ for read, WRITE for write), bounces through a 4 KiB stack scratch, routes to `dev->read` / `dev->write`. New `uaccess_store_u8` mirrors `uaccess_load_u8` with its own fixup-table entry; the existing fault dispatcher already handled write faults via userland_demand_page's VMA prot check. 4 new kernel-internal tests including end-to-end pipe round-trip (write 7 bytes via SYS_WRITE; read back via SYS_READ; FIFO order). Read-after-close test composes with `specs/pipe.tla::ReadEof`. **No spec extension** — pure composition.
- ✅ **P5-fd-syscalls** (landed this chunk — bundles P5-fd-close + P5-fd-dup + P5-fd-pipe-probe) — SYS_CLOSE = 11 + SYS_DUP = 12 SVC handlers + libt stubs (t_pipe / t_read / t_write / t_close / t_dup) + first userspace test binary /pipe-probe exercising the full pipe + read/write/close/dup cycle end-to-end. KOBJ_SPOOR acquire path verified via kernel-internal `sys_pipe.dup_spoor_handle_acquires_ref` test. Boot log marker: `pipe-probe: PASS`. **No spec extension** — pure composition; RightsCeiling enforcement via handle_dup's existing check.
- ⬜ **P5-wire-lock / -xattr / -stratum-ext** — extends the wire codec with the remaining (non-standard / extension) message families.

### Reference

- `stratum/v2/docs/reference/20-9p.md` — Stratum's 9P2000.L server wire
  semantics (the canonical contract Thylacine binds to).
- `stratum/v2/docs/reference/23-9p_client.md` — Stratum's reference
  C-side client (sync, one-op-at-a-time; Thylacine's kernel client
  pipelines per `ARCH §21`).
- `stratum/v2/docs/OS-INTEGRATION.md` — OS-side integration manual.
- `ARCHITECTURE.md §10.2` + `§14` + `§28` — Thylacine-side dialect +
  integration + invariants.

## corvus.tla — P5-corvus-spec (Phase 5 corvus daemon; spec-first)

> **Currency note (RW-10, 2026-06-11).** (a) Rows below that map to
> `SYS_POST_SERVICE` / `SYS_SRV_CONNECT` / `SRV_CONN_PER_PROC_MAX` describe
> syscalls **retired at stalk-3c** — the as-built path is the per-territory
> `/srv` (post = `devsrv_create`, connect = open-on-`/srv` via
> `devsrv_open_connect`). (b) The spec treats the console-attach bit as
> one-way; the impl gained `proc_revoke_console_attached` (A-4c2/RW-7 —
> revocable on a LIVE Proc). No live violation (RW-6 F2 re-queries the live
> bit at the gates), but the spec under-models revocation — a re-model rides
> any future corvus-protocol change.

Status: **landed at P5-corvus-spec**. Models the corvus key-agent
daemon's session state machine + capability arithmetic + authorization
surface. Pins CORVUS-DESIGN.md §9 invariants C-3 (session user-binding
immutable), C-7 (unwrap refused for non-owner), C-11 (session-cap ×
Proc-cap orthogonal authorization), C-22 + C-23 (P5-corvus-srv: the
/srv per-connection transport — kernel-stamped peer identity), and the
§5.5 HostownerRequiresConsole rule. Implementation: P5-corvus-bringup-a
through -d landed (skeleton → wire → crypto → WRAP/UNWRAP); AUTH /
SESSION_CLOSE / UNWRAP are as-built; the AdminElevate / SessionTransfer
surface is later sub-chunks. The impl is a single `usr/corvus/src/
main.rs` — the `verbs/*.rs` paths below are the original intended
layout, not yet split out.

State universe at the model's cfg: Procs={p1,p2}, Users={u1,u2}, one
ProcCap (CapHostowner). Datasets identified by their owner user
(`Datasets == Users`, `DatasetOwner(d) == d`) — sidesteps TLC's config-
file limitation on record/function constants; the convention matches
the live system (`thylacine/users/<name>` dataset path).

Safety: TLC-clean — `corvus.cfg` explores 186,493,573 states /
21,641,580 distinct, depth 32, ~3min36s wall time on 8 cores. The
P5-corvus-srv-impl-a2 post-gate extension added the connection-layer
variables `service_marked` / `service_poster` (replacing the prior
one-shot `service_posted` BOOLEAN) / `service_posters`, the actions
`MarkMayPost` / `PostService(p)` / `ServiceTombstone`, the invariant
`ServicePosterEverMarked`, and the bug class `BuggyPostWithoutMarker`.

| Config | Flag | Invariant | Result | Distinct | Depth |
|---|---|---|---|---|---|
| `corvus.cfg`                              | all flags FALSE | `Invariants` | clean | 21641580 | 32 |
| `corvus_buggy_unwrap_cross_user.cfg`      | `BUGGY_UNWRAP_CROSS_USER`       | `UnwrapOwnerOnly`        | violation | 82 | 4 |
| `corvus_buggy_auth_binding_mutate.cfg`    | `BUGGY_AUTH_BINDING_MUTATE`     | `SessionUserImmutable`   | violation | 85 | 5 |
| `corvus_buggy_admin_without_proc_cap.cfg` | `BUGGY_ADMIN_WITHOUT_PROC_CAP`  | `AdminRequiresProcCap`   | violation | 83 | 5 |
| `corvus_buggy_elevate_without_console.cfg`| `BUGGY_ELEVATE_WITHOUT_CONSOLE` | `HostownerRequiresConsole` | violation | 105 | 4 |
| `corvus_buggy_transfer_rebind.cfg`        | `BUGGY_TRANSFER_REBIND`         | `SessionUserImmutable`   | violation | 83 | 5 |
| `corvus_buggy_identity_cached_on_fid.cfg` | `BUGGY_IDENTITY_CACHED_ON_FID`  | `ConnOpIdentityIsKernelTruth` | violation | 17372 | 9 |
| `corvus_buggy_dead_proc_stale.cfg`        | `BUGGY_DEAD_PROC_STALE`         | `ConnOpPeerWasLive`      | violation | 25819 | 9 |
| `corvus_buggy_post_without_marker.cfg`    | `BUGGY_POST_WITHOUT_MARKER`     | `ServicePosterEverMarked` | violation | 79 | 4 |

The session identity model uses (creation_proc, bound_user) as the
immutable pair, with owner_proc as a separate field that mutates on
SessionTransfer. This separation lets SessionUserImmutable catch BOTH
in-place mutation (BuggyAuthBindingMutate) AND mutation-during-transfer
(BuggyTransferRebind) by checking the immutable pair has a matching
origin record, regardless of which Proc currently owns the session
Spoor.

Spec actions ↔ impl mapping (impl filled in at P5-corvus-bringup):

| Spec action | Source location (target) | Notes |
|---|---|---|
| `MarkConsoleAttached(p)` | `kernel/proc.c::proc_mark_console_attached` (as-built; landed P5-hostowner-a), called from `kernel/joey.c::joey_thunk` | One-way set of `PROC_FLAG_CONSOLE_ATTACHED` (`proc_flags` bit 3). At v1.0 joey marks itself — the console-login chain root; P5-login extends the chain to per-user shells. `rfork` never propagates the bit (`proc_alloc` zeroes `proc_flags`; `rfork_internal` does not copy it), so it is conferred ONLY by an explicit call — the impl form of "console_attached grows solely via MarkConsoleAttached." `proc_is_console_attached` is the fail-closed query. |
| `MarkMayPost(p)` | `kernel/proc.c::proc_mark_may_post_service` (as-built; landed P5-corvus-srv-impl-a2), called from `kernel/syscall.c::sys_spawn_with_fds_thunk` when `SPAWN_PERM_MAY_POST_SERVICE` was set (b3a) | One-way set of `PROC_FLAG_MAY_POST_SERVICE` (`proc_flags` bit 4) — same one-way-marker discipline as `MarkConsoleAttached` (never cleared, never `rfork`-propagated). P5-corvus-srv-impl-b3a added `SYS_SPAWN_WITH_PERMS` as the race-free production path: joey spawns `/sbin/corvus` with `T_SPAWN_PERM_MAY_POST_SERVICE`; the kernel applies the bit inside the spawn thunk BEFORE `exec_setup`, so the child's first user-mode instruction observes the final `proc_flags`. The gate at the public entry is `proc_is_console_attached(parent)` — only joey can confer it. `proc_may_post_service` is the fail-closed query. |
| `AuthSuccess(p, u)` | `usr/corvus/src/main.rs::handle_auth` (as-built) | AUTH verb (verb_id=1). Argon2id → KEK → unwrap hybrid keypair → session token mint + bind to peer Proc identity. Spec models the resulting session record; in-RAM secret discipline (C-1/C-2/C-5) is runtime, not state-machine. |
| `SessionClose(p)` | `usr/corvus/src/main.rs::handle_session_close` (as-built) + Proc-exit cleanup path | SESSION_CLOSE verb (verb_id=3) or Spoor-close-driven cleanup. Clears in-RAM secrets. |
| `SessionTransfer(src, dst)` | `usr/corvus/src/transport/spoor_peer.rs::on_peer_change` | Triggered when the kernel reports a new peer Proc identity on /srv/corvus/'s Spoor (9P RIGHT_TRANSFER). MUST copy bound_user from existing session; MUST NOT take user from request fields. |
| `AdminElevate(p)` | `usr/corvus/src/verbs/admin.rs::handle_admin_elevate` | ADMIN_ELEVATE verb (verb_id=7). Reads /srv/corvus/peer/proc for console-attachment bit; argon2id system passphrase; on success grants CapHostowner via kernel cap-bump syscall. |
| `Unwrap(p, d)` | `usr/corvus/src/main.rs::handle_unwrap` (as-built; landed P5-corvus-bringup-d) | UNWRAP verb (verb_id=4). Reads session.bound_user, the dataset-ownership table, compares; refuses `PermissionDenied` on mismatch (`NotFound` if the dataset is unknown). The C-7 gate fires before any crypto. WRAP (verb_id=10) is the inverse and shares the gate. |
| `AdminVerb(p)` | `usr/corvus/src/verbs/admin.rs::dispatch_admin_verb` | USER_CREATE / USER_DELETE / ROTATE_KEY (verb_id=5,6,9). Reads peer Proc cap set via kernel; refuses without CapHostowner. NO session.bound_user check. |
| `BuggyUnwrapCrossUser` | (none — bug class statically prevented by `assert!(session.bound_user == owner)` in unwrap.rs) | Impl-side: the equality check is a single condition; bug shapes are short-circuit, wrong-direction, or wrong-fields. Code review + audit catches; spec models the consequence. |
| `BuggyAuthBindingMutate` | (none — bug class prevented by struct's immutable bound_user field) | Impl-side: Session struct's bound_user is `final` / non-`mut`; bug requires changing the API surface. |
| `BuggyAdminWithoutProcCap` | (none — bug class prevented by `kernel.has_cap(peer_pid, CapHostowner)` gate) | Same shape as BuggyUnwrapCrossUser: single check; bug shapes are missing-check or wrong-cap. |
| `BuggyElevateWithoutConsole` | (none — bug class prevented by `peer_console_bit?` precondition) | Same shape; bug = missing precondition. |
| `BuggyTransferRebind` | (none — bug class prevented by the SessionTransfer impl COPYING bound_user from src) | Critical impl discipline: the transfer path MUST take bound_user from the existing session by reference, not from any request-supplied field. |
| `PostService(p)` | `kernel/syscall.c::sys_post_service_for_proc` + `kernel/devsrv.c::srv_reserve`/`srv_commit` (as-built; landed P5-corvus-srv-impl-a2) | `SYS_POST_SERVICE` registers the caller as the `/srv/<name>` server. Gated on `proc_may_post_service` (spec precondition `p \in service_marked`). Reserve-then-commit two-phase so a failed `handle_alloc` rolls back (`srv_abort`). The kernel owns all transport (C-23). corvus's startup `SYS_POST_SERVICE("corvus")` call lands at P5-corvus-srv-impl-b. |
| `ServiceTombstone` | `kernel/devsrv.c::srv_proc_exit_notify`, called from `kernel/proc.c::exits` (as-built; landed P5-corvus-srv-impl-a2) | On poster exit the kernel flips the service `LIVE → TOMBSTONED`: the name stays reserved, re-postable only by a joey-marked Proc. Poster identity is held by value (`stripes`), so a reaped poster is no UAF. |
| `SrvBind(cn, p)` | `kernel/devsrv.c::srv_conn_open_for_proc` + `kernel/srvconn.c::srvconn_create` (kernel; landed P5-corvus-srv-impl-a3b) + `kernel/syscall.c::sys_srv_connect_handler` (production client-open path; landed P5-corvus-srv-impl-b2) | The kernel mints a fresh per-Proc connection (transport pair + dedicated synchronous `p9_client`) when Proc p opens `/srv/corvus`, stamping the peer identity. `SRV_CONN_PER_PROC_MAX = 1` enforces "one connection per LIVE connection per Proc" structurally; the cap is checked at `srv_conn_open_for_proc` entry and decremented in `handle_close`'s KOBJ_SRV arm — allowing reconnect after teardown. |
| `ConnTeardown(cn)` | `kernel/handle.c::handle_close` KOBJ_SRV arm + `kernel/srvconn.c::srvconn_teardown` (idempotent LIVE→TORN; landed across P5-corvus-srv-impl-a3a/a3b/b2) | A client closes its KObj_Srv handle → `handle_close` runs the KOBJ_SRV close-time dispatch → `srvconn_teardown` flips state LIVE→TORN under both channel locks, sets EOF on both rings, wakes pollers + accept_rendez. The per-Proc `srv_conn_count` decrements via the magic discriminator BEFORE `handle_release_obj` (the last unref can clobber the magic). After teardown the same Proc may open a fresh connection (`SrvBind` re-fires). F5 close (P5-corvus-srv-impl audit): the corvus.tla model gained a `connections_history` parallel APPEND-ONLY variable so the post-hoc identity invariant `ConnOpIdentityIsKernelTruth` stays sound across reconnects — the live `connections` set shrinks on teardown; the history set retains every binding ever minted for the invariant check. |
| `SrvAccept(cn)` | `kernel/syscall.c::sys_srv_accept_handler` (kernel; landed P5-corvus-srv-impl-a3b) + `usr/corvus/src/main.rs::srv_server_loop` (corvus accept loop; landed P5-corvus-srv-impl-b3b) | corvus accepts a kernel-bound connection; it never mints transport itself. The b3b accept loop polls `[listener, conns...]`; on listener-readiness it calls `t_srv_accept(listener)` (gets a `KObj_Spoor` connection endpoint) then `t_srv_peer(handle)` (gets the `TSrvPeerInfo` 24-byte by-value identity snapshot). |
| `SrvPeerOp(cn)` | `usr/corvus/src/main.rs::Conn::peer` (captured at accept time; landed P5-corvus-srv-impl-b3b) — per-request re-resolution lands when an identity-gated verb is added (admin verbs at P5-hostowner-b) | At b3b corvus captures the kernel-stamped peer identity at accept time and stores it on the per-Conn record by value. None of v1.0's five verbs (AUTH / USER_CREATE / UNWRAP / WRAP / SESSION_CLOSE) gate on peer identity (they gate on the session's bound_user, set at AUTH). When admin verbs land at P5-hostowner-b (ADMIN_ELEVATE / USER_CREATE-as-admin), each one will re-call `t_srv_peer(conn.handle)` per request to read the LIVE `caps` field (the immutable identity fields stay by-value; only `caps` + `alive` need a kernel-truth re-read). The C-22 discipline — "never cache peer identity for gating; always re-read fresh" — is structurally enforced when those verbs land. |
| `ProcExit(p)` | `kernel/proc.c` Proc-exit path (as-built) + P5-corvus-srv-impl-a connection teardown | A peer Proc exits; `SYS_SRV_PEER` thereafter fail-closes for that connection until teardown. |
| `BuggyIdentityCachedOnFid(cn1, cn2)` | (none — bug class prevented by corvus calling `SYS_SRV_PEER(cn)` per request rather than caching peer identity on fid state) | The bug: corvus credits an op on one connection to another connection's peer. Caught by `ConnOpIdentityIsKernelTruth`. |
| `BuggyDeadProcStale(cn)` | (none — bug class prevented by `SYS_SRV_PEER`'s dead-Proc guard, which reads `caps` live under the process-table lock and fail-closes for an exited peer) | The bug: `SYS_SRV_PEER` returns an exited peer's stale identity. Caught by `ConnOpPeerWasLive`. |
| `BuggyPostWithoutMarker(p)` | (none — bug class prevented by the `proc_may_post_service` gate in `sys_post_service_for_proc`, the first check before any registry mutation) | The bug: `SYS_POST_SERVICE` skips the post-gate, letting an unmarked Proc claim `/srv/corvus` and impersonate the key agent. Caught by `ServicePosterEverMarked`. |

Spec invariants ↔ impl enforcement:

| Spec invariant | Source enforcement |
|---|---|
| `SessionUserImmutable` (C-3) | Session struct's bound_user is non-mut after construction. SessionTransfer copies the field, never accepts an override. |
| `UnwrapOwnerOnly` (C-7) | The comparison of `session.bound_user` against the dataset-ownership table in `main.rs::handle_unwrap` and `handle_wrap` — C-7-gated, fires before any crypto. Landed P5-corvus-bringup-d. |
| `AdminRequiresProcCap` (C-11 Proc-cap path) | The single check `kernel.peer_has_cap(CapHostowner)` in dispatch_admin_verb. |
| `HostownerRequiresConsole` (§5.5) | The single check `peer_proc.console_attached?` in handle_admin_elevate. Combined with the kernel-side discipline that console_attached is never propagatable across rfork. |
| `ConnOpIdentityIsKernelTruth` (C-22) | corvus calls `SYS_SRV_PEER(connection_handle)` per request; the kernel returns the connection's bind-time-stamped peer. corvus never sources peer identity from the client nor caches it on fid state. |
| `ConnOpPeerWasLive` (C-22) | `SYS_SRV_PEER`'s dead-Proc guard — an exited / zombie / reaped peer yields a fail-closed result, so no op is authorized against a dead peer. |
| `ConnTransportKernelOwned` (C-23) | `SYS_POST_SERVICE`: the kernel creates and owns every `/srv/corvus` connection's transport; corvus only accepts kernel-bound connections. `KObj_Srv` (handles.tla) keeps the transport out of any handle table. |
| `ServicePosterEverMarked` (§6.1 post-gate) | `sys_post_service_for_proc` calls `proc_may_post_service` before any registry mutation; the bit is one-way + never `rfork`-propagated, so a tombstoned name is re-postable only by a joey-marked Proc — no malicious rebind race on corvus restart. Landed P5-corvus-srv-impl-a2. |

References:
- `docs/CORVUS-DESIGN.md` — full design + invariant numbering.
- `docs/STRATUM-API-V1.md` — bilateral contract with Stratum (A3
  corvus-validated UNWRAP wire is the wire used by Spec UNWRAP).
- `docs/ARCHITECTURE.md §15` — capabilities + CAP_HOSTOWNER addition.

## sched_ctxsw.tla — P5-el1h-kernel (uniform-EL1h kernel model)

Status: **invariant I-21 pinned.** Models the relationship between a
CPU's live SPSel and the execution mode the running thread requires.
The constant `DUAL_MODE` selects the kernel model: `FALSE` = Model 1
(uniform EL1h — the fix), `TRUE` = Model 2 (the pre-P5 EL1t/EL1h
dual-mode bug). `CtxSwitchModeConsistent` (`cpu_mode =
thread_mode[mode_running]`) holds by construction under Model 1;
`BuggyModeSwitch` violates it under Model 2 — the executable
counterexample for the secondary-CPU `msr SP_EL0` crash.

| Spec action | Impl target | Notes |
|---|---|---|
| `Init` (all `el1h`) | `arch/arm64/start.S` `_real_start`/`secondary_entry` | Boot asserts `SPSel=1`, sets `SP_EL1` = kernel stack. |
| `ModePreempt` / `ModeReturn` | `arch/arm64/vectors.S` `KERNEL_ENTRY` / `KERNEL_EXIT` | Exception entry/exit; EL1h→EL1h for a kernel-mode exception. |
| `ModeSwitch(next)` | `arch/arm64/context.S` `cpu_switch_context` | One SP bank (`SP_EL1`); no SPSel to mis-restore. |
| `BuggyModeSwitch(next)` | (none — bug class statically eliminated) | The pre-P5 `cpu_switch_context` carried SP not SPSel; the uniform-EL1h model removes the second mode entirely. |
| `CtxSwitchModeConsistent` | runtime: `test_smp.exception_stack_smoke` asserts `SPSel==1` | ARCH §28 I-21. |

cfgs: `sched_ctxsw.cfg` (`DUAL_MODE=FALSE`) clean; `sched_ctxsw_buggy.cfg`
(`DUAL_MODE=TRUE`) expected-violation. Companion sibling module to
`scheduler.tla` (the `pipe.tla` / `corvus.tla` precedent). See
`docs/reference/67-el1h-kernel.md`.

The model is single-CPU: it captures the *essence* of the bug (a switch
retargeting the CPU across a thread-mode boundary) as the sound
abstraction of the cross-CPU work-stealing case.

## tsleep.tla — P5-tsleep (deadline-bounded Rendez sleep)

Status: **landed at P5-tsleep.** Models `tsleep` — `sleep` plus a
deadline — and the three-way wake race it introduces: the condition,
`wakeup`, and the timer (the `sched_tick` scan of the global timer-wait
list). A focused sibling of `scheduler.tla`, on the `sched_ctxsw.tla` /
`pipe.tla` precedent: `scheduler.tla` already proves the check-then-
sleep atomicity for `sleep` (and `tsleep`'s no-deadline path *is*
`sleep`), so `tsleep.tla` models only the new surface — the deadline.

State universe: one waiter, one Rendez, one deadline (`Rendez` is
single-waiter by construction, so one sleeper exercises the whole race;
`MaxSleeps` bounds the spurious-wakeup re-sleep loop). CONSTANTS:
`HAS_DEADLINE` (FALSE = the no-deadline path), `BUGGY_LAZY_UNLINK`,
`BUGGY_TIMEOUT_STALE`, `BUGGY_RECHECK_ORDER`.

| Config | Flags | Checked | Result | Distinct |
|---|---|---|---|---|
| `tsleep.cfg`                     | all FALSE, `HAS_DEADLINE`  | `Invariants` | clean (depth 10) | 52 |
| `tsleep_nodeadline.cfg`          | `HAS_DEADLINE=FALSE`       | `Invariants` | clean | 18 |
| `tsleep_liveness.cfg`            | all FALSE, `Spec_Live`     | `Invariants` + `TsleepTerminates` | clean | 37 |
| `tsleep_buggy_lazy_unlink.cfg`   | `BUGGY_LAZY_UNLINK`        | `NoStaleTimerEntry` | violation (depth 3) | 15 |
| `tsleep_buggy_double_wake.cfg`   | `BUGGY_LAZY_UNLINK` + `BUGGY_TIMEOUT_STALE` | `NoDoubleWake` | violation (depth 5) | 58 |
| `tsleep_buggy_recheck_order.cfg` | `BUGGY_RECHECK_ORDER`      | `TimeoutSound` | violation (depth 4) | 56 |
| `tsleep_buggy_wedge.cfg`         | `HAS_DEADLINE=FALSE`, `Spec_Live` | `TsleepTerminates` | violation (stutter) | 13 |

Spec action ↔ impl mapping:

| Spec action | Source location | Notes |
|---|---|---|
| `Commit` | `kernel/sched.c::tsleep()` loop body | The cond-first-then-deadline evaluation + the enqueue (`timerwait_link` + `r->waiter` + `SLEEPING`). Both first entry and every resume. |
| `Wakeup` | `kernel/sched.c::wakeup()` → `wake_rendez_waiter(…, false)` | `wakeup` does the `timerwait_unlink` under `g_timerwait.lock`, releases it, then `wake_rendez_waiter` runs the transition under `r->lock`. |
| `Timeout` | `kernel/sched.c::timerwait_tick()` → `wake_rendez_waiter(…, true)` | The `sched_tick` scan, one thread per `g_timerwait.lock` acquisition (P5-tsleep-scale): `timerwait_unlink` + the `state==SLEEPING && r->waiter==t` re-check + the wake, all under `g_timerwait.lock` + `r->lock` held continuously. Skips an `on_cpu` thread (woken next tick). |
| `SetCond` / `AdvanceTime` | the producer's cond mutation / the monotonic counter | Environment actions — not kernel code. |
| `BuggyLazyUnlink` | (none — `wakeup` + `timerwait_tick` both `timerwait_unlink` before readying) | Eager unlink is the discipline; lazy removal is the bug. |
| `BuggyTimeoutStale` | (none — `timerwait_tick` re-checks `state==SLEEPING && r->waiter==t`) | The scan re-validates before waking. |
| `BuggyRecheckOrder` | (none — `tsleep`'s loop checks `cond` before the timeout) | cond-first is success precedence. |

Spec invariant ↔ impl enforcement:

| Spec invariant | Source enforcement |
|---|---|
| `NoStaleTimerEntry` | The eager `timerwait_unlink` in `wakeup` (under `g_timerwait.lock`) and in `timerwait_tick`. A thread is on the list iff in a deadlined `tsleep`. |
| `NoDoubleWake` | The `r->waiter` single-consumer check + `timerwait_tick`'s `state==SLEEPING` re-check: `wakeup` and the timeout scan are mutually exclusive — whichever fires first clears `r->waiter` and unlinks the thread. |
| `WokenSound` / `TimeoutSound` | `tsleep`'s resume re-checks `cond` first; only a false `cond` consults `sleep_timedout` / the deadline. Success precedence. |
| `SleepingHasWaiter` | `tsleep` sets `r->waiter` and `SLEEPING` together under `r->lock`. |
| `TsleepTerminates` (liveness) | The deadline + the periodic `timerwait_tick` scan: a hung producer (no `wakeup`, `cond` never true) still sees the wait end at the deadline. |

The spec's `Expired == HAS_DEADLINE /\ (timedout_flag \/ deadline_passed)`
lumps **two** impl mechanisms that `Commit` consults: `t->sleep_timedout`
(set by `timerwait_tick`, the `Timeout` action) and the synchronous
`timer_get_counter() >= deadline_cnt` read in `tsleep`'s loop (the entry
/ spurious-resume path — no dedicated spec action; modeled implicitly by
`AdvanceTime` + `Commit`). Both yield `TIMEDOUT` only with `cond` false,
so `TimeoutSound` covers both.

P5-tsleep-scale (audit F6) reworked `timerwait_tick` to wake a herd of
expired sleepers one thread per `g_timerwait.lock` acquisition rather
than under one hold. No spec change: each thread is still unlinked +
woken atomically (both locks held continuously across its wake), which
is exactly what the single-waiter `Timeout` action models — the
inter-thread lock release is below the one-sleeper model's resolution
and introduces no race (independent per-thread processing composes).

cfgs run with `-deadlock`; `tsleep.tla`'s `Done` self-loop keeps a
legitimate terminal state from tripping the deadlock check. See
`docs/reference/16-rendez.md` (the `tsleep` section).

## poll.tla — P5-poll-spec (spec landed); P5-poll-a (mechanism + SYS_POLL + devpipe poll); P5-poll-b (devsrv poll)

Status: **spec landed at P5-poll-spec; the poll mechanism + `SYS_POLL`
+ `devpipe` poll landed at P5-poll-a; `devsrv` poll (connection
endpoint + KObj_Srv listener) landed at P5-poll-b.** Models `poll` —
one thread waiting on N readiness sources whose state lives behind N
different locks. `scheduler.tla` already proves the single-`Rendez`
check-then-sleep atomicity and `tsleep.tla` the deadline race;
`poll.tla` models the surface neither covers — the cross-lock
`poll_waiter` hand-off and the register-then-observe discipline.

State universe: one poller, N fds (`Fds`), one timeout. One poller
exercises the missed-wakeup-across-N-fds race fully; multiple pollers on
one fd's hook list compose (each has its own private `Rendez` +
`poll_waiter`, no shared mutable state). CONSTANTS: `HAS_TIMEOUT`
(FALSE = poll(-1)), `BUGGY_CHECK_BEFORE_REGISTER`, `BUGGY_NO_WAKE`,
`BUGGY_LAZY_UNREGISTER`.

| Config | Flags | Checked | Result | Distinct |
|---|---|---|---|---|
| `poll.cfg`                             | all FALSE, `HAS_TIMEOUT`      | `Invariants` | clean | 25 |
| `poll_notimeout.cfg`                   | `HAS_TIMEOUT=FALSE`           | `Invariants` | clean | 12 |
| `poll_liveness.cfg`                    | all FALSE, `Spec_Live`        | `Invariants` + `PollTerminates` + `PollReturnsWhenReady` | clean | 25 |
| `poll_buggy_check_before_register.cfg` | `BUGGY_CHECK_BEFORE_REGISTER` | `NoMissedPoll` | violation (depth 5) | — |
| `poll_buggy_no_wake.cfg`               | `BUGGY_NO_WAKE`               | `NoMissedPoll` | violation (depth 4) | — |
| `poll_buggy_lazy_unregister.cfg`       | `BUGGY_LAZY_UNREGISTER`       | `NoStaleHook`  | violation (depth 4) | — |

Spec action ↔ impl mapping:

| Spec action | Source location | Notes |
|---|---|---|
| `Register` | `kernel/poll.c::poll_scan_one` (first scan); `kernel/pipe.c::devpipe_poll`; `kernel/srvconn.c::srvconn_poll`; `kernel/devsrv.c::svc_listener_poll` + `devsrv_poll` + `srv_handle_poll` | `dev->poll(spoor, events, pw)` for KObj_Spoor; `srv_handle_poll(obj, events, pw)` for KObj_Srv (magic-discriminated SrvService/SrvConn dispatch). Each installs the hook AND samples readiness in one locked step under the object's lock(s) (`r->lock` for pipe; `c2s.lock` + `s2c.lock` for SrvConn; `g_srv_registry.lock` for SrvService). |
| `CommitOrSleep` | `kernel/poll.c::sys_poll_for_proc` (post-scan fast-path + `tsleep`) | Fast-path return on `ready_count > 0` or `timeout_ms == 0`; else compute `deadline_ns` + `tsleep` on the poller's private rendez with `poll_cond_any_flagged`. |
| `MakeReady(f)` | devpipe: `kernel/pipe.c::devpipe_close` + `devpipe_read` (drain) + `devpipe_write` (append). srvconn: `kernel/srvconn.c::srvconn_teardown` + `srvconn_client_send` + `srvconn_server_send`. devsrv listener: `kernel/devsrv.c::srv_conn_open_for_proc` (push) + `srv_proc_exit_notify` (tombstone) + `srv_registry_reset`. | Every existing readiness `wakeup` site also calls `poll_waiter_list_wake` — sets each registered `pw->ready` AND signals each `pw->rendez`. |
| `Timeout` | `kernel/sched.c::tsleep` deadline (landed, P5-tsleep) | poll's timeout IS a `tsleep` deadline; the cond re-check at resume has precedence over the deadline (tsleep.tla TimeoutSound). |
| `NoStaleHook` (unregister sweep) | `kernel/poll.c::sys_poll_for_proc` (the `unregister_and_return:` label) | Every exit path from `sys_poll_for_proc` goes through the sweep; idempotent on already-unregistered hooks; scribbles `pw->magic = 0` for defense-in-depth. |
| `BuggyCheckBeforeRegister` / `BuggyNoWake` / `BuggyLazyUnregister` | (none — register-then-observe lives in every `.poll` impl; every readiness site calls `poll_waiter_list_wake`; the sweep above is unconditional) | The three disciplines the impl uphold. |

cfgs run with `-deadlock`; `poll.tla`'s `Done` self-loop keeps a
legitimate terminal state from tripping the deadlock check. See
ARCH §23.3.

## cons_poll.tla — P7-LS-8a (the pollable-console deferred poll-wake; spec-first re-enabled)

Status: **spec landed at LS-8 scripture; the mechanism lands at LS-8a
(source map filled then).** Models the LS-8a DEFERRED poll-wake: the
console RX IRQ cannot walk the non-IRQ-safe `poll_waiter_list` (a plain
non-irqsave lock + a nested `wakeup`), so it sets `poll_wake_pending`
under `g_cons.lock` and wakes the `console_mgr` kthread, which drains the
flag and walks the hook list in *process* context (Linux's tty
`flush_to_ldisc` model). `poll.tla` owns the poller-side
register-then-observe + the N-fd fan; `cons_poll.tla` adds the SECOND
register-then-observe the relay introduces — the mgr's own sleep on
`poll_wake_pending` must be register-then-observe
(`sleep(&mgr_rendez, cons_mgr_pending)`), or a flag set as the mgr heads
back to sleep is lost. I-9 across the IRQ→mgr→hook-list chain.

State universe: one poller, one console, one console_mgr, one benign
extra mgr waker (`SpuriousWake`, capped once — the Ctrl-C/SAK path that
puts the mgr in the "awake, about to re-sleep" state where the relay race
opens). CONSTANT: `BUGGY_MGR_LOST_WAKE` (the mgr's go-to-sleep as a
hand-rolled check-then-sleep instead of register-then-observe).

| Config | Flags | Checked | Result | Distinct |
|---|---|---|---|---|
| `cons_poll.cfg`                 | `BUGGY_MGR_LOST_WAKE=FALSE` | `Invariants` | clean | 31 |
| `cons_poll_liveness.cfg`        | `Spec_Live`, all FALSE      | `PollerEventuallyServed` | clean | 31 |
| `cons_poll_buggy_lost_wake.cfg` | `BUGGY_MGR_LOST_WAKE`       | `NoMissedConsPoll` | violation (depth 9) | — |

Spec action ↔ impl mapping: **filled at LS-8a** — the intended map is
`kernel/cons.c::cons_rx_input` = `DataArrives`; `console_mgr_main` =
`MgrDrainWalk` + the register-then-observe `MgrSleep`; the `.poll` impl on
devcons/devdev = `PollerRegister`; `poll_waiter_list_wake` from the mgr =
the walk inside `MgrDrainWalk`; the `g_cons_mgr_rendez` non-poll wakers
(Ctrl-C/SAK) = `SpuriousWake`. The `BUGGY_MGR_LOST_WAKE` counterexample is
the durable regression for the mgr-relay register-then-observe.

cfgs run with `-deadlock`; the `Done` self-loop keeps the legitimate
terminal state from tripping the deadlock check (the lost-wake stuck state
is PRE-terminal, so it is still caught). See ARCH §23.5.1 + I-9.

## futex.tla — DROPPED (2026-05-23 spec-to-code suspension)

Never written. The `torpor` wait-on-address primitive (Phase 6) is
prose-validated (`kernel/torpor.c` + `torpor.h` reasoning + the focused
audit); `death_wake.tla` covers the death-wake interaction (I-9 generalized).

## notes.tla — DROPPED (2026-05-23 spec-to-code suspension)

Never written. The kernel notes substrate (Phase 6, sub-chunk 13a) is
prose-validated: design + N-1..N-5 invariants in ARCH §7.6.1-§7.6.8, the
4-round 13a audit, and the `notes.*` test suite (I-19).

## pty.tla — DEFERRED (the PTY master/slave mechanism unbuilt; Phase 8)

The PTY *master/slave* pair (`/dev/ptmx` + `/dev/pts/<n>`, per-fd termios,
I-20) is Phase-8; `pty.tla` lands with that server. NOTE: LS-8's
*single-console* line discipline does NOT wait on this — its load-bearing
invariant is I-9's deferred poll-wake, specced by `cons_poll.tla` (above),
not I-20. `kernel/cons.c` records the master/slave deferral; I-20 is
RESERVED in ARCH §28 until the Phase-8 server lands.

---

## loom.tla — Loom-1 (the SQ/CQ ring transport; spec-first re-enabled)

Status: **TLC-green; Loom-2a + Loom-2b + Loom-3 + Loom-4 (4a/4b/4c/4d) landed
(substrate + the pluggable-completion seam + the batch-enter core + the SQPOLL
transport-deadline substrate + the CQ wait-list + the SQPOLL poll-thread + the
focused SQPOLL audit close); the Loom-5 SPEC landed as two new focused modules
(`loom_multishot.tla` + `loom_order.tla`, documented below). *(Currency: the
Loom-5 IMPL + the whole Loom-6 arc subsequently LANDED -- the arc is COMPLETE
at 6d; see `docs/reference/107-loom.md`. No spec mechanism changed after
`d48a8da`.)* Loom-4d (the audit close) added NO
new spec mechanism -- the F1 borrow-guard `spoor_ref`, the F2 admittability park
cond, and the SA-2 `P9_PUMP_BUSY` yield are impl refinements WITHIN the `d48a8da`
CQ-waiter + Teardown model (the kthread is one `PostCqe` producer + the `Teardown`
actor); `loom.cfg` re-run clean (2429) + liveness (1457) + all 7 buggy cfgs violate
their target is the gate.** Models the
Loom submission / completion ring op-lifecycle and pins the two reserved ARCH §28
invariants — **I-29** (completion integrity: no-lost / no-double / no-stale) and
**I-30** (submit-time capability pin) — plus the docs/LOOM.md §6 soundness
obligations (ring TOCTOU, CQ back-pressure) and, since Loom-4, **I-9 on the CQ
wait-list** (the SQPOLL completion wait/wake). Spec-first is re-enabled for this
surface (docs/LOOM.md §7); TLC-green on `loom.cfg` gates every Loom impl
sub-chunk. The spec-action↔source mapping below is populated as Loom-2..6 land.
**Loom-3 added no new spec mechanism** (its `Consume` / `Dispatch` / `Reap` /
`Teardown` were already modeled). **Loom-4 DID** — the CQ-waiter actor
(`CqWaitRegister` / `CqWaitCommitOrSleep` + the `PostCqe` / `Teardown` wake) +
`CqFlagTracksCq` / `NoMissedCqWake` / `NoStrandedWaiter` / `CqWaiterReturns` + the
two `BUGGY_CQWAIT_*` cfgs, extended FIRST (docs/LOOM.md §8.6) before the impl.

Safety: TLC-clean at `Ops = {o1, o2}, CQ_CAP = 1, MAX_INFLIGHT = 2` — 2429 distinct states (Loom-4 CQ-waiter dimension).
Liveness: TLC-clean (`EventuallyCompletes` + `CqWaiterReturns`, `ALLOW_TEARDOWN = FALSE`) at the same universe — 1457 distinct states.

| Config | Flag | Invariant / Property | Result | Distinct |
|---|---|---|---|---|
| `loom.cfg`                              | all FALSE, teardown on        | `Invariants` (15)       | clean | 2429 |
| `loom_liveness.cfg`                     | Spec_Live, no teardown        | `EventuallyCompletes` + `CqWaiterReturns` | clean | 1457 |
| `loom_buggy_live_sqe_reread.cfg`        | `BUGGY_LIVE_SQE_REREAD`       | `ArgPinnedToSnapshot`   | violation | — |
| `loom_buggy_recheck_at_completion.cfg`  | `BUGGY_RECHECK_AT_COMPLETION` | `ObjPinnedToSnapshot`   | violation | — |
| `loom_buggy_double_post.cfg`            | `BUGGY_DOUBLE_POST`           | `NoDoubleCompletion`    | violation | — |
| `loom_buggy_lost_on_full.cfg`           | `BUGGY_LOST_ON_FULL_CQ`       | `CqNeverOverfull`       | violation | — |
| `loom_buggy_stale_after_teardown.cfg`   | `BUGGY_STALE_AFTER_TEARDOWN`  | `NoStaleCompletion`     | violation | — |
| `loom_buggy_cqwait_no_wake.cfg`         | `BUGGY_CQWAIT_NO_WAKE`        | `NoMissedCqWake`        | violation | — |
| `loom_buggy_cqwait_check_early.cfg`     | `BUGGY_CQWAIT_CHECK_EARLY`    | `NoMissedCqWake`        | violation | — |

| Spec action | Source location | Notes |
|---|---|---|
| `UserProduce` / `UserMutateSqe` | userspace side (native API at Loom-6); the kernel READ side is **Loom-3** `kernel/loom.c::loom_enter` (SQ-index consume from the kernel-private `sq_head`) | userspace fills / mutates an SQE slot in the shared Burrow and bumps the SQ tail; the mutate models a thread racing the kernel's ring read (loom_enter copies the SQE to kernel memory before acting). |
| `UserRegister` | **Loom-2a**: `kernel/loom.c::loom_register_handles` + `kernel/syscall.c::sys_loom_register_for_proc` (`SYS_LOOM_REGISTER` LOOM_REGISTER_HANDLES) | install / replace a registered-handle table slot (a clunk + reuse is a replace); the held `spoor_ref` + the rights snapshot are the I-30 pin SUBSTRATE. |
| `Consume` | **Loom-3**: `kernel/loom.c::loom_submit_one` (SQE consume in `loom_enter` / `SYS_LOOM_ENTER`) | the submit-time snapshot + pin: copy the SQE to a kernel `struct loom_sqe` (ring TOCTOU), validate (opcode / flags / handle_idx), resolve + rights-check the registered handle (RIGHT_WRITE for FSYNC — I-2 / I-6), take an independent `spoor_ref` (the pin), allocate a 9P tag via `p9_client_submit_async` (I-10 bound). |
| `Dispatch` | **Loom-3**: `kernel/loom.c::loom_build_fsync` (the `build` thunk) + `kernel/9p_client.c::p9_client_submit_async` | issue the 9P Tmsg on the pinned (client, fid) + the snapshot args. The async submit entry (Loom-2b) drives the per-opcode `build`; the op acts on the kernel snapshot, never re-reading the shared SQE (`ArgPinnedToSnapshot`). |
| `ReplyArrives` | **Loom-2b**: `kernel/9p_client.c::demux_frame_locked` (the async `on_complete != NULL` branch) → **Loom-3** `kernel/loom.c::loom_async_complete` | the #841 elected-reader demux fires the pluggable POST_CQE action (`on_complete` = `loom_async_complete`) instead of WAKE_RENDEZ (docs/LOOM.md §8.4); the reap-side reader is `p9_client_reader_pump_once`, driven by `loom_enter`. |
| `PostCqe` (+ the Loom-4 wake) | **Loom-2b** (the writer): `kernel/loom.c::loom_post_cqe`; **Loom-3** call site: `loom_async_complete` (async) + `loom_submit_one` (inline error/NOP CQEs); **Loom-4b** the wake: `loom_post_cqe` calls `poll_waiter_list_wake(&l->cq_waiters)` after publishing the CQE + releasing `l->lock` | write the `loom_cqe` (user_data + mapped result) into the CQ ring; back-pressure on a full CQ (`overflow` counter, never overwrite). The op never re-resolves the registered handle at completion (`ObjPinnedToSnapshot`). A successful post wakes the CQ wait-list (a refused full-CQ post does NOT — `CqWaitFlagSound`); the wake is below `l->lock` in the lock order + does not sleep, so it composes with the `c->lock` the async path holds (`CqFlagTracksCq` / `NoMissedCqWake`). |
| `Reap` | **Loom-3**: userspace CQ-head bump (native API at Loom-6) + the kernel container reclaim `kernel/loom.c::loom_reap_terminal` (run by `loom_enter` after the wait) | userspace consumes a CQE; permitted post-teardown for already-posted CQEs. The kernel reaps terminal-op containers (clunk pin + free) outside `l->lock`. |
| `Teardown` | **Loom-3**: `kernel/loom.c::loom_free` quiesce → `kernel/9p_client.c::p9_client_abandon_async` (the #845 Tflush-on-abandon, #898); the session-death CQ-waiter wake is realized at **Loom-4b** (`client_mark_dead_locked` posts an error CQE per in-flight async op → `loom_post_cqe` → wake); **Loom-4c** realizes the SQPOLL-kthread teardown: `loom_free` JOINS the kthread FIRST (set `sqpoll_stopping` + wake `sqpoll_park` + spin `sqpoll_exited` + `thread_free`) before the quiesce, then makes the spec's `Teardown`-wakes-the-wait-list action LITERAL via `poll_waiter_list_wake(&l->cq_waiters)` | quiesce every in-flight async op before freeing the ring Burrow: under the client's `c->lock`, clear `inflight[tag]` (no future `on_complete`) + Tflush (a late reply is discarded ownerless); then clunk the pin + free the container; free the loom last (`NoStaleCompletion`). At **Loom-4b/4c** `NoStrandedWaiter` holds **vacuously**: a `loom_enter` caller holds a loom ref for its whole duration, so `loom_free` cannot run while a CQ-waiter sleeps; and `KObj_Loom` is per-Proc + non-transferable, so all concurrent ENTERs are sibling threads that group-terminate together (`SLEEP_INTR`). The SQPOLL kthread holds no loom ref and is joined by the explicit `sqpoll_exited` handshake, not the wait-list. |
| `CqWaitRegister` / `BuggyCqWaitCheck` / `BuggyCqWaitRegisterLate` | **Loom-4b**: `kernel/loom.c::loom_wait_for_completions` (driven by `loom_enter`) — install a `poll_waiter` on `l->cq_waiters` AND re-sample `loom_cq_ready` in one `l->lock`-held step (register-then-observe), then `sleep(&r, loom_cqw_cond, &pw)` (death-interruptible, #811) | the `ENTER` waiter (`min_complete >= 1`) that finds a sibling holding the reader role blocks for completions; the cross-lock flag is the wait-list hook (`pw->ready`). `BuggyCqWait*` model the check-before-register order (the bug `CqFlagTracksCq` forbids). |
| `CqWaitCommitOrSleep` | **Loom-4b**: the `loom_cqw_cond` flag read in `sleep()` + the `loom_cq_ready >= min_complete` / `async_inflight == 0` give-up arms in `loom_wait_for_completions` | the evaluate point: flag set -> return; nothing more can complete -> return; else sleep on the CQ wait-list. The wake co-fires with `PostCqe` (`loom_post_cqe` walks the wait-list after publishing) (`NoMissedCqWake`). |
| `BuggyDoublePost` / the seven `BUGGY_*` flags | (none — these are the disciplines the impl upholds) | snapshot-not-reread; pin-not-re-resolve-at-completion; one-CQE-per-op; never-post-into-a-full-CQ; quiesce-on-teardown; **register-the-wait-hook-before-sampling-the-CQ**; **wake-the-wait-list-on-every-post-and-on-teardown** (`CqFlagTracksCq` / `NoMissedCqWake` / `NoStrandedWaiter`). |

cfgs run with `-deadlock`; `loom.tla`'s `Done` self-loop keeps the
torn-and-drained terminal state from tripping the deadlock check. See
docs/LOOM.md + ARCH §28 (I-29, I-30 reserved at impl).

### Loom-6a/6b source map (registered buffers + READ/WRITE + read-shaped + mutation ops; `loom.tla` unchanged)

**Loom-6a adds no new spec mechanism** — the registered-buffer pin + the
slice-validation are the abstract `reg` slot + the `sqe_arg` / `ValidArgs` model
`loom.tla` reserved from Loom-1 (the `sqe_arg` comment explicitly names "a
malformed / out-of-bounds **buffer** descriptor"). The concrete two-level buffer
resolution maps onto the existing actions + invariants (the Loom-3 precedent):

| Spec action / invariant | Loom-6a source location | Notes |
|---|---|---|
| `UserRegister(obj)` → `reg` slot | `kernel/loom.c::loom_register_buffers` / `loom_resolve_buf` | the registered-buffer TABLE is a second concrete realization of the abstract `reg` slot: `vma_lookup` → one writable anon Burrow → `burrow_ref` the pin + snapshot the contiguous-direct-map kva + len; all-or-nothing install (resolve under `p->vma_lock`, swap under `l->lock`, unref displaced outside both). |
| `Consume(o)`: `sqe_arg ∈ ValidArgs` + pin `reg` | `kernel/loom.c::loom_submit_payload` (was `loom_submit_rw`; generalized at 6b-1) | the submit gate: resolve + pin the file handle (`spoor_ref`) AND the registered buffer (an independent `burrow_ref`) together under `l->lock`; bounds-check the `[buf_off, buf_off+len)` slice against the KERNEL snapshot (`ActedArgValidated` — an OOB slice is `arg_bad` → rejected); rights-gate (the *builder* + the *required right* are opcode-selected: WRITE→`RIGHT_WRITE`, every read-shaped op→`RIGHT_READ`). |
| `Dispatch(o)`: act on `snap_arg` + `snap_obj` (`ArgPinnedToSnapshot` + `ObjPinnedToSnapshot`) | `kernel/loom.c::loom_build_read`/`write`/`readdir`/`readlink`/`getattr`/`statfs` | the builder reads only the op's submit-time snapshot (`op->op_fid`/`op_offset`/`op_count`/`buf_kva`) under `c->lock`; WRITE reads the pinned buffer slice; never re-reads the shared SQE fields, never re-resolves `buf_idx`. |
| `PostCqe(o)`: no re-resolve (`ObjPinnedToSnapshot`) | `kernel/loom.c::loom_payload_result` in `loom_async_complete` | the read-shaped ops copy the reply INTO the pinned buffer (`min(reply_len, op_count)`, under `c->lock`): READ/READDIR/READLINK stream bytes; GETATTR/STATFS copy the fixed `struct p9_attr`/`p9_statfs` (the Loom output ABI, `_Static_assert`-pinned). The result is the byte count; the pin is never re-resolved at completion. |
| `Teardown` quiesce (`NoStaleCompletion`) | `loom_reap_terminal` / the #898 `loom_free` abandon / `loom_free`'s `reg_buf[]` sweep | the op's `pinned_buf` `burrow_ref` is released exactly once on reap / quiesce; `loom_free` releases every table pin (the #847 dual-refcount frees the pages once both counts hit 0). |

**Loom-6b-1 (read-shaped ops: READDIR / READLINK / GETATTR / STATFS)** adds no new
spec mechanism either: each is one more `Dispatch` against a pinned `reg` slot
with a `ValidArgs`-checked `sqe_arg` dest slice, mapping onto the SAME rows above
(generalized `Consume`/`Dispatch`/`PostCqe`). The fid-lifecycle ops (WALK / LOPEN
/ LCREATE / CLUNK) are the deferred direct-descriptor seam (task #916) — they
would add a `reg`-slot install/release from the completion path, a genuinely new
mechanism that gets its own model when it lands.

**Loom-6b-2 (metadata-mutation ops: SETATTR / MKDIR / MKNOD / SYMLINK / UNLINKAT /
RENAMEAT / LINK)** adds no new spec mechanism either. Each is one more `Dispatch`
against a pinned `reg` slot: the name(s) / input struct are read FROM the pinned
buffer slice in the build thunk (the `Dispatch`-acts-on-`snap_arg` row, the WRITE
precedent), bounded at submit by the two memory-safety gates (the two-name split
sub-length `<= len`; SETATTR's `len >= sizeof(struct p9_setattr)`) which realize
`ActedArgValidated` (a malformed descriptor is `arg_bad` → rejected at `Consume`).
The **two-fid ops (RENAMEAT / LINK)** pin a SECOND `reg` slot (`op->pinned2`): the
spec models ONE representative `reg`/`snap_obj` per op, but the I-30 properties
(`ObjPinnedToSnapshot` + `ActedUnderAdmittedRights`) are PER-OBJECT, and the
second pin is the identical mechanism (resolve + snapshot at `Consume`, never
re-resolve at `PostCqe`) applied to a second object — the single-`reg` model is a
faithful abstraction of N independent pins, so no module extension is needed (the
same reasoning as a multi-fid op being one `Dispatch`). The mutation reply is
scalar, so `PostCqe` copies nothing (the `loom_scalar_result` default).

The untrusted-server payload-count bound is the documented v1.x seam (shared with
#841 / Loom-4-F4; the `min(reply_len, op_count)` clamp makes a hostile count safe
regardless). The 3-module suite is re-run clean as the Loom-6a/6b pre-commit
gate; the formal focused audit lands at Loom-6c over the whole 6a+6b surface.

---

## loom_multishot.tla — Loom-5 (the multishot op lifecycle; spec-first)

Status: **TLC-green; the Loom-5 SPEC is landed, the Loom-5 IMPL is pending
(#909).** A NEW focused module (not an extension of `loom.tla`): the audited
core models the CQ as `cq \subseteq Ops` (at most one unreaped CQE per op),
which is gate-tied for its 8 cfgs and structurally cannot represent multishot's
MULTISET CQ (several unreaped CQEs from one op). Refactoring the frozen,
already-audited core to a count-CQ would invalidate its landed counterexamples;
instead multishot gets its own module with a COUNT CQ (`cq[o] \in Nat`), the
scheduler-suite precedent (`sched_oncpu.tla` + `sched_alpha.tla`).

Models a multishot op as one SQE -> MANY CQEs: arm once, a `LOOM_CQE_MORE`-set
CQE per event (the op re-arms), then EXACTLY ONE terminal (MORE-clear) CQE, and
NOTHING after. Pins **I-29 generalized to a stream** (`ExactlyOneTerminal` +
`TerminalEndsStream`), the **I-30 pin held ACROSS shots** (`ObjPinnedAcrossShots`
— the clunk+reuse-between-shots amplification), per-shot **CQ back-pressure**
(`CqNeverOverfull`), and **teardown-quiesce** (`NoStaleAfterTeardown`). A
single-shot op is the 1-shot special case (its lone CQE is its terminal).

Safety: TLC-clean at `Ops = {o1, o2}, CQ_CAP = 2, MAX_INFLIGHT = 2,
MAX_SHOTS = 2` — 2940 distinct states.
Liveness: TLC-clean (`EventuallyTerminal`, `ALLOW_TEARDOWN = FALSE`) — 1633 states.

| Config | Flag | Invariant / Property | Result |
|---|---|---|---|
| `loom_multishot.cfg`                          | all FALSE, teardown on       | `Invariants` (13)        | clean (2940) |
| `loom_multishot_liveness.cfg`                 | Spec_Live, no teardown       | `EventuallyTerminal`     | clean (1633) |
| `loom_multishot_buggy_double_terminal.cfg`    | `BUGGY_DOUBLE_TERMINAL`      | `ExactlyOneTerminal`     | violation |
| `loom_multishot_buggy_shot_lost_on_full.cfg`  | `BUGGY_SHOT_LOST_ON_FULL`    | `CqNeverOverfull`        | violation |
| `loom_multishot_buggy_resolve_at_shot.cfg`    | `BUGGY_RESOLVE_AT_SHOT`      | `ObjPinnedAcrossShots`   | violation |
| `loom_multishot_buggy_more_after_terminal.cfg`| `BUGGY_MORE_AFTER_TERMINAL`  | `TerminalEndsStream`     | violation |
| `loom_multishot_buggy_stale_after_teardown.cfg`| `BUGGY_STALE_AFTER_TEARDOWN`| `NoStaleAfterTeardown`   | violation |

| Spec action | Source location (Loom-5 impl, #909) | Notes |
|---|---|---|
| `UserProduce(o, ms)` / `Consume` | `kernel/loom.c::loom_submit_one` (the `LOOM_SQE_MULTISHOT` flag in `loom_sqe.flags`) | a multishot SQE is admitted once: the I-30 pin (`spoor_ref`) + tag are taken at submit and held for the WHOLE stream (released at the terminal). |
| `ReplyArrives` / `PostShot` | `kernel/loom.c::loom_async_complete` (the multishot branch: post a `LOOM_CQE_MORE`-set CQE, RE-ARM the op instead of reaping it) | each non-terminal reply posts a MORE CQE under `l->lock` (no sleep) and re-issues the request; the pin is reused, never re-resolved (`ObjPinnedAcrossShots`). |
| `PostTerminal` | `kernel/loom.c::loom_async_complete` (the terminal branch: clear MORE, move to terminal, reap the container) | the terminal reply (EOF / error / cancel) posts the MORE-clear CQE + ends the stream (`ExactlyOneTerminal`, `TerminalEndsStream`); back-pressure holds it on a full CQ. |
| `Teardown` | `kernel/loom.c::loom_free` quiesce + `p9_client_abandon_async` | an armed multishot op is quiesced (no late shot) — the #898 / #845 path, now covering a re-armable op (`NoStaleAfterTeardown`). |
| the five `BUGGY_*` flags | (none — disciplines the impl upholds) | exactly-one-terminal; never-post-a-shot-into-a-full-CQ; pin-not-re-resolved-per-shot; no-MORE-after-terminal (the Tapestry recycle-gate); quiesce-the-armed-op-on-teardown. |

---

## loom_order.tla — Loom-5 (LINK / DRAIN inter-op ordering; spec-first)

Status: **TLC-green; the Loom-5 SPEC is landed, the Loom-5 IMPL is pending
(#909).** A NEW focused module: LINK/DRAIN is an ADMISSION-ORDERING concern over
an ORDERED chain of ops (submission order = SQ-index order), modeled with integer
indices + a `Pred(i)` predecessor set (empty for `i = 1`, so no index leaves
DOMAIN) — orthogonal to the multishot stream lifecycle and the core CQ shape.

LINK (chain): a linked op starts only after its predecessor completes
SUCCESSFULLY; a link member's FAILURE cancels the rest of the chain, each
cancelled op posting EXACTLY ONE -ECANCELED CQE. DRAIN (barrier): a drain op
waits for ALL prior; post-drain ops wait for the drain. Pins `LinkOrdered`,
`DrainOrdered`, `EveryDoneOpPosted` (I-29 no-lost: a cancelled op is never
silently dropped), `NoOrphanCancel`, and the liveness `EverySubmittedPosts` (a
failed link must cancel its successors so they don't strand).

Safety: TLC-clean at `N = 3` (link/drain flags + completion result chosen
nondeterministically -> all 2^N x 2^N topologies) — 1505 distinct states.
Liveness: TLC-clean (`EverySubmittedPosts`) — 1505 states.

| Config | Flag | Invariant / Property | Result |
|---|---|---|---|
| `loom_order.cfg`                          | all FALSE                  | `Invariants` (7)        | clean (1505) |
| `loom_order_liveness.cfg`                 | Spec_Live                  | `EverySubmittedPosts`   | clean (1505) |
| `loom_order_buggy_link_reorder.cfg`       | `BUGGY_LINK_REORDER`       | `LinkOrdered`           | violation |
| `loom_order_buggy_drain_jumps_ahead.cfg`  | `BUGGY_DRAIN_JUMPS_AHEAD`  | `DrainOrdered`          | violation |
| `loom_order_buggy_cancel_no_cqe.cfg`      | `BUGGY_CANCEL_NO_CQE`      | `EveryDoneOpPosted`     | violation |
| `loom_order_buggy_cancel_skips.cfg`       | `BUGGY_CANCEL_SKIPS`       | `EverySubmittedPosts` (temporal) | violation |

| Spec action | Source location (Loom-5 impl, #909) | Notes |
|---|---|---|
| `Submit` / `Start` | `kernel/loom.c::loom_submit_one` (the `LOOM_SQE_LINK` / `LOOM_SQE_DRAIN` flags) + the admission gate | a linked successor is held until its predecessor's terminal CQE; a drain barrier until all prior terminal (`LinkOrdered` / `DrainOrdered`). |
| `Complete` / `CancelVictim` | `kernel/loom.c::loom_async_complete` (on a link member's terminal CQE: dispatch the next link, or cancel the chain) | a non-ok link member cancels its successors, each posting one -ECANCELED CQE (`EveryDoneOpPosted` / `NoOrphanCancel`); no successor strands (`EverySubmittedPosts`). |
| the four `BUGGY_*` flags | (none — disciplines the impl upholds) | link-ordering; drain-ordering; a-cancelled-op-still-posts-a-CQE; the-cancel-actually-fires (no strand). |

cfgs run with `-deadlock`; each module's `Done` self-loop keeps the
all-terminal state from tripping the deadlock check. The three modules together
are the Loom-5 pre-commit gate; see docs/LOOM.md §7 + §10.

## death_wake.tla — the #811/LS-5 group-terminate death-wake cascade (HOLOTYPE RW-2 SA-1)

Spec-first re-enabled for this surface (user-directed 2026-06-10; CLAUDE.md). The
model pins the EXISTING, audit-clean impl — the design-level proof of the
no-lost-death-wake (I-9 generalized) + exactly-once-ZOMBIE (I-24) the #809/#811
audits established by prose. The clean cfg is TLC-green; `death_wake_buggy.cfg`
(`BUGGY_OBSERVE_BEFORE_REGISTER`) is the executable counterexample of the
#809-audit F1 lost-wake / non-reaping hang.

| Spec action | Code site | Invariant pinned |
|---|---|---|
| `SleepBegin` / `AcquireLock` / `RegisterObserve` (the CORRECT register-then-observe under `wlock`) | `kernel/sched.c::sleep` + `tsleep` — take per-Thread `wait_lock`, register (`rendez_blocked_on` + `THREAD_SLEEPING`), re-check `group_exit_msg` (+ the LS-5 latch via `thread_die_pending`) BEFORE dropping the lock + sleeping | `NoLostDeathWake` / `NoStuckSleeper` (I-9): a registered-then-observed sleeper either sees `gflag` and dies or is found+woken by the walk. |
| `RegisterBuggy` / `SleepBegin` buggy leg (observe BEFORE register, OUTSIDE the lock) | (none — the anti-pattern the impl does NOT do; the buggy cfg only) | the lost-wake window: `BUGGY_OBSERVE_BEFORE_REGISTER` makes `NoLostDeathWake` fail. |
| `CascadeSet` (publish `gflag` once) | `kernel/proc.c::proc_group_terminate` — the set-once `group_exit_msg` RELEASE CAS | set-once; the RELEASE precedes the per-Thread walk. |
| `CascadeWalk(t)` (per-Thread wake under `~wlock[t]`) | `proc_group_terminate` / `proc_interrupt_terminate_wake` — walk `p->threads` under `g_proc_table_lock`, take each peer's `wait_lock`, read `rendez_blocked_on`, `wakeup()` (Option-A stack-pin: lock held across the wake) | the cascade wakes only a SLEEPING peer it can lock (never one mid reg-obs). |
| `Resume(t)` / `RunCheckpoint(t)` (die at the EL0-return tail) | `arch/arm64/exception.c::el0_return_die_check` (sync + IRQ-from-EL0 tails) — a flagged Thread `sched()`s away noreturn; a woken sleeper returns `*_INTR` then dies at the tail | `ZombieImpliesAllDead` (I-24): no Thread runs at EL0 after the ZOMBIE transition. |
| `ProcReap` (last out → ZOMBIE, set once) | `proc.c::thread_exit_self` / `exits` last-Thread-out `proc_become_zombie_locked` | exactly-once ZOMBIE; `EventuallyReaps` (liveness) is the witness the hang cannot occur. |

Pre-commit gate: `death_wake.cfg` clean GREEN + `death_wake_buggy.cfg`
counterexample confirmed, on any change to `sleep`/`tsleep`'s register-then-observe,
the `wait_lock`/`rendez_blocked_on` protocol, or `proc_group_terminate`'s cascade.

---

## pipe.tla — P5-pipe (section added at RW-10; the spec landed P5)

Models the two-direction pipe wait/wake state machine (I-9 specialized):
bounded ring + reader/writer sleep/wake pairs + EOF/EPIPE on close. Clean cfg
+ 4 buggy cfgs (`read_no_wake_writer` / `write_no_wake_reader` /
`close_read_no_wake_writer` / `close_write_no_wake_reader`), each dropping
one wake edge.

| Spec action | Code site | Invariant pinned |
|---|---|---|
| `ReadDrain` / `ReadEof` / `ReadSleep` | `kernel/pipe.c::pipe_read` (drain under the pipe lock; EOF when write end closed + ring empty; sleep on the read Rendez otherwise) | a reader sleeps only when the ring is empty AND the write end is open |
| `WriteAppend` / `WriteEpipe` / `WriteSleep` | `kernel/pipe.c::pipe_write` (append under the lock; `-1` + the synthetic `pipe` note when the read end is closed; sleep when full) | a writer sleeps only when the ring is full AND the read end is open |
| `CloseRead` / `CloseWrite` | `kernel/pipe.c` close paths (wake the OPPOSITE side's sleepers on every close) | the buggy cfgs prove dropping any close-wake edge strands a sleeper |

Pre-commit gate: `pipe.cfg` clean + the 4 buggy cfgs on any change to
`kernel/pipe.c`'s wait/wake or close paths.

---

## asid.tla — RW-1 B-F1 (the rolling-ASID allocator; spec-first re-enabled, model-first)

Models the Linux-arm64-style generation-rollover allocator (I-31). Written +
TLC-green BEFORE the impl (`d742ffa`); the focused audit (`d40dbbb`) fixed
the spec's own F1 (ownerless reservation reclaim the impl never had — the
`rproc` ownership model; cfg bounds >= 4 Procs). Clean cfg + 5 buggy cfgs
(`rollover_steals_active` / `fast_no_regen` / `fast_no_flush_check` /
`no_flush_pending` / `reserve_value_only`).

| Spec action | Code site | Invariant pinned |
|---|---|---|
| `FastSwitch(c, p)` | `arch/arm64/asid.c::asid_check_and_switch` generation-match fast path — lockless `xchg` publish into `active_asids[cpu]` | `NoActiveAlias`: the publish makes the ASID visible to a concurrent rollover BEFORE the TTBR0 write |
| `SlowSwitch(c, p)` | `asid.c::new_context` under `g_asid_lock` — claim a free ASID or roll the generation (reset bitmap; preserve each CPU's active ASID into `reserved_asids[cpu]`; set per-CPU `flush_pending`) | `ActiveClaimed` + rollover preserves active/reserved (a running CPU is never yanked) |
| `Deschedule(c)` | the context switch away from a Proc (the per-CPU active slot becomes reclaimable only via the rollover's reserve pass) | — |
| `CacheTranslation(c)` | (hardware TLB fill; no code site) — cleared by the `flush_pending` local `tlbi` consumed at the next switch | `NoStaleTLB`: no CPU runs on a translation cached under a previous generation's aliasing |

Pre-commit gate: `asid.cfg` clean + the 5 buggy cfgs on any change to
`arch/arm64/asid.c` or the context-switch pre-hook.

---

## sched_oncpu.tla — deep-smp-review (the #860 DIAGNOSTIC; counterexample-only)

Models the PRE-redesign scheduler: the on_cpu protocol + the off-tree
`g_bootcpu_idle` deadlock-path dispatch. **Maps to NO live code** — the
modeled mechanism was retired at #863; the spec exists to REPRODUCE the #860
class (`prefix860` cfgs) and to prove the in-tree guard (`intree_guard` cfg)
closes it. Per-option cfgs only (no default `sched_oncpu.cfg`), so `make
specs` skips it by design.

| Spec action | Code site | Role |
|---|---|---|
| `DeadlockDispatch(c, prev)` | (RETIRED — was `kernel/sched.c`'s g_bootcpu_idle fallback; history in comments at `sched.c:396,817,973`) | the #860 root cause, reproduced by `sched_oncpu_prefix860*.cfg` |
| `StartSwitch` / `FinishSwitch` / `StealSrc` / `Resume` | the on_cpu multi-step switch as it existed pre-redesign | the substrate the diagnostic needs |

---

## sched_alpha.tla — deep-smp-review (the SMP-redesign GATING model)

The target-design model the redesign (#863) was validated against: per-CPU
**pinned in-tree idle** (`IsPinned(t) == t \in CPUs` — each CPU's idle thread
lives in its own runq and is never stolen), the multi-step switch with the
on_cpu handoff, stealing, and `Place`. TLC-green (2-CPU + 3-CPU cfgs).

| Spec action | Code site | Invariant pinned |
|---|---|---|
| `StartSwitch(c)` / `FinishSwitch(c)` | `kernel/sched.c::sched` + `arch/arm64/context.S::cpu_switch_context` — the two-step switch with the `prev_to_clear_on_cpu` handoff | I-21-adjacent migration safety: a ctx/kstack is never written by two CPUs concurrently |
| `StealCand(c)` / `RemoveFromRunq` | `kernel/sched.c::try_steal` (pinned idles excluded — `cpu_pinned`) | steal never moves a pinned idle; no double-enqueue |
| `Place(t)` | `kernel/sched.c::ready` / `ready_on` (+ the RW-2 2A-F1 vd_t clamp) | a woken thread lands in exactly one runq |

Pre-commit gate: `sched_alpha.cfg` + `sched_alpha_3cpu.cfg` clean on any
change to the on_cpu protocol, the pinned-idle dispatch, or `try_steal`.

## allowance.tla — Menagerie build-arc 2 (the hardware allowance; spec-first re-enabled, model-first)

The per-Proc hardware allowance that scopes `CAP_HW_CREATE` (I-34;
MENAGERIE.md §4). Spec-first RE-ENABLED for this surface (user-voted
2026-06-15) because the central hazard is an SMP race (the
capability-revocation-vs-in-flight-create class). Written + TLC-green BEFORE
the impl (spec `1602e37`; the impl follows in the next commit). The model is
the KERNEL mechanism; the warden is the implicit actor driving Confer/Revoke.

| Spec action | Code site | Invariant pinned |
|---|---|---|
| `Confer(d, N, A)` | `kernel/allowance.c::proc_confer_allowance` (the warden's set-once-at-spawn grant) | the conferred set ⊆ node (ConferredWithinNode — the warden's grant policy) |
| `CreateBegin(d, r)` | `kernel/allowance.c::allowance_permits` (the lock-free gate) at `kernel/syscall.c::sys_{mmio,irq,dma}_create_handler` | the gate check: r ∈ allowance |
| `CreateCommit(d)` | `kernel/allowance.c::allowance_handle_alloc` (re-check `revoked` under `allowance->lock` then `handle_alloc`) | HandlesWithinAllowance — the revoke-vs-create race loses nothing |
| `Revoke(d)` | `kernel/allowance.c::proc_revoke_allowance` (set `revoked`) + the caller's `proc_group_terminate` | RevokedFullyCleared — the handle-axis teardown |
| (rfork inherit) | `kernel/allowance.c::allowance_clone_into` (in `rfork_internal`) | AllowanceWithinConferred — a child is never broader than its parent |

Invariants: HandlesWithinAllowance (the gate + the race), AllowanceWithinConferred
(never widened), ConferredWithinNode (the grant ⊆ node), RevokedFullyCleared
(fully revoked on teardown) + the `EventuallyResolves` liveness witness (the
re-check gate cannot wedge an in-flight `SYS_*_CREATE` against a concurrent revoke).

Pre-commit gate: `allowance.cfg` clean GREEN + the 4 buggy cfgs
(`allowance_buggy_revoke_race` [the headline SMP race -> HandlesWithinAllowance]
/ `allowance_buggy_revoke_leak` / `allowance_buggy_confer_widen` /
`allowance_buggy_self_widen`) confirmed failing on any change to the gate
protocol, the confer/revoke path, or the clone-inherit.

## loom_devgone.tla — Menagerie build-arc 4 (the Loom device-gone terminal CQE; spec-first re-enabled, model-first)

The I-29 device-gone extension (MENAGERIE.md §10): a 9P session death completes
every in-flight Loom op with a reason-tagged terminal CQE -- a backing-device-gone
session (a clean peer-gone EOF) yields the device-gone `-P9_E_NODEV` terminal,
distinct from a generic transport `-P9_E_IO`. A FOCUSED module (the
`loom_multishot` / `loom_order` precedent; the audited `loom.tla` core untouched --
`loom.tla`'s `Teardown` is the RING destroy [abandon, no CQE], this is the
orthogonal SESSION death [complete WITH a reason-tagged CQE]). Written + TLC-green
BEFORE the impl (spec `6db71fa`).

| Spec action | Code site | Invariant pinned |
|---|---|---|
| `Admit(o)` | `kernel/loom.c::loom_submit_one` -> `p9_client_submit_async` (an async op goes in-flight) | (the op holds a tag; only on a live session) |
| `ReplyComplete(o)` | `kernel/9p_client.c::demux_frame_locked` -> `loom_async_complete` -> `loom_post_cqe` (the #841 demux + the CQE post, one `c->lock` step) | (the ok terminal) |
| `SessionDies(devgone)` | `kernel/9p_client.c::client_mark_dead_locked(c, true)` (each in-flight async op `on_complete(-P9_E_NODEV)`) -- reached automatically via `reader_recv_frame` returning `0` (peer-gone EOF) at the 3 reader sites, or explicitly via `p9_client_mark_devgone` | DeathResultFaithful (the devgone reason -> `err_devgone`), SessionDeathCompletes (no op left in-flight) |
| `SessionDies(transport)` | `client_mark_dead_locked(c, false)` (the existing ~8 protocol/error sites + `reader_recv_frame` returning `-1`) | DeathResultFaithful (a transport death -> `err_transport`, the unchanged `-EIO`) |
| `LateReply(o)` (buggy double) | the impl forecloses it: `demux_frame_locked` clears `inflight[tag]` BEFORE completing, so a late reply on a death-completed op dispatches OWNERLESS (no 2nd CQE) | NoDoubleTerminal |
| `Reap(o)` | userspace advances `cq_head` | (drains the CQ) |

Invariants: DeathResultFaithful (the headline -- a death-completed op carries its
session's reason), SessionDeathCompletes (the §10 no-hang), NoDoubleTerminal (the
reply-vs-death race), DevgoneOnlyFromDevgoneSession, ResultSetIffTerminal,
ViaDeathImpliesDead + the `EventuallyTerminates` liveness witness.

Pre-commit gate: `loom_devgone.cfg` clean GREEN + `loom_devgone_liveness.cfg` +
the 3 buggy cfgs (`loom_devgone_buggy_drops_reason` [-> DeathResultFaithful: the
pre-step-4 behavior masking devgone] / `loom_devgone_buggy_leaks_inflight` [->
SessionDeathCompletes: an op hangs past removal] / `loom_devgone_buggy_double` [->
NoDoubleTerminal]) confirmed failing on any change to the death-reason threading,
the `reader_recv_frame` EOF-vs-error split, or the `loom_async_complete` terminal.

---

## net_poll.tla — net-6b (the dev9p.poll readiness bridge; spec-first re-enabled, model-first)

Status: **spec landed model-first at net-6b-1; the mechanism lands at
net-6b-2 (source map filled then).** Models the `dev9p.poll` PROBE-then-observe
bridge (NET-DESIGN.md §12.2). Unlike the console (`cons_poll.tla`), whose
readiness is a LOCAL edge an RX IRQ produces, a 9P socket's readiness lives in
netd and must be ELICITED: the kernel issues a readiness READ (a deferred 9P
Tread on a non-consuming netd `ready` file, the offset carrying the requested
event mask) that netd answers only when the socket is ready per that mask. The
reply is demuxed by the kernel 9P client's #841 elected reader — driven, because
a `poll()` caller parks (it is precisely not doing a blocking read), by a
per-client poll-pump kthread (the Loom-4 SQPOLL analog). The reader's demux
fires the async op's `on_complete` UNDER `c->lock`, which RECORDS the readiness
bitmap into the dev9p fid and sets a relay flag — it does NOT walk the hook list
there (illegal under `c->lock`); the kthread walks it after the pump, `c->lock`
released, in PROCESS context (the LS-8a `console_mgr` deferred-wake discipline).

`poll.tla` owns the poller-side register-then-observe + the N-fd fan;
`cons_poll.tla` owns the kthread-relayed deferred wake (the relay's own
register-then-observe sleep, reused verbatim for the poll-pump kthread's sleep).
`net_poll.tla` adds the one thing neither covers: readiness here is not produced
spontaneously — it must be PROBED. The load-bearing discipline is
PROBE-then-observe: `dev9p.poll` must ensure a readiness read is OUTSTANDING
(atomically with installing the hook and sampling the cached bitmap) BEFORE the
poller observes not-ready and parks; else the readiness edge fires in netd with
no request to answer, the reader demuxes nothing, the relay never runs, and the
poller sleeps forever on a ready socket. I-9 generalized to the elicited-
readiness relay.

State universe: one poller, one fd, the poll-pump relay (NetdReplyDemux =
the reader's demux + `on_complete` record under `c->lock`; KthreadWalk = the
post-pump process-context walk). CONSTANT: `BUGGY_LOST_READY` (the register step
installs the hook + samples but never ensures a probe).

| Config | Flags | Checked | Result | Distinct |
|---|---|---|---|---|
| `net_poll.cfg`                  | `BUGGY_LOST_READY=FALSE` | `Invariants` | clean | 10 |
| `net_poll_liveness.cfg`         | `Spec_Live`, all FALSE   | `PollerEventuallyServed` | clean | 10 |
| `net_poll_buggy_lost_ready.cfg` | `BUGGY_LOST_READY=TRUE`  | `NoMissedNetPoll` | violation (depth 4) | 6 |

Spec action ↔ impl mapping (filled at net-6b-2b, `kernel/dev9p_poll.c`):
`kernel/dev9p_poll.c::dev9p_poll` (register the hook on the Spoor's poll-state +
`dev9p_poll_submit_locked` ensures a non-terminal readiness probe is outstanding +
sample `ps->cached_revents`, one `g_dev9p_poll_lock` step) = `PollerRegister`;
`kernel/dev9p_poll.c::dev9p_poll_complete` (fired by `kernel/9p_client.c::demux_frame_locked`
under `c->lock`; record the bitmap into `ps->cached_revents` + set the op terminal,
atomics only) = `NetdReplyDemux`; the poll-pump kthread's
(`dev9p_poll_service_once`) post-pump `poll_waiter_list_wake` (process context,
`c->lock` released) = `KthreadWalk`; netd serving the `ready` file's deferred
reply = the `SocketReady`→reply edge; `kernel/poll.c::sys_poll_for_proc`'s
evaluate/sleep = `PollerCommit`. The kthread's own go-to-sleep register-then-observe is
`cons_poll.tla::MgrSleep` (the same `sleep(&rendez, cond)` contract). The
`BUGGY_LOST_READY` counterexample is the durable regression for the
probe-then-observe order.

cfgs run with `-deadlock`; the `Done` self-loop keeps the legitimate terminal
state from tripping the deadlock check (the lost-ready stuck state is
PRE-terminal, so it is still caught). See NET-DESIGN.md §12.2 + ARCH §28 I-9.
