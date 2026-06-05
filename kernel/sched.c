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

#include "../arch/arm64/gic.h"      // P3-G: gic_send_ipi for ready/wakeup wake-idle-peer.
#include "../arch/arm64/timer.h"    // P5-tsleep: counter read + ns->counter conversion.
#include "../arch/arm64/uart.h"     // DEBUG (#857): sched_dump_runnable diagnostic.

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

    // P3-G: WFI signaling for cross-CPU wake. Set TRUE by THIS CPU
    // immediately before its `wfi` instruction in per_cpu_main's idle
    // loop; cleared TRUE→FALSE by THIS CPU immediately after WFI exits
    // (via any IRQ — IPI_RESCHED is the canonical case). Read by PEER
    // CPUs in `sched_notify_idle_peer()` to identify a wake target.
    //
    // Race semantics: peer's read can stall behind this CPU's wfi-exit
    // clear, causing a spurious IPI to a CPU that just woke up. The
    // IPI handler is a no-op + count, so the spurious IPI is harmless.
    // Use of `volatile` is sufficient — the writer/reader access is
    // single-store/single-load with no derived data; relaxed atomic
    // semantics are what we want.
    //
    // Boot CPU: stays FALSE forever (boot's idle is the kthread driven
    // by timer IRQ, not per_cpu_main's explicit wfi loop). At v1.0 the
    // primary's post-init flow sits in `_torpor`'s asm wfi loop with no
    // C-level set/clear hooks; secondaries are the only candidates for
    // wake-via-IPI placement. Modeling this in `scheduler.tla` is
    // covered by `EnterWFI(cpu)` + `IPI_Deliver`-clears-`wfi[dst]`.
    volatile bool  idle_in_wfi;
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
    // #801/F6: published to other CPUs by per_cpu_main's `dsb sy` after
    // sched_init returns (smp.c); a secondary's try_steal / sched read of a
    // peer's `initialized` relies on that barrier as the acquire side, so the
    // plain store suffices. If that dsb is ever removed, make this a RELEASE
    // store + the peer reads ACQUIRE loads.
    cs->initialized  = true;
}

struct Thread *sched_idle_thread(unsigned cpu_idx) {
    if (cpu_idx >= DTB_MAX_CPUS) return NULL;
    return g_cpu_sched[cpu_idx].idle;
}

void sched_set_idle_in_wfi(bool in_wfi) {
    unsigned idx = smp_cpu_idx_self();
    if (idx >= DTB_MAX_CPUS) return;
    if (!g_cpu_sched[idx].initialized) return;
    g_cpu_sched[idx].idle_in_wfi = in_wfi;
}

bool sched_idle_in_wfi(unsigned cpu_idx) {
    if (cpu_idx >= DTB_MAX_CPUS) return false;
    if (!g_cpu_sched[cpu_idx].initialized) return false;
    return g_cpu_sched[cpu_idx].idle_in_wfi;
}

// P4-Ic6-impl (R12-sched): boot CPU's deadlock-path idle thread.
// Registered via sched_set_bootcpu_idle in boot_main; consulted by
// sched()'s deadlock path. Read-only after init; no lock needed (set
// once before tests + secondaries running).
static struct Thread *g_bootcpu_idle;

void sched_set_bootcpu_idle(struct Thread *t) {
    if (g_bootcpu_idle) extinction("sched_set_bootcpu_idle called twice");
    if (!t) extinction("sched_set_bootcpu_idle(NULL)");
    if (t->magic != THREAD_MAGIC) extinction("sched_set_bootcpu_idle: corrupted Thread");
    if (t->band != SCHED_BAND_IDLE) extinction("sched_set_bootcpu_idle: thread must be SCHED_BAND_IDLE");
    g_bootcpu_idle = t;
}

// P4-Ic6-impl (R12-sched): boot CPU's idle loop. See sched.h doc.
//
// Body mirrors per_cpu_main's tail (kernel/smp.c) for secondaries. The
// IRQ-mask-then-set-flag-then-sched-then-wfi ordering is the R7 F128
// close: peer notifiers observe idle_in_wfi=TRUE before the wfi commits,
// so a peer that calls ready() with new work between (flag set) and
// (wfi enters) will send an IPI that wfi sees as pending and exits on.
//
// Reached via thread_trampoline → blr x21 with x21 = bootcpu_idle_main
// (set by thread_create in boot_main). The trampoline already calls
// sched_finish_task_switch + msr daifclr,#2 before reaching here, so
// PSTATE.I is unmasked on entry. The first spin_lock_irqsave(NULL)
// re-masks for the body.
__attribute__((noreturn))
void bootcpu_idle_main(void) {
    for (;;) {
        irq_state_t s = spin_lock_irqsave(NULL);
        sched_set_idle_in_wfi(true);
        sched();
        __asm__ __volatile__("wfi" ::: "memory");
        sched_set_idle_in_wfi(false);
        spin_unlock_irqrestore(NULL, s);
    }
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
    // #860 invariant tripwire (durable regression guard): g_bootcpu_idle is the
    // CPU-0-pinned deadlock-path idle; it is the ONE idle-band thread with a
    // real kstack (the secondaries' idles are kstack_base==NULL), so if it ever
    // enters a run tree it becomes stealable by a secondary (try_steal's only
    // gate is kstack_base != NULL) AND re-pickable by cpu 0's deadlock path ->
    // two CPUs run it on one kstack/ctx -> cross-CPU context corruption (the
    // #860 wild-PC / smashed-stack crash). It MUST stay off-tree. Deterministic:
    // fires at the INSERTION (the precondition), not the rare double-run.
    if (t == g_bootcpu_idle)
        extinction("#860: g_bootcpu_idle entered a run tree (must stay off-tree)");
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

// P3-G: notify-idle-peer toggle. Off during in-kernel tests (UP-like),
// on once production /init starts. Tests are written against UP semantics
// (e.g., rendez.basic_handoff assumes consumer runs on the same CPU that
// readied it); enabling cross-CPU work-stealing during tests breaks those
// assumptions. The toggle keeps tests UP-like while letting real workload
// (post-test, /init and beyond) benefit from work-stealing.
//
// `sched_set_notify_enabled(true)` is called from boot_main between
// `test_run_all()` and `joey_run()`. After that point, every ready() /
// wakeup() call wakes an idle peer if any.
//
// Reads/writes of g_sched_notify_enabled use volatile + relaxed — the
// flag is set once at boot, observed by every CPU; no synchronization
// needed beyond memory ordering of the boot-CPU's set being visible
// before the first cross-CPU read (boot's banner UART writes after the
// set provide an implicit dsb-equivalent boundary).
static volatile bool g_sched_notify_enabled;

void sched_set_notify_enabled(bool enabled) {
    g_sched_notify_enabled = enabled;
}

// P3-G: pick an idle peer CPU (one currently halted in WFI, identified by
// cs->idle_in_wfi) and send it IPI_RESCHED to wake it. Returns true if an
// IPI was sent, false if no peer was in WFI.
//
// Walks `g_cpu_sched[i]` for i ≠ self; first peer with `idle_in_wfi==true`
// wins. Stops on first send — only one peer needs to wake to run try_steal.
// Multiple-peer wakes would be a thundering herd (all wake, all try_steal,
// only one gets the work).
//
// Closes R5-H F78. Without this, ready/wakeup placing work on this CPU's
// tree leaves idle peer CPUs stuck in WFI indefinitely (secondaries have
// no per-CPU timer at v1.0; only IPI wakes WFI). Maps to scheduler.tla
// `NotifyWFIPeer(src, dst)` action.
//
// Called WITHOUT cs->lock held — peer's wake handler doesn't contend with
// this CPU's tree. The IPI itself is fire-and-forget (gic_send_ipi sets
// the SGI pend bit; GIC delivers asynchronously).
//
// The caller's CPU is excluded (self can't be in WFI while running ready).
//
// Gated by g_sched_notify_enabled — disabled during in-kernel tests,
// enabled before /init runs.
static bool sched_notify_idle_peer(void) {
    if (!g_sched_notify_enabled) return false;

    unsigned self = smp_cpu_idx_self();
    unsigned online = smp_cpu_online_count();
    if (online <= 1) return false;     // UP: no peers to wake.

    // Walk peers in cyclic order starting from self+1 (mod max). The
    // rotation distributes wakes across peers when many readys fire
    // concurrently. We use online count for the bound; CPUs above
    // `online` are guaranteed to have idle_in_wfi=false (uninitialized
    // sched state).
    for (unsigned k = 1; k < DTB_MAX_CPUS; k++) {
        unsigned i = (self + k) % DTB_MAX_CPUS;
        if (i == self) continue;
        if (i >= DTB_MAX_CPUS) continue;
        struct CpuSched *peer = &g_cpu_sched[i];
        if (!peer->initialized) continue;
        // Relaxed read — the racy outcome (peer woke between read and IPI)
        // is benign: the IPI fires, peer's already-running IPI handler
        // runs as a no-op, control returns to peer's loop.
        if (!peer->idle_in_wfi) continue;
        // Found an idle peer. Send IPI; ignore failure (gic_send_ipi
        // returns false on out-of-range CPU idx, which can't happen here).
        (void)gic_send_ipi(i, IPI_RESCHED);
        return true;
    }
    return false;
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

    // P3-G: notify an idle peer (in WFI) so it can wake and try_steal
    // the new work. Done AFTER releasing our lock so the peer's IPI
    // handler doesn't contend on it. Closes R5-H F78. Maps to
    // scheduler.tla `NotifyWFIPeer(src=self, dst=peer)`.
    (void)sched_notify_idle_peer();
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
// P2-Dd: rotate the start index per try_steal invocation. With multiple
// CPUs concurrently trying to steal, fixed-order scanning concentrates
// spin_trylock contention on CPU 0; rotating spreads it. The counter
// is a single global atomic that all CPUs increment, so successive
// try_steal calls (from any CPU) get sequential start offsets. The
// spread is approximate, not strictly fair — CPUs racing on the
// counter may both observe similar values — but it's enough to break
// the pathological "everyone hits CPU 0 first" pattern.
static volatile u32 g_try_steal_rotate;

// P3-G: try_steal returns NULL on either "all peers genuinely empty" OR
// "some peer's lock was held — couldn't see its tree". Distinguishing
// matters at the SLEEPING-prev path: with prev blocking, NULL+contended
// is a transient race (retry); NULL+empty is a true deadlock (extinct).
// `contended_out` is set TRUE iff at least one peer was skipped due to
// spin_trylock failure. Closes R5-H F77.
static struct Thread *try_steal(struct CpuSched *cs, bool *contended_out) {
    if (contended_out) *contended_out = false;
    unsigned self = (unsigned)(cs - g_cpu_sched);
    u32 base = __atomic_fetch_add(&g_try_steal_rotate, 1u, __ATOMIC_RELAXED);

    for (unsigned k = 0; k < DTB_MAX_CPUS; k++) {
        unsigned i = (unsigned)((base + k) % DTB_MAX_CPUS);
        if (i == self) continue;
        struct CpuSched *peer = &g_cpu_sched[i];
        if (!peer->initialized) continue;

        if (!spin_trylock(&peer->lock)) {
            // Peer is mid-mutation (insert/remove/pick). We couldn't see
            // its tree. Mark contended so the caller can distinguish
            // "saw nothing" from "couldn't see anything".
            if (contended_out) *contended_out = true;
            continue;
        }

        // Pick the highest-priority runnable thread from peer.
        //
        // P5-el1h-kernel (audit F2 / invariant I-21): never steal a
        // thread whose kstack_base is NULL. Such a thread is a bootstrap
        // context — kthread, or a per-CPU idle thread from
        // thread_init_per_cpu_idle — that runs on a CPU's *boot* stack,
        // not on a portable per-thread kstack. Under the uniform-EL1h
        // model exception frames are built on the running thread's
        // stack; migrating a boot-stack thread to another CPU would run
        // it (and build its exception frames) on a stack its origin CPU
        // still owns — cross-CPU stack corruption. Boot-stack threads
        // are therefore CPU-pinned. The realistic case is a per-CPU idle
        // thread sitting in run_tree[BAND_IDLE]; skipping it there costs
        // nothing (an idle thread is not work worth stealing).
        // #860: g_bootcpu_idle is CPU-0-pinned despite owning a real kstack
        // (so the kstack_base != NULL gate alone does NOT exclude it the way it
        // excludes the secondaries' kstack_base==NULL idles). It must stay
        // off-tree (the yield path enforces that), so this guard is defense in
        // depth -- a backstop if any future path leaks it into a tree.
        struct Thread *stolen = NULL;
        for (unsigned b = 0; b < SCHED_BAND_COUNT && !stolen; b++) {
            struct Thread *cand = peer->run_tree[b];
            if (cand && cand->kstack_base != NULL && cand != g_bootcpu_idle) {
                stolen = cand;
                unlink(peer, stolen);
            }
        }

        // #801/F1: claim the victim under peer->lock BEFORE releasing it.
        // Otherwise `stolen` sits out-of-tree + RUNNABLE + on_cpu==false in an
        // unlocked limbo between here and the picker's on_cpu set (~line 670),
        // so a concurrent thread_free could observe it free-able and reclaim it
        // mid-steal (a UAF on its ctx/kstack -- the F1 window the SMP prosecutor
        // found; production-unreachable today since only tests free a RUNNABLE
        // in-tree thread, but closed here for any future caller). Setting on_cpu
        // under peer->lock means a racing thread_free -- whose
        // sched_remove_if_runnable walk also takes peer->lock -- either unlinked
        // `stolen` FIRST (we won't find it) or observes on_cpu==true after its
        // walk and waits the steal out via its on_cpu spin. RELAXED matches the
        // picker's set; the peer->lock release/acquire is the inter-CPU publish
        // edge. on_cpu is later cleared normally when `stolen` is switched out.
        if (stolen)
            __atomic_store_n(&stolen->on_cpu, true, __ATOMIC_RELAXED);

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

    // R5-H F93: clear the per-CPU need_resched flag at sched() entry.
    // preempt_check_irq already clears before calling, so this is a no-op
    // on the IRQ-driven path. For voluntary sched() callers (sleep,
    // exits, explicit yield), this absorbs a stale flag set by sched_tick
    // since the last preempt_check_irq — preventing a redundant
    // preempt_check_irq → sched() spin on the next IRQ. Safe at this
    // location because g_need_resched is per-CPU; only this CPU writes
    // its own slot (sched_tick + this clear), and the fetch is monotonic
    // (set TRUE → cleared FALSE → set TRUE again on next slice expiry).
    g_need_resched[smp_cpu_idx_self()] = false;

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
    bool steal_contended = false;
    if (!next) {
        // P2-Ce: try work-stealing before falling back to local
        // semantics. If a peer CPU has runnable threads we'd otherwise
        // sit idle while they're under-served — pull one over. We hold
        // our own lock through the steal (peer access uses try_lock).
        // P3-G: pass contention sentinel so we can distinguish "saw
        // peers empty" from "couldn't see peers due to lock-held".
        next = try_steal(cs, &steal_contended);

        // P3-G F77 close: if try_steal failed AND some peer was contended
        // AND we're on the blocking path (extinction would fire below),
        // retry once. The peer's mid-mutation is transient (single critical
        // section in ready/sched/sched_remove_if_runnable); a brief spin
        // before re-querying gives it time to release. Yield-path callers
        // (prev RUNNING) don't need the retry — they can keep running prev
        // and try again on the next sched.
        if (!next && steal_contended && prev->state != THREAD_RUNNING) {
            // Brief CPU-relax so the peer holding the contended lock has
            // time to release. The cap (~256 iters) is bounded so a
            // truly-empty system still extincts in finite time.
            for (volatile unsigned r = 0; r < 256; r++) { /* yield */ }
            next = try_steal(cs, &steal_contended);
        }
    }
    if (!next) {
        if (prev->state != THREAD_RUNNING) {
            // P4-Ic6-impl (R12-sched): fall back to the boot CPU's
            // dedicated idle thread instead of extincting. Pre-fix
            // this was an unconditional ELE because no idle thread
            // existed distinct from kthread (kthread doubled as
            // cs->idle); kthread blocking with no peer meant the
            // kernel had nothing to switch to. The boot CPU has
            // g_bootcpu_idle (allocated in boot_main); sched() switches
            // to it explicitly on the deadlock path. The idle thread
            // WFI loops until an IRQ arrives that makes some thread
            // RUNNABLE; its own sched() then picks that thread up.
            //
            // g_bootcpu_idle is kept OFF the run tree, and that is a HARD
            // correctness requirement (#860), NOT an artifact: it owns a real
            // kstack (thread_create in boot_main), so unlike the secondaries'
            // kstack_base==NULL idles it would PASS try_steal's gate. If it
            // entered a tree a secondary would steal it while this deadlock path
            // concurrently re-picks it -> two CPUs run it on one kstack/ctx ->
            // context corruption (the #860 wild-PC / smashed-stack crash). The
            // yield path keeps it off-tree; this path is its sole dispatcher.
            // (P4-Ic6's original off-tree motivation -- dodging the pre-P5
            // EL1t/EL1h exception-stack clobber -- is obsolete under uniform
            // EL1h/I-21, but the off-tree invariant is now load-bearing for a
            // DIFFERENT reason: stealability. Folding it into the tree like the
            // secondaries' idles would REINTRODUCE the bug, not clean it up.)
            //
            // Only the boot CPU has g_bootcpu_idle registered. For secondaries
            // this path is a genuine deadlock (their per_cpu_main idle is
            // kstack_base==NULL + in run_tree[BAND_IDLE], so pick_next normally
            // finds it). Defensive extinction kept for secondaries / mis-init.
            if (g_bootcpu_idle && smp_cpu_idx_self() == 0) {
                // #860 belt-and-suspenders: g_bootcpu_idle is off-tree +
                // unstealable, so it can only ever run here on cpu 0 -- it must
                // NOT already be on_cpu. A true on_cpu here is a double-dispatch
                // (the exact corruption); fail loud rather than corrupt silent.
                if (__atomic_load_n(&g_bootcpu_idle->on_cpu, __ATOMIC_ACQUIRE))
                    extinction("#860: g_bootcpu_idle already on_cpu at deadlock dispatch");
                next = g_bootcpu_idle;
            } else {
                extinction("sched: deadlock — current is blocking, no runnable peer system-wide");
            }
        }
    }
    if (!next) {
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
        // Yield: advance vd_t past all currently-runnable threads; insert prev
        // at the back of its band's rotation. The SECONDARY per-CPU idle
        // threads participate in the run tree like any other thread -- they are
        // kstack_base==NULL, so try_steal skips them (CPU-pinned to their boot
        // stack). g_bootcpu_idle is the #860 EXCEPTION: it owns a REAL kstack
        // (thread_create in boot_main -- cpu 0's boot stack belongs to kthread,
        // so cpu 0's idle can't be a bare-boot-stack thread like the
        // secondaries'). If it entered a tree it would be stealable (try_steal's
        // only gate is kstack_base != NULL) AND concurrently re-pickable by cpu
        // 0's deadlock path -> two CPUs run it on one kstack/ctx -> cross-CPU
        // context corruption. So it MUST stay off-tree: on yield it goes
        // RUNNABLE but is NOT inserted; the deadlock path below is its sole
        // dispatcher, re-selecting it from that RUNNABLE-off-tree resting state.
        prev->vd_t = cs->vd_counter++;
        prev->state = THREAD_RUNNABLE;
        if (prev != g_bootcpu_idle)
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
    // #801/F3: the store is RELAXED, not RELEASE -- correct because the only
    // inter-CPU edge guarding ctx/kstack reuse is the RELEASE-clear (~line 700)
    // -> ACQUIRE-load (thread_free / reap / wakeup) pairing; this set is consumed
    // in program order on this CPU. Any future reader that branches on
    // on_cpu==true for SAFETY must wait for false, never trust true. (The steal
    // path sets on_cpu earlier under peer->lock -- see try_steal -- so a stolen
    // `next` may already read true here; the idempotent re-store is harmless.)
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
    // P2-Cd: aggregate runnable WORK across all CPUs' run trees. Diagnostic
    // only -- not a hot-path operation, and consulted by NO scheduling
    // decision (the /ctl `runnable` line, the boot print, and the in-kernel
    // quiescence assertions).
    //
    // SCHED_BAND_IDLE is EXCLUDED on purpose. The per-CPU idle threads are
    // always-runnable scheduler infrastructure, not pending work: each
    // secondary's idle thread lives in cs->run_tree[BAND_IDLE] ("participates
    // in the run tree like any other thread", sched() below), and the boot
    // CPU's g_bootcpu_idle is the off-tree exception. Counting the in-tree
    // secondary idles made this report a phantom backlog on an idle multi-CPU
    // system, and made the `sched_runnable_count()==0` quiescence assertions
    // race a secondary idle thread that, under host load, is in-tree (rather
    // than the running thread) at the check instant -- the #857 "smp8 cons.*
    // flake", which was never console_mgr and never a kernel fault, just a
    // benign idle thread miscounted as work. Real runnable work
    // (BAND_INTERACTIVE / BAND_NORMAL) is still counted, so a genuinely
    // stranded work thread is never masked. (g_bootcpu_idle MUST stay off-tree
    // -- #860: it owns a real kstack so a tree entry makes it stealable; it is
    // never counted here regardless.)
    unsigned n = 0;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        struct CpuSched *cs = &g_cpu_sched[i];
        if (!cs->initialized) continue;
        for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
            if (b == SCHED_BAND_IDLE) continue;
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

// Best-effort snapshot of every runnable thread across ALL CPUs' run trees
// (every band, INCLUDING idle -- the dump shows everything so a quiescence
// failure can be fingerprinted), with enough identity (cpu / tid / band /
// state / on_cpu / proc) to name a thread. Lock-free on purpose: it is called
// from the test-fail path and the per-test quiescence guard, where a racing
// mutation during the walk is acceptable -- the goal is to SEE the thread, not
// a consistent count. Cost is paid only on the failure path, never the hot
// path. This is the instrument that cracked #857 (cpu=1 tid=1 band=IDLE = a
// benign secondary idle thread, not a stranded work thread).
void sched_dump_runnable(const char *tag) {
    uart_puts("  [runnable-dump ");
    uart_puts(tag ? tag : "?");
    uart_puts("]\n");
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        struct CpuSched *cs = &g_cpu_sched[i];
        if (!cs->initialized) continue;
        for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
            for (struct Thread *t = cs->run_tree[b]; t; t = t->runnable_next) {
                uart_puts("    cpu=");    uart_putdec((u64)i);
                uart_puts(" tid=");       uart_putdec((u64)(unsigned)t->tid);
                uart_puts(" band=");      uart_putdec((u64)b);
                uart_puts(" state=");     uart_putdec((u64)t->state);
                uart_puts(" on_cpu=");    uart_putdec((u64)(t->on_cpu ? 1u : 0u));
                uart_puts(" proc=");      uart_puthex64((u64)(uintptr_t)t->proc);
                uart_puts(" magic_ok=");  uart_putdec((u64)(t->magic == THREAD_MAGIC ? 1u : 0u));
                uart_puts("\n");
            }
        }
    }
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

// P5-tsleep: the global timer-wait list. Every thread inside a
// deadlined tsleep() is linked here; sched_tick() scans the list on
// each timer fire and wakes any thread whose deadline has passed.
//
// Lock order: g_timerwait.lock -> Rendez.lock -> CpuSched.lock. wakeup()
// takes g_timerwait.lock as its OUTER lock even for a plain sleep()
// waiter (which is never on the list) — it cannot know whether the
// waiter is a deadlined sleeper until it holds the Rendez lock, and the
// order forbids taking g_timerwait.lock second.
//
// One global lock, not per-CPU: a deadlined wait is the cold path (a
// hung-server backstop; poll/futex timeouts), the scan is O(timed
// sleepers) which is small, and the global lock is what specs/tsleep.tla
// verifies. Per-CPU sharding is a documented future optimization.
static struct {
    spin_lock_t    lock;
    struct Thread *head;
} g_timerwait = { SPIN_LOCK_INIT, NULL };

// True iff t is on the timer-wait list. Caller holds g_timerwait.lock.
// Three-way check mirrors in_run_tree: a sole list element has both
// links NULL but is the head.
static bool timerwait_is_linked(struct Thread *t) {
    return t->timerwait_next != NULL || t->timerwait_prev != NULL ||
           g_timerwait.head == t;
}

// Prepend t to the timer-wait list. Caller holds g_timerwait.lock and
// has confirmed t is not already linked.
static void timerwait_link(struct Thread *t) {
    t->timerwait_prev = NULL;
    t->timerwait_next = g_timerwait.head;
    if (g_timerwait.head) g_timerwait.head->timerwait_prev = t;
    g_timerwait.head = t;
}

// Remove t from the timer-wait list. Caller holds g_timerwait.lock and
// has confirmed t is linked.
static void timerwait_unlink(struct Thread *t) {
    if (t->timerwait_prev) {
        t->timerwait_prev->timerwait_next = t->timerwait_next;
    } else {
        g_timerwait.head = t->timerwait_next;
    }
    if (t->timerwait_next) {
        t->timerwait_next->timerwait_prev = t->timerwait_prev;
    }
    t->timerwait_next = NULL;
    t->timerwait_prev = NULL;
}

// P5-tsleep: the wake transition shared by wakeup() and the sched_tick
// timeout scan. Transitions the SLEEPING waiter t of r to RUNNABLE and
// readies it. `timed_out` records the cause in t->sleep_timedout for
// tsleep's resume.
//
// Caller holds r->lock, has validated r->waiter == t with t SLEEPING on
// r, AND has already unlinked t from the timer-wait list (under
// g_timerwait.lock) if it was a deadlined sleeper. This runs under
// r->lock ONLY — never g_timerwait.lock: the on_cpu spin and ready() are
// deliberately kept off the global lock so a wakeup racing a context
// switch cannot stall every CPU's timerwait_tick.
//
// Maps to the wake effect shared by tsleep.tla's Wakeup and Timeout.
static void wake_rendez_waiter(struct Rendez *r, struct Thread *t,
                               bool timed_out) {
    // P2-Cf SMP race: if t is still mid-switch-out on its previous CPU
    // (cpu_switch_context still saving regs to t->ctx), readying it
    // would let a peer pick it from a half-saved context. Spin until
    // the previous CPU's resume path clears on_cpu. wakeup() can hit
    // this window; timerwait_tick() pre-filters on_cpu, so the spin is
    // a no-op when the caller is the timeout scan.
    while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        __asm__ __volatile__("yield" ::: "memory");
    }
    r->waiter            = NULL;
    // P6 #811: do NOT clear t->rendez_blocked_on here. Only the OWNING Thread
    // mutates it -- it is cleared on the owner's sleep/tsleep resume, under the
    // owner's own wait_lock -- so the group-terminate cascade can read it
    // lock-safely (under wait_lock). Clearing it here (under r->lock, not
    // wait_lock) would race the cascade's read. The owner is still SLEEPING at
    // this point, so rendez_blocked_on stays == r until it resumes and clears.
    t->sleep_timedout    = timed_out;
    t->state             = THREAD_RUNNABLE;
    ready(t);
}

int sleep(struct Rendez *r, int (*cond)(void *arg), void *arg) {
    if (!r)    extinction("sleep(NULL rendez)");
    if (!cond) extinction("sleep with NULL cond");

    struct Thread *t = current_thread();
    if (!t)                       extinction("sleep: no current thread");
    if (t->magic != THREAD_MAGIC) extinction("sleep: corrupted current");

    // P6 #811: wait_lock is the OUTERMOST wait-lock (ARCH §8.8.1) and carries
    // the IRQ mask for the whole call (incl. the sched() yields). It is taken
    // BEFORE r->lock so the group-terminate cascade -- which holds a peer's
    // wait_lock while it reads rendez_blocked_on and wakeup()s the rendez --
    // is serialized against this Thread's register-then-observe of
    // group_exit_msg (the I-9 death-wake close).
    irq_state_t s = spin_lock_irqsave(&t->wait_lock);
    spin_lock(&r->lock);

    int rc = SLEEP_OK;
    // Loop: re-check cond on each wakeup. Single-waiter UP: cond should be true
    // after a normal wakeup. The loop also absorbs spurious / cascade wakes.
    while (!cond(arg)) {
        if (r->waiter)
            extinction("sleep: rendez already has a waiter (single-waiter discipline)");
        if (t->state != THREAD_RUNNING)
            extinction("sleep: current is not RUNNING");
        if (t->rendez_blocked_on)
            extinction("sleep: current already blocked on a rendez");

        // Register under wait_lock + r->lock: enqueue + state transition. Any
        // wakeup after the unlock below sees waiter == t and walks correctly.
        r->waiter            = t;
        t->rendez_blocked_on = r;
        t->state             = THREAD_SLEEPING;

        // Register-then-observe (I-9 death-wake close, ARCH §8.8.1): re-check
        // the Proc's group_exit_msg UNDER wait_lock -- the same lock the
        // cascade (proc_group_terminate) takes per peer. If set: either we
        // registered before the cascade's walk (it finds + wakes us), or the
        // flag-set happens-before our wait_lock acquire (so this acquire-load
        // observes it). Either way we must NOT sleep through the group exit:
        // undo the registration and return SLEEP_INTR -- the caller unwinds and
        // the Thread dies at its EL0-return die-check. kproc never group-
        // terminates, so its kernel threads never take this branch.
        if (t->proc &&
            __atomic_load_n(&t->proc->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) {
            r->waiter            = NULL;
            t->rendez_blocked_on = NULL;
            t->state             = THREAD_RUNNING;
            rc = SLEEP_INTR;
            break;
        }

        // Drop BOTH locks before sched() (the IRQ mask stays via the wait_lock
        // irqsave). wait_lock MUST NOT be held across sched() -- a descheduled
        // sleeper holding it would deadlock the cascade. sched() sees state ==
        // SLEEPING and keeps t out of the run tree; the unlock..sched window is
        // the canonical wait/wake race closed by the on_cpu finish-task-switch.
        spin_unlock(&r->lock);
        spin_unlock(&t->wait_lock);

        sched();

        // Resumed: a normal wakeup, a timeout, or the cascade transitioned us
        // RUNNABLE -> RUNNING. wake_rendez_waiter no longer clears
        // rendez_blocked_on (only the owner does), so clear it here under
        // wait_lock before re-evaluating cond. Re-acquire wait_lock then r->lock.
        spin_lock(&t->wait_lock);
        t->rendez_blocked_on = NULL;
        spin_lock(&r->lock);

        // Woken by the cascade? Return INTR rather than looping (the next
        // register-then-observe would catch it anyway -- this is the prompt path).
        if (t->proc &&
            __atomic_load_n(&t->proc->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) {
            rc = SLEEP_INTR;
            break;
        }
    }

    spin_unlock(&r->lock);
    spin_unlock_irqrestore(&t->wait_lock, s);
    return rc;
}

// P5-tsleep: sleep bounded by an absolute deadline. See rendez.h for the
// contract. Modeled by specs/tsleep.tla; the action names in the
// comments below cross-reference that spec.
//
// The deadline adds a third wake source (the timer, via timerwait_tick)
// to sleep's two (cond + wakeup). g_timerwait.lock + r->lock serialize
// all three so the waiter is woken exactly once; cond is re-checked
// first on every (re-)evaluation so a wait satisfied at the deadline
// reports AWOKEN (tsleep.tla WokenSound / TimeoutSound).
int tsleep(struct Rendez *r, int (*cond)(void *arg), void *arg,
           u64 deadline_ns) {
    if (!r)    extinction("tsleep(NULL rendez)");
    if (!cond) extinction("tsleep with NULL cond");

    // No deadline: tsleep degrades to plain sleep (ARCH §8.8). sleep is the
    // spec-proven path (scheduler.tla NoMissedWakeup); route through it. Map
    // sleep's death-interrupt (SLEEP_INTR) onto TSLEEP_INTR; otherwise AWOKEN.
    if (deadline_ns == 0) {
        return (sleep(r, cond, arg) == SLEEP_INTR) ? TSLEEP_INTR : TSLEEP_AWOKEN;
    }

    u64 deadline_cnt = timer_ns_to_counter(deadline_ns);

    struct Thread *t = current_thread();
    if (!t)                       extinction("tsleep: no current thread");
    if (t->magic != THREAD_MAGIC) extinction("tsleep: corrupted current");

    // P6 #811: wait_lock is the OUTERMOST wait-lock (ARCH §8.8.1), then
    // g_timerwait.lock, then r->lock. The irqsave on wait_lock carries the IRQ
    // mask across the whole call (incl. the sched() yields). wait_lock taken
    // before g_timerwait/r->lock so the cascade's per-peer wait_lock walk is
    // serialized against this Thread's register-then-observe.
    irq_state_t s = spin_lock_irqsave(&t->wait_lock);
    spin_lock(&g_timerwait.lock);
    spin_lock(&r->lock);

    if (t->state != THREAD_RUNNING)
        extinction("tsleep: current is not RUNNING");
    if (t->rendez_blocked_on)
        extinction("tsleep: current already blocked on a rendez");

    // Fresh wait: clear any timeout flag a prior tsleep left set.
    t->sleep_timedout = false;

    int ret;
    for (;;) {
        // cond has precedence — tsleep.tla's correct Commit checks cond first,
        // so a wait satisfied at (or past) the deadline still reports AWOKEN.
        if (cond(arg)) { ret = TSLEEP_AWOKEN; break; }

        // Timed out: the tick scan flagged us (sleep_timedout), or the deadline
        // already lay in the past at this (re-)evaluation.
        if (t->sleep_timedout || timer_get_counter() >= deadline_cnt) {
            ret = TSLEEP_TIMEDOUT;
            break;
        }

        if (r->waiter)
            extinction("tsleep: rendez already has a waiter (single-waiter discipline)");
        if (timerwait_is_linked(t))
            extinction("tsleep: current already on the timer-wait list");

        // Atomic under wait_lock + g_timerwait.lock + r->lock: enqueue on the
        // Rendez AND the timer-wait list, then transition to SLEEPING.
        r->waiter            = t;
        t->rendez_blocked_on = r;
        t->sleep_deadline    = deadline_cnt;
        timerwait_link(t);
        t->state             = THREAD_SLEEPING;

        // Register-then-observe (I-9 death-wake close, ARCH §8.8.1): identical
        // to sleep(). Set => undo the FULL registration (rendez + timer-wait)
        // and return TSLEEP_INTR; the caller unwinds + the Thread dies at its
        // EL0-return die-check.
        if (t->proc &&
            __atomic_load_n(&t->proc->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) {
            r->waiter            = NULL;
            t->rendez_blocked_on = NULL;
            timerwait_unlink(t);
            t->state             = THREAD_RUNNING;
            ret = TSLEEP_INTR;
            break;
        }

        // Drop all three locks (the IRQ mask stays via the wait_lock irqsave).
        // sched() sees SLEEPING and keeps t out of the run tree; wakeup() / the
        // tick scan / the cascade transition t back, eagerly unlinking it from
        // the timer-wait list.
        spin_unlock(&r->lock);
        spin_unlock(&g_timerwait.lock);
        spin_unlock(&t->wait_lock);

        sched();

        // Resumed: clear rendez_blocked_on under wait_lock (the owner clears,
        // not wake_rendez_waiter; t is already unlinked from the timer-wait
        // list by whoever woke it), then re-acquire in order and re-evaluate.
        spin_lock(&t->wait_lock);
        t->rendez_blocked_on = NULL;
        spin_lock(&g_timerwait.lock);
        spin_lock(&r->lock);

        // Woken by the cascade? Return INTR (prompt path).
        if (t->proc &&
            __atomic_load_n(&t->proc->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) {
            ret = TSLEEP_INTR;
            break;
        }
    }

    spin_unlock(&r->lock);
    spin_unlock(&g_timerwait.lock);
    spin_unlock_irqrestore(&t->wait_lock, s);
    return ret;
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

// P5-tsleep: wake every thread whose deadline has passed. Called from
// sched_tick() on every timer fire (IRQ context, IRQs already masked).
// Maps to specs/tsleep.tla Timeout.
//
// Threads are woken ONE AT A TIME: each iteration acquires
// g_timerwait.lock, finds one expired sleeper, unlinks + wakes it under
// g_timerwait.lock + r->lock, then RELEASES g_timerwait.lock before the
// next. Each thread is still unlinked + woken atomically — the two locks
// are held continuously across its wake, so no wakeup can interleave a
// given wake and there is no re-enqueue (ABA) window — but the global
// lock is no longer held across a whole herd of wakes, so a burst of
// simultaneous timeouts cannot stall other CPUs' ticks behind one long
// hold (P5-tsleep audit F6). `now` is sampled once, so the set of
// threads THIS invocation wakes is fixed and the loop terminates — each
// iteration unlinks exactly one. (A thread whose deadline lapses later,
// or that is skipped below for on_cpu, is caught by a later tick's
// fresh `now`.)
//
// The rescan-from-head is O(n^2) in the per-tick herd size — bounded and
// cheap for the cold deadlined-wait path; per-CPU sharding of the list
// would make it O(n), a future optimization, not a correctness need.
//
// A thread that is expired but still mid-context-switch (on_cpu set) is
// not selected — left linked, woken a later tick — so the wake never
// spins in the timer IRQ handler.
static void timerwait_tick(void) {
    u64 now = timer_get_counter();

    for (;;) {
        spin_lock(&g_timerwait.lock);

        struct Thread *t = NULL;
        for (struct Thread *p = g_timerwait.head; p; p = p->timerwait_next) {
            if (now >= p->sleep_deadline &&
                !__atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE)) {
                t = p;
                break;
            }
        }
        if (!t) {
            spin_unlock(&g_timerwait.lock);
            break;
        }

        // Unlink the selected thread unconditionally — even on the
        // (impossible, since g_timerwait.lock is held continuously from
        // the find through the wake) re-check miss — so the rescan from
        // head cannot re-select it and spin. r is non-NULL for any
        // listed thread: tsleep sets rendez_blocked_on and links it in
        // one g_timerwait.lock + r->lock section.
        struct Rendez *r = t->rendez_blocked_on;
        if (r) {
            spin_lock(&r->lock);
            timerwait_unlink(t);
            if (t->state == THREAD_SLEEPING && r->waiter == t) {
                wake_rendez_waiter(r, t, true);
            }
            spin_unlock(&r->lock);
        } else {
            timerwait_unlink(t);
        }

        spin_unlock(&g_timerwait.lock);
    }
}

void sched_tick(void) {
    // P2-Cd: per-CPU need_resched + this CPU's sched state.
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) return;
    if (!g_cpu_sched[cpu].initialized) return;

    // P5-tsleep: expire deadlined tsleep() waiters. Independent of the
    // current thread's slice accounting below — runs on every CPU's
    // tick regardless of what current_thread() is doing.
    timerwait_tick();

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

    // P5-tsleep: g_timerwait.lock is the OUTER lock (order g_timerwait
    // -> r->lock). wakeup takes it because it must unlink a deadlined
    // tsleep() sleeper from the timer-wait list, and cannot tell whether
    // the waiter is one until it holds r->lock — by which point taking
    // g_timerwait.lock would invert the order. It is released the moment
    // the unlink is done: the on_cpu spin + ready() then run under
    // r->lock alone, off the global lock, so a wakeup racing a context
    // switch cannot stall every CPU's timerwait_tick.
    irq_state_t s = spin_lock_irqsave(&g_timerwait.lock);
    spin_lock(&r->lock);

    struct Thread *t = r->waiter;
    if (!t) {
        // No waiter — caller's cond mutation may still be useful if a
        // future sleeper checks it; nothing to do here. This is the
        // intended idempotency: wakeup is a no-op when no thread is
        // sleeping on r. (After a tsleep timeout the tick scan has
        // already cleared r->waiter, so wakeup correctly no-ops here.)
        spin_unlock(&r->lock);
        spin_unlock_irqrestore(&g_timerwait.lock, s);
        return 0;
    }

    if (t->magic != THREAD_MAGIC)
        extinction("wakeup: corrupted waiter");
    if (t->state != THREAD_SLEEPING)
        extinction("wakeup: waiter is not SLEEPING");
    if (t->rendez_blocked_on != r)
        extinction("wakeup: waiter rendez backref mismatch");

    // The only g_timerwait.lock work: unlink a deadlined tsleep sleeper.
    // Then release the global lock — r->lock alone covers the wake. t
    // cannot re-enter tsleep mid-wake (it is not readied until
    // wake_rendez_waiter's tail, all under r->lock), so dropping the
    // global lock here is race-free, and a concurrent timerwait_tick can
    // no longer see t (it is already off the list).
    if (timerwait_is_linked(t)) timerwait_unlink(t);
    spin_unlock(&g_timerwait.lock);

    // Under r->lock only: clear the waiter, transition to RUNNABLE, ready
    // it. After this step, sleep/tsleep's resume re-checks cond (which
    // the caller set TRUE before calling wakeup) and exits the loop.
    wake_rendez_waiter(r, t, false);

    spin_unlock_irqrestore(&r->lock, s);
    return 1;
}
