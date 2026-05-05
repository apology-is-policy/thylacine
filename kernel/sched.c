// Scheduler dispatch + Plan 9 idiom layer (P2-Ba sched/ready; P2-Bb
// adds sleep/wakeup over Rendez). Per ARCH §8.
//
// State model:
//   - One run tree per priority band (INTERACTIVE / NORMAL / IDLE).
//     Implemented as a doubly-linked list sorted ascending by `vd_t`.
//     The head holds the minimum vd_t — the next-to-run for the band.
//   - Highest-priority band with runnable threads wins; within a band,
//     pick-min-vd_t.
//   - On yield (sched() with prev->state == RUNNING): prev's vd_t is
//     advanced past every other runnable thread's vd_t (g_vd_counter++)
//     so prev lands at the back of the rotation; prev → RUNNABLE +
//     inserted into the band's tree; pick-next switches in.
//   - On block (sched() with prev->state set to SLEEPING by caller):
//     prev is NOT inserted into the run tree — it remains SLEEPING
//     until some peer transitions it back via wakeup() / ready(). The
//     caller's responsibility to set state BEFORE calling sched().
//
// At P2-Ba+P2-Bb this is FIFO-like dispatch keyed on monotonic vd_t.
// Full EEVDF math (vd_t = ve_t + slice × W_total / w_self with weighted
// virtual time advance + bounded latency proof I-17) lands at P2-Bc.
//
// UP + no preemption: no contention on the run tree. sleep/wakeup use
// per-Rendez spin_lock_irqsave; the IRQ mask is real (PSTATE.DAIF.I)
// even on UP and is what makes the wait/wake protocol robust against
// IRQ-context wakers (P2-Bc lands the timer-IRQ-driven scheduler tick
// that uses this discipline). P2-C adds the SMP CPU-cross IPI / per-
// CPU run-tree refinements; the per-Rendez spinlock then becomes a
// real contention point and sched() needs the finish_task_switch
// pattern — see trip-hazards in docs/phase2-status.md.

#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// Per-band sorted-by-vd_t doubly-linked list head pointer. NULL if the
// band has no runnable threads. The head's vd_t is the minimum.
static struct Thread *g_run_tree[SCHED_BAND_COUNT];

// Monotonic vd_t counter. Starts at 1 (kthread reserves vd_t=0); each
// `sched()` advances current's vd_t to g_vd_counter++.
static s64 g_vd_counter = 1;

// One-shot init flag.
static bool g_sched_initialized = false;

// P2-Bc: preemption signal. Set by sched_tick() (timer IRQ handler)
// when the current thread's slice expires. Cleared by
// preempt_check_irq() right before the actual sched() call.
//
// `volatile` because it's read in C without an explicit barrier
// across the preempt-check / sched call site; the IRQ handler is
// the writer; the reader is the IRQ-return path. UP at v1.0 P2-Bc:
// no SMP race; volatile suffices. P2-C SMP needs an atomic_bool with
// release/acquire.
static volatile bool g_need_resched = false;

void sched_init(void) {
    if (g_sched_initialized) extinction("sched_init called twice");
    if (!current_thread()) extinction("sched_init before thread_init");

    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        g_run_tree[b] = NULL;
    }
    g_vd_counter = 1;
    g_sched_initialized = true;
}

// True iff `t` is currently in some run tree (linked or as head).
static bool in_run_tree(struct Thread *t) {
    return t->runnable_next != NULL || t->runnable_prev != NULL ||
           g_run_tree[t->band] == t;
}

// Insert `t` into its band's tree, sorted ascending by vd_t. Ties (equal
// vd_t) place `t` AFTER existing equal-keyed nodes — FIFO within ties.
static void insert_sorted(struct Thread *t) {
    struct Thread **head = &g_run_tree[t->band];

    // Empty band or t < head: prepend.
    if (!*head || (*head)->vd_t > t->vd_t) {
        t->runnable_prev = NULL;
        t->runnable_next = *head;
        if (*head) (*head)->runnable_prev = t;
        *head = t;
        return;
    }

    // Walk to find first node with vd_t > t->vd_t (or end of list).
    // FIFO tie-breaking: walk past nodes with vd_t == t->vd_t.
    struct Thread *cur = *head;
    while (cur->runnable_next && cur->runnable_next->vd_t <= t->vd_t) {
        cur = cur->runnable_next;
    }

    // Insert after cur.
    t->runnable_prev = cur;
    t->runnable_next = cur->runnable_next;
    if (cur->runnable_next) cur->runnable_next->runnable_prev = t;
    cur->runnable_next = t;
}

// Remove `t` from its band's tree. Caller has confirmed t is in the tree.
static void unlink(struct Thread *t) {
    if (t->runnable_prev) {
        t->runnable_prev->runnable_next = t->runnable_next;
    } else {
        // t was the head.
        g_run_tree[t->band] = t->runnable_next;
    }
    if (t->runnable_next) {
        t->runnable_next->runnable_prev = t->runnable_prev;
    }
    t->runnable_next = NULL;
    t->runnable_prev = NULL;
}

// Pick the highest-priority runnable thread; remove from its tree.
// Returns NULL if no thread is runnable across any band.
static struct Thread *pick_next(void) {
    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        struct Thread *t = g_run_tree[b];
        if (t) {
            unlink(t);
            return t;
        }
    }
    return NULL;
}

void ready(struct Thread *t) {
    if (!t)                       extinction("ready(NULL)");
    if (t->magic != THREAD_MAGIC) extinction("ready of corrupted Thread");
    if (t->state != THREAD_RUNNABLE)
                                  extinction("ready of non-RUNNABLE Thread");
    if (t->band >= SCHED_BAND_COUNT)
                                  extinction("ready: invalid band");

    // P2-Bc: IRQ-mask discipline. The run tree is mutated below;
    // a timer IRQ firing here would re-enter preempt_check_irq →
    // sched() and observe the half-mutated tree. Mask IRQs for the
    // insert. (Same rationale as sched().)
    irq_state_t s = spin_lock_irqsave(NULL);

    if (in_run_tree(t))           extinction("ready of already-runnable Thread");

    insert_sorted(t);

    spin_unlock_irqrestore(NULL, s);
}

void sched(void) {
    if (!g_sched_initialized) extinction("sched() before sched_init");

    // P2-Bc: IRQ-mask discipline. sched() mutates shared state
    // (run tree, current_thread via TPIDR_EL1, vd_t counter) — a
    // timer IRQ firing inside this critical section would re-enter
    // preempt_check_irq → sched(), corrupting the run tree. Mask
    // IRQs locally for the duration; the saved DAIF is restored on
    // resumption (after the eventual switch-back propagates the
    // saved state through cpu_switch_context).
    //
    // The lock argument is NULL — we're not contending on a shared
    // lock (UP single-CPU at v1.0); the IRQ mask is what we need.
    // P2-C SMP turns the spin part real with a per-CPU run-tree lock.
    irq_state_t s = spin_lock_irqsave(NULL);

    struct Thread *prev = current_thread();
    if (!prev) extinction("sched() with no current thread");
    if (prev->magic != THREAD_MAGIC) extinction("sched() with corrupted current");

    // sched() respects prev->state — yield vs block dispatch:
    //   THREAD_RUNNING   → yield: prev → RUNNABLE + inserted into tree.
    //   THREAD_SLEEPING  → block: caller already transitioned. Don't
    //                       re-insert into run tree; some peer will
    //                       wake via wakeup()/ready() later.
    //   THREAD_EXITING   → exit: caller is winding down. Same as
    //                       SLEEPING — don't re-insert. (Phase 2 close
    //                       lands the actual reap.)
    //   THREAD_RUNNABLE  → INVALID — prev is current; it cannot be
    //                       both current and already runnable.
    //   _INVALID         → INVALID — magic check above caught corrupt
    //                       memory; any other state is a logic error.
    if (prev->state != THREAD_RUNNING &&
        prev->state != THREAD_SLEEPING &&
        prev->state != THREAD_EXITING) {
        extinction("sched: invalid prev state");
    }

    struct Thread *next = pick_next();
    if (!next) {
        if (prev->state != THREAD_RUNNING) {
            // Block path with no runnable peer. UP-no-preempt: deadlock
            // (the only event source that could wake prev is an IRQ,
            // which by the wait/wake discipline must release prev's
            // wait condition before this; if we got here, the protocol
            // is broken). P2-C adds idle-WFI on SMP — a CPU with no
            // runnable thread parks at WFI, gets woken by IPI / IRQ.
            extinction("sched: deadlock — current is blocking, no runnable peer");
        }
        // Yield path with no runnable peer: keep prev running. Refill
        // slice — the rationale for this sched() call (pre-emption,
        // explicit yield) was that prev's slice expired or yield was
        // requested; either way, give prev a fresh slice now that
        // it's the only runnable thread and we'd otherwise have to
        // immediately preempt-back-into-it.
        prev->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
        spin_unlock_irqrestore(NULL, s);
        return;
    }

    if (prev == next) {
        // pick_next pulled prev out of its own runqueue. Possible
        // under SMP race (cross-CPU wakeup re-inserted prev into a
        // runqueue between drop-Rendez-lock and entering sched).
        // Don't switch to self — re-insert and let the next sched()
        // try again. UP: should not happen at v1.0 (see SMP race
        // trip-hazard).
        if (prev->state == THREAD_RUNNING) {
            // Prev was yielding; it's still RUNNING. pick_next won't
            // return prev unless prev is RUNNABLE (in some runqueue).
            // RUNNING but in a runqueue is the SMP race symptom.
            insert_sorted(prev);
        }
        spin_unlock_irqrestore(NULL, s);
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        // Yield: advance vd_t past all currently-runnable threads;
        // insert prev at the back of its band's rotation.
        prev->vd_t = g_vd_counter++;
        prev->state = THREAD_RUNNABLE;
        insert_sorted(prev);
    }
    // else: block (SLEEPING) or exit (EXITING) — prev stays out of
    // the run tree. wakeup()/ready() will re-insert SLEEPING; EXITING
    // never returns and gets reaped at Phase 2 close.

    // Replenish next's slice on RUNNABLE → RUNNING transition. The
    // slice is consumed during the running quantum; sched_tick()
    // decrements it on each timer IRQ.
    next->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
    next->state = THREAD_RUNNING;
    set_current_thread(next);

    cpu_switch_context(&prev->ctx, &next->ctx);

    // Resumption: prev was switched back to. State and current_thread
    // were set by whichever peer transitioned us out of SLEEPING (via
    // wakeup → ready) or yielded back (via sched picking us). prev is
    // no longer in the run tree (the peer's pick_next removed it
    // before switching). prev's slice was replenished by whichever
    // peer's sched() picked prev (the same code path above, in their
    // frame).
    spin_unlock_irqrestore(NULL, s);
}

void sched_remove_if_runnable(struct Thread *t) {
    if (!t) return;
    if (t->state != THREAD_RUNNABLE) return;
    if (!in_run_tree(t)) return;
    unlink(t);
}

unsigned sched_runnable_count(void) {
    unsigned n = 0;
    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        for (struct Thread *t = g_run_tree[b]; t; t = t->runnable_next) {
            n++;
        }
    }
    return n;
}

unsigned sched_runnable_count_band(unsigned band) {
    if (band >= SCHED_BAND_COUNT) return 0;
    unsigned n = 0;
    for (struct Thread *t = g_run_tree[band]; t; t = t->runnable_next) {
        n++;
    }
    return n;
}

// ============================================================================
// Plan 9 wait/wake protocol — sleep + wakeup over Rendez (P2-Bb).
//
// Maps to scheduler.tla actions:
//   sleep   ↔ WaitOnCond  (atomic cond-check + sleep transition + enqueue
//                          under r->lock).
//   wakeup  ↔ WakeAll     (atomic clear waiter + RUNNABLE transition +
//                          ready under r->lock).
//
// The atomicity that defeats the missed-wakeup race comes from r->lock:
// cond is checked under the lock; if cond is FALSE the thread enqueues
// itself + transitions to SLEEPING BEFORE the lock is released. Any
// wakeup that fires after the lock release sees the waiter and wakes
// it. Any wakeup that fires before the cond check has already set cond
// TRUE; the cond check observes TRUE and takes the fast path.
//
// IRQ discipline: r->lock is held with spin_lock_irqsave so concurrent
// IRQ-context wakeups can't preempt the cond check / sleep transition
// at UP. P2-Bc lands the timer-IRQ scheduler tick that exercises this;
// P2-C lands the SMP cross-CPU race + finish_task_switch pattern.
//
// Single-waiter discipline: at most one Thread per Rendez at a time.
// extincts on second sleeper. Multi-waiter wait queues land at Phase 5
// (poll, futex).
// ============================================================================

void sleep(struct Rendez *r, int (*cond)(void *arg), void *arg) {
    if (!r)    extinction("sleep(NULL rendez)");
    if (!cond) extinction("sleep with NULL cond");

    irq_state_t s = spin_lock_irqsave(&r->lock);

    // Loop: re-check cond on each wakeup. Single-waiter UP: cond should
    // be true after wakeup. The loop is robustness against future multi-
    // waker / spurious-wake scenarios (Phase 5 poll/futex).
    while (!cond(arg)) {
        if (r->waiter) {
            // Single-waiter discipline. A second sleeper indicates
            // either a Rendez being reused incorrectly (caller error)
            // or a multi-waiter use case that needs a wait queue.
            extinction("sleep: rendez already has a waiter (single-waiter discipline)");
        }

        struct Thread *t = current_thread();
        if (!t)                       extinction("sleep: no current thread");
        if (t->magic != THREAD_MAGIC) extinction("sleep: corrupted current");
        if (t->state != THREAD_RUNNING)
            extinction("sleep: current is not RUNNING");
        if (t->rendez_blocked_on)
            extinction("sleep: current already blocked on a rendez");

        // Atomic under r->lock: enqueue + state transition. Any wakeup
        // that fires after the unlock below sees waiter == t and walks
        // the protocol correctly.
        r->waiter = t;
        t->rendez_blocked_on = r;
        t->state = THREAD_SLEEPING;

        // Drop the rendez spinlock (the IRQ mask remains via the
        // outer irqsave — we're about to call sched, which may
        // observe a pending IRQ as a wakeup notification on
        // resumption; the mask spans the entire sleep call). sched()
        // sees state == SLEEPING and refrains from re-inserting prev
        // into the run tree. On SMP this window (between unlock and
        // sched) is the canonical wait/wake race that P2-C closes
        // with the finish_task_switch pattern.
        spin_unlock(&r->lock);

        sched();

        // Resumption point: a wakeup transitioned us back to RUNNABLE
        // under r->lock; ready() inserted us in the run tree;
        // pick_next pulled us out and set us RUNNING. Reacquire the
        // lock to re-evaluate cond. If cond is still false (multi-
        // waker scenarios — none at v1.0 P2-Bb but plausible later),
        // loop and sleep again.
        spin_lock(&r->lock);
    }

    spin_unlock_irqrestore(&r->lock, s);
}

// ============================================================================
// Scheduler-tick preemption (P2-Bc).
//
// The timer IRQ fires at fixed Hz (1000 at v1.0). Each fire:
//   1. timer_irq_handler increments g_ticks + reloads the timer.
//   2. timer_irq_handler calls sched_tick() (defined here).
//   3. sched_tick() decrements current's slice_remaining; if ≤ 0,
//      sets g_need_resched and replenishes the slice (so the
//      decrement doesn't continually fire need_resched on every
//      subsequent tick before preempt_check_irq runs).
//   4. The vectors.S IRQ slot, after exception_irq_curr_el returns,
//      calls preempt_check_irq() (also defined here).
//   5. preempt_check_irq() sees g_need_resched, clears it, calls
//      sched(). sched() saves current's context (the "C-stack" state
//      of preempt_check_irq's invocation, including the bl-return
//      address into vectors.S), switches to next.
//   6. When the formerly-current thread is eventually resumed, its
//      cpu_switch_context's ret returns into preempt_check_irq's
//      caller frame, which returns to vectors.S, which runs
//      .Lexception_return → KERNEL_EXIT → eret. The thread continues
//      from exactly where it was IRQed.
//
// The atomicity of "decrement slice + maybe-set-need-resched" + "clear
// need-resched + call sched" is provided by the IRQ being serialized
// (one IRQ in flight at a time per CPU); concurrent SMP CPUs each have
// their own per-CPU sched_tick (P2-C). Reentrancy of sched() from
// within sched() is prevented by sched()'s irq_save (set above).
// ============================================================================

void sched_tick(void) {
    if (!g_sched_initialized) return;

    struct Thread *t = current_thread();
    if (!t) return;
    if (t->magic != THREAD_MAGIC) return;     // boot transient; ignore
    if (t->state != THREAD_RUNNING) return;    // ignore non-running

    // Decrement; if expired, request preemption + replenish.
    if (--t->slice_remaining <= 0) {
        g_need_resched = true;
        t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
    }
}

void preempt_check_irq(void) {
    // Fast path: scheduler isn't initialized OR no preemption pending.
    // Every IRQ-return runs this; keep it cheap.
    if (!g_need_resched) return;
    if (!g_sched_initialized) return;

    struct Thread *t = current_thread();
    if (!t) return;                             // pre-thread_init IRQ
    if (t->magic != THREAD_MAGIC) return;       // corruption — defer

    // Clear the flag BEFORE sched() so a re-fire-during-sched (which
    // can't happen at UP because IRQs are masked, but defensive depth
    // for SMP) doesn't double-trigger.
    g_need_resched = false;

    // sched() does its own irqsave/irqrestore around the run-tree
    // mutation. We're called from vectors.S IRQ-return path with IRQs
    // masked already (the vector was entered with masking implicit).
    // sched()'s save/restore preserves that — the IRQ stays masked
    // through the cpu_switch_context, then is restored by
    // .Lexception_return's KERNEL_EXIT eret to whatever the IRQed
    // thread had originally.
    sched();
}

// ============================================================================

int wakeup(struct Rendez *r) {
    if (!r) extinction("wakeup(NULL rendez)");

    irq_state_t s = spin_lock_irqsave(&r->lock);

    struct Thread *t = r->waiter;
    if (!t) {
        // No waiter — caller's cond mutation may still be useful if a
        // future sleeper checks it; nothing to do here. This is the
        // intended idempotency: wakeup is a no-op when no thread is
        // sleeping on r.
        spin_unlock_irqrestore(&r->lock, s);
        return 0;
    }

    if (t->magic != THREAD_MAGIC)
        extinction("wakeup: corrupted waiter");
    if (t->state != THREAD_SLEEPING)
        extinction("wakeup: waiter is not SLEEPING");
    if (t->rendez_blocked_on != r)
        extinction("wakeup: waiter rendez backref mismatch");

    // Atomic under r->lock: clear the waiter + transition state +
    // ready. After this step, sleep's resume re-checks cond (which
    // the caller set TRUE before calling wakeup) and exits the loop.
    r->waiter = NULL;
    t->rendez_blocked_on = NULL;
    t->state = THREAD_RUNNABLE;
    ready(t);

    spin_unlock_irqrestore(&r->lock, s);
    return 1;
}
