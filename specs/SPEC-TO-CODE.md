# Spec-to-code mapping

For each TLA+ spec, this file maps each action / invariant to a source location. CI will eventually verify the mapping is current — file must exist, function must exist, line range must match. Stale mapping = failing CI (Phase 2 close adds the CI gate).

Per `CLAUDE.md` Spec-first policy: if the four (spec, technical reference, code, user reference) disagree, **the spec wins**.

---

## scheduler.tla — P2-Cg/H impl mapping (SMP discipline lifted to spec)

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

Status: **rights ceiling proven; hardware-handle non-transferability proven; transfer-only-via-9P proven; capability monotonicity proven; hw-handle non-duplicability proven; hw-resource exclusivity proven; hw-handle-implies-cap proven; rfork-capability-grant ceiling proven; impl landed at P2-Fc + P4-Ib + P4-Ic3 + P4-Ic5b1b (DMA-side now wired).** Models the kernel handle table with typed kobjs partitioned into transferable + hardware sets, per-handle provenance tags, abstract 9P sessions, per-Proc coarse capabilities, and dynamic per-Proc capability ceilings (rfork-time inheritance). ARCH §28 invariants pinned: I-2 (capability monotonic reduction), I-4 (transfer only via 9P), I-5 (hardware handles non-transferable — covers MMIO + IRQ + DMA), I-6 (rights monotonic on transfer). I-7 (BURROW refcount + mapping lifecycle) is OUT OF SCOPE — covered by `burrow.tla`.

P4-Ic5b1b adds no new spec state, actions, or invariants — the existing `HwKObjs` partition + the 5 hw-kobj invariants (HwHandlesAtOrigin, NoHwDup, HwResourceExclusive, HwHandleImpliesCap, RightsCeiling) apply uniformly to KObj_DMA. PA stability is a structural impl property (no kernel code path mutates KObj_DMA's `pa` after `kobj_dma_create`). The kernel-allocates-PA discipline (vs MMIO's user-specifies-PA) is a syscall-surface design choice that doesn't surface in the abstract handle-table model. See the `HandleAlloc` row below for the DMA-side impl cross-reference.

TLC-clean at `Procs = {p1, p2}, TxKObjs = {kx}, HwKObjs = {kh}, Rights = {R, T}, Caps = {C, HW}, CapHwCreate = HW` — **11,715,248 distinct states explored / 317,925,297 generated; depth 25; ~9 min on 8 cores** (state space grew vs. the pre-P4-Ic3 baseline because `proc_ceiling` adds a state dimension). Buggy configs:

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
| `RforkWithCaps(parent, child, granted)` | `kernel/proc.c::rfork_internal` (P4-Ic3, lines 469-538) + `rfork_with_caps` (lines 545-548) | Kernel-internal capability grant primitive. `child->caps = __atomic_load_n(&parent->caps, __ATOMIC_ACQUIRE) & caps_mask` enforces `granted \subseteq proc_caps[parent]` regardless of caller-supplied mask. The plain `rfork` is a delegate with `caps_mask = CAP_NONE`. |
| `BuggyRforkElevate(parent, child, gained)` | (none — bug class statically prevented at P4-Ic3) | The AND with `parent_caps` at line 499 of `rfork_internal` makes it impossible for the caller to elevate the child's caps above the parent's. A future refactor that replaces `&` with `=` or `\|` would re-introduce the bug. |

| Spec invariant | Source enforcement |
|---|---|
| `RightsCeiling` (every handle's rights ⊆ origin_rights of its kobj) | `handle_dup`'s subset check before insert; `handle_transfer_via_9p`'s subset check (Phase 4 wire-up). The kobj's origin_rights is set once at `handle_alloc` and never modified. |
| `HwHandlesAtOrigin` (every hw-typed handle held by origin proc) | `handle_transfer_via_9p`'s switch statement omits the hw kinds entirely. `_Static_assert(KIND_COUNT == ...)` ensures every kind is enumerated; missing case is a compile error. |
| `OnlyTransferVia9P` (no handle has via="direct") | The impl has no direct-transfer syscall. The only public handle-cross-proc API is `handle_transfer_via_9p`. |
| `CapsCeiling` (every proc's caps ⊆ proc_ceiling[p]) | **At P4-Ic3 this is dynamic**: ProcRoot's ceiling is `Caps` at Init; non-root Procs start with ceiling `{}` (via `KP_ZERO`) and have their ceiling set by `RforkWithCaps` to `proc_caps[parent]` at fork time. Impl-side, `rfork_internal`'s AND-with-parent_caps clamps the child's caps to its newly-established ceiling. Since v1.0 has no syscall that grows caps post-fork (no GrantCaps), `proc_caps[p]` only changes monotonically downward (via the future ReduceCaps) or via the one-shot rfork initialization — so the dynamic ceiling holds. |
| `NoHwDup` (no hw-kobj handle has via="dup") | `handle_dup` rejects when `h->obj->kind` is hw-kobj-kind via `handle_kind_is_hw_kobj()`. P4-Ib. |
| `HwResourceExclusive` (at most one alive handle per hw kobj across all procs) | `kobj_mmio_create`'s `g_mmio_claims` overlap scan + `kobj_irq_create`'s `g_intid_claimed` bitmap. P4-Ib + P4-Ic2 kernel-MMIO-reservation. **DMA case (P4-Ic5b1b)**: `kobj_dma_create` calls `alloc_pages` which returns a fresh contiguous page chunk per allocation; the buddy allocator's free-list partitioning structurally prevents two live `KObj_DMA` from sharing PA. No explicit claim table is needed because the page allocator IS the claim layer. |
| `HwHandleImpliesCap` (every hw-handle holder has CAP_HW_CREATE) | Syscall handlers gate on `__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE` before allocating. `ReduceCaps` (spec; future syscall) refuses to drop CAP_HW_CREATE while holding hw handles (R9 F150 implementer note). |

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
- ⬜ **P5-attach-syscall** (deferred) — `kernel/sys_attach_9p.c`: the user-visible `attach_9p(transport_fd, aname, n_uname) → spoor_fd` SVC handler. Requires fd-syscall infrastructure (open/close/read/write/dup + KOBJ_SPOOR populated from userspace) to be useful; deferred to a later chunk that builds that surface. The kernel-internal body is P5-attach-create above.
- ⬜ **P5-mount-syscall** (deferred) — `kernel/sys_mount.c`: the user-visible `mount(source_spoor_fd, target_path, flags) → 0` + `unmount(target_path) → 0` SVC handlers. Same dependency as P5-attach-syscall (fd-syscall infrastructure + KOBJ_SPOOR population from userspace). The kernel-internal C API landed at P5-attach-mount.
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

## poll.tla — Phase 5 (planned)

(Stub. Will pin: wait/wake state machine, missed-wakeup-freedom — ARCH §28 I-9.)

## futex.tla — Phase 5 (planned)

(Stub. Will pin: FUTEX_WAIT / FUTEX_WAKE atomicity — ARCH §28 I-9.)

## notes.tla — Phase 5 (planned)

(Stub. Will pin: note delivery ordering, signal mask correctness — ARCH §28 I-19.)

## pty.tla — Phase 5 (planned)

(Stub. Will pin: master/slave atomicity, termios state transitions — ARCH §28 I-20.)
