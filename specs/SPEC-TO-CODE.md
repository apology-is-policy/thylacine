# Spec-to-code mapping

For each TLA+ spec, this file maps each action / invariant to a source location. CI will eventually verify the mapping is current — file must exist, function must exist, line range must match. Stale mapping = failing CI (Phase 2 close adds the CI gate).

Per `CLAUDE.md` Spec-first policy: if the four (spec, technical reference, code, user reference) disagree, **the spec wins**.

---

## scheduler.tla — P2-Bb impl mapping (wait/wake atomicity proven)

Status: **wait/wake proven; EEVDF math + IPI ordering deferred** (P2-Bb). Models thread state machine + per-CPU dispatch with the four state-consistency invariants + the wait/wake protocol. Proves `NoMissedWakeup` (ARCH §28 I-9) under the atomic wait/wake protocol; produces a missed-wakeup counterexample under the buggy variant. Does NOT yet model EEVDF deadline math (P2-Bc), IPI ordering (P2-C), or work-stealing (P2-C).

TLC-clean at `Threads = {t1, t2, t3}, CPUs = {c1, c2}` — 283 distinct states explored; depth 10. `scheduler_buggy.cfg` (BUGGY=TRUE) produces the missed-wakeup counterexample at depth 4.

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/proc.c::proc_init` + `kernel/thread.c::thread_init` | Bootstrap: kproc PID 0 + kthread TID 0 RUNNING on CPU0. cond=FALSE, waiters={} initial. |
| `Yield(cpu)` | `kernel/sched.c::sched()` (RUNNING-state branch) | Cooperative yield: prev → RUNNABLE, advance vd_t, insert into run tree, switch. |
| `Block(cpu)` | `kernel/sched.c::sched()` (SLEEPING-state branch) | Caller-set state SLEEPING; sched() leaves prev out of run tree. P2-Bb path is via sleep(); generic Block usable for postnote stop / etc. at Phase 2 close. |
| `Wake(t)` | (deferred — generic non-cond wake) | At P2-Bb, all wakes go through wakeup(r) → ready(t). Generic Wake(t) lands when notes / signals at Phase 5. |
| `Resume(cpu)` | implicit in `kernel/sched.c::pick_next` | Idle CPU pick-up; v1.0 P2-Bb has no idle CPU (UP). P2-C makes this explicit (idle WFI → IPI/IRQ wakeup → resume). |
| `WaitOnCond(cpu)` | `kernel/sched.c::sleep(r, cond, arg)` | **The atomic protocol.** Under `r->lock`: `cond(arg)` check; if FALSE, `r->waiter = current` + `current->state = THREAD_SLEEPING` + `current->rendez_blocked_on = r`; drop spinlock; `sched()` (which sees state SLEEPING, leaves prev out of run tree). Resume reacquires `r->lock` and re-checks. |
| `WakeAll` | `kernel/sched.c::wakeup(r)` | **The atomic publish.** Under `r->lock`: clear `r->waiter`, `current->rendez_blocked_on = NULL`, transition `THREAD_SLEEPING → THREAD_RUNNABLE`, `ready(t)`. Producer must have set cond TRUE before calling (or rely on wakeup's lock acquisition for the happens-before edge). Single-waiter version of multi-waiter spec. |
| `BuggyCheck(cpu)` | (none — bug class statically prevented) | The bug requires splitting cond check from sleep transition; in `sleep()` both happen inside one `while (!cond)` body under `r->lock`. |
| `BuggySleep(cpu)` | (none) | Same as above. |

| Spec invariant | Source enforcement |
|---|---|
| `StateConsistency` (RUNNING ⇔ some CPU's current) | `sched()` + `sleep()` + `wakeup()` update state under their respective locks; only one CPU at v1.0 UP so the implication is one-CPU-trivial. SMP at P2-C needs the per-CPU run-tree synchronization. |
| `NoSimultaneousRun` (RUNNING on ≤1 CPU) | Single-CPU at v1.0 UP; trivially holds. P2-C SMP adds atomic sequence. |
| `RunnableInQueue` (RUNNABLE ⇔ in some runq) | `ready()` is the only entry to RUNNABLE; sets state + inserts. `sched()` pick_next removes prev RUNNING-→-RUNNABLE-→-tree (via insert_sorted) and pulls next out (RUNNABLE-→-RUNNING). |
| `SleepingNotInQueue` (SLEEPING ⇒ no runq + no current) | `sleep()` sets state SLEEPING ONLY after `sched()` has switched the thread off-CPU and pick-next has not re-inserted (the SLEEPING-state branch of sched() is the no-op insert). |
| **`NoMissedWakeup`** (cond=TRUE ⇒ waiters={}) | `sleep()`'s cond check + waiter-add + sleep transition all happen inside one `while (!cond)` body under `r->lock`. `wakeup()`'s waiter-clear + cond is set by caller, both observable to sleep's resume re-check. The atomicity defeats the missed-wakeup race — proven in TLC, enforced at compile time by the lexical structure of `sleep()`. |

### P2-Bc refinement targets

- Model EEVDF: virtual time advancement, vd_t / ve_t per thread, pick-earliest-deadline. Prove latency bound `slice_size × N` (ARCH §28 I-17). Add `PickIsMinDeadline` invariant.
- Model scheduler-tick preemption (timer IRQ → sched()). Add an `IRQ` action firing periodically; prove that under preemption + EEVDF the latency bound holds.
- Add IRQ-mask discipline to the model (currently atomicity is implicit; preemption requires it explicit).

### P2-C refinement targets

- Add cross-CPU `Steal` action with locks; prove fairness (no preferential band treatment).
- Model IPI ordering: per-CPU send queues; prove send-order delivery (ARCH §28 I-18).
- Model the per-CPU "current running thread" set with the work-stealing transfer protocol.
- Generalize wait/wake to multi-CPU with `finish_task_switch` ordering.

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
