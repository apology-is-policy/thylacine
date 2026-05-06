// Thread descriptor — Plan 9 Thread, Thylacine adaptation.
//
// Per ARCHITECTURE.md §7.3. A Thread owns its register context, a kernel
// stack, run state, and Proc backref. P2-A's minimal struct carries
// everything cpu_switch_context needs (struct Context, kstack); per-band
// EEVDF data + scheduler links land at P2-B.
//
// "current thread" is parked in TPIDR_EL1 — ARMv8's per-CPU register
// reserved for OS use. This makes the accessor SMP-correct without a
// per-CPU array indirection: each CPU has its own TPIDR_EL1.

#ifndef THYLACINE_THREAD_H
#define THYLACINE_THREAD_H

#include <thylacine/context.h>
#include <thylacine/types.h>

struct Proc;
struct Rendez;

// Thread states. P2-A primarily uses RUNNING and RUNNABLE (only those
// transition during context switches). SLEEPING and EXITING land at
// P2-B (wait/wake) and Phase 2 close (exit/reap) respectively.
//
// State values are non-zero so a zero-initialized Thread is detectably
// invalid — kmem_cache_alloc with KP_ZERO returns all-zero memory; the
// Thread is only usable after explicit initialization sets state.
enum thread_state {
    THREAD_STATE_INVALID  = 0,    // zero-initialized; not usable
    THREAD_RUNNING        = 1,
    THREAD_RUNNABLE       = 2,
    THREAD_SLEEPING       = 3,    // P2-B: wait/wake protocol
    THREAD_EXITING        = 4,    // P2 close: exit/reap path
};

_Static_assert(THREAD_STATE_INVALID == 0,
               "THREAD_STATE_INVALID == 0 is the invariant that makes "
               "zero-initialized Thread structs detectably invalid "
               "(P2-A audit R4 F46 close)");

// THREAD_MAGIC — sentinel set at thread_create / thread_init; checked at
// thread_free. At offset 0 so SLUB's freelist write naturally clobbers
// it on kmem_cache_free; subsequent thread_free reads the clobbered
// value and extincts (P2-A audit R4 F42 close).
#define THREAD_MAGIC 0x54485244C0DEFADEULL    // 'THRD' || 0xC0DE'FADE

struct Thread {
    u64                magic;     // THREAD_MAGIC
    int                tid;
    enum thread_state  state;
    struct Proc       *proc;

    // Saved register context. Valid only when this thread is NOT
    // currently running on any CPU; for the running thread the live
    // CPU registers are canonical and ctx is stale until the next
    // cpu_switch_context call saves into it.
    struct Context     ctx;

    // Kernel stack. 16 KiB (THREAD_KSTACK_SIZE) allocated via alloc_pages
    // at order=2. kstack_base is the low address; ctx.sp is set to
    // kstack_base + kstack_size on thread_create (initial top of stack).
    //
    // No guard page below the stack at P2-A — Phase 2 close adds the
    // per-CPU exception stack + per-thread guard page.
    void              *kstack_base;
    size_t             kstack_size;

    // Linked-list links into Proc->threads (doubly-linked).
    struct Thread     *next_in_proc;
    struct Thread     *prev_in_proc;

    // P2-Ba: EEVDF / run-tree fields. The scheduler's per-CPU run tree
    // (sched.c) keys on `vd_t`; band selects which of three priority
    // tiers (INTERACTIVE / NORMAL / IDLE — sched.h SCHED_BAND_*) the
    // thread belongs to; weight is the EEVDF weight (default 1; higher
    // → more CPU share — full EEVDF math at P2-Bc).
    //
    // runnable_{next,prev} link the thread into its band's sorted-by-
    // vd_t doubly-linked list. Both NULL when the thread is NOT in any
    // run tree (RUNNING / SLEEPING / freshly-created).
    s64                vd_t;
    u32                weight;
    u32                band;            // SCHED_BAND_* (sched.h)
    struct Thread     *runnable_next;
    struct Thread     *runnable_prev;

    // P2-Bb: rendez backref. NULL when not sleeping on a Rendez. Set
    // by sleep(r, ...) under r->lock atomically with state =
    // THREAD_SLEEPING; cleared by wakeup(r) under r->lock atomically
    // with state = THREAD_RUNNABLE. Diagnostic + invariant aid: a
    // SLEEPING thread with rendez_blocked_on != NULL is sleeping on
    // that specific Rendez; any debugger / extinction dump can name
    // the wait condition. Future poll/futex (Phase 5) generalize to a
    // wait-list of (Rendez, condition) tuples; for now single-Rendez.
    struct Rendez     *rendez_blocked_on;

    // P2-Bc: scheduler-tick preemption. `slice_remaining` is the
    // number of remaining timer ticks before this thread's slice
    // expires. Replenished to THREAD_DEFAULT_SLICE_TICKS on every
    // RUNNABLE → RUNNING transition (sched() pick-next path).
    // Decremented by sched_tick() (called from the timer IRQ
    // handler); when ≤ 0, sets g_need_resched so preempt_check_irq
    // triggers a context switch on IRQ-return.
    //
    // Read/written from IRQ context (sched_tick) AND from sched()
    // (replenish on pick + decrement-check from preempt_check_irq).
    // IRQ-mask discipline in sched() prevents the IRQ handler from
    // racing with the replenish.
    s64                slice_remaining;

    // P2-Cf: SMP wait/wake race close. True while this thread is
    // ACTIVELY RUNNING on some CPU (registers live; saved context in
    // ctx is stale). Set to true by sched() (or thread_switch) when
    // the thread is picked as `next`; cleared to false by the resume
    // path on the destination CPU AFTER cpu_switch_context completed
    // — meaning the thread is fully switched out, ctx is canonical,
    // and another CPU may safely pick this thread without racing
    // mid-save.
    //
    // wakeup() spins on this flag before transitioning a SLEEPING
    // waiter to RUNNABLE: if the waiter is mid-switch, ready() would
    // insert it into a runqueue while it's still being saved on
    // another CPU — peer pick + execution while ctx half-written.
    // Linux's `task->on_cpu`; same role.
    //
    // Accessed via __atomic_load_n / __atomic_store_n with acquire/
    // release ordering so cross-CPU readers see the consistent
    // monotonic transitions.
    volatile bool      on_cpu;
};

_Static_assert(sizeof(struct Thread) == 232,
               "struct Thread size pinned at 232 bytes (P3-Bdb: P2-Cf "
               "baseline 224 + ctx.ttbr0 +8 = 232 — Context grew from "
               "112 to 120 bytes when TTBR0_EL1 was added at P3-Bdb). "
               "Adding a field grows the SLUB cache; update this assert "
               "deliberately so the change is intentional.");
_Static_assert(__builtin_offsetof(struct Thread, magic) == 0,
               "magic must be at offset 0 (P2-A audit R4 F42)");

// Default kernel stack size per thread. 4 pages × 4 KiB = 16 KiB.
// Matches ARCHITECTURE.md §7.3 (THREAD_STACK_SIZE = 16 KiB).
//
// P2-Dc adds a 4-page (16 KiB) guard region BELOW the kstack. Total
// allocation per thread is 8 pages = 32 KiB at order=3 from buddy.
// The guard pages are marked no-access in TTBR0 — a stack-overflow
// access faults via the kernel's identity mapping, the exception
// handler reports it, and extinction("kstack overflow") fires.
#define THREAD_KSTACK_SIZE         (16 * 1024)
#define THREAD_KSTACK_GUARD_SIZE   (16 * 1024)
#define THREAD_KSTACK_TOTAL_SIZE   (THREAD_KSTACK_SIZE + THREAD_KSTACK_GUARD_SIZE)
#define THREAD_KSTACK_TOTAL_ORDER  3                  // 8 pages = 32 KiB
#define THREAD_KSTACK_GUARD_PAGES  4                  // bottom 4 pages

// "current thread" is held in TPIDR_EL1, the per-CPU OS-use register.
// Accessed by inline mrs / msr — no function call overhead in the
// hot path. Returns NULL before thread_init runs (TPIDR_EL1 is zero
// at boot from BSS-default).
static inline struct Thread *current_thread(void) {
    u64 v;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(v));
    return (struct Thread *)(uintptr_t)v;
}

static inline void set_current_thread(struct Thread *t) {
    __asm__ __volatile__("msr tpidr_el1, %0" :: "r"((u64)(uintptr_t)t) : "memory");
}

// Bring up the thread subsystem. Allocates the kernel thread (TID 0),
// wires it to kproc (set up by proc_init), parks it in TPIDR_EL1 as
// the current thread. Must be called after proc_init (so kproc exists)
// and after slub_init (for the SLUB caches).
//
// The boot CPU was already running on the boot stack — kthread's ctx is
// initialized to zero; the FIRST cpu_switch_context call out of kthread
// will save the live registers into kthread.ctx. No "initial save" is
// needed.
void thread_init(void);

// Accessor for the kernel thread (TID 0). Returns NULL before thread_init.
struct Thread *kthread(void);

// P2-Cd: allocate the idle Thread for a secondary CPU.
//
// Same role kthread plays on the boot CPU: a Thread descriptor that
// sched_init records as "this CPU's idle." Doesn't own a kstack — the
// thread runs on the per-CPU boot stack already in use by per_cpu_main
// (the trampoline assigned that stack via SP_EL0 in start.S).
//
// State = THREAD_RUNNING (the caller is "running" as this thread by
// the time sched_init is called); ctx is zero-initialized and gets
// filled in by the first cpu_switch_context save (same pattern as
// kthread).
//
// Caller is expected to:
//   1. Call thread_init_per_cpu_idle(cpu_idx) on this CPU.
//   2. set_current_thread(returned).
//   3. sched_init(cpu_idx) — records the returned thread as this
//      CPU's idle.
//   4. Enter the per-CPU idle loop.
//
// Returns NULL on OOM (Thread alloc fail). cpu_idx >= 1 only — CPU 0's
// idle is kthread (set up in thread_init).
struct Thread *thread_init_per_cpu_idle(unsigned cpu_idx);

// Create a kernel thread of `proc` running `entry`. SLUB-allocates the
// Thread descriptor + 16 KiB kernel stack via alloc_pages(order=2);
// initializes ctx so the first switch-into the new thread lands at the
// trampoline which then blr's `entry`. Adds to proc->threads list.
//
// Returns NULL on OOM (Thread alloc fail or kstack alloc fail; cleanup
// is internal).
//
// `entry` takes no argument. For arg-passing (P2-D rfork), use
// thread_create_with_arg below.
//
// At P2-A, entry must not return — the trampoline halts on WFE if it
// does. P2-D adds exits() as the structured termination path.
struct Thread *thread_create(struct Proc *proc, void (*entry)(void));

// P2-D: same as thread_create but passes `arg` in x0 to `entry`.
// Internally sets ctx.x20 = arg; thread_trampoline does `mov x0, x20`
// before `blr x21` (entry pointer). The trampoline behavior is identical
// for both forms — thread_create is exactly thread_create_with_arg with
// arg=0; the call sites differ only in the entry signature they
// document.
struct Thread *thread_create_with_arg(struct Proc *proc,
                                      void (*entry)(void *),
                                      void *arg);

// Release a Thread descriptor + its kstack. Caller must ensure the
// thread is not current (current_thread() != t) and not still on any
// runqueue. Extincts on violation.
void thread_free(struct Thread *t);

// Direct context switch from current_thread to `next`. Updates state
// fields (prev → RUNNABLE, next → RUNNING), parks `next` in TPIDR_EL1,
// then calls cpu_switch_context.
//
// At P2-A this is the only way to multitask — there's no scheduler yet.
// Test code uses it to demonstrate the switching primitive works
// end-to-end; subsequent sub-chunks add the EEVDF dispatch on top.
//
// IRQ masking: P2-A does NOT mask IRQs around the switch. The only IRQ
// source live at v1.0 is the timer; its handler increments g_ticks and
// returns — it does not touch thread state. P2-B's scheduler will add
// the IRQ-mask discipline once preemption-via-IRQ becomes possible.
void thread_switch(struct Thread *next);

// Diagnostic.
u64 thread_total_created(void);
u64 thread_total_destroyed(void);

#endif // THYLACINE_THREAD_H
