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

#include "../arch/arm64/mmu.h"
#include "../mm/phys.h"
#include "../mm/slub.h"

// P2-Dc: 32 KiB total allocation per thread = 4 stack pages + 4 guard
// pages. order=3 = 8 pages from buddy. The guard pages are marked
// no-access via mmu_set_no_access so a stack-overflow access faults.
_Static_assert((1u << THREAD_KSTACK_TOTAL_ORDER) * PAGE_SIZE == THREAD_KSTACK_TOTAL_SIZE,
               "THREAD_KSTACK_TOTAL_ORDER must produce THREAD_KSTACK_TOTAL_SIZE bytes");
_Static_assert(THREAD_KSTACK_GUARD_PAGES * PAGE_SIZE == THREAD_KSTACK_GUARD_SIZE,
               "THREAD_KSTACK_GUARD_PAGES * PAGE_SIZE must equal THREAD_KSTACK_GUARD_SIZE");

static struct kmem_cache *g_thread_cache;
static struct Thread     *g_kthread;
// P6-pouch-threads (sub-chunk 9a) audit F4 close: atomic u32 (was non-
// atomic int). Multi-thread Procs now allow concurrent sys_thread_spawn
// calls from peer Threads on different CPUs; the prior non-atomic `++`
// races and can yield duplicate tids (R/M/W collision on the counter),
// breaking pthread_self uniqueness. Mirrors the proc_alloc g_next_pid
// discipline (P3-A: atomic fetch-add + u32 + INT_MAX guard). The public
// per-Thread `tid` type stays signed `int` — the cast at assignment is
// well-defined for u32 < 2^31.
static u32                g_next_tid = 0;
static u64                g_thread_created;
static u64                g_thread_destroyed;
// #788: count of thread_free calls that had to spin for an in-flight
// cpu_switch_context away from the victim (on_cpu==true at entry). >0 proves
// the SLEEPING/EXITING-but-still-on_cpu free window is real + the gate caught
// it. Diagnostic stat; read via thread_free_oncpu_waits().
static u64                g_thread_free_oncpu_waits;

u64 thread_free_oncpu_waits(void);
u64 thread_free_oncpu_waits(void) {
    return __atomic_load_n(&g_thread_free_oncpu_waits, __ATOMIC_RELAXED);
}

// Allocate the next monotonic tid + extinct on INT_MAX overflow. The
// overflow path is unreachable at v1.0 thread counts (< 10^5 in any
// imaginable workload), but the guard fail-closes loudly if it ever
// approaches — INT_MAX-wrap would alias the (-1) error sentinel.
static int alloc_next_tid(void) {
    u32 next = __atomic_fetch_add(&g_next_tid, 1u, __ATOMIC_RELAXED);
    if (next >= 0x7fffffffu) extinction("alloc_next_tid: g_next_tid would overflow INT_MAX");
    return (int)next;
}

// P6-pouch-threads (sub-chunk 9a) audit F1 close: link/unlink under
// g_proc_table_lock so concurrent walkers (proc_count_live_peers_locked,
// wait_pid's reap-loop pre-unlink_child snapshot) observe a coherent
// list. Pre-9a the lock was unnecessary because Procs were single-
// threaded (only one writer per Proc, ever). Multi-thread Procs at 9a
// introduce a second writer (sys_thread_spawn from another peer Thread
// on a peer CPU) — without the lock, a walker can see `p->threads =
// new_head` published before `new_head->next_in_proc = old_head` is
// visible (aarch64 weak memory), terminate the walk at the new head,
// and miss the previously-existing threads. Taking the lock makes the
// (head, next_in_proc, thread_count) trio a single linearizable
// observation.
//
// Boot ordering: proc_init runs before thread_init (kernel/main.c), so
// g_proc_table_lock is initialized by the time thread_init's first call
// arrives. Boot is single-CPU; the lock is uncontended there. SMP arrives
// later with thread_init_per_cpu_idle, by which point the lock is fully
// live.
static void thread_link_into_proc(struct Thread *t, struct Proc *p) {
    irq_state_t s = proc_table_lock_acquire();
    t->prev_in_proc = NULL;
    t->next_in_proc = p->threads;
    if (p->threads) p->threads->prev_in_proc = t;
    p->threads = t;
    p->thread_count++;
    proc_table_lock_release(s);
}

static void thread_unlink_from_proc(struct Thread *t) {
    irq_state_t s = proc_table_lock_acquire();
    struct Proc *p = t->proc;
    if (t->prev_in_proc) {
        t->prev_in_proc->next_in_proc = t->next_in_proc;
    } else {
        if (p->threads != t) {
            proc_table_lock_release(s);
            extinction("thread_unlink: list head mismatch");
        }
        p->threads = t->next_in_proc;
    }
    if (t->next_in_proc) {
        t->next_in_proc->prev_in_proc = t->prev_in_proc;
    }
    t->next_in_proc = NULL;
    t->prev_in_proc = NULL;
    p->thread_count--;
    proc_table_lock_release(s);
}

void thread_init(void) {
    if (g_thread_cache) extinction("thread_init called twice");
    if (!kproc())       extinction("thread_init before proc_init");

    // R12-FP audit close (F165 P2): align via `_Alignof(struct Thread)`
    // rather than the prior hardcoded 8. struct Thread's `_Alignas(16)`
    // on its embedded Context.fp_v requires alignof=16 for the STP/LDP-Q
    // pairs in cpu_switch_context (ARM ARM C7.2.348: STP/LDP-Q at
    // [Xn, #offset] require Xn + offset to be 16-aligned). With align=8,
    // adjacent SLUB slab objects work today only because sizeof(struct
    // Thread)=784 happens to be a multiple of 16 — a future field
    // addition that produces a non-multiple-of-16 sizeof would silently
    // misalign object N>0 and corrupt the first context switch.
    g_thread_cache = kmem_cache_create("thread",
                                       sizeof(struct Thread),
                                       _Alignof(struct Thread),
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
    g_kthread->on_cpu = true;       // P2-Cf: kthread is the boot CPU's running thread

    // P3-Bdb: pre-populate ctx.ttbr0 with the kernel-only TTBR0 root
    // (l0_ttbr0 PA + ASID 0). kthread is the boot CPU's running thread
    // at thread_init time; the FIRST cpu_switch_context FROM kthread
    // would capture the live TTBR0_EL1 anyway, but pre-populating
    // protects the path where some peer CPU switches INTO kthread (via
    // work-stealing or kproc-bound work) before kthread has switched-
    // out once — that load would otherwise read ctx.ttbr0 = 0 (KP_ZERO)
    // and write 0 to TTBR0_EL1, faulting on any kernel-mode kstack
    // access via SP=high VA → TTBR1 (which is fine — TTBR1 is alive)
    // BUT also breaking any later transition to user-mode (TTBR0 = 0
    // points at PA 0, garbage). Defense-in-depth.
    g_kthread->ctx.ttbr0 = (u64)mmu_kernel_ttbr0_pa();

    thread_link_into_proc(g_kthread, kproc());
    __atomic_fetch_add(&g_thread_created, 1u, __ATOMIC_RELAXED);
    // Boot-only single-CPU assignment; subsequent allocations use
    // alloc_next_tid (atomic).
    __atomic_store_n(&g_next_tid, 1u, __ATOMIC_RELAXED);

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
    // secondary boot stack from start.S, the current SP_EL1; the
    // kernel runs uniformly at EL1h, so SP_EL1 is the live kernel
    // stack — ARCHITECTURE.md §12.1, I-21), ctx (zero; first
    // cpu_switch_context save fills it in), runnable_{next,prev}
    // (NULL; not in any tree until the idle loop yields once).
    //
    // kstack_base == NULL marks this as a CPU-pinned bootstrap thread:
    // it has no portable per-thread kstack, so try_steal must never
    // migrate it (sched.c, audit F2 / I-21).
    t->magic  = THREAD_MAGIC;
    t->tid    = alloc_next_tid();
    t->state  = THREAD_RUNNING;
    t->proc   = kproc();
    t->weight = 1;
    t->band   = SCHED_BAND_IDLE;          // lowest priority band; only
                                          // runs when nothing else
                                          // is runnable on this CPU.
    t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;
    t->on_cpu = true;                     // P2-Cf: per-CPU idle is
                                          // the executing thread on
                                          // this CPU at sched_init.

    // P3-Bdb: kernel-only TTBR0 root. See thread_init for rationale.
    t->ctx.ttbr0 = (u64)mmu_kernel_ttbr0_pa();

    thread_link_into_proc(t, kproc());
    __atomic_fetch_add(&g_thread_created, 1u, __ATOMIC_RELAXED);
    return t;
}

// Internal: shared body of thread_create + thread_create_with_arg.
// `entry_void` is the entry pointer (no arg or void* arg form — the
// trampoline doesn't distinguish; x0 is loaded from x20 unconditionally).
// `arg` is parked in ctx.x20; for no-arg entries pass NULL and the entry
// just ignores x0 (caller-saved).
//
// P2-Dc: kstack layout — 8 pages total (32 KiB) at order=3.
//
//   page 0 (lowest)  ┐
//   page 1           │  guard region (16 KiB no-access)
//   page 2           │
//   page 3           ┘
//   page 4           ┐
//   page 5           │  usable kstack (16 KiB RW)
//   page 6           │
//   page 7 (highest) ┘  ctx.sp starts at top of page 7
//
// Stack grows DOWN. An overflow past page 4 hits the guard at page 3,
// where the L3 PTE is invalid in the kernel direct map (P3-Bca) →
// permission/translation fault → exception handler reports stack-
// overflow extinction.
//
// P3-Bca: kstack VAs are kernel direct-map KVAs (pa_to_kva of the
// alloc'd PA). Pre-P3-Bca they were PA-as-VA via TTBR0 identity; P3-Bd
// retires TTBR0 identity entirely so the direct-map form is the durable
// one. The PA is recovered via kva_to_pa for free + guard restore.
static struct Thread *thread_create_internal(struct Proc *proc,
                                             void *entry_void,
                                             void *arg) {
    if (!g_thread_cache) extinction("thread_create before thread_init");
    if (!proc)           extinction("thread_create with NULL proc");
    if (!entry_void)     extinction("thread_create with NULL entry");
    if (proc->magic != PROC_MAGIC)
        extinction("thread_create with corrupted proc");

    struct Thread *t = kmem_cache_alloc(g_thread_cache, KP_ZERO);
    if (!t) return NULL;

    struct page *stack_pg = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, KP_ZERO);
    if (!stack_pg) {
        kmem_cache_free(g_thread_cache, t);
        return NULL;
    }
    paddr_t guard_pa = page_to_pa(stack_pg);
    void *kalloc_base = pa_to_kva(guard_pa);

    // P2-Dc / P3-Bca: protect the lower THREAD_KSTACK_GUARD_PAGES as
    // no-access in the direct map. The 8-page allocation at order=3 is
    // 32-KiB-aligned, so the 4 guard pages at the bottom always lie
    // within a single 2 MiB block (32 KiB ≪ 2 MiB). One demote chain +
    // one TLB flush covers all guards.
    if (!mmu_set_no_access_range(guard_pa, THREAD_KSTACK_GUARD_PAGES)) {
        free_pages(stack_pg, THREAD_KSTACK_TOTAL_ORDER);
        kmem_cache_free(g_thread_cache, t);
        return NULL;
    }

    // KP_ZERO leaves every field 0/NULL; only the non-zero-default
    // values get explicit setters. EEVDF defaults: weight=1, band=
    // NORMAL, vd_t=0 (caller can ready() to insert into run tree;
    // ready picks up the current vd_t — fresh threads at vd_t=0 sort
    // ahead of long-running threads whose vd_t has advanced via
    // sched() yields).
    t->magic       = THREAD_MAGIC;
    t->tid         = alloc_next_tid();
    t->state       = THREAD_RUNNABLE;
    t->proc        = proc;
    t->kstack_base = kalloc_base;          // P2-Dc: lowest page (guard)
    t->kstack_size = THREAD_KSTACK_TOTAL_SIZE;
    t->weight      = 1;
    t->band        = SCHED_BAND_NORMAL;
    t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;

    // Lay out the initial saved context so the first cpu_switch_context
    // into this thread lands at thread_trampoline, which blr's entry.
    //
    //   ctx.x20 = arg           — trampoline does `mov x0, x20` (P2-D)
    //   ctx.x21 = entry         — trampoline does `blr x21`
    //   ctx.lr  = trampoline    — cpu_switch_context's `ret` lands here
    //   ctx.sp  = top of kstack — page 7's top edge = kalloc_base +
    //                              THREAD_KSTACK_TOTAL_SIZE (P2-Dc).
    //                              Stack grows down through pages 7→4
    //                              (16 KiB usable) then hits guard.
    //   ctx.ttbr0 = (asid << 48) | pgtable_root  — P3-Bdb. Loaded into
    //                              TTBR0_EL1 on first cpu_switch_context
    //                              into this thread. For non-kproc Procs:
    //                              the Proc's ASID + pgtable_root.
    //                              For kproc threads: kernel-only TTBR0
    //                              (l0_ttbr0 PA | ASID 0).
    //
    // Other ctx fields stay zero via KP_ZERO.
    t->ctx.x20 = (u64)(uintptr_t)arg;
    t->ctx.x21 = (u64)(uintptr_t)entry_void;
    t->ctx.lr  = (u64)(uintptr_t)thread_trampoline;
    t->ctx.sp  = (u64)((uintptr_t)kalloc_base + THREAD_KSTACK_TOTAL_SIZE);
    t->ctx.ttbr0 = (proc->pgtable_root != 0)
                 ? (((u64)proc->asid << 48) | (u64)proc->pgtable_root)
                 : (u64)mmu_kernel_ttbr0_pa();      // ASID 0 implicit

    thread_link_into_proc(t, proc);
    __atomic_fetch_add(&g_thread_created, 1u, __ATOMIC_RELAXED);
    return t;
}

struct Thread *thread_create(struct Proc *proc, void (*entry)(void)) {
    return thread_create_internal(proc, (void *)(uintptr_t)entry, NULL);
}

struct Thread *thread_create_with_arg(struct Proc *proc,
                                      void (*entry)(void *),
                                      void *arg) {
    return thread_create_internal(proc, (void *)(uintptr_t)entry, arg);
}

// P6-pouch-threads (sub-chunk 9): create a Thread that on first dispatch
// erets to EL0 — the substrate for pthread_create.
//
// Same body as thread_create_internal but ctx.lr points at
// thread_user_trampoline (not thread_trampoline) and the four user-mode
// arguments are parked in the callee-saved ctx slots that
// cpu_switch_context reloads before `ret`:
//
//   ctx.x19 = user_sp_va    — SP_EL0 install before eret
//   ctx.x20 = user_arg      — moved to x0 (AAPCS64 arg-0) before eret
//   ctx.x21 = user_entry_va — moved to ELR_EL1 before eret
//   ctx.x22 = user_tls_va   — written to TPIDR_EL0 before eret
//
// thread_user_trampoline (arch/arm64/context.S) consumes those four
// slots, does the userland_enter dance inline (so x0 = arg survives the
// GPR-zeroing sweep), and erets. See its asm comment for the discipline.
//
// The kstack layout (16 KiB usable + 16 KiB guard) is identical to the
// EL1-side helpers; trampoline runs at EL1 for the brief window between
// schedule-in and eret. The kstack stays valid past the eret because
// every later exception (syscall, IRQ, fault) lands on it.
//
// TTBR0 (ASID | pgtable_root) comes from `proc` exactly as in
// thread_create_internal — a userspace Proc always has pgtable_root != 0
// post-exec_setup. Kproc would fail with TTBR0 = kernel's, which is the
// correct fault for a buggy caller; the syscall handler rejects calls
// from threads not on a userspace Proc upstream.
struct Thread *thread_create_user(struct Proc *proc,
                                  u64 user_entry_va,
                                  u64 user_sp_va,
                                  u64 user_arg,
                                  u64 user_tls_va) {
    if (!g_thread_cache) extinction("thread_create_user before thread_init");
    if (!proc)           extinction("thread_create_user with NULL proc");
    if (proc->magic != PROC_MAGIC)
        extinction("thread_create_user with corrupted proc");

    struct Thread *t = kmem_cache_alloc(g_thread_cache, KP_ZERO);
    if (!t) return NULL;

    struct page *stack_pg = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, KP_ZERO);
    if (!stack_pg) {
        kmem_cache_free(g_thread_cache, t);
        return NULL;
    }
    paddr_t guard_pa = page_to_pa(stack_pg);
    void *kalloc_base = pa_to_kva(guard_pa);

    if (!mmu_set_no_access_range(guard_pa, THREAD_KSTACK_GUARD_PAGES)) {
        free_pages(stack_pg, THREAD_KSTACK_TOTAL_ORDER);
        kmem_cache_free(g_thread_cache, t);
        return NULL;
    }

    t->magic       = THREAD_MAGIC;
    t->tid         = alloc_next_tid();
    t->state       = THREAD_RUNNABLE;
    t->proc        = proc;
    t->kstack_base = kalloc_base;
    t->kstack_size = THREAD_KSTACK_TOTAL_SIZE;
    t->weight      = 1;
    t->band        = SCHED_BAND_NORMAL;
    t->slice_remaining = THREAD_DEFAULT_SLICE_TICKS;

    // Park the four user-mode args in callee-saved ctx slots.
    t->ctx.x19 = user_sp_va;
    t->ctx.x20 = user_arg;
    t->ctx.x21 = user_entry_va;
    t->ctx.x22 = user_tls_va;
    t->ctx.lr  = (u64)(uintptr_t)thread_user_trampoline;
    t->ctx.sp  = (u64)((uintptr_t)kalloc_base + THREAD_KSTACK_TOTAL_SIZE);
    t->ctx.ttbr0 = (proc->pgtable_root != 0)
                 ? (((u64)proc->asid << 48) | (u64)proc->pgtable_root)
                 : (u64)mmu_kernel_ttbr0_pa();

    thread_link_into_proc(t, proc);
    __atomic_fetch_add(&g_thread_created, 1u, __ATOMIC_RELAXED);
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
    // unlinking from the proc list. sched_remove_if_runnable walks all
    // CPU run trees under their locks and removes if found; it is a
    // no-op for non-RUNNABLE threads or threads not in any tree (e.g.
    // fresh-created Threads that never had ready() called on them — a
    // legitimate test-only pattern: thread_create then thread_free
    // without any scheduler exposure).
    sched_remove_if_runnable(t);

    // R5-H F76: post-walk RUNNING re-check. Closes the SMP race in which
    // pick_next on a peer CPU transitions t from RUNNABLE to RUNNING
    // BETWEEN the line-253 state read and sched_remove_if_runnable's
    // walk: thread_free's first check sees RUNNABLE → passes; the walk's
    // fast-path observes RUNNING → returns without removing; thread_free
    // would otherwise free a thread that's currently running on the
    // peer CPU (UAF on its kstack mid-cpu_switch_context). After the
    // walk, t is provably not in any tree (we held every cs->lock
    // momentarily), so no future pick can transition state to RUNNING.
    // Thus this re-check IS atomic with the walk: state is stable
    // here. If state is RUNNING at this point, we lost the race.
    if (t->state == THREAD_RUNNING)
        extinction("thread_free: peer CPU transitioned t to RUNNING between gate and walk");

    // #788: wait for any in-flight cpu_switch_context AWAY from t to finish
    // before reclaiming its Thread slot + kstack. The state gates above prove
    // t will not be PICKED again (not RUNNING; removed from every run tree by
    // the walk) -- but a thread that just went SLEEPING or EXITING can still
    // be on_cpu==true: t's OWN sched() is mid-flight on a peer CPU, physically
    // executing on t's kstack and about to do the register-SAVE half of
    // cpu_switch_context into t->ctx. on_cpu is cleared only by the
    // destination CPU's resume frame AFTER that switch completes (sched.c
    // prev_to_clear_on_cpu). The R5-H F76 re-check above closed the RUNNING
    // flavor of this race (RUNNABLE->RUNNING via a peer pick) but NOT the
    // SLEEPING/EXITING-but-still-on_cpu flavor. Freeing here returns t's SLUB
    // slot + order-3 kstack to the allocators while the peer is still writing
    // them: a buddy-LIFO recycle (the very next rfork's thread_create) aliases
    // t's slot/kstack, and the peer's stale register-save corrupts the
    // recycled thread's ctx (sp/lr/callee-saved) -> the recycled thread
    // resumes onto a wild SP and faults in its own guard ("kernel stack
    // overflow"). That is the #788 race: SMP-only (needs a secondary running
    // the freed thread -- 0/20 at -smp 1 vs 2/20 at -smp 4), ~10-20% on the
    // exact clean binary, riding the #789 E-core host-stall that widens the
    // SLEEPING..on_cpu-cleared window past the caller's drain. on_cpu is the
    // canonical "stack/ctx still in use" signal; gate on it. Mirrors the
    // reap-path spin in wait_pid (proc.c). thread_free is always called
    // lock-free, so the spin cannot deadlock; it is bounded by the single
    // in-flight switch (the peer always resumes and clears on_cpu).
    if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&g_thread_free_oncpu_waits, 1u, __ATOMIC_RELAXED);
        while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
            __asm__ __volatile__("yield" ::: "memory");
        }
    }

    thread_unlink_from_proc(t);

    if (t->kstack_base) {
        // P2-Dc / P3-Bca: restore guard pages to normal RW in the direct
        // map before returning the allocation to buddy. If a future
        // allocation reuses these pages (for any purpose, kstack or
        // otherwise), they need normal access; leaving them no-access
        // would silently fault the next user. Batched into one TLB
        // flush. kstack_base is a direct-map KVA; recover the PA via
        // kva_to_pa.
        paddr_t guard_pa = kva_to_pa(t->kstack_base);
        mmu_restore_normal_range(guard_pa, THREAD_KSTACK_GUARD_PAGES);

        struct page *pg = pa_to_page(guard_pa);
        free_pages(pg, THREAD_KSTACK_TOTAL_ORDER);
    }

    kmem_cache_free(g_thread_cache, t);
    __atomic_fetch_add(&g_thread_destroyed, 1u, __ATOMIC_RELAXED);
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

    // P2-Cf: same on_cpu handoff as sched(). thread_switch is a P2-A
    // direct-switch primitive that bypasses the scheduler; we still
    // need on_cpu transitions so wakeup() observes correct state if
    // a thread_switch'd thread later participates in Rendez-based
    // wait/wake. Use sched's per-CPU prev_to_clear_on_cpu via the
    // public arm/consume helpers.
    __atomic_store_n(&next->on_cpu, true, __ATOMIC_RELAXED);
    sched_arm_clear_on_cpu(prev);
    set_current_thread(next);

    cpu_switch_context(&prev->ctx, &next->ctx);

    // Resumption point: prev was switched back to. current_thread() now
    // points to prev (set by whichever peer called thread_switch(prev)),
    // and prev->state = RUNNING.
    //
    // P2-Cf: clear PREV's on_cpu now that cpu_switch_context completed.
    // Mirrors sched()'s C-side resume path. Skips lock release because
    // thread_switch never took the per-CPU run-tree lock; the helper
    // sched_finish_task_switch's NULL-guard handles that.
    sched_finish_task_switch();
}

u64 thread_total_created(void)   { return __atomic_load_n(&g_thread_created, __ATOMIC_RELAXED); }
u64 thread_total_destroyed(void) { return __atomic_load_n(&g_thread_destroyed, __ATOMIC_RELAXED); }
