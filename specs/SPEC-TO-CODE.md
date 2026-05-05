# Spec-to-code mapping

For each TLA+ spec, this file maps each action / invariant to a source location. CI will eventually verify the mapping is current — file must exist, function must exist, line range must match. Stale mapping = failing CI (Phase 2 close adds the CI gate).

Per `CLAUDE.md` Spec-first policy: if the four (spec, technical reference, code, user reference) disagree, **the spec wins**.

---

## scheduler.tla — P2-A sketch

Status: **sketch** (P2-A). Models thread state machine + per-CPU dispatch with the four state-consistency invariants. Does NOT yet model EEVDF deadline math, wait/wake atomicity, IPI ordering, or work-stealing — those refinements land at P2-B / P2-C / Phase 2 close.

TLC-clean at `Threads = {t1, t2, t3}, CPUs = {c1, c2}` — 99 distinct states explored; depth 9.

| Spec action | Source location | Notes |
|---|---|---|
| `Init` | `kernel/proc.c::proc_init` + `kernel/thread.c::thread_init` | Bootstrap: kproc PID 0 + kthread TID 0 RUNNING on CPU0; other threads RUNNABLE in CPU0's runqueue (modeled, no equivalent at P2-A — there's only kthread). |
| `Yield(cpu)` | `kernel/thread.c::thread_switch` | P2-A direct switch; P2-B refines with EEVDF pick-earliest-deadline. |
| `Block(cpu)` | (P2-B) | `thread_block` not implemented yet. |
| `Wake(t)` | (P2-B) | `thread_wake` not implemented yet. |
| `Resume(cpu)` | (P2-B scheduler dispatch from idle) | No idle-dispatch path at P2-A. |

| Spec invariant | Source enforcement |
|---|---|
| `StateConsistency` | `thread_switch` updates state symmetrically with `set_current_thread`. |
| `NoSimultaneousRun` | Single-CPU at v1.0; trivially holds. SMP at Phase 2 close needs the IRQ-mask + atomic sequence. |
| `RunnableInQueue` | (P2-B — when runqueues exist) |
| `SleepingNotInQueue` | (P2-B — when SLEEPING transitions exist) |

### P2-B refinement targets

- Split `Block` into separate `CheckCond` + `Sleep` actions; prove `NoMissedWakeup`.
- Land `scheduler_buggy.cfg` showing the missed-wakeup race when atomicity is violated (executable-documentation pattern per CLAUDE.md).
- Model EEVDF: virtual time advancement, vd_t / ve_t per thread, pick-earliest-deadline. Prove latency bound `slice_size × N` (ARCH §28 I-17).
- Model IPI ordering: per-CPU send queues; prove send-order delivery.

### P2-C refinement targets

- Add cross-CPU `Steal` action with locks; prove fairness (no preferential band treatment).
- Model the per-CPU "current running thread" set with the work-stealing transfer protocol.

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
