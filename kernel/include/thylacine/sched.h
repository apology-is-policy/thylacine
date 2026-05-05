// Scheduler — Plan 9 idiom layer over EEVDF dispatch (P2-Ba).
//
// Per ARCHITECTURE.md §8 (scheduler design): EEVDF on per-CPU run trees,
// three priority bands (INTERACTIVE / NORMAL / IDLE), Plan 9 idiom layer
// on top.
//
// P2-Ba lands the bare dispatch primitive: `sched()` (yield) + `ready(t)`
// (mark runnable). The wait/wake protocol (`thread_block` / `thread_wake`
// over a `Rendez`) lands at P2-Bb. Scheduler-tick preemption + IRQ-mask
// discipline land at P2-Bc.
//
// At P2-Ba: simplified EEVDF — each thread has an integer vd_t (virtual
// deadline). On yield, current's vd_t is advanced past the run tree's
// max so it lands at the back of the rotation; pick-next chooses the
// runnable thread with min vd_t. Full EEVDF math (vd_t = ve_t + slice ×
// W_total / w_self with weighted virtual time advance) lands at P2-Bc.

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

// Initialize the scheduler. Must be called AFTER thread_init: the kernel
// thread is the boot CPU's initial current_thread and must be in the
// scheduler's accounting before sched() can pick alternatives.
//
// Sets up the per-CPU run trees (one per band, all empty initially —
// kthread is RUNNING and not in any tree).
void sched_init(void);

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

#endif // THYLACINE_SCHED_H
