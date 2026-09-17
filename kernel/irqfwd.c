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

#include <thylacine/pci_irq.h>
#include <thylacine/dtb.h>                   // F-A1: dtb_pci_intid_is_level
#include <thylacine/extinction.h>
#include <thylacine/irqfwd.h>
#include <thylacine/rendez.h>
#include <thylacine/sched.h>                 // RW-11 SA-1b: sched_mark_interactive
#include <thylacine/spinlock.h>
#include <thylacine/thread.h>                // RW-11 SA-1b: current_thread
#include <thylacine/types.h>

#include <thylacine/smp.h>                  // IPI_RESCHED (P4-Ib R9 F142)

#include "../arch/arm64/gic.h"
#include "../arch/arm64/gic_msi.h"
#include "../arch/arm64/timer.h"            // TIMER_INTID_EL1_VIRT
#include "../arch/arm64/uart.h"
#include "../mm/slub.h"

// F-A1 (C): the upper bound on a SYS_IRQ_WAIT timeout. Any legitimate IRQ wait
// is far below this (the GPU's is 100 ms); a wait genuinely wanting to block
// indefinitely passes 0 (forever). The cap is defense-in-depth against a caller
// that passes an indeterminate x1 -- it degrades an absurd value to a bounded,
// safe wait rather than a wrapped-deadline immediate timeout. 1 hour, in ns.
#define KOBJ_IRQ_WAIT_MAX_TIMEOUT_NS  3600000000000ull

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
// Permanent dispatch indirection. GIC callbacks never retain a KObj pointer.
// g_intid_lock guards publication/removal and precedes rendez.lock when pinning.
static struct KObj_IRQ *g_intid_owner[GIC_NUM_INTIDS];
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
//   - PPI 27 (TIMER_INTID_EL1_VIRT): timer tick; attached at
//     boot_main via gic_attach directly.
// Without reservation, a syscall caller with CAP_HW_CREATE could pass
// either INTID to SYS_IRQ_CREATE and overwrite the kernel's handler.
// SYS_IRQ_CREATE additionally enforces intid >= 32 (F145), but this
// reservation closes the kernel-internal path as defense-in-depth and
// keeps the spec invariant HwResourceExclusive sound even if some
// future refactor exposes the SGI/PPI range.
void irqfwd_init(void) {
    irq_state_t s = spin_lock_irqsave(&g_intid_lock);
    g_intid_claimed[IPI_MSI_FAULT]           = true;     // SGI 14
    g_intid_claimed[IPI_IRQ_BARRIER]         = true;     // SGI 15
    g_intid_claimed[IPI_RESCHED]             = true;     // SGI 0
    g_intid_claimed[TIMER_INTID_EL1_VIRT]    = true;     // PPI 27 (virtual timer)
    g_intid_claimed[UART_INTID_PL011]        = true;     // A-4c-1: kernel cons RX
    spin_unlock_irqrestore(&g_intid_lock, s);

    uart_puts("irqfwd: reserved kernel INTIDs ");
    uart_putdec((u64)IPI_RESCHED);
    uart_puts(",");
    uart_putdec((u64)TIMER_INTID_EL1_VIRT);
    uart_puts(",");
    uart_putdec((u64)UART_INTID_PL011);
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

bool kobj_irq_intid_claimed(u32 intid) {
    if (intid >= GIC_NUM_INTIDS) return false;
    irq_state_t s = spin_lock_irqsave(&g_intid_lock);
    bool claimed = g_intid_claimed[intid];
    spin_unlock_irqrestore(&g_intid_lock, s);
    return claimed;
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
    (void)arg;
    if (intid >= GIC_NUM_INTIDS) return;
    irq_state_t ds = spin_lock_irqsave(&g_intid_lock);
    struct KObj_IRQ *k = g_intid_owner[intid];
    if (!k) { spin_unlock_irqrestore(&g_intid_lock, ds); return; }
    // Resolve and pin while removal is excluded. The old in-object dying
    // check could itself dereference freed storage before acquiring its lock.
    spin_lock(&k->rendez.lock);
    if (k->dying) {
        spin_unlock(&k->rendez.lock);
        spin_unlock_irqrestore(&g_intid_lock, ds);
        return;
    }
    if (k->level) gic_disable_irq(intid);
    if (k->pending_count < 0xFFFFFFFEu) k->pending_count++;
    k->in_dispatch++;
    spin_unlock(&k->rendez.lock);
    spin_unlock_irqrestore(&g_intid_lock, ds);
    __atomic_fetch_add(&g_irq_total_fires, 1u, __ATOMIC_RELAXED);
    wakeup(&k->rendez);
    irq_state_t rs = spin_lock_irqsave(&k->rendez.lock);
    k->in_dispatch--; // Last touch; destructor drains under the same lock.
    spin_unlock_irqrestore(&k->rendez.lock, rs);
}

// =============================================================================
// Lifecycle.
// =============================================================================

struct KObj_IRQ *kobj_irq_create(u32 intid) {
    // A routed PCI line belongs to its function-scoped domain even before
    // the first endpoint is created. Numeric routing hints grant no authority.
    bool pci_level;
    if (dtb_pci_intid_is_level(intid, &pci_level) || gic_msi_reserved(intid)) return NULL;
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

    // F-A1: derive the SPI trigger from the DTB (I-15) rather than forcing
    // edge. virtio-PCI legacy INTx is LEVEL; virtio-mmio is EDGE. A level line
    // forced to edge drops an overlapping re-assertion (the line stays high ->
    // no fresh rising edge -> the GIC latches no new pending), and the driver's
    // kobj_irq_wait then blocks forever -- on the single-threaded compositor a
    // synchronous GPU present wedges the display (the intermittent console-gate
    // silence). Read the trigger from the PCIe interrupt-map's flags cell; an
    // SPI absent from the map keeps the edge default -- the I-15-argued fallback
    // (QEMU-virt declares virtio-mmio EDGE_RISING; SGIs are always edge; a
    // strict superset of the old universal-edge). ICFGR is set EXPLICITLY both
    // ways so a reused INTID never inherits a stale config. SGIs/PPIs
    // (intid < 32) skip ICFGR -- SGIs are always edge (IHI 0069 12.9.7); PPIs
    // are kernel-reserved at v1.0 (timer at 27, IPIs at 0). k->level is set
    // BEFORE gic_attach so the dispatch/wait mask+ack reads a published value
    // (no fire can route here until gic_attach + gic_enable_irq below).
    k->level = false;
    if (intid >= 32) {
        bool lvl = false;
        if (dtb_pci_intid_is_level(intid, &lvl) && lvl) {
            k->level = true;
            gic_set_spi_level_triggered(intid);
        } else {
            gic_set_spi_edge_triggered(intid);
        }
    }

    // Publish a weak owner before enabling, protected against last-unref.
    // Count it before publication so rollback can use the normal destructor.
    __atomic_fetch_add(&g_kobj_irq_live, 1u, __ATOMIC_RELAXED);
    irq_state_t ds = spin_lock_irqsave(&g_intid_lock);
    g_intid_owner[intid] = k;
    spin_unlock_irqrestore(&g_intid_lock, ds);
    if (!gic_attach(intid, kobj_irq_dispatch, NULL) || !gic_enable_irq(intid)) {
        kobj_irq_unref(k);
        return NULL;
    }

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
    if (k->pci) { pci_irq_free(k); return; }
    if (k->magic != KOBJ_IRQ_MAGIC)
        extinction("kobj_irq_free_internal of corrupted KObj_IRQ");
    if (k->ref != 0)
        extinction("kobj_irq_free_internal with ref > 0");

    gic_disable_irq(k->intid);
    // Detach the owner under the same lock used by dispatch BEFORE its first
    // dereference. Keep the permanent GIC callback installed: a late arrival
    // sees an empty slot, never freed memory. A new owner cannot publish until
    // intid_release below, after every old dispatch has finished.
    irq_state_t ds = spin_lock_irqsave(&g_intid_lock);
    if (g_intid_owner[k->intid] != k) extinction("IRQ owner mismatch on detach");
    g_intid_owner[k->intid] = NULL;
    spin_lock(&k->rendez.lock);
    k->dying = true;
    spin_unlock(&k->rendez.lock);
    spin_unlock_irqrestore(&g_intid_lock, ds);
    for (;;) {
        irq_state_t rs = spin_lock_irqsave(&k->rendez.lock);
        u32 in_flight = k->in_dispatch;
        spin_unlock_irqrestore(&k->rendez.lock, rs);
        if (!in_flight) break;
        __asm__ volatile("yield" ::: "memory");
    }

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

u32 kobj_irq_wait_timed(struct KObj_IRQ *k, u64 timeout_ns) {
    if (!k) return 0;
    if (k->magic != KOBJ_IRQ_MAGIC)
        extinction("kobj_irq_wait of corrupted KObj_IRQ");

    if (k->pci) return KOBJ_IRQ_WAIT_BUSY; // PCI WAIT has a separate ticket ABI

    // RW-7 R1-F1: the Rendez is single-waiter -- tsleep() EXTINCTS the kernel
    // on a 2nd concurrent sleeper (sched.c "rendez already has a waiter"). But
    // the KObj_IRQ handle lives in the per-Proc handle table, SHARED across a
    // multi-thread Proc's peer Threads, so two of them could both reach a
    // SYS_IRQ_WAIT on one fd. Claim the single-waiter slot under the lock and
    // refuse a 2nd concurrent waiter (the devcons single-reader pattern) so a
    // driver bug is a clean error, not a whole-kernel extinction.
    irq_state_t s = spin_lock_irqsave(&k->rendez.lock);
    if (k->waiting) {
        spin_unlock_irqrestore(&k->rendez.lock, s);
        return KOBJ_IRQ_WAIT_BUSY;
    }
    k->waiting = true;
    spin_unlock_irqrestore(&k->rendez.lock, s);

    // F-A1: re-arm a LEVEL line before sleeping. kobj_irq_dispatch masked it on
    // the last fire; the driver has since acked the device (deasserting the
    // line) and re-entered here. gic_enable_irq is a lock-free write-1-to-set to
    // ISENABLER; if a new completion arrived while masked the still-high level
    // re-triggers at once (no loss), otherwise the deasserted line stays quiet.
    // I-9 holds: the unmask can only CAUSE a dispatch (which increments
    // pending_count under rendez.lock), and tsleep's cond re-checks
    // pending_count under the same lock, so no wakeup is lost. EDGE lines are
    // never masked, so the branch is skipped for them.
    if (k->level) gic_enable_irq(k->intid);

    // RW-11 SA-1b: an IRQ-service thread is latency-critical -- its wake should
    // preempt NORMAL work. Promote it to the INTERACTIVE band (ARCH 8.3) so the
    // pending IRQ runs it ahead of any NORMAL thread sharing its CPU (closes the
    // IRQ-to-driver leg of the 6 ms slice cliff). Sticky + no-op for kernel
    // threads; the driver thread is the current thread here (pre-sleep).
    sched_mark_interactive(current_thread());

    // Block until pending_count > 0, bounded by the deadline (F-A1 C). tsleep's
    // cond loop guarantees no spurious return; cond has precedence over the
    // deadline. deadline_ns == 0 -> no deadline (exactly sleep()). #811 (ARCH
    // §8.8.1): TSLEEP_INTR means the Proc is group-terminating -- return so the
    // Thread unwinds to its EL0-return die-check (the count is immaterial; the
    // Thread never reaches EL0). TSLEEP_TIMEDOUT means no IRQ arrived within the
    // timeout; the driver treats the count-0 return as "re-check the device
    // used-ring + continue", catching a lost completion instead of hanging.
    // Cap the requested timeout to a sane maximum before forming the deadline
    // (defense-in-depth). SYS_IRQ_WAIT reads x1 as timeout_ns; a caller that
    // fails to zero x1 (a hand-wrapped varargs syscall -- see ARCH 9.3.1's ABI
    // note) passes an indeterminate value. Without a cap, a garbage-huge value
    // could overflow `now + timeout_ns` OR the ns->counter conversion in tsleep
    // and read as an IMMEDIATE timeout (a spurious 0-count return). The cap
    // degrades any absurd value to a bounded, safe wait; a genuine timeout is
    // far below it (the GPU's is 100 ms) and forever is still timeout_ns == 0.
    // A driver that legitimately wants longer simply re-waits (a timed wait is
    // a backstop that loops). now is monotonic-since-boot, so now + cap never
    // wraps u64.
    u64 deadline_ns = 0;
    if (timeout_ns != 0) {
        if (timeout_ns > KOBJ_IRQ_WAIT_MAX_TIMEOUT_NS)
            timeout_ns = KOBJ_IRQ_WAIT_MAX_TIMEOUT_NS;
        deadline_ns = timer_now_ns() + timeout_ns;
    }
    int rc = tsleep(&k->rendez, kobj_irq_pending_cond, k, deadline_ns);

    // Re-take the lock to clear the waiter slot AND atomically read + zero
    // pending_count. An IRQ that fires between tsleep's return and this read
    // MUST NOT be lost -- it is reflected in the next wait; `count` returned
    // here captures only the IRQs that arrived BEFORE the lock acquire. On
    // TSLEEP_TIMEDOUT cond had precedence, so pending_count is 0 (count 0);
    // TSLEEP_INTR (death) also returns 0.
    s = spin_lock_irqsave(&k->rendez.lock);
    k->waiting = false;
    u32 count = (rc == TSLEEP_INTR) ? 0u : k->pending_count;
    k->pending_count = 0;
    spin_unlock_irqrestore(&k->rendez.lock, s);
    return count;
}

u32 kobj_irq_wait(struct KObj_IRQ *k) {
    return kobj_irq_wait_timed(k, 0);
}

#ifdef KERNEL_TESTS
void irqfwd_test_dispatch(u32 intid, void *untrusted_arg);
void irqfwd_test_dispatch(u32 intid, void *untrusted_arg) {
    kobj_irq_dispatch(intid, untrusted_arg);
}
#endif
