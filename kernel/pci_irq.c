// Shared INTx uses permanent GIC domain callbacks, never a freed endpoint as
// the GIC argument. Membership is weak, bounded and protected by domain_lock.
// Dispatch pins are independent of object references: last-unref first removes
// membership, then drains these pins before freeing. No ref-zero resurrection.
//
// Lock order: domain_lock -> IRQ rendez.lock -> PCI cfg_lock. WAIT takes only
// rendez.lock. Wakeups occur after dropping domain_lock, with a dispatch pin.
// Function quiesce drops cfg_lock before revoking membership; never reverse it.
#include <thylacine/pci_irq.h>
#include <thylacine/pci_handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/virtio_pci.h>
#include <thylacine/dtb.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
#include <thylacine/sched.h>
#include <thylacine/thread.h>
#include "../arch/arm64/gic.h"
#include "../arch/arm64/gic_msi.h"
#include "../arch/arm64/timer.h"
#include "../mm/slub.h"

#define PCI_IRQ_MAX 32u
#define IRQ_WAIT_MAX_NS 3600000000000ull
// Initial bounded policy, to be measured with audio/network/GPU workloads.
#define IRQ_BURST_WINDOW_NS 1000000ull
#define IRQ_BURST_LIMIT 128u
#define IRQ_RETRY_NS 100000ull
#define IRQ_STRAY_LIMIT 32u
#define IRQ_DOMAIN_RECOVERY_NS 100000000ull

struct PciIrq {
    struct KObj_PCI *pci; // strong; parent membership is weak
    u64 generation, sequence;
    u64 deliveries, retries, cooldowns, burst_start, not_before;
    u32 state, mode, reason, burst_count, count, table_index;
    bool route_owned; // backing retains the lease through quarantine
    bool ready; // route setup finished before a handle may ARM
    struct gic_msi_route route;
    u32 dispatch_pins; // atomic, manipulated while membership is locked
};
struct pci_irq_domain {
    bool installed, faulted;
    u32 stray;
    u64 retry_at; // recovery attempts are bounded even across fresh claims
};
static spin_lock_t domain_lock = SPIN_LOCK_INIT;
static struct KObj_IRQ *endpoints[PCI_IRQ_MAX];
static struct pci_irq_domain domains[GIC_NUM_INTIDS];
static u64 next_generation;

static void endpoint_pin(struct KObj_IRQ *k) {
    __atomic_fetch_add(&k->pci->dispatch_pins, 1u, __ATOMIC_RELAXED);
}
static void endpoint_wake_unpin(struct KObj_IRQ *k) {
    wakeup(&k->rendez);
    // Last access: a destructor on another CPU may free immediately after it.
    __atomic_fetch_sub(&k->pci->dispatch_pins, 1u, __ATOMIC_RELEASE);
}
static bool disable_locked(struct KObj_IRQ *k, u32 state) {
    // domain_lock + rendez.lock held. POLLED and revoked sources stay masked.
    bool failed = false;
    if (k->pci->mode == PCI_IRQ_MSIX) {
        failed = !kobj_pci_msix_mask(k->pci->pci, k->pci->table_index, true) &&
                  kobj_pci_is_live(k->pci->pci);
        if (failed) kobj_pci_irq_fault(k->pci->pci);
    } else kobj_pci_intx_set(k->pci->pci, false);
    k->pci->state = state;
    return failed;
}

// Caller retains the function through an endpoint ref or a dispatch pin.
// Function-level failure affects every vector; wake all peers after releasing
// membership. Only config masking runs here, so this is safe in IRQ context.
static void fault_function(struct KObj_PCI *pci) {
    struct KObj_IRQ *wake[PCI_IRQ_MAX]; u32 count = 0;
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    kobj_pci_irq_fault(pci);
    for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
        struct KObj_IRQ *k = endpoints[i];
        if (!k || k->pci->pci != pci) continue;
        spin_lock(&k->rendez.lock);
        if (k->pci->state != PCI_IRQ_REVOKED) k->pci->state = PCI_IRQ_FAULT;
        endpoint_pin(k); wake[count++] = k;
        spin_unlock(&k->rendez.lock);
    }
    spin_unlock_irqrestore(&domain_lock, ds);
    for (u32 i = 0; i < count; i++) endpoint_wake_unpin(wake[i]);
}
static int source_failure_locked(struct KObj_IRQ *k) {
    bool live = kobj_pci_is_live(k->pci->pci);
    k->pci->state = live ? PCI_IRQ_FAULT : PCI_IRQ_REVOKED;
    return live ? -T_E_IO : -T_E_CANCELED;
}

static void pci_intx_dispatch(u32 intid, void *arg) {
    (void)arg;
    struct KObj_IRQ *wake[PCI_IRQ_MAX];
    u32 nwake = 0;
    bool recognized = false;
    gic_disable_irq(intid);
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    struct pci_irq_domain *domain = &domains[intid];
    u64 now = timer_now_ns();
    for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
        struct KObj_IRQ *k = endpoints[i];
        if (!k || k->intid != intid || k->pci->mode != PCI_IRQ_INTX) continue;
        struct PciIrq *p = k->pci;
        spin_lock(&k->rendez.lock);
        if (p->state == PCI_IRQ_ARMED && kobj_pci_intx_asserted(p->pci)) {
            recognized = true;
            // Clear this function's delivery enable before re-enabling the
            // shared wire. The readback/barrier completes the config write.
            kobj_pci_intx_set(p->pci, false);
            if (p->sequence == ~0ull) {
                p->state = PCI_IRQ_FAULT; // sequence exhaustion cannot ABA
            } else {
                p->sequence++;
                if (p->deliveries != ~0ull) p->deliveries++;
                p->count = 1;
                p->state = PCI_IRQ_DELIVERED;
                p->reason = PCI_IRQ_EVENT;
                p->not_before = 0;
                if (now - p->burst_start >= IRQ_BURST_WINDOW_NS) {
                    p->burst_start = now; p->burst_count = 0;
                }
                if (++p->burst_count > IRQ_BURST_LIMIT) {
                    if (p->cooldowns != ~0ull) p->cooldowns++;
                    p->reason = PCI_IRQ_COOLDOWN;
                    p->not_before = now + IRQ_BURST_WINDOW_NS;
                }
            }
            endpoint_pin(k);
            wake[nwake++] = k;
        }
        spin_unlock(&k->rendez.lock);
    }
    if (recognized) domain->stray = 0;
    else if (++domain->stray >= IRQ_STRAY_LIMIT) {
        // No known ARMED function explains a repeatedly asserted wire. Bound
        // the storm and surface a terminal domain fault to every subscriber.
        domain->faulted = true;
        domain->retry_at = now + IRQ_DOMAIN_RECOVERY_NS;
        for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
            struct KObj_IRQ *k = endpoints[i];
            if (!k || k->intid != intid || k->pci->mode != PCI_IRQ_INTX) continue;
            spin_lock(&k->rendez.lock);
            disable_locked(k, PCI_IRQ_FAULT);
            endpoint_pin(k);
            wake[nwake++] = k;
            spin_unlock(&k->rendez.lock);
        }
    }
    if (!domain->faulted) gic_enable_irq(intid);
    spin_unlock_irqrestore(&domain_lock, ds);
    for (u32 i = 0; i < nwake; i++) endpoint_wake_unpin(wake[i]);
}

static void pci_msix_dispatch(u32 intid, void *arg) {
    (void)arg;
    struct KObj_IRQ *wake = NULL;
    bool failed = false;
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
        struct KObj_IRQ *k = endpoints[i];
        if (!k || k->intid != intid || k->pci->mode != PCI_IRQ_MSIX) continue;
        struct PciIrq *p = k->pci;
        spin_lock(&k->rendez.lock);
        if (p->state == PCI_IRQ_ARMED) {
            bool masked = kobj_pci_msix_mask(p->pci, p->table_index, true);
            if (!masked || p->sequence == ~0ull) {
                failed = true;
                kobj_pci_irq_fault(p->pci); // contain even if the entry mask failed
                p->state = PCI_IRQ_FAULT;
                gic_disable_irq(intid);
            } else {
                p->sequence++; p->count = 1;
                if (p->deliveries != ~0ull) p->deliveries++;
                p->state = PCI_IRQ_DELIVERED; p->reason = PCI_IRQ_EVENT;
                p->not_before = 0;
                u64 now = timer_now_ns();
                if (now - p->burst_start >= IRQ_BURST_WINDOW_NS) {
                    p->burst_start = now; p->burst_count = 0;
                }
                if (++p->burst_count > IRQ_BURST_LIMIT) {
                    if (p->cooldowns != ~0ull) p->cooldowns++;
                    p->reason = PCI_IRQ_COOLDOWN;
                    p->not_before = now + IRQ_BURST_WINDOW_NS;
                }
            }
            endpoint_pin(k); wake = k;
        } else if (p->state == PCI_IRQ_DELIVERED) {
            // A message already in flight when the entry was masked may
            // arrive once more. Preserve the ticket and saturate its count.
            if (p->count != 0xffffffffu) p->count++;
            if (p->deliveries != ~0ull) p->deliveries++;
            endpoint_pin(k); wake = k;
        }
        spin_unlock(&k->rendez.lock);
        if (wake) break;
    }
    if (!wake) gic_disable_irq(intid); // stale/unowned delivery is contained
    spin_unlock_irqrestore(&domain_lock, ds);
    if (failed && wake) fault_function(wake->pci->pci);
    if (wake) endpoint_wake_unpin(wake);
}
static void controller_fault(u32 node) {
    struct KObj_IRQ *wake[PCI_IRQ_MAX]; u32 count = 0;
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
        struct KObj_IRQ *k = endpoints[i];
        if (!k || k->pci->mode != PCI_IRQ_MSIX || !k->pci->ready ||
            k->pci->route.controller != node) continue;
        spin_lock(&k->rendez.lock);
        kobj_pci_irq_fault(k->pci->pci);
        if (k->pci->state != PCI_IRQ_REVOKED) k->pci->state = PCI_IRQ_FAULT;
        endpoint_pin(k); wake[count++] = k;
        spin_unlock(&k->rendez.lock);
    }
    spin_unlock_irqrestore(&domain_lock, ds);
    for (u32 i = 0; i < count; i++) endpoint_wake_unpin(wake[i]);
}
void pci_irq_init(void) { gic_msi_init(pci_msix_dispatch, controller_fault); }

struct KObj_IRQ *pci_irq_create(struct KObj_PCI *pci, u32 mode, u32 ordinal, int *error) {
    *error = -T_E_INVAL;
    if (!pci || pci->magic != KOBJ_PCI_MAGIC) return NULL;
    u32 intid = 0xffffffffu;
    if (mode == PCI_IRQ_INTX) {
        if (ordinal || !pci->intid_valid) return NULL;
        intid = pci->intid;
        bool level = false;
        if (intid < 32 || intid >= GIC_NUM_INTIDS || intid > gic_max_intid() ||
            !dtb_pci_intid_is_level(intid, &level) || !level) return NULL;
    } else if (mode == PCI_IRQ_MSIX) {
        if (!pci->msix.cap_offset) { *error = -T_E_NODEV; return NULL; }
        if (ordinal >= PCI_MSIX_VECTOR_MAX || ordinal >= pci->msix.entries) return NULL;
        if (!pci->msix_table || !pci->common_cfg) { *error = -T_E_NODEV; return NULL; }
    } else return NULL;
    struct KObj_IRQ *k = kmalloc(sizeof(*k), KP_ZERO);
    struct PciIrq *p = kmalloc(sizeof(*p), KP_ZERO);
    if (!k || !p) { kfree(k); kfree(p); *error = -T_E_NOMEM; return NULL; }
    k->magic = KOBJ_IRQ_MAGIC; k->ref = 1; k->intid = intid; k->pci = p;
    rendez_init(&k->rendez);
    p->pci = pci; p->mode = mode; p->state = PCI_IRQ_DISARMED;
    p->table_index = mode == PCI_IRQ_MSIX ? ordinal : 0xffffffffu;
    kobj_pci_ref(pci);
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    if (!kobj_pci_is_live(pci)) { *error = -T_E_CANCELED; goto fail_locked; }
    if (!kobj_pci_irq_usable(pci)) { *error = -T_E_IO; goto fail_locked; }
    u32 slot = PCI_IRQ_MAX;
    for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
        if (!endpoints[i]) { if (slot == PCI_IRQ_MAX) slot = i; continue; }
        struct PciIrq *other = endpoints[i]->pci;
        if (other->pci != pci) continue;
        // Configure all vectors before any ARM. No live mode migration or
        // table reconfiguration underneath an active driver/waiter.
        if (other->mode != mode || mode == PCI_IRQ_INTX ||
            other->table_index == ordinal || other->state != PCI_IRQ_DISARMED) {
            *error = -T_E_BUSY; goto fail_locked;
        }
    }
    if (slot == PCI_IRQ_MAX || next_generation == ~0ull) { *error = -T_E_BUSY; goto fail_locked; }
    if (mode == PCI_IRQ_INTX) {
        struct pci_irq_domain *d = &domains[intid];
        if (d->faulted) {
            // No old ticket may regain authority through domain recovery.
            // All previous subscribers must close; new claims start POLLED.
            // An unknown stuck source keeps ISPENDR set and recovery fails.
            u64 now = timer_now_ns();
            if (now < d->retry_at) { *error = -T_E_IO; goto fail_locked; }
            for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
                if (endpoints[i] && endpoints[i]->intid == intid &&
                    endpoints[i]->pci->mode == PCI_IRQ_INTX) {
                    *error = -T_E_IO; goto fail_locked;
                }
            }
            d->retry_at = now + IRQ_DOMAIN_RECOVERY_NS;
            // One non-waiting controller probe under the membership lock.
            // An active callback or unfinished GIC RWP also defers recovery.
            if (!gic_drain_spi(intid)) { *error = -T_E_IO; goto fail_locked; }
            d->faulted = false;
            d->stray = 0;
        }
        if (!d->installed) {
            if (kobj_irq_intid_claimed(intid)) { *error = -T_E_BUSY; goto fail_locked; }
            gic_disable_irq(intid); gic_set_spi_level_triggered(intid);
            if (!gic_attach(intid, pci_intx_dispatch, NULL)) { *error = -T_E_IO; goto fail_locked; }
            d->installed = true;
        }
    }
    p->generation = ++next_generation;
    endpoints[slot] = k;
    if (mode == PCI_IRQ_INTX) { p->ready = true; gic_enable_irq(intid); }
    spin_unlock_irqrestore(&domain_lock, ds);
    if (mode == PCI_IRQ_MSIX) {
        // Backend operations may wait for ITS commands. Membership reserves
        // the local index, but no handle is visible until setup completes.
        u16 rid = ((u16)pci->bus << 8) | ((u16)pci->dev << 3) | pci->fn;
        if (!gic_msi_alloc(rid, ordinal, &p->route)) { *error = -T_E_NODEV; goto abandon; }
        ds = spin_lock_irqsave(&domain_lock);
        k->intid = p->route.intid;
        spin_unlock_irqrestore(&domain_lock, ds);
        int installed = kobj_pci_msix_program(pci, ordinal, &p->route);
        if (!installed) {
            (void)gic_msi_retire(&p->route, true); // address was never installed
            *error = -T_E_BUSY; goto abandon;
        }
        p->route_owned = true;
        if (installed < 0) { *error = -T_E_IO; goto abandon; }
        ds = spin_lock_irqsave(&domain_lock);
        spin_lock(&k->rendez.lock);
        bool live = p->state == PCI_IRQ_DISARMED && kobj_pci_irq_usable(pci);
        if (live) p->ready = true;
        spin_unlock(&k->rendez.lock);
        spin_unlock_irqrestore(&domain_lock, ds);
        if (!live) { *error = kobj_pci_is_live(pci) ? -T_E_IO : -T_E_CANCELED; goto abandon; }
    }
    *error = 0; return k;
fail_locked:
    spin_unlock_irqrestore(&domain_lock, ds);
    kfree(p); kfree(k); kobj_pci_unref(pci); return NULL;
abandon:
    pci_irq_free(k); return NULL;
}

static bool source_arm_locked(struct KObj_IRQ *k) {
    struct PciIrq *p = k->pci;
    if (p->mode == PCI_IRQ_INTX) return kobj_pci_intx_set(p->pci, true);
    if (!p->route_owned) return false;
    if (!gic_enable_irq(k->intid)) return false;
    if (kobj_pci_msix_mask(p->pci, p->table_index, false)) return true;
    gic_disable_irq(k->intid); return false;
}

int pci_irq_arm(struct KObj_IRQ *k) {
    if (!k || !k->pci) return -T_E_INVAL;
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    spin_lock(&k->rendez.lock);
    int rc = -T_E_INVAL;
    bool configuring = !k->pci->ready;
    for (u32 i = 0; i < PCI_IRQ_MAX; i++)
        if (endpoints[i] && endpoints[i]->pci->pci == k->pci->pci &&
            endpoints[i]->pci->state == PCI_IRQ_DISARMED && !endpoints[i]->pci->ready)
            configuring = true;
    if (configuring) rc = -T_E_BUSY;
    else if (k->pci->state == PCI_IRQ_DISARMED) {
        if (source_arm_locked(k)) {
            k->pci->state = PCI_IRQ_ARMED; rc = 0;
        } else rc = source_failure_locked(k);
    }
    spin_unlock(&k->rendez.lock);
    spin_unlock_irqrestore(&domain_lock, ds);
    if (rc == -T_E_IO) fault_function(k->pci->pci);
    else if (rc == -T_E_CANCELED) wakeup(&k->rendez);
    return rc;
}

// These predicates are always called with rendez.lock held.
static int event_exists(void *arg) {
    struct PciIrq *p = ((struct KObj_IRQ *)arg)->pci;
    return p->state == PCI_IRQ_DELIVERED || p->state >= PCI_IRQ_REVOKED;
}
static int event_due(void *arg) {
    struct PciIrq *p = ((struct KObj_IRQ *)arg)->pci;
    return p->state >= PCI_IRQ_REVOKED ||
        (p->state == PCI_IRQ_DELIVERED && timer_now_ns() >= p->not_before);
}
int pci_irq_wait(struct KObj_IRQ *k, u64 timeout_ns, struct pci_irq_event *event) {
    if (!k || !k->pci || !event) return -T_E_INVAL;
    sched_mark_interactive(current_thread());
    u64 now = timer_now_ns();
    if (timeout_ns > IRQ_WAIT_MAX_NS) timeout_ns = IRQ_WAIT_MAX_NS;
    u64 deadline = timeout_ns ? now + timeout_ns : 0;
    irq_state_t rs = spin_lock_irqsave(&k->rendez.lock);
    if (k->waiting) { spin_unlock_irqrestore(&k->rendez.lock, rs); return -T_E_BUSY; }
    k->waiting = true;
    int result;
    for (;;) {
        struct PciIrq *p = k->pci;
        now = timer_now_ns();
        if (p->state >= PCI_IRQ_REVOKED) {
            result = p->state == PCI_IRQ_FAULT ? -T_E_IO : -T_E_CANCELED; break;
        }
        if (p->state == PCI_IRQ_DELIVERED && now >= p->not_before) {
            *event = (struct pci_irq_event){ .generation = p->generation,
                .sequence = p->sequence, .count = p->count, .reason = p->reason,
                .retry_after_ns = 0 };
            result = 1; break;
        }
        if (deadline && now >= deadline) { result = 0; break; }
        bool pending = p->state == PCI_IRQ_DELIVERED;
        u64 until = deadline;
        if (pending && (!until || p->not_before < until)) until = p->not_before;
        spin_unlock_irqrestore(&k->rendez.lock, rs);
        int rc = tsleep(&k->rendez, pending ? event_due : event_exists, k, until);
        rs = spin_lock_irqsave(&k->rendez.lock);
        if (rc == TSLEEP_INTR) { result = -T_E_INTR; break; }
    }
    k->waiting = false;
    spin_unlock_irqrestore(&k->rendez.lock, rs);
    return result;
}

int pci_irq_complete(struct KObj_IRQ *k, u64 generation, u64 sequence) {
    if (!k || !k->pci) return -T_E_INVAL;
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    spin_lock(&k->rendez.lock);
    struct PciIrq *p = k->pci;
    int rc = -T_E_INVAL;
    if (p->state == PCI_IRQ_DELIVERED && p->generation == generation && p->sequence == sequence) {
        u64 now = timer_now_ns();
        if (now < p->not_before) rc = -T_E_AGAIN;
        else if (p->mode == PCI_IRQ_INTX && kobj_pci_intx_asserted(p->pci)) {
            // New work may race the driver's ack. Keep the same ticket and
            // mask, then make WAIT delay the retry instead of storming.
            if (p->retries != ~0ull) p->retries++;
            p->reason = PCI_IRQ_RETRY;
            p->not_before = now + IRQ_RETRY_NS;
            rc = -T_E_AGAIN;
        } else {
            if (source_arm_locked(k)) {
                p->state = PCI_IRQ_ARMED; rc = 0;
            } else rc = source_failure_locked(k);
        }
    }
    spin_unlock(&k->rendez.lock);
    spin_unlock_irqrestore(&domain_lock, ds);
    if (rc == -T_E_IO) fault_function(k->pci->pci);
    else if (rc == -T_E_CANCELED) wakeup(&k->rendez);
    return rc;
}

int pci_irq_disable(struct KObj_IRQ *k) {
    if (!k || !k->pci) return -T_E_INVAL;
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    spin_lock(&k->rendez.lock);
    bool failed = disable_locked(k, PCI_IRQ_REVOKED);
    spin_unlock(&k->rendez.lock);
    spin_unlock_irqrestore(&domain_lock, ds);
    if (failed) fault_function(k->pci->pci);
    wakeup(&k->rendez); // caller holds a reference
    return 0;
}
void pci_irq_revoke_function(struct KObj_PCI *pci) {
    struct KObj_IRQ *wake[PCI_IRQ_MAX]; u32 n = 0;
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    for (u32 i = 0; i < PCI_IRQ_MAX; i++) {
        struct KObj_IRQ *k = endpoints[i];
        if (!k || k->pci->pci != pci) continue;
        spin_lock(&k->rendez.lock);
        disable_locked(k, PCI_IRQ_REVOKED);
        endpoint_pin(k); wake[n++] = k;
        spin_unlock(&k->rendez.lock);
    }
    spin_unlock_irqrestore(&domain_lock, ds);
    for (u32 i = 0; i < n; i++) endpoint_wake_unpin(wake[i]);
}
int pci_irq_get_info(struct KObj_IRQ *k, struct pci_irq_info *info) {
    if (!k || !k->pci || !info) return -T_E_INVAL;
    irq_state_t rs = spin_lock_irqsave(&k->rendez.lock);
    struct PciIrq *p = k->pci;
    *info = (struct pci_irq_info){ .generation = p->generation,
        .deliveries = p->deliveries, .retries = p->retries, .cooldowns = p->cooldowns,
        .mode = p->mode, .state = p->state, .table_index = p->table_index, .reserved = 0 };
    spin_unlock_irqrestore(&k->rendez.lock, rs);
    return 0;
}
void pci_irq_free(struct KObj_IRQ *k) {
    irq_state_t ds = spin_lock_irqsave(&domain_lock);
    spin_lock(&k->rendez.lock);
    bool failed = disable_locked(k, PCI_IRQ_REVOKED);
    for (u32 i = 0; i < PCI_IRQ_MAX; i++) if (endpoints[i] == k) endpoints[i] = NULL;
    bool peer = false;
    for (u32 i = 0; i < PCI_IRQ_MAX; i++)
        if (endpoints[i] && endpoints[i]->pci->pci == k->pci->pci) peer = true;
    if (k->pci->mode == PCI_IRQ_MSIX && !peer) kobj_pci_msix_off(k->pci->pci);
    spin_unlock(&k->rendez.lock);
    spin_unlock_irqrestore(&domain_lock, ds);
    if (failed) fault_function(k->pci->pci);
    while (__atomic_load_n(&k->pci->dispatch_pins, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield" ::: "memory");
    struct KObj_PCI *pci = k->pci->pci;
    if (k->pci->route_owned) kobj_pci_msix_retire(pci, k->pci->table_index);
    kfree(k->pci); k->pci = NULL; k->magic = 0; kfree(k);
    kobj_pci_unref(pci);
}

struct KObj_PCI *pci_irq_owner(struct KObj_IRQ *k) {
    return k && k->pci ? k->pci->pci : NULL;
}

#ifdef KERNEL_TESTS
// Inject dispatch over synthetic config bytes; never writes a real device's
// read-only PCI Interrupt Status. Tests use a DTB-routed, otherwise idle line.
void pci_irq_test_controller_fault(u32 node);
void pci_irq_test_controller_fault(u32 node) { controller_fault(node); }
void pci_irq_test_msix_dispatch(u32 intid);
void pci_irq_test_msix_dispatch(u32 intid) { pci_msix_dispatch(intid, NULL); }
void pci_irq_test_dispatch(u32 intid);
void pci_irq_test_dispatch(u32 intid) { pci_intx_dispatch(intid, NULL); }
#endif
