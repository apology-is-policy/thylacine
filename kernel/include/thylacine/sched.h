// Scheduler — Plan 9 idiom layer over EEVDF dispatch (P2-Ba sched/ready;
// P2-Bb sleep/wakeup over Rendez).
//
// Per ARCHITECTURE.md §8 (scheduler design): EEVDF on per-CPU run trees,
// three priority bands (INTERACTIVE / NORMAL / IDLE), Plan 9 idiom layer
// on top.
//
// P2-Ba lands the bare dispatch primitive: `sched()` (yield) + `ready(t)`
// (mark runnable). P2-Bb lands `sleep(r, cond, arg)` + `wakeup(r)` (the
// wait/wake protocol over a Rendez per ARCH §8.5; declarations live in
// rendez.h). Scheduler-tick preemption + IRQ-mask discipline land at
// P2-Bc.
//
// At P2-Ba+P2-Bb: simplified EEVDF — each thread has an integer vd_t
// (virtual deadline). On yield, current's vd_t is advanced past the
// run tree's max so it lands at the back of the rotation; pick-next
// chooses the runnable thread with min vd_t. Full EEVDF math (vd_t =
// ve_t + slice × W_total / w_self with weighted virtual time advance)
// lands at P2-Bc.

#ifndef THYLACINE_SCHED_H
#define THYLACINE_SCHED_H

#include <thylacine/types.h>

struct Thread;

// Three priority bands per ARCH §8.3. Lower numeric value = higher
// priority. The scheduler always serves the highest-priority band with
// runnable threads; within a band, pick-min-vd_t.
#define SCHED_BAND_INTERACTIVE  0u
#define SCHED_BAND_NORMAL       1u
#define SCHED_BAND_IDLE         2u
#define SCHED_BAND_COUNT        3u

// HMP foundation (deep-smp-review #864, ARCH §8.4.4). The normalized
// per-CPU capacity scale: the most-capable core(s) on a topology read
// SCHED_CAPACITY_SCALE; a less-capable core reads a proportionally smaller
// value (Linux's 1024 convention). Per-task `util` (struct Thread) shares
// this scale. On a uniform topology (QEMU virt / RPi) every CPU is
// SCHED_CAPACITY_SCALE and the placement policy is inert.
#define SCHED_CAPACITY_SCALE    1024u

// Default scheduler slice in ticks. At 1000 Hz this is 6 ms, matching
// Linux EEVDF's default `sched_latency_ns / sched_min_granularity` ~=
// 6 ms. Per ARCH §8.2 — slice_size is tunable at /ctl/sched/slice-size
// post-Phase 4 (when /ctl/ exists); v1.0 P2-Bc bakes the default.
#define THREAD_DEFAULT_SLICE_TICKS  6

// Initialize THIS CPU's scheduler state. Must be called AFTER the CPU's
// idle thread is parked in TPIDR_EL1 — the idle thread is what sched()
// falls back to when the run tree empties (P2-Cd). For the boot CPU
// (cpu_idx=0), the idle thread is `kthread` (set up by thread_init).
// For secondaries, per_cpu_main creates a fresh idle Thread before
// calling sched_init.
//
// Each CPU initializes its own slot in the per-CPU sched state array;
// concurrent calls from different CPUs are race-free because they
// touch disjoint slots. Calling twice for the same cpu_idx extincts.
void sched_init(unsigned cpu_idx);

// P2-Ce: finish_task_switch helper. Called from thread_trampoline
// (arch/arm64/context.S) on a fresh thread's first run to release
// the run-tree lock that prev held at switch time. Also clears
// prev's on_cpu (P2-Cf) so a peer's wakeup() spin can observe
// completion.
void sched_finish_task_switch(void);

// P2-Cf: arm a clear-on_cpu handoff. Called by callers that perform
// their own context switch (currently: thread_switch, the P2-A
// direct-switch primitive). Stores `prev` in this CPU's CpuSched
// slot; the destination CPU's resume path consumes it via
// sched_finish_task_switch.
void sched_arm_clear_on_cpu(struct Thread *prev);

// RW-1 B-F1: the context-switch ASID pre-hook. Resolves next's Proc to its
// current-generation rolling ASID and installs the TTBR0_EL1 value into next's
// Context before the asm cpu_switch_context loads it. No-op for kproc / kernel
// threads (pgtable_root == 0). MUST be called with IRQs masked on the CPU next
// will run on (the run-queue lock is held at the sched() site). See
// arch/arm64/asid.{c,h}.
void sched_install_asid_ttbr0(struct Thread *next);

// Diagnostic accessor: per-CPU idle thread pointer. Returns the Thread
// installed at sched_init time (CPU 0 = kthread; CPU N = the per_cpu_main
// idle). NULL if sched_init has not yet been called for cpu_idx, or
// cpu_idx is out of range.
struct Thread *sched_idle_thread(unsigned cpu_idx);

// SMP redesign (ARCH §8.4.2): the boot CPU's idle-loop entry. cpu0's idle
// thread (thread_create_bootcpu_idle) runs this loop, dispatched by the
// ORDINARY pick_next from cpu0's run_tree[IDLE] — exactly like a secondary's
// idle in per_cpu_main's tail. NO deadlock-path special case, NO off-tree
// state (the #860 root cause retired). Same IRQ-mask + idle_in_wfi discipline
// as per_cpu_main's tail loop (R7 F128). Noreturn.
__attribute__((noreturn))
void bootcpu_idle_main(void);

// SMP redesign (ARCH §8.4.2): install cpu0's idle thread. Records it as
// g_cpu_sched[0].idle (overriding the kthread placeholder sched_init(0) set)
// AND ready()'s it into cpu0's run_tree[IDLE]. Called once from boot_main on
// cpu0 after thread_create_bootcpu_idle. Replaces the old
// sched_set_bootcpu_idle (which stored an off-tree deadlock-path pointer).
void sched_install_bootcpu_idle(struct Thread *t);

// P3-G: WFI signaling for ready/wakeup wake-idle-peer (closes R5-H F78).
//
// `sched_set_idle_in_wfi(true)` is called by THIS CPU immediately before
// entering `wfi` in per_cpu_main's idle loop; `false` is called immediately
// after WFI exits. Peer CPUs read these flags via `sched_idle_in_wfi(idx)`
// to identify wake targets; ready() / wakeup() send IPI_RESCHED to one
// such target so it wakes from WFI and runs sched (which try_steals the
// new work). Without this signaling, secondaries (no per-CPU timer at
// v1.0) starve runnable threads while sitting in WFI.
//
// The boot CPU participates too: post-SMP-redesign cpu0 runs a per-CPU pinned
// in-tree idle (`bootcpu_idle_main`, ARCH §8.4.2) that toggles this flag around
// its own WFI exactly like a secondary's idle -- the old "boot CPU stays FALSE
// forever / `_torpor` asm loop with no C hook" special case is retired with the
// `g_bootcpu_idle` off-tree state. See ARCH §8.4.5 + scheduler.tla `EnterWFI`.
void sched_set_idle_in_wfi(bool in_wfi);
bool sched_idle_in_wfi(unsigned cpu_idx);

// Tickless idle (NO_HZ_IDLE; docs/TICKLESS-IDLE.md TI-1, #299). The earliest
// pending deadline (absolute CNTVCT counter value) across all deadlined tsleep
// sleepers, or 0 if none. The idle loop (TI-2) arms its one-shot to
// min(this, now + TICKLESS_IDLE_BACKSTOP) before WFI so a genuinely-idle CPU
// takes no 1 kHz ticks yet still wakes for the next real deadline. O(n)
// min-scan under g_timerwait.lock (irqsave; a leaf acquisition). At TI-1 no
// production path calls this -- it lands with its consumer at TI-2.
u64 timerwait_earliest_deadline(void);

// Tickless idle re-poll backstop (NO_HZ_IDLE; docs/TICKLESS-IDLE.md TI-4e, #299).
// An idle CPU arms its one-shot to min(nearest deadline, now + this), so it
// re-polls (sched() -> try_steal) at ~250 Hz. 4 ms (TI-4e, user-voted).
//
// WHY 4 ms not the prior 100 ms: TI-4e root-caused the tickless boot slowdown to
// the wake LATENCY, not a guest bug -- the wake path is IPI-prompt (measured:
// 99.85% of tickless parks woken by an IPI, not the backstop), but under HVF
// resuming a DEEP-parked vCPU via SGI costs ~0.85 ms vs ~7 us when hot. That is
// an EMULATION artifact (HVF GICv2-MMIO vmexits + the host vCPU-thread resume;
// #299/#890), NOT a scheduler defect: on the bare-metal production target an SGI
// to a WFI'd core is hardware-fast (~ns), so deep-park already gives fast boot +
// ~0% idle. The 4 ms re-poll keeps the dev-loop vCPUs warm enough for a fast HVF
// boot (~7 s vs the ~17-35 s the 100 ms deep-park took) at ~5% HVF idle; on bare
// metal the re-poll is ~free. The 100 ms deep-park (0.3% HVF idle, the #299
// number) is the alternative -- 4 ms trades HVF idle for HVF dev-boot speed. A
// v1.x adaptive (warm-while-active / deep-when-idle) or accel-gated backstop is
// the recorded path to reclaim 0.3% HVF idle without the dev-boot cost.
#define TICKLESS_IDLE_BACKSTOP_NS  4000000ull

// Tickless idle (NO_HZ_IDLE; docs/TICKLESS-IDLE.md TI-2). Pure: the absolute
// CNTVCT value an idle CPU arms its one-shot to, given the current counter, the
// nearest pending deadline (0 = none), and the backstop delta in counter ticks.
// min(deadline, now + backstop), or just the backstop when no deadline. Split
// out so the arm-decision is unit-testable without the live timer/list.
u64 tickless_target_cnt(u64 now_cnt, u64 earliest_deadline, u64 backstop_cnt);

// Tickless idle (NO_HZ_IDLE; docs/TICKLESS-IDLE.md TI-2, #299). The shared idle
// body for bootcpu_idle_main + per_cpu_main: under one IRQ-masked region, set
// idle_in_wfi, sched() (yield to any runnable), arm a one-shot to the nearest
// deadline-or-backstop, WFI, then on wake restore the periodic tick + run the
// deadline scan. `tickless` gates the one-shot arm + restore: the boot CPU
// passes true; a secondary passes false until its timer PPI is enabled (then it
// just WFIs on IPI -- the byte-identical pre-preempt behavior). The I-9 arm-race
// holds because idle_in_wfi is set BEFORE the arm + WFI (register-then-observe;
// specs/sched_tickless.tla). See ARCH 8.6.
void sched_idle_park(bool tickless);

// P3-G: notify-idle-peer toggle. Off (default) during in-kernel tests so
// they keep their UP-like assumptions; on (set by boot_main between
// test_run_all() and joey_run()) for production. When off, ready() /
// wakeup() do NOT send IPI_RESCHED to idle peers — work-stealing happens
// only via the secondary's natural sched cycle (which never fires while
// secondaries sit in WFI; tests that depend on cross-CPU placement send
// IPIs explicitly). When on, every ready/wakeup that places work wakes
// an idle peer, closing R5-H F78 in the production path.
void sched_set_notify_enabled(bool enabled);

// Yield the CPU.
//
// Picks the highest-priority runnable thread across bands; within a
// band, the thread with min vd_t. If no other thread is runnable, sched
// returns immediately and the caller continues running.
//
// Otherwise: caller transitions RUNNING → RUNNABLE, is inserted into
// its band's run tree (with vd_t advanced past the max, putting it at
// the back of the rotation); pick-next is removed from its band's tree
// and transitioned RUNNABLE → RUNNING; cpu_switch_context performs the
// register save/load.
//
// Per ARCH §8.8 Plan 9 idiom layer. P2-Bc adds scheduler-tick preemption
// (timer IRQ injects sched() when a slice expires).
void sched(void);

// Mark `t` runnable and insert into its band's run tree.
//
// Preconditions:
//   - t != NULL.
//   - t->magic == THREAD_MAGIC.
//   - t->state == THREAD_RUNNABLE (caller's responsibility — typically
//     set by thread_create or thread_wake).
//   - t is NOT already in the run tree.
//
// thread_create defaults state to RUNNABLE but does NOT call ready —
// the caller decides when t becomes schedulable. This matches Plan 9's
// thread_create + ready separation.
//
// HMP foundation (ARCH §8.4.3): ready() = ready_on(select_target_cpu(t,
// self), t) — placement POLICY (select_target_cpu) is separated from the
// enqueue MECHANISM (ready_on). On a uniform topology select_target_cpu
// returns the caller's own CPU, so ready() is byte-identical to the
// pre-#864 "enqueue on the CPU that woke you" behavior.
void ready(struct Thread *t);

// HMP foundation (ARCH §8.4.3): the enqueue MECHANISM. Insert RUNNABLE `t`
// into `target_cpu`'s run tree under that CPU's lock. target_cpu == the
// caller's own CPU is the common (and only, on uniform topology) path and
// is identical to the pre-#864 ready(); a cross-CPU target (a capacity-aware
// placement, or a future misfit push) enqueues onto the peer's tree and —
// when notify is enabled (production) — wakes it. This cross-CPU capability
// is also the load-bearing shape for balance()'s deferred push path. An
// out-of-range / uninitialized target falls back to the caller's CPU.
// Safety under arbitrary target is the composition result proved by
// specs/sched_alpha.tla (Place picks the target non-deterministically).
void ready_on(unsigned target_cpu, struct Thread *t);

// RW-11 SA-1b (ARCH 8.3): realize the INTERACTIVE band for a latency-critical
// USER thread -- one that blocks waiting for a device IRQ (kobj_irq_wait) or
// console input (devcons_read). Sticky NORMAL -> INTERACTIVE so its wakes
// preempt NORMAL work (sched_wake_preempts). No-op for kernel threads (keeps
// the in-kernel test runner NORMAL) and idempotent (never touches IDLE idles).
void sched_mark_interactive(struct Thread *t);

// HMP foundation (ARCH §8.4.3): the placement POLICY. Choose the CPU whose
// run tree `t` should be enqueued on, given the CPU it last ran on / the
// CPU waking it (prev_cpu). v1.0 homogeneous body returns prev_cpu (CPU-
// pinned threads ALWAYS return prev_cpu — idles/kthread never migrate). On a
// DECLARED-heterogeneous topology it biases a high-util ("misfit") task to a
// higher-capacity CPU. Pure-ish (reads only per-CPU capacity + topology
// flag + t->util/cpu_pinned); the core decision is the testable pure
// function sched_place_by_capacity().
unsigned select_target_cpu(struct Thread *t, unsigned prev_cpu);

// HMP foundation (ARCH §8.4.4): parse + normalize per-CPU capacity from the
// DTB (capacity-dmips-mhz). Called ONCE on the boot CPU during late bring-up
// (after smp_init, so dtb_cpu_count is final). Stamps each CpuSched.capacity
// and the topology-heterogeneity flag. Composes with I-15.
void sched_capacity_init(void);

// Normalized capacity class of `cpu` in [0, SCHED_CAPACITY_SCALE]. Returns
// SCHED_CAPACITY_SCALE for an out-of-range index (defensive default = full).
u32  sched_cpu_capacity(unsigned cpu);

// True iff the DTB declared a heterogeneous topology (CPU capacities differ).
// False on QEMU virt / RPi (uniform) — the placement policy is then inert.
bool sched_topology_hetero(void);

// HMP foundation (ARCH §8.4.4) — the two PURE functions that hold the
// load-bearing logic, factored out so a kernel unit test can drive them
// deterministically against a SYNTHETIC asymmetric DTB (no real perf
// asymmetry needed; the "verification boundary").
//
// sched_capacity_normalize: given each CPU's raw capacity-dmips-mhz (0 =
// the node did not declare it), produce normalized capacities in [0,
// SCHED_CAPACITY_SCALE] (scaled so the max raw maps to SCALE; an absent
// entry defaults to SCALE). Returns true iff the result is heterogeneous
// (some CPU's capacity differs from another's).
bool sched_capacity_normalize(const u32 *raw_dmips, unsigned n, u32 *out_caps);

// sched_place_by_capacity: the placement decision. Given a task's `util`,
// the CPU it would default to (prev_cpu), the per-CPU normalized `caps`
// array (n entries), and whether the topology is heterogeneous, return the
// target CPU. Homogeneous → always prev_cpu. Heterogeneous → prev_cpu unless
// `util` overruns prev_cpu's capacity (a "misfit"), in which case the
// highest-capacity CPU. Pure; no globals.
unsigned sched_place_by_capacity(u32 util, unsigned prev_cpu,
                                 const u32 *caps, unsigned n, bool hetero);

// RW-11 SA-1b wake-preemption policy, PURE (the verification boundary -- a unit
// test drives it against synthetic bands). True iff a wake of a `woken_band`
// thread should preempt a `cur_band` current thread (or the idle). Fixed-
// priority bands (ARCH 8.3, lower number = higher priority): the idle yields to
// any real thread; otherwise a strictly-higher band preempts; same band is
// EEVDF-fair (no wake-preempt). No globals.
bool sched_wake_preempts(u32 woken_band, u32 cur_band, bool cur_is_idle);

// Diagnostic (HMP tests): true iff `t` is currently linked in `cpu`'s run
// tree. Takes that CPU's lock for a consistent read. Used by the cross-CPU
// ready_on enqueue test to prove placement landed on the intended CPU.
bool sched_in_cpu_tree(unsigned cpu, struct Thread *t);

// Test-support accessors (gated by KERNEL_TESTS, #61): both are exercised only
// by the in-kernel suite, so they share the build-shape gate with their .c
// definitions.
//   - sched_need_resched_pending (HMP tests, #866 F1): true iff `cpu`'s
//     need-resched flag is set. The cross-CPU ready_on enqueue test asserts it
//     is set on the target after a cross-CPU placement -- proving the placed
//     thread is reconsidered at the next preempt_check_irq rather than waiting a
//     full slice (the I-17/I-8 leak the #864 audit found and F1 fixes).
//   - sched_clear_need_resched_for_test (RW-11 SA-1b): clears a CPU's
//     need_resched so a unit test can establish a clean baseline before
//     exercising the same-CPU wake-preempt path.
//   - sched_cpu_has_surplus_for_test (TI-4c): the busy-tick overload-kick
//     decision for `cpu` -- true iff a non-IDLE run-tree band head is non-NULL
//     (migratable surplus). The unit test asserts a queued NORMAL thread makes
//     it true and a band-IDLE thread does not.
#ifdef KERNEL_TESTS
bool sched_need_resched_pending(unsigned cpu);
void sched_clear_need_resched_for_test(unsigned cpu);
bool sched_cpu_has_surplus_for_test(unsigned cpu);
void sched_set_need_resched_for_test(unsigned cpu);   // #360 gate test
#endif /* KERNEL_TESTS */

// #361 (audit-360 F2): extinction tail of the EL0-return preempt-count leak
// detector. Called from el0_return_die_check (kernel/proc.c) ONLY when
// t->preempt_count != 0 at an EL0 return -- a counted-acquire/raw-release
// mismatch (or an outright leak) that would otherwise pin the CPU
// non-preemptible forever with no detector (the sched() assert needs a
// sleep; a CPU-bound EL0 loop never sleeps). Lives in sched.c to read the
// sched-private outer-acquire breadcrumb for the report.
void sched_report_el0_leak(void) __attribute__((noreturn));

// Diagnostic accessors.
unsigned sched_runnable_count(void);
unsigned sched_runnable_count_band(unsigned band);
void sched_dump_runnable(const char *tag);   // DEBUG (#857): all-CPU runnable set + RX-IRQ counters

// TI-4d work-conservation telemetry. A core that parks in WFI while a runqueue
// holds queued-but-not-running work is the classic "wasted core" -- the
// work-conservation invariant ("A Decade of Wasted Cores", EuroSys'16). The
// idle-park path samples sched_has_runnable_work() at the instant it commits to
// parking; if work is queued, the WHOLE park duration is charged as STARVED (a
// steal/handoff gap -- this CPU should have run that work, not slept). The
// numbers decide a scheduler workload's character: a high starved fraction says
// queued-but-unstolen work exists (push/steal rebalance is the lever); ~0 says
// the workload is genuinely sequential (idle parks are correct, the cost is
// per-park overhead). Diagnostic only -- consulted by NO scheduling decision.
// The accumulators split by idle regime: the TOTAL covers every park; the
// TICKLESS subset covers only parks that went tickless (go_tickless == true,
// i.e. production -- g_sched_notify_enabled on). The split is load-bearing for
// the TI-4 question: a starved PERIODIC park ends at the next <=1ms tick (the
// pre-tickless baseline, correct), so most boot-time starvation is periodic
// test-phase re-poll; a starved TICKLESS park can run to the 100ms backstop, so
// the TICKLESS starved_ns / max is the regression's clean signal.
struct sched_wc_stats {
    u64 park_events;     // total idle parks (periodic + tickless)
    u64 idle_ns;         // total ns spent parked (the denominator)
    u64 starved_events;  // total parks committed while work was queued elsewhere
    u64 starved_ns;      // total ns parked while work was queued
    u64 max_starved_ns;  // the single longest starved park (any regime)
    u64 tickless_parks;          // parks that went tickless (production)
    u64 tickless_starved_events; // tickless parks committed on queued work
    u64 tickless_starved_ns;     // ns the tickless path starved (THE regression signal)
    u64 tickless_max_starved_ns; // longest tickless starved park (= backstop if hit)
    u64 tickless_oneshot_wakes;  // TI-4e wake-source telemetry: parks woken by the one-shot/backstop
    u64 tickless_ipi_wakes;      // TI-4e wake-source telemetry: parks woken (prompt) by an IPI
};
void sched_wc_stats(struct sched_wc_stats *out);

// prowl-3a (PROWL-DESIGN.md section 3.4): cumulative ns `cpu` spent idle-parked
// -- the per-CPU meter denominator for /ctl/cpu (utilization = 1 - the idle_ns
// fraction, diffed across polls). Coherent __atomic snapshot; 0 for an
// out-of-range or not-yet-initialized CPU. READ-ONLY telemetry.
u64 sched_cpu_idle_ns(unsigned cpu);

// True iff any non-idle band on any CPU's run tree is non-empty (queued
// runnable work exists somewhere). Reads ONLY the per-band head pointers
// (never dereferences a Thread), so it is safe to call lock-free from the hot
// idle-park path without risking a dangling runnable_next walk -- unlike
// sched_runnable_count(), which follows the chains and is for the cold callers.
bool sched_has_runnable_work(void);

// SYS_YIELD (#33): voluntary yield. If another non-idle thread is queued
// runnable on the calling CPU, requeue the caller (sched() with prev RUNNING
// -- the modeled StartSwitch kind="yield") and dispatch it; if the only local
// tree occupant is the pinned idle, return false WITHOUT switching (skipping
// the pointless idle bounce). Lock-free advisory peek (the TI-4c
// cpu_has_surplus_for_kick predicate); a hint, never a fairness guarantee.
// Returns whether it dispatched.
bool sched_yield_hint(void);

// Internal — called by thread_free if t->state == THREAD_RUNNABLE so the
// run tree doesn't carry a dangling pointer. Idempotent: safe to call
// on an already-not-in-tree thread.
void sched_remove_if_runnable(struct Thread *t);

// P2-Bc: scheduler tick. Called from the timer IRQ handler on every
// fire (1000 Hz at v1.0). Decrements current_thread's slice_remaining;
// when it drops to ≤ 0, sets the global need_resched flag so that
// preempt_check_irq triggers a context switch on IRQ-return. Cheap
// fast-path when sched_init hasn't run yet (silent no-op) — boot
// sequence safety.
void sched_tick(void);

// P2-Bc: preemption check, called from vectors.S after the IRQ
// handler has returned but before .Lexception_return restores the
// interrupted thread's context. If need_resched is set + the
// scheduler is initialized + current_thread is valid + IRQs are
// safely maskable, transitions current → RUNNABLE + advances vd_t
// + picks next + cpu_switch_context. On resume, returns up the
// stack to vectors.S which then runs KERNEL_EXIT and erets the
// (formerly preempted) thread back to its IRQed instruction.
void preempt_check_irq(void);

#endif // THYLACINE_SCHED_H
