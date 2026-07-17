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

#include <thylacine/allowance.h>
#include <thylacine/burrow.h>
#include <thylacine/caps.h>
#include <thylacine/devcap.h>
#include <thylacine/devsrv.h>
#include <thylacine/env.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/notes.h>
#include <thylacine/page.h>
#include <thylacine/poll.h>        // child_waiters multi-waiter reap (#344)
#include <thylacine/territory.h>
#include <thylacine/proc.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>
#include <thylacine/torpor.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>
#include <thylacine/vma.h>
#include <thylacine/weft.h>

#include "../arch/arm64/mmu.h"
#include "../arch/arm64/hwdebug.h" // 8a-2b: hwdebug_free (per-Proc bp table teardown)
#include "../arch/arm64/exception.h" // 8a-2b-2: struct exception_context -- arm SPSR.SS in the resume frame
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
//   `exits()` holds proc_table_lock through the wake of parent's
//   child_waiters — poll_waiter_list_wake takes the list lock (nested)
//   then, for each registered waiter, sets pw->ready and signals that
//   waiter's private rendez. Order: proc_table_lock → list → rendez.
//
//   This bracketing is REQUIRED to defeat a self-audit-found race:
//   without it, between exits's release of proc_table_lock and exits's
//   call to poll_waiter_list_wake(&parent->child_waiters), the parent
//   could be reaped + freed by the grandparent's wait_pid (which also
//   takes proc_table_lock — but acquires AFTER our release, blocking us
//   not at all). Holding proc_table_lock through the wake ensures the
//   parent stays alive until the wake completes.
//
//   #344 (the multi-waiter lift). wait_pid_for no longer sleeps on a
//   single-waiter `child_done` Rendez with a `wait_pid_cond` that walks
//   the children list locklessly. Instead each waiting Thread registers
//   its OWN stack `poll_waiter` on `child_waiters` and parks on its OWN
//   private rendez whose cond reads ONLY `pw->ready` (a flag) — it
//   touches NO lineage state. This DISSOLVES the deadlock hazard the old
//   design fought, on three grounds:
//
//     (1) Acyclic by construction. The register (poll_waiter_list_-
//         register) and the wake (poll_waiter_list_wake) both run UNDER
//         proc_table_lock and take the list lock nested inside it:
//         proc_table_lock → list. The rendez lock is taken strictly
//         INSIDE the list-locked wake (list → rendez) and, separately,
//         by a parked Thread's own sleep cond-check (rendez only). NO
//         path takes the rendez lock then acquires proc_table_lock, so
//         proc_table_lock → list → rendez has no cycle. The old
//         `wait_pid_cond reads children under r->lock` — the one path
//         that risked an r->lock → proc_table_lock inversion — is gone.
//
//     (2) The authoritative scan is always locked. The "a matching
//         zombie is reapable" decision is wait_pid_for's re-scan of
//         p->children UNDER proc_table_lock at the top of every loop
//         iteration (never locklessly). So the old observations about a
//         lockless cond walk tolerating mid-insert / stale-state races
//         are MOOT: the scan is fully synchronized with every concurrent
//         exits / rfork / reparent (all hold proc_table_lock), including
//         the multi-writer orphan-adopter list (R6-A F105 / 2B-F3) —
//         there is no lockless reader left to race it.
//
//     (3) No lost death-wake (I-9 register-then-observe, the poll.c
//         discipline). The no-zombie scan AND the poll_waiter_list_-
//         register run in ONE proc_table_lock critical section. A child
//         that becomes ZOMBIE *after* that section must take proc_table_-
//         lock (after our release), so it finds our already-registered
//         waiter and sets pw->ready + wakes — the parked Thread re-scans
//         and reaps. A child that became ZOMBIE *before* our scan is seen
//         by the scan directly. `pw->ready` is only the wake RELAY; the
//         re-scan is the readiness truth, so a spurious/non-matching wake
//         simply re-scans and re-parks. The set-ready (under list lock)
//         is made visible to the cond-check (under rendez lock) by the
//         wake's intervening wakeup(rendez), exactly as in poll.c.
//
//   Multi-thread Procs (P6-pouch-threads) are now first-class here: ANY
//   number of a Proc's Threads may wait_pid_for concurrently — each parks
//   on its own rendez, the wake fans out to all, and exactly one reaps
//   each zombie (a loser re-scans and either waits again or returns -1 =
//   no matching child). This RETIRES the RW-2 2B-F1/F2 `wait_active`
//   guard that had to refuse the 2nd concurrent waiter (-1) — the very
//   refusal that broke multi-threaded Go's parallel `go build` (#342).
//
// CASCADING-EXITS RACE (R5-H F75):
//   Without this lock, parent A's `proc_reparent_children` walk could
//   rewrite child B's `parent` pointer concurrently with B's `exits()`
//   reading the same field at `poll_waiter_list_wake(&p->parent->child_-
//   waiters)`. If B holds the stale (pre-reparent) pointer in a register
//   and A's subsequent ZOMBIE + parent-wake chain causes A to be reaped +
//   freed before B's wake line fires, B accesses freed-A → UAF on the
//   wait list. This lock serializes A's mutation with B's read; the
//   wake-inside-lock structure additionally prevents the
//   reaped-between-release-and-wake variant.
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
    // PTY-1a: a parentless Proc is its own session + group leader (sid =
    // pgid = pid). rfork_internal overwrites both with the parent's (a
    // child JOINS its parent's session + group, POSIX fork semantics);
    // proc_alloc re-stamps beside its real pid assignment.
    p->sid   = (u32)pid;
    p->pgid  = (u32)pid;
    p->state = PROC_STATE_ALIVE;
    poll_waiter_list_init(&p->child_waiters);   // #344: multi-waiter child-reap
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
    // PTY-1a: keep the own-session default tracking the REAL pid (the
    // proc_init_fields stamp used the placeholder). rfork_internal
    // overwrites both for a spawned child.
    p->sid  = next;
    p->pgid = next;

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

// RW-7 R3-F1: before a dying Proc's KObj_DMA pages return to the buddy
// allocator, stop every virtio device the Proc drove -- otherwise an armed
// device keeps the freed pages' PAs latched in its virtqueue and DMAs a
// used-ring completion into recycled memory (silent cross-Proc corruption;
// I-7's device-stop clause is unmet on ABNORMAL driver death -- e.g.
// stratumd's virtio-blk dying via _Exit on an abort / mallocng assert). A
// well-behaved driver resets its own device before exit; this is the kernel
// backstop for the crash path the kernel cannot make a driver run.
//
// A driver's device-register claim (KObj_MMIO) is reachable two ways, and we
// walk BOTH (round-2 F1): the handle table (an open fd) AND the per-Proc VMA
// list (a BURROW_TYPE_MMIO mapping). SYS_MMIO_MAP transfers the KObj_MMIO ref
// to the VMA's mapping and a driver can then CLOSE the fd (the idiomatic
// mmap-then-close), leaving the device reachable ONLY through the VMA -- a
// handle-only walk would miss it and reopen the corruption window. Both walks
// run BEFORE vma_drain frees the VMAs; a device reached by both is reset twice
// (status=0 is idempotent). The reset writes through the kernel's permanent
// .base MMIO mapping, independent of the user mapping vma_drain tears down.
// Page-exclusive KObj_MMIO claims (mmio_handle.c overlap rejection) mean no
// OTHER live DRIVER owns a slot in the range; the one in-range slot the kernel
// itself drives -- virtio-rng -- is skipped inside virtio_mmio_reset_in_range
// (round-2 F2), so the reset never disturbs a device this Proc did not own.
//
// Called from every proc-exit handle-close site so every death path is
// covered before the pages free (#68 round-2 F4 -- the three-site topology):
// proc_close_handles_at_exit from exits() (the last-live-thread voluntary
// exit; live_peers-gated) AND from thread_exit_self (the last Thread out of
// a group terminate -- multi-thread exit_group incl. pouch _Exit, kills,
// fault-terminate), plus proc_free (the fallback: orphan/rollback paths whose
// table is still intact). NO lock: like the adjacent handle_table_free +
// vma_drain, this runs only on a quiesced Proc -- at the two at-exit sites
// the quiescence argument is live_peers == 0 (every peer has committed
// EXITING, whose residual execution never touches the table, and no new
// peer can spawn without a RUNNING thread); at proc_free all threads are
// reaped. Returns the device count for the regression test.
//
// RESIDUAL (round-3 F1, trust-envelope): a driver that FULLY releases its
// KObj_MMIO claim (SYS_BURROW_DETACH + close the fd) while the device is still
// armed is invisible to BOTH walks -- but that is a malicious/buggy
// CAP_HW_CREATE driver (the v1.0 envelope: kproc grants CAP_HW_CREATE only to
// trusted drivers; stratumd maps-and-holds), the same posture as the
// mmio_handle.c rng-slot residual. The structural close is a per-device
// KObj_VIRTIO_DEV that resets at owner-death OR last-claim-drop (device-session
// model; R3-F8, P5+), which closes this residual by construction.
int proc_quiesce_owned_devices(struct Proc *p) {
    if (!p) return 0;
    int reset = 0;

    // (a) device-register claims held by an open fd.
    struct HandleTable *t = p->handles;
    if (t) {
        for (int i = 0; i < PROC_HANDLE_MAX; i++) {
            struct Handle *h = &t->slots[i];
            if (h->magic != HANDLE_MAGIC) continue;
            if (h->kind != KOBJ_MMIO || !h->obj) continue;
            struct KObj_MMIO *k = (struct KObj_MMIO *)h->obj;
            reset += virtio_mmio_reset_in_range(k->pa, k->size);
        }
    }

    // (b) device-register claims held ONLY by a BURROW_TYPE_MMIO mapping (fd
    // closed after SYS_MMIO_MAP). Independent of (a): a NULL handle table does
    // not imply no mapped devices.
    for (struct Vma *v = p->vmas; v; v = v->next) {
        struct Burrow *b = v->burrow;
        if (!b || b->type != BURROW_TYPE_MMIO || !b->kobj_mmio) continue;
        struct KObj_MMIO *k = b->kobj_mmio;
        reset += virtio_mmio_reset_in_range(k->pa, k->size);
    }

    return reset;
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

    // RW-7 R3-F1: stop any virtio device this Proc drove BEFORE vma_drain /
    // handle_table_free free its KObj_DMA pages back to the buddy (a still-
    // armed device would DMA into recycled memory). No-op when p->handles is
    // already NULL -- since #68 that is EVERY exit path (exits()'s
    // live_peers-gated close + thread_exit_self's last-out close both
    // quiesce + NULL the table); this does real work only for the direct
    // `state=ZOMBIE; proc_free()` orphan/rollback paths whose table is
    // still intact (round-2 F4).
    proc_quiesce_owned_devices(p);

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

    // I-34: release the per-Proc hardware allowance (NULL-tolerant). A plain
    // heap struct, independent of the handle/notes/VMA frees above.
    allowance_free(p);

    // G15: release the per-Proc environment group (NULL-tolerant; frees every
    // entry's value + the struct). Like the allowance, a plain heap struct
    // independent of the frees above; owned 1:1 by this Proc (RFENVG sharing
    // deferred), so this is the sole release.
    env_free(p);

    // 8a-2b (I-39): release the per-Proc HW-breakpoint table (NULL-tolerant). A
    // plain heap struct freed ONLY here at reap -- never at detach -- so the
    // ctx-switch install hook (hwdebug_switch_in) never derefs freed memory
    // (every thread was reaped + on_cpu-spun before proc_free, so no CPU is in a
    // switch-in for this Proc). detach clears the table (bp_count=0) but leaves
    // it allocated for this final free.
    hwdebug_free(p->debug_hw);
    p->debug_hw = NULL;

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
// PTY-1a (PTY-DESIGN.md section 4): POSIX sessions + process groups.
// =============================================================================
//
// sid/pgid live on struct Proc (rfork-inherited; own-session default for a
// parentless Proc). Every operation runs find + validate + mutate under ONE
// g_proc_table_lock hold -- the children list, the table linkage, and
// sid/pgid are all guarded fields, and splitting the hold would let a
// concurrent exit / setpgid / rfork invalidate a checked premise.
//
// Errno discipline: -T_E_ACCES for every POSIX-EPERM contour (the errno.h
// warning: -T_E_PERM would alias the bare -1 sentinel), -T_E_SRCH for
// no-such-process, -T_E_INVAL for a negative pgid. The PTY-3 boundary-line
// re-maps ACCES -> EPERM at the libc surface.

// Lock-held: does process group `pgid` exist within session `sid`? POSIX: a
// group persists while it has ANY member (the leader may already be gone), so
// existence is a membership scan, not a leader lookup. ZOMBIEs do not count
// as members for setpgid-target purposes (a group of only corpses is not a
// group you can join).
struct pgrp_scan_ctx { u32 pgid; u32 sid; };
static int pgrp_exists_cb(struct Proc *q, void *arg) {
    struct pgrp_scan_ctx *c = arg;
    return (q->state == PROC_STATE_ALIVE
            && q->pgid == c->pgid && q->sid == c->sid) ? 1 : 0;
}

// proc_setsid -- SYS_SETSID core. The caller becomes the leader of a new
// session + a new group (sid = pgid = pid) with NO controlling terminal.
// POSIX: fails if the caller is ALREADY a process-group leader -- else the
// leader's own group would be left spanning two sessions. The controlling-
// terminal drop is structural at PTY-1a: the pts registry that can hold a
// session<->pts binding lands at PTY-1c, and its acquisition path checks
// "caller is a session leader with no existing ctty" against the registry,
// so a fresh setsid session starts clean by construction; when the registry
// exists, this core also clears any binding owned by the OLD session iff
// the caller was its leader (wired at PTY-1d).
int proc_setsid(struct Proc *p) {
    if (!p) return -1;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    if (p->pgid == (u32)p->pid) {
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        return -T_E_ACCES;              // POSIX EPERM: already a group leader
    }
    p->sid  = (u32)p->pid;
    p->pgid = (u32)p->pid;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return p->pid;
}

// proc_setpgid -- SYS_SETPGID core. POSIX rules, one lock hold:
//   pid 0 = the caller; pgid 0 = the target's own pid (mint a new group).
//   The target must be the caller or a LIVE direct child (-T_E_SRCH).
//   The target must share the caller's session and must not be a session
//   leader (EPERM contour -> -T_E_ACCES).
//   pgid must equal the target's pid (minting) or name an existing group in
//   the CALLER's session (EPERM contour -> -T_E_ACCES).
// The POSIX "child already exec'd -> EACCES" rule has no analog (spawn is a
// fused fork+exec); the self-or-child + same-session rules carry the
// security weight. A ZOMBIE target is -T_E_SRCH: the mutation is meaningless
// on a corpse, and PTY-1e's waitpid(-pgid) reads a zombie's pgid for reap
// matching -- moving a corpse between groups mid-wait would be a seam.
int proc_setpgid(struct Proc *self, int pid, int pgid) {
    if (!self) return -1;
    if (pgid < 0) return -T_E_INVAL;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *target = NULL;
    if (pid == 0 || pid == self->pid) {
        target = self;
    } else {
        for (struct Proc *c = self->children; c; c = c->sibling) {
            if (c->pid == pid) { target = c; break; }
        }
    }
    if (!target || target->state != PROC_STATE_ALIVE) {
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        return -T_E_SRCH;
    }
    if (target->sid != self->sid || (u32)target->pid == target->sid) {
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        return -T_E_ACCES;
    }
    u32 want = (pgid == 0) ? (u32)target->pid : (u32)pgid;
    if (want != (u32)target->pid) {
        struct pgrp_scan_ctx ctx = { .pgid = want, .sid = self->sid };
        if (!proc_for_each_walk(kproc(), pgrp_exists_cb, &ctx)) {
            spin_unlock_irqrestore(&g_proc_table_lock, s);
            return -T_E_ACCES;          // POSIX EPERM: no such group in session
        }
    }
    target->pgid = want;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return 0;
}

// proc_getpgid / proc_getsid -- SYS_GETPGID/GETSID cores. pid 0 = the
// caller. Read-only introspection, no privilege gate (POSIX getpgid(2)/
// getsid(2); /proc exposes more). ZOMBIEs answer: a zombie still HAS a pgid
// -- PTY-1e's waitpid(-pgid) matches zombie children by it, and a shell may
// legitimately getpgid a child that already exited. One lock hold covers
// find + read (the walk only reaches table-linked Procs; unlinking requires
// the same lock, so the deref is safe).
int proc_getpgid(struct Proc *self, int pid) {
    if (!self) return -1;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *target = (pid == 0) ? self : proc_find_by_pid_walk(kproc(), pid);
    int rv = target ? (int)target->pgid : -T_E_SRCH;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return rv;
}

int proc_getsid(struct Proc *self, int pid) {
    if (!self) return -1;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *target = (pid == 0) ? self : proc_find_by_pid_walk(kproc(), pid);
    int rv = target ? (int)target->sid : -T_E_SRCH;
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
    // #65 (I-32): child_count == the length of `children`. Atomic (under the
    // lock here, but a cross-Proc /proc reader loads it without the lock).
    __atomic_fetch_add(&parent->child_count, 1u, __ATOMIC_RELEASE);
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
    // #65 (I-32): keep child_count == list length. Guarded so a (corruption-
    // induced) zero never wraps to UINT32_MAX.
    if (__atomic_load_n(&parent->child_count, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&parent->child_count, 1u, __ATOMIC_RELEASE);
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
// (I-9-shaped lost-wake). Wake the adopter's child_waiters once if any
// adoptee arrived ZOMBIE -- every Thread of the adopter waiting in
// wait_pid_for re-scans (#344 multi-waiter). Lock order proc_table_lock ->
// list -> rendez is the established exits() discipline; the adopter is alive
// under the lock
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
        // #65 (I-32): reparent splices directly (no proc_link/unlink_child), so
        // rebase both counts to keep child_count == list length. p is dying
        // (its count is about to vanish) but track it symmetrically anyway.
        __atomic_fetch_add(&adopter->child_count, 1u, __ATOMIC_RELEASE);
        if (__atomic_load_n(&p->child_count, __ATOMIC_RELAXED) > 0)
            __atomic_fetch_sub(&p->child_count, 1u, __ATOMIC_RELEASE);
        if (c->state == PROC_STATE_ZOMBIE)
            adopted_zombie = true;
    }
    if (adopted_zombie)
        poll_waiter_list_wake(&adopter->child_waiters);
}

// =============================================================================
// #65 (invariant I-32): the per-Proc resource floor
// =============================================================================

bool proc_resource_exempt(const struct Proc *p) {
    // The TCB (PRINCIPAL_SYSTEM: kproc + the boot/service chain) is unbounded so
    // the floor cannot pinch the FS server / the orphan-adopter / the kthread
    // root. Unforgeable: no post-login Proc can acquire PRINCIPAL_SYSTEM
    // (CAP_SET_IDENTITY rejects it), and principal_id is immutable on a running
    // Proc -> a plain read is sound. NULL -> non-exempt (fail-closed).
    return p && p->principal_id == PRINCIPAL_SYSTEM;
}

bool proc_page_charge(struct Proc *p, u32 npages) {
    if (!p) return false;
    // Caller holds p->vma_lock (the attach/detach serialization domain), so the
    // load + the cap decision + the store are atomic against sibling attaches
    // -> the page cap is EXACT. The atomic store keeps a lockless cross-Proc
    // /proc reader coherent.
    u32 cur = __atomic_load_n(&p->page_count, __ATOMIC_RELAXED);
    if (npages > 0xFFFFFFFFu - cur) return false;   // counter overflow (refuse)
    if (!proc_resource_exempt(p) && cur + npages > PROC_PAGE_MAX)
        return false;                                // over cap -> caller -ENOMEM
    __atomic_store_n(&p->page_count, cur + npages, __ATOMIC_RELEASE);
    return true;
}

void proc_page_uncharge(struct Proc *p, u32 npages) {
    if (!p) return;
    // Caller holds p->vma_lock. Clamp so an over-uncharge (should never happen --
    // every uncharge matches a charge) never wraps past 0.
    u32 cur = __atomic_load_n(&p->page_count, __ATOMIC_RELAXED);
    u32 nv  = (cur >= npages) ? cur - npages : 0;
    __atomic_store_n(&p->page_count, nv, __ATOMIC_RELEASE);
}

bool proc_vma_charge(struct Proc *p) {
    if (!p) return false;
    // Caller holds p->vma_lock (the vma_insert/vma_remove domain), so the load + the
    // cap decision + the store are atomic against a sibling attach -> the VMA cap is
    // EXACT. The atomic store keeps a lockless cross-Proc /proc reader coherent. The
    // I-32 fourth axis: the bound a free SYS_BURROW_ATTACH_LAZY reservation needs.
    u32 cur = __atomic_load_n(&p->vma_count, __ATOMIC_RELAXED);
    if (cur == 0xFFFFFFFFu) return false;            // counter saturation (refuse)
    if (!proc_resource_exempt(p) && cur >= PROC_VMA_MAX)
        return false;                                // over cap -> vma_insert rejects
    __atomic_store_n(&p->vma_count, cur + 1, __ATOMIC_RELEASE);
    return true;
}

void proc_vma_uncharge(struct Proc *p) {
    if (!p) return;
    // Caller holds p->vma_lock. Clamp so an over-uncharge (every uncharge matches a
    // charge) never wraps past 0.
    u32 cur = __atomic_load_n(&p->vma_count, __ATOMIC_RELAXED);
    u32 nv  = (cur > 0) ? cur - 1 : 0;
    __atomic_store_n(&p->vma_count, nv, __ATOMIC_RELEASE);
}

bool proc_thread_cap_ok(struct Proc *p) {
    if (!p) return false;
    if (proc_resource_exempt(p)) return true;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    bool ok = p->thread_count < PROC_THREAD_MAX;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return ok;
}

bool proc_child_cap_ok(struct Proc *p) {
    if (!p) return false;
    if (proc_resource_exempt(p)) return true;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    bool ok = p->child_count < PROC_CHILD_MAX;
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return ok;
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

    // #65 (I-32): the per-Proc child cap. Reject a fork bomb EARLY -- before the
    // heavy proc_alloc / territory_clone / thread_create -- so it is cheap. The
    // TOCTOU vs proc_link_child's ++ (a later, separate lock hold) is a bounded
    // overshoot (<= ncpus-1 concurrent spawners) -- acceptable for a floor. kproc
    // + the SYSTEM boot/service chain are exempt (a bomb is untrusted post-login
    // code, not the TCB). The graceful-OOM backstop bounds the recursive case.
    if (!proc_child_cap_ok(parent)) return -1;

    // MENAGERIE section 13.2 (resolved fork) + section 5 / I-34: "bus drivers
    // are sources, not spawners -- one auditable chokepoint." A hardware-
    // allowance-NARROWED Proc (a sandboxed driver/source) may not create a child
    // Proc. The child would inherit a CLONE of the narrowed allowance
    // (allowance_clone_into below) -- or be conferred a subset -- as an
    // INDEPENDENT allowance (revoked == 0) that SURVIVES the parent's
    // DeviceRemoved: proc_revoke_allowance is per-Proc and proc_group_terminate
    // is thread-group-scoped, so a child Proc is reparented to init, not torn
    // down -- leaving a hw-capable grandchild holding live MMIO/IRQ/DMA the
    // warden never tracks, scattering the privilege decision off the warden's
    // sole chokepoint. The gate keys on the ALLOWANCE, not the identity: a
    // SYSTEM-identity driver is still a sandboxed leaf, so -- unlike the #65
    // resource caps -- there is NO SYSTEM exemption here. The broad warden/TCB
    // (allowance == NULL) is unaffected: it is the only spawner of hw-capable
    // children, via the deliberate confer path. Fail-closed -- drivers serve
    // files; they are leaves. (5e-4 audit F2.)
    if (allowance_is_narrowed(parent)) return -1;

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

    // PTY-1a (PTY-DESIGN.md section 4): a child JOINS its parent's session
    // + process group -- POSIX fork semantics. Overwrites proc_alloc's
    // own-session default; only proc_setsid / proc_setpgid move it later.
    child->sid  = parent->sid;
    child->pgid = parent->pgid;

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

    // I-34 (specs/allowance.tla): inherit the hardware allowance. A NARROWED
    // parent's child is equally narrowed -- the hardware-axis analog of caps'
    // monotonic reduction (a child can never reach a BROADER hardware
    // authority than its parent; the same I-2 spirit the `& ~CAP_ELEVATION_-
    // ONLY` strip enforces for caps). A BROAD parent (allowance == NULL) ->
    // child stays NULL (broad), but a plain-rfork child holds CAP_NONE so the
    // broad path is unreachable for it -- only the warden/TCB (broad +
    // CAP_HW_CREATE) spawns broad hw-capable children, and it does so via the
    // confer path, not inheritance. allowance_clone_into leaves child->-
    // allowance NULL on its own failure, so the proc_free rollback's
    // allowance_free is a clean no-op there; a LATER failure (territory_clone
    // / thread_create) frees the just-cloned allowance via the same path.
    if (allowance_clone_into(child, parent) != 0) {
        child->state = PROC_STATE_ZOMBIE;
        proc_free(child);
        return -1;
    }

    // G15 (ARCH section 9.7): deep-copy the parent's environment group into the
    // child -- the Plan 9 default-copy-on-rfork (RFENVG sharing deferred), so a
    // spawned child inherits $GOROOT etc. exactly as it inherits the namespace.
    // A NULL parent->env leaves child->env NULL. On OOM, child->env stays NULL,
    // so the proc_free rollback's env_free is a clean no-op (mirrors the
    // allowance_clone_into discipline above).
    if (env_clone_into(child, parent) != 0) {
        child->state = PROC_STATE_ZOMBIE;
        proc_free(child);
        return -1;
    }

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

// proc_is_console_owner -- true iff `p` is the current g_console_owner (the
// Ctrl-C target = the session shell, via SPAWN_PERM_CONSOLE_OWNER). Reads the
// owner pointer under g_proc_table_lock (the sanctioned discipline -- the
// pointer is written under that lock and cleared on owner-death) and compares it
// to `p`; it NEVER dereferences the owner, so there is no UAF even against a
// concurrent owner exit. RW-11 SA-1b uses this to gate the devcons_read
// INTERACTIVE promotion to the trusted console session (the shell + the
// console-attached login/corvus), keeping an arbitrary console-stdin-reading
// foreground program OUT of the INTERACTIVE band (audit F1). Fail-closed on NULL.
bool proc_is_console_owner(const struct Proc *p) {
    if (!p) return false;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    bool yes = (g_console_owner == p);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return yes;
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
// PTY-1b (PTY-DESIGN.md section 4): kernel-synthetic note fan-out to a
// process group -- the pgrp generalization of proc_console_post_interrupt
// below (see the notes.h declaration for the full contract). One
// g_proc_table_lock hold covers the membership walk, every member's post
// (the established g_proc_table_lock -> q->lock edge, the console-poster
// precedent -- the hold pins each member's lifetime across its post), and
// the per-member LS-5c terminate-wake (the wake's lock contract; it
// self-gates on the latch, so non-terminate names cost nothing). The walk
// visits each table-linked Proc exactly once (a rooted tree), and p->pgid
// is read under the lock that serializes setpgid/rfork/exit -- the F14
// exactly-once-per-member argument. pgid 0 is REFUSED (the boot group --
// kproc/joey -- is never a tty-signal target; the seam's fg_pgid can never
// be 0 since acquisition copies a setsid'd leader's pgid, but a stray
// caller must not be able to fan a terminate note across the boot chain).
struct pgrp_post_ctx { u32 pgid; const char *name; u32 arg; int posted; };
static int pgrp_post_cb(struct Proc *q, void *arg) {
    struct pgrp_post_ctx *c = arg;
    if (q->state == PROC_STATE_ALIVE && q->pgid == c->pgid) {
        if (notes_post(q, c->name, c->arg, NULL, true) == 0)
            c->posted++;
        proc_interrupt_terminate_wake(q);
    }
    return 0;   // keep walking -- every member gets its post
}
int notes_post_pgrp(u32 pgid, const char *name, u32 arg) {
    if (pgid == 0 || !name) return 0;
    struct pgrp_post_ctx ctx = { .pgid = pgid, .name = name, .arg = arg,
                                 .posted = 0 };
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    proc_for_each_walk(kproc(), pgrp_post_cb, &ctx);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return ctx.posted;
}

// The single-Proc sibling (PTY-1d; notes.h contract): the tty seam's F13
// second SIGHUP target. Same one-lock-hold post + terminate-wake shape as
// the pgrp fan-out, keyed by pid. pid 0 refused (never a valid target;
// the pgid-0 refusal analog).
int notes_post_pid(int pid, const char *name, u32 arg) {
    if (pid <= 0 || !name) return 0;
    int posted = 0;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *q = proc_find_by_pid_walk(kproc(), pid);
    if (q && q->state == PROC_STATE_ALIVE) {
        if (notes_post(q, name, arg, NULL, true) == 0) posted = 1;
        proc_interrupt_terminate_wake(q);
    }
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return posted;
}

// PTY-1d (proc.h contract): the tcsetpgrp membership gate.
struct pgrp_in_sid_ctx { u32 pgid; u32 sid; bool found; };
static int pgrp_in_sid_cb(struct Proc *q, void *arg) {
    struct pgrp_in_sid_ctx *c = arg;
    if (q->state == PROC_STATE_ALIVE && q->pgid == c->pgid &&
        q->sid == c->sid) {
        c->found = true;
        return 1;   // stop walking
    }
    return 0;
}
bool proc_pgrp_in_session(u32 pgid, u32 sid) {
    if (pgid == 0) return false;
    struct pgrp_in_sid_ctx ctx = { .pgid = pgid, .sid = sid, .found = false };
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    proc_for_each_walk(kproc(), pgrp_in_sid_cb, &ctx);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return ctx.found;
}

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
// mid-transition (the A-4c-1 console-owner lifetime discipline). RW-7 R2-F2: the
// SAK posts NO note, so it takes ONLY g_proc_table_lock -- the prior
// g_proc_table_lock -> note q->lock edge is gone (revoke/mark/is-attached are
// lock-free atomic RMWs), strictly simplifying the lock order.
void proc_console_sak(void) {
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *owner   = g_console_owner;
    struct Proc *trusted = g_console_trusted_proc;

    bool trusted_live = trusted && trusted->magic == PROC_MAGIC
                        && trusted->state == PROC_STATE_ALIVE;

    // Idempotent under a BREAK flood: once the trusted login authority is the
    // sole console authority (console-attached) and no owner remains to revoke,
    // a repeat SAK is a no-op. RW-7 R2-F1: post-fix the trusted Proc is
    // attach-only and is NEVER the console OWNER, so the prior `owner == trusted`
    // guard could no longer fire -- this is its replacement.
    if (trusted_live && owner == NULL && proc_is_console_attached(trusted)) {
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        return;
    }

    // (1) Revoke the console-attach bit from the current owner. RW-7 R2-F2: post
    // NO note here. Reusing `interrupt` to mean "you lost the console" was a
    // benign courtesy BEFORE LS-5; LS-5 made `interrupt` a real
    // terminate-if-uncaught note, so posting it to the old owner TERMINATES a
    // non-self-managing owner (joey during bringup -> init dies) or spuriously
    // kills a session shell's foreground command. SAK-revoke needs its OWN note
    // name (a dedicated `hangup` / `console-revoked`; RW-7 R2-F3, a v1.x notes
    // SEAM) -- until then, the attach-bit revoke is the SAK's observable effect
    // on the old owner (it loses elevation authority). Guarded on a live owner:
    // after the owner exited, proc_become_zombie_locked already cleared it.
    if (owner && owner->magic == PROC_MAGIC && owner->state == PROC_STATE_ALIVE) {
        proc_revoke_console_attached(owner);
    }

    // (2) Re-grant the console-ATTACH (elevation authority) to the trusted login
    // authority, but do NOT make it the console OWNER. RW-7 R2-F1: owner and
    // attach are DISTINCT roles post-LS-5 -- the OWNER is the `interrupt`
    // (Ctrl-C) target; the ATTACH gates SAK/elevation redemption (the devcap
    // gate keys on PROC_FLAG_CONSOLE_ATTACHED). corvus is the login AUTHORITY,
    // never a Ctrl-C target; making it the owner meant a Ctrl-C after SAK posted
    // `interrupt` to corvus, arming its terminate latch (non-self-managing) and
    // killing the trusted path until reboot. The Ctrl-C owner is re-established
    // when login spawns the session shell (SPAWN_PERM_CONSOLE_OWNER); during the
    // login window there is no foreground terminate target. FAIL-SAFE: with no
    // trusted Proc alive, the attach is simply not granted -- no Proc can redeem
    // elevation until a trusted login claims the console.
    g_console_owner = NULL;
    if (trusted_live) {
        proc_mark_console_attached(trusted);   // atomic OR; trusted ALIVE-checked
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
    // PTY-1b: EITHER terminate-class latch (interrupt OR tty:quit/hup) --
    // this is the wake gate; the per-family mask precision lives in
    // thread_die_pending's re-check, so a spurious wake here costs one
    // predicate re-evaluation, never a wrong unwind.
    return (__atomic_load_n(&p->proc_flags, __ATOMIC_ACQUIRE)
            & (PROC_FLAG_INTR_TERMINATE_PENDING |
               PROC_FLAG_TTY_TERMINATE_PENDING)) != 0;
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

// PTY-1f: the death-side orphan-rule hook (defined with the job-stop
// machinery below; forward-declared because the ZOMBIE chokepoint precedes
// it in the file).
static void proc_orphan_rule_locked(struct Proc *dying);

// Internal: common Proc-ZOMBIE transition body shared by exits() and
// thread_exit_self(). MUST be called UNDER g_proc_table_lock. The Proc
// must be ALIVE; transitions to ZOMBIE, captures exit_msg/exit_status,
// re-parents orphan children, wakes parent's child_waiters (#344).
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

    // PTY-1f (POSIX 2.4.3, the orphan rule): a death can newly-orphan a
    // process group it anchored (its children's groups; its own). A
    // newly-orphaned group with job-stopped members gets tty:hup then
    // tty:cont per member -- no stopped job strands when its shell dies
    // (the F8 residual's load-bearing leg). MUST run BEFORE the reparent
    // (the children list is consumed there); `p` is excluded from every
    // membership/anchor walk as already-dead, so evaluating pre-flip is
    // exactly "the world once p is gone".
    proc_orphan_rule_locked(p);

    if (p->children) {
        proc_reparent_children(p);
    }
    p->exit_msg    = msg ? msg : "ok";
    p->exit_status = status;
    p->state       = PROC_STATE_ZOMBIE;
    // Wake parent's child_waiters UNDER the lock — parent stays alive
    // through the wake (the original P3-A discipline; #344 multi-waiter:
    // every Thread of the parent waiting in wait_pid_for is woken to
    // re-scan). Lock order: proc_table_lock → list → rendez.
    if (p->parent) {
        poll_waiter_list_wake(&p->parent->child_waiters);
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
// CALLED FROM two places, both BEFORE the ZOMBIE transition and both gated
// on live_peers == 0 under g_proc_table_lock (round-2 F2 retired the old
// thread_count==1 gate -- thread_count counts unreaped EXITING peers):
//   (1) exits() (the voluntary-exit last live thread -- single-thread
//       Procs AND joined-then-exits native multi-thread Procs);
//   (2) thread_exit_self(), by the LAST live Thread out of a group
//       terminate (#68 -- multi-thread exit_group and the killed paths).
// Three properties make these the sound places:
//   - t is still RUNNING (EXITING not yet set), so a sleep-capable close
//     hook (a 9P clunk's Tclunk/Rclunk wait, srvconn teardown) is LEGAL --
//     sleeping while EXITING trips sched()'s "current is not RUNNING"
//     assertion. (Site (2) additionally sets t->exit_close_active so
//     thread_die_pending() reads false for the closer: group_exit_msg is
//     set on every SYS_EXIT_GROUP, and without the flag the write-behind
//     flush + Tclunk sends would short-circuit -- the #68 F1 data-loss/
//     fid-leak class. See the thread_exit_self call site.)
//   - p is still ALIVE (not yet ZOMBIE), so wait_pid cannot reap it -- there
//     is no risk that the reaper thread_free's this Thread while it sleeps
//     mid-close (a UAF). The reaper only ever touches ZOMBIE Procs.
//   - The table has exactly ONE potential toucher: at site (1)
//     thread_count == 1 (no peers exist); at site (2) every peer has
//     committed THREAD_EXITING under g_proc_table_lock, and an EXITING
//     peer's remaining execution (clear-child-tid handoff + sched()) never
//     touches the handle table, while no new peer can appear (spawning
//     requires a RUNNING thread in this Proc and the closer is the only
//     one).
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
        // #68 F1 (round-2): the whole close runs under exit_close_active so
        // thread_die_pending() reads false for the closer at BOTH call sites.
        // exits()'s site is reachable with the LS-5 interrupt-terminate latch
        // DELIBERATELY still armed (the default-terminate path leaves the
        // note queued -- notes.c), and thread_exit_self's site always has
        // group_exit_msg set (every SYS_EXIT_GROUP) -- either would
        // short-circuit the dev9p write-behind close-flush (silent data
        // loss) and the close-time Tclunk (a server-side fid leak per fd).
        // The closer is always current_thread() (both sites are the dying
        // thread's own straight-line code); the flag is cleared before
        // return on the same line-of-control, so it cannot leak.
        struct Thread *closer = current_thread();
        closer->exit_close_active = true;
        // RW-7 R3-F1: stop this Proc's virtio devices before its fds (and the
        // KObj_DMA pages they hold) close -- the at-exit leg of the
        // device-death quiesce. proc_free covers the orphan/rollback paths.
        proc_quiesce_owned_devices(p);
        handle_table_free(p->handles);
        p->handles = NULL;
        closer->exit_close_active = false;
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

    // Weft-6a-2: GC any per-flow ring this Proc registered (SYS_WEFT_SHARE) but
    // that the kernel never claimed -- drops each I-30 registration pin so a
    // netd that shared a ring then died can't leak it (weft.tla ShareBounded-
    // ByFlow). Same leaf-lock discipline (g_weft_lock, no relation with
    // g_proc_table_lock). No-op for the overwhelming majority (only netd ever
    // registers shares).
    weft_share_release_owner(p);

    // P3-A (R5-H F75 close): all lineage mutations + parent wakeup
    // happen UNDER g_proc_table_lock atomically. The previous code did
    // these without synchronization, allowing a parallel parent's
    // proc_reparent_children to rewrite p->parent between our read and
    // use of it for wakeup. Holding proc_table_lock through the wakeup
    // additionally prevents a self-audit-found variant: between lock
    // release and the wake, the parent could be reaped + freed by the
    // grandparent's wait_pid; our wake would access freed memory.
    // Holding the lock through the wake ensures the parent stays alive.
    //
    // Lock order: proc_table_lock → list → rendez (poll_waiter_list_wake
    // takes the list lock then signals each waiter's rendez). Sound iff no
    // path takes the rendez lock then acquires proc_table_lock — #344: a
    // wait_pid_for waiter's cond reads only pw->ready under its rendez lock
    // and touches NO lineage state, so the old r->lock → proc_table_lock
    // inversion candidate (wait_pid_cond) no longer exists.
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

    // #926/#68 (round-2 F2): the LAST live thread out closes the Proc's
    // handles HERE, in the same RUNNING+ALIVE window thread_exit_self uses --
    // so inherited pipe write ends deliver EOF at process TERMINATION, not at
    // reap (a shell draining `$(cmd)` sees EOF immediately). The gate is the
    // live_peers determination, NOT thread_count: thread_count counts
    // unreaped EXITING peers (it decrements only at reap), so a well-formed
    // native multi-thread program that joins its workers then calls exits()
    // arrives with thread_count > 1 and live_peers == 0 -- the old
    // thread_count==1 gate skipped its close entirely and the #926
    // drain-before-reap deadlock survived on that path. Window soundness is
    // the thread_exit_self argument verbatim: every peer has committed
    // EXITING (whose residual execution never touches the handle table), no
    // new peer can spawn without a RUNNING thread, p stays ALIVE (no reap),
    // and t stays RUNNING (sleep-capable close hooks legal).
    // proc_close_handles_at_exit sets exit_close_active internally (round-2
    // F1: this site is reachable with the LS-5 interrupt-terminate latch
    // still armed -- the default-terminate path calls exits() with the note
    // deliberately left queued -- and a racing cross-Proc kill can set
    // group_exit_msg mid-close; either would short-circuit the write-behind
    // flush + Tclunk without the flag).
    if (p->handles) {
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        proc_close_handles_at_exit(p);
        s = spin_lock_irqsave(&g_proc_table_lock);
        if (proc_count_live_peers_locked(p, t) != 0)
            extinction("exits: peer appeared during handle close");
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

    // #68 (completes the #926 multi-thread seam): the LAST Thread out closes
    // the Proc's handles HERE, in a deliberately-opened RUNNING+ALIVE window
    // BEFORE the ZOMBIE transition -- so inherited pipe write ends deliver
    // EOF at process termination, not at reap. Pre-#68 a multi-thread Proc
    // (every Go binary) kept its fds until proc_free, so a parent draining
    // the child's stdout to EOF BEFORE reaping deadlocked: EOF needed the
    // reap, the reap needed the parent's wait, the wait waited on EOF (the
    // nora gofmt-on-save hang; ut's `$(go ...)` substitution wedge).
    //
    // Why the window is sound (the same three properties as exits()'s
    // single-thread close, re-derived for the last-out):
    //   - t is still RUNNING (EXITING is written below, after re-taking the
    //     lock), so a sleep-capable close hook is legal.
    //   - p is still ALIVE (the ZOMBIE transition is below), so wait_pid
    //     cannot reap + thread_free us mid-close.
    //   - live_peers == 0 means every peer has committed THREAD_EXITING
    //     (under this lock) -- an EXITING peer executes only its own
    //     thread_exit_self tail (clear-child-tid handoff + sched()), which
    //     never touches the handle table; and no new peer can appear because
    //     SYS_THREAD_SPAWN requires a RUNNING thread in this Proc and t --
    //     here -- is the only one. The determination is therefore STABLE
    //     across the unlock; the recount below is a structural assert, not a
    //     retry.
    //
    // The close runs under exit_close_active (set INSIDE
    // proc_close_handles_at_exit -- round-2 F1 hoisted it so exits()'s site
    // is covered too): group_exit_msg is set on EVERY SYS_EXIT_GROUP -- a
    // clean exit_group(0) included -- and without the flag
    // thread_die_pending() reads true for the closer, so every
    // sleep-capable close hook short-circuited (SLEEP_INTR / the 9P
    // client_self_dying send refusal): the dev9p write-behind close-flush
    // silently DROPPED its staged bytes (data loss for a file left open at
    // a multi-thread exit -- accepted write()s must survive exit, the page
    // cache contract) and the close-time Tclunk was never sent (a
    // server-side fid leak per open dev9p fd). Pre-#68 both ran on the
    // REAPER's (non-dying) thread and worked; the flag restores exactly
    // that behavior inside the new window. The re-admitted wedged-server
    // strand is RELOCATED from the parent (where it hung the shell's
    // wait_pid) onto the already-dying Proc -- and, unlike the old
    // reap-time strand, it is NOT breakable by a further kill (the flag
    // suppresses both death legs for the closer): a wedged flagged close
    // parks the dying Proc unreapably. Precondition = a wedged TRUSTED
    // server (an already system-degraded state); a bounded/abortable
    // close-flush is the recorded v1.x seam (round-2 F3). proc_free's
    // handle_table_free remains the fallback for orphan/rollback paths
    // (idempotent: p->handles is NULLed by the close).
    if (become_zombie && p->handles) {
        spin_unlock_irqrestore(&g_proc_table_lock, s);
        proc_close_handles_at_exit(p);
        s = spin_lock_irqsave(&g_proc_table_lock);
        if (proc_count_live_peers_locked(p, t) != 0)
            extinction("thread_exit: peer appeared during last-out handle close");
    }

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
        weft_share_release_owner(p);   // Weft-6a-2: GC un-claimed per-flow shares
        // #68: the handle close already ran ABOVE (the pre-ZOMBIE
        // RUNNING+ALIVE window), so the EOF-on-death contract now covers
        // multi-thread Procs AND the killed-single-thread path (both reach
        // here) -- the #926 asymmetry is closed. proc_free's
        // handle_table_free remains the fallback for orphan/rollback paths
        // and for hooks a death-flagged close skipped.
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

// Menagerie step 5a / audit F1: install a new allowance pointer on p UNDER
// g_proc_table_lock, returning the displaced one for the caller to free OUTSIDE
// the lock. proc_confer_allowance runs in the child's spawn thunk, AFTER the
// child is proc-tree-linked (proc_link_child + ready) and thus reachable by a
// concurrent killer's proc_group_terminate -> proc_revoke_allowance, which locks
// the OLD allowance. A lockless swap+free there raced that revoke's
// spin_lock(&old->lock) -> UAF (on the narrowed-parent-spawns-child path, where
// the child inherits a non-NULL clone). Serializing the swap on g_proc_table_lock
// -- the same lock the revoke runs under (via proc_group_terminate) -- closes it:
// post-swap, the displaced pointer is unreferenced (any concurrent revoke either
// completed before the swap, done with old, or now reads the new pointer), so the
// caller's kfree outside the lock is safe. g_proc_table_lock is file-static here,
// so the swap must live in proc.c. The RELEASE store preserves the gate-read
// ACQUIRE pairing (allowance.c F4).
struct Allowance *proc_allowance_install_locked(struct Proc *p, struct Allowance *na) {
    if (!p) return NULL;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Allowance *old = p->allowance;
    __atomic_store_n(&p->allowance, na, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return old;
}

void proc_group_terminate(struct Proc *p, const char *msg) {
    if (!p || p->magic != PROC_MAGIC) return;   // fail-safe; caller validates
    if (p == g_kproc) return;   // #809 P3a: kproc runs at EL1 + never group-exits
    if (!msg) msg = "killed";

    // Menagerie build-arc step 5 / #160: revoke this Proc's hardware allowance
    // as the FIRST teardown step (I-34). For a warden-spawned driver this is
    // the DeviceRemoved revoke -- folding it here makes "killgrp the driver"
    // revoke-then-terminate atomically, so an in-flight SYS_*_CREATE racing the
    // removal observes `revoked` at its CreateCommit re-check and aborts
    // (allowance.tla revoke_race), rather than slipping a fresh MMIO/IRQ/DMA
    // handle through onto a device that is gone. NULL-safe (a non-driver Proc
    // has no allowance -> no-op), so this is universal + harmless. Lock order:
    // we hold g_proc_table_lock here; proc_revoke_allowance takes al->lock (a
    // near-leaf), so g_proc_table_lock -> al->lock is the new, acyclic edge.
    proc_revoke_allowance(p);

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

    // #361 (audit-360 F2): no lock is legally held across the kernel->EL0
    // boundary, so a nonzero preempt count here is definitionally a leak (a
    // counted acquire whose release went through the raw variant, or none at
    // all). Undetected it pins this CPU non-preemptible forever -- the sched()
    // assert needs a sleep, and a CPU-bound EL0 loop never sleeps. Checked
    // FIRST: the die path below calls thread_exit_self -> sched(), which would
    // report the leak as a misleading "lock-across-sleep". Runs on BOTH EL0
    // return tails (sync + IRQ, vectors.S); a leak on the first-enter path
    // (userland_enter, no die-check) is caught at the thread's next kernel
    // round-trip. One load on an already-hot line + a predictable branch.
    if (t->preempt_count != 0u)
        sched_report_el0_leak();

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

// =============================================================================
// 8a-1b-beta: the debugger stop / resume state machine (I-39; specs/debug_stop.
// tla). Composes with the death path (#811/#68) -- the most bug-prone lineage in
// the tree -- so DeathWinsOverStop is load-bearing: the EL0-return tail runs this
// stop-check AFTER el0_return_die_check, and this loop re-checks group death on
// every wake, so a kill/exit_group racing the stop terminates the thread here,
// never eret-ing to EL0.
// =============================================================================

// The park wake-cond (specs/debug_stop.tla RegisterObserve; PTY-1f widens it
// to specs/pty_stop.tla's stopOwners): proceed when BOTH stop owners have
// cleared -- the debugger's resume clears debug_stop_req, a job-control
// resume clears job_stop_req, and a thread woken by EITHER resume re-checks
// this cond and re-parks while the OTHER owner still holds (the per-owner
// clear; a tty:cont can never run a debugger-stopped thread --
// StopCompatI39 / BUGGY_DOUBLE_STOP). Death is handled separately --
// sleep()'s own thread_die_pending SLEEP_INTR return breaks the park, and
// the loop's group_exit check terminates -- so this cond need only track
// the resumes.
static int stop_park_wake_cond(void *arg) {
    const struct Proc *p = (const struct Proc *)arg;
    return !proc_stop_requested(p);
}

void el0_return_stop_check(struct exception_context *ctx) {
    struct Thread *t = current_thread();
    if (!t || t->magic != THREAD_MAGIC) return;
    struct Proc *p = t->proc;
    if (!p || p->magic != PROC_MAGIC)   return;

    // Fast path: no stop pending from EITHER owner -- the overwhelmingly common
    // case. Two ACQUIRE loads off one already-hot cache line (job_stop_req
    // occupies debug_stop_req's pad slot) + a predictable not-taken branch. The
    // ACQUIRE pairs with proc_debug_stop_deliver's / proc_job_stop's RELEASE
    // sets (specs/debug_stop.tla: the tail observes a set sflag; PTY-1f: the
    // job owner rides the same tail).
    if (!proc_stop_requested(p))
        return;

    // 8a-1c: publish the EL0-entry trapframe pointer (== the current SP at the
    // vector tail; see thread.h debug_trapframe) so /proc/<pid>/regs reads THIS
    // entry's saved GPR frame -- its kstack offset is not fixed. Set BEFORE the
    // park; a concurrent debug reader only trusts it once the thread is parked
    // (rendez_blocked_on == &debug_rendez + on_cpu==false), which happens below.
    t->debug_trapframe = ctx;

    for (;;) {
        // DEATH WINS (DeathWinsOverStop). A group termination (kill /
        // SYS_EXIT_GROUP / a multi-thread interrupt-terminate, all via
        // proc_group_terminate) that races the stop terminates the thread HERE
        // -- thread_exit_self is el0_return_die_check's core (noreturn), so we
        // never eret to EL0. Re-checked at the top every iteration: a death
        // flagged while we were parked woke debug_rendez (proc_group_terminate's
        // cascade reads rendez_blocked_on == &debug_rendez under wait_lock), so
        // we resume here and die rather than re-parking. group_exit_msg is the
        // model's `gflag`.
        if (__atomic_load_n(&p->group_exit_msg, __ATOMIC_ACQUIRE) != NULL) {
            t->debug_trapframe = NULL;   // 8a-1c holotype F2: don't die with a dangling frame pointer
            thread_exit_self();          // noreturn
        }

        // Resumed (specs/debug_stop.tla ResumeThread -> proceed): start / detach
        // / ctl-fd close cleared the debug flag via proc_debug_resume, AND/OR
        // the job-control resume (tty:cont / SYS_TTY_CONT / the F8-orphan
        // paths) cleared the job flag -- BOTH owners must be clear to proceed
        // (pty_stop.tla stopOwners = {}; a woken thread whose OTHER owner
        // still holds re-parks below -- the per-owner clear). Each clear is a
        // RELEASE ordered before its cascade's per-peer wake (the I-9 close).
        // Proceed to the eret. (Also the fast-path re-observe under no death.)
        if (!proc_stop_requested(p)) {
            // 8a-2b-2 (specs/debug_step.tla Tail->stepping): a step-resume arms the
            // arm64 SS machine in the frame the eret restores -- SPSR.SS (bit 21) =
            // active-not-pending, so exactly ONE EL0 instruction executes before
            // the EC 0x32. MDSCR.SS is armed per-thread by hwdebug_switch_in (so it
            // survives a mid-step migration); this sets the frame's SPSR.SS. `ctx`
            // is the on-stack trapframe the KERNEL_EXIT eret restores from (== SP).
            // A step-resume is IRQ-masked from here to the eret (KERNEL_EXIT masks
            // DAIF), so no preempt lands between arming SPSR.SS and the eret.
            if (t->debug_ss_armed && ctx)
                ctx->spsr |= (1ull << 21);   // SPSR_EL1.SS
            t->debug_trapframe = NULL;   // 8a-1c: no longer parked -> stop pointing at the (about-to-be-live) frame
            return;
        }

        // A SOFT interrupt-terminate latched while parked (LS-5c; group death
        // ruled out above, so thread_die_pending here is exactly the latch leg).
        // Its terminate-vs-handler-vs-mask resolution is notes_deliver's, not
        // ours; leave the park so the thread erets and delivers it at its next
        // checkpoint (standard interrupt checkpoint-delivery -- an interrupt
        // never preempts mid-EL0). Without this bail, sleep() would return
        // SLEEP_INTR every iteration on the still-set latch -> a livelock.
        if (thread_die_pending(t)) {
            t->debug_trapframe = NULL;   // 8a-1c: leaving the park to deliver the interrupt
            return;
        }

        // Park (specs/debug_stop.tla Acquire+RegisterObserve). sleep() registers
        // rendez_blocked_on = &t->debug_rendez under t->wait_lock, re-checks
        // stop_park_wake_cond under wait_lock+r->lock (serialized against
        // BOTH resumes' clear-before-walk -- the register-then-observe I-9
        // close), and returns SLEEP_INTR if this Proc is group-terminating (the
        // loop's death check fires next iteration). The rendez is THIS thread's
        // own (single-waiter -- a multi-thread target parks each thread on its
        // own debug_rendez, never a shared one).
        (void)sleep(&t->debug_rendez, stop_park_wake_cond, p);
    }
}

// 8c-2 stop-of-a-sleeper (DEBUG-FS-DESIGN 5c.2): the nested stop park a
// blocking sleep()/tsleep() detours into when a stop is pending on the
// caller's Proc -- a debugger stop OR (PTY-1f) a job-control stop; the detour
// gate reads proc_stop_requested's disjunction. This is el0_return_stop_-
// check's park reached from a blocking syscall instead of the EL0-return tail
// -- sleep on THIS thread's own debug_rendez until BOTH owners clear
// (stop_park_wake_cond), then return SLEEP_OK so the caller re-checks its
// ORIGINAL wait condition and re-blocks in place (the syscall is preserved on
// the stack; no unwind, no restart). Returns SLEEP_INTR if the Proc is
// group-terminating (or a soft interrupt-terminate latched) while stop-parked
// -- sleep()'s own thread_die_pending check catches it -- so the caller
// unwinds and the thread dies / delivers at its EL0-return tail: DEATH WINS
// over a stop, exactly as at the tail (pty_stop.tla DeathWinsOverJobStop on
// the job axis). sleep()'s `r != &debug_rendez` detour gate skips the
// stop-check for this nested park (r == debug_rendez here), so there is no
// recursion. specs/debug_stop.tla: the sleeper's register-then-observe park
// (StopWakesSleeper -> the handshake -> "stopped"); NoLostStop +
// DeathWinsOverStop + the EventuallyStopSettles liveness.
int proc_stop_sleeper_park(struct Thread *t) {
    // t is always current_thread() (magic-validated by sleep() before the detour
    // reaches here), so a corrupt t is a real invariant violation -- extinct
    // rather than return SLEEP_OK, which would make the sleep() detour re-invoke
    // this on the same corrupt t forever (a busy livelock). Matches sleep()'s own
    // "corrupted current" guard. (8c-2 close F3.)
    if (!t || t->magic != THREAD_MAGIC)
        extinction("proc_stop_sleeper_park: corrupt thread");
    return sleep(&t->debug_rendez, stop_park_wake_cond, t->proc);
}

// The stop-delivery wake cascade, shared by BOTH stop owners (the debugger's
// proc_debug_stop_deliver and PTY-1f's job stop): wake every SLEEPING peer so
// it returns from its blocking sleep()/tsleep(), re-observes the now-set stop
// flag under its wait_lock, and DETOURS to park on its own debug_rendez
// (8c-2 stop-of-a-sleeper, DEBUG-FS-DESIGN 5c.2 -- the multi-thread-Go-target
// fix: an idle futex-parked M never reaches the EL0-return tail on its own).
// This is proc_group_terminate's death-wake cascade VERBATIM, but the woken
// sleeper arms the sleep()-detour park instead of the die:
// torpor_wake_all_for_proc for the futex (torpor) waiters, then the per-peer
// wait_lock rendez wake for every other blocking sleep. The caller's RELEASE
// store of its stop flag happens-before each wake AND before each peer's
// wait_lock acquire here, so the woken sleeper's ACQUIRE-read of the flag
// under its wait_lock observes the stop -- no stop-wake is lost between the
// wake and the sleeper's detour re-park (register-then-observe, I-9;
// specs/debug_stop.tla StopWakesSleeper). A RUNNING peer reads NULL
// rendez_blocked_on and is skipped -- it stops at its tail via the IPI kick.
// wakeup() re-validates r->waiter under r->lock, so a peer already woken (or
// by torpor_wake_all) is a safe no-op; wait_lock held across wakeup pins a
// torpor waiter's stack rendez. LOCK CONTRACT: caller holds g_proc_table_lock
// (pins p->threads); order g_proc_table_lock -> wait_lock -> (wakeup: r->lock);
// torpor_lock strictly below g_proc_table_lock. The EL0-running-peer IPI kick
// is SEPARATE (smp_resched_others below) -- it is group-GLOBAL (a broadcast to
// every online CPU), so a fan-out over N members issues it ONCE after the walk
// (audit F2), not per-member.
static void proc_stop_wake_sleepers_locked(struct Proc *p) {
    torpor_wake_all_for_proc(p);
    for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
        irq_state_t ws = spin_lock_irqsave(&peer->wait_lock);
        struct Rendez *r = peer->rendez_blocked_on;
        if (r) wakeup(r);
        spin_unlock_irqrestore(&peer->wait_lock, ws);
    }
}

// The full single-target cascade: wake this Proc's sleepers, then kick any
// peer RUNNING at EL0 on another CPU so it traps to its IRQ-from-EL0 tail
// (0x480 -> el0_return_stop_check) and parks without waiting for a timer tick
// (broadcast; the periodic tick is the floor if an IPI is missed --
// proc_group_terminate step (4)'s proven delivery vehicle). Used by the
// single-target debugger deliver. The per-member job-stop fan uses
// proc_stop_wake_sleepers_locked + one trailing smp_resched_others (F2).
static void proc_stop_wake_cascade_locked(struct Proc *p) {
    proc_stop_wake_sleepers_locked(p);
    smp_resched_others();
}

void proc_debug_stop_deliver(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC) return;
    if (p == g_kproc) return;   // kproc is never debuggable (undebuggable at the gate too)

    // Set the stop flag BEFORE the kick (the I-9 shape, mirror of
    // proc_group_terminate's flag-set-before-walk): a thread reaching the tail
    // after this store observes the set flag in its ACQUIRE load and parks. A
    // thread already heading to the tail (returning from a syscall/handler)
    // observes it too. RELEASE pairs with el0_return_stop_check's ACQUIRE.
    __atomic_store_n(&p->debug_stop_req, 1u, __ATOMIC_RELEASE);

    // A whole-Proc stop SUPERSEDES any in-flight single-step (8a-2c F1): cancel
    // every thread's pending step so a step that a peer's bp/wp fire (or a
    // detach/re-attach) interrupted before its own EC 0x32 does not leave
    // debug_ss_armed set -> a spurious SPSR.SS armed into the NEXT resume ->
    // an unexpected one-instruction stop after a `continue`. Idempotent with the
    // normal step completion, which already cleared debug_ss_armed at its EC 0x32
    // (hwdebug_singlestep_from_el0) before reaching this deliver. Under
    // g_proc_table_lock (both callers hold it: the ctl `stop` verb via
    // proc_for_each, and proc_debug_fault_stop), so p->threads is stable; the
    // RELEASE store pairs with hwdebug_switch_in's + el0_return_stop_check's
    // ACQUIRE reads of debug_ss_armed. (v1.0 arms only the head thread, so this is
    // usually a one-element clear; the walk is future-proof for a per-thread step.)
    for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
        __atomic_store_n(&peer->debug_ss_armed, false, __ATOMIC_RELEASE);
        peer->debug_stepover_va = 0;
    }

    // The 8c-2 sleeper-wake + EL0 IPI kick, shared with the PTY-1f job stop
    // (proc_stop_wake_cascade_locked above carries the full rationale).
    proc_stop_wake_cascade_locked(p);
}

// proc_debug_fault_stop -- see proc.h. The EC-path (hardware fire) counterpart
// of proc_debug_stop_deliver: it takes g_proc_table_lock so it serializes with a
// concurrent detach / ctl-fd close (devproc_debug_walk_cb / devproc_debug_release
// _cb, which clear debug_owner + the hw table + proc_debug_resume under the same
// lock) and delivers the stop ONLY while a debugger still owns the slot. That
// closes SA-1: a fire that raced a detach and set debug_stop_req AFTER the
// detach's resume cleared it would park the target with no debugger left to
// resume it (specs/debug_stop.tla StopImpliesOwned). The debug_owner read is a
// plain field read under the lock (the same discipline as attach/detach); a NULL
// owner (detached in the window) is reported as "not delivered" so the caller
// falls back to the benign STALE-arm path. smp_resched_others() runs under the
// lock exactly as the ctl `stop` verb already does it (proc_for_each -> callback
// -> proc_debug_stop_deliver -> smp_resched_others).
bool proc_debug_fault_stop(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC) return false;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    bool deliver = (p->debug_owner != NULL);
    if (deliver)
        proc_debug_stop_deliver(p);   // caller (this frame) now holds g_proc_table_lock
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return deliver;
}

void proc_debug_resume(struct Proc *p) {
    if (!p || p->magic != PROC_MAGIC) return;

    // PTY-1f (pty_stop.tla ResumeDebug): this clears ONLY the debug owner.
    // A woken thread whose Proc is ALSO job-stopped re-checks the park cond
    // (both flags) and re-parks -- job_stop_req is proc_job_resume's to
    // clear, never this path's (the per-owner separation StopCompatI39 /
    // BUGGY_DOUBLE_STOP pin).

    // Clear the stop flag BEFORE the wake walk (specs/debug_stop.tla StartResume:
    // sflag' = FALSE armed before the wake). This is the register-then-observe
    // I-9 close mirrored on the CLEAR: a thread that registers on its debug_rendez
    // at the tail AFTER this walk re-observes the cleared flag under its wait_lock
    // (via the wait_lock release/acquire sync with this walk) and proceeds without
    // parking; a thread that registered BEFORE the walk is found + woken below.
    __atomic_store_n(&p->debug_stop_req, 0u, __ATOMIC_RELEASE);

    // Wake every thread parked on its OWN debug_rendez -- the #811 death-cascade
    // shape (walk p->threads, per-peer wait_lock -> read rendez_blocked_on ->
    // wakeup), but waking ONLY debug-parked peers (rendez_blocked_on ==
    // &peer->debug_rendez), never a peer legitimately sleeping in a syscall.
    // wait_lock is held across wakeup (Option A, ARCH 8.8.1): it pins the peer so
    // its debug_rendez cannot be resumed + reused under the waker. Lock order
    // g_proc_table_lock (caller-held) -> wait_lock -> (wakeup) g_timerwait.lock ->
    // r->lock; acyclic (only the owner WRITES rendez_blocked_on).
    for (struct Thread *peer = p->threads; peer; peer = peer->next_in_proc) {
        irq_state_t ws = spin_lock_irqsave(&peer->wait_lock);
        if (peer->rendez_blocked_on == &peer->debug_rendez)
            wakeup(&peer->debug_rendez);
        spin_unlock_irqrestore(&peer->wait_lock, ws);
    }
}

// =============================================================================
// PTY-1f: the job-control stop (I-20 stop leg; PTY-DESIGN.md section 4;
// specs/pty_stop.tla). The SECOND stop owner beside the debugger's -- the
// SAME park (debug_rendez + el0_return_stop_check + the sleep/tsleep detour,
// whose predicate is proc_stop_requested's disjunction), its OWN flag
// (job_stop_req), per-owner set/clear (StopCompatI39 / BUGGY_DOUBLE_STOP).
// All helpers run under g_proc_table_lock (the fan-outs take it; the
// notes_post_pgrp precedent covers every nested edge: q->lock for the posts,
// torpor_lock + wait_lock -> r->lock for the cascade, the poll-waiter list
// for the parent wake).
// =============================================================================

// Stop ONE member: set the job owner + latch the PTY-1e stop report + run the
// stop-delivery cascade + wake the parent's wait. Idempotent on an already-
// job-stopped Proc (a second Ctrl-Z on a stopped process is discarded --
// POSIX; the report latch is NOT re-armed). The report latches are mutated
// under g_proc_table_lock -- the same lock wait_pid_for's report arm consumes
// them under -- and each stop supersedes any unreported cont (latest-state
// reporting: a parent that never saw the cont sees only the current stop).
// Returns true iff it stopped `m` (a fresh stop -- the caller issues ONE
// group-global reschedule IPI after the fan if any member stopped, F2).
static bool proc_job_stop_one_locked(struct Proc *m) {
    if (!m || m->magic != PROC_MAGIC) return false;
    if (m == g_kproc) return false;   // the boot Proc is never a job-stop target
    if (__atomic_load_n(&m->job_stop_req, __ATOMIC_ACQUIRE) != 0) return false;
    __atomic_store_n(&m->job_stop_req, 1u, __ATOMIC_RELEASE);
    m->stop_report_pending = true;
    m->cont_report_pending = false;
    proc_stop_wake_sleepers_locked(m);   // the IPI is hoisted to the fan (F2)
    // The PTY-1e WAIT_UNTRACED edge: a parent parked in wait_pid_for
    // re-scans on any child_waiters wake (the latch was set above under the
    // SAME lock its re-scan takes -- register-then-observe holds exactly as
    // for the zombie wake).
    if (m->parent) poll_waiter_list_wake(&m->parent->child_waiters);
    return true;
}

// Resume ONE member: the job owner's OWN clear (pty_stop.tla ResumeJob) +
// the PTY-1e cont report + the debug_rendez wake walk. A member ALSO
// debugger-stopped is woken but re-parks on the still-set debug flag (the
// park cond checks both owners) -- a job resume can NEVER run a
// debugger-stopped thread (StopCompatI39; the BUGGY_DOUBLE_STOP
// counterexample is exactly a resume that clears both). No-op for a Proc
// that is not job-stopped (the cont note still gets posted by the fan-out;
// only the resume machinery is stop-gated).
static void proc_job_resume_one_locked(struct Proc *m) {
    if (!m || m->magic != PROC_MAGIC) return;
    if (__atomic_load_n(&m->job_stop_req, __ATOMIC_ACQUIRE) == 0) return;
    // Clear BEFORE the wake walk (the I-9 register-then-observe close on the
    // CLEAR -- proc_debug_resume's discipline verbatim, on the job owner).
    __atomic_store_n(&m->job_stop_req, 0u, __ATOMIC_RELEASE);
    m->cont_report_pending = true;
    m->stop_report_pending = false;   // an unreported stop is superseded
    for (struct Thread *peer = m->threads; peer; peer = peer->next_in_proc) {
        irq_state_t ws = spin_lock_irqsave(&peer->wait_lock);
        if (peer->rendez_blocked_on == &peer->debug_rendez)
            wakeup(&peer->debug_rendez);
        spin_unlock_irqrestore(&peer->wait_lock, ws);
    }
    if (m->parent) poll_waiter_list_wake(&m->parent->child_waiters);
}

// The tty:susp catchability gate (round-2 R2-F3; the LS-5
// notes_interrupt_should_terminate_locked analog, evaluated at POST time
// because the stop -- unlike the terminate -- is applied post-side, not at
// the tail): the default STOP fires only when the target has no async
// handler, is not self-managing (no notes fd -- it would read + act on the
// susp itself), and at least one thread leaves NOTE_BIT_TTY unmasked (the
// POSIX any-thread-unblocked delivery shape; all-masked defers to a
// note-only post). handler_va / proc_flags are read lock-free -- a handler
// registered concurrently with the decision orders before-or-after it,
// indistinguishable from the signal arriving a moment earlier (the POSIX
// signal race; the LS-5c latch coherence concern does not apply since no
// latch is armed here). The thread walk is pinned by the caller's
// g_proc_table_lock; note_mask is owner-written (SYS_NOTE_MASK), so the
// cross-thread load is the same benign-race read, made explicit atomic.
static bool proc_tty_susp_would_stop_locked(struct Proc *m) {
    if (__atomic_load_n(&m->handler_va, __ATOMIC_ACQUIRE) != 0) return false;
    if (proc_is_self_managing_notes(m)) return false;
    for (struct Thread *th = m->threads; th; th = th->next_in_proc) {
        if ((__atomic_load_n(&th->note_mask, __ATOMIC_RELAXED) &
             (1ull << NOTE_BIT_TTY)) == 0)
            return true;
    }
    return false;   // every thread masks the tty family (or no threads)
}

// The POSIX orphaned-pgrp predicate: `pgid` is orphaned iff NO ALIVE member
// has a parent that is ALIVE, in the SAME session, and in ANOTHER group --
// i.e. no shell-shaped process remains that could resume it. `excl` (may be
// NULL) is a Proc treated as ALREADY DEAD, both as a member and as a parent
// -- the proc_become_zombie_locked caller evaluates "orphaned once I am
// gone" BEFORE its state flip + reparent (a child whose parent is the dying
// Proc counts as parentless, exactly what the init-reparent makes true).
// Caller holds g_proc_table_lock (membership + parent edges stable).
struct pgrp_orphan_ctx { u32 pgid; const struct Proc *excl; bool anchored; };
static int pgrp_orphan_cb(struct Proc *q, void *arg) {
    struct pgrp_orphan_ctx *c = arg;
    if (q->state != PROC_STATE_ALIVE || q->pgid != c->pgid) return 0;
    if (q == c->excl) return 0;
    const struct Proc *par = q->parent;
    if (par && par != c->excl && par->state == PROC_STATE_ALIVE &&
        par->sid == q->sid && par->pgid != c->pgid) {
        c->anchored = true;
        return 1;   // an anchor exists -- not orphaned; stop the walk
    }
    return 0;
}
static bool pgrp_orphaned_locked(u32 pgid, const struct Proc *excl) {
    struct pgrp_orphan_ctx ctx = { .pgid = pgid, .excl = excl,
                                   .anchored = false };
    proc_for_each_walk(g_kproc, pgrp_orphan_cb, &ctx);
    return !ctx.anchored;
}

// Any ALIVE member of `pgid` (excluding `excl`) currently job-stopped?
// The orphan rule fires only for groups with stopped members. Lock held.
struct pgrp_stopped_ctx { u32 pgid; const struct Proc *excl; bool stopped; };
static int pgrp_stopped_cb(struct Proc *q, void *arg) {
    struct pgrp_stopped_ctx *c = arg;
    if (q->state == PROC_STATE_ALIVE && q->pgid == c->pgid && q != c->excl &&
        __atomic_load_n(&q->job_stop_req, __ATOMIC_ACQUIRE) != 0) {
        c->stopped = true;
        return 1;
    }
    return 0;
}
static bool pgrp_has_job_stopped_locked(u32 pgid, const struct Proc *excl) {
    struct pgrp_stopped_ctx ctx = { .pgid = pgid, .excl = excl,
                                    .stopped = false };
    proc_for_each_walk(g_kproc, pgrp_stopped_cb, &ctx);
    return ctx.stopped;
}

// The orphan rule's delivery half (POSIX 2.4.3 / _exit(): a newly-orphaned
// pgrp with stopped members gets SIGHUP followed by SIGCONT, per process):
// per ALIVE member (excluding `excl`), post tty:hup (which arms the
// terminate latch for an uncaught target -- notes_arm_intr_terminate_locked
// inside notes_post -- and the terminate-wake unwinds its blocked threads to
// die at their tails; a stop-parked thread's park loop bails on
// thread_die_pending and dies too: DEATH WINS from inside a stop), then post
// tty:cont + job-resume it (so a hup-catching survivor actually runs).
// The hup-then-cont per-member order is POSIX's. Lock held by caller.
struct pgrp_hupcont_ctx { u32 pgid; const struct Proc *excl; };
static int pgrp_hupcont_cb(struct Proc *q, void *arg) {
    struct pgrp_hupcont_ctx *c = arg;
    if (q->state != PROC_STATE_ALIVE || q->pgid != c->pgid || q == c->excl)
        return 0;
    (void)notes_post(q, NOTE_NAME_TTY_HUP, 0u, NULL, true);
    proc_interrupt_terminate_wake(q);
    (void)notes_post(q, NOTE_NAME_TTY_CONT, 0u, NULL, true);
    proc_job_resume_one_locked(q);
    return 0;
}
static void pgrp_orphan_hup_cont_locked(u32 pgid, const struct Proc *excl) {
    struct pgrp_hupcont_ctx ctx = { .pgid = pgid, .excl = excl };
    proc_for_each_walk(g_kproc, pgrp_hupcont_cb, &ctx);
}

// The death-side orphan-rule hook (POSIX 2.4.3), called from
// proc_become_zombie_locked BEFORE the reparent (the children list is still
// `dying`'s; `dying` is excluded from every walk as already-dead). A death
// can newly-orphan (a) each distinct group among its ALIVE children -- iff
// the dying Proc itself anchored that group (same session, another group;
// a non-anchoring parent's death changes nothing, and re-signalling an
// ALREADY-orphaned group would deliver a spurious, possibly lethal hup) --
// and (b) its OWN group, iff the dying Proc's own parent-edge anchored it.
// For each candidate that is orphaned-once-dying-is-gone AND has a
// job-stopped member: the hup+cont fan. Children are deduped by
// first-sibling-with-this-pgid (no allocation; children counts are
// PROC_CHILD_MAX-bounded). The table walks are O(procs) per candidate --
// death is not a hot path, and the fan itself is the rare case.
static void proc_orphan_rule_locked(struct Proc *dying) {
    if (!dying) return;
    // (a) the children's groups.
    for (struct Proc *c = dying->children; c; c = c->sibling) {
        if (c->state != PROC_STATE_ALIVE) continue;
        u32 g = c->pgid;
        if (g == 0) continue;
        // The dying Proc anchored this group? (Same session, another group.)
        if (dying->sid != c->sid || dying->pgid == g) continue;
        // Dedup: only the FIRST child carrying this pgid does the work.
        bool seen = false;
        for (struct Proc *prev = dying->children; prev != c;
             prev = prev->sibling) {
            if (prev->state == PROC_STATE_ALIVE && prev->pgid == g) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        if (pgrp_orphaned_locked(g, dying) &&
            pgrp_has_job_stopped_locked(g, dying))
            pgrp_orphan_hup_cont_locked(g, dying);
    }
    // (b) the dying Proc's own group: its removal severs the (dying,
    // dying->parent) anchoring edge -- newly-orphaning iff that edge
    // anchored and no other member's does.
    const struct Proc *par = dying->parent;
    if (par && par->state == PROC_STATE_ALIVE && par->sid == dying->sid &&
        par->pgid != dying->pgid && dying->pgid != 0) {
        if (pgrp_orphaned_locked(dying->pgid, dying) &&
            pgrp_has_job_stopped_locked(dying->pgid, dying))
            pgrp_orphan_hup_cont_locked(dying->pgid, dying);
    }
}

// The TSTP fan-out (SYS_TTY_SIGNAL's suspend class routes here from
// pts_tty_signal, with g_pts_lock RELEASED). Full contract in proc.h.
struct job_stop_ctx { u32 pgid; bool orphaned; int affected; bool any_stopped; };
static int job_stop_cb(struct Proc *q, void *arg) {
    struct job_stop_ctx *c = arg;
    if (q->state != PROC_STATE_ALIVE || q->pgid != c->pgid) return 0;
    if (!proc_tty_susp_would_stop_locked(q)) {
        // CAUGHT (handler / self-managing / all-masked): the note delivers
        // on the target's own terms; NO stop (round-2 R2-F3 -- tmux/bash
        // catch SIGTSTP to save terminal state; an uncatchable susp would
        // fail PTY-4's own gate).
        if (notes_post(q, NOTE_NAME_TTY_SUSP, 0u, NULL, true) == 0)
            c->affected++;
    } else if (!c->orphaned) {
        // UNCAUGHT + resumable: the default STOP consumes the signal.
        if (proc_job_stop_one_locked(q)) c->any_stopped = true;
        c->affected++;
    }
    // UNCAUGHT + orphaned: discarded entirely (the POSIX orphan rule's
    // stop-suppression half -- nobody could resume it).
    return 0;
}
int proc_job_stop_pgrp(u32 pgid) {
    if (pgid == 0) return 0;
    struct job_stop_ctx ctx = { .pgid = pgid, .orphaned = false,
                                .affected = 0, .any_stopped = false };
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    ctx.orphaned = pgrp_orphaned_locked(pgid, NULL);
    proc_for_each_walk(g_kproc, job_stop_cb, &ctx);
    // ONE group-global reschedule IPI after every member's job_stop_req is set
    // (F2): a peer RUNNING at EL0 on another CPU traps to its tail and observes
    // the set flag. Issued only if a member actually stopped (a pure note fan
    // needs no kick). Every stopped member's OWN sleepers were already woken
    // per-member inside proc_job_stop_one_locked.
    if (ctx.any_stopped) smp_resched_others();
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return ctx.affected;
}

// The tty:cont fan-out (SYS_TTY_CONT / the F8 teardown resume). Full
// contract in proc.h.
struct job_cont_ctx { u32 pgid; int visited; };
static int job_cont_cb(struct Proc *q, void *arg) {
    struct job_cont_ctx *c = arg;
    if (q->state != PROC_STATE_ALIVE || q->pgid != c->pgid) return 0;
    (void)notes_post(q, NOTE_NAME_TTY_CONT, 0u, NULL, true);
    proc_job_resume_one_locked(q);
    c->visited++;
    return 0;
}
int proc_job_cont_pgrp(u32 pgid) {
    if (pgid == 0) return 0;
    struct job_cont_ctx ctx = { .pgid = pgid, .visited = 0 };
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    proc_for_each_walk(g_kproc, job_cont_cb, &ctx);
    spin_unlock_irqrestore(&g_proc_table_lock, s);
    return ctx.visited;
}

// child_wait_ready_cond — wait_pid_for's sleep predicate (#344). Returns 1
// iff the caller's OWN stack `poll_waiter` has its `ready` flag set, i.e. a
// child of this Proc entered ZOMBIE (or was adopted ZOMBIE) and the wake site
// (proc_become_zombie_locked / proc_reparent_children) ran poll_waiter_list_-
// wake on `child_waiters`. Reads ONLY `pw->ready` (a flag) -- it touches NO
// lineage state, so unlike the retired `wait_pid_cond` it adds nothing to the
// lock order and cannot invert proc_table_lock (the deadlock the old design
// fought; see the g_proc_table_lock declaration block).
//
// Called under the caller's OWN private rendez lock (sleep's discipline). The
// producer's set-ready runs under `child_waiters`' list lock, but its
// intervening wakeup(rendez) takes THIS rendez lock -- so the release/acquire
// on the rendez lock makes `ready` visible to this cond re-check. That is the
// poll.c register-then-observe chain verbatim (specs/poll.tla; <thylacine/
// poll.h>). `ready` is the wake RELAY only; the authoritative reapable-zombie
// decision is wait_pid_for's re-scan of p->children under proc_table_lock, so
// a spurious or non-matching wake simply re-scans and re-parks.
static int child_wait_ready_cond(void *arg) {
    const struct poll_waiter *pw = (const struct poll_waiter *)arg;
    return pw->ready ? 1 : 0;
}

int wait_pid_for(int want_pid, int flags, int *status_out) {
    struct Thread *t = current_thread();
    if (!t)                  extinction("wait_pid with no current thread");
    struct Proc *p = t->proc;
    if (!p)                  extinction("wait_pid with no proc");
    if (p->magic != PROC_MAGIC)
                             extinction("wait_pid with corrupted proc");

    const bool nohang = (flags & WAIT_WNOHANG) != 0;
    // PTY-1e: either job-control flag opts the caller into the packed
    // status encoding (a plain wait keeps the raw exit status -- full
    // compatibility for every pre-PTY caller).
    const bool want_stops = (flags & WAIT_UNTRACED)  != 0;
    const bool want_conts = (flags & WAIT_CONTINUED) != 0;
    const bool packed     = want_stops || want_conts;
    // The pgrp selector's group, computed in s64 so want_pid == INT_MIN
    // cannot overflow the negation.
    const u32 want_pgid_sel = (want_pid < -1) ? (u32)(-(s64)want_pid) : 0u;

    // #344 multi-waiter wait. THIS Thread's OWN private rendez + stack
    // poll_waiter. ANY number of a Proc's Threads may be in wait_pid_for
    // concurrently -- each registers its own waiter on `child_waiters` and
    // parks on its own rendez, the wake fans out to all, exactly one reaps each
    // zombie. This RETIRES the RW-2 2B-F1/F2 `wait_active` guard that refused a
    // 2nd concurrent waiter (-1) -- the refusal that broke multi-threaded Go's
    // parallel `go build` (#342). rendez_init once; poll_waiter_init is re-done
    // per park to clear `ready` (the wake-RELAY flag) before each sleep.
    struct Rendez self_rendez;
    rendez_init(&self_rendez);
    struct poll_waiter pw;
    poll_waiter_init(&pw, &self_rendez);

    int ret;
    for (;;) {
        // P3-A: walk + unlink + state capture under g_proc_table_lock.
        // Atomic with concurrent exits() on our children — they hold the
        // same lock during their ZOMBIE transition, so we either see
        // the pre-transition state (no zombie yet → sleep) or the post-
        // transition state (zombie ready to reap).
        irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);

        // Single scan: find a MATCHING zombie to reap (exit outranks the
        // PTY-1e reports) + the first reportable latched child; also note
        // whether any matching child exists at all. No matching child ->
        // -1; matching-but-nothing-ready -> sleep (or 0 under WNOHANG).
        //
        // The selectors (PTY-1e): -1 = any; > 0 = that child; 0 = any
        // child in the CALLER's group (p->pgid read here, under the same
        // lock setpgid mutates it -- the POSIX at-each-check reading);
        // < -1 = any child in group -want_pid. A child setpgid-leaving the
        // waited group simply stops matching at this authoritative
        // re-scan (the R2-F6 mid-wait obligation).
        struct Proc *zombie   = NULL;
        struct Proc *reportee = NULL;
        bool report_cont = false;
        bool any_match   = false;
        for (struct Proc *c = p->children; c; c = c->sibling) {
            bool match;
            if      (want_pid > 0)   match = (c->pid == want_pid);
            else if (want_pid == -1) match = true;
            else if (want_pid == 0)  match = (c->pgid == p->pgid);
            else                     match = (c->pgid == want_pgid_sel);
            if (!match) continue;
            any_match = true;
            if (c->state == PROC_STATE_ZOMBIE) {
                zombie = c;
                break;                       // exit outranks every report
            }
            if (!reportee && c->state == PROC_STATE_ALIVE) {
                // Continue outranks stop (proc.h precedence note); each
                // arm only fires when its flag requested it, so a plain
                // wait neither sees nor consumes a latch.
                if (want_conts && c->cont_report_pending) {
                    reportee = c; report_cont = true;
                } else if (want_stops && c->stop_report_pending) {
                    reportee = c; report_cont = false;
                }
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
            int status = packed ? WAIT_STATUS_EXITED(zombie->exit_status)
                                : zombie->exit_status;

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

        if (reportee) {
            // PTY-1e report-is-not-reap (R2-F6): return the pid + packed
            // status, consume the latch exactly once (under the same lock
            // the 1f setters take), run NONE of the reap teardown -- the
            // child stays linked + ALIVE, reapable by a later wait.
            int pid = reportee->pid;
            if (report_cont) reportee->cont_report_pending = false;
            else             reportee->stop_report_pending = false;
            spin_unlock_irqrestore(&g_proc_table_lock, s);
            if (status_out)
                *status_out = report_cont ? WAIT_STATUS_CONTINUED
                                          : WAIT_STATUS_STOPPED;
            ret = pid;
            goto out;
        }

        // A matching child is alive but not yet a zombie.
        if (nohang) {
            // WAIT_WNOHANG: report "not ready" without blocking.
            spin_unlock_irqrestore(&g_proc_table_lock, s);
            ret = 0;
            goto out;
        }

        // #344 register-then-observe (the poll.c discipline; I-9). Install
        // THIS Thread's stack waiter on child_waiters BEFORE releasing
        // g_proc_table_lock -- atomic with the no-zombie scan above. A child
        // that becomes ZOMBIE *after* our release must take g_proc_table_lock
        // (after us), so it finds this registered waiter and sets pw->ready +
        // wakes; a child already ZOMBIE was seen by the scan. So no death-wake
        // is lost. The previous iteration's unregister (or the pre-loop init)
        // left pw->list == NULL, so this register is clean (no double-register);
        // the re-init clears `ready` so a stale wake from a prior iteration
        // cannot make this park spin.
        poll_waiter_init(&pw, &self_rendez);
        poll_waiter_list_register(&p->child_waiters, &pw);
        spin_unlock_irqrestore(&g_proc_table_lock, s);

        // Park on our OWN rendez; child_wait_ready_cond reads ONLY pw->ready.
        // On any wake we unregister + loop to re-scan under the lock -- the
        // re-scan is the authoritative readiness check, so a spurious or
        // non-matching wake simply re-parks.
        // #811 (ARCH §8.8.1): a death-interrupted sleep means THIS Proc is
        // group-terminating (a peer / kill flagged it while we waited on a
        // child). Unregister, then return so the waiting Thread unwinds to its
        // EL0-return die-check; do NOT loop (re-sleep would re-INTR = livelock).
        int sl = sleep(&self_rendez, child_wait_ready_cond, &pw);
        poll_waiter_list_unregister(&pw);
        if (sl == SLEEP_INTR) {
            ret = -1;
            goto out;
        }
    }
out:
    // #344 defensive unregister (poll.tla NoStaleHook). Every out: path leaves
    // pw->list == NULL today (the no-match / zombie-reap / nohang paths never
    // registered; the SLEEP_INTR path unregistered above), so this is a no-op
    // -- but it guarantees no stack waiter outlives this frame even if a future
    // edit adds a goto from within the registered window. pw is always
    // initialized (poll_waiter_init before the loop), so this is safe.
    poll_waiter_list_unregister(&pw);
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

// proc_test_link_child -- link `p` as a child of `parent` (the PTY-1f
// orphan-rule tests fabricate the parent-edges the rule's anchor
// qualification reads: a group is anchored by an ALIVE same-session
// out-of-group PARENT of a member). Same discipline as proc_test_link; a
// synthetic child linked under a REAL parent is reparented to kproc by that
// parent's death, after which proc_test_unlink (which searches kproc's
// list) cleans it up.
void proc_test_link_child(struct Proc *parent, struct Proc *p) {
    if (!parent || !p || p->magic != PROC_MAGIC)
        extinction("proc_test_link_child: NULL or corrupted Proc");
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    proc_link_child(parent, p);
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

// proc_test_unlink — remove `p` from its parent's children list (kproc for
// a proc_test_link'd Proc; the fabricated parent for a
// proc_test_link_child'd one). The test MUST call this before freeing `p`
// so the table holds no dangling pointer.
void proc_test_unlink(struct Proc *p) {
    if (!p) return;
    irq_state_t s = spin_lock_irqsave(&g_proc_table_lock);
    struct Proc *par = p->parent ? p->parent : kproc();
    for (struct Proc **pp = &par->children; *pp; pp = &(*pp)->sibling) {
        if (*pp == p) {
            *pp = p->sibling;
            // #65 (I-32): keep the parent's child_count == list length
            // (proc_test_link / _link_child bumped it via proc_link_child).
            if (__atomic_load_n(&par->child_count, __ATOMIC_RELAXED) > 0)
                __atomic_fetch_sub(&par->child_count, 1u, __ATOMIC_RELEASE);
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
