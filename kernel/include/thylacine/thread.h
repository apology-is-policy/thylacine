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

// Thread states. P2-A primarily uses RUNNING and RUNNABLE (only those
// transition during context switches). SLEEPING and EXITING land at
// P2-B (wait/wake) and Phase 2 close (exit/reap) respectively.
//
// State values are non-zero so a zero-initialized Thread is detectably
// invalid — kmem_cache_alloc returns zero memory; the Thread is only
// usable after explicit initialization sets state.
enum thread_state {
    THREAD_STATE_INVALID  = 0,    // zero-initialized; not usable
    THREAD_RUNNING        = 1,
    THREAD_RUNNABLE       = 2,
    THREAD_SLEEPING       = 3,    // P2-B: wait/wake protocol
    THREAD_EXITING        = 4,    // P2 close: exit/reap path
};

struct Thread {
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
};

// Default kernel stack size per thread. 4 pages × 4 KiB = 16 KiB. Matches
// ARCHITECTURE.md §7.3 (THREAD_STACK_SIZE = 16 KiB default with guard
// page; the guard page lands at Phase 2 close).
#define THREAD_KSTACK_SIZE   (16 * 1024)

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

// Create a kernel thread of `proc` running `entry`. SLUB-allocates the
// Thread descriptor + 16 KiB kernel stack via alloc_pages(order=2);
// initializes ctx so the first switch-into the new thread lands at the
// trampoline which then blr's `entry`. Adds to proc->threads list.
//
// Returns NULL on OOM (Thread alloc fail or kstack alloc fail; cleanup
// is internal).
//
// At P2-A, entry takes no argument and must not return — the trampoline
// halts on WFE if it does. Phase 2 close adds entry-with-arg + thread_exit.
struct Thread *thread_create(struct Proc *proc, void (*entry)(void));

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
