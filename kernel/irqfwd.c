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

#include <thylacine/smp.h>                  // IPI_RESCHED (P4-Ib R9 F142)

#include "../arch/arm64/gic.h"
#include "../arch/arm64/timer.h"            // TIMER_INTID_EL1_PHYS_NS
#include "../arch/arm64/uart.h"
#include "../mm/slub.h"

// =============================================================================
// INTID claim tracking (P4-Ib).
// =============================================================================
//
// The GIC has one handler slot per INTID; gic_attach silently overwrites
// when a handler is already registered. Without explicit claim tracking,
// two callers of kobj_irq_create on the same INTID would both succeed —
// the second's attach overwrites the first's, leaving the first KObj_IRQ
// alive but never receiving IRQs (and the dispatcher arg points at the
// SECOND KObj_IRQ). That's a HwResourceExclusive violation in spec terms.
//
// Fix: maintain a per-INTID claimed bitmap under g_intid_lock. Reject
// kobj_irq_create if the INTID is already claimed. Clear on
// kobj_irq_free_internal. The static-array form (one bool per INTID,
// 1020 bytes) is wasteful vs. a bitmap but keeps the code obvious; the
// memory is negligible.

static bool        g_intid_claimed[GIC_NUM_INTIDS];
static spin_lock_t g_intid_lock = SPIN_LOCK_INIT;

// Try to claim `intid`. Returns true on success (caller now owns it),
// false if already claimed or out of range.
//
// R12-gic-edge audit close (F205 P3): bound against runtime
// g_max_intid (from GICD_TYPER.ITLinesNumber) rather than the
// architectural GIC_NUM_INTIDS = 1020. ICFGR / ISENABLER writes
// beyond the implementation's actual line count are UNPREDICTABLE
// per IHI 0069 §12.9.7. Without this tighter bound, a syscall caller
// with CAP_HW_CREATE could pass intid in (g_max_intid, GIC_NUM_INTIDS]
// and reach the GIC helpers with an unimplemented INTID. The
// architectural-max bound is preserved as defense-in-depth (against
// gic_max_intid() returning > GIC_NUM_INTIDS - 1, which dist_init's
// clamp at gic.c:234 already prevents).
static bool intid_try_claim(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) return false;
    if (intid > gic_max_intid()) return false;
    irq_state_t s = spin_lock_irqsave(&g_intid_lock);
    if (g_intid_claimed[intid]) {
        spin_unlock_irqrestore(&g_intid_lock, s);
        return false;
    }
    g_intid_claimed[intid] = true;
    spin_unlock_irqrestore(&g_intid_lock, s);
    return true;
}

// Release a previously-claimed INTID.
static void intid_release(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) return;
    irq_state_t s = spin_lock_irqsave(&g_intid_lock);
    g_intid_claimed[intid] = false;
    spin_unlock_irqrestore(&g_intid_lock, s);
}

// R9 F142 (P0) close: reserve INTIDs owned by kernel-internal callers
// that bypass kobj_irq_create + the intid_try_claim guard. At v1.0:
//   - SGI 0 (IPI_RESCHED): cross-CPU wake; attached at smp_per_cpu_main.
//   - PPI 30 (TIMER_INTID_EL1_PHYS_NS): timer tick; attached at
//     boot_main via gic_attach directly.
// Without reservation, a syscall caller with CAP_HW_CREATE could pass
// either INTID to SYS_IRQ_CREATE and overwrite the kernel's handler.
// SYS_IRQ_CREATE additionally enforces intid >= 32 (F145), but this
// reservation closes the kernel-internal path as defense-in-depth and
// keeps the spec invariant HwResourceExclusive sound even if some
// future refactor exposes the SGI/PPI range.
void irqfwd_init(void) {
    irq_state_t s = spin_lock_irqsave(&g_intid_lock);
    g_intid_claimed[IPI_RESCHED]             = true;     // SGI 0
    g_intid_claimed[TIMER_INTID_EL1_PHYS_NS] = true;     // PPI 30
    spin_unlock_irqrestore(&g_intid_lock, s);

    uart_puts("irqfwd: reserved kernel INTIDs ");
    uart_putdec((u64)IPI_RESCHED);
    uart_puts(",");
    uart_putdec((u64)TIMER_INTID_EL1_PHYS_NS);
    uart_puts("\n");
}

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
    // P4-Ib: claim the INTID before allocating. Pins
    // specs/handles.tla::HwResourceExclusive — two callers asking for
    // the same INTID can't both succeed.
    if (!intid_try_claim(intid)) return NULL;

    struct KObj_IRQ *k = kmalloc(sizeof(*k), KP_ZERO);
    if (!k) {
        intid_release(intid);
        return NULL;
    }

    k->magic         = KOBJ_IRQ_MAGIC;
    k->intid         = intid;
    k->ref           = 1;
    rendez_init(&k->rendez);
    k->pending_count = 0;

    // P4-Ic5b2: SPIs claimed via kobj_irq_create are edge-triggered by
    // default. The kernel GIC init pre-configures all SPIs to level
    // (the safer unknown-signalling default); the device-driver layer
    // knows its IRQ is edge-triggered (QEMU virt's virtio-mmio +
    // typical real-ARM virtio devices) and flips ICFGR here. Phase 5+
    // can add a `bool edge_triggered` parameter or a DTB-driven default
    // when level-triggered userspace IRQs become a real use case. For
    // SGIs/PPIs (intid < 32) the helper is a no-op — SGIs are always
    // edge per IHI 0069 §12.9.7, and PPIs are kernel-reserved at v1.0
    // (timer at 30, IPIs at 0).
    if (intid >= 32) {
        gic_set_spi_edge_triggered(intid);
    }

    // Register handler + enable the IRQ. gic_attach binds (intid →
    // dispatch + arg); gic_enable_irq unmasks it on the CPU's
    // redistributor (for SGIs/PPIs) or distributor (for SPIs).
    if (!gic_attach(intid, kobj_irq_dispatch, k)) {
        intid_release(intid);
        // R9 F152 (P3) close: clobber magic before kfree so any stale
        // post-free dispatch (impossible here — no fires can route to
        // k since gic_attach FAILED, but defense for the symmetric
        // gic_enable failure path below) sees magic=0 in the freed
        // memory and returns early. Mirrors kobj_irq_free_internal.
        k->magic = 0;
        kfree(k);
        return NULL;
    }
    if (!gic_enable_irq(intid)) {
        // R9 F152 (P3): gic_attach SUCCEEDED here — g_handlers[intid]
        // now points at (kobj_irq_dispatch, k). If we just kfree(k)
        // without clearing the handler slot OR clobbering magic, an
        // in-flight IRQ (unlikely since enable failed; defense) could
        // dispatch into freed memory and read undefined magic bytes.
        // gic_attach(intid, NULL, NULL) is the proper unregister but
        // is currently rejected by gic_attach's NULL-handler guard;
        // the magic clobber is the active defense.
        intid_release(intid);
        k->magic = 0;
        kfree(k);
        return NULL;
    }

    __atomic_fetch_add(&g_kobj_irq_live, 1u, __ATOMIC_RELAXED);
    return k;
}

void kobj_irq_ref(struct KObj_IRQ *k) {
    if (!k)                          extinction("kobj_irq_ref(NULL)");
    if (k->magic != KOBJ_IRQ_MAGIC)  extinction("kobj_irq_ref of corrupted KObj_IRQ");

    // R9 F148 (P2) close: atomic ref bump. See mmio_handle.c for the
    // pattern rationale (concurrent ref+unref must not torn-update).
    int old = __atomic_fetch_add(&k->ref, 1, __ATOMIC_RELAXED);
    if (old <= 0) {
        extinction("kobj_irq_ref of zero-ref KObj_IRQ (already freed?)");
    }
}

static void kobj_irq_free_internal(struct KObj_IRQ *k) {
    if (k->magic != KOBJ_IRQ_MAGIC)
        extinction("kobj_irq_free_internal of corrupted KObj_IRQ");
    if (k->ref != 0)
        extinction("kobj_irq_free_internal with ref > 0");

    // Disable the IRQ first so no more fires arrive.
    gic_disable_irq(k->intid);
    // Unregister attempt — gic_attach(intid, NULL, NULL) currently
    // returns false (NULL handler is rejected by the gic API), so the
    // handler slot retains its kobj_irq_dispatch + arg=k pointer. The
    // magic-clobber below makes any post-free dispatch see magic=0 and
    // return early. See docs/reference/36-irqfwd.md "stale-fire safety"
    // for the full lifecycle discussion.
    gic_attach(k->intid, NULL, NULL);

    // P4-Ib: release the INTID claim so a subsequent kobj_irq_create
    // for the same INTID can succeed.
    intid_release(k->intid);

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

    // R9 F148 (P2) close: atomic ref decrement. ACQ_REL ordering on
    // the dec ensures all prior accesses to *k happen-before any
    // observation of the dec by another CPU. Only the caller that
    // observed old==1 (1→0 edge) frees.
    int old = __atomic_fetch_sub(&k->ref, 1, __ATOMIC_ACQ_REL);
    if (old <= 0) {
        extinction("kobj_irq_unref of zero-ref KObj_IRQ (double-free?)");
    }
    if (old == 1) {
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
