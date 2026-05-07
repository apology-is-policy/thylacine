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
| `scheduler.cfg`              | safety-only             | `Invariants`          | clean (3T × 2C) | 10188 |
| `scheduler_liveness.cfg`     | Spec_Live + SF          | `Invariants` + `LatencyBound` | clean (2T × 1C) | 23 |
| `scheduler_buggy.cfg`        | `BUGGY = TRUE`          | `NoMissedWakeup`     | violation (depth 6) | 101 |
| `scheduler_buggy_steal.cfg`  | `BUGGY_STEAL = TRUE`    | `NoDoubleEnqueue`    | violation (depth 4) | 40 |
| `scheduler_buggy_ipi.cfg`    | `BUGGY_IPI_ORDER = TRUE`| `IPIOrdering`        | violation (depth 6) | 442 |
| `scheduler_buggy_starve.cfg` | Spec (no fairness)      | `LatencyBound`       | stuttering (depth 3) | 23 |

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

### P3-G landed (this chunk)

- **`wfi: [CPUs -> BOOLEAN]`** variable added: TRUE while CPU is halted in WFI awaiting IPI. Maps to `kernel/sched.c::struct CpuSched::idle_in_wfi`.
- **`EnterWFI(cpu)`** action: pre `current[cpu]=NULL ∧ runq[cpu]={} ∧ ~wfi[cpu] ∧ Cardinality(CPUs)>1`. Effect: `wfi[cpu]=TRUE`. Maps to `kernel/smp.c::per_cpu_main`'s set-and-WFI sequence in the idle loop.
- **`NotifyWFIPeer(src, dst)`** action: pre `wfi[dst] ∧ ∃c: runq[c]≠{}`. Sends IPI; structurally identical to `IPI_Send` with stronger precondition. Maps to `kernel/sched.c::sched_notify_idle_peer` called from `ready()`.
- **`Resume(cpu)`** + **`Steal(stealer, victim)`** preconditions extended with `~wfi`. The impl-side analogue: a CPU in WFI is halted; can't run `sched()` and can't `try_steal` until IPI wakes it.
- **`IPI_Deliver(src, dst)`** + **`BuggyIPI_Deliver`**: also clear `wfi[dst]=FALSE`. Models the GIC SGI delivery exiting WFI on the dst CPU.
- **`Liveness_Wfi`**: extends `Liveness` with `\A pair : SF_vars(NotifyWFIPeer(pair[1],pair[2])) ∧ \A pair : SF_vars(IPI_Deliver(pair[1],pair[2]))`. Strong fairness on both clauses ensures a WFI'd CPU never indefinitely starves a runnable thread.
- **`Spec_Live_Wfi == Init ∧ [][Next] ∧ Liveness_Wfi`**.
- **`scheduler_liveness_wfi.cfg`** (3T × 2C, MaxIPIs=2): correct, ~5760 distinct states, LatencyBound holds.
- **`scheduler_buggy_wfi.cfg`** (3T × 2C, MaxIPIs=2): SPECIFICATION uses `Spec_Live` (no WFI fairness). LatencyBound violated — TLC produces counterexample showing CPU stuck in WFI while a thread starves on a peer's runq. Models the F78 impl bug (ready/wakeup don't IPI a WFI'd peer).
- Existing `scheduler.cfg` state count grew 10188 → 25416 due to the wfi variable. All existing buggy configs (`scheduler_buggy.cfg` / `_steal.cfg` / `_ipi.cfg` / `_starve.cfg`) still produce their original counterexamples; the wfi addition is orthogonal.
- Closes R5-H **F77** (try_steal contention sentinel + sched retry; impl-only, no spec change required) + **F78** (WFI machinery + NotifyWFIPeer fairness).
- ARCH §28 **I-9** (NoMissedWakeup) and **I-18** (IPIOrdering) preserved by the extension. **I-17** (LatencyBound) now proven across two universes: 1C cooperative-only (`scheduler_liveness.cfg`, 23 states) AND 2C with WFI/IPI (`scheduler_liveness_wfi.cfg`, 5760 states).

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

## territory.tla — P2-Ea spec (impl at P2-Eb)

Status: **cycle-freedom proven; isolation structural; impl deferred to P2-Eb.** Models the Plan 9 territory primitives — `bind` as a directed graph with cycle-freedom (I-3) the primary state invariant. Isolation (I-1) is structural in the model: bindings[p] and bindings[q] for p # q are independent function values; no action updates two procs simultaneously. RFNAMEG (shared territory) is deliberately not modeled at this phase — at v1.0 P2-E impl, rfork extincts on non-RFPROC flags, so the spec mirrors private-territory-per-Proc semantics. Phase 5+ adds the Territory layer.

TLC-clean at `Procs = {p1, p2}, Paths = {a, b, c}` — 625 distinct states explored; depth 7. `namespace_buggy.cfg` (BUGGY_CYCLE=TRUE) produces a 2-bind cycle counterexample at depth 4 / 95 states.

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/territory.c::territory_init` (P2-Eb) | Empty bindings per Proc. At v1.0 P2-Eb the kernel proc (PID 0) starts with the boot-time ramfs bindings; spec models the abstract "territory exists, empty" precondition. |
| `Bind(p, src, dst)` | `kernel/territory.c::bind` (P2-Eb) | The cycle-checked bind primitive. Calls cycle-detection (DFS or transitive-closure walk over the existing bindings) BEFORE inserting the edge. ARCH §9.1 says: "bind(old, new, flags) — attach a file or directory at another point in the tree." Spec's `(src, dst)` is `(old, new)` in ARCH terms. |
| `Unbind(p, src, dst)` | `kernel/territory.c::unmount` (P2-Eb) | Removes a bind edge. ARCH §9.1: "unmount(name, old) — remove a mount point." |
| `ForkClone(parent, child)` | `kernel/proc.c::rfork` (P2-Eb extension) | Models rfork(RFPROC) without RFNAMEG: child gets a private copy of parent's territory. v1.0 impl is at P2-Eb where rfork's flag handling extends to clone the Territory. |
| `BuggyBind(p, src, dst)` | (none — bug class statically prevented if the impl uses a single `bind` entry point that always calls cycle-detection) | The bug class is "forgot to call cycle-check." The impl's bind() function structures the cycle-check as inseparable from the insert (verify-then-insert under the same lock). A future caller that bypasses bind() and modifies the mount table directly would re-introduce the bug. |

| Spec invariant | Source enforcement |
|---|---|
| `NoCycle` (no path is reachable from its own bindings via transitive closure) | `kernel/territory.c::bind`'s cycle-check before insert. The check walks the existing bind graph from `src` searching for `dst`; if found, the bind would close a cycle and bind() returns -1 with errstr("territory cycle: cannot bind X onto Y"). |

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

## handles.tla — P2-Fa spec (impl at P2-Fc)

Status: **rights ceiling proven; hardware-handle non-transferability proven; transfer-only-via-9P proven; capability monotonicity proven; impl deferred to P2-Fc.** Models the kernel handle table with typed kobjs partitioned into transferable + hardware sets, per-handle provenance tags, abstract 9P sessions, and per-Proc coarse capabilities. ARCH §28 invariants pinned: I-2 (capability monotonic reduction), I-4 (transfer only via 9P), I-5 (hardware handles non-transferable), I-6 (rights monotonic on transfer). I-7 (BURROW refcount + mapping lifecycle) is OUT OF SCOPE — covered by `burrow.tla`.

TLC-clean at `Procs = {p1, p2}, TxKObjs = {kx}, HwKObjs = {kh}, Rights = {R, T}, Caps = {C}` — 6,055,072 distinct states explored; depth 26; ~4 min on 8 cores. Buggy configs:

| Config | Flag | Invariant violated | Depth | Distinct states |
|---|---|---|---|---|
| `handles_buggy_elevate.cfg` | `BUGGY_ELEVATE = TRUE`         | `RightsCeiling`     | 4 | 91 |
| `handles_buggy_hw.cfg`      | `BUGGY_HW_TRANSFER = TRUE`    | `HwHandlesAtOrigin` | 5 | 444 |
| `handles_buggy_direct.cfg`  | `BUGGY_DIRECT_TRANSFER = TRUE`| `OnlyTransferVia9P` | 4 | 90 |
| `handles_buggy_caps.cfg`    | `BUGGY_CAPS_ELEVATE = TRUE`   | `CapsCeiling`       | 4 | 89 |

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/handle.c::handle_init` (P2-Fc) | Per-Proc handle table allocated empty; ProcRoot starts with full caps. v1.0 P2-Fc has no rfork capability mask wiring; the spec's `proc_caps` is forward-looking for the Phase 5+ syscall surface. |
| `HandleAlloc(p, k, granted)` | `kernel/handle.c::handle_alloc(struct Proc *, kobj_kind, kobj_t, rights_t)` (P2-Fc) | Kernel allocates a fresh kobj k of statically-typed kind, grants `granted` rights to proc p. Sets origin_rights[k] = granted permanently. Returns handle index. |
| `HandleClose(p, h)` | `kernel/handle.c::handle_close(struct Proc *, hidx_t)` (P2-Fc) | Releases h from p's table. Decrements kobj's refcount; cascades to burrow_unref if last handle. |
| `HandleDup(p, h, new_rights)` | `kernel/handle.c::handle_dup(struct Proc *, hidx_t, rights_t new_rights)` (P2-Fc) | Creates a fresh handle in p's table sharing h's kobj with rights ⊆ h.rights. Rejects elevated rights with -EINVAL. |
| `BuggyDupElevate(p, h, new_rights)` | (none — bug class statically prevented) | The impl's `handle_dup` checks `(new_rights & h->rights) == new_rights` (subset test) before insert. A future caller that bypasses this check would re-introduce the bug. |
| `OpenSession / CloseSession` | (Phase 4: `kernel/9p_client.c::9p_attach / 9p_clunk`) | At v1.0 P2-Fc the spec models sessions abstractly; no impl callers. Phase 4's 9P client wires actual session lifecycle. |
| `HandleTransferVia9P(src, dst, h, new_rights)` | (Phase 4: `kernel/handle.c::handle_transfer_via_9p`) | The transfer codepath is **defined** at P2-Fc as a stub (returns -ENOTSUP at v1.0); the policy gates (TxKObjs ∈ TransferableTypes, RightTransfer ∈ rights, session open, new_rights ⊆ rights) are coded against the spec actions. Phase 4 wires the actual 9P payload extraction. |
| `BuggyHwTransfer(src, dst, h, new_rights)` | (none — bug class statically prevented) | The transfer switch in §18.3 has NO case for KObj_MMIO/IRQ/DMA/Interrupt. `_Static_assert` over the kobj_kind enum ensures every kind is accounted for; a future addition that's transferable must be added explicitly. |
| `BuggyDirectTransfer(src, dst, h, new_rights)` | (none — bug class statically prevented) | No syscall exists in the impl that transfers a handle directly between procs. The only cross-proc handle path is via 9P (Phase 4). |
| `ReduceCaps(p, lost)` | (Phase 5+: `kernel/proc.c::rfork` capability mask) | At v1.0 there is no in-kernel caller of capability reduction; the spec models the future behavior. |
| `BuggyCapsElevate(p, gained)` | (none — bug class statically prevented) | The impl's `rfork` capability mask is `parent->caps & mask` (intersection); no codepath sets caps to a superset. |

| Spec invariant | Source enforcement |
|---|---|
| `RightsCeiling` (every handle's rights ⊆ origin_rights of its kobj) | `handle_dup`'s subset check before insert; `handle_transfer_via_9p`'s subset check (Phase 4 wire-up). The kobj's origin_rights is set once at `handle_alloc` and never modified. |
| `HwHandlesAtOrigin` (every hw-typed handle held by origin proc) | `handle_transfer_via_9p`'s switch statement omits the hw kinds entirely — there is no codepath that copies a hw handle to a non-origin proc. `_Static_assert(KIND_COUNT == ...)` ensures every kind is enumerated; missing case is a compile error. |
| `OnlyTransferVia9P` (no handle has via="direct") | The impl has no direct-transfer syscall. The only public handle-cross-proc API is `handle_transfer_via_9p`. |
| `CapsCeiling` (every proc's caps ⊆ initial ceiling) | `rfork`'s capability mask is `&` (bitwise AND); no codepath ORs in elevated bits. |

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

## burrow.tla — P2-Fb spec (impl at P2-Fd)

Status: **BURROW refcount + mapping lifecycle proven; impl deferred to P2-Fd.** Models the dual-refcount discipline for Virtual Memory Objects per ARCH §19. Pins ARCH §28 invariant I-7 (pages live until last handle closed AND last mapping unmapped).

TLC-clean at `VmoIds = {v1, v2}, MaxRefs = 2` — 100 distinct states explored; depth 9. Buggy configs:

| Config | Flag | Invariant violated | Depth | Distinct states |
|---|---|---|---|---|
| `burrow_buggy_free_on_close.cfg` | `BUGGY_FREE_ON_HANDLE_CLOSE = TRUE` | `NoUseAfterFree` (premature) | 6 | 56 |
| `burrow_buggy_free_on_unmap.cfg` | `BUGGY_FREE_ON_UNMAP = TRUE`        | `NoUseAfterFree` (premature) | 6 | 58 |
| `burrow_buggy_never_free.cfg`    | `BUGGY_NEVER_FREE = TRUE`           | `NoUseAfterFree` (delayed)   | 6 | 43 |

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/burrow.c::burrow_init` (P2-Fd) | SLUB cache for struct Burrow. No VMOs alive at boot. |
| `VmoCreate(v)` | `kernel/burrow.c::burrow_create_anon(size)` (P2-Fd) | Allocates struct Burrow; allocates `size / PAGE_SIZE` pages from buddy; sets handle_count=1, mapping_count=0. Returns the new BURROW; the caller's `handle_alloc` wraps it in a Handle. |
| `HandleOpen(v)` | `kernel/burrow.c::burrow_ref(struct Burrow *)` (P2-Fd) | Atomic increment of handle_count. Called from `handle_dup` (intra-Proc) and (Phase 4) `handle_transfer_via_9p` (cross-Proc). |
| `HandleClose(v)` | `kernel/burrow.c::burrow_unref(struct Burrow *)` (P2-Fd) | Atomic decrement of handle_count. If both counts reach 0, calls `burrow_free_pages` + `kmem_cache_free`. |
| `BuggyFreeOnHandleClose(v)` | (none — bug class statically prevented) | The impl's `burrow_unref` checks BOTH counts before freeing: `if (h_count == 0 && m_count == 0) burrow_free_pages(...)`. A future caller that bypasses the check would re-introduce the bug. |
| `MapVmo(v)` | `kernel/burrow.c::burrow_map(struct Burrow *)` (P2-Fd) | Atomic increment of mapping_count. Called from mmap_handle for BURROW-backed VMAs. |
| `UnmapVmo(v)` | `kernel/burrow.c::burrow_unmap(struct Burrow *)` (P2-Fd) | Atomic decrement of mapping_count. Same dual-check on free as burrow_unref. |
| `BuggyFreeOnUnmap(v)` | (none — bug class statically prevented) | Same dual-check as BuggyFreeOnHandleClose; structural guarantee. |
| `BuggyNoFreeHandleClose(v) / BuggyNoFreeUnmap(v)` | (none — bug class statically prevented) | The impl's `burrow_unref` and `burrow_unmap` always evaluate the dual-check at decrement time; no codepath skips the free transition when both counts reach 0. |

| Spec invariant | Source enforcement |
|---|---|
| `RefcountConsistent` (counts = 0 if not in vmos) | The impl's struct Burrow is allocated/freed via SLUB; counts are part of the struct. Before allocation: no struct Burrow exists, so counts are not addressable; "not in vmos" maps to "no Burrow struct allocated yet." |
| `NoUseAfterFree` (pages alive iff at least one count > 0) | `burrow_unref` and `burrow_unmap` both check `(handle_count == 0 AND mapping_count == 0)` after their decrement; if true, free pages. The dual check is the runtime enforcement of I-7. |

### P2-Fb landed (this chunk)

- `burrow.tla` (~290 LOC TLA+) + 4 cfg files (1 clean + 3 buggy variants).
- BURROW state: `vmos`, `handle_count[v]`, `mapping_count[v]`, `pages_alive`.
- Per-BURROW refcounts bounded by `MaxRefs` (CONSTANT) for TLC tractability.
- Five spec actions: `VmoCreate`, `HandleOpen`, `HandleClose`, `MapVmo`, `UnmapVmo`.
- Three buggy actions: premature-on-close, premature-on-unmap, never-free (with two delayed-free variants — close + unmap).
- Two state invariants: `RefcountConsistent` + `NoUseAfterFree` (iff form catches both premature AND delayed free).

### P2-Fd impl targets (next chunk)

- `kernel/include/thylacine/burrow.h` — struct Burrow (size + type + handle_count + mapping_count + pages); inline `burrow_ref` / `burrow_unref` (atomic ops).
- `kernel/burrow.c` — `burrow_init`, `burrow_create_anon(size)`, `burrow_map`, `burrow_unmap`, `burrow_free_pages`. SLUB cache. Anonymous VMOs only at v1.0 (VMO_PHYS deferred to Phase 3 with the device tree's reserved-memory handling; VMO_FILE deferred post-v1.0).
- New tests: `burrow.create_close_round_trip`, `burrow.refcount_lifecycle`, `burrow.map_unmap_lifecycle`, `burrow.handles_x_mappings_matrix` (combinations of close-before-unmap, unmap-before-close, etc., verifying pages free at the correct boundary).

### Deferred

- **Physical VMOs (VMO_PHYS) at Phase 3**: when the device tree's reserved-memory regions are exposed, drivers will need physical VMOs over fixed PA ranges (DMA buffers, framebuffer). Spec is agnostic to the backing type — refcount mechanics are identical.
- **File-backed VMOs (VMO_FILE) post-v1.0**: integration with Stratum's page cache. Spec extension: VmoCreate variants per backing type; refcount mechanics unchanged.
- **Concurrent map/unmap and ref/unref (Phase 5+)**: at v1.0 P2-F, the impl runs single-CPU; Phase 5+ adds a per-Burrow lock OR atomic-only operations. The spec is single-stepping (atomic transitions); Phase 5+ refinement may need finer-grained atomicity modeling.



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
