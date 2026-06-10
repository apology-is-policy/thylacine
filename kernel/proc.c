// Process descriptor management (P2-A) + rfork/exits/wait lifecycle (P2-D).
//
// Per ARCHITECTURE.md §7.2 + §7.4 + §7.9. P2-A established the bare
// kproc + proc_alloc/proc_free + monotonic PID assignment. P2-D adds
// the multi-process lifecycle: rfork creates a new Proc with one
// initial Thread; exits transitions Proc to ZOMBIE; wait_pid reaps
// zombie children.
//
// At v1.0 P2-D, only RFPROC is supported in rfork — territory, fd
// table, address space, credentials, etc. land in subsequent P2 sub-
// chunks. Multi-thread Procs and the Linux clone() flag translation
// land at Phase 5+ with the syscall surface.
//
// Bootstrap order (kernel/main.c calls in this order):
//   1. slub_init      — kmem_cache_create can allocate now
//   2. proc_init      — kproc (PID 0) appears
//   3. thread_init    — kthread (TID 0) appears, parented to kproc

#include <thylacine/caps.h>
#include <thylacine/devcap.h>
#include <thylacine/devsrv.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/notes.h>
#include <thylacine/page.h>
#include <thylacine/territory.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/torpor.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../arch/arm64/mmu.h"
#include "../arch/arm64/timer.h"   // timer_now_ns (A-4a legate valid_until expiry)
#include "../arch/arm64/uaccess.h"
#include "../arch/arm64/uart.h"
#include "../mm/slub.h"

static struct kmem_cache *g_proc_cache;
static struct Proc       *g_kproc;
// 2B-F3: init (joey, the first user Proc) -- the orphan-adopter per ARCH
// section 7.9 step 6 ("children re-parented to PID 1 (init)"). NULL until
// joey_thunk publishes it (proc_publish_init); proc_reparent_children
// falls back to kproc while NULL (early boot / in-kernel tests). Written
// ONLY under g_proc_table_lock (publish; the clear in proc_become_zombie_
// locked when init itself dies -- so it never dangles past init's ZOMBIE
// transition, and a post-init-death reparent falls back to kproc instead
// of chaining onto a reapable zombie). Lock-held readers use plain loads;
// the lock-free accessor proc_init_proc() pairs acquire with the
// store-release.
static struct Proc       *g_init_proc;
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

// P5-corvus-srv: monotonic source of per-Proc `stripes` identity tags.
// A u64 — incremented once per Proc creation, it cannot wrap in any
// physically realizable runtime (unlike g_next_pid's u32, which carries
// an explicit INT_MAX guard). Value 0 is never handed out: stripes == 0
// is the reserved fail-closed sentinel. proc_init seeds it to 1.
static u64                g_next_stripes;

// A-4a: monotonic source of legate scope ids (legate_scope_alloc). Like
// g_next_stripes, value 0 is never handed out (0 == not-a-legate). A scope id
// is the teardown-walk match key, so it MUST be unique per legate -- kernel-
// allocated here, never derived from any caller-supplied value. u32 to match
// the struct Proc field; a wrap (after ~4 billion legates) skips 0.
static u32                g_next_legate_scope;

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
//         **ADOPTER EXCEPTION (R6-A F105; widened by 2B-F3)**: the
//         orphan-adopter's children list (`g_init_proc->children` when
//         init is up, else `g_kproc->children`) IS multi-writer — every
//         exiting Proc with orphan children calls
//         `proc_reparent_children(p)` which head-inserts into the
//         adopter's list (sets `c->sibling = adopter->children;
//         adopter->children = c`). The mutators serialize via
//         proc_table_lock, but the adopter's own thread walking
//         lockless in wait_pid_cond CAN observe a mid-insert
//         interleaving (e.g. the new head visible before its sibling
//         store, ARM64 weak ordering).
//
//         Since 2B-F3 this is ROUTINE, not quiescent (orphan adoption
//         is the feature, and the adopter — init/joey — DOES wait_pid).
//         It stays SOUND on two grounds:
//
//           (a) No UAF: a stale walk can only land on the dying
//               parent's remaining orphans (valid, unfreed Procs mid-
//               move) or NULL. Nodes in the adopter's list are
//               unlinked+freed ONLY by the adopter's own wait_pid_for
//               (joey is single-threaded; a multi-thread adopter is
//               serialized by wait_active, RW-2 2B-F1/F2) — never
//               concurrently with that same thread's sleeping cond
//               walk.
//
//           (b) No lost wake: a mid-insert walk can at worst MISS a
//               zombie and sleep. Every zombie-producing event wakes
//               the adopter's child_done — the child's own exits
//               (parent == adopter post-adoption) or, for children
//               adopted already-ZOMBIE, the explicit adopted-zombie
//               wakeup in proc_reparent_children. The wakeup's r->lock
//               release pairs with the sleeper's cond-check acquire,
//               so the re-evaluation sees the consistent post-insert
//               list (the insert precedes the wakeup in the same
//               proc_table_lock critical section).
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
    // P3-Bcb: pgtable_root + context_id left at 0 by KP_ZERO. proc_alloc
    // (post-phys_init) installs a real pgtable_root and leaves context_id 0
    // for the rolling allocator to stamp at first switch; proc_init (kproc,
    // pre-phys_init) leaves both at 0 — kproc never enters EL0 and never
    // needs a user-half page table or a non-kernel ASID.
    // parent / children / sibling / exit_status / exit_msg / threads /
    // territory all left NULL/0 via KP_ZERO; caller (proc_init for kproc;
    // rfork for child Procs) wires linkage explicitly.
}

void proc_init(void) {
    if (g_proc_cache) extinction("proc_init called twice");
    if (!kpgrp())     extinction("proc_init before territory_init "
                                 "(kproc needs a territory at allocation)");

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
    g_kproc->territory = kpgrp();

    // P4-Ib: kproc gets the full capability mask. It's the root of
    // trust: kernel-internal test code (running in kproc context) can
    // create hw handles for testing; rfork'd children inherit CAP_NONE
    // and must be granted caps via a future Phase 5+ capability syscall
    // before they can create hw handles. Maps to specs/handles.tla
    // ProcRoot starting with full caps.
    g_kproc->caps = CAP_ALL;

    // A-1a: kproc is the SYSTEM identity (the boot/kernel-proc principal).
    // It holds caps via the boot chain (CAP_ALL above), NOT via identity
    // (I-22 — identity confers no authority). Every rfork child inherits
    // this until /sbin/login stamps a real user identity, so the whole
    // boot chain (kproc -> joey -> corvus/stratumd) runs as SYSTEM.
    // supp_gid_count stays 0 (KP_ZERO). docs/IDENTITY-DESIGN.md §9.1.
    g_kproc->principal_id = PRINCIPAL_SYSTEM;
    g_kproc->primary_gid  = GID_SYSTEM;

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

    // P5-corvus-srv: seed the stripes counter at 1 (0 is the reserved
    // fail-closed sentinel) and stamp kproc with the first tag. kproc
    // never opens /srv, but a real non-zero `stripes` keeps the tag
    // total over every Proc — no zero-stripes special case to reason
    // about. Drawn from the same counter rfork's children draw from.
    g_next_stripes = 1u;
    g_kproc->stripes = __atomic_fetch_add(&g_next_stripes, 1u, __ATOMIC_RELAXED);
}

struct Proc *kproc(void) {
    return g_kproc;
}

// 2B-F3: publish `p` as init (the orphan-adopter). Called once per boot
// from joey_thunk, in the child's own context before exec -- the same
// stamp-in-own-context pattern as the console-attach bit. Single-publish
// is a v1.0 invariant: joey never exits on success, and a failed joey
// extincts the boot, so no re-publish path exists.
void proc_publish_init(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC)
        extinction("proc_publish_init: NULL or corrupted Proc");
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    bool dup = (g_init_proc != NULL);
    if (!dup)
        __atomic_store_n(&g_init_proc, p, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    if (dup) extinction("proc_publish_init: init already published");
}

// Lock-free accessor (tests, diagnostics). Acquire pairs with the
// publish/clear store-release; NULL means "init not up" (pre-joey boot,
// or init died -- both fall back to kproc as the orphan-adopter).
struct Proc *proc_init_proc(void) {
    return __atomic_load_n(&g_init_proc, __ATOMIC_ACQUIRE);
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
    // struct Proc growth (R5-F F53). The Proc has no territory / threads /
    // children yet (KP_ZERO), so proc_free's territory_unref(NULL) +
    // handle_table_free(NULL) + lifecycle gates all no-op cleanly.
    p->handles = handle_table_alloc();
    if (!p->handles) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);              // does its own g_proc_destroyed++
        return NULL;
    }

    // P3-Bcb / RW-1 B-F1: per-Proc page-table root. A fresh L0 (KP_ZERO; all
    // 512 entries invalid), installed in TTBR0_EL1 at context switch so each
    // Proc's user-half is independent. The ASID is NOT assigned here:
    // context_id stays 0 ("never assigned", KP_ZERO) and the rolling allocator
    // stamps it at the first context switch (asid_resolve, the context-switch
    // pre-hook; ARCH section 6.2.1). There is no ASID-space exhaustion to roll
    // back from -- rollover recycles the space.
    p->pgtable_root = proc_pgtable_create();
    if (p->pgtable_root == 0) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);
        return NULL;
    }

    // P6-pouch-signals-impl: allocate the per-Proc note queue. NULL on OOM
    // routes through the same rollback path as the handle-table / pgtable
    // failure: ZOMBIE + proc_free. proc_free knows to skip notes_queue_free
    // when notes is NULL (no-op safe), so a partially-constructed Proc
    // doesn't double-free.
    p->notes = notes_queue_alloc();
    if (!p->notes) {
        p->state = PROC_STATE_ZOMBIE;
        proc_free(p);
        return NULL;
    }

    // F89: assign the real PID now that handle table is live. This is
    // the only path that exposes a Proc to the world; subsequent rollback
    // (e.g., territory_clone or thread_create_with_arg failure in rfork) frees
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

    // P5-corvus-srv: stamp the per-Proc identity tag. Consumed HERE,
    // alongside the PID — late, after every fallible alloc step has
    // succeeded — so a rolled-back proc_alloc (handle-table / pgtable
    // OOM) never burns a `stripes` value (the R5-H F89 discipline that
    // keeps the PID space dense, applied to the tag space). The tag is
    // immutable hereafter; an rfork child gets its own fresh value via
    // its own proc_alloc, never the parent's (CORVUS-DESIGN.md §6.3).
    p->stripes = __atomic_fetch_add(&g_next_stripes, 1u, __ATOMIC_RELAXED);

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

    // P3-Da: drain VMAs first. Each Vma carries a burrow_unmap; releasing
    // them BEFORE handle_table_free is the right order — handle closure
    // independently does burrow_unref (handle_count--) and a BURROW with
    // mapping_count > 0 must NOT free even if handle_count drops to 0
    // (per specs/burrow.tla NoUseAfterFree).
    vma_drain(p);

    // P2-Eb: release the territory. Most Procs have a private territory
    // (refcount 1; freed here). Phase 5+ shared territories decrement
    // refcount and free only at last release.
    territory_unref(p->territory);
    p->territory = NULL;

    // P2-Fc: release the handle table. #926: normally ALREADY closed at
    // exit (proc_close_handles_at_exit on the exits()/thread_exit_self
    // zombie path NULLs p->handles), so this is a handle_table_free(NULL)
    // no-op for the common case -- a Proc's fds close at process exit, not
    // here at reap. This call remains the fallback for the direct
    // `state=ZOMBIE; proc_free()` paths (orphan re-parent / early-boot
    // alloc rollback) that never run the at-exit close, so p->handles is
    // still set there. The handle-close cascade includes any open devnotes
    // Spoors -- devnotes_close clears Spoor state without touching
    // p->notes, so the queue stays valid for the notes_queue_free below
    // (and the at-exit close preserves the same close-before-free order:
    // exit precedes reap).
    handle_table_free(p->handles);
    p->handles = NULL;

    // P6-pouch-signals-impl: release the note queue. Must run AFTER
    // handle_table_free so any devnotes_close fires before the queue is
    // freed (otherwise a close-side read of q->waiters / q->poll_list
    // would UAF). NULL-tolerant: a Proc that failed allocation pre-notes
    // (e.g., handle_table_alloc OOM rollback) reaches here with notes ==
    // NULL — no-op safe.
    if (p->notes) {
        notes_queue_free(p->notes);
        p->notes = NULL;
    }

    // RW-1 B-F1: release the per-Proc page table. There is NO per-Proc ASID
    // free in the rolling-ASID model -- the Proc's context_id is simply
    // dropped; its hardware ASID value stays reserved in the current
    // generation's bitmap until the next rollover, which reclaims the whole
    // space at once (ARCH section 6.2.1).
    //
    // This is TLB-safe WITHOUT the old asid_free broadcast (the F4
    // asid_free-before-destroy ordering is moot -- there is no asid_free):
    //   - the leaf user mappings were already invalidated by vma_drain's
    //     all-ASID `tlbi vaae1is` (vma_drain runs above, before proc_free);
    //   - no live CPU holds this dead Proc's TTBR0 (every thread was reaped +
    //     on_cpu-spun before proc_free), so no CPU translates under its ASID
    //     and no walk reaches a recently-recycled L1/L2/L3 page;
    //   - any eventual reuse of the ASID value is gated by the rollover's
    //     per-CPU flush_pending local flush before the value goes live again.
    // (Matches the Linux model: no TLB flush at mm teardown; reclaim at
    // rollover.)
    if (p->pgtable_root != 0) {
        proc_pgtable_destroy(p->pgtable_root);
        p->pgtable_root = 0;
    }

    kmem_cache_free(g_proc_cache, p);
    // R5-H F79: atomic counter bump.
    __atomic_fetch_add(&g_proc_destroyed, 1u, __ATOMIC_RELAXED);
}

u64 proc_total_created(void)   { return __atomic_load_n(&g_proc_created, __ATOMIC_RELAXED); }
u64 proc_total_destroyed(void) { return __atomic_load_n(&g_proc_destroyed, __ATOMIC_RELAXED); }

// =============================================================================
// P4-C: Proc lookup for devproc (by pid + iterate-all).
// =============================================================================
//
// Both walkers DFS from kproc through the children/sibling tree. The
// proc-table forms a rooted tree with kproc at the root; every Proc
// alive has a path from kproc through parent pointers (init is a child
// of kproc, and orphans re-parent on exit to init-else-kproc per
// proc_reparent_children — both stay inside the kproc-rooted tree).

// Recursive helper. PRECONDITION: caller holds g_proc_table_lock.
static struct Proc *proc_find_by_pid_walk(struct Proc *root, int pid) {
    if (!root) return NULL;
    if (root->pid == pid) return root;
    for (struct Proc *child = root->children; child; child = child->sibling) {
        struct Proc *r = proc_find_by_pid_walk(child, pid);
        if (r) return r;
    }
    return NULL;
}

struct Proc *proc_find_by_pid(int pid) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *p = proc_find_by_pid_walk(kproc(), pid);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return p;
}

// Recursive iterate. Returns first non-zero callback result; 0 if all
// callbacks returned 0. PRECONDITION: caller holds g_proc_table_lock.
static int proc_for_each_walk(struct Proc *root,
                              int (*cb)(struct Proc *, void *), void *arg) {
    if (!root) return 0;
    int rv = cb(root, arg);
    if (rv) return rv;
    for (struct Proc *child = root->children; child; child = child->sibling) {
        rv = proc_for_each_walk(child, cb, arg);
        if (rv) return rv;
    }
    return 0;
}

int proc_for_each(int (*callback)(struct Proc *p, void *arg), void *arg) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    int rv = proc_for_each_walk(kproc(), callback, arg);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return rv;
}

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

// Re-parent a Proc's children to init (g_init_proc) on exit -- ARCH
// section 7.9 step 6. Fallback to kproc while init is not up (early
// boot before joey exists, the in-kernel test phase, or after init
// itself died): orphans created then must still get a valid parent.
// init (joey) runs a wait-any WNOHANG sweep in its supervisor loop, so
// adopted orphans are reaped instead of leaking as permanent zombies
// (the pre-2B-F3 defect: kproc never calls wait_pid for arbitrary
// orphans, so a kproc-adopted orphan that exited leaked its Proc).
//
// PRECONDITION (P3-A, R5-H F75 close): caller must hold
// `g_proc_table_lock`. This function rewrites every reparented child's
// `parent` and `sibling` pointers — the exact mutations the F75 race
// targeted. Holding the lock here closes the race against any child's
// concurrent `exits()` that reads `p->parent`. The same lock makes the
// g_init_proc read here atomic with the clear in proc_become_zombie_
// locked: a dying init's own reparent runs AFTER its clear (program
// order in the same critical section), so `adopter == p` is
// structurally unreachable -- the check below is belt.
//
// Adopted-ZOMBIE wakeup: a child that is ALREADY a zombie at adoption
// generated its exits()-side wakeup against the OLD parent (now dying),
// so without a fresh wake the adopter could sleep in a blocking
// wait-any over a reapable zombie until some unrelated child exits
// (I-9-shaped lost-wake). Wake the adopter's child_done once if any
// adoptee arrived ZOMBIE. Lock order proc_table_lock -> r->lock is the
// established exits() discipline; the adopter is alive under the lock
// (kproc never dies; a dead init was cleared before this read). No
// child_exit note is re-posted: the note was delivered to the
// then-parent at exit time, and wait_pid re-discovers zombies by
// walking p->children (the documented note-loss recovery).
static void proc_reparent_children(struct Proc *p) {
    struct Proc *adopter = g_init_proc;
    if (!adopter || adopter == p || adopter->state != PROC_STATE_ALIVE)
        adopter = g_kproc;
    bool adopted_zombie = false;
    while (p->children) {
        struct Proc *c = p->children;
        p->children = c->sibling;
        c->parent = adopter;
        c->sibling = adopter->children;
        adopter->children = c;
        if (c->state == PROC_STATE_ZOMBIE)
            adopted_zombie = true;
    }
    if (adopted_zombie)
        wakeup(&adopter->child_done);
}

// Shared internal worker for rfork + rfork_with_caps. The only difference
// between them is `caps_mask`: the child's caps are set to
// `parent->caps & caps_mask` AFTER proc_alloc (which KP_ZEROs caps to
// CAP_NONE). This preserves specs/handles.tla::RforkWithCaps's invariant
// `granted \subseteq proc_caps[parent]` — bit-AND with parent's caps
// ceils the grant at the parent's current caps regardless of what mask
// the caller passes.
//
// P4-Ic3: the rfork_with_caps entry point exposes this kernel-internal
// capability grant path so kproc can spawn a driver Proc holding
// CAP_HW_CREATE (or any subset of kproc's caps). The plain rfork()
// surface delegates with mask=CAP_NONE so children inherit no caps —
// the v1.0 default for any rfork-from-non-kproc-context path that
// hasn't been explicitly designed to grant caps.
static int rfork_internal(unsigned flags, void (*entry)(void *), void *arg,
                          caps_t caps_mask) {
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

    // P4-Ic3: capture parent->caps once under acquire fence. R9 F146
    // applied the acquire-on-read discipline at the syscall layer; the
    // same applies here because the child's caps are bounded by what
    // the parent observably holds NOW. Without the fence the compiler
    // could hoist or split the read, admitting torn intermediate states
    // (mid-ReduceCaps update from another CPU). Maps to spec's
    // `granted \subseteq proc_caps[parent]`: the AND with caps_mask is
    // the impl-side "ceiling at parent's current caps" enforcement.
    //
    // A-4-pre / I-2: AND with ~CAP_ELEVATION_ONLY unconditionally. An
    // elevated parent (one that legitimately gained CAP_HOSTOWNER via the
    // console-gated `cap` device) must not leak it across a fork —
    // elevation-only caps are the sole sanctioned capability growth and
    // flow ONLY through the cap device for a console-attached Proc, never
    // by inheritance. caps_mask alone can't enforce this (a caller may
    // pass a mask that includes the bit); the ~CAP_ELEVATION_ONLY strip
    // is load-bearing. Honors the contract caps.h already documents.
    caps_t parent_caps = __atomic_load_n(&parent->caps, __ATOMIC_ACQUIRE);
    child->caps = (parent_caps & caps_mask) & ~CAP_ELEVATION_ONLY;

    // A-1a: identity is INHERITED across rfork (the durable principal-id +
    // groups flow parent -> child unchanged). This is the opposite of caps
    // (which monotonically reduce) and stripes (fresh per Proc): identity is
    // the stable durable attribute. A plain read suffices — identity is
    // NEVER mutated on a running Proc (the spawn thunk's optional override
    // via proc_apply_identity runs in the CHILD before userland_enter, not
    // on the parent), so the parent's identity is immutable for its life
    // once set. The override (when the parent holds CAP_SET_IDENTITY) lands
    // in sys_spawn_full_argv_thunk, after this inherit and before exec.
    // Inheriting then optionally overriding keeps "set at creation" true:
    // the child never runs userspace under the wrong identity.
    child->principal_id   = parent->principal_id;
    child->primary_gid    = parent->primary_gid;
    // A-1a R1 F2: clamp the inherited count symmetrically with the copy loop
    // below. The loop is already bounded by PROC_SUPP_GIDS_MAX; clamping the
    // stored count too means a (corruption-induced) parent count > 15 can
    // never leave the child with an out-of-bounds count that a downstream
    // consumer (A-2d permission walk) would trust.
    child->supp_gid_count = parent->supp_gid_count > PROC_SUPP_GIDS_MAX
                          ? PROC_SUPP_GIDS_MAX : parent->supp_gid_count;
    for (u8 i = 0; i < child->supp_gid_count; i++)
        child->supp_gids[i] = parent->supp_gids[i];

    // A-4a: inherit the legate scope tag across rfork (IDENTITY-DESIGN
    // §9.8, I-25). A child of a legate-scoped Proc JOINS the scope: it
    // carries scope_id + session_id + valid_until so the teardown walk
    // (A-4a-2b) finds it and it can detect valid_until expiry at its own
    // EL0-return tail. It carries only the FORK-GRANTABLE subset of the
    // caps -- the elevation-only members were already stripped above by
    // `& ~CAP_ELEVATION_ONLY` (A-4-pre), so a scope member cannot wield
    // the legate's fs-admin authority; the membership tag governs lifetime,
    // not authority. PROC_FLAG_LEGATE_ROOT is NOT inherited (proc_flags
    // never are; see below), so the child is a scope MEMBER, never a second
    // root. For a non-legate parent these are all 0 -> child not-a-legate.
    child->legate_scope_id    = parent->legate_scope_id;
    child->legate_session_id  = parent->legate_session_id;
    child->legate_valid_until = parent->legate_valid_until;

    // P5-hostowner-a: child->proc_flags stays 0 (KP_ZERO from
    // proc_alloc) — deliberately NOT copied from the parent. In
    // particular PROC_FLAG_CONSOLE_ATTACHED is never conferred by
    // rfork: console-attachment grows ONLY via an explicit
    // proc_mark_console_attached (specs/corvus.tla — console_attached
    // grows solely via the MarkConsoleAttached action). A child of a
    // console-attached Proc is therefore NOT console-attached unless
    // something marks it explicitly; this is what stops a future
    // remote-login (sshd) chain from inheriting the local-console
    // trust anchor that gates hostowner elevation.

    // P2-Eb + P5-attach-mount: clone parent's territory into the child.
    // Maps to the spec's ForkClone action — child gets a deep copy of
    // parent's bindings AND mounts; each cloned mount entry takes a
    // fresh spoor_ref(source). Subsequent bind/unbind/mount/unmount on
    // either is independent. RFNAMEG (shared territory) is unsupported
    // at v1.0; the parent ALWAYS gets a clone.
    struct Territory *child_pgrp = territory_clone(parent->territory);
    if (!child_pgrp) {
        child->state = PROC_STATE_ZOMBIE;
        // child->territory is still NULL; proc_free's territory_unref(NULL) is a no-op.
        proc_free(child);
        return -1;
    }
    child->territory = child_pgrp;

    struct Thread *ct = thread_create_with_arg(child, entry, arg);
    if (!ct) {
        // Roll back proc_alloc + territory_clone. Transition to ZOMBIE so
        // proc_free's lifecycle gate passes; proc_free will territory_unref
        // the just-allocated territory.
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

int rfork(unsigned flags, void (*entry)(void *), void *arg) {
    return rfork_internal(flags, entry, arg, CAP_NONE);
}

int rfork_with_caps(unsigned flags, void (*entry)(void *), void *arg,
                    caps_t caps_mask) {
    return rfork_internal(flags, entry, arg, caps_mask);
}

// =============================================================================
// P5-hostowner-a: console attachment.
// =============================================================================

void proc_mark_console_attached(struct Proc *p) {
    if (!p)                    extinction("proc_mark_console_attached(NULL)");
    if (p->magic != PROC_MAGIC)
        extinction("proc_mark_console_attached on corrupted Proc");
    // Console-attachment is trust-conferring — it is the gate a future
    // CAP_HOSTOWNER grant checks. Refuse to stamp a Proc that is not
    // ALIVE so a caller bug (marking a dying/zombie descriptor) surfaces
    // loudly rather than silently stamping trust on it.
    if (p->state != PROC_STATE_ALIVE)
        extinction("proc_mark_console_attached on non-ALIVE Proc");
    // Idempotent set. The OR is ATOMIC: A-4c-2 makes PROC_FLAG_CONSOLE_-
    // ATTACHED multi-writer -- the SAK re-grant marks corvus from the
    // console_mgr kthread (a DIFFERENT thread than the marked Proc), and
    // proc_revoke_console_attached clears the same bit from that kthread.
    // So this one bit no longer obeys the v1.0 single-writer proc_flags
    // convention. The OTHER proc_flags bits (MAY_POST_SERVICE / DUMPABLE /
    // TRACEABLE / MLOCKALL) remain set-once before EL0 by the Proc's own
    // thread -- temporally disjoint from any SAK on an already-running
    // owner -- so they do not concurrently RMW this word with the console
    // transitions. RELAXED: the bit carries no ordering dependency (the
    // redeem gate and the SAK both read it as a standalone predicate).
    __atomic_or_fetch(&p->proc_flags, PROC_FLAG_CONSOLE_ATTACHED, __ATOMIC_RELAXED);
}

// Clear PROC_FLAG_CONSOLE_ATTACHED (the unset side proc_console_sak needs).
// Atomic AND -- the bit is multi-writer (see proc_mark_console_attached). A
// no-op on a NULL/corrupt Proc (fail-closed). The caller pins owner lifetime
// (proc_console_sak holds g_proc_table_lock with the owner ALIVE-checked).
void proc_revoke_console_attached(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC) return;
    __atomic_and_fetch(&p->proc_flags, ~PROC_FLAG_CONSOLE_ATTACHED, __ATOMIC_RELAXED);
}

bool proc_is_console_attached(const struct Proc *p) {
    // Fail-closed: a NULL or corrupted Proc reads as NOT console-
    // attached — this query gates hostowner elevation, so the safe
    // default on a bad pointer is "no console, no elevation." The load is
    // ATOMIC: the bit is multi-writer post-A-4c-2 (the SAK kthread mutates
    // it), so a plain read would be a C11 data race.
    if (!p || p->magic != PROC_MAGIC) return false;
    return (__atomic_load_n(&p->proc_flags, __ATOMIC_RELAXED)
            & PROC_FLAG_CONSOLE_ATTACHED) != 0;
}

// =============================================================================
// A-4c-1: the kernel console owner (the trusted-path anchor for /dev/cons).
// =============================================================================
//
// g_console_owner is the single Proc currently holding the console for the
// kernel UART console Dev. Protected by g_proc_table_lock (the proc-lifecycle
// lock) so a reader can deref it without racing the owner's exit/reap. Init
// NULL; joey sets itself the owner at boot (joey_thunk, right after
// proc_mark_console_attached); proc_become_zombie_locked clears it on
// owner-death (every death path -- clean exit / kill / group-terminate). A-4c-2
// adds the SAK revoke/re-grant transitions.
static struct Proc *g_console_owner;   // BSS NULL

// g_console_trusted_proc is the trusted login authority (corvus) -- the target
// the A-4c-2 SAK re-grants the console to. Set when joey establishes corvus
// (SPAWN_PERM_CONSOLE_TRUSTED, applied in the spawn thunk). Same lifetime
// discipline as g_console_owner: protected by g_proc_table_lock, cleared by
// proc_become_zombie_locked on the trusted Proc's death so it never dangles (a
// then-fired SAK falls back to revoke-only -- the security-correct default).
static struct Proc *g_console_trusted_proc;   // BSS NULL

void proc_set_console_owner(struct Proc *p) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    g_console_owner = p;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

void proc_set_console_trusted(struct Proc *p) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    g_console_trusted_proc = p;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// Post the `interrupt` note (Ctrl-C) to the current console owner. Runs in the
// console_mgr kthread's process context. Reads g_console_owner + posts under
// g_proc_table_lock so the owner cannot be reaped/freed mid-post (the A-4b kill
// pattern: hold g_proc_table_lock, post a note; lock order g_proc_table_lock ->
// note q->lock). A no-op when there is no live owner (e.g. after joey exits, or
// a future SAK revoke-only).
void proc_console_post_interrupt(void) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *owner = g_console_owner;
    if (owner && owner->magic == PROC_MAGIC && owner->state == PROC_STATE_ALIVE) {
        notes_post(owner, "interrupt", 0u, NULL, true);
        // LS-5c (P3-terminate): if the post armed the terminate latch (the
        // owner has no handler and is not self-managing -- never the session
        // shell, which is self-managing), wake its blocked threads so the
        // LS-5b terminate fires at their EL0-return tails. g_proc_table_lock
        // is held (this function's lock), satisfying the wake's contract.
        proc_interrupt_terminate_wake(owner);
    }
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// A-5a (I-27 carry): `p` relinquishes its own console-attach. The attach-bit
// clear is the atomic proc_revoke_console_attached; the owner-pointer clear (when
// p IS the owner) takes g_proc_table_lock -- the same lock proc_console_sak +
// proc_set_console_owner hold, so the SAK cannot observe a torn owner/attach
// state. joey calls this at the bringup->session boundary; afterward no Proc is
// console-attached (corvus is g_console_trusted_proc, attached only on SAK), so a
// later SAK leaves corvus the SOLE attached Proc. owner -> NULL pre-SAK drops
// Ctrl-C (no foreground consumer at v1.0). The handler self-restricts (passes
// only the caller's Proc), so this never revokes another Proc.
void proc_console_relinquish(struct Proc *p) {
    if (!p) return;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    proc_revoke_console_attached(p);   // atomic AND on proc_flags
    if (g_console_owner == p) g_console_owner = NULL;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// A-4c-2: the SAK transition (I-27 trusted-path handoff). Called from the
// console_mgr kthread on a recognized serial BREAK. The whole transition runs
// under g_proc_table_lock so the owner + trusted pointers cannot be reaped/freed
// mid-transition (the A-4c-1 console-owner lifetime discipline). Lock order
// g_proc_table_lock -> note q->lock matches proc_console_post_interrupt and the
// proc_become_zombie_locked -> notes_post_child_exit order (audited; acyclic).
void proc_console_sak(void) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *owner   = g_console_owner;
    struct Proc *trusted = g_console_trusted_proc;

    bool trusted_live = trusted && trusted->magic == PROC_MAGIC
                        && trusted->state == PROC_STATE_ALIVE;

    // Idempotent under a BREAK flood: if the trusted login authority already
    // holds the console, the SAK is a no-op -- no spurious revoke / re-grant /
    // note. (This is also the only path on which owner == trusted, so the steps
    // below never revoke-then-re-grant the same Proc.)
    if (trusted_live && owner == trusted) {
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        return;
    }

    // (1) Revoke the console-attach bit from the current owner + post it a
    // notify note (a foreground app learns it lost the console). Guarded on a
    // live owner: after the owner exited, proc_become_zombie_locked already
    // cleared the pointer, so there is nothing to revoke. The note reuses the
    // existing `interrupt` name (the closed notes table -- notes.c -- has no
    // dedicated "console-revoked"/"hangup" name; a distinct name is a v1.x notes
    // SEAM, additive when a consumer needs to tell SAK-revoke from Ctrl-C). At
    // v1.0 the owner is joey, whose fd 0 is a pipe/cap, not /dev/cons -- the note
    // is a courtesy with no behavioral consumer yet.
    if (owner && owner->magic == PROC_MAGIC && owner->state == PROC_STATE_ALIVE) {
        proc_revoke_console_attached(owner);
        notes_post(owner, "interrupt", 0u, NULL, true);
        // LS-5c: same conditional wake as proc_console_post_interrupt -- a
        // blocked non-self-managing revoked owner unwinds + terminates rather
        // than sleeping through the revocation note. g_proc_table_lock held.
        proc_interrupt_terminate_wake(owner);
    }

    // (2) Re-grant the console to the trusted login authority and make it the
    // owner. FAIL-SAFE: with no trusted Proc alive, revoke-only (owner = NULL) --
    // no Proc can redeem CAP_HOSTOWNER / a clearance (the devcap redeem gate keys
    // on PROC_FLAG_CONSOLE_ATTACHED) until a trusted login claims the console.
    if (trusted_live) {
        proc_mark_console_attached(trusted);   // atomic OR; trusted ALIVE-checked
        g_console_owner = trusted;
    } else {
        g_console_owner = NULL;
    }
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// Test-only: read g_console_owner (the SAK-transition target assertion in
// kernel/test/test_cons.c). Externed in the test (the proc_test_link pattern).
struct Proc *proc_test_console_owner(void) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *o = g_console_owner;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return o;
}

// =============================================================================
// P5-corvus-srv-impl-a2: the /srv service-registry post-gate.
// =============================================================================
//
// PROC_FLAG_MAY_POST_SERVICE marks a Proc allowed to register a name in
// the /srv service registry via SYS_POST_SERVICE. joey is the sole
// stamper — it marks the corvus Proc it spawns (CORVUS-DESIGN.md §6.1) —
// so an ordinary Proc cannot post or hijack /srv/corvus, and a
// tombstoned name is re-postable only by a marked Proc. Same one-way
// discipline as console-attachment: kernel-stamped, never cleared, never
// propagated by rfork. specs/corvus.tla pins it as MarkMayPost gating
// PostService.

void proc_mark_may_post_service(struct Proc *p) {
    if (!p)                    extinction("proc_mark_may_post_service(NULL)");
    if (p->magic != PROC_MAGIC)
        extinction("proc_mark_may_post_service on corrupted Proc");
    // The post-gate is trust-conferring — refuse to stamp a dying/zombie
    // descriptor so a caller bug surfaces loudly (mirrors
    // proc_mark_console_attached).
    if (p->state != PROC_STATE_ALIVE)
        extinction("proc_mark_may_post_service on non-ALIVE Proc");
    // One-way, idempotent — never cleared, never propagated by rfork
    // (rfork_internal does not copy proc_flags). The OR is atomic: A-4c-2
    // made PROC_FLAG_CONSOLE_ATTACHED (same word) multi-writer, so every
    // proc_flags RMW is atomic to avoid a torn update clobbering the console
    // bit. (This writer is itself disjoint from the SAK -- it stamps a child
    // in the spawn thunk pre-EL0, never the live console owner -- but the
    // all-RMWs-atomic posture closes the class.)
    __atomic_or_fetch(&p->proc_flags, PROC_FLAG_MAY_POST_SERVICE, __ATOMIC_RELAXED);
}

bool proc_may_post_service(const struct Proc *p) {
    // Fail-closed: a NULL or corrupted Proc reads as NOT permitted — this
    // query gates SYS_POST_SERVICE, so the safe default on a bad pointer
    // is "may not post." Atomic load (the word is multi-writer post-A-4c-2).
    if (!p || p->magic != PROC_MAGIC) return false;
    return (__atomic_load_n(&p->proc_flags, __ATOMIC_RELAXED)
            & PROC_FLAG_MAY_POST_SERVICE) != 0;
}

// LS-5 (P2 default disposition): the self-managing-notes mark. A Proc that
// opens its notes fd declares it consumes its own notes; the uncaught-
// `interrupt` default-terminate exempts it (notes_deliver_at_el0_return).
void proc_mark_self_managing_notes(struct Proc *p) {
    if (!p)                    extinction("proc_mark_self_managing_notes(NULL)");
    if (p->magic != PROC_MAGIC)
        extinction("proc_mark_self_managing_notes on corrupted Proc");
    // The sole caller (sys_note_open_handler) runs on the Proc's own thread
    // mid-syscall, so the Proc is always ALIVE -- a non-ALIVE Proc here is a
    // caller bug; surface it loudly (mirrors proc_mark_may_post_service).
    if (p->state != PROC_STATE_ALIVE)
        extinction("proc_mark_self_managing_notes on non-ALIVE Proc");
    // One-way, idempotent — never cleared, never propagated by rfork
    // (rfork_internal does not copy proc_flags). Atomic OR: the proc_flags
    // word is multi-writer post-A-4c-2 (the SAK kthread mutates the console
    // bit), so every RMW on it must be atomic. RELAXED: the bit is a
    // standalone predicate with no ordering dependency.
    __atomic_or_fetch(&p->proc_flags, PROC_FLAG_SELF_MANAGING_NOTES, __ATOMIC_RELAXED);
}

bool proc_is_self_managing_notes(const struct Proc *p) {
    // Fail-closed: a NULL or corrupted Proc reads as NOT self-managing — the
    // SAFE default, since this query gates the uncaught-interrupt default-
    // terminate (an unverifiable Proc must not dodge it). Atomic load (the
    // word is multi-writer post-A-4c-2).
    if (!p || p->magic != PROC_MAGIC) return false;
    return (__atomic_load_n(&p->proc_flags, __ATOMIC_RELAXED)
            & PROC_FLAG_SELF_MANAGING_NOTES) != 0;
}

bool proc_intr_terminate_pending(const struct Proc *p) {
    // Fail-closed: a NULL or corrupted Proc reads as nothing-pending. Acquire
    // pairs with the release set in notes_post's arm (the latch is read
    // LOCK-FREE by the #811 sleep predicate, thread_die_pending — see the
    // PROC_FLAG_INTR_TERMINATE_PENDING contract in proc.h).
    if (!p || p->magic != PROC_MAGIC) return false;
    return (__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE)
            & PROC_FLAG_INTR_TERMINATE_PENDING) != 0;
}

// LS-5c (P3-terminate, ARCH 8.8.2): wake every blocked Thread of `p` so it
// unwinds (*_INTR) to its EL0-return tail, where the LS-5b uncaught-interrupt
// default-terminate fires against the live queue. The walk is the #811
// universal death-wake template (proc_group_terminate, above the same lock
// contract): the only record of "Thread T sleeps on Rendez R" is the reverse
// pointer t->rendez_blocked_on, read under the peer's wait_lock -- the same
// lock the sleeper's register-then-observe takes, which is the I-9
// serialization -- with wait_lock HELD ACROSS wakeup (Option A) so a torpor
// waiter's stack rendez cannot be popped out from under the waker.
//
// Deliberate deltas from the death template (see the proc.h contract):
// no torpor_wake_all_for_proc (torpor waiters are reachable through
// rendez_blocked_on, and tsleep's widened register-then-observe closes the
// register-after-walk race) and no smp_resched_others (the IRQ-from-EL0 tail
// evaluates only group_exit_msg -- an IPI cannot accelerate a RUNNING
// thread's interrupt-death; task #964 tracks the never-syscalling gap). The
// no-IPI shape also lets the in-kernel unit test drive this REAL waker under
// the deterministic single-CPU harness (the death test must hand-roll the
// cascade to avoid waking idle secondaries).
void proc_interrupt_terminate_wake(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC) return;
    if (p == g_kproc) return;            // belt: the arm never latches kproc
    if (p->state != PROC_STATE_ALIVE) return;
    if (!proc_intr_terminate_pending(p)) return;
    for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
        irq_state_t ws = spin_lock_irqsave(&peer->wait_lock);
        struct Rendez *r = peer->rendez_blocked_on;
        if (r) wakeup(r);
        spin_unlock_irqrestore(&peer->wait_lock, ws);
    }
}

// =============================================================================
// P5-corvus-srv: per-Proc identity tag.
// =============================================================================

u64 proc_stripes(const struct Proc *p) {
    // Fail-closed: a NULL or corrupted Proc reads as `stripes` 0 — the
    // reserved sentinel that authorizes nothing. SYS_SRV_PEER reads a
    // connection peer's identity through here, so a bad pointer must
    // degrade to "no identity," never to a stale or fabricated tag.
    if (!p || p->magic != PROC_MAGIC) return 0;
    return p->stripes;
}

// proc_for_each context for proc_peer_snapshot_by_stripes: match an ALIVE
// Proc by its stripes tag and snapshot the fields a /srv peer query needs.
struct peer_snapshot_ctx {
    u64    stripes;       // IN  — the tag to match
    caps_t caps;          // OUT — the matched Proc's live caps
    u32    principal_id;  // OUT — A-1a: the peer's durable identity
    u32    primary_gid;   // OUT — A-1a: the peer's primary group
    bool   found;         // OUT — set once an ALIVE Proc matched
};

static int peer_snapshot_cb(struct Proc *p, void *arg) {
    struct peer_snapshot_ctx *c = arg;
    if (p->state == PROC_STATE_ALIVE && p->stripes == c->stripes) {
        // caps read ATOMICALLY (RW-5 F2): proc_become_legate is a cross-thread
        // writer of p->caps since A-4a (it does not hold g_proc_table_lock), so
        // even under this locked walk a plain load is C11-racy vs the legate OR.
        c->caps         = __atomic_load_n(&p->caps, __ATOMIC_ACQUIRE);
        c->principal_id = p->principal_id;
        c->primary_gid  = p->primary_gid;
        c->found        = true;
        return 1;                 // first match wins — stop the walk
    }
    return 0;
}

bool proc_peer_snapshot_by_stripes(u64 stripes, caps_t *caps_out,
                                   u32 *principal_out, u32 *primary_gid_out) {
    // 0 is the reserved fail-closed sentinel; no Proc is ever stamped 0,
    // so it can never match. Reject it before the scan. Out-params may be
    // NULL — the caller takes only what it needs.
    if (stripes == 0) return false;

    struct peer_snapshot_ctx ctx = { .stripes      = stripes,
                                     .caps         = 0,
                                     .principal_id = PRINCIPAL_NONE,
                                     .primary_gid  = GID_NONE,
                                     .found        = false };
    // proc_for_each holds g_proc_table_lock across the whole DFS, so the
    // callback's "is this Proc ALIVE" test and its field reads are one
    // snapshot under the lock. Only VALUES escape — never the Proc pointer
    // — so a peer reaped after the scan is not a UAF.
    proc_for_each(peer_snapshot_cb, &ctx);
    if (!ctx.found) return false;
    if (caps_out)        *caps_out        = ctx.caps;
    if (principal_out)   *principal_out   = ctx.principal_id;
    if (primary_gid_out) *primary_gid_out = ctx.primary_gid;
    return true;
}

// proc_caps_by_stripes — caps-only wrapper over the richer snapshot. Keeps
// the existing SYS_SRV_PEER + spec-mapped API (and its NULL-out rejection;
// specs/corvus.tla ConnOpPeerWasLive) unchanged for current callers.
bool proc_caps_by_stripes(u64 stripes, caps_t *caps_out) {
    if (!caps_out) return false;
    return proc_peer_snapshot_by_stripes(stripes, caps_out, NULL, NULL);
}

// A-1a: proc_apply_identity — the single audited identity mutation site.
// See <thylacine/proc.h> for the full contract. Called from the spawn
// thunk in the CHILD's context before userland_enter, only after the
// parent verified CAP_SET_IDENTITY + value bounds.
void proc_apply_identity(struct Proc *p, u32 principal_id, u32 primary_gid,
                         const u32 *supp_gids, u8 supp_gid_count) {
    if (!p || p->magic != PROC_MAGIC)
        extinction("proc_apply_identity: NULL or corrupted Proc");
    if (supp_gid_count > PROC_SUPP_GIDS_MAX)
        extinction("proc_apply_identity: supp_gid_count exceeds PROC_SUPP_GIDS_MAX");
    // A-1a R1 F3: make the "single audited mutation site" contract real.
    // INVALID(0) and SYSTEM are never legitimate values to STAMP via this path
    // (the boot chain sets SYSTEM directly in proc_init; the spawn gate
    // pre-validates real ids / NONE). One reaching here means a caller bypassed
    // the gate -- a kernel-internal contract violation, like the count check.
    // (Supplementary gid VALUES remain the gate's responsibility, per the
    // header contract; this guards the primary identity scalars.)
    if (principal_id == PRINCIPAL_INVALID || principal_id == PRINCIPAL_SYSTEM)
        extinction("proc_apply_identity: principal_id is a reserved sentinel");
    if (primary_gid == GID_INVALID || primary_gid == GID_SYSTEM)
        extinction("proc_apply_identity: primary_gid is a reserved sentinel");
    p->principal_id   = principal_id;
    p->primary_gid    = primary_gid;
    p->supp_gid_count = supp_gid_count;
    for (u8 i = 0; i < supp_gid_count; i++)
        p->supp_gids[i] = supp_gids ? supp_gids[i] : 0u;
    // Zero the tail so no stale inherited gid survives past the new count.
    for (u8 i = supp_gid_count; i < PROC_SUPP_GIDS_MAX; i++)
        p->supp_gids[i] = 0u;
}

// =============================================================================
// A-4a: the legate stamp + scope teardown (IDENTITY-DESIGN.md §9.8, I-25).
// =============================================================================

// Allocate a fresh, nonzero legate scope id. The teardown-walk match key, so
// uniqueness is load-bearing -- kernel-allocated, never caller-supplied. The
// loop skips the 0 sentinel on the (physically-unreachable) u32 wrap.
static u32 legate_scope_alloc(void) {
    u32 id;
    do {
        id = __atomic_fetch_add(&g_next_legate_scope, 1u, __ATOMIC_RELAXED) + 1u;
    } while (id == 0u);
    return id;
}

void proc_become_legate(struct Proc *p, u64 caps_to_or, u32 session_id,
                        u64 valid_until) {
    if (!p || p->magic != PROC_MAGIC)
        extinction("proc_become_legate: NULL or corrupted Proc");

    u32 scope = legate_scope_alloc();

    // OR the (already self-restricted) cleared caps atomically -- a sibling
    // thread of this Proc may read p->caps concurrently in a syscall cap-check
    // (multi-thread Procs exist since P6).
    __atomic_fetch_or(&p->caps, caps_to_or, __ATOMIC_ACQ_REL);

    // Durable principal_id is UNCHANGED (scripture §3.1). Record the scope
    // context. session_id + valid_until are written before scope_id so a
    // concurrent teardown walk (of some OTHER scope) that observes a nonzero
    // scope_id via the RELEASE store below also observes these. (Correctness
    // does not depend on it -- a fresh scope id matches no in-flight teardown
    // ctx -- but it keeps the publication clean.)
    p->legate_session_id  = session_id;
    p->legate_valid_until = valid_until;
    __atomic_store_n(&p->legate_scope_id, scope, __ATOMIC_RELEASE);

    // Mark the ROOT. One-way; NEVER inherited by rfork (proc_flags never are),
    // so an rfork child is a scope MEMBER (carries scope_id, not the flag),
    // never a second root. RELEASE pairs with the ACQUIRE read in exits().
    __atomic_fetch_or(&p->proc_flags, PROC_FLAG_LEGATE_ROOT, __ATOMIC_RELEASE);
}

// Teardown walk context + callback. The callback group-terminates every Proc
// carrying `scope_id` except `except` (the legate root on its own exit, which
// exits via the normal path) and kproc. Returns 0 always so proc_for_each /
// proc_for_each_walk visits the ENTIRE table.
//
// Member teardown is the scripture-mandated tidiness sweep: at v1.0 the
// clearance set is ALL elevation-only, which rfork strips, so a scope MEMBER
// never holds the elevated caps (only the root does). I-25's privilege
// guarantee ("no elevated Proc outlives the scope") therefore rests on the
// ROOT -- which dies on its own exit (trigger 1) or self-terminates on
// valid_until expiry (trigger 2, which passes except=NULL to include self).
// A member spawned racing this walk that the sweep misses is a benign,
// UNELEVATED straggler with a stale scope tag -- not an I-25 violation. (A
// strict whole-subtree close via an rfork-under-lock parent-flag check is a
// documented v1.x tidiness refinement.)
struct legate_teardown_ctx {
    u32          scope_id;
    struct Proc *except;
};

static int legate_teardown_cb(struct Proc *m, void *arg) {
    struct legate_teardown_ctx *ctx = arg;
    if (ctx->scope_id == 0u)  return 0;   // never tear down the 0 (not-a-legate) scope
    if (m == ctx->except)     return 0;
    if (m == g_kproc)         return 0;
    if (m->legate_scope_id != 0u && m->legate_scope_id == ctx->scope_id)
        proc_group_terminate(m, "legate scope ended");
    return 0;   // visit every Proc
}

// proc_legate_teardown_if_root -- if `p` is a legate ROOT, group-terminate every
// OTHER Proc carrying its legate_scope_id (the scripture-mandated subtree sweep,
// I-25). Called from proc_become_zombie_locked -- the SINGLE chokepoint every
// LIVE Proc's ZOMBIE transition passes through (exits() AND thread_exit_self) --
// so the sweep fires on EVERY root death path: a clean exit AND a kill /
// group-terminate / SYS_EXIT_GROUP death (A-4a audit F1; the path A-4b's CAP_KILL
// drives). `except = p`: the root dies via the surrounding zombie transition. A
// non-root Proc (a scope MEMBER -- scope_id set, no ROOT flag) is a no-op: only
// the root sweeps its scope, so a member's own death never tears down the group.
// PRECONDITION: caller holds g_proc_table_lock (uses the LOCKED proc_for_each_walk;
// proc_for_each would re-take it -> deadlock). The flag read pairs (ACQUIRE) with
// proc_become_legate's RELEASE store; legate_scope_id is its coherent companion.
void proc_legate_teardown_if_root(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC) return;
    if (!(__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE) & PROC_FLAG_LEGATE_ROOT))
        return;
    struct legate_teardown_ctx tctx = { .scope_id = p->legate_scope_id, .except = p };
    proc_for_each_walk(g_kproc, legate_teardown_cb, &tctx);
}

// P6-pouch-threads (sub-chunk 9a) audit F1 close: cross-module
// acquire/release for g_proc_table_lock. thread.c's thread_link_into_proc
// / thread_unlink_from_proc need to serialize with proc_count_live_peers_-
// locked's walk; the lock is `static` here so we expose helpers rather
// than the symbol.
irq_state_t proc_table_lock_acquire(void) {
    return spin_lock_irqsave(&g_proc_table_lock);
}
void proc_table_lock_release(irq_state_t s) {
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// P6-pouch-threads (sub-chunk 9): the clear-child-tid handoff shared by
// exits() and thread_exit_self(). On thread exit, if this Thread has a
// non-zero `clear_child_tid` (set via SYS_SET_TID_ADDRESS), atomically
// zero *clear_child_tid + torpor_wake(UINT32_MAX) on the same VA so a
// joiner blocked in torpor_wait observes the death. Best-effort — an
// unmapped tidptr silently skips the wake without extinction.
//
// The store-then-wake order matters: the joiner's torpor_wait re-checks
// the user word inside torpor_lock; the lock acquire pairs with this
// store's preceding lock-release sequence (the producer side of the
// Linux-futex discipline). Same as torpor's standard producer pattern.
static void thread_clear_child_tid_handoff(struct Thread *t, struct Proc *p) {
    u64 tidptr = t->clear_child_tid;
    if (tidptr == 0) return;
    // F7 audit close: defensive re-validation of alignment. tidptr was
    // validated at SYS_SET_TID_ADDRESS time (alignment + bound), but a
    // future cross-thread setter (none today — see the clear_child_tid
    // field comment in thread.h, F10 audit close) could violate the
    // alignment invariant. An unaligned STR alignment-faults to the
    // dispatcher; the fault-fixup table catches only translation /
    // permission fault classes, NOT EC_DATA_ABORT_ALIGN — so unguarded,
    // an unaligned tidptr would extinct the kernel. The check is cheap
    // forward-defense.
    if (tidptr & 0x3u) return;
    // Defensive: tidptr was validated at SYS_SET_TID_ADDRESS time
    // (alignment + bound), so this store should succeed. If userspace
    // munmap'd the page in between, uaccess_store_u32 returns -1; we
    // skip the wake (no waiter could be blocked on a torn-down page
    // sanely) and consider it a userspace bug, not ours.
    if (uaccess_store_u32(tidptr, 0u) != 0) return;
    (void)sys_torpor_wake_for_proc(p, tidptr, (u32)~0u);
}

// Internal: common Proc-ZOMBIE transition body shared by exits() and
// thread_exit_self(). MUST be called UNDER g_proc_table_lock. The Proc
// must be ALIVE; transitions to ZOMBIE, captures exit_msg/exit_status,
// re-parents orphan children, wakes parent's child_done.
//
// status: 0 = clean exit ("ok"); non-zero = error.
// msg:    captured by reference; caller-owned (typically a string
//         literal). NULL becomes "ok".
static void proc_become_zombie_locked(struct Proc *p, int status, const char *msg) {
    // A-4a (I-25): if p is a legate ROOT, tear down its scope as it dies. Placed
    // at this chokepoint (not in exits() alone) so the sweep fires on EVERY death
    // path -- a clean exit AND a kill / group-terminate (the path A-4b's CAP_KILL
    // drives, and the multi-thread-root SYS_EXIT_GROUP path). A-4a audit F1.
    proc_legate_teardown_if_root(p);

    // A-4c-1: if p is the kernel console owner, clear the owner pointer so it
    // never dangles to a zombie/freed Proc. Same chokepoint discipline as the
    // legate teardown above -- fires on every death path. (caller holds
    // g_proc_table_lock, which proc_set_console_owner / proc_console_post_-
    // interrupt also take, so the clear is race-free w.r.t. those readers.)
    if (g_console_owner == p) {
        g_console_owner = NULL;
    }
    // A-4c-2: likewise clear the trusted login authority (corvus) on its death,
    // so a SAK fired after it exits falls back to revoke-only rather than
    // re-granting the console to a freed Proc.
    if (g_console_trusted_proc == p) {
        g_console_trusted_proc = NULL;
    }

    // 2B-F3: clear init on its own death, BEFORE the reparent below --
    // same never-dangle chokepoint discipline as the console clears, and
    // the ordering makes a dying init's own children fall back to kproc
    // (a self-adopt here would loop the reparent forever). Not a v1.0
    // success path (init never exits; a failed init extincts the boot in
    // joey_run), but the death path must be sound regardless.
    if (g_init_proc == p) {
        __atomic_store_n(&g_init_proc, NULL, __ATOMIC_RELEASE);
    }

    if (p->children) {
        proc_reparent_children(p);
    }
    p->exit_msg    = msg ? msg : "ok";
    p->exit_status = status;
    p->state       = PROC_STATE_ZOMBIE;
    // Wake parent's child_done UNDER the lock — parent stays alive
    // through the wakeup (the original P3-A discipline). Lock order:
    // proc_table_lock → r->lock.
    if (p->parent) {
        wakeup(&p->parent->child_done);
        // P6-pouch-signals-impl (sub-chunk 13a): post the synthetic
        // `child_exit` note to the parent's queue. notes_post takes the
        // queue lock + the poll_list.lock + (after dropping queue lock)
        // the queue's Rendez lock — all compose with proc_table_lock
        // (no path takes those then proc_table_lock). synthetic=true
        // enables coalesce-on-full so the post is contractually
        // infallible (a queue-full parent may lose precise (pid, status)
        // tuples but will still observe "a child exited"; wait_pid
        // re-discovers any losses by walking p->children).
        notes_post_child_exit(p->parent, p->pid, p->exit_status);
    }
}

// #926 (U-6f command-substitution prerequisite): close + free a SINGLE-thread
// Proc's handle table at process exit, NOT deferred to reap. A process's
// inherited file descriptors -- pipe write ends, sockets, /srv connections --
// thus close when the PROCESS terminates, which is the correct Unix / Plan 9
// semantics: a peer reading the dying process's pipe sees EOF immediately,
// instead of blocking until the (possibly much later) parent wait_pid reaps
// it. Before this, a shell draining `$(cmd)`'s stdout to EOF would hang --
// the child's pipe write end stayed open in the zombie until reap, so
// write_eof was never delivered (kernel/pipe.c).
//
// CALLED FROM exits() at the TOP, BEFORE the g_proc_table_lock acquire +
// the ZOMBIE transition, and ONLY when p->thread_count == 1. Three
// properties make this the sound place:
//   - t is still RUNNING (EXITING is not set until under the lock below), so
//     a sleep-capable close hook (a 9P clunk's Tclunk/Rclunk wait,
//     srvconn teardown) is LEGAL -- sleeping while EXITING trips sched()'s
//     "current is not RUNNING" assertion.
//   - p is still ALIVE (not yet ZOMBIE), so wait_pid cannot reap it -- there
//     is no risk that the reaper thread_free's this Thread while it sleeps
//     mid-close (a UAF). The reaper only ever touches ZOMBIE Procs.
//   - thread_count == 1, so there are NO peer Threads sharing this handle
//     table -- closing it cannot pull an fd out from under a live peer.
//
// MULTI-thread Procs (thread_count > 1) keep the close at reap (proc_free):
// their last-Thread ZOMBIE transition marks the Thread EXITING ATOMICALLY
// (under the lock) with the last-Thread determination, leaving no RUNNING
// window in which the dying Thread could do a sleeping close. Closing the
// shared table earlier would race live peers. Multi-thread fds-close-at-exit
// is a v1.x refinement (needs an EXITING-protocol restructure); at v1.0 they
// retain the historical close-at-reap, so this is a strict improvement with
// no regression.
//
// ORDERING vs proc_free's vma_drain (which still runs at reap): inverted
// (handle close at exit precedes vma_drain at reap), but SAFE by the #847
// per-Burrow dual refcount -- a Burrow frees only when BOTH handle_count==0
// AND mapping_count==0, so dropping handle_count here while a VMA still maps
// it (mapping_count>0) does not free it; vma_drain at reap drops the last
// mapping ref and frees. The handle-before-notes order (devnotes_close
// before notes_queue_free) is preserved: this close happens-before
// proc_free's notes_queue_free (exit precedes reap).
//
// IDEMPOTENT: a Proc that does NOT pass through here (multi-thread, or a
// direct `state=ZOMBIE; proc_free()` orphan/rollback path) keeps p->handles
// set and proc_free's handle_table_free closes it; this path NULLs p->handles
// so proc_free's handle_table_free(NULL) no-ops. No double-free either way.
static void proc_close_handles_at_exit(struct Proc *p) {
    if (!p) return;
    if (p->handles) {
        handle_table_free(p->handles);
        p->handles = NULL;
    }
}

// Internal: count peer Threads that are NOT in THREAD_EXITING state
// (i.e. still live — RUNNING / RUNNABLE / SLEEPING / etc.) AND are not
// `self`. MUST be called UNDER g_proc_table_lock.
//
// What the lock buys: synchronization with the THREAD_EXITING write
// path — exits() and thread_exit_self() both write `t->state =
// THREAD_EXITING` under this lock. Holding the lock here means the
// `state != THREAD_EXITING` check is sound: an EXITING peer was
// committed before any reader holding the lock could observe it as
// non-EXITING.
//
// What the lock does NOT buy: synchronization with RUNNING ↔ RUNNABLE
// ↔ SLEEPING transitions, which run under per-CPU sched cs->lock
// (sched.c) NOT under g_proc_table_lock. The plain reads here can
// observe stale RUNNING/RUNNABLE/SLEEPING values — and that's fine,
// because all three compare-equal as "live" (!= THREAD_EXITING) and
// the check doesn't distinguish them. F5 audit close: any future
// check that DOES distinguish them (e.g. RUNNING vs SLEEPING) MUST
// add its own synchronization; the lock here is NOT sufficient for
// that. Also: the link/unlink of p->threads itself runs under this
// lock (F1 audit close), so the list walk is coherent.
int proc_count_live_peers_locked(struct Proc *p, struct Thread *self) {
    int n = 0;
    for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
        if (peer == self) continue;
        if (peer->state != THREAD_EXITING) n++;
    }
    return n;
}

// P6 hardening #3a (scripture e45a571 -- docs/ERRORS.md + the snare:*
// note family + the user-authorized 2026-05-26 design): terminate the
// faulting Proc instead of extincting the kernel on EL0 unhandled
// fault. Called from arch/arm64/exception.c::exception_sync_lower_el
// for every EL0 sync exception that previously called
// extinction_with_addr (FAULT_UNHANDLED_USER, FAULT_FATAL,
// EC_PC_ALIGN, EC_SP_ALIGN, EC_BTI, EC_BRK, default).
//
// CONTRACT:
//   - `name` MUST be one of the snare:* string-literal constants from
//     <thylacine/notes.h> (NOTE_NAME_SNARE_SEGV, etc.). The kernel
//     forwards it verbatim to exits() so wait_pid + the parent's
//     uart log see a recognizable tag.
//   - `faulting_addr` is the FAR_EL1 (data abort) or ELR_EL1 (PC
//     alignment / BTI / BRK / unknown EC) value at the fault; printed
//     in the diagnostic for forensics.
//   - NEVER returns. Calls exits() which transitions the calling
//     Thread's Proc to ZOMBIE and yields; the scheduler picks
//     another thread.
//
// DEFENSE-IN-DEPTH:
//   - If current_thread() / its Proc is corrupted or NULL, extincts
//     (the fault path is itself broken; can't safely call exits).
//   - If the calling Proc is kproc, extincts (kproc runs at EL1; an
//     EL0 fault tagged to kproc means the exception handler's
//     current_thread()/proc bookkeeping is broken).
//   - The Proc may have live peer Threads (thread_count > 1): exits()
//     (below) routes a multi-thread Proc through the #809/#811 group
//     cascade, so a userspace fault terminates the whole Proc, not the
//     kernel. (Pre-#809 this branch extincted because exits() could not
//     yet shoot down peers; that dependency has landed -- RW-1 C-F1.)
//
// The uart line preserves the visibility the prior `extinction_with_addr`
// call provided: a faulting userspace binary still announces itself on
// boot logs, so test failures + production diagnostics surface the
// pid + reason + address without requiring a debugger.
__attribute__((noreturn))
void proc_fault_terminate(const char *name, uintptr_t faulting_addr) {
    // F2 audit close: defense-in-depth on `name` itself. The contract
    // says `name` MUST be a NOTE_NAME_SNARE_* string literal; today's
    // only callers in arch/arm64/exception.c pass literals, so the
    // NULL case is latent. But the function downstream passes `name`
    // straight to uart_puts (which `while(*s)`-derefs) and to exits
    // (which strcmp's against "ok"). A NULL passed in via a future
    // caller bug would NULL-deref the kernel from the uart layer.
    // Cheap to guard here; matches the surrounding extinction-on-
    // contract-violation pattern.
    if (!name)                       extinction_with_addr(
                                         "proc_fault_terminate with NULL name (contract violation)",
                                         faulting_addr);

    struct Thread *t = current_thread();
    if (!t)                          extinction("proc_fault_terminate with no current thread");
    if (t->magic != THREAD_MAGIC)    extinction("proc_fault_terminate from corrupted current thread");

    struct Proc *p = t->proc;
    if (!p)                          extinction("proc_fault_terminate from thread with no proc");
    if (p->magic != PROC_MAGIC)      extinction("proc_fault_terminate from thread with corrupted proc");
    if (p == g_kproc)                extinction_with_addr(
                                         "proc_fault_terminate routed to kproc (impossible at EL0)",
                                         faulting_addr);

    // Terminate the faulting Proc -- single OR multi-thread (RW-1 C-F1).
    // exits() runs the standard teardown for a peerless Proc (child_exit
    // note to parent, ZOMBIE, sched) AND, for a Proc with live peer
    // Threads, routes through the #809/#811 group cascade
    // (proc_group_terminate flags the group + wakes/IPIs every peer;
    // thread_exit_self self-exits this faulting Thread; the last Thread out
    // reaps with this snare:* status). So a userspace fault in a
    // multi-thread Proc (stratumd, the virtio-blk driver running DMA
    // pointer arithmetic) terminates the PROC, not the kernel -- the exact
    // snare:* per-Proc-termination contract the pre-#809 thread_count>1
    // extinction predated and violated. The dependency that branch waited
    // on (cross-thread shootdown) has landed; the branch is now retired.
    //
    // The uart line preserves pre-#3a visibility: a faulting binary
    // announces pid + reason + addr on the boot log. Parent wait_pid
    // observes exit_status = 1 at v1.0 (sys_exits_handler collapses non-"ok"
    // to 1); the structured 64-bit status is a v1.x lift per docs/ERRORS.md.
    uart_puts("user fault: pid=");
    uart_putdec((u64)p->pid);
    uart_puts(" reason=\"");
    uart_puts(name);
    uart_puts("\" addr=");
    uart_puthex64((u64)faulting_addr);
    uart_puts(" -- terminating Proc\n");

    exits(name);
    /* UNREACHABLE -- exits is noreturn (single-thread: sched; multi-thread:
       thread_exit_self after the group cascade) */
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
    // v1.0 P6-pouch-threads (sub-chunk 9): multi-thread Procs are now
    // allowed via SYS_THREAD_SPAWN. exits() declares program-wide
    // termination, which v1.0 REQUIRES all peer Threads to have already
    // EXITED — the pthread_join contract guarantees this when the
    // program is well-formed. A peer in RUNNING / RUNNABLE / SLEEPING
    // state at this point indicates an un-joined Thread (programmer
    // error). Cross-thread shootdown (Linux's CLONE_THREAD-style
    // exit_group) is a v1.x extension.

    // P5-corvus-srv-impl-a2: tombstone any /srv service this Proc posted
    // (specs/corvus.tla ServiceTombstone). Done here — p still ALIVE,
    // still this thread's own valid Proc — and BEFORE the g_proc_table_-
    // lock acquire: srv_proc_exit_notify takes only the leaf registry
    // lock, so it never enters a lock-ordering relation with
    // g_proc_table_lock. exits() is the sole termination path at v1.0
    // (proc.c: state reaches ZOMBIE only through here); a future async-
    // kill path must call srv_proc_exit_notify too.
    srv_proc_exit_notify(p);

    // P5-hostowner-b-a: drop any pending /cap grant targeting this Proc.
    // Same discipline as srv_proc_exit_notify — leaf grant-table lock,
    // no relation with g_proc_table_lock. Safety doesn't strictly need
    // this (stripes are fresh per Proc — a recycled pid gets a fresh
    // stripes, no accidental elevation), but cleanup frees the table slot.
    cap_proc_exit_notify(p);

    // #926: a SINGLE-thread Proc closes its fds HERE -- at exit, while still
    // RUNNING + ALIVE + peerless -- so inherited pipe write ends (and other
    // fds) close at process termination, delivering pipe EOF immediately to a
    // peer reading them (a shell draining `$(cmd)` no longer hangs). This is
    // the ONLY sound spot: t is still RUNNING (a sleep-capable close hook,
    // e.g. a 9P clunk, is legal -- it would trip sched()'s "not RUNNING"
    // assert after the EXITING mark below); p is still ALIVE so wait_pid
    // cannot reap+thread_free us mid-close; and thread_count==1 means no peer
    // shares this table. Multi-thread Procs keep the close at reap (proc_free)
    // -- their last-Thread EXITING mark is atomic-under-lock with the
    // last-Thread determination, leaving no RUNNING window for a sleeping
    // close (v1.x refinement). See proc_close_handles_at_exit.
    if (p->thread_count == 1) {
        proc_close_handles_at_exit(p);
    }

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

    // A-4a (I-25) legate-root scope teardown is NOT inline here: it fires in
    // proc_become_zombie_locked (the shared ZOMBIE chokepoint), so a root that
    // dies via the group-terminate path (thread_exit_self below, when this Proc
    // has live peers; or a kill / SYS_EXIT_GROUP) sweeps its scope too -- not
    // only the clean single-thread exit. A-4a audit F1.

    // Peer-Thread check (multi-thread Proc gate): every peer Thread MUST
    // be in THREAD_EXITING state already. wait_pid's reap loop later
    // walks p->threads and frees each, so the count itself is not
    // restricted — only that none is live.
    int live_peers = proc_count_live_peers_locked(p, t);
    if (live_peers != 0) {
        // #811 (ARCH §8.8.1, closes #809-audit F4): a whole-Proc exits() with
        // live peers is no longer a kernel extinction. Route through the SAME
        // universal cascade as exit_group -- flag + wake every sleeping peer +
        // IPI-kick running peers (under the lock proc_group_terminate now
        // requires) -- then self-exit. Each peer dies at its EL0-return
        // die-check; the last Thread out reaps the Proc with this msg's status
        // (thread_exit_self reads the recorded group_exit_msg). A well-formed
        // multi-thread program joins its peers first and never reaches here.
        proc_group_terminate(p, msg);
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        thread_exit_self();
        extinction("exits: thread_exit_self returned after group terminate");
    }

    int status = (msg && msg[0] == 'o' && msg[1] == 'k' && msg[2] == 0) ? 0 : 1;
    proc_become_zombie_locked(p, status, msg);

    // Mark the executing thread EXITING so sched() leaves it out of the
    // run tree (it will be reaped by the parent's wait_pid).
    t->state = THREAD_EXITING;

    spin_unlock_irqrestore(&g_proc_table_lock, s);

    // F3 audit close: clear-child-tid handoff runs AFTER the EXITING
    // commit (and the Proc-zombie transition above) so any joiner woken
    // by the torpor_wake observes a consistent state — the EXITING write
    // is visible via the spin_unlock_irqrestore's release pairing with
    // the joiner's subsequent acquire of any synchronizing lock. Without
    // this order, a joiner could resume, return from pthread_join, call
    // exits(), and trip the "exits with live peer threads" extinction
    // against our still-RUNNING worker self. uaccess_store_u32 may
    // demand-page (vma_lock + buddy); torpor_wake takes torpor_lock —
    // both compose with proc_table_lock (no path takes proc_table_lock
    // while holding either), so doing them AFTER the lock release keeps
    // the original lock-order discipline.
    thread_clear_child_tid_handoff(t, p);

    // Yield. Will not return — we're EXITING, sched() doesn't re-insert,
    // and there's no future wake target for us.
    sched();
    extinction("exits: returned from sched (impossible)");
}

// P6-pouch-threads (sub-chunk 9): SYS_THREAD_EXIT body. See proc.h.
void thread_exit_self(void) {
    struct Thread *t = current_thread();
    if (!t)                  extinction("thread_exit with no current thread");
    if (t->magic != THREAD_MAGIC)
                             extinction("thread_exit from corrupted current thread");

    struct Proc *p = t->proc;
    if (!p)                  extinction("thread_exit from thread with no proc");
    if (p->magic != PROC_MAGIC)
                             extinction("thread_exit from thread with corrupted proc");
    if (p == g_kproc)        extinction("thread_exit from kproc (boot thread)");
    if (p->state != PROC_STATE_ALIVE)
                             extinction("thread_exit from non-ALIVE proc (race?)");
    // Defensive: current_thread() always returns the running thread (it
    // IS t since we just read it via current_thread()), so t->state ==
    // THREAD_RUNNING is the structural invariant. The check fires only
    // if kernel state is otherwise corrupted (TPIDR_EL1 pointing at a
    // freed Thread, scheduler bug, etc.). F6 audit close: documented as
    // defense-in-depth, not race-related — sched()'s preempt path may
    // transition prev to RUNNABLE momentarily but always restores RUNNING
    // before user code resumes; the read here is from the running CPU's
    // perspective so it always sees RUNNING.
    if (t->state != THREAD_RUNNING)
                             extinction("thread_exit from non-RUNNING thread (defensive — kernel state corruption?)");

    // F3 audit close: do NOT run the clear-child-tid handoff yet. The
    // EXITING transition must commit FIRST so that a joiner woken by
    // the torpor_wake observes a state-consistent producer (running
    // → EXITING) before it can resume + call exits(); see the F3
    // prosecution chain in `memory/audit_p6_pouch_threads_9a_closed_-
    // list.md`. The handoff happens below, AFTER the spin_unlock.

    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);

    int live_peers = proc_count_live_peers_locked(p, t);
    bool become_zombie = (live_peers == 0);

    if (become_zombie) {
        // This Thread is the last live one. Proc transitions to ZOMBIE.
        // SYS_EXIT_GROUP / kill cross-thread shootdown (I-24): if a group
        // termination is in progress, use the recorded group_exit_msg + its
        // derived status (the same "ok" -> 0 / else -> 1 collapse exits()
        // uses); otherwise the SYS_THREAD_EXIT convention is status 0 / "ok"
        // (no user-specified status; explicit-status program exit goes through
        // exits()). The group_exit_msg read is under g_proc_table_lock here +
        // set via release CAS in proc_group_terminate -- a coherent snapshot.
        const char *gmsg = __atomic_load_n(&p->group_exit_msg, __ATOMIC_ACQUIRE);
        if (gmsg) {
            int gstatus = (gmsg[0] == 'o' && gmsg[1] == 'k' && gmsg[2] == 0) ? 0 : 1;
            proc_become_zombie_locked(p, gstatus, gmsg);
        } else {
            proc_become_zombie_locked(p, 0, "ok");
        }
    }

    t->state = THREAD_EXITING;

    spin_unlock_irqrestore(&g_proc_table_lock, s);

    // F3 audit close: clear-child-tid handoff runs HERE — after the
    // EXITING commit so a joiner waking from the torpor_wake never
    // observes us as RUNNING in any subsequent peer-state walk. The
    // handoff still runs OUTSIDE proc_table_lock to preserve the
    // existing lock-order discipline (uaccess_store_u32 may
    // demand-page → vma_lock + buddy; torpor_wake → torpor_lock —
    // neither composes with proc_table_lock).
    thread_clear_child_tid_handoff(t, p);

    // If we became zombie, also do the srv / cap notifies (the leaf-lock
    // discipline allows them outside proc_table_lock — same as exits()).
    // Skipped on the non-last path: a Proc still ALIVE has live /srv
    // posts and pending /cap grants that should NOT be tombstoned.
    if (become_zombie) {
        srv_proc_exit_notify(p);
        cap_proc_exit_notify(p);
        // #926: NO at-exit handle close here. A multi-thread Proc's fds close
        // at reap (proc_free) -- the EXITING mark above is atomic-under-lock
        // with the last-Thread determination, so there is no RUNNING window
        // in which this dying Thread could safely run a sleep-capable close;
        // and the table was shared with peers until just now. Multi-thread
        // fds-close-at-exit is a v1.x refinement. Single-thread Procs that exit
        // VOLUNTARILY (return from main -> exits()) DO close at exit. KNOWN
        // ASYMMETRY: a single-thread Proc that is KILLED (proc_group_terminate
        // -> el0_return_die_check -> thread_exit_self) reaches HERE, so its fds
        // also defer to reap -- the EOF-on-death contract is complete for
        // voluntary exit but not yet for the kill path. The v1.x EXITING-
        // protocol restructure (which enables multi-thread at-exit close)
        // closes the kill path too. See proc_close_handles_at_exit.
    }

    sched();
    extinction("thread_exit_self: returned from sched (impossible)");
}

// ===========================================================================
// SYS_EXIT_GROUP / cross-Proc kill cross-thread shootdown (ARCH §7.9.1, I-24).
// ===========================================================================
//
// The flag-and-self-terminate model (Plan 9 / Linux / Zircon convergent): the
// caller flags the Proc + wakes/kicks its Threads, and each Thread kills
// ITSELF at its next EL0-return die-check (el0_return_die_check). No Thread is
// force-torn-down from outside; the IPI is a latency accelerant, not a
// synchronous stop. See proc.h for the contract + ARCH §7.9.1 for the design.
//
// CONTRACT: the caller MUST hold g_proc_table_lock. The universal death-wake
// (#811, ARCH §8.8.1) walks p->threads, and that list is mutated (peers
// self-remove on exit) only under g_proc_table_lock -- a lockless walk would
// race a concurrent thread_free into a use-after-free. All callers comply: the
// kill walk_cb runs under proc_for_each's lock; sys_exit_group_handler and
// exits() acquire it around this call. Holding it also serializes every
// group-termination, so the CAS below only guards idempotency for a serialized
// second caller (exit_group racing a kill).

void proc_group_terminate(struct Proc *p, const char *msg) {
    if (!p || p->magic != PROC_MAGIC) return;   // fail-safe; caller validates
    if (p == g_kproc) return;   // #809 P3a: kproc runs at EL1 + never group-exits
    if (!msg) msg = "killed";

    // Flag the Proc for group termination. Set-once via CAS: the first
    // caller's msg wins; a racing second group-terminate (two threads both
    // exit_group, or exit_group racing a kill) is a no-op for the flag but
    // still re-runs the wake + kick below (idempotent). __ATOMIC_RELEASE so a
    // peer's __ATOMIC_ACQUIRE load at its die-check sees a fully-published msg.
    const char *expected = NULL;
    __atomic_compare_exchange_n(&p->group_exit_msg, &expected, msg,
                                false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);

    // Wake every futex (torpor) sleeper of p so it returns from torpor_wait to
    // its EL0-return die-check. MUST run AFTER the flag set: a peer that
    // registers in torpor_wait after this walk re-observes the now-set flag in
    // torpor_wait's post-register check (register-then-observe; I-9). The
    // torpor lock-order (torpor_lock) is strictly below g_proc_table_lock, so
    // this composes under the lock the contract now requires.
    torpor_wake_all_for_proc(p);

    // Universal death-wake (#811, ARCH §8.8.1): wake EVERY peer blocked in a
    // sleep()/tsleep() rendez sleep so it returns *_INTR and dies at its EL0-
    // return die-check -- closing the §7.9.1 residual where an indefinite
    // poll(-1) / pipe / devnotes_read sleeper was never woken and its Proc was
    // never reaped (the #809-audit F1 hang). The only record of "Thread T
    // sleeps on Rendez R" is the reverse pointer t->rendez_blocked_on: read it
    // under the peer's wait_lock -- the SAME lock the sleeper's register-then-
    // observe takes, which is the I-9 serialization -- and wakeup() the Rendez.
    //
    // wait_lock is HELD ACROSS wakeup (Option A, ARCH §8.8.1): it pins the peer
    // so a torpor waiter's STACK-allocated w.rendez (rendez_blocked_on can
    // point into a sleeping peer's kernel stack frame) cannot be popped out
    // from under the waker. rendez_blocked_on is non-NULL exactly while the
    // owner is registered-and-sleeping (set/cleared only by the owner, under
    // wait_lock); a RUNNING peer reads NULL and is skipped (it reaches its own
    // die-check). wakeup() re-validates r->waiter under r->lock, so a peer
    // already woken on its normal path (or by torpor_wake_all above) is a safe
    // no-op. Lock order: g_proc_table_lock -> wait_lock -> (wakeup:
    // g_timerwait.lock -> r->lock); acyclic (only the owner WRITES
    // rendez_blocked_on; the cascade only READS it, under wait_lock).
    for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
        irq_state_t ws = spin_lock_irqsave(&peer->wait_lock);
        struct Rendez *r = peer->rendez_blocked_on;
        if (r) wakeup(r);
        spin_unlock_irqrestore(&peer->wait_lock, ws);
    }

    // Kick any peer RUNNING in userspace on another CPU so it traps + hits its
    // IRQ-from-EL0 die-check without waiting for a timer tick (Linux
    // kick_process). Broadcast to other online CPUs (rare path; <= ncpus-1
    // IPIs); a CPU not running a peer of p simply no-ops its die-check. The
    // periodic preemption timer is the floor if the IPI is somehow missed.
    smp_resched_others();
}

void el0_return_die_check(void) {
    struct Thread *t = current_thread();
    if (!t || t->magic != THREAD_MAGIC) return;
    struct Proc *p = t->proc;
    if (!p || p->magic != PROC_MAGIC)   return;

    // A-4a (I-25) trigger 2: legate scope time-expiry. Cheap guard FIRST -- the
    // common case is not-a-legate (scope_id == 0), which short-circuits before
    // any timer read or lock. If this Proc is in a legate scope whose deadline
    // has passed, tear down the ENTIRE scope INCLUDING self (except = NULL):
    // proc_group_terminate flags each member's group_exit_msg, and the
    // fall-through check below then observes self's own flag and self-terminates
    // (so the elevated ROOT never executes more EL0 work past valid_until). This
    // tail is LOCKLESS, so the lock-TAKING proc_for_each is correct here (unlike
    // exits()'s already-locked walk). Re-entrant-safe: once flagged, a second
    // expiry pass is a CAS no-op. scope_id read ACQUIRE (pairs with
    // proc_become_legate's RELEASE); valid_until is its coherent companion.
    u32 scope = __atomic_load_n(&p->legate_scope_id, __ATOMIC_ACQUIRE);
    if (scope != 0u && p->legate_valid_until != 0u &&
        timer_now_ns() > p->legate_valid_until) {
        struct legate_teardown_ctx tctx = { .scope_id = scope, .except = NULL };
        proc_for_each(legate_teardown_cb, &tctx);
    }

    if (__atomic_load_n(&p->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) {
        // The Proc is group-terminating; self-terminate. thread_exit_self
        // marks self EXITING and, if this is the last live Thread, transitions
        // the Proc to ZOMBIE with the recorded group_exit_msg status. NEVER
        // returns (sched() away) -- so on the sync tail the subsequent
        // notes_deliver is skipped, and on the IRQ tail the eret is never
        // reached (#713-safe: no interruptible ELR-set..eret window is entered
        // on the die path).
        thread_exit_self();
    }
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
//   1. Single-writer children list (per non-adopter parent at v1.0).
//      **ADOPTER EXCEPTION (R6-A F105; widened by 2B-F3)**: the orphan-
//      adopter's children list (init when up, else kproc) IS multi-
//      writer (any exiting Proc with orphan children head-inserts here
//      under proc_table_lock) and since 2B-F3 that is ROUTINE. Sound
//      because a stale walk only reaches valid unfreed Procs (no
//      concurrent free of the adopter's children — only the adopter's
//      own thread reaps them) and a missed zombie is re-discovered at
//      the next child_done wakeup (exits-side wake, or the adopted-
//      zombie wake in proc_reparent_children). Full chain at the
//      g_proc_table_lock declaration block above.
//
//   2. Per-child `state` visibility via the wakeup→sleep handshake's
//      release/acquire on r->lock.
//
//   3. Defense-in-depth: stale-read tolerance via the cond loop's
//      re-evaluation under (2)'s chain on each subsequent wakeup.
//
// See g_proc_table_lock's header for the full chain.
// cond predicate for wait_pid_for's sleep: a child MATCHING want_pid is
// in ZOMBIE state, OR no matching child exists at all (-1 return path).
// `want_pid == -1` matches any child (the original wait_pid semantics);
// `want_pid > 0` selects that one child.
//
// CALLED FROM `sleep` UNDER r->lock. P3-A: deliberately does NOT acquire
// g_proc_table_lock — that would create a r->lock → g_proc_table_lock
// nesting that, combined with exits's g_proc_table_lock → r->lock-via-
// wakeup discipline, deadlocks. The pid filter only compares `c->pid`
// (set once at proc_alloc, never mutated), so it adds nothing to the
// lock/visibility reasoning — the same three invariants documented at
// g_proc_table_lock's declaration hold unchanged.
struct wait_cond_ctx {
    struct Proc *parent;
    int          want_pid;
};

static int wait_pid_cond(void *arg) {
    struct wait_cond_ctx *ctx = arg;
    struct Proc *parent = ctx->parent;
    bool any_match = false;
    for (struct Proc *c = parent->children; c; c = c->sibling) {
        if (ctx->want_pid != -1 && c->pid != ctx->want_pid) continue;
        any_match = true;
        if (c->state == PROC_STATE_ZOMBIE) return 1;   // matching zombie → wake to reap
    }
    return any_match ? 0 : 1;   // no matching child → wake to return -1; else sleep
}

int wait_pid_for(int want_pid, int flags, int *status_out) {
    struct Thread *t = current_thread();
    if (!t)                  extinction("wait_pid with no current thread");
    struct Proc *p = t->proc;
    if (!p)                  extinction("wait_pid with no proc");
    if (p->magic != PROC_MAGIC)
                             extinction("wait_pid with corrupted proc");

    const bool nohang = (flags & WAIT_WNOHANG) != 0;

    // RW-2 2B-F1/F2: serialize wait_pid_for per-Proc -- at most ONE Thread of a
    // Proc may be in the reap-or-sleep critical section at a time. `p->child_-
    // done` is a single-waiter Rendez (a 2nd concurrent waiter trips `sleep`'s
    // single-waiter assert -> kernel extinction, F1), and `wait_pid_cond` walks
    // `p->children` locklessly (under r->lock, never g_proc_table_lock -- the
    // P3-A lock-order choice), racing a peer's `proc_unlink_child`+`proc_free`
    // reap -> UAF (F2). Both rest on a single-writer/single-waiter premise the
    // P6 multi-thread-Proc lift falsified. The exchange refuses a genuinely-
    // CONCURRENT 2nd caller (returns -1) -- NOT every multi-thread Proc: a
    // single-thread Proc, AND a legitimately-multi-thread Proc with only one
    // waiting Thread (kproc has many kthreads but only the boot Thread reaps;
    // a server with one designated reaper), see wait_active == 0 and proceed.
    // The flag is contended ONLY by two Threads of the SAME Proc both inside
    // wait_pid_for. Cleared at the single `out:` exit. (Promoting `child_done`
    // to multi-waiter + a proc_table_lock-protected cond -- the tracked v1.x
    // completeness -- lifts this from "refuse the 2nd" to "all may wait".)
    if (__atomic_exchange_n(&p->wait_active, 1u, __ATOMIC_ACQUIRE) != 0u)
        return -1;

    int ret;
    for (;;) {
        // P3-A: walk + unlink + state capture under g_proc_table_lock.
        // Atomic with concurrent exits() on our children — they hold the
        // same lock during their ZOMBIE transition, so we either see
        // the pre-transition state (no zombie yet → sleep) or the post-
        // transition state (zombie ready to reap).
        irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);

        // Single scan: find a MATCHING zombie to reap; also note whether
        // any matching child exists at all (alive or zombie). No matching
        // child → -1; matching-but-alive → sleep (or 0 under WNOHANG).
        struct Proc *zombie = NULL;
        bool any_match = false;
        for (struct Proc *c = p->children; c; c = c->sibling) {
            // want_pid -1 = any; >0 = that child. A child's pid is in
            // [1, INT_MAX) (proc_alloc: g_next_pid starts at 1), and pid 0
            // is kproc (never a child of a user Proc), so want_pid 0 / < -1
            // (POSIX pgroup selectors) match no child here -> any_match stays
            // false -> -1. Reserved for a future process-group lift.
            if (want_pid != -1 && c->pid != want_pid) continue;
            any_match = true;
            if (c->state == PROC_STATE_ZOMBIE) {
                zombie = c;
                break;
            }
        }

        if (!any_match) {
            // No matching child (none at all, or none with want_pid).
            spin_unlock_irqrestore(&g_proc_table_lock, s);
            ret = -1;
            goto out;
        }

        if (zombie) {
            int pid    = zombie->pid;
            int status = zombie->exit_status;

            // P6-pouch-threads (sub-chunk 9): multi-thread Procs are
            // allowed. Every Thread in zombie->threads must be EXITING
            // (the program-exit gate enforces this — see exits() /
            // thread_exit_self()). Sanity-check ALL threads INSIDE the
            // lock; the per-thread state writes happen under the same
            // lock so the observation is consistent.
            if (!zombie->threads) {
                spin_unlock_irqrestore(&g_proc_table_lock, s);
                extinction("wait_pid: zombie with no threads");
            }
            for (struct Thread *ct = zombie->threads; ct; ct = ct->next_in_proc) {
                if (ct->state != THREAD_EXITING) {
                    spin_unlock_irqrestore(&g_proc_table_lock, s);
                    extinction("wait_pid: zombie thread not in EXITING state");
                }
            }

            proc_unlink_child(p, zombie);

            spin_unlock_irqrestore(&g_proc_table_lock, s);

            // Outside the lock: spin on on_cpu, then free EVERY Thread in
            // p->threads + proc_free. We released the lock to avoid
            // holding it across thread_free's multi-CPU run-tree walk
            // (which acquires every CPU's cs->lock). Lock order would
            // be: g_proc_table_lock → cs->lock. No reverse exists
            // (sched/ready/wakeup never touch lineage state), so holding
            // both would be safe — but releasing first reduces lock-hold
            // time.
            //
            // P2-Dd-pre on_cpu spin: each EXITING thread had its state
            // set under proc_table_lock + then yielded via sched(); the
            // destination CPU's resume code clears on_cpu via
            // cs->prev_to_clear_on_cpu. Without this spin, thread_free
            // could race with a destination CPU still mid-switch
            // (TPIDR_EL1 briefly points at ct). Mirrors the on_cpu
            // spin in wakeup() (P2-Cf).
            //
            // Walk-with-next discipline: thread_free unlinks ct from
            // zombie->threads + decrements thread_count, so capture
            // next BEFORE the free. The list is doubly-linked so an
            // unlinked node's `next_in_proc` stays valid for the
            // capture-then-free idiom (the unlink resets it to NULL
            // AFTER we've read it). Loop terminates when zombie->threads
            // becomes NULL — every thread freed, thread_count reaches 0.
            struct Thread *ct = zombie->threads;
            while (ct) {
                while (__atomic_load_n(&ct->on_cpu, __ATOMIC_ACQUIRE)) {
                    __asm__ __volatile__("yield" ::: "memory");
                }
                struct Thread *next = ct->next_in_proc;
                thread_free(ct);
                ct = next;
            }

            // thread_free walks unlinked every Thread; thread_count == 0
            // and threads == NULL by here — proc_free's preconditions
            // are met.
            proc_free(zombie);

            if (status_out) *status_out = status;
            ret = pid;
            goto out;
        }

        // A matching child is alive but not yet a zombie. Release the lock.
        spin_unlock_irqrestore(&g_proc_table_lock, s);

        if (nohang) {
            ret = 0;         // WAIT_WNOHANG: report "not ready" without blocking.
            goto out;
        }

        // Sleep on child_done; exits() in a matching child wakes us. The
        // cond re-evaluates the SAME pid filter under r->lock; sleep is
        // atomic with the cond check (see scheduler.tla NoMissedWakeup
        // proof). `ctx` is stack-local to this frame, which outlives the
        // sleep call.
        struct wait_cond_ctx ctx = { .parent = p, .want_pid = want_pid };
        // #811 (ARCH §8.8.1): a death-interrupted sleep means THIS Proc is
        // group-terminating (a peer / kill flagged it while we waited on a
        // child). Return so the waiting Thread unwinds to its EL0-return
        // die-check; do NOT loop (re-sleep would re-INTR = livelock).
        if (sleep(&p->child_done, wait_pid_cond, &ctx) == SLEEP_INTR) {
            ret = -1;
            goto out;
        }
    }
out:
    // RW-2 2B-F1/F2: release the per-Proc wait serialization. Reached on EVERY
    // non-extinction exit (the zombie-with-bad-threads paths are noreturn). The
    // RELEASE pairs with the next claimant's ACQUIRE exchange.
    __atomic_store_n(&p->wait_active, 0u, __ATOMIC_RELEASE);
    return ret;
}

// wait_pid — reap ANY zombie child, blocking. The pervasive (any,
// blocking) case; thin wrapper over wait_pid_for. Plan 9 wait(2) shape.
int wait_pid(int *status_out) {
    return wait_pid_for(-1, 0, status_out);
}

// =============================================================================
// Test support — NOT a production API; deliberately absent from proc.h.
// =============================================================================
//
// A production Proc is spliced into the kproc-rooted process table by
// rfork (proc_link_child); a Proc the in-kernel test harness allocates
// directly with proc_alloc is not. proc_for_each / proc_find_by_pid only
// see linked Procs, so a test exercising proc_caps_by_stripes against a
// bare proc_alloc'd Proc must link it first. The harness extern-declares
// these; they have no production caller.

// proc_test_link — splice `p` in as a child of kproc.
void proc_test_link(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC)
        extinction("proc_test_link: NULL or corrupted Proc");
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    proc_link_child(kproc(), p);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// proc_test_set_init — point g_init_proc at `p` (NULL restores the
// pre-init fallback). Bypasses proc_publish_init's single-publish gate
// so the 2B-F3 reparent-to-init test can simulate a live init during
// the pre-joey test phase and restore afterward.
void proc_test_set_init(struct Proc *p) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    __atomic_store_n(&g_init_proc, p, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// proc_test_unlink — remove `p` from kproc's children list. The test MUST
// call this before freeing `p` so the table holds no dangling pointer.
void proc_test_unlink(struct Proc *p) {
    if (!p) return;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    for (struct Proc **pp = &kproc()->children; *pp; pp = &(*pp)->sibling) {
        if (*pp == p) {
            *pp = p->sibling;
            break;
        }
    }
    p->parent  = NULL;
    p->sibling = NULL;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
}

// proc_test_legate_teardown — run the A-4a legate-scope teardown walk for
// `scope_id`, excluding `except`. Exposes the lockless (proc_for_each) form of
// trigger 2 so the unit test can verify the walk flags every scope member's
// group_exit_msg + spares non-members / kproc -- the actual death step fires at
// the EL0-return die-check, which kernel test threads (EL1) never reach, so the
// flag is the unit-testable observable. No production caller.
void proc_test_legate_teardown(u32 scope_id, struct Proc *except) {
    struct legate_teardown_ctx tctx = { .scope_id = scope_id, .except = except };
    proc_for_each(legate_teardown_cb, &tctx);
}
