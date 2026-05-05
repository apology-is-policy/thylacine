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

#include <thylacine/dtb.h>
#include <thylacine/extinction.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

// P2-Cd: per-CPU sched state. Each CPU owns its own band-indexed run
// tree, monotonic vd_counter, init flag, and idle-thread pointer. This
// replaces the global singletons used at P2-Ba..Cc — each CPU now
// dispatches independently against its own queue, and ready() inserts
// into THIS CPU's tree (cross-CPU placement and work-stealing land at
// P2-Ce).
//
// All accesses index by `smp_cpu_idx_self()` (which reads MPIDR_EL1.
// Aff0). Writes to a foreign CPU's slot are not yet supported and
// would require coordinated IPIs (P2-Cdc).
struct CpuSched {
    // P2-Ce: per-CPU run-tree lock. Acquired by ready()/sched()/
    // sched_remove_if_runnable on THIS CPU; acquired via try_lock by
    // peer CPUs during work-stealing. Cross-CPU lock acquisition uses
    // try_lock only — if a peer is already in its critical section
    // (insert/remove/pick), the stealer moves on rather than block.
    spin_lock_t    lock;
    struct Thread *run_tree[SCHED_BAND_COUNT];
    s64            vd_counter;
    bool           initialized;
    struct Thread *idle;             // Per-CPU idle thread. CPU 0 = kthread.

    // P2-Ce: finish_task_switch handoff. Set by prev's sched right
    // before cpu_switch_context: "this is the lock the resuming
    // thread should release." Read by the resuming thread (in its
    // own frame's post-cpu_switch_context resume path, OR in
    // thread_trampoline for fresh threads). The destination CPU's
    // cs.pending_release_lock is what gets read — which is exactly
    // the lock prev held on this CPU when it switched away. Cross-
    // CPU thread migration (work-stealing) doesn't break this:
    // whoever switched TO us on the current CPU set the field
    // correctly for our resume.
    spin_lock_t   *pending_release_lock;

    // P2-Cf: clear-on_cpu handoff. Set by prev's sched right before
    // cpu_switch_context to point at prev (the thread being switched
    // away from on this CPU). Read by the destination CPU's resume
    // path (the thread that's coming on-CPU here) to clear prev's
    // on_cpu flag — signaling to a wakeup() spinner that prev is
    // fully switched out and safe to transition.
    struct Thread *prev_to_clear_on_cpu;
};

static struct CpuSched g_cpu_sched[DTB_MAX_CPUS];

// P2-Cd: per-CPU preemption signal. Each CPU's timer IRQ writes its
// own slot via sched_tick(); preempt_check_irq() on the same CPU
// reads it. No cross-CPU access — the writer/reader is always the
// same CPU because timer IRQs are CPU-local. `volatile` is sufficient
// (no SMP atomicity needed for self-CPU access).
static volatile bool g_need_resched[DTB_MAX_CPUS];

static inline struct CpuSched *this_cpu_sched(void) {
    unsigned idx = smp_cpu_idx_self();
    if (idx >= DTB_MAX_CPUS) extinction("this_cpu_sched: cpu idx out of range");
    return &g_cpu_sched[idx];
}

void sched_init(unsigned cpu_idx) {
    if (cpu_idx >= DTB_MAX_CPUS)
        extinction("sched_init: cpu_idx out of range");
    struct CpuSched *cs = &g_cpu_sched[cpu_idx];
    if (cs->initialized) extinction("sched_init called twice for the same cpu");
    if (!current_thread())
        extinction("sched_init before this CPU's idle thread is parked in TPIDR_EL1");

    spin_lock_init(&cs->lock);
    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        cs->run_tree[b] = NULL;
    }
    cs->vd_counter   = 1;
    cs->idle         = current_thread();   // CPU's idle thread = first
                                           // thing parked in TPIDR_EL1
                                           // before sched_init runs.
    cs->initialized  = true;
}

struct Thread *sched_idle_thread(unsigned cpu_idx) {
    if (cpu_idx >= DTB_MAX_CPUS) return NULL;
    return g_cpu_sched[cpu_idx].idle;
}

// True iff `t` is currently in `cs`'s run tree (linked or as head).
// At v1.0 P2-Cd a thread is only ever in one CPU's tree at a time; the
// caller passes the CPU's CpuSched. Cross-CPU work-stealing (P2-Ce)
// will need a "find which CPU has t" helper.
static bool in_run_tree(struct CpuSched *cs, struct Thread *t) {
    return t->runnable_next != NULL || t->runnable_prev != NULL ||
           cs->run_tree[t->band] == t;
}

// Insert `t` into `cs`'s band tree, sorted ascending by vd_t. Ties (equal
// vd_t) place `t` AFTER existing equal-keyed nodes — FIFO within ties.
static void insert_sorted(struct CpuSched *cs, struct Thread *t) {
    struct Thread **head = &cs->run_tree[t->band];

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

// Remove `t` from `cs`'s band tree. Caller has confirmed t is in the tree.
static void unlink(struct CpuSched *cs, struct Thread *t) {
    if (t->runnable_prev) {
        t->runnable_prev->runnable_next = t->runnable_next;
    } else {
        // t was the head.
        cs->run_tree[t->band] = t->runnable_next;
    }
    if (t->runnable_next) {
        t->runnable_next->runnable_prev = t->runnable_prev;
    }
    t->runnable_next = NULL;
    t->runnable_prev = NULL;
}

// Pick the highest-priority runnable thread from `cs`; remove from its
// tree. Returns NULL if no thread is runnable across any band.
static struct Thread *pick_next(struct CpuSched *cs) {
    for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
        struct Thread *t = cs->run_tree[b];
        if (t) {
            unlink(cs, t);
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

    // P2-Cd/Ce: insert into THIS CPU's run tree under THIS CPU's lock.
    // Cross-CPU thread placement is a P2-Ce work-stealing concern: the
    // peer CPU pulls work from us, not the other way around.
    struct CpuSched *cs = this_cpu_sched();

    // P2-Bc/Ce: IRQ-mask discipline + per-CPU run-tree lock. Disable
    // IRQs first (own-CPU re-entry) then take the lock (cross-CPU
    // contention against work-stealing peers).
    irq_state_t s = spin_lock_irqsave(&cs->lock);

    if (in_run_tree(cs, t))       extinction("ready of already-runnable Thread");

    insert_sorted(cs, t);

    spin_unlock_irqrestore(&cs->lock, s);
}

// P2-Ce: finish_task_switch helper for thread_trampoline.
//
// A freshly-created thread's first run lands at thread_trampoline (set
// in ctx.lr by thread_create). thread_trampoline calls this helper to
// release the run-tree lock that prev held when switching to us.
// Mirrors the spin_unlock + msr daif sequence that the resume path
// of a normal sched()-via-context-switch executes — but called from
// asm, not C, since thread_trampoline doesn't have a sched() frame
// to return into.
//
// Thread_trampoline's IRQ-mask handling (msr daifclr, #2) follows
// this helper, so we don't need to touch DAIF here. This function
// only releases the lock that prev held at switch time.
//
// NULL-guard: thread_switch() (the legacy direct-switch primitive
// used by P2-A context tests) bypasses sched() and does NOT acquire
// a per-CPU run-tree lock. When a fresh thread is the target of a
// thread_switch into, the trampoline still runs, and this helper
// sees pending_release_lock = NULL (or stale from an earlier sched).
// Skip the unlock in that case. The "stale from earlier sched" case
// is closed by clearing pending_release_lock to NULL after each
// consumption.
void sched_arm_clear_on_cpu(struct Thread *prev) {
    struct CpuSched *cs = this_cpu_sched();
    cs->prev_to_clear_on_cpu = prev;
}

void sched_finish_task_switch(void) {
    struct CpuSched *cs = this_cpu_sched();
    // P2-Cf: clear prev's on_cpu (mirror of sched()'s C-side resume
    // path). For fresh threads coming through thread_trampoline, this
    // is the first opportunity post-cpu_switch_context to mark prev
    // as fully switched out.
    struct Thread *prev_to_clear = cs->prev_to_clear_on_cpu;
    cs->prev_to_clear_on_cpu = NULL;
    if (prev_to_clear) {
        __atomic_store_n(&prev_to_clear->on_cpu, false, __ATOMIC_RELEASE);
    }
    spin_lock_t *lk = cs->pending_release_lock;
    cs->pending_release_lock = NULL;
    if (lk) spin_unlock(lk);
}

// P2-Ce: try to steal a runnable thread from a peer CPU's run tree.
// Returns NULL if no peer has work or all peers' locks are held by
// other ops. The stolen thread is REMOVED from the peer's tree;
// caller is responsible for inserting it (or making it `next`) in
// THIS CPU's context.
//
// Must be called with this CPU's run-tree lock HELD (we update
// cs->vd_counter while holding it). Per-peer access uses spin_trylock
// — if any peer is mid-mutation, skip; the next sched() (or this
// CPU's own ready() arrival) will retry.
//
// Stolen thread's vd_t is rebased to fresh on this CPU so it sorts
// at the back of this CPU's rotation. Without rebasing, the stolen
// thread's old vd_t (from peer's clock) could be ahead of or behind
// this CPU's clock, producing arbitrary ordering against locally-
// queued threads.
static struct Thread *try_steal(struct CpuSched *cs) {
    unsigned self = (unsigned)(cs - g_cpu_sched);
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        if (i == self) continue;
        struct CpuSched *peer = &g_cpu_sched[i];
        if (!peer->initialized) continue;

        if (!spin_trylock(&peer->lock)) continue;

        // Pick the highest-priority runnable thread from peer.
        struct Thread *stolen = NULL;
        for (unsigned b = 0; b < SCHED_BAND_COUNT && !stolen; b++) {
            stolen = peer->run_tree[b];
            if (stolen) unlink(peer, stolen);
        }

        spin_unlock(&peer->lock);

        if (stolen) {
            // Rebase vd_t into this CPU's clock space.
            stolen->vd_t = cs->vd_counter++;
            return stolen;
        }
    }
    return NULL;
}

void sched(void) {
    // P2-Cd: read THIS CPU's sched state. All run-tree access goes
    // through `cs`.
    struct CpuSched *cs = this_cpu_sched();
    if (!cs->initialized) extinction("sched() before this CPU's sched_init");

    // P2-Bc/Ce: IRQ-mask + per-CPU run-tree lock. sched() mutates
    // shared state (run tree, current_thread via TPIDR_EL1, vd_t
    // counter) — a timer IRQ firing inside this critical section
    // would re-enter preempt_check_irq → sched(), corrupting the
    // run tree. The per-CPU lock additionally protects against
    // cross-CPU work-stealing: a peer's try_steal will fail to acquire
    // and move on rather than walk our tree mid-mutation.
    irq_state_t s = spin_lock_irqsave(&cs->lock);

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

    struct Thread *next = pick_next(cs);
    if (!next) {
        // P2-Ce: try work-stealing before falling back to local
        // semantics. If a peer CPU has runnable threads we'd otherwise
        // sit idle while they're under-served — pull one over. We hold
        // our own lock through the steal (peer access uses try_lock).
        next = try_steal(cs);
    }
    if (!next) {
        if (prev->state != THREAD_RUNNING) {
            // Block path with no runnable peer anywhere. The thread
            // that's blocking has no waker scheduled (sleep/wakeup
            // protocol broken) — extinct loudly. With work-stealing
            // live, this is a true deadlock signal: no CPU has any
            // runnable thread that could eventually wake us.
            extinction("sched: deadlock — current is blocking, no runnable peer system-wide");
        }
        // Yield path with no runnable peer: keep prev running. Refill
        // slice — sched() was called for preempt or explicit yield;
        // either way, prev is the only runnable thread, so give it a
        // fresh quantum.
        prev->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
        spin_unlock_irqrestore(&cs->lock, s);
        return;
    }

    if (prev == next) {
        // pick_next or steal returned prev. Don't switch to self —
        // re-insert and continue. With work-stealing this can happen
        // if our own pick_next gave NULL but try_steal pulled prev
        // back from a peer (e.g., prev was migrated mid-flight by
        // some future cross-CPU placer). Re-insert under our lock.
        if (prev->state == THREAD_RUNNING) {
            insert_sorted(cs, prev);
        }
        spin_unlock_irqrestore(&cs->lock, s);
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        // Yield: advance vd_t past all currently-runnable threads;
        // insert prev at the back of its band's rotation. P2-Cd: the
        // per-CPU idle thread participates in the run tree like any
        // other thread — it's "always runnable" by being re-inserted
        // here on yield. When a CPU has nothing else queued, sched()
        // picks idle, which calls WFI inside its run loop and yields
        // back when an IRQ wakes it. (Per_cpu_main on secondaries +
        // kthread on primary play this role.)
        prev->vd_t = cs->vd_counter++;
        prev->state = THREAD_RUNNABLE;
        insert_sorted(cs, prev);
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

    // P2-Ce finish_task_switch handoff: stash the lock to be released
    // on the destination CPU. cs->pending_release_lock points at THIS
    // CPU's lock; the resuming thread reads its destination-CPU's
    // pending_release_lock (which after a cross-CPU migration steal-
    // path may differ from `cs` in our local frame). Either way, this
    // CPU's slot is the one whoever resumes here will read.
    cs->pending_release_lock = &cs->lock;

    // P2-Cf wait/wake race close: mark next as on_cpu BEFORE the
    // switch (it's about to be running), and stash prev so the
    // destination CPU's resume path can clear prev->on_cpu AFTER
    // cpu_switch_context completes (prev is fully switched out).
    // wakeup() spins on the waiter's on_cpu, so this transition
    // protects against a peer transitioning a still-running thread.
    __atomic_store_n(&next->on_cpu, true, __ATOMIC_RELAXED);
    cs->prev_to_clear_on_cpu = prev;

    cpu_switch_context(&prev->ctx, &next->ctx);

    // Resumption: prev was switched back to. State and current_thread
    // were set by whichever peer transitioned us out of SLEEPING (via
    // wakeup → ready) or yielded back (via sched picking us). prev is
    // no longer in the run tree (the peer's pick_next removed it
    // before switching). prev's slice was replenished by whichever
    // peer's sched() picked prev (the same code path above, in their
    // frame).
    //
    // P2-Ce: lock release after migration. After cpu_switch_context
    // we may be on a DIFFERENT CPU than at sched-entry (work-stealing
    // moved us). The local `cs` variable points at our entry-CPU's
    // CpuSched, which is no longer the right lock to release. Re-read
    // this_cpu_sched() and unlock its pending_release_lock — the
    // destination CPU's prev set this field before switching to us.
    // Then restore IRQ state from our local frame's `s` (per-thread
    // value, captured at our sched entry).
    {
        struct CpuSched *cs_now = this_cpu_sched();
        // P2-Cf: clear PREV's on_cpu now that cpu_switch_context has
        // saved its full register state. After this release-store any
        // peer's wakeup() spin loop on prev will exit and proceed to
        // transition prev → RUNNABLE safely.
        struct Thread *prev_to_clear = cs_now->prev_to_clear_on_cpu;
        cs_now->prev_to_clear_on_cpu = NULL;
        if (prev_to_clear) {
            __atomic_store_n(&prev_to_clear->on_cpu, false, __ATOMIC_RELEASE);
        }
        spin_lock_t *lk = cs_now->pending_release_lock;
        cs_now->pending_release_lock = NULL;
        spin_unlock(lk);
        __asm__ __volatile__("msr daif, %x0\n" :: "r"(s) : "memory");
    }
}

void sched_remove_if_runnable(struct Thread *t) {
    if (!t) return;
    if (t->state != THREAD_RUNNABLE) return;
    // P2-Cd/Ce: walk every CPU's run tree to find the one that owns t.
    // At v1.0 P2-Ce a thread is only in one CPU's tree at a time
    // (work-stealing transfers ownership atomically under the peer's
    // lock); the first match wins. Take each CPU's lock for the
    // mutation — full lock (not try_lock) because thread_free expects
    // unconditional cleanup; if a peer is mid-mutation we wait.
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        struct CpuSched *cs = &g_cpu_sched[i];
        if (!cs->initialized) continue;
        irq_state_t s = spin_lock_irqsave(&cs->lock);
        if (in_run_tree(cs, t)) {
            unlink(cs, t);
            spin_unlock_irqrestore(&cs->lock, s);
            return;
        }
        spin_unlock_irqrestore(&cs->lock, s);
    }
}

unsigned sched_runnable_count(void) {
    // P2-Cd: aggregate across all CPUs' run trees. Diagnostic only —
    // not a hot-path operation.
    unsigned n = 0;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        struct CpuSched *cs = &g_cpu_sched[i];
        if (!cs->initialized) continue;
        for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
            for (struct Thread *t = cs->run_tree[b]; t; t = t->runnable_next) {
                n++;
            }
        }
    }
    return n;
}

unsigned sched_runnable_count_band(unsigned band) {
    if (band >= SCHED_BAND_COUNT) return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        struct CpuSched *cs = &g_cpu_sched[i];
        if (!cs->initialized) continue;
        for (struct Thread *t = cs->run_tree[band]; t; t = t->runnable_next) {
            n++;
        }
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
    // P2-Cd: per-CPU need_resched + this CPU's sched state.
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) return;
    if (!g_cpu_sched[cpu].initialized) return;

    struct Thread *t = current_thread();
    if (!t) return;
    if (t->magic != THREAD_MAGIC) return;     // boot transient; ignore
    if (t->state != THREAD_RUNNING) return;    // ignore non-running

    // Decrement; if expired, request preemption + replenish. The flag
    // is per-CPU — each CPU's IRQ writes its own slot.
    if (--t->slice_remaining <= 0) {
        g_need_resched[cpu] = true;
        t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
    }
}

void preempt_check_irq(void) {
    // Fast path: scheduler isn't initialized OR no preemption pending.
    // Every IRQ-return runs this; keep it cheap. P2-Cd: per-CPU flag.
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) return;
    if (!g_need_resched[cpu]) return;
    if (!g_cpu_sched[cpu].initialized) return;

    struct Thread *t = current_thread();
    if (!t) return;                             // pre-thread_init IRQ
    if (t->magic != THREAD_MAGIC) return;       // corruption — defer

    // Clear the flag BEFORE sched() so a re-fire-during-sched (which
    // can't happen on the same CPU because IRQs are masked, but
    // defensive depth for SMP cross-CPU writes via IPI_RESCHED at
    // P2-Cdc) doesn't double-trigger.
    g_need_resched[cpu] = false;

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

    // P2-Cf: SMP wait/wake race close. If t is still mid-switch-out
    // on its previous CPU (cpu_switch_context still saving regs to
    // t->ctx), transitioning t to RUNNABLE + ready() would let a
    // peer pick t before its ctx is canonical — concurrent execution
    // on two CPUs from a half-saved context. Spin until t->on_cpu
    // becomes false, signaling that the previous CPU's resume path
    // cleared the flag (= cpu_switch_context completed).
    //
    // On UP / single-CPU rendez tests this is a no-op: t is already
    // off-CPU when wakeup runs (t entered sched() inside sleep()
    // and was switched out before any wakeup observer could run).
    while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        __asm__ __volatile__("yield" ::: "memory");
    }

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
