// Process descriptor management (P2-A) + rfork/exits/wait lifecycle (P2-D).
//
// Per ARCHITECTURE.md §7.2 + §7.4 + §7.9. P2-A established the bare
// kproc + proc_alloc/proc_free + monotonic PID assignment. P2-D adds
// the multi-process lifecycle: rfork creates a new Proc with one
// initial Thread; exits transitions Proc to ZOMBIE; wait_pid reaps
// zombie children.
//
// At v1.0 P2-D, only RFPROC is supported in rfork — namespace, fd
// table, address space, credentials, etc. land in subsequent P2 sub-
// chunks. Multi-thread Procs and the Linux clone() flag translation
// land at Phase 5+ with the syscall surface.
//
// Bootstrap order (kernel/main.c calls in this order):
//   1. slub_init      — kmem_cache_create can allocate now
//   2. proc_init      — kproc (PID 0) appears
//   3. thread_init    — kthread (TID 0) appears, parented to kproc

#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/page.h>
#include <thylacine/pgrp.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../arch/arm64/asid.h"
#include "../arch/arm64/mmu.h"
#include "../mm/slub.h"

static struct kmem_cache *g_proc_cache;
static struct Proc       *g_kproc;
// R6-A F107: u32 (was int). Signed-overflow on `int` atomic_fetch_add at
// INT_MAX is UB per C11 5.1.2.4; u32 has defined modular wrap. Cast to
// int at p->pid assignment with an INT_MAX guard so the public PID type
// stays signed (sentinel -1 for errors). v1.0 doesn't approach overflow
// (test PIDs reach low thousands), but the discipline future-proofs
// against long-running systems and uniformly with R5-H F90's u32-wrap-
// correct-but-type-fragile concern for g_try_steal_rotate.
static u32                g_next_pid = 0;
static u64                g_proc_created;
static u64                g_proc_destroyed;

// P3-A (R5-H F75 close): global proc-table lock guarding the Proc-
// lineage state machine on SMP.
//
// PROTECTS:
//   * Each Proc's children list head + per-child sibling chain.
//   * Each Proc's `parent` pointer.
//   * Each Proc's `state` transitions ALIVE → ZOMBIE.
//   * Each Proc's `exit_msg` / `exit_status` mutations.
//   * The companion Thread's transition to THREAD_EXITING in `exits()`.
//
// LOCK ORDER:
//
//   `exits()` holds proc_table_lock through the wakeup of parent's
//   child_done — the wakeup acquires `r->lock` while proc_table_lock is
//   still held. Order: proc_table_lock → r->lock.
//
//   This bracketing is REQUIRED to defeat a self-audit-found race:
//   without it, between exits's release of proc_table_lock and exits's
//   call to wakeup(parent_to_wake->child_done), the parent could be
//   reaped + freed by the grandparent's wait_pid (which also takes
//   proc_table_lock — but acquires AFTER our release, blocking us not
//   at all). Holding proc_table_lock through wakeup ensures the parent
//   stays alive until our wakeup completes.
//
//   For the order to be deadlock-free, NO PATH may hold `r->lock` while
//   acquiring proc_table_lock. `wait_pid_cond` is THE candidate — it's
//   called from `sleep` under `r->lock`. The discipline at P3-A:
//   `wait_pid_cond` reads the children list WITHOUT proc_table_lock,
//   relying on three observations:
//
//     (1) The parent's children list head + sibling chain is single-
//         writer per non-kproc parent at v1.0: only the parent's own
//         thread mutates it (via its own rfork / wait_pid / exits-
//         reparent). Single-thread Procs at v1.0; the parent's own
//         thread is the only modifier. When the parent is in wait_pid
//         (calling sleep, which calls wait_pid_cond), it is NOT
//         concurrently in exits or rfork.
//
//         **kproc EXCEPTION (R6-A F105)**: `g_kproc->children` IS
//         multi-writer — every exiting non-kproc Proc with orphan
//         children calls `proc_reparent_children(p)` which mutates
//         `g_kproc->children` (sets `c->sibling = g_kproc->children;
//         g_kproc->children = c`). The mutators serialize via
//         proc_table_lock, so each mutation is internally atomic, but
//         a kthread (kproc's own thread) walking lockless in
//         wait_pid_cond CAN observe an interleaving where a non-kproc
//         Proc's reparent is mid-execution.
//
//         At v1.0 P3-A this is QUIESCENT: no test creates orphan
//         grandchildren (every test reaps grandchildren before parent
//         exits, so `proc_reparent_children` always runs with empty
//         `p->children`, never mutating kproc's list). Concrete
//         consequences IF a future test breaks this: walker could miss
//         a child mid-insert and return "no zombie" → benign sleep
//         until next wakeup. No UAF possible because kproc never reaps,
//         so children of kproc are never freed.
//
//         Phase 5+ (when concurrent parent+child exits become routine
//         OR a kthread reaper for kproc's adopted orphans lands), this
//         walker MUST acquire proc_table_lock (or the mutators MUST
//         use atomic stores so the walker is structurally safe with
//         atomic loads). Trip-hazard documented in handoffs/015 +
//         phase3-status.md + reference/14-process-model.md.
//
//     (2) Per-child `state` is mutated by the child's own exits under
//         proc_table_lock + observed via the wakeup→sleep handshake's
//         release/acquire on `r->lock`. Plain reads in wait_pid_cond
//         see post-wakeup state because `r->lock`'s acquire pairs with
//         the child's release on `r->lock` from its wakeup, and the
//         child's wakeup happens AFTER the child's proc_table_lock-
//         held state mutation. Acquire/release transitivity covers the
//         visibility — for any child that has called wakeup, the first
//         cond check ALSO sees the post-wakeup state (no "stale state"
//         window in practice; see (3)).
//
//     (3) Defense-in-depth: even if (2)'s release/acquire chain didn't
//         cover the first cond check (e.g., if a child has not yet
//         called wakeup), the cond loop's structure tolerates a stale
//         read: if no zombie is visible, sleep; the next wakeup re-
//         evaluates with fresh visibility via (2). At v1.0 P3-A (2)
//         actually covers the first call because parent's program-
//         ordered writes (rfork) are visible to its own thread, and any
//         child's wakeup release pairs with parent's first r->lock
//         acquire. (3) is a defensive re-statement, not the primary
//         correctness mechanism.
//
//   When v1.0 lifts to multi-thread Procs (Phase 5+), assumption (1)
//   weakens for ALL parents (not just kproc): a sibling thread of the
//   parent could mutate the list concurrently. wait_pid_cond will need
//   to acquire proc_table_lock then, AND the sleep protocol will need
//   refactoring to avoid the r->lock → proc_table_lock nesting that
//   re-introduces the cycle. Documented as a Phase 5+ trip-hazard.
//
// CASCADING-EXITS RACE (R5-H F75):
//   Without this lock, parent A's `proc_reparent_children` walk could
//   rewrite child B's `parent` pointer concurrently with B's `exits()`
//   reading the same field at `wakeup(&p->parent->child_done)`. If B
//   holds the stale (pre-reparent) pointer in a register and A's
//   subsequent ZOMBIE + parent-wakeup chain causes A to be reaped +
//   freed before B's wakeup line fires, B accesses freed-A → UAF on the
//   Rendez. This lock serializes A's mutation with B's read; the
//   wakeup-inside-lock structure additionally prevents the
//   reaped-between-release-and-wakeup variant.
//
// SPIN_LOCK_IRQSAVE used uniformly: at v1.0 P3-A, no IRQ handler
// modifies Proc lineage state, but the discipline future-proofs against
// notes/signals (Phase 5+) that may surface from IRQ context.
static spin_lock_t g_proc_table_lock = SPIN_LOCK_INIT;

// Initialize a freshly-allocated Proc descriptor. Caller has already
// passed KP_ZERO to kmem_cache_alloc, so all fields are zero/NULL on
// entry; this only sets the non-zero-default values.
static void proc_init_fields(struct Proc *p, int pid) {
    p->magic = PROC_MAGIC;
    p->pid   = pid;
    p->state = PROC_STATE_ALIVE;
    rendez_init(&p->child_done);
    // P3-Bcb: pgtable_root + asid left at 0 by KP_ZERO. proc_alloc
    // (post-phys_init) installs real values; proc_init (kproc, pre-
    // phys_init) leaves them at 0 — kproc never enters EL0 and never
    // needs a user-half page table or a non-kernel ASID.
    // parent / children / sibling / exit_status / exit_msg / threads /
    // pgrp all left NULL/0 via KP_ZERO; caller (proc_init for kproc;
    // rfork for child Procs) wires linkage explicitly.
}

void proc_init(void) {
    if (g_proc_cache) extinction("proc_init called twice");
    if (!kpgrp())     extinction("proc_init before pgrp_init "
                                 "(kproc needs a pgrp at allocation)");

    g_proc_cache = kmem_cache_create("proc",
                                     sizeof(struct Proc),
                                     8,
                                     KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_proc_cache) {
        extinction("kmem_cache_create(proc) returned NULL");
    }

    g_kproc = kmem_cache_alloc(g_proc_cache, KP_ZERO);
    if (!g_kproc) extinction("kmem_cache_alloc(kproc) failed");
    proc_init_fields(g_kproc, 0);
    g_kproc->pgrp = kpgrp();

    // P2-Fc: kproc gets its own handle table. handle_init must run
    // before proc_init (main.c bootstrap order). Failures here panic —
    // boot can't continue without kproc.
    g_kproc->handles = handle_table_alloc();
    if (!g_kproc->handles) extinction("handle_table_alloc(kproc) failed");

    // R5-H F79: atomic counter — boot-time bump is single-CPU but the
    // load side (proc_total_created) may be observed from secondary CPUs
    // post-bring-up; using atomic ops uniformly avoids torn-read hazards.
    __atomic_fetch_add(&g_proc_created, 1u, __ATOMIC_RELAXED);
    g_next_pid = 1u;       // R6-A F107: u32 (was int).
}

struct Proc *kproc(void) {
    return g_kproc;
}

struct Proc *proc_alloc(void) {
    if (!g_proc_cache) extinction("proc_alloc before proc_init");
    struct Proc *p = kmem_cache_alloc(g_proc_cache, KP_ZERO);
    if (!p) return NULL;

    // R5-H F89: PID consumption is deferred until ALL alloc steps
    // succeed. The previous pattern (proc_init_fields(p, g_next_pid++)
    // before handle_table_alloc) advanced the PID even on rollback,
    // permanently sparsifying the PID space. Now: assign the PID only
    // after handle_table_alloc returns non-NULL.
    proc_init_fields(p, 0);

    // R5-F F50/F53 close: count this Proc as created BEFORE any failure
    // path that might tear it down via proc_free. proc_free does its
    // own destroyed++ — this hoist keeps the counters balanced even
    // when rollback fires (created+1 / destroyed+1 net zero).
    // R5-H F79: atomic counter bump (was non-atomic ++).
    __atomic_fetch_add(&g_proc_created, 1u, __ATOMIC_RELAXED);

    // P2-Fc: each Proc gets its own handle table. On OOM (now reachable
    // post-R5-F F50; PANIC_ON_FAIL dropped from g_handle_table_cache),
    // delegate cleanup to proc_free — which is future-proof against
    // struct Proc growth (R5-F F53). The Proc has no pgrp / threads /
    // children yet (KP_ZERO), so proc_free's pgrp_unref(NULL) +
    // handle_table_free(NULL) + lifecycle gates all no-op cleanly.
    p->handles = handle_table_alloc();
    if (!p->handles) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);              // does its own g_proc_destroyed++
        return NULL;
    }

    // P3-Bcb: per-Proc page-table root + ASID. The pgtable_root is a
    // fresh L0 (KP_ZERO; all 512 entries invalid). The asid is taken
    // from the asid_alloc free-list / monotonic counter. Both are
    // installed in TTBR0_EL1 at context switch (P3-Bd) so each Proc's
    // user-half is independent.
    //
    // Order: pgtable_create FIRST (more failure-prone — buddy may be
    // exhausted), asid_alloc SECOND. Rollback flow on either failure
    // transitions to ZOMBIE and calls proc_free, which idempotently
    // releases pgtable (root != 0) + asid (asid != 0).
    p->pgtable_root = proc_pgtable_create();
    if (p->pgtable_root == 0) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);
        return NULL;
    }
    p->asid = asid_alloc();      // extincts on exhaustion (no rollback path)

    // F89: assign the real PID now that handle table is live. This is
    // the only path that exposes a Proc to the world; subsequent rollback
    // (e.g., pgrp_clone or thread_create_with_arg failure in rfork) frees
    // a Proc with the assigned PID via proc_free, which doesn't unwind
    // PID consumption — but the destroyed++ counter still balances.
    //
    // P3-A self-audit: atomic fetch-add. Cascading rforks on SMP (P3-A
    // test_proc_cascading_rfork_stress exercises this) can have multiple
    // Procs in proc_alloc concurrently from different CPUs; non-atomic
    // ++ would let two Procs collide on the same PID. RELAXED ordering
    // suffices — PID assignment is monotonic but not synchronized with
    // other state.
    //
    // R6-A F107: u32 atomic + INT_MAX guard. Signed-overflow on int
    // atomic_fetch_add at INT_MAX is UB; u32 has defined wrap. The
    // public PID type stays signed (`int`) so -1 remains the error
    // sentinel; the cast at assignment is well-defined for u32 < 2^31.
    // The INT_MAX guard extincts loudly on the (unreachable at v1.0)
    // overflow rather than silently producing a negative PID that aliases
    // the error sentinel.
    u32 next = __atomic_fetch_add(&g_next_pid, 1u, __ATOMIC_RELAXED);
    if (next >= 0x7fffffffu) extinction("proc_alloc: g_next_pid would overflow INT_MAX");
    p->pid = (int)next;

    return p;
}

void proc_free(struct Proc *p) {
    if (!p)                       extinction("proc_free(NULL)");
    // Magic check catches double-free and corrupt-Proc passes. SLUB's
    // freelist write at kmem_cache_free clobbers magic; subsequent free
    // reads the clobbered value and trips here.
    if (p->magic != PROC_MAGIC)   extinction("proc_free of corrupted or already-freed Proc");
    if (p == g_kproc)             extinction("proc_free attempted on kproc");
    if (p->thread_count)          extinction("proc_free with live threads (caller must drain)");
    if (p->threads)               extinction("proc_free with non-NULL threads list");
    if (p->children)              extinction("proc_free with live children (caller must reap or re-parent)");
    // P2-D: state must be ZOMBIE (came through exits) — no other path
    // legitimately reaches proc_free in the lifecycle. ALIVE means we
    // forgot exits; INVALID means we're freeing an uninitialized Proc.
    if (p->state != PROC_STATE_ZOMBIE)
        extinction("proc_free of non-ZOMBIE Proc (lifecycle violation)");

    // P3-Da: drain VMAs first. Each Vma carries a vmo_unmap; releasing
    // them BEFORE handle_table_free is the right order — handle closure
    // independently does vmo_unref (handle_count--) and a VMO with
    // mapping_count > 0 must NOT free even if handle_count drops to 0
    // (per specs/vmo.tla NoUseAfterFree).
    vma_drain(p);

    // P2-Eb: release the namespace. Most Procs have a private pgrp
    // (refcount 1; freed here). Phase 5+ shared namespaces decrement
    // refcount and free only at last release.
    pgrp_unref(p->pgrp);
    p->pgrp = NULL;

    // P2-Fc: release the handle table. Closes any in-use slots first
    // (defensive — well-behaved Procs close all handles before exits;
    // but a Proc that crashed mid-session leaves stragglers).
    handle_table_free(p->handles);
    p->handles = NULL;

    // P3-Bcb: release per-Proc address space resources. Both calls are
    // idempotent on 0: kproc + rolled-back-pre-pgtable_create paths
    // hit no-ops cleanly. asid_free issues an inner-shareable broadcast
    // TLB-flush by ASID before returning the slot to the pool (so the
    // next reuser sees a clean TLB).
    if (p->pgtable_root != 0) {
        proc_pgtable_destroy(p->pgtable_root);
        p->pgtable_root = 0;
    }
    if (p->asid != 0) {                // ASID 0 is kernel-reserved
        asid_free(p->asid);
        p->asid = 0;
    }

    kmem_cache_free(g_proc_cache, p);
    // R5-H F79: atomic counter bump.
    __atomic_fetch_add(&g_proc_destroyed, 1u, __ATOMIC_RELAXED);
}

u64 proc_total_created(void)   { return __atomic_load_n(&g_proc_created, __ATOMIC_RELAXED); }
u64 proc_total_destroyed(void) { return __atomic_load_n(&g_proc_destroyed, __ATOMIC_RELAXED); }

// =============================================================================
// P2-D: rfork / exits / wait_pid
// =============================================================================

// Link a child Proc onto a parent's children list (head insertion).
//
// PRECONDITION (P3-A, R5-H F75 close): caller must hold
// `g_proc_table_lock`. This function mutates `parent->children`,
// `child->parent`, and `child->sibling` — all guarded fields.
static void proc_link_child(struct Proc *parent, struct Proc *child) {
    child->parent  = parent;
    child->sibling = parent->children;
    parent->children = child;
}

// Unlink a child from its parent's children list. The child's state is
// expected to be ZOMBIE (post-exits); after unlink, caller proc_frees
// the child.
//
// PRECONDITION (P3-A): caller must hold `g_proc_table_lock`.
static void proc_unlink_child(struct Proc *parent, struct Proc *child) {
    if (parent->children == child) {
        parent->children = child->sibling;
    } else {
        struct Proc *prev = parent->children;
        while (prev && prev->sibling != child) prev = prev->sibling;
        if (!prev) extinction("proc_unlink_child: child not in parent's list");
        prev->sibling = child->sibling;
    }
    child->sibling = NULL;
    child->parent  = NULL;
}

// Re-parent a Proc's children to kproc on exit. At Phase 5+ this targets
// init (PID 1); at v1.0 there's no init yet so kproc adopts orphans.
// kproc never calls wait_pid, so adopted orphans become permanent
// zombies — acceptable at v1.0 because the only test scenarios that
// exercise exits don't have orphan grandchildren. Phase 2 close adds
// a kthread reaper or moves to PID 1.
//
// PRECONDITION (P3-A, R5-H F75 close): caller must hold
// `g_proc_table_lock`. This function rewrites every reparented child's
// `parent` and `sibling` pointers — the exact mutations the F75 race
// targeted. Holding the lock here closes the race against any child's
// concurrent `exits()` that reads `p->parent`.
static void proc_reparent_children(struct Proc *p) {
    while (p->children) {
        struct Proc *c = p->children;
        p->children = c->sibling;
        c->parent = g_kproc;
        c->sibling = g_kproc->children;
        g_kproc->children = c;
    }
}

int rfork(unsigned flags, void (*entry)(void *), void *arg) {
    // P2-D: only RFPROC supported. Other flags reserved for subsequent
    // sub-chunks (RFNAMEG at P2-E, RFFDG at P2-F, RFMEM at P2-G, etc.).
    if (flags != RFPROC) {
        extinction("rfork: only RFPROC supported at P2-D");
    }
    if (!entry) extinction("rfork with NULL entry");

    struct Thread *t = current_thread();
    if (!t)                  extinction("rfork with no current thread");
    if (t->magic != THREAD_MAGIC)
                             extinction("rfork from corrupted current thread");
    struct Proc *parent = t->proc;
    if (!parent)             extinction("rfork from thread with no proc");
    if (parent->magic != PROC_MAGIC)
                             extinction("rfork from thread with corrupted proc");

    struct Proc *child = proc_alloc();
    if (!child) return -1;

    // P2-Eb: clone parent's namespace into the child. Maps to the spec's
    // ForkClone action — child gets a deep copy of parent's bindings;
    // subsequent bind/unmount on either is independent. RFNAMEG (shared
    // namespace) is unsupported at v1.0; the parent ALWAYS gets a clone.
    struct Pgrp *child_pgrp = pgrp_clone(parent->pgrp);
    if (!child_pgrp) {
        child->state = PROC_STATE_ZOMBIE;
        // child->pgrp is still NULL; proc_free's pgrp_unref(NULL) is a no-op.
        proc_free(child);
        return -1;
    }
    child->pgrp = child_pgrp;

    struct Thread *ct = thread_create_with_arg(child, entry, arg);
    if (!ct) {
        // Roll back proc_alloc + pgrp_clone. Transition to ZOMBIE so
        // proc_free's lifecycle gate passes; proc_free will pgrp_unref
        // the just-allocated pgrp.
        child->state = PROC_STATE_ZOMBIE;
        proc_free(child);
        return -1;
    }

    // P3-A: link child into parent's children list under the proc-table
    // lock. This is the publication point — after release, the child is
    // visible to any concurrent exits()/wait_pid() on the parent.
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    proc_link_child(parent, child);
    spin_unlock_irqrestore(&g_proc_table_lock, s);

    // Insert into the local CPU's run tree. ready() handles the state
    // transition (RUNNABLE → in-runtree). thread_create already set
    // state = THREAD_RUNNABLE. ready() takes its own per-CPU lock; lock
    // ordering is irrelevant here because we've already released
    // g_proc_table_lock.
    ready(ct);

    return child->pid;
}

void exits(const char *msg) {
    struct Thread *t = current_thread();
    if (!t)                  extinction("exits with no current thread");
    if (t->magic != THREAD_MAGIC)
                             extinction("exits from corrupted current thread");

    struct Proc *p = t->proc;
    if (!p)                  extinction("exits from thread with no proc");
    if (p->magic != PROC_MAGIC)
                             extinction("exits from thread with corrupted proc");
    if (p == g_kproc)        extinction("exits from kproc (boot thread)");
    if (p->state != PROC_STATE_ALIVE)
                             extinction("exits from non-ALIVE proc (double exits?)");
    // v1.0 P2-D: single-thread Procs (rfork creates 1 thread). Multi-
    // threaded exits requires terminating all sibling threads (Phase 5+
    // via cross-CPU IPI to halt + reap). Guard so we surface the limit
    // explicitly.
    if (p->thread_count != 1)
        extinction("exits with thread_count != 1 (multi-thread Procs not supported at P2-D)");

    // P3-A (R5-H F75 close): all lineage mutations + parent wakeup
    // happen UNDER g_proc_table_lock atomically. The previous code did
    // these without synchronization, allowing a parallel parent's
    // proc_reparent_children to rewrite p->parent between our read and
    // use of it for wakeup. Holding proc_table_lock through the wakeup
    // additionally prevents a self-audit-found variant: between lock
    // release and wakeup, the parent could be reaped + freed by the
    // grandparent's wait_pid; our wakeup would access freed memory.
    // Holding the lock through wakeup ensures the parent stays alive.
    //
    // Lock order: proc_table_lock → r->lock (the rendez lock acquired
    // inside wakeup). Sound iff no path holds r->lock and tries to
    // acquire proc_table_lock — at P3-A, wait_pid_cond is the only
    // r->lock holder, and it does NOT acquire proc_table_lock (per
    // header comment, single-thread-Proc invariant + wakeup-acquire
    // visibility chain).
    //
    // t->state = THREAD_EXITING also under lock so wait_pid's reap-path
    // observes consistent (p->state == ZOMBIE AND ct->state == EXITING).
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);

    // Re-parent any orphan children to kproc before transitioning.
    if (p->children) {
        proc_reparent_children(p);
    }

    // Capture exit status. At v1.0 the convention is "ok" = clean
    // exit (status 0); anything else = error (status 1). msg is
    // captured by reference; caller-owned (typically a string literal).
    p->exit_msg    = msg ? msg : "ok";
    p->exit_status = (msg && msg[0] == 'o' && msg[1] == 'k' && msg[2] == 0) ? 0 : 1;
    p->state       = PROC_STATE_ZOMBIE;

    // Mark the executing thread EXITING so sched() leaves it out of the
    // run tree (it will be reaped by the parent's wait_pid).
    t->state = THREAD_EXITING;

    // Wake parent's child_done Rendez UNDER proc_table_lock. The lock
    // bracketing keeps p->parent alive through the wakeup (lock release
    // happens AFTER wakeup returns). wakeup() acquires r->lock; lock
    // order proc_table_lock → r->lock established here.
    if (p->parent) {
        wakeup(&p->parent->child_done);
    }

    spin_unlock_irqrestore(&g_proc_table_lock, s);

    // Yield. Will not return — we're EXITING, sched() doesn't re-insert,
    // and there's no future wake target for us.
    sched();
    extinction("exits: returned from sched (impossible)");
}

// cond predicate for wait_pid's sleep: any child in ZOMBIE state, OR
// the parent has no children at all (-1 return path).
//
// CALLED FROM `sleep` UNDER r->lock. P3-A: deliberately does NOT
// acquire g_proc_table_lock — that would create a r->lock →
// g_proc_table_lock nesting that, combined with exits's
// g_proc_table_lock → r->lock-via-wakeup discipline, deadlocks.
//
// Soundness rests on the three invariants documented in detail at
// `g_proc_table_lock`'s declaration block above. Briefly:
//
//   1. Single-writer children list (per non-kproc parent at v1.0).
//      **kproc EXCEPTION (R6-A F105)**: g_kproc->children IS multi-
//      writer (any exiting Proc with orphan children writes here under
//      proc_table_lock). At v1.0 P3-A this is quiescent because no
//      test creates orphan grandchildren. Phase 5+ (or any future test
//      with cascading-exits + non-empty-grandchildren) MUST refactor
//      this walker to take proc_table_lock or use atomic loads.
//
//   2. Per-child `state` visibility via the wakeup→sleep handshake's
//      release/acquire on r->lock.
//
//   3. Defense-in-depth: stale-read tolerance via the cond loop's
//      re-evaluation under (2)'s chain on each subsequent wakeup.
//
// See g_proc_table_lock's header for the full chain.
static int wait_pid_cond(void *arg) {
    struct Proc *parent = arg;
    if (!parent->children) return 1;          // no children → wake to return -1
    for (struct Proc *c = parent->children; c; c = c->sibling) {
        if (c->state == PROC_STATE_ZOMBIE) return 1;
    }
    return 0;
}

int wait_pid(int *status_out) {
    struct Thread *t = current_thread();
    if (!t)                  extinction("wait_pid with no current thread");
    struct Proc *p = t->proc;
    if (!p)                  extinction("wait_pid with no proc");
    if (p->magic != PROC_MAGIC)
                             extinction("wait_pid with corrupted proc");

    for (;;) {
        // P3-A: walk + unlink + state capture under g_proc_table_lock.
        // Atomic with concurrent exits() on our children — they hold the
        // same lock during their ZOMBIE transition, so we either see
        // the pre-transition state (no zombie yet → sleep) or the post-
        // transition state (zombie ready to reap).
        irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);

        if (!p->children) {
            // No children at all (live or zombie). Nothing to wait for.
            spin_unlock_irqrestore(&g_proc_table_lock, s);
            return -1;
        }

        struct Proc *zombie = NULL;
        for (struct Proc *c = p->children; c; c = c->sibling) {
            if (c->state == PROC_STATE_ZOMBIE) {
                zombie = c;
                break;
            }
        }

        if (zombie) {
            int pid    = zombie->pid;
            int status = zombie->exit_status;

            // Capture the (single) thread to reap. Sanity-check state
            // INSIDE the lock — exits() set ct->state = EXITING under
            // the same lock, so this is a consistent observation.
            struct Thread *ct = zombie->threads;
            if (!ct) {
                spin_unlock_irqrestore(&g_proc_table_lock, s);
                extinction("wait_pid: zombie with no threads");
            }
            if (ct->state != THREAD_EXITING) {
                spin_unlock_irqrestore(&g_proc_table_lock, s);
                extinction("wait_pid: zombie thread not in EXITING state");
            }

            proc_unlink_child(p, zombie);

            spin_unlock_irqrestore(&g_proc_table_lock, s);

            // Outside the lock: spin on on_cpu, then free thread + Proc.
            // We released the lock to avoid holding it across thread_free's
            // multi-CPU run-tree walk (which acquires every CPU's
            // cs->lock). Lock order would be: g_proc_table_lock → cs->lock.
            // No reverse exists (sched/ready/wakeup never touch lineage
            // state), so holding both would be safe — but releasing
            // first reduces lock-hold time and avoids a long-tail latency
            // contributor.
            //
            // P2-Dd-pre on_cpu spin: exits() set state=EXITING + called
            // sched(); the destination CPU's resume code clears
            // ct->on_cpu via cs->prev_to_clear_on_cpu. Without this
            // spin, thread_free could race with the destination CPU
            // still mid-switch (TPIDR_EL1 briefly points at ct).
            // Mirrors the on_cpu spin in wakeup() (P2-Cf).
            while (__atomic_load_n(&ct->on_cpu, __ATOMIC_ACQUIRE)) {
                __asm__ __volatile__("yield" ::: "memory");
            }

            thread_free(ct);
            // thread_free unlinks ct from zombie->threads + decrements
            // thread_count, so by here zombie has thread_count==0 and
            // threads==NULL — proc_free's preconditions are met.

            proc_free(zombie);

            if (status_out) *status_out = status;
            return pid;
        }

        // No zombie yet, but live children exist. Release the lock and
        // sleep on child_done. exits() in any of our children will
        // wakeup this Rendez. The cond predicate re-evaluates "any
        // zombie? or no children?" under r->lock + g_proc_table_lock;
        // sleep is atomic with the cond check (see scheduler.tla
        // NoMissedWakeup proof).
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        sleep(&p->child_done, wait_pid_cond, p);
    }
}
