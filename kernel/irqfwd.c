// IRQ forwarding — KObj_IRQ lifecycle + GIC dispatch hook (P4-G).
//
// Per <thylacine/irqfwd.h> + ARCH §9.3. The data path: GIC delivers
// IRQ → arch/arm64/exception.c IRQ vector → gic_dispatch → registered
// handler (kobj_irq_dispatch) → wakeup the KObj_IRQ's Rendez. The
// driver thread (kernel-internal at v1.0; userspace at P4-I+) returns
// from kobj_irq_wait with the collapsed-count of IRQs that fired since
// its last wait.
//
// Wait/wake atomicity follows the scheduler.tla protocol pinned by
// NoMissedWakeup (I-9): pending_count is mutated under the Rendez lock
// AND the wake transition happens after the count update is visible.
// The lock is taken in:
//   - kobj_irq_dispatch: increment pending_count + drop, then wakeup.
//   - kobj_irq_wait::cond:  read pending_count under r->lock (sleep
//     calls cond with the lock held).
//   - kobj_irq_wait post-sleep: re-take r->lock to atomically read +
//     zero pending_count (an IRQ that fires between sleep's return
//     and our zeroing must NOT be lost).

#include <thylacine/extinction.h>
#include <thylacine/irqfwd.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#include "../arch/arm64/gic.h"
#include "../mm/slub.h"

// =============================================================================
// Diagnostic counters.
// =============================================================================

static u64 g_irq_total_fires;
static u64 g_kobj_irq_live;

u64 kobj_irq_total_fires(void) {
    return __atomic_load_n(&g_irq_total_fires, __ATOMIC_RELAXED);
}

u64 kobj_irq_live_count(void) {
    return __atomic_load_n(&g_kobj_irq_live, __ATOMIC_RELAXED);
}

// =============================================================================
// GIC dispatch hook.
// =============================================================================
//
// Called from arch/arm64/gic.c::gic_dispatch when an attached IRQ
// fires. The arg is the KObj_IRQ pointer that gic_attach was called
// with. Increments pending_count under the Rendez lock + wakes any
// blocked waiter. Both lock/unlock + the wakeup are IRQ-context safe.

static void kobj_irq_dispatch(u32 intid, void *arg) {
    (void)intid;        // intid is the registered IRQ; we trust it
    struct KObj_IRQ *k = (struct KObj_IRQ *)arg;
    if (!k || k->magic != KOBJ_IRQ_MAGIC) return;

    // Order: increment under r->lock + DROP the lock + then wakeup
    // (which RE-takes r->lock). Holding the lock through wakeup would
    // deadlock-by-recursion since wakeup wants the same lock.
    irq_state_t s = spin_lock_irqsave(&k->rendez.lock);
    k->pending_count++;
    spin_unlock_irqrestore(&k->rendez.lock, s);

    __atomic_fetch_add(&g_irq_total_fires, 1u, __ATOMIC_RELAXED);

    wakeup(&k->rendez);
}

// =============================================================================
// Lifecycle.
// =============================================================================

struct KObj_IRQ *kobj_irq_create(u32 intid) {
    struct KObj_IRQ *k = kmalloc(sizeof(*k), KP_ZERO);
    if (!k) return NULL;

    k->magic         = KOBJ_IRQ_MAGIC;
    k->intid         = intid;
    k->ref           = 1;
    rendez_init(&k->rendez);
    k->pending_count = 0;

    // Register handler + enable the IRQ. gic_attach binds (intid →
    // dispatch + arg); gic_enable_irq unmasks it on the CPU's
    // redistributor (for SGIs/PPIs) or distributor (for SPIs).
    if (!gic_attach(intid, kobj_irq_dispatch, k)) {
        kfree(k);
        return NULL;
    }
    if (!gic_enable_irq(intid)) {
        gic_attach(intid, NULL, NULL);
        kfree(k);
        return NULL;
    }

    __atomic_fetch_add(&g_kobj_irq_live, 1u, __ATOMIC_RELAXED);
    return k;
}

void kobj_irq_ref(struct KObj_IRQ *k) {
    if (!k)                          extinction("kobj_irq_ref(NULL)");
    if (k->magic != KOBJ_IRQ_MAGIC)  extinction("kobj_irq_ref of corrupted KObj_IRQ");
    if (k->ref <= 0)
        extinction("kobj_irq_ref of zero-ref KObj_IRQ (already freed?)");

    k->ref++;
}

static void kobj_irq_free_internal(struct KObj_IRQ *k) {
    if (k->magic != KOBJ_IRQ_MAGIC)
        extinction("kobj_irq_free_internal of corrupted KObj_IRQ");
    if (k->ref != 0)
        extinction("kobj_irq_free_internal with ref > 0");

    // Disable the IRQ first so no more fires arrive.
    gic_disable_irq(k->intid);
    // Unregister so a stale fire post-free doesn't dispatch into a
    // freed KObj_IRQ. gic_attach(intid, NULL, NULL) is the convention
    // for clearing.
    gic_attach(k->intid, NULL, NULL);

    // Defensive: clobber magic so a stale-pointer dereference between
    // free and SLUB-list-write extincts on the magic check.
    k->magic = 0;

    kfree(k);
    __atomic_fetch_sub(&g_kobj_irq_live, 1u, __ATOMIC_RELAXED);
}

void kobj_irq_unref(struct KObj_IRQ *k) {
    if (!k) return;
    if (k->magic != KOBJ_IRQ_MAGIC)
        extinction("kobj_irq_unref of corrupted KObj_IRQ");
    if (k->ref <= 0) extinction("kobj_irq_unref of zero-ref KObj_IRQ");

    k->ref--;
    if (k->ref == 0) {
        kobj_irq_free_internal(k);
    }
}

void kobj_irq_destroy(struct KObj_IRQ *k) {
    kobj_irq_unref(k);
}

// =============================================================================
// Wait.
// =============================================================================

// sleep's cond predicate. Called under k->rendez.lock by the Rendez
// machinery; safe to read pending_count without re-locking.
static int kobj_irq_pending_cond(void *arg) {
    struct KObj_IRQ *k = (struct KObj_IRQ *)arg;
    return k->pending_count > 0;
}

u32 kobj_irq_wait(struct KObj_IRQ *k) {
    if (!k) return 0;
    if (k->magic != KOBJ_IRQ_MAGIC)
        extinction("kobj_irq_wait of corrupted KObj_IRQ");

    // Block until pending_count > 0. sleep's cond loop guarantees no
    // spurious return.
    sleep(&k->rendez, kobj_irq_pending_cond, k);

    // Re-take the lock to atomically read + zero pending_count. An IRQ
    // that fires between sleep's return and this read MUST NOT be lost
    // — it'll be reflected in the next wait, but `count` returned here
    // captures only the IRQs that arrived BEFORE the lock acquire.
    irq_state_t s = spin_lock_irqsave(&k->rendez.lock);
    u32 count = k->pending_count;
    k->pending_count = 0;
    spin_unlock_irqrestore(&k->rendez.lock, s);
    return count;
}
