# Spec-to-code mapping

For each TLA+ spec, this file maps each action / invariant to a source location. CI will eventually verify the mapping is current — file must exist, function must exist, line range must match. Stale mapping = failing CI (Phase 2 close adds the CI gate).

Per `CLAUDE.md` Spec-first policy: if the four (spec, technical reference, code, user reference) disagree, **the spec wins**.

---

## scheduler.tla — P2-Cg impl mapping (SMP discipline lifted to spec)

Status: **wait/wake proven; SMP runqueue + IPI ordering proven; EEVDF math + LatencyBound deferred** (P2-Cg). Models thread state machine + per-CPU dispatch + cross-CPU work-stealing (`Steal`) + per-(src,dst) FIFO IPI delivery (`IPI_Send` / `IPI_Deliver`) with state-consistency, wait/wake-atomicity, no-double-enqueue, and FIFO-ordering invariants. Proves `NoMissedWakeup` (ARCH §28 I-9), `NoDoubleEnqueue` (ARCH §8.4), and `IPIOrdering` (ARCH §28 I-18) under the correct primitives; produces a counterexample for each under the corresponding buggy primitive. Does NOT yet model EEVDF deadline math (post-P2-Cg) or LatencyBound liveness (Phase 2 close).

TLC-clean at `Threads = {t1, t2, t3}, CPUs = {c1, c2}, MaxIPIs = 2` — 10188 distinct states explored; depth 16. Buggy configs:

| Config | Flag | Invariant violated | Depth | Distinct states |
|---|---|---|---|---|
| `scheduler_buggy.cfg`       | `BUGGY = TRUE`           | `NoMissedWakeup`  | 6 | 245  |
| `scheduler_buggy_steal.cfg` | `BUGGY_STEAL = TRUE`     | `NoDoubleEnqueue` | 4 | 38   |
| `scheduler_buggy_ipi.cfg`   | `BUGGY_IPI_ORDER = TRUE` | `IPIOrdering`     | 6 | 470  |

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

### Deferred (post-P2-Cg)

- **LatencyBound** (I-17, slice × N): liveness property requiring weak fairness. Phase 2 close adds explicit `Slice` variable + `IRQ`/`Preempt` actions + weak fairness annotations. Bounded slice value to keep state space tractable.
- **Full EEVDF math**: `vd_t = ve_t + slice × W_total / w_self` with weighted virtual time advance. Becomes meaningful when weights differ — Phase 5+ when sched_setweight is exposed. v1.0 weight=1 always; current `g_vd_counter++` advance is a valid instantiation.
- **`PickIsMinDeadline` invariant**: implies the impl's pick-min-vd_t is correct. Currently provable manually (the impl is mechanically pick-min) but not modeled.
- **Per-IPI-type ordering**: when multiple IPI types coexist (P5+ TLB shootdown, halt, generic), the ordering invariant should refine to per-(src, type, dst) FIFO. Today's per-(src, dst) version is sound only when all IPIs are equal-priority + same-type.
- **`finish_task_switch` ordering invariant**: P2-Ce `pending_release_lock` handoff + P2-Cf `prev_to_clear_on_cpu` handoff maintain "destination CPU completes prev's release before resuming." The spec's atomic-action model already enforces this implicitly; an explicit invariant would require modeling intermediate save states.
- **`Spawn(t)` action**: open-universe thread creation (current spec is closed-universe). P2-D rfork lands the impl side; spec extension follows.

---

## namespace.tla — Phase 2 close (planned)

(Stub. Will pin: bind/mount semantics, cycle-freedom, isolation between processes — ARCH §28 I-1, I-3.)

## handles.tla — Phase 2 close (planned)

(Stub. Will pin: rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability — ARCH §28 I-2, I-4, I-5, I-6.)

## vmo.tla — Phase 3 (planned)

(Stub. Will pin: refcount + mapping lifecycle — ARCH §28 I-7.)

## 9p_client.tla — Phase 4 (planned)

(Stub. Will pin: tag uniqueness per session, fid lifecycle — ARCH §28 I-10, I-11.)

## poll.tla — Phase 5 (planned)

(Stub. Will pin: wait/wake state machine, missed-wakeup-freedom — ARCH §28 I-9.)

## futex.tla — Phase 5 (planned)

(Stub. Will pin: FUTEX_WAIT / FUTEX_WAKE atomicity — ARCH §28 I-9.)

## notes.tla — Phase 5 (planned)

(Stub. Will pin: note delivery ordering, signal mask correctness — ARCH §28 I-19.)

## pty.tla — Phase 5 (planned)

(Stub. Will pin: master/slave atomicity, termios state transitions — ARCH §28 I-20.)
