# 16 — Rendez (wait/wake) (as-built reference)

P2-Bb's deliverable: the kernel can now block a thread on a caller-supplied condition + wake it from a producer site, with formal proof (`scheduler.tla` `NoMissedWakeup`) that no wakeup is lost across the cond-check ↔ sleep-transition race. The Plan 9 idiom `sleep(r, cond, arg)` / `wakeup(r)` lands on top of P2-Ba's `sched()` dispatch — the scheduler's `pick-next` machinery is what runs while a thread is sleeping; the Rendez plus the spinlock are what make the wait/wake protocol atomic.

Scope: `kernel/include/thylacine/rendez.h` (new), `kernel/sched.c` (`sleep` + `wakeup` impl + `sched()` refactor to respect `prev->state`), `kernel/include/thylacine/thread.h` (added `rendez_blocked_on` field), `kernel/test/test_rendez.c` (new).

Reference: `ARCHITECTURE.md §8.5` (wakeup atomicity); `§8.8` (Plan 9 idiom layer); `§28` invariant **I-9** (no wakeup is lost between wait-condition check and sleep). Spec: `specs/scheduler.tla` actions `WaitOnCond` (sleep) and `WakeAll` (wakeup); invariant `NoMissedWakeup` (cond=TRUE ⇒ waiters={}).

---

## Purpose

The classic OS bug: thread A is about to sleep on condition X; thread B sets X just before A actually sleeps; A sleeps with X already set; A misses the wakeup and never runs. The fix is the **wait/wake protocol** — both sides take a shared lock, A checks the cond AND transitions to SLEEPING under the lock, B mutates the cond AND wakes the waiter under the lock. The atomicity defeats the race: any wake fired before A's enqueue has already set X (so A's cond check observes TRUE and skips the sleep); any wake fired after A's enqueue sees the waiter and wakes it.

`Rendez` is the synchronization object that owns the lock. `sleep(r, cond, arg)` is the consumer-side primitive: it checks `cond(arg)` under `r->lock`; if FALSE, it transitions the calling thread to `THREAD_SLEEPING`, drops the lock, and yields to the scheduler. `wakeup(r)` is the producer-side primitive: under `r->lock`, it transitions the (single) waiter back to `THREAD_RUNNABLE` and `ready()`s it.

Single-waiter discipline at v1.0 P2-Bb (Plan 9 convention): at most one thread per Rendez. Multi-waiter wait queues are deferred to Phase 5 (poll, futex). The single-waiter case is structurally a special case of the multi-waiter spec — invariants carry over (a singleton-or-empty waiters set still satisfies `cond=TRUE ⇒ waiters={}` under the same protocol).

---

## Public API

### `<thylacine/rendez.h>`

```c
struct Rendez {
    spin_lock_t    lock;
    struct Thread *waiter;     // NULL or the single sleeper
};

#define RENDEZ_INIT  ((struct Rendez){ SPIN_LOCK_INIT, NULL })

static inline void rendez_init(struct Rendez *r);

void sleep(struct Rendez *r, int (*cond)(void *arg), void *arg);
int  wakeup(struct Rendez *r);
```

### `sleep(r, cond, arg)` semantics

**Preconditions:**
- `r` initialized (via `rendez_init` or `RENDEZ_INIT`).
- `cond` is a side-effect-free predicate that may be evaluated multiple times. `cond` is invoked under `r->lock`; the producer's writes to whatever state `cond` reads must therefore be protected by `r->lock` (or be followed by `wakeup(r)` which re-takes `r->lock`, providing a happens-before edge).
- Caller is NOT in IRQ context — `sleep` may yield indefinitely.
- At most one thread sleeps on `r` at a time. Second sleeper extincts.

**Semantics:**
- Acquires `r->lock` with `spin_lock_irqsave` (IRQ mask on; saved state restored at exit).
- Loop: evaluate `cond(arg)`. If TRUE → drop lock, return. If FALSE → enqueue self + transition state, drop spinlock (keep IRQ mask), call `sched()`, on resume reacquire spinlock and re-evaluate cond.
- Slow-path bookkeeping (under lock): `r->waiter = current`; `current->rendez_blocked_on = r`; `current->state = THREAD_SLEEPING`.

**Resumption:** `sched()` returns when some peer has called `wakeup(r)`, which transitioned `current` back to `THREAD_RUNNABLE` and inserted it into the scheduler's run tree. After `sched()` returns, the loop reacquires `r->lock` and re-checks `cond` (robustness for multi-waker scenarios; under v1.0 single-waker, cond should be TRUE post-wakeup).

### `wakeup(r)` semantics

**Preconditions:**
- `r` initialized.
- The cond's underlying state has been set TRUE by the caller BEFORE `wakeup` (either under `r->lock` taken separately, or trivially because `wakeup` itself takes `r->lock` after the caller's write — providing the publish-after-mutate ordering required by sleep's resume re-check).

**Semantics:**
- Acquires `r->lock` with `spin_lock_irqsave`.
- If `r->waiter == NULL`: drop lock, return 0. (Idempotent no-op — this is intended; producers can call `wakeup` unconditionally without coordinating with consumers.)
- Else: `r->waiter = NULL`; waiter's `rendez_blocked_on = NULL`; waiter's `state = THREAD_RUNNABLE`; `ready(waiter)`. Drop lock, return 1.

**Postcondition:** the woken thread is in the scheduler's run tree; some subsequent `sched()` call (on this CPU, or another at SMP) will pick it up and resume it inside its `sleep` loop.

**IRQ-context safe:** the lock is irqsave; `wakeup` may be called from an IRQ handler (P2-Bc lands the timer-IRQ scheduler-tick path that exercises this).

---

## Implementation

### Lock + IRQ discipline

`r->lock` is a `spin_lock_t`. At v1.0 UP the spin part is a no-op (no contention possible); at SMP (P2-C) it becomes the real cross-CPU contention point. The IRQ mask (saved/restored via PSTATE.DAIF.I) is real even on UP — it prevents an IRQ handler from re-entering `sleep` / `wakeup` (e.g., timer IRQ wakes a thread that's mid-sleep, before the sleep call has actually transitioned to SLEEPING).

The Rendez lock spans the cond-check + state-transition (consumer) and the cond-publish + waiter-clear (producer) — exactly mapping the spec's WaitOnCond / WakeAll atomic blocks. `sched()` runs *without* `r->lock` held (the `spin_unlock(&r->lock)` happens in `sleep()` before `sched()` is called); the IRQ mask remains held across the `sched()` call.

### `sched()` refactor

P2-Ba's `sched()` unconditionally treated `prev` as yielding — set `prev->state = RUNNABLE`, advanced `vd_t`, inserted into run tree. P2-Bb refactors to respect the caller's pre-set `prev->state`:

| `prev->state` set by caller | sched() behavior |
|---|---|
| `THREAD_RUNNING`  | Yield: → RUNNABLE + advance vd_t + insert into run tree. |
| `THREAD_SLEEPING` | Block: leave alone — wakeup() will re-insert later. |
| `THREAD_EXITING`  | Exit: leave alone — Phase 2 close reaps the thread. |
| anything else     | Logic error → extinction. |

This unifies yield + block + exit through one `sched()` entry point, matching Plan 9 / Linux. The `THREAD_RUNNING` semantics preserve P2-Ba's existing tests.

A new defensive `prev == next` check guards against the SMP race where a cross-CPU wakeup re-inserts `prev` into a runqueue between the spinlock drop and the sched entry — see P2-C trip-hazards.

### Deadlock detection

`sched()` with `prev->state == SLEEPING` AND no other runnable thread is a deadlock under UP (the only event source that could wake `prev` is an IRQ — and the IRQ mask is held throughout). The path extincts loudly:

```
EXTINCTION: sched: deadlock — current is blocking, no runnable peer
```

P2-C lands SMP idle-WFI: an idle CPU with no runnable thread parks at WFI and wakes via IPI / IRQ — at which point this extinction becomes valid only on UP-no-other-CPU, which is itself a deprecated config.

### `sleep(r, cond, arg)` impl

```
spin_lock_irqsave(&r->lock)
while (!cond(arg)) {
    extinction-check: r->waiter == NULL  (single-waiter discipline)
    extinction-check: current->state == THREAD_RUNNING (no nested sleep)
    extinction-check: current->rendez_blocked_on == NULL

    r->waiter = current
    current->rendez_blocked_on = r
    current->state = THREAD_SLEEPING

    spin_unlock(&r->lock)         /* drop spin-lock; IRQ mask remains */
    sched()                       /* prev is SLEEPING → no re-insert */
    spin_lock(&r->lock)           /* resume: reacquire to re-check cond */
}
spin_unlock_irqrestore(&r->lock, s)
```

The `while` loop is robustness against future multi-waker / spurious-wake scenarios. At v1.0 P2-Bb single-waker UP, the loop runs at most one iteration past the initial check (cond is true after wakeup).

### `wakeup(r)` impl

```
spin_lock_irqsave(&r->lock)
t = r->waiter
if (!t) { unlock; return 0 }

extinction-check: t->magic == THREAD_MAGIC
extinction-check: t->state == THREAD_SLEEPING
extinction-check: t->rendez_blocked_on == r

r->waiter = NULL
t->rendez_blocked_on = NULL
t->state = THREAD_RUNNABLE
ready(t)

spin_unlock_irqrestore(&r->lock, s)
return 1
```

The three extinction checks defend against corruption (magic mismatch — likely a use-after-free in the scheduler), state inconsistency (SLEEPING is a precondition for being on a Rendez waiter slot), and backref disagreement (a thread sleeping on a *different* Rendez somehow ending up here, indicating logic error elsewhere).

---

## Data structures

### `struct Rendez` (16 bytes)

```c
struct Rendez {
    spin_lock_t    lock;       /* 4 bytes — _stub at v1.0 UP */
    struct Thread *waiter;     /* 8 bytes — single sleeper or NULL */
    /* implicit padding to align: 16 bytes total on aarch64 */
};
```

The `lock` field is a `spin_lock_t` — at v1.0 a 4-byte stub; the IRQ-mask discipline is what it provides via `spin_lock_irqsave`. SMP P2-C grows the field to a real LL/SC lock; the size may grow but is contained to this struct.

### `struct Thread` extension (`rendez_blocked_on`, 8 bytes)

A pointer field added at end of `struct Thread`:

```c
struct Rendez *rendez_blocked_on;     /* NULL when not sleeping; r when sleeping */
```

Set by `sleep` under `r->lock` atomically with `state = THREAD_SLEEPING`; cleared by `wakeup` under `r->lock` atomically with `state = THREAD_RUNNABLE`. Diagnostic + invariant aid: a SLEEPING thread with `rendez_blocked_on != NULL` is sleeping on that specific Rendez; `wakeup` cross-checks the backref.

`struct Thread` size grew 200 → 208 bytes. `_Static_assert` in `thread.h` is bumped accordingly.

---

## State machine

```
                  ┌────────── ready(t) ─────────┐
                  ▼                             │
                ┌────────────┐                  │
              ┌─►│ RUNNABLE   │◄──── wakeup(r)  │
              │ └────┬───────┘                  │
   sched()    │      │ pick_next()              │
   (yield)    │      ▼                          │
              │ ┌────────────┐                  │
              └─┤  RUNNING   │                  │
                └─────┬──────┘                  │
                      │                         │
              sleep(r, ...)  cond=FALSE         │
                      │                         │
                      ▼                         │
                ┌────────────┐                  │
                │  SLEEPING  │──────────────────┘
                └────────────┘
```

`THREAD_EXITING` is a fourth state landed at Phase 2 close (exit/reap path) — sched()'s refactor accommodates it but no transition lands at P2-Bb.

---

## Spec cross-reference

`specs/scheduler.tla` actions and the impl call sites:

| Spec action       | Impl site                           | What it models |
|---|---|---|
| `WaitOnCond(cpu)` | `kernel/sched.c:sleep()`            | Atomic cond-check + sleep transition + enqueue under lock. |
| `WakeAll`         | `kernel/sched.c:wakeup()`           | Atomic clear waiter + state transition + run-tree insert under lock. |
| `Yield(cpu)`      | `kernel/sched.c:sched()` RUNNING-branch | Cooperative yield; prev → RUNNABLE + insert. |
| `Block(cpu)`      | `kernel/sched.c:sched()` SLEEPING-branch (caller pre-set state) | Block on a non-cond reason — same dispatch path; caller's responsibility to set state. |
| `Resume(cpu)`     | implicit in `sched()` pick-next | Idle CPU picks up runnable thread. P2-C makes idle-CPU explicit; v1.0 UP has no idle. |

Spec invariants and the impl's defenses against violation:

- **`NoMissedWakeup`** (cond=TRUE ⇒ waiters={}). The spec proof: `WaitOnCond` is one atomic step; cond check + waiter-add happen together. Any `WakeAll` that fires after sees the waiter; any that fires before has already set cond TRUE so `WaitOnCond` takes the fast path. Impl: `sleep`'s entire slow-path enqueue runs under `r->lock`; `wakeup`'s clear runs under `r->lock`; same atomicity.

- **`StateConsistency`** (RUNNING iff some CPU's current). Impl: `sleep` only transitions `current` (RUNNING → SLEEPING under lock); no other thread transitions out of RUNNING in `sleep`. `wakeup` transitions a SLEEPING thread to RUNNABLE; no other state appears.

- **`SleepingNotInQueue`** (SLEEPING ⇒ in no runqueue ∧ no CPU's current). Impl: `sleep`'s state transition happens under `r->lock` only after `sched()` has run (which removed `current` from the runqueue at pick-next); the sleeping thread is also no longer "current" because `sched()` set `current = next`. `wakeup` puts the thread back in the runqueue ONLY after setting `state = RUNNABLE`.

`scheduler_buggy.cfg` (`BUGGY=TRUE`) demonstrates the canonical missed-wakeup violation: if `BuggyCheck` (cond observe) and `BuggySleep` (state transition) are SEPARATE actions, a `WakeAll` between them produces `cond=TRUE ∧ waiters={t}` — `NoMissedWakeup` violated. Impl: this split CANNOT occur because both are within the single `while (!cond)` body under `r->lock`.

---

## Tests

| Test | What it verifies |
|---|---|
| `rendez.sleep_immediate_cond_true` | Fast path: cond-true at sleep entry → no state transition, no waiter-enqueue. Mirrors WaitOnCond's cond-true branch. |
| `rendez.basic_handoff` | Two-thread producer/consumer over a Rendez. Consumer calls sleep with cond-FALSE → SLEEPING; boot sets cond TRUE + wakeup → consumer RUNNABLE; consumer resumes inside sleep loop → returns. Validates the entire sleep + wakeup + state-transition + scheduler interaction. |
| `rendez.wakeup_no_waiter` | Idempotent no-op: wakeup on empty Rendez returns 0, no side effects. Allows producers to call wakeup unconditionally. |

Tests exercise cooperative dispatch — timer-IRQ-driven preemption is not yet live (P2-Bc).

---

## Error paths

| Extinction message | Trigger | What it catches |
|---|---|---|
| `sleep(NULL rendez)` | `sleep(NULL, ...)` | Caller bug. |
| `sleep with NULL cond` | `sleep(r, NULL, ...)` | Caller bug. |
| `sleep: rendez already has a waiter (single-waiter discipline)` | Second sleeper on the same Rendez. | Misuse — multi-waiter wait queues are Phase 5. |
| `sleep: no current thread` | `sleep` called before `thread_init`. | Boot ordering bug. |
| `sleep: corrupted current` | `current_thread()->magic != THREAD_MAGIC`. | UAF / corruption — unlikely under UP-no-preempt. |
| `sleep: current is not RUNNING` | `current->state != THREAD_RUNNING`. | State machine logic error — caller called sleep from inside another scheduler primitive. |
| `sleep: current already blocked on a rendez` | `current->rendez_blocked_on != NULL`. | Nested sleep — protocol violation (Plan 9 forbids; Phase 5 multi-waiter generalizes via wait queues). |
| `sched: deadlock — current is blocking, no runnable peer` | `sched()` block-path with empty run tree. | UP deadlock — only an IRQ could wake us and IRQs are masked. |
| `wakeup(NULL rendez)` | `wakeup(NULL)`. | Caller bug. |
| `wakeup: corrupted waiter` | Waiter's magic mismatch. | UAF / corruption. |
| `wakeup: waiter is not SLEEPING` | Waiter's `state != THREAD_SLEEPING`. | State machine logic error. |
| `wakeup: waiter rendez backref mismatch` | `t->rendez_blocked_on != r`. | Cross-Rendez confusion — logic error. |

---

## Performance characteristics

`sleep` and `wakeup` each: one atomic lock acquire/release pair, O(1) state transitions, one O(1) `ready` call (insert into run tree, which is O(N) walk through band — typically tens of nodes at v1.0). At v1.0 single-CPU UP-no-preempt the spinlock contention is zero; the IRQ mask save/restore is two `mrs`/`msr` instructions each. The `sched()` call inside `sleep` is the dominant cost (whole context save/load); orders of magnitude beyond the Rendez bookkeeping.

Phase 5 measured numbers go here when fuzz/stress harness lands.

---

## Status

Implemented: P2-Bb at `<commit-pending>`. Stubbed: nothing. Deferred:
- **Multi-waiter wait queues** — Phase 5 (poll + futex). Single-waiter Rendez sufficient for v1.0 internal kernel uses (timer expiry, simple producer/consumer).
- **Timeout (`sleep_timeout`)** — when the timer wheel lands at P2-C / Phase 5; currently `sleep` is unbounded.
- **Interruptible sleep** — when notes (Plan 9 signals) land at Phase 5.

---

## Known caveats / footguns

1. **The producer must mutate cond's underlying state under `r->lock`** OR rely on `wakeup`'s subsequent re-acquire of `r->lock` to provide the happens-before edge. The "rely on wakeup's re-acquire" pattern works under sequentially-consistent memory models + `wakeup` always being called after the mutate; under SMP weakly-ordered memory the safer pattern is "mutate under r->lock, then wakeup."

2. **`cond` must be side-effect-free.** It may be called multiple times — once during the initial fast-path check, again on each wake-up cycle. Side effects in `cond` will fire repeatedly.

3. **No nested sleep on the same Rendez.** Single-waiter discipline. A thread that's already sleeping on `r1` cannot enter `sleep(r1, ...)` again — extincts. For "wait on multiple conditions" use poll (Phase 5).

4. **IRQ mask is held across `sched()`.** This is intentional (the IRQ mask is what makes the sleep transition + sched call atomic against IRQ-context wakeups). But it means the next thread (the one `sched()` switches to) inherits the IRQ mask state as the saved DAIF in the eventual `spin_unlock_irqrestore` on its own sleep call — which is fine because every kernel critical section that uses irqsave/irqrestore is balanced within itself. P2-Bc's scheduler-tick preemption tests this discipline at scale.

5. **SMP race not yet closed (P2-Bb is UP-only).** Between `spin_unlock(&r->lock)` and entering `sched()`, on SMP a peer wakeup can fire on another CPU, transition `current` to RUNNABLE, and insert it into a runqueue. When `current` then enters `sched()`, `pick_next` may pull `current` (now RUNNABLE) out of its own runqueue. The sched() refactor's `prev == next` check (re-insert + return-without-switch) defends against this; the proper fix is the `finish_task_switch` pattern at P2-C where the runqueue lock is dropped only after the context switch completes.

6. **`sched()` block-path with no runnable peer extincts.** The deadlock detection assumes some other thread will become runnable while `current` is sleeping. At v1.0 P2-Bb single-CPU UP this is enforced — every kernel thread eventually yields. P2-C makes idle-WFI the legitimate "no runnable" path; the extinction path then narrows to genuine deadlocks.

---

## Naming rationale

`sleep` / `wakeup` / `Rendez` are the Plan 9 idiomatic names. They're loadbearing across Plan 9 documentation + comments + community vocabulary; renaming them would create a translation barrier without adding clarity. Held under the "don't force it" discipline in CLAUDE.md — the standard names communicate intent best for anyone reading the kernel.

`rendez_blocked_on` (the Thread field) is descriptive: "what Rendez is this Thread blocked on?" Plan 9's source uses `up->r` (single character); the verbose name reduces the likelihood of confusion when `r` could mean "register" or "result" in surrounding context.
