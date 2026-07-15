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
#include <thylacine/notes.h>    // LS-5c: thread_die_pending (the widened #811 predicate)
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <atomic_lse.h>   // t_atomic_fetch_add_relaxed_u32 (W1.5 LSE-patchable)

#include "../arch/arm64/asid.h"     // RW-1 B-F1: asid_resolve (context-switch ASID pre-hook).
#include "../arch/arm64/hwdebug.h"  // 8a-2b: hwdebug_switch_in (context-switch HW-breakpoint install).
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

    // P3-G: WFI signaling for cross-CPU wake. Means "this CPU's current thread
    // is its idle" (ARCH 8.4.5): set TRUE by THIS CPU's idle loop before its
    // `wfi`, cleared after WFI exits, AND maintained by sched() = (next==idle) at
    // every switch (the F7 accuracy fix -- a CPU running stolen work no longer
    // looks idle). Read by PEER CPUs in `sched_notify_idle_peer()` to identify a
    // wake target.
    //
    // Race semantics: peer's read can stall behind this CPU's wfi-exit clear,
    // causing a spurious IPI to a CPU that just woke up. The IPI handler is a
    // no-op + count, so the spurious IPI is harmless. `volatile` is sufficient --
    // single-store/single-load with no derived data; relaxed atomic semantics.
    //
    // SMP redesign: cpu0 now has a real in-tree idle (bootcpu_idle) running the
    // same idle loop as the secondaries, so cpu0's flag toggles like any other
    // CPU's (the old "boot CPU stays FALSE forever" no longer holds). And since
    // #868 cpu0 ATTACHES IPI_RESCHED (smp_boot_cpu_ipi_init), so a peer's notify
    // wakes cpu0's idle immediately like any secondary -- cpu0 is a full SGI peer
    // (TI-2: cpu0's idle now arms a 100 ms ONE-SHOT backstop, not the 1 kHz
    // periodic; #868's reliable IPI is the primary wake, the one-shot self-heals
    // a hypothetical dropped IPI). Modeled in `scheduler.tla` by `EnterWFI` +
    // `IPI_Deliver`-clears-`wfi[dst]`.
    volatile bool  idle_in_wfi;

    // HMP foundation (#864, ARCH §8.4.4): this CPU's normalized capacity class
    // in [0, SCHED_CAPACITY_SCALE], parsed from the DTB's capacity-dmips-mhz by
    // sched_capacity_init (boot CPU, once). SCHED_CAPACITY_SCALE on a uniform
    // topology (QEMU virt / RPi) -- where the placement policy is inert. Set
    // once at boot, never mutated thereafter (read-only data after init), so a
    // plain field with the boot-time publish barrier suffices.
    u32            capacity;
};

static struct CpuSched g_cpu_sched[DTB_MAX_CPUS];

// HMP foundation (#864, ARCH §8.4.4): true iff the DTB declared a
// heterogeneous topology (some CPU's capacity-dmips-mhz differs). Set once by
// sched_capacity_init on the boot CPU; read by select_target_cpu. FALSE on
// QEMU virt / RPi (uniform), so the capacity-aware placement is inert and
// ready() preserves the pre-#864 "enqueue on the waking CPU" behavior exactly.
//
// #866 F3: this flag is the RELEASE/ACQUIRE publish point for the per-CPU
// `capacity` stamps. sched_capacity_init writes every capacity (plain) then
// RELEASE-stores this flag; select_target_cpu ACQUIRE-loads it FIRST and only
// touches `capacity` when it reads true -- so a cross-CPU reader that sees a
// heterogeneous topology is guaranteed to see the finished capacity stores
// (never a torn/stale capacity). Dormant on the uniform v1.0 targets (the
// acquire short-circuits to prev before any capacity read), but correct for
// the day a real heterogeneous DTB activates it.
static bool g_sched_hetero;

// Placement misfit threshold (percent of a CPU's capacity). A task whose util
// exceeds this fraction of its prev CPU's capacity is a "misfit" and is biased
// toward a higher-capacity CPU. A v1.0 placeholder -- the empirical misfit
// tuning is deferred to real heterogeneous hardware (ARCH §8.4.4). Only
// consulted on a declared-heterogeneous topology (g_sched_hetero), so inert at
// v1.0 on the uniform targets.
#define SCHED_MISFIT_PCT  80u

// HMP foundation (#864): util EWMA shift. util ramps toward SCHED_CAPACITY_-
// SCALE while a thread runs (accrue, sched_tick) and decays toward 0 while it
// is blocked (sched() SLEEPING path), each step moving 1/2^SHIFT of the gap.
// A simple, cheap estimator; the tuned PELT geometric series (y^32 window) is
// the deferred EAS work (ARCH §8.4.4). Read by placement only when hetero.
#define SCHED_UTIL_SHIFT  3u

// Forward declaration: sched_in_cpu_tree (HMP section) reads in_run_tree,
// which is defined further down with the run-tree helpers.
static bool in_run_tree(struct CpuSched *cs, struct Thread *t);
static bool cpu_has_surplus_for_kick(struct CpuSched *cs);

// P2-Cd: per-CPU preemption signal. Each CPU's timer IRQ sets its own slot
// (sched_tick); preempt_check_irq on the same CPU reads + clears it. #866 F1
// adds ONE cross-CPU producer: ready_on's cross-CPU placement sets the TARGET
// CPU's slot so a busy target reschedules and considers the just-placed thread
// at its next preempt_check_irq (otherwise the placed misfit thread waits up to
// a full slice -- the I-17/I-8 leak the audit found). The target is the sole
// consumer. Accesses are RELAXED atomics: the flag is a hint, and the actual
// reschedule is driven by an IRQ (timer or the paired IPI), which carries its
// own barriers; a late/lost observation only defers to the next tick, never a
// correctness loss. Volatile alone would be a data race now that a peer writes.
static u8 g_need_resched[DTB_MAX_CPUS];

// #360: the per-THREAD plain-spinlock hold count bodies (spinlock.h has the
// discipline + declarations; thread.h has the per-thread-over-per-CPU
// rationale). Out-of-line because spinlock.h cannot see struct Thread
// (thread.h includes spinlock.h for wait_lock). Pre-thread boot (TPIDR_EL1
// not yet parked) skips counting -- that code runs single-CPU with IRQs
// masked, so it needs no gate. The __builtin_return_address(0) here is the
// caller of the inlined spin_lock/spin_unlock -- the lock site.
//
// PERMANENT #360 diagnostics (audit F4: both caught real bugs during
// bring-up -- the first-cut per-CPU tear AND a counted release of the raw
// acquire -- and the underflow check is the ONLY detector of the counted/raw
// mismatch class): a per-CPU outer-acquire breadcrumb (recorded on this
// thread's 0->1 transition; racy only in windows that don't matter for a
// diagnostic) printed by sched()'s assert, and an underflow extinction
// naming an unbalanced release at its own site. Cost: one predictable
// branch per unlock + one store per OUTERMOST acquire.
static volatile u64 g_spin_outer_acquire[DTB_MAX_CPUS];

void spin_preempt_inc(void) {
    struct Thread *t = current_thread();
    if (!t || t->magic != THREAD_MAGIC) return;
    if (t->preempt_count++ == 0u) {
        unsigned cpu = smp_cpu_idx_self();
        if (cpu < DTB_MAX_CPUS)
            g_spin_outer_acquire[cpu] =
                (u64)(uintptr_t)__builtin_return_address(0);
    }
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}

void spin_preempt_dec(void) {
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    struct Thread *t = current_thread();
    if (!t || t->magic != THREAD_MAGIC) return;
    if (t->preempt_count == 0u)   // #360: unbalanced release (see above)
        extinction_with_addr(
            "spin_unlock without a counted lock (preempt underflow)",
            (uintptr_t)__builtin_return_address(0));
    t->preempt_count--;
}

// #361 (audit-360 F2): the extinction tail of the EL0-return leak detector.
// The fast-path check (t->preempt_count != 0) is inlined at
// el0_return_die_check (kernel/proc.c) -- this out-of-line body exists only
// to read the sched-private outer-acquire breadcrumb on the never-taken
// path, keeping the per-EL0-return cost at one load + a predictable branch.
void sched_report_el0_leak(void) {
    unsigned cpu = smp_cpu_idx_self();
    extinction_with_addr(
        "counted spinlock leaked to EL0 return (acquire/release mismatch)",
        cpu < DTB_MAX_CPUS ? (uintptr_t)g_spin_outer_acquire[cpu] : 0);
}

static inline void need_resched_set(unsigned cpu) {
    if (cpu < DTB_MAX_CPUS)
        __atomic_store_n(&g_need_resched[cpu], 1u, __ATOMIC_RELAXED);
}
static inline void need_resched_clear(unsigned cpu) {
    if (cpu < DTB_MAX_CPUS)
        __atomic_store_n(&g_need_resched[cpu], 0u, __ATOMIC_RELAXED);
}
static inline bool need_resched_pending(unsigned cpu) {
    return cpu < DTB_MAX_CPUS &&
           __atomic_load_n(&g_need_resched[cpu], __ATOMIC_RELAXED) != 0u;
}

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

// ============================================================================
// HMP foundation (#864, ARCH §8.4.4): per-CPU capacity + capacity-aware
// placement. The placement POLICY is separated from the enqueue MECHANISM
// (ready_on, below) per ARCH §8.4.3. The two PURE functions
// (sched_capacity_normalize + sched_place_by_capacity) hold the load-bearing
// logic and are unit-tested against a synthetic asymmetric DTB; the safety of
// ANY placement is the composition result proved by specs/sched_alpha.tla
// (Place picks the target non-deterministically). The EMPIRICAL EAS tuning
// (PELT decay constants, energy model, schedutil/DVFS, misfit thresholds) is
// deferred to real heterogeneous hardware -- unverifiable on QEMU/HVF (the
// "verification boundary", ARCH §8.4.4). On the uniform v1.0 targets this
// whole layer is inert: g_sched_hetero is false, so select_target_cpu returns
// the prev CPU and ready() behaves exactly as it did before #864.
// ============================================================================

bool sched_capacity_normalize(const u32 *raw_dmips, unsigned n, u32 *out_caps) {
    if (!raw_dmips || !out_caps || n == 0) return false;

    // Max declared raw value across the present cpus.
    u32 max_raw = 0;
    for (unsigned i = 0; i < n; i++)
        if (raw_dmips[i] > max_raw) max_raw = raw_dmips[i];

    if (max_raw == 0) {
        // No CPU declared capacity-dmips-mhz -- the QEMU-virt / homogeneous
        // case. Every CPU is the default full capacity; not heterogeneous.
        for (unsigned i = 0; i < n; i++) out_caps[i] = SCHED_CAPACITY_SCALE;
        return false;
    }

    // Normalize so the most-capable core maps to SCHED_CAPACITY_SCALE. A cpu
    // that did NOT declare the property (raw 0) on a board where others did is
    // assumed full capacity (never wrongly demoted). u64 math: raw <= ~10^6,
    // * 1024 fits comfortably.
    for (unsigned i = 0; i < n; i++) {
        out_caps[i] = raw_dmips[i]
            ? (u32)(((u64)raw_dmips[i] * SCHED_CAPACITY_SCALE) / max_raw)
            : SCHED_CAPACITY_SCALE;
    }

    // Heterogeneous iff any normalized capacity differs from another.
    for (unsigned i = 1; i < n; i++)
        if (out_caps[i] != out_caps[0]) return true;
    return false;
}

unsigned sched_place_by_capacity(u32 util, unsigned prev_cpu,
                                 const u32 *caps, unsigned n, bool hetero) {
    // Homogeneous topology: the placement is the identity -- keep the prev/
    // waking CPU (the pre-#864 behavior). This is the v1.0 path.
    if (!hetero || !caps || prev_cpu >= n) return prev_cpu;

    // "Fits comfortably" on the prev CPU: util is at or below the misfit
    // fraction of its capacity. Keep it where it is (no needless migration).
    u32 prev_cap = caps[prev_cpu];
    if ((u64)util * 100u <= (u64)prev_cap * SCHED_MISFIT_PCT)
        return prev_cpu;

    // Misfit: route to the highest-capacity CPU. If prev already IS the
    // highest, the scan leaves best == prev_cpu (a heavy task on the biggest
    // core stays put).
    unsigned best = prev_cpu;
    u32 best_cap = prev_cap;
    for (unsigned i = 0; i < n; i++) {
        if (caps[i] > best_cap) { best = i; best_cap = caps[i]; }
    }
    return best;
}

u32 sched_cpu_capacity(unsigned cpu) {
    if (cpu >= DTB_MAX_CPUS) return SCHED_CAPACITY_SCALE;
    u32 c = g_cpu_sched[cpu].capacity;
    return c ? c : SCHED_CAPACITY_SCALE;
}

bool sched_topology_hetero(void) {
    return __atomic_load_n(&g_sched_hetero, __ATOMIC_ACQUIRE);
}

void sched_capacity_init(void) {
    unsigned n = dtb_cpu_count();
    if (n == 0) n = 1;
    if (n > DTB_MAX_CPUS) n = DTB_MAX_CPUS;

    u32 raw[DTB_MAX_CPUS];
    for (unsigned i = 0; i < n; i++) {
        u32 c = 0;
        raw[i] = dtb_cpu_capacity(i, &c) ? c : 0u;   // 0 == not declared
    }

    u32 caps[DTB_MAX_CPUS];
    bool hetero = sched_capacity_normalize(raw, n, caps);

    // Stamp every slot: parsed value for 0..n, full capacity for any slot
    // beyond the DTB-reported count (defensive default).
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++)
        g_cpu_sched[i].capacity = (i < n) ? caps[i] : SCHED_CAPACITY_SCALE;
    // #866 F3: RELEASE so the capacity stamps above are visible to any CPU that
    // ACQUIRE-loads g_sched_hetero==true (the publish point; see the decl).
    __atomic_store_n(&g_sched_hetero, hetero, __ATOMIC_RELEASE);
}

// HMP foundation (#864): per-task util accounting. Accrue while RUNNING (the
// task is consuming CPU), decay while blocked. A simple EWMA stepping
// 1/2^SCHED_UTIL_SHIFT of the gap each call; the tuned PELT series is deferred
// (ARCH §8.4.4). Inert on uniform topology -- the value is maintained but
// select_target_cpu ignores it when !g_sched_hetero.
static void sched_util_accrue(struct Thread *t) {
    if (!t) return;
    u32 u = t->util;
    // Saturate at the scale. Also guards the (SCALE - u) subtraction below from
    // u32 underflow should util ever be set >= SCALE out of band (the live
    // accrue/decay dynamics keep it < SCALE, but this makes the estimator
    // robust to any future direct setter).
    if (u >= SCHED_CAPACITY_SCALE) { t->util = SCHED_CAPACITY_SCALE; return; }
    t->util = u + ((SCHED_CAPACITY_SCALE - u) >> SCHED_UTIL_SHIFT);
}

static void sched_util_decay(struct Thread *t) {
    if (!t) return;
    t->util -= (t->util >> SCHED_UTIL_SHIFT);
}

// g_sched_notify_enabled: the production gate (set once at boot between
// test_run_all and joey_run; the full semantics + the setter are at
// sched_set_notify_enabled below). Declared HERE -- ahead of its original spot
// -- so the TI-4b push-placement it gates can read it. volatile + relaxed: set
// once, observed by every CPU.
static volatile bool g_sched_notify_enabled;

// Push-complete placement (TI-4b): rotate placement across distinct idle CPUs so
// a burst from one busy producer (the boot spawn-storm) spreads instead of
// piling on the waking CPU's own tree + relying on the single best-effort kick.
// Distinct from g_try_steal_rotate (the pull side); a relaxed counter suffices
// (the spread is approximate, not strictly fair).
static volatile u32 g_idle_place_rotate;

// Pick an IDLE peer (one that announced idle_in_wfi) to PLACE a waking thread on,
// so it runs there NOW -- ready_on enqueues on the peer + sched_notify_cpu IPIs
// it -- instead of the enqueue-locally + kick-ONE-peer-to-PULL path the retired
// 1 kHz re-poll backstopped (the TI-3 boot regression). Returns prev_cpu when
// the waker is itself idle (it runs the work on its own IRQ-return -- no needless
// migration) OR no peer is idle (the saturated regime: keep local; the busy-side
// overload kick (TI-4c) then rebalances). The idle_in_wfi read is the same
// volatile hint sched_notify_idle_peer uses; a stale TRUE only costs a harmless
// IPI to a just-woken CPU. The place+IPI no-lost-wake is sched_tickless.tla's
// register-then-observe (idle_in_wfi set BEFORE the peer's arm + WFI); safety
// under an arbitrary target is sched_alpha.tla's non-deterministic Place.
static unsigned select_idle_target(unsigned prev_cpu) {
    if (prev_cpu < DTB_MAX_CPUS && g_cpu_sched[prev_cpu].idle_in_wfi)
        return prev_cpu;
    if (smp_cpu_online_count() <= 1) return prev_cpu;
    u32 base = t_atomic_fetch_add_relaxed_u32((u32 *)&g_idle_place_rotate, 1u);
    for (unsigned k = 0; k < DTB_MAX_CPUS; k++) {
        unsigned i = (unsigned)((base + k) % DTB_MAX_CPUS);
        if (i == prev_cpu) continue;
        struct CpuSched *peer = &g_cpu_sched[i];
        if (!peer->initialized) continue;
        if (!peer->idle_in_wfi) continue;
        return i;
    }
    return prev_cpu;
}

// Affinity-ready predicate (the priority/affinity pluggable seam, user-ratified
// TI-4e). True iff thread `t` is permitted to run on `cpu`. v1.0 has no
// per-thread affinity mask, so every non-cpu_pinned thread may run anywhere and
// this is unconditionally true -- a trivially-true gate today that becomes load-
// bearing the day a SYS_SCHED_SETATTR affinity mask lands (then: return
// (t->affinity_mask >> cpu) & 1u). Consulted at the two CPU-binding decisions --
// placement (select_target_cpu) and steal (try_steal's victim pick) -- so the
// balancer never binds a thread to a forbidden CPU; the future mask plugs into
// THIS one function. cpu_pinned (the idle/kthread hard pin) stays a separate,
// stronger predicate. Inert today (always true): no behavior change, only the
// seam, so the work-conservation redesign does not foreclose affinity.
static inline bool thread_may_run_on(const struct Thread *t, unsigned cpu) {
    (void)t; (void)cpu;
    return true;
}

unsigned select_target_cpu(struct Thread *t, unsigned prev_cpu) {
    if (!t) return prev_cpu;
    // CPU-pinned threads (every per-CPU idle + kthread) NEVER migrate -- their
    // home CPU is fixed (ARCH §8.4.2 / sched_alpha.tla IdleStaysHome). This is
    // the placement-side companion to try_steal's cpu_pinned skip.
    if (t->cpu_pinned) return prev_cpu;
    // Push-complete placement (TI-4b): in production, prefer an IDLE CPU for a
    // waking thread -- place it where it runs NOW + IPI that CPU, instead of
    // enqueuing locally + best-effort-kicking one peer to PULL (the pull-with-
    // one-kick the retired 1 kHz re-poll backstopped -- the TI-3 boot
    // regression). Gated on g_sched_notify_enabled so the in-kernel test phase
    // stays UP-like (no cross-CPU placement; the ready_on cross-CPU enqueue test
    // relies on the placed thread sitting still until removed). Composes with
    // HMP: on a hetero topology the capacity placement below still applies when
    // no peer is idle.
    if (g_sched_notify_enabled) {
        unsigned idle = select_idle_target(prev_cpu);
        if (idle != prev_cpu && thread_may_run_on(t, idle)) return idle;
    }
    // Uniform topology (v1.0 QEMU virt / RPi): identity placement -- keep the
    // waking CPU. Byte-identical to the pre-#864 ready() behavior. #866 F3:
    // ACQUIRE pairs with sched_capacity_init's RELEASE so a true result implies
    // the per-CPU capacity stamps below are visible (no torn/stale read).
    if (!__atomic_load_n(&g_sched_hetero, __ATOMIC_ACQUIRE)) return prev_cpu;

    unsigned n = smp_cpu_count();
    if (n > DTB_MAX_CPUS) n = DTB_MAX_CPUS;
    u32 caps[DTB_MAX_CPUS];
    for (unsigned i = 0; i < n; i++)
        caps[i] = g_cpu_sched[i].initialized ? g_cpu_sched[i].capacity : 0u;

    unsigned tgt = sched_place_by_capacity(t->util, prev_cpu, caps, n, true);
    // Safety: never place on an out-of-range / uninitialized CPU (an
    // uninitialized slot had caps[i]==0 so could only be chosen if prev was
    // also 0 -- defensive belt regardless). Fall back to the prev CPU.
    if (tgt >= DTB_MAX_CPUS || !g_cpu_sched[tgt].initialized) return prev_cpu;
    // Affinity-ready seam: never place on a CPU the thread's (future) mask
    // forbids; inert today (thread_may_run_on is unconditionally true).
    if (!thread_may_run_on(t, tgt)) return prev_cpu;
    return tgt;
}

bool sched_in_cpu_tree(unsigned cpu, struct Thread *t) {
    if (cpu >= DTB_MAX_CPUS || !t) return false;
    struct CpuSched *cs = &g_cpu_sched[cpu];
    if (!cs->initialized) return false;
    irq_state_t s = spin_lock_irqsave(&cs->lock);
    bool present = in_run_tree(cs, t);
    spin_unlock_irqrestore(&cs->lock, s);
    return present;
}

#ifdef KERNEL_TESTS
// Test-support accessors (gated by KERNEL_TESTS, #61): the unit suite reads
// need_resched_pending() to observe the same-CPU wake-preempt path (RW-11
// SA-1b) and clears it to establish a clean baseline. Production code never
// calls either -- preemption is driven by sched()'s entry need_resched_clear
// (R5-H F93) and preempt_check_irq -- so they compile out of the production
// shape rather than sit dead in the symbol table.
bool sched_need_resched_pending(unsigned cpu) {
    return need_resched_pending(cpu);
}

void sched_clear_need_resched_for_test(unsigned cpu) {
    need_resched_clear(cpu);
}

// #360: arm this CPU's preempt flag from the test suite -- the gate test
// (sched.preempt_gate_defers_while_locked) proves preempt_check_irq DEFERS
// (does not consume) a pending resched while a plain spinlock is held.
void sched_set_need_resched_for_test(unsigned cpu) {
    need_resched_set(cpu);
}
#endif /* KERNEL_TESTS */

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

// SMP redesign (ARCH 8.4.2): install cpu0's idle thread. cpu0's idle is an
// ordinary in-tree pinned thread (thread_create_bootcpu_idle), retiring the old
// off-tree real-kstack g_bootcpu_idle + its deadlock-path dispatch (the #860
// root cause). Records it as cpu0's idle (so the idle_in_wfi `next==idle` logic
// names it -- overriding the kthread placeholder sched_init(0) set) and ready()s
// it into cpu0's run_tree[IDLE], whence ordinary pick_next dispatches it.
void sched_install_bootcpu_idle(struct Thread *t) {
    if (!t)                         extinction("sched_install_bootcpu_idle(NULL)");
    if (t->magic != THREAD_MAGIC)   extinction("sched_install_bootcpu_idle: corrupted Thread");
    if (t->band != SCHED_BAND_IDLE) extinction("sched_install_bootcpu_idle: thread must be SCHED_BAND_IDLE");
    if (!t->cpu_pinned)             extinction("sched_install_bootcpu_idle: idle must be cpu_pinned");
    if (smp_cpu_idx_self() != 0)    extinction("sched_install_bootcpu_idle: must run on cpu0");
    struct CpuSched *cs = &g_cpu_sched[0];
    if (!cs->initialized)           extinction("sched_install_bootcpu_idle before sched_init(0)");
    cs->idle = t;
    ready(t);
}

// SMP redesign (ARCH 8.4.2): cpu0's idle loop. Body mirrors per_cpu_main's tail
// (kernel/smp.c) for secondaries. The mask -> set-flag -> sched -> wfi ordering
// is the R7 F128 close: a peer notifier observes idle_in_wfi=TRUE before the wfi
// commits, so a peer that ready()s new work between (flag set) and (wfi enters)
// sends an IPI that wfi sees pending and exits on. The complementary half of the
// idle_in_wfi accuracy fix (F7) lives in sched(): the flag is cleared to
// (next==idle) at every switch, so a CPU running stolen work no longer looks
// idle to peers.
//
// Reached via thread_trampoline -> blr x21 with x21 = bootcpu_idle_main (set by
// thread_create_bootcpu_idle). The trampoline already calls
// sched_finish_task_switch + msr daifclr,#2 before reaching here, so PSTATE.I is
// unmasked on entry; the first spin_lock_irqsave(NULL) re-masks for the body.
// Dispatched by ordinary pick_next from cpu0's run_tree[IDLE] (NO deadlock path).
__attribute__((noreturn))
void bootcpu_idle_main(void) {
    // Tickless idle (NO_HZ_IDLE; TI-2): the boot CPU's timer + PPI are armed
    // before this loop runs (timer_init + gic_enable_irq in boot_main), so it
    // is always tickless-eligible -- sched_idle_park(true) arms a one-shot to
    // the nearest deadline-or-backstop instead of holding the 1 kHz periodic
    // tick. The mask -> idle_in_wfi -> sched -> arm -> wfi -> restore body lives
    // in sched_idle_park (shared with per_cpu_main).
    for (;;) sched_idle_park(true);
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
            // ARCH 8.4.5 steal-invariant: cs->lock is held across the whole
            // multi-step switch, so a tree reached here holds only RUNNABLE,
            // !on_cpu threads (RunqOnCpuSafe). Fail loud if a future change ever
            // shortens that hold and leaves an on_cpu/non-RUNNABLE thread linked
            // (a silent double-run / half-saved-ctx resume otherwise).
            ASSERT_OR_DIE(t->state == THREAD_RUNNABLE &&
                          !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE),
                          "pick_next: victim not RUNNABLE-and-off-cpu (run-tree invariant)");
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
// (The definition lives up in the push-placement section (TI-4b) so
// select_target_cpu can gate its idle-preference on it.)

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
// tree leaves an idle peer waiting in WFI for its next 1 ms tick (or, during
// the pre-production quiescent phase — before smp_enable_secondary_preemption
// arms the secondaries' banked timers — indefinitely; only IPI wakes WFI
// there). The IPI is the prompt wake on both. Maps to scheduler.tla
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

// HMP foundation (#864, ARCH §8.4.3): targeted cross-CPU placement notify --
// the PROMPTNESS half. The CORRECTNESS half is ready_on's `need_resched_set`
// (#866 F1): that is what makes the target actually reschedule + consider the
// just-placed thread at its next preempt_check_irq. This IPI just makes it
// PROMPT: it traps a target running in EL1/EL0 into the kernel immediately
// (Linux's kick_process) rather than waiting up to a slice for the next timer
// tick; for an idle target it exits WFI. The sibling sched_notify_idle_peer
// instead wakes ANY idle peer to steal. Gated by g_sched_notify_enabled so the
// UP-like in-kernel tests stay quiescent (a parked secondary is not self-woken;
// the ready_on cross-CPU enqueue test relies on the placed thread sitting still
// until removed -- note need_resched_set is NOT gated, so the test still sees
// it). Never targets self.
static void sched_notify_cpu(unsigned target) {
    if (!g_sched_notify_enabled) return;
    if (target >= DTB_MAX_CPUS) return;
    if (target == smp_cpu_idx_self()) return;
    if (!g_cpu_sched[target].initialized) return;
    (void)gic_send_ipi(target, IPI_RESCHED);
}

// Wake-preemption policy (RW-11 SA-1b -- the 6 ms slice cliff). PURE so a unit
// test drives it against synthetic bands (the "verification boundary", like
// sched_place_by_capacity). Lower band number = higher priority (sched.h).
// Fixed-priority bands (ARCH 8.3): a strictly-higher-priority band preempts; a
// CPU running its idle yields to ANY real thread; same band is EEVDF-fair (no
// wake-preempt -- a latency-critical thread runs in the INTERACTIVE band, which
// always preempts NORMAL/IDLE). This is what enforces 8.3's "always serves the
// highest-priority band with runnable threads" ON THE WAKE PATH: without it a
// newly-runnable higher-band thread waited up to a full slice for the next
// tick-driven preempt.
bool sched_wake_preempts(u32 woken_band, u32 cur_band, bool cur_is_idle) {
    if (cur_is_idle) return true;
    return woken_band < cur_band;
}

// Should a just-woken thread on THIS CPU preempt the thread currently running
// here? Reads current_thread() + cs->idle, so it MUST be called under cs->lock
// with IRQs masked (the window ready_on holds) where both are stable. `cs` is
// THIS CPU's sched (target_cpu == self at the only call site).
static bool sched_should_preempt_on_wake(struct CpuSched *cs, struct Thread *woken) {
    struct Thread *cur = current_thread();
    if (!cur) return false;                  // pre-thread boot: nothing to preempt
    return sched_wake_preempts(woken->band, cur->band, cur == cs->idle);
}

// HMP foundation (#864, ARCH §8.4.3): the enqueue MECHANISM, separated from the
// placement POLICY (select_target_cpu). Insert RUNNABLE `t` into target_cpu's
// run tree under that CPU's lock. On a uniform topology target_cpu is always
// the caller's own CPU (select_target_cpu returns prev), so this is identical
// to the pre-#864 ready(): enqueue locally + wake an idle peer to steal. A
// cross-CPU target enqueues onto the peer's tree + wakes the target. Safety
// under an arbitrary target is the composition result of specs/sched_alpha.tla
// (Place picks the target CPU non-deterministically). Lock discipline: holds
// exactly ONE CpuSched lock (the target's) and no other -- it never nests, so
// it cannot form a cycle with try_steal (which try_locks peers while holding
// its own) or sched_remove_if_runnable (which takes one CPU lock at a time).
void ready_on(unsigned target_cpu, struct Thread *t) {
    if (!t)                       extinction("ready(NULL)");
    if (t->magic != THREAD_MAGIC) extinction("ready of corrupted Thread");
    if (t->state != THREAD_RUNNABLE)
                                  extinction("ready of non-RUNNABLE Thread");
    if (t->band >= SCHED_BAND_COUNT)
                                  extinction("ready: invalid band");

    // #107/F1: mask IRQs BEFORE reading the per-CPU index. `self` and the
    // `target_cpu == self` placement/preempt decisions below are a TOCTOU on the
    // CPU identity, exactly the sched() #104 bug class: a migration between this
    // read and the run-tree-lock acquire would enqueue `t` onto -- and poke -- the
    // WRONG CPU's slot. Mask-only form (spin_lock_irqsave(NULL)); `s` restored at
    // the unlock below. (Sound even pre-fix because every live caller already masks
    // -- wakeup holds g_timerwait.lock irqsave, the SVC spawners are hardware-
    // masked -- but a future IRQ-enabled kthread caller would reintroduce it;
    // mask-first closes the class rather than rely on an unstated precondition.)
    irq_state_t s = spin_lock_irqsave(NULL);
    unsigned self = smp_cpu_idx_self();
    // #866 F4: a clean extinction (not an OOB g_cpu_sched[] access) if THIS CPU's
    // index is out of range -- the target-fallback below only guards `target_cpu`.
    if (self >= DTB_MAX_CPUS) extinction("ready_on: cpu idx out of range");
    // Safety: fall back to the caller's CPU for an out-of-range or uninitialized
    // target. select_target_cpu already guards this; belt for any other caller.
    if (target_cpu >= DTB_MAX_CPUS || !g_cpu_sched[target_cpu].initialized)
        target_cpu = self;

    // The target CPU's run-tree lock (IRQs already masked above; cross-CPU
    // contention against work-stealing peers / a peer's own sched).
    struct CpuSched *cs = &g_cpu_sched[target_cpu];
    spin_lock(&cs->lock);

    if (in_run_tree(cs, t))       extinction("ready of already-runnable Thread");

    // RW-2 2A-F1 (ARCH §8.2: "reinserted with its ve_t set to the current
    // virtual time"): a thread carries the vd_t from its last yield on whatever
    // CPU it last ran on. Each CPU's vd_counter is an independent clock; a
    // cross-CPU wake (a wakeup() / timer-tick on a CPU other than where t last
    // ran -- select_target_cpu returns the waker's CPU at v1.0) would otherwise
    // enqueue t with a STALE key minted by a foreign clock. A high stale key
    // from a fast clock onto a slow-clock CPU tails t behind every fresh
    // yielder -> starvation bounded only by the inter-CPU counter gap (an I-17
    // latency-bound violation). Clamp to this CPU's clock: t is placed at "now"
    // -- never penalized (a fresh-yielder back-of-queue key) and never unfairly
    // credited (it keeps a lower key only when it already sorts at/before now,
    // preserving the benign same-CPU "brief sleeper wakes near the front"). The
    // steal path already rebases (unconditionally) for the same reason.
    if (t->vd_t > cs->vd_counter) t->vd_t = cs->vd_counter;

    insert_sorted(cs, t);

    // Wake-preemption (RW-11 SA-1b): if the just-enqueued thread outranks the
    // thread currently on THIS CPU, request a reschedule so it is served at the
    // next preempt point (preempt_check_irq at IRQ-return / the EL0-return tail)
    // instead of waiting up to a full slice -- the empirically-pinned 6 ms cliff.
    // Decided HERE, under cs->lock with IRQs masked, where current_thread() +
    // cs->idle are stable. The cross-CPU branch's need_resched_set (below) is the
    // peer-targeted analog (#866 F1); the same-CPU notify (sched_notify_idle_peer)
    // is orthogonal -- it offloads the wakee to an IDLE PEER if one exists (no
    // preemption needed then); this flag covers the no-idle-peer case, the
    // saturated regime the p99.9 budget describes.
    bool preempt_self = (target_cpu == self) &&
                        sched_should_preempt_on_wake(cs, t);
    if (preempt_self) need_resched_set(self);

    spin_unlock_irqrestore(&cs->lock, s);

    // Wake the right CPU, AFTER releasing the lock so its IPI handler doesn't
    // contend on it. Local placement -> wake an idle peer to steal (the pre-
    // #864 path; maps to scheduler.tla NotifyWFIPeer). Cross-CPU placement ->
    // make the target reschedule + consider the placed thread.
    if (target_cpu == self) {
        (void)sched_notify_idle_peer();
    } else {
        // #866 F1: set the target's need_resched so its next preempt_check_irq
        // (timer tick OR the IPI below) actually reschedules and considers the
        // thread we just placed -- WITHOUT this the placed thread waits up to a
        // full slice on a busy target (the I-17/I-8 leak the audit found). This
        // is the CORRECTNESS half + is NOT gated by g_sched_notify_enabled (so a
        // busy target reschedules at its next tick even when the wake-IPI is
        // suppressed during tests). sched_notify_cpu is the PROMPTNESS half: a
        // gated IPI that traps the target in NOW rather than at the next tick.
        need_resched_set(target_cpu);
        sched_notify_cpu(target_cpu);
    }
}

void ready(struct Thread *t) {
    if (!t)                       extinction("ready(NULL)");
    if (t->magic != THREAD_MAGIC) extinction("ready of corrupted Thread");

    // HMP foundation (#864, ARCH §8.4.3): placement POLICY then enqueue
    // MECHANISM. select_target_cpu chooses the run tree; on a uniform topology
    // (v1.0) it returns the caller's own CPU, so ready() == ready_on(self, t)
    // == the pre-#864 "enqueue on the CPU that woke you" behavior, exactly.
    // #107/F1: the smp_cpu_idx_self() here is a stale-tolerant placement HINT, not
    // a lock key -- ready_on re-reads the running CPU under its entry mask and
    // validates `target`, so a migration between this read and ready_on only
    // affects WHICH valid CPU's tree `t` lands on (benign on a uniform topology),
    // never which lock is taken.
    unsigned target = select_target_cpu(t, smp_cpu_idx_self());
    ready_on(target, t);
}

// Realize the INTERACTIVE band (ARCH 8.3) for a latency-critical USER thread.
// A thread that blocks waiting for a device IRQ (kobj_irq_wait) or for console
// input (devcons_read) is a latency-sensitive "terminal app / driver" in 8.3's
// taxonomy, so its wakes should preempt NORMAL work (via sched_wake_preempts).
// The two callers each enforce their OWN trust gate so the realized INTERACTIVE
// set stays narrow: kobj_irq_wait is implicitly CAP_HW_CREATE-gated (reaching it
// requires an IRQ kobj), and devcons_read gates on the trusted console session
// (owner/attached) -- this generic helper only adds the user-thread gate below.
// Sticky + one-way (NORMAL -> INTERACTIVE): set when the thread first takes the
// role; never demoted at v1.0 -- the dynamic boost-on-wake/demote-on-quantum
// classifier (Plan 9 sched) is the v1.x EEVDF lift (I-17).
//
// Gated to USER threads (proc->pgtable_root != 0): a kernel thread -- notably
// the in-kernel test runner, which drives both wait paths synchronously --
// stays NORMAL, so the boost never pollutes kernel scheduling or test
// isolation. Idempotent; ONLY NORMAL is promoted, so a pinned IDLE idle is
// never touched. `t->band` is written while `t` is current (RUNNING, in no run
// tree); the value publishes to a waker's under-cs->lock read via the
// sleep->wake->ready_on(insert_sorted under cs->lock) happens-before edge.
//
// Caveat (ARCH 8.3 "no aging across bands at v1.0"): a CPU-bound INTERACTIVE
// thread starves NORMAL. Bounded in practice -- the realized INTERACTIVE set is
// mostly-blocked + trusted (CAP_HW_CREATE drivers + the console-owner/attached
// session: the shell + login/corvus -- NOT an arbitrary console-stdin reader);
// the general CPU-DoS bound is the per-Proc quota (#65).
void sched_mark_interactive(struct Thread *t) {
    if (!t) return;
    if (t->magic != THREAD_MAGIC) return;
    if (!t->proc || t->proc->pgtable_root == 0) return;   // kernel thread: stays NORMAL
    if (t->band != SCHED_BAND_NORMAL) return;             // idempotent; never touch IDLE
    t->band = SCHED_BAND_INTERACTIVE;
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

// RW-1 B-F1: the context-switch ASID pre-hook. Resolves next's Proc to its
// current-generation rolling ASID (asid_resolve; spec asid.tla FastSwitch /
// SlowSwitch) and composes the TTBR0_EL1 value into next's Context, so the asm
// cpu_switch_context loads a coherent (ASID | pgtable_root) for next's address
// space. kproc / kernel threads (pgtable_root == 0) keep the kernel TTBR0 baked
// at thread create -- the hook is a no-op for them (ASID 0, never the
// allocator). MUST run with IRQs masked on the CPU next is about to run on: the
// run-queue lock is held at the sched() call site (and thread_switch is the
// single-threaded test primitive), so smp_cpu_idx_self() names next's CPU and
// the per-CPU active slot the resolve publishes into is the right one.
void sched_install_asid_ttbr0(struct Thread *next) {
    struct Proc *p = next->proc;
    if (p && p->pgtable_root != 0) {
        u64 asid = asid_resolve(&p->context_id, smp_cpu_idx_self());
        next->ctx.ttbr0 = (asid << ASID_TTBR0_SHIFT) | (u64)p->pgtable_root;
    }
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
    if (lk) spin_unlock_raw(lk);   // #360: the cross-thread handoff release
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
    u32 base = t_atomic_fetch_add_relaxed_u32((u32 *)&g_try_steal_rotate, 1u);

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

        // Pick the highest-priority STEALABLE thread from peer.
        //
        // SMP redesign (ARCH 8.4.2, invariant I-21): never migrate a CPU-pinned
        // thread (cpu_pinned). Pinned == every per-CPU idle (cpu0's bootcpu_idle
        // + each secondary's) + kthread -- all run on a static boot/idle stack
        // that belongs to one specific CPU. Under the uniform-EL1h model
        // exception frames build on the running thread's stack; migrating a
        // boot-stack thread to another CPU would build its frames on a stack its
        // origin CPU still owns -> cross-CPU stack corruption. cpu_pinned is the
        // single clean predicate REPLACING the prior (kstack_base != NULL &&
        // cand != g_bootcpu_idle) gate: the g_bootcpu_idle special case (a real
        // kstack the kstack_base gate did NOT exclude) was the #860 root cause;
        // a pinned in-tree idle is now skipped here by construction.
        struct Thread *stolen = NULL;
        for (unsigned b = 0; b < SCHED_BAND_COUNT && !stolen; b++) {
            // #866 F2: walk PAST pinned threads within the band -- a non-pinned
            // thread queued BEHIND a pinned one (behind kthread, or behind a
            // CPU's in-tree idle in BAND_IDLE) is still stealable. The prior
            // head-only check skipped the WHOLE band when its head was pinned,
            // diverging from sched_alpha.tla's StealCand (ANY non-pinned runq
            // member). Steady-state-unreachable today (kthread's vd_t bump sorts
            // it to the band tail; nothing non-pinned is placed in BAND_IDLE),
            // but the head-only scan would silently strand such a thread the day
            // a path creates one. The vd_t sort is preserved: the FIRST non-
            // pinned thread in the band is the lowest-vd_t one.
            for (struct Thread *cand = peer->run_tree[b]; cand;
                 cand = cand->runnable_next) {
                if (cand->cpu_pinned || !thread_may_run_on(cand, self)) continue;
                // ARCH 8.4.5 steal-invariant: we hold peer->lock, so peer is not
                // mid-switch (its spin_trylock would have failed) and its tree
                // holds only RUNNABLE, !on_cpu threads (RunqOnCpuSafe). Fail loud
                // if that ever breaks rather than steal a half-saved ctx.
                ASSERT_OR_DIE(cand->state == THREAD_RUNNABLE &&
                              !__atomic_load_n(&cand->on_cpu, __ATOMIC_ACQUIRE),
                              "try_steal: victim not RUNNABLE-and-off-cpu (run-tree invariant)");
                stolen = cand;
                unlink(peer, stolen);
                break;
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

// HMP foundation (#864, ARCH §8.4.3): balance() -- the load-rebalancing
// abstraction. v1.0 body is PULL-only work-stealing (try_steal: an idle CPU
// pulls one non-pinned runnable thread from a peer). The abstraction is
// deliberately shaped to host a future capacity-aware PUSH path -- misfit
// migration, pushing a heavy task off a low-capacity CPU onto a high-capacity
// one even when the latter is not idle -- the one HMP mechanism a pull-only
// stealer structurally cannot express. The push ENQUEUE primitive already
// exists: ready_on (cross-CPU enqueue) + sched_notify_cpu (wake the target).
// So adding push is ADDITIVE (a tick-time misfit scan -> ready_on(high_cap_cpu)
// + IPI), not a scheduler rewrite -- the design-for-the-future shape the
// redesign pins (user-directed). Push is deferred to real heterogeneous HW
// (the empirical-EAS verification boundary, §8.4.4); v1.0 is pull-only. Same
// signature + contract as try_steal (the implementation it currently wraps).
static struct Thread *balance_pull(struct CpuSched *cs, bool *contended_out) {
    return try_steal(cs, contended_out);
}

void sched(void) {
    // #104/#107: MASK IRQs BEFORE resolving the per-CPU sched state. `cs` is
    // named by this_cpu_sched() = &g_cpu_sched[smp_cpu_idx_self()] (MPIDR). Read
    // with IRQs ENABLED this is a TOCTOU on the CPU identity: a timer-IRQ preempt
    // between the read and the run-tree-lock acquire can switch this thread out,
    // let a peer steal it, and resume it on ANOTHER CPU still inside this sched()
    // call -- with `cs` left pointing at the ORIGIN CPU's slot. sched() would then
    // acquire the origin CPU's lock while running elsewhere, breaking the per-CPU
    // pending_release_lock handoff and LEAKING that lock (the #104 SMP deadlock:
    // a later sched() on the origin CPU spins forever on the orphaned lock). The
    // bug is exposed under heavy migration churn -- which #60's syscall-return
    // preempt produced (every syscall return), so #104 first masked it by
    // removing that preempt; this is the real root-cause fix. Masking first pins
    // the CPU so smp_cpu_idx_self()/`cs` cannot go stale before cpu_switch_context
    // (after which the resume path re-reads this_cpu_sched() as `cs_now`). `s`
    // (the caller's DAIF) is restored on the resume + early-return paths. This is
    // `spin_lock_irqsave(NULL)` -- the mask-only form -- kept right here so the
    // per-CPU read below sits strictly inside the mask. Masking only I (not
    // A/SError) is intentional + sufficient: the sole migration vector in the
    // window is a taken IRQ (FIQ has no online source; an SError routes to
    // extinction, never a switch-out + steal).
    irq_state_t s = spin_lock_irqsave(NULL);

    // P2-Cd: read THIS CPU's sched state (migration-safe now -- IRQs masked).
    // All run-tree access goes through `cs`.
    struct CpuSched *cs = this_cpu_sched();
    if (!cs->initialized) extinction("sched() before this CPU's sched_init");

    // R5-H F93: clear the per-CPU need_resched flag at sched() entry.
    // preempt_check_irq already clears before calling, so this is a no-op
    // on the IRQ-driven path. For voluntary sched() callers (sleep,
    // exits, explicit yield), this absorbs a stale flag set by sched_tick
    // since the last preempt_check_irq -- preventing a redundant
    // preempt_check_irq -> sched() spin on the next IRQ. Under the entry mask
    // above, smp_cpu_idx_self() here names the same CPU as `cs`.
    need_resched_clear(smp_cpu_idx_self());

    // P2-Bc/Ce: per-CPU run-tree lock (IRQs already masked above). sched()
    // mutates shared state (run tree, current_thread via TPIDR_EL1, the vd_t
    // counter); the lock protects against a re-entrant preempt_check_irq ->
    // sched() AND a peer's try_steal (which try_locks and moves on if held).
    //
    // #360: RAW (uncounted) acquire -- this is the one cross-thread lock
    // handoff in the kernel: prev acquires here, the RESUMING thread (or a
    // fresh thread's trampoline) releases via pending_release_lock, so a
    // per-thread count cannot balance it. Sound without the count: IRQs are
    // masked from the entry mask above through the release, so the hold is
    // non-preemptible by masking. See spinlock.h (spin_lock_raw).
    spin_lock_raw(&cs->lock);

    // #107 loud-fail guard (the durable regression for a timing-only SMP race):
    // with the entry mask above, the per-CPU slot we just locked MUST belong to
    // the running CPU. A mismatch means a future change reintroduced the
    // read-before-mask TOCTOU -- fail loud here instead of silently leaking a
    // foreign CPU's run-queue lock (the #104 deadlock).
    ASSERT_OR_DIE((unsigned)(cs - g_cpu_sched) == smp_cpu_idx_self(),
                  "sched: per-CPU cs mismatches the running CPU (read-before-mask?)");

    struct Thread *prev = current_thread();
    if (!prev) extinction("sched() with no current thread");
    if (prev->magic != THREAD_MAGIC) extinction("sched() with corrupted current");

    // #360: a plain spinlock may never be held across sched() -- a descheduled
    // holder deadlocks every masked spinner behind it (the #359 class; the
    // lock-across-sleep twin of preempt_check_irq's gate). Every legitimate
    // caller holds none here: sleep/tsleep drop their locks first,
    // preempt_check_irq gates on count==0, exits / thread_exit_self / the
    // idle parks / the sqpoll terminal hold none. sched()'s own cs->lock is
    // the RAW-acquired handoff below -- never counted, never trips this.
    if (prev->preempt_count != 0u)   // breadcrumb names the outer acquire
        extinction_with_addr(
            "sched: plain spinlock held across sched() (lock-across-sleep)",
            (uintptr_t)g_spin_outer_acquire[smp_cpu_idx_self()]);

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
        next = balance_pull(cs, &steal_contended);

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
            next = balance_pull(cs, &steal_contended);
        }
    }
    if (!next) {
        if (prev->state != THREAD_RUNNING) {
            // SMP redesign (ARCH 8.4.2): structurally unreachable in steady
            // state. Every CPU has a pinned in-tree idle, so whenever a non-idle
            // thread is current its CPU's idle sits in run_tree[IDLE] (displaced
            // when that thread was picked) -- pick_next above ALWAYS finds at
            // least the idle (sched_alpha.tla IdleAvailable). The old design
            // dispatched the off-tree g_bootcpu_idle here on cpu0 (the #860 root
            // cause: a real-kstack thread that became stealable if leaked into a
            // tree); that special case is retired. The sole remaining way to
            // reach here is the boot window before sched_install_bootcpu_idle
            // readies cpu0's idle, or a secondary mis-init -- both are bugs. Fail
            // loud rather than run nothing.
            extinction("sched: deadlock -- current blocking, no runnable thread and no in-tree idle");
        }
    }
    if (!next) {
        // Yield path with no runnable peer: keep prev running. Refill
        // slice — sched() was called for preempt or explicit yield;
        // either way, prev is the only runnable thread, so give it a
        // fresh quantum.
        prev->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
        // #360: RAW release pairing the RAW acquire above (a counted release
        // of an uncounted acquire underflows the per-thread count -- the
        // first-cut bug this comment memorializes). Then restore DAIF.
        spin_unlock_raw(&cs->lock);
        spin_unlock_irqrestore(NULL, s);
        return;
    }

    if (prev == next) {
        // RW-2 2A-F2: structurally impossible in the redesigned scheduler, so
        // fail loud rather than corrupt the tree. `prev` is `current` -- it is
        // on_cpu and is NOT in any run tree (it was unlinked when it became
        // current). pick_next returns only tree members; try_steal ASSERTs its
        // victim is RUNNABLE && !on_cpu (§8.4.5). Neither can return `prev`. The
        // prior body re-inserted a RUNNING, on_cpu thread into the run tree and
        // dropped the lock -- a latent RunqRunnable / RunqOnCpuSafe violation
        // (a running thread, lock-free, looking stealable) that would extinct
        // the *next* picker on its invariant ASSERT rather than here. Fail at
        // the source.
        extinction("sched: pick_next/try_steal returned the current thread");
    }

    if (prev->state == THREAD_RUNNING) {
        // Yield: advance vd_t past all currently-runnable threads; insert prev at
        // the back of its band's rotation. EVERY thread re-enters the tree here,
        // the per-CPU idles included -- an idle is an ordinary in-tree thread
        // (ARCH 8.4.2; the old g_bootcpu_idle off-tree exception is retired).
        // try_steal skips it via cpu_pinned, so an in-tree idle is never
        // migrated. This restores the "a non-idle thread is current => its CPU's
        // idle is in run_tree[IDLE]" invariant that makes the deadlock path above
        // unreachable.
        prev->vd_t = cs->vd_counter++;
        prev->state = THREAD_RUNNABLE;
        insert_sorted(cs, prev);
    } else {
        // Block (SLEEPING) or exit (EXITING) — prev stays out of the run tree.
        // wakeup()/ready() will re-insert SLEEPING; EXITING never returns and
        // gets reaped at Phase 2 close. HMP foundation (#864): prev stopped
        // consuming CPU, so decay its util estimate (the dequeue-to-blocked
        // hook, ARCH §8.4.4). Inert on uniform topology + for pinned threads
        // (select_target_cpu ignores util there).
        sched_util_decay(prev);
    }

    // Replenish next's slice on RUNNABLE → RUNNING transition. The
    // slice is consumed during the running quantum; sched_tick()
    // decrements it on each timer IRQ.
    next->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
    next->state = THREAD_RUNNING;
    set_current_thread(next);

    // ARCH 8.4.5 idle_in_wfi accuracy (F7): the flag means "this CPU's current
    // thread is its idle." Set it at every switch so a CPU that switched its idle
    // AWAY to real work (next != idle) no longer looks idle to peers'
    // sched_notify_idle_peer (the F7 bug both review prosecutors found), and a CPU
    // that switched real work BACK to its idle (next == idle) is correctly marked
    // idle so the about-to-wfi resume re-declares it. The idle loop additionally
    // sets the flag TRUE before its sched() (covering the born-running first
    // iteration + the stays-idle no-switch case); together they keep the flag
    // accurate without reopening the R7 F128 window. `cs` is this CPU's slot;
    // cs->idle is the real idle (kthread placeholder overridden for cpu0 by
    // sched_install_bootcpu_idle). Set before the switch; after it we may resume
    // on a different CPU, but this set correctly describes THIS CPU's new current.
    cs->idle_in_wfi = (next == cs->idle);

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

    // RW-1 B-F1: install next's rolling-ASID TTBR0 before the asm switch loads
    // it. Runs here -- IRQs masked, cs->lock held, CPU stable -- so the ASID
    // resolve publishes into the active slot of the CPU next will run on.
    sched_install_asid_ttbr0(next);

    // 8a-2b (I-39): install next's Proc HW breakpoints onto THIS CPU (or clear
    // them + MDE if next is not debugged and this CPU had them loaded). Same
    // invariants as the ASID install -- IRQs masked, cs->lock held, CPU stable --
    // so the DBGB*/MDSCR writes take effect on the CPU next runs on, and the
    // per-CPU MDE isolation fires a bp ONLY while next's (debugged) Proc runs.
    hwdebug_switch_in(next);

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
        // RW-2 2A-F3: NULL-guard, symmetric with sched_finish_task_switch (the
        // trampoline resume). The test-only thread_switch primitive bypasses
        // sched() and arms no pending_release_lock; a thread suspended INSIDE
        // sched() that is later resumed via thread_switch reaches here with
        // lk == NULL -> spin_unlock(NULL) would store through address 0.
        if (lk) spin_unlock_raw(lk);   // #360: the cross-thread handoff release
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
    // always-runnable scheduler infrastructure, not pending work: EVERY CPU's
    // idle -- cpu0's bootcpu_idle AND each secondary's -- is an ordinary
    // BAND_IDLE in-tree thread (ARCH 8.4.2; the old off-tree g_bootcpu_idle is
    // retired), so the band filter uniformly excludes them all. Counting the
    // in-tree idles made this report a phantom backlog on an idle multi-CPU
    // system, and made the `sched_runnable_count()==0` quiescence assertions
    // race an idle thread that, under host load, is in-tree (rather than the
    // running thread) at the check instant -- the #857 "smp8 cons.* flake",
    // which was never console_mgr and never a kernel fault, just a benign idle
    // thread miscounted as work. Real runnable work (BAND_INTERACTIVE /
    // BAND_NORMAL) is still counted, so a genuinely stranded work thread is
    // never masked.
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

// TI-4d: is there ANY queued non-idle runnable work anywhere? Reads ONLY each
// band's HEAD pointer (a stable slot in g_cpu_sched[]) and tests it for NULL --
// it never dereferences a Thread, so unlike sched_runnable_count() it cannot
// follow a runnable_next that a peer is concurrently freeing. That makes it
// safe to call lock-free on the hot idle-park path (3M+ times during boot). A
// relaxed load races a concurrent insert/unlink, but the answer is only used as
// a statistic-at-this-instant -- a few-ns-stale yes/no is exactly right for the
// "was work queued when I parked" sample. SCHED_BAND_IDLE excluded (the per-CPU
// idle threads are infrastructure, not pending work -- same rule as the count).
bool sched_has_runnable_work(void) {
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        struct CpuSched *cs = &g_cpu_sched[i];
        if (!cs->initialized) continue;
        for (unsigned b = 0; b < SCHED_BAND_COUNT; b++) {
            if (b == SCHED_BAND_IDLE) continue;
            if (__atomic_load_n(&cs->run_tree[b], __ATOMIC_RELAXED)) return true;
        }
    }
    return false;
}

// TI-4d work-conservation accumulators. Updated only from sched_idle_park (each
// park adds one sample); read by sched_wc_stats. Relaxed atomics -- they are
// pure statistics, no ordering dependency on other state. The tickless_* subset
// counts only parks that went tickless (production); the regression lives there.
static u64 g_wc_park_events;
static u64 g_wc_idle_ns;
static u64 g_wc_starved_events;
static u64 g_wc_starved_ns;
static u64 g_wc_max_starved_ns;
static u64 g_wc_tickless_parks;
static u64 g_wc_tickless_starved_events;
static u64 g_wc_tickless_starved_ns;
static u64 g_wc_tickless_max_starved_ns;
// Wake-source telemetry (TI-4e): a tickless park woken by an IPI (prompt, the
// common case) vs by the one-shot/backstop timer. The split is the wake-path
// health signal -- TI-4e measured 99.85% IPI under the 100 ms deep-park, proving
// the wake path correct (the boot slowdown was HVF deep-park resume LATENCY, not
// a stranding bug); under the 4 ms re-poll the one-shot share rises by design.
static u64 g_wc_tickless_oneshot_wakes;
static u64 g_wc_tickless_ipi_wakes;

// Relaxed atomic monotonic-max: store dt iff it exceeds the current value.
static void wc_max_update(u64 *slot, u64 dt) {
    u64 prev = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (dt > prev &&
           !__atomic_compare_exchange_n(slot, &prev, dt, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        // prev was reloaded by the failed CAS; retry while dt is still larger.
    }
}

void sched_wc_stats(struct sched_wc_stats *out) {
    if (!out) return;
    out->park_events    = __atomic_load_n(&g_wc_park_events, __ATOMIC_RELAXED);
    out->idle_ns        = __atomic_load_n(&g_wc_idle_ns, __ATOMIC_RELAXED);
    out->starved_events = __atomic_load_n(&g_wc_starved_events, __ATOMIC_RELAXED);
    out->starved_ns     = __atomic_load_n(&g_wc_starved_ns, __ATOMIC_RELAXED);
    out->max_starved_ns = __atomic_load_n(&g_wc_max_starved_ns, __ATOMIC_RELAXED);
    out->tickless_parks =
        __atomic_load_n(&g_wc_tickless_parks, __ATOMIC_RELAXED);
    out->tickless_starved_events =
        __atomic_load_n(&g_wc_tickless_starved_events, __ATOMIC_RELAXED);
    out->tickless_starved_ns =
        __atomic_load_n(&g_wc_tickless_starved_ns, __ATOMIC_RELAXED);
    out->tickless_max_starved_ns =
        __atomic_load_n(&g_wc_tickless_max_starved_ns, __ATOMIC_RELAXED);
    out->tickless_oneshot_wakes =
        __atomic_load_n(&g_wc_tickless_oneshot_wakes, __ATOMIC_RELAXED);
    out->tickless_ipi_wakes =
        __atomic_load_n(&g_wc_tickless_ipi_wakes, __ATOMIC_RELAXED);
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

// Tickless idle (NO_HZ_IDLE; docs/TICKLESS-IDLE.md TI-1). The earliest pending
// deadline across all deadlined sleepers -- the absolute CNTVCT counter value
// an idle CPU arms its one-shot to (min'd with the backstop at TI-2). Returns
// 0 iff no thread is in a deadlined tsleep (the idle CPU then arms only the
// backstop). The 0 sentinel is unambiguous: a linked sleeper's sleep_deadline
// is a post-boot CNTVCT timestamp (timer_ns_to_counter of timer_now_ns()+t),
// never 0; the `found` seed makes the min correct regardless.
//
// O(n) min-scan under g_timerwait.lock -- a LEAF acquisition (no nested lock;
// the lock-order's outermost). irqsave because a g_timerwait.lock holder MUST
// have IRQs masked: the timer IRQ's timerwait_tick takes the same lock, so an
// IRQ landing mid-hold on this CPU would self-deadlock. The idle-loop caller
// (TI-2) already runs IRQ-masked, so the save/restore is a confirmed no-op
// there; the irqsave keeps this helper correct from any context. Unlike
// timerwait_tick it does NOT filter on_cpu -- it reads deadlines (wakes
// nothing), and a mid-switch sleeper's near deadline still needs covering. A
// deadline already in the past is returned as-is: the one-shot clamp fires it
// ASAP, which is correct for an overdue sleeper.
u64 timerwait_earliest_deadline(void) {
    u64 earliest = 0;
    bool found = false;
    irq_state_t s = spin_lock_irqsave(&g_timerwait.lock);
    for (struct Thread *p = g_timerwait.head; p; p = p->timerwait_next) {
        if (!found || p->sleep_deadline < earliest) {
            earliest = p->sleep_deadline;
            found = true;
        }
    }
    spin_unlock_irqrestore(&g_timerwait.lock, s);
    return found ? earliest : 0;
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

        // Register-then-observe (I-9 death-wake close, ARCH §8.8.1; widened
        // by LS-5c per §8.8.2): re-check the death predicate UNDER wait_lock
        // -- the same lock both wakers (proc_group_terminate AND
        // proc_interrupt_terminate_wake) take per peer. thread_die_pending =
        // group_exit_msg set, OR a terminate-disposition `interrupt` is
        // pending (the PROC_FLAG_INTR_TERMINATE_PENDING latch, unmasked for
        // this thread). If true: either we registered before the waker's
        // walk (it finds + wakes us), or the flag-set happens-before our
        // wait_lock acquire (so the acquire-load observes it). Either way we
        // must NOT sleep through it: undo the registration and return
        // SLEEP_INTR -- the caller unwinds and the Thread dies at its
        // EL0-return tail (die-check for group exit; the LS-5b dispatch for
        // the interrupt, which re-validates against the live queue). kproc
        // never group-terminates and the latch is never armed on kproc, so
        // kernel threads never take this branch.
        if (thread_die_pending(t)) {
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

        // Woken by a death/terminate waker? Return INTR rather than looping
        // (the next register-then-observe would catch it anyway -- this is
        // the prompt path). Widened to thread_die_pending (LS-5c).
        if (thread_die_pending(t)) {
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

        // Register-then-observe (I-9 death-wake close, ARCH §8.8.1; widened
        // by LS-5c per §8.8.2): identical to sleep() -- thread_die_pending =
        // group-exit death OR a pending terminate-disposition `interrupt`
        // unmasked for this thread. True => undo the FULL registration
        // (rendez + timer-wait) and return TSLEEP_INTR; the caller unwinds +
        // the Thread dies at its EL0-return tail.
        if (thread_die_pending(t)) {
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

        // Woken by a death/terminate waker? Return INTR (prompt path).
        // Widened to thread_die_pending (LS-5c).
        if (thread_die_pending(t)) {
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

// ===========================================================================
// Tickless idle (NO_HZ_IDLE; docs/TICKLESS-IDLE.md TI-2, #299). Defined here,
// after timerwait_tick, so sched_idle_park can call the static timerwait_tick +
// timerwait_earliest_deadline directly. bootcpu_idle_main (above) +
// per_cpu_main (smp.c) reach it via the sched.h declaration.
// ===========================================================================

u64 tickless_target_cnt(u64 now_cnt, u64 earliest_deadline, u64 backstop_cnt) {
    // now_cnt + backstop_cnt cannot overflow on v1.0 targets (a 62.5 MHz CNTVCT
    // has a ~300,000-year horizon before u64 wrap). If it ever did, the wrapped
    // small `backstop` would be passed to timer_arm_oneshot_cnt, whose clamp
    // turns target<=now into MIN -> a benign fire-ASAP wake, never a lost wake.
    u64 backstop = now_cnt + backstop_cnt;
    return (earliest_deadline != 0 && earliest_deadline < backstop)
         ? earliest_deadline : backstop;
}

void sched_idle_park(bool tickless) {
    // Production-gate tickless on cpu0 (TI-4b): cpu0's bootcpu_idle_main always
    // passes tickless=true, but during the in-kernel test phase the work-steal /
    // cross-CPU-handoff re-poll the 1 kHz tick provided is load-bearing
    // (g_sched_notify_enabled is OFF -> no wake IPIs, so a cross-CPU test handoff
    // relies on cpu0's idle re-poll). Going tickless there stalled every handoff
    // to the backstop (the test-phase half of the TI-3 regression). The
    // secondaries already gate on timer_armed; this extends the same "only
    // tickless in production" discipline to cpu0 -- so the test phase is
    // byte-identical to the pre-tickless periodic idle (modulo the #363
    // park-guard below, which runs in all modes: dormant on pre-preempt
    // secondaries, and on cpu0 it only ACCELERATES requeued-work service
    // from <=1ms to immediate).
#ifdef THYLACINE_NO_TICKLESS
    // TI-4e tickful-baseline capture (tools/build.sh --no-tickless): force the
    // old 1 kHz-always idle so the periodic tick stays the work-steal re-poll.
    // The redesign's cpubench numbers are measured against this gold standard
    // (the fast scheduler that predates the #299 tickless trade). Production
    // builds never define this -- the code below is then byte-identical.
    bool go_tickless = false;
    (void)tickless;
#else
    bool go_tickless = tickless && g_sched_notify_enabled;
#endif
    irq_state_t s = spin_lock_irqsave(NULL);
    struct CpuSched *cs = this_cpu_sched();   // idle is cpu_pinned; stable
    sched_set_idle_in_wfi(true);
    sched();
    // #363 (the #33-audit F1): do NOT park over our own queue. sched() picks
    // BEFORE it requeues prev, so a slice-expiry preempt (or a yield) of a
    // thread with an otherwise-empty local queue dispatches THIS idle -- and
    // the preempted thread lands in run_tree[NORMAL] right after the pick.
    // The dispatched idle does not restart its loop; it resumes HERE, past
    // the sched() above, headed for the one-shot arm + WFI. Without this
    // re-check the CPU parks up to TICKLESS_IDLE_BACKSTOP_NS over its own
    // just-requeued RUNNABLE thread (no IPI exists for a local self-requeue;
    // the one-shot deasserted the periodic tick) -- up to ~4 ms lost per
    // 6 ms slice for a solo compute-bound thread, the misattributed source
    // of the TI-4d multi-ms starved-park records. Two relaxed head loads
    // (the #33 yield predicate); loop until our own non-idle bands are
    // empty. A peer's concurrent insert into our tree is NOT this race --
    // a peer that places ONTO us IPIs unconditionally (ready_on's
    // cross-CPU tail: need_resched_set + sched_notify_cpu -- NOT gated on
    // idle_in_wfi; do not "optimize" that gate in), and a peer that
    // places locally reads idle_in_wfi (set above, register-then-observe)
    // and kicks us via sched_notify_idle_peer -- either way the WFI takes
    // the IPI pended (I-9).
    // The deferred park is a stutter on sched_tickless.tla's Park action;
    // NoLostWake / ParkedImpliesRegistered are untouched.
    while (cpu_has_surplus_for_kick(cs))
        sched();
    // TI-4d work-conservation sample: we are committed to parking THIS CPU.
    // The #363 loop above just verified our OWN non-idle tree is empty, so a
    // positive sched_has_runnable_work() here is a PEER's queued backlog this
    // CPU is about to sleep through (a steal/handoff gap). Charge the whole
    // park as starved if so. In the periodic path the next <=1ms tick re-polls
    // and ends a starved park fast; in tickless it can run to the backstop --
    // so a large starved_ns/max is exactly the steal-gap regression's signature.
    bool starved = sched_has_runnable_work();
    u64 park_start = timer_now_ns();
    u64 armed_target = 0;  // TI-4e: one-shot target, kept for the wake-source classify below
    if (go_tickless) {
        // Arm a one-shot to the nearest deadline (or the backstop) instead of
        // leaving the 1 kHz periodic tick armed -- a genuinely-idle CPU then
        // takes no timer IRQs until a real wake (the #299 fix). idle_in_wfi is
        // already TRUE (set above, BEFORE this arm + the WFI) -> register-then-
        // observe: a peer placing work sends IPI_RESCHED which the WFI sees
        // pending, so the work-arrival wake is never lost across the arm
        // (I-9; specs/sched_tickless.tla Register-before-Park).
        u64 now    = timer_get_counter();
        armed_target = tickless_target_cnt(now, timerwait_earliest_deadline(),
                                           timer_ns_to_counter(TICKLESS_IDLE_BACKSTOP_NS));
        timer_arm_oneshot_cnt(armed_target);
    }
    __asm__ __volatile__("wfi" ::: "memory");
    sched_set_idle_in_wfi(false);
    if (go_tickless) {
        // Wake-source telemetry (TI-4e): now2 < armed_target -> woke BEFORE the
        // one-shot = an IPI woke us (the prompt path, the common case); >= -> the
        // one-shot/backstop timer woke us. The split is the wake-path health
        // signal -- TI-4e measured 99.85% IPI under the 100 ms deep-park, proving
        // the wake path correct (the boot slowdown was HVF deep-park resume
        // LATENCY, not a stranding gap), which the 4 ms re-poll then mitigates.
        if (timer_get_counter() >= armed_target)
            __atomic_fetch_add(&g_wc_tickless_oneshot_wakes, 1, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&g_wc_tickless_ipi_wakes, 1, __ATOMIC_RELAXED);
        // Wake-to-running restore, under the same IRQ-masked region so it runs
        // BEFORE any placed work is dispatched. Two jobs:
        //   1. timer_arm_this_cpu() re-arms the periodic tick, so a CPU woken
        //      from tickless idle by an IPI_RESCHED runs the placed thread with
        //      1 kHz slice ticking immediately -- not up to the backstop with no
        //      tick (the I-17 window the one-shot would otherwise leave open).
        //   2. The re-arm DEASSERTS the one-shot's pending timer IRQ (CNTV_TVAL
        //      back > 0), so timerwait_tick must run here explicitly: had the
        //      one-shot fired on a passed deadline, deasserting it would stop the
        //      handler ever running timerwait_tick for it -> the sleeper would
        //      never wake (a busy-spin: re-arm a MIN one-shot on the still-past
        //      deadline, fire, deassert, repeat). Running it here wakes the
        //      sleeper now; an IPI-wake makes it a cheap no-op scan.
        timer_arm_this_cpu();
        timerwait_tick();
    }
    // TI-4d: charge the park. timer_now_ns() is monotonic CNTVCT-derived (a
    // register read, no vmexit), so bracketing the WFI is cheap. The tickless
    // subset is the load-bearing diagnostic (a tickless starved park can run to
    // the 100ms backstop = the regression); the periodic remainder ends at the
    // next <=1ms tick (the correct pre-tickless baseline). idle_ns/park_events
    // are the denominators.
    u64 dt = timer_now_ns() - park_start;
    __atomic_fetch_add(&g_wc_park_events, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_wc_idle_ns, dt, __ATOMIC_RELAXED);
    if (go_tickless)
        __atomic_fetch_add(&g_wc_tickless_parks, 1, __ATOMIC_RELAXED);
    if (starved) {
        __atomic_fetch_add(&g_wc_starved_events, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_wc_starved_ns, dt, __ATOMIC_RELAXED);
        wc_max_update(&g_wc_max_starved_ns, dt);
        if (go_tickless) {
            __atomic_fetch_add(&g_wc_tickless_starved_events, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_wc_tickless_starved_ns, dt, __ATOMIC_RELAXED);
            wc_max_update(&g_wc_tickless_max_starved_ns, dt);
        }
    }
    spin_unlock_irqrestore(NULL, s);
}

// TI-4c: does THIS CPU hold queued work a parked peer could steal? Reads ONLY
// the non-IDLE band HEAD pointers (the sched_has_runnable_work lock-free
// discipline -- never derefs a Thread, so it is safe from the IRQ-context tick
// without the run-tree lock; a racy stale read only costs a benign spurious or
// skipped kick, self-correcting next tick). A non-NULL INTERACTIVE/NORMAL head
// is a RUNNABLE thread queued BEHIND the running current (current is unlinked
// while running) = genuine migratable surplus. SCHED_BAND_IDLE is excluded: it
// holds this CPU's pinned idle (unstealable) and at most a best-effort IDLE-band
// thread (not worth a cross-CPU kick at v1.0; the boot/IPC workload is NORMAL
// band). Band-aware by scanning the real-work bands; the kicked peer's try_steal
// then pulls the highest band first. Affinity-ready: the per-thread mask gate
// lives in try_steal's victim pick (thread_may_run_on) -- this is only the cheap
// "is there anything to push" pre-check.
static bool cpu_has_surplus_for_kick(struct CpuSched *cs) {
    return __atomic_load_n(&cs->run_tree[SCHED_BAND_INTERACTIVE], __ATOMIC_RELAXED) ||
           __atomic_load_n(&cs->run_tree[SCHED_BAND_NORMAL], __ATOMIC_RELAXED);
}

#ifdef KERNEL_TESTS
// Test accessor (TI-4c): expose the static surplus-kick decision for `cpu` so a
// unit test can assert it. Bounds-checked; false for an OOB/uninitialized CPU.
bool sched_cpu_has_surplus_for_test(unsigned cpu) {
    if (cpu >= DTB_MAX_CPUS || !g_cpu_sched[cpu].initialized) return false;
    return cpu_has_surplus_for_kick(&g_cpu_sched[cpu]);
}
#endif /* KERNEL_TESTS */

// SYS_YIELD (#33): voluntary yield. sched() with prev RUNNING already IS the
// yield primitive (requeue at the back of the band + dispatch -- the
// sched_alpha.tla StartSwitch kind="yield" transition tick preemption drives);
// this wrapper adds only the fast path that makes a yield SYSCALL affordable:
// the per-CPU pinned idle is ALWAYS in-tree while a non-idle thread runs
// (sched_alpha.tla IdleAvailable), so an unconditional sched() on an
// otherwise-empty queue would dispatch the idle -- two context switches for
// nothing (thread -> idle -> the #363 park-guard loop re-dispatches the
// requeued yielder), on a call the Go runtime issues from spin loops (36.8M
// osyield calls per go build). Pre-#363 the cost was far worse -- a park up
// to the tickless backstop over the requeued yielder (the #33-audit F1).
//
// The peek reuses cpu_has_surplus_for_kick (TI-4c): non-idle band heads only,
// relaxed loads, never a Thread deref. Band IDLE is deliberately excluded --
// it holds the pinned idle (+ at most best-effort work, which the tick path
// serves at slice granularity); yielding to it is never useful. In the model
// the skipped call is a stutter (no state change), trivially safe.
//
// Advisory by design, racy in both directions, both benign:
//   - stale non-NULL (the queued thread was just stolen): sched() re-picks
//     under cs->lock and dispatches the idle for one bounce (the #363 loop
//     re-checks and parks only on a genuinely-empty queue) -- wasteful once,
//     correct.
//   - stale NULL (a thread is being placed concurrently): this yield skips;
//     the placer's own wake-preemption (ready_on's need_resched_set) serves
//     the placed thread at this very syscall's return tail
//     (preempt_check_irq runs on the EL0 sync-return path), and yield
//     callers loop.
//   - CPU-identity staleness (the #104/#107 TOCTOU shape) is HYPOTHETICAL
//     today: syscalls run IRQ-masked end-to-end (spinlock.h), so no preempt
//     can land inside the peek from the SVC path, and the test callers run
//     on the cpu_pinned kthread. A future IRQ-enabled kthread caller could
//     migrate between this_cpu_sched() and the loads and peek a FOREIGN
//     CPU's heads -- still benign: no lock is taken and nothing is mutated
//     on the peeked slot, and sched() re-derives the CPU under its own
//     entry mask.
//
// Returns whether it dispatched (called sched()) -- consumed by the kernel
// tests; the syscall handler discards it and returns 0 (POSIX sched_yield).
bool sched_yield_hint(void) {
    struct CpuSched *cs = this_cpu_sched();
    if (!cpu_has_surplus_for_kick(cs)) return false;
    sched();
    return true;
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

    // HMP foundation (#864): the running thread consumed a tick of CPU --
    // accrue its util estimate (the tick hook, ARCH §8.4.4). Inert on uniform
    // topology + for pinned threads (select_target_cpu ignores util there).
    sched_util_accrue(t);

    // Decrement; if expired, request preemption + replenish. The flag
    // is per-CPU — each CPU's IRQ writes its own slot.
    if (--t->slice_remaining <= 0) {
        need_resched_set(cpu);
        t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
    }

    // TI-4c push-on-overload rebalance (sched_rebalance.tla::Overload). The busy
    // CPU's still-running tick is the work-conservation re-poll the never-stopped
    // 1 kHz tick silently WAS before NO_HZ_IDLE: a deep-parked peer no longer
    // re-runs try_steal every ms, so without this, queued work strands on a busy
    // CPU until the 100 ms backstop (the TI-3 2.4x boot regression). If THIS
    // running CPU holds surplus stealable work AND a peer is parked in WFI, kick
    // ONE peer to come steal it (it wakes -> sched() -> try_steal pulls). The
    // kick IS sched_notify_idle_peer's register-then-observe IPI: the peer set
    // idle_in_wfi BEFORE parking (sched_tickless.tla), so the IPI lifts its park
    // (Overload's NoLostWake leg -- the BUGGY_KICK_NO_LIFT counterexample). The
    // pull-realizes-the-migration shape (kick -> peer try_steals) sits inside
    // sched_alpha.tla's proven arbitrary-placement envelope. Suppressed while
    // THIS CPU runs its own idle (no surplus to push -- it would run any queued
    // work itself via preempt) and gated on g_sched_notify_enabled so the
    // in-kernel test phase stays UP-quiescent.
    struct CpuSched *cs = &g_cpu_sched[cpu];
    if (g_sched_notify_enabled && t != cs->idle && cpu_has_surplus_for_kick(cs))
        (void)sched_notify_idle_peer();
}

void preempt_check_irq(void) {
    // Fast path: scheduler isn't initialized OR no preemption pending.
    // Every IRQ-return runs this; keep it cheap. P2-Cd: per-CPU flag.
    unsigned cpu = smp_cpu_idx_self();
    if (cpu >= DTB_MAX_CPUS) return;
    if (!need_resched_pending(cpu)) return;
    if (!g_cpu_sched[cpu].initialized) return;

    struct Thread *t = current_thread();
    if (!t) return;                             // pre-thread_init IRQ
    if (t->magic != THREAD_MAGIC) return;       // corruption — defer

    // #360: never preempt a thread inside a plain-spinlock hold. The
    // interrupted thread holds (or is acquiring) a lock that IRQ-masked
    // spinners may be waiting on; switching it out RUNNABLE while every CPU
    // fills with masked spinners is the #359 permanent deadlock. Return
    // WITHOUT consuming need_resched -- the flag must stay pending (it may
    // be the #866-F1 cross-CPU placement kick, set exactly once) so the
    // deferred preempt fires at the first IRQ-return after the hold drops
    // (<= 1 tick; the same granularity as the existing tick-driven
    // preemption). Reading the pre-increment value of a mid-RMW count is
    // safe: the thread holds nothing yet at that point (spinlock.h).
    if (t->preempt_count != 0u) return;

    // Clear the flag BEFORE sched() so a re-fire-during-sched doesn't double-
    // trigger. The cross-CPU write now exists for real (#866 F1: ready_on's
    // cross-CPU placement sets this CPU's flag so a busy target reschedules and
    // considers a just-placed thread) -- the clear here consumes both the local
    // (timer-tick) and the cross-CPU (placement) producer.
    need_resched_clear(cpu);

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
