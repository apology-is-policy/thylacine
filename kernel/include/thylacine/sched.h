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

// Diagnostic accessor: per-CPU idle thread pointer. Returns the Thread
// installed at sched_init time (CPU 0 = kthread; CPU N = the per_cpu_main
// idle). NULL if sched_init has not yet been called for cpu_idx, or
// cpu_idx is out of range.
struct Thread *sched_idle_thread(unsigned cpu_idx);

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
void ready(struct Thread *t);

// Diagnostic accessors.
unsigned sched_runnable_count(void);
unsigned sched_runnable_count_band(unsigned band);

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
