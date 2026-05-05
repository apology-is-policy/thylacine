// Thread descriptor + context-switch wrapper (P2-A).
//
// Per ARCHITECTURE.md §7.3 + §8.5 (wakeup atomicity sketch — refined at
// P2-B). thread_create allocates a Thread + 16 KiB kernel stack and lays
// out the saved context so the FIRST cpu_switch_context into it lands at
// thread_trampoline, which then blr's the entry function.
//
// "current thread" is parked in TPIDR_EL1 (the per-CPU OS-reserved
// register) — accessed via inline mrs/msr in thread.h. thread_init
// installs kthread (TID 0) there as the boot CPU's current.
//
// At P2-A, thread_switch is the only multitasking primitive — direct
// switch, no scheduler dispatch, no IRQ masking (timer IRQ handler at
// v1.0 doesn't touch thread state, so reentrancy is trivially safe).
// P2-B adds the EEVDF scheduler on top + IRQ masking around the switch
// once preemption-via-IRQ becomes possible.

#include <thylacine/context.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../mm/phys.h"
#include "../mm/slub.h"

// 16 KiB stack = 4 pages = order 2.
#define THREAD_KSTACK_ORDER  2
_Static_assert((1u << THREAD_KSTACK_ORDER) * PAGE_SIZE == THREAD_KSTACK_SIZE,
               "THREAD_KSTACK_ORDER must produce THREAD_KSTACK_SIZE bytes");

static struct kmem_cache *g_thread_cache;
static struct Thread     *g_kthread;
static int                g_next_tid = 0;
static u64                g_thread_created;
static u64                g_thread_destroyed;

static void thread_link_into_proc(struct Thread *t, struct Proc *p) {
    t->prev_in_proc = NULL;
    t->next_in_proc = p->threads;
    if (p->threads) p->threads->prev_in_proc = t;
    p->threads = t;
    p->thread_count++;
}

static void thread_unlink_from_proc(struct Thread *t) {
    struct Proc *p = t->proc;
    if (t->prev_in_proc) {
        t->prev_in_proc->next_in_proc = t->next_in_proc;
    } else {
        if (p->threads != t) extinction("thread_unlink: list head mismatch");
        p->threads = t->next_in_proc;
    }
    if (t->next_in_proc) {
        t->next_in_proc->prev_in_proc = t->prev_in_proc;
    }
    t->next_in_proc = NULL;
    t->prev_in_proc = NULL;
    p->thread_count--;
}

void thread_init(void) {
    if (g_thread_cache) extinction("thread_init called twice");
    if (!kproc())       extinction("thread_init before proc_init");

    g_thread_cache = kmem_cache_create("thread",
                                       sizeof(struct Thread),
                                       8,
                                       KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_thread_cache) extinction("kmem_cache_create(thread) returned NULL");

    g_kthread = kmem_cache_alloc(g_thread_cache, KP_ZERO);
    if (!g_kthread) extinction("kmem_cache_alloc(kthread) failed");

    // KP_ZERO leaves every field 0/NULL on entry. Only the non-zero-
    // default values get explicit setters (magic, state, proc, weight,
    // band). The boot CPU is already running on the boot stack from
    // start.S; kthread doesn't own a separately-allocated stack —
    // kstack_base stays NULL via KP_ZERO. thread_free's free_pages
    // call is gated on kstack_base != NULL.
    //
    // EEVDF defaults: vd_t = 0 (via KP_ZERO; reserved kthread slot —
    // P2-Ba sched.c starts g_vd_counter at 1 so kthread keeps its
    // initial 0 until first sched() advances it). weight = 1 (full
    // EEVDF weight semantics at P2-Bc). band = SCHED_BAND_NORMAL.
    g_kthread->magic  = THREAD_MAGIC;
    g_kthread->tid    = 0;
    g_kthread->state  = THREAD_RUNNING;
    g_kthread->proc   = kproc();
    g_kthread->weight = 1;
    g_kthread->band   = SCHED_BAND_NORMAL;
    g_kthread->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;

    thread_link_into_proc(g_kthread, kproc());
    g_thread_created++;
    g_next_tid = 1;

    // Park kthread in TPIDR_EL1 as the current thread. From now on,
    // current_thread() returns g_kthread (until the first thread_switch).
    set_current_thread(g_kthread);
}

struct Thread *kthread(void) {
    return g_kthread;
}

struct Thread *thread_init_per_cpu_idle(unsigned cpu_idx) {
    if (!g_thread_cache) extinction("thread_init_per_cpu_idle before thread_init");
    if (!kproc())        extinction("thread_init_per_cpu_idle before proc_init");
    if (cpu_idx == 0)    extinction("thread_init_per_cpu_idle: cpu_idx 0 reserved for kthread");

    struct Thread *t = kmem_cache_alloc(g_thread_cache, KP_ZERO);
    if (!t) return NULL;

    // Same skeleton as thread_init's kthread setup. KP_ZERO already
    // cleared kstack_base (NULL — this thread runs on the per-CPU
    // boot stack from start.S, already-current SP_EL0), ctx (zero;
    // first cpu_switch_context save fills it in), runnable_{next,prev}
    // (NULL; not in any tree until the idle loop yields once).
    t->magic  = THREAD_MAGIC;
    t->tid    = g_next_tid++;
    t->state  = THREAD_RUNNING;
    t->proc   = kproc();
    t->weight = 1;
    t->band   = SCHED_BAND_IDLE;          // lowest priority band; only
                                          // runs when nothing else
                                          // is runnable on this CPU.
    t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;

    thread_link_into_proc(t, kproc());
    g_thread_created++;
    return t;
}

struct Thread *thread_create(struct Proc *proc, void (*entry)(void)) {
    if (!g_thread_cache) extinction("thread_create before thread_init");
    if (!proc)           extinction("thread_create with NULL proc");
    if (!entry)          extinction("thread_create with NULL entry");
    if (proc->magic != PROC_MAGIC)
        extinction("thread_create with corrupted proc");

    struct Thread *t = kmem_cache_alloc(g_thread_cache, KP_ZERO);
    if (!t) return NULL;

    struct page *stack_pg = alloc_pages(THREAD_KSTACK_ORDER, KP_ZERO);
    if (!stack_pg) {
        kmem_cache_free(g_thread_cache, t);
        return NULL;
    }
    void *kstack = (void *)(uintptr_t)page_to_pa(stack_pg);

    // KP_ZERO leaves every field 0/NULL; only the non-zero-default
    // values get explicit setters. EEVDF defaults: weight=1, band=
    // NORMAL, vd_t=0 (caller can ready() to insert into run tree;
    // ready picks up the current vd_t — fresh threads at vd_t=0 sort
    // ahead of long-running threads whose vd_t has advanced via
    // sched() yields).
    t->magic       = THREAD_MAGIC;
    t->tid         = g_next_tid++;
    t->state       = THREAD_RUNNABLE;
    t->proc        = proc;
    t->kstack_base = kstack;
    t->kstack_size = THREAD_KSTACK_SIZE;
    t->weight      = 1;
    t->band        = SCHED_BAND_NORMAL;
    t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;

    // Lay out the initial saved context so the first cpu_switch_context
    // into this thread lands at thread_trampoline, which blr's entry.
    //
    //   ctx.x21 = entry         — trampoline does `blr x21`
    //   ctx.lr  = trampoline    — cpu_switch_context's `ret` lands here
    //   ctx.sp  = top of kstack — 16-byte aligned (alloc_pages returns
    //                              page-aligned, 16 KiB = aligned at top)
    //
    // Other ctx fields stay zero via KP_ZERO.
    t->ctx.x21 = (u64)(uintptr_t)entry;
    t->ctx.lr  = (u64)(uintptr_t)thread_trampoline;
    t->ctx.sp  = (u64)((uintptr_t)kstack + THREAD_KSTACK_SIZE);

    thread_link_into_proc(t, proc);
    g_thread_created++;
    return t;
}

void thread_free(struct Thread *t) {
    if (!t)                       extinction("thread_free(NULL)");
    // Magic check catches double-free and corrupt-Thread passes. SLUB's
    // freelist write at kmem_cache_free clobbers magic; subsequent free
    // reads the clobbered value and trips here.
    if (t->magic != THREAD_MAGIC) extinction("thread_free of corrupted or already-freed Thread");
    if (t == g_kthread)           extinction("thread_free attempted on kthread");
    if (t == current_thread())    extinction("thread_free of currently-running thread");
    if (t->state == THREAD_STATE_INVALID)
                                  extinction("thread_free of uninitialized Thread");
    if (t->state == THREAD_RUNNING)
                                  extinction("thread_free of RUNNING thread");

    // If t was RUNNABLE and sitting in a run tree, remove it before
    // unlinking from the proc list. sched_remove_if_runnable is a
    // no-op for non-RUNNABLE threads.
    sched_remove_if_runnable(t);

    thread_unlink_from_proc(t);

    if (t->kstack_base) {
        struct page *pg = pa_to_page((paddr_t)(uintptr_t)t->kstack_base);
        free_pages(pg, THREAD_KSTACK_ORDER);
    }

    kmem_cache_free(g_thread_cache, t);
    g_thread_destroyed++;
}

void thread_switch(struct Thread *next) {
    struct Thread *prev = current_thread();
    if (!prev)            extinction("thread_switch with no current thread");
    if (!next)            extinction("thread_switch(NULL)");
    if (next->magic != THREAD_MAGIC)
        extinction("thread_switch into corrupted or freed Thread");
    if (prev == next)     return;
    if (next->state == THREAD_STATE_INVALID)
        extinction("thread_switch into uninitialized Thread");
    if (next->state == THREAD_EXITING)
        extinction("thread_switch into EXITING thread");

    // State + current pointer updates BEFORE the asm switch. From the
    // outgoing thread's perspective these writes are observed by it
    // again only when some peer switches BACK into it (and at that
    // point, the peer's writes have set prev->state = RUNNING and
    // current_thread = prev). The window between these writes and the
    // cpu_switch_context call is invisible to the outgoing thread —
    // and at v1.0 single-CPU + no preemption, no other observer races
    // it. P2-B refines the ordering when SMP + scheduler-tick preemption
    // make this window observable.
    prev->state = THREAD_RUNNABLE;
    next->state = THREAD_RUNNING;
    set_current_thread(next);

    cpu_switch_context(&prev->ctx, &next->ctx);

    // Resumption point: prev was switched back to. current_thread() now
    // points to prev (set by whichever peer called thread_switch(prev)),
    // and prev->state = RUNNING.
}

u64 thread_total_created(void)   { return g_thread_created; }
u64 thread_total_destroyed(void) { return g_thread_destroyed; }
