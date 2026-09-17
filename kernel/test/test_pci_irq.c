#include "test.h"
#include <thylacine/pci_irq.h>
#include <thylacine/pci_handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/virtio_pci.h>
#include <thylacine/dtb.h>
#include <thylacine/errno.h>
#include "../../mm/slub.h"

void pci_irq_test_dispatch(u32 intid);
void test_pci_irq_shared_tickets(void);

struct fixture {
    struct KObj_PCI *pci;
    struct virtio_pci_dev dev;
    u8 config[64];
};
static bool fixture_init(struct fixture *f, u32 intid) {
    for (u32 i = 0; i < sizeof(f->config); i++) f->config[i] = 0;
    f->dev = (struct virtio_pci_dev){ .cfg = f->config };
    f->pci = kmalloc(sizeof(*f->pci), KP_ZERO);
    if (!f->pci) return false;
    f->pci->magic = KOBJ_PCI_MAGIC;
    f->pci->ref = 1; // retained fixture reference, never drop via real claim table
    f->pci->vpd = &f->dev;
    f->pci->intid = intid; f->pci->intid_valid = true;
    spin_lock_init(&f->pci->cfg_lock);
    f->config[PCI_CFG_COMMAND + 1] = 4; // INTxDisable
    return true;
}
static bool masked(struct fixture *f) {
    return (virtio_pci_cfg_read16(&f->dev, PCI_CFG_COMMAND) & PCI_CMD_INTX_DISABLE) != 0;
}
void test_pci_irq_shared_tickets(void) {
    u32 intid = 0;
    TEST_ASSERT(dtb_pci_intx_route(0, 1, &intid), "test topology has PCI INTx route");
    struct fixture a, b;
    TEST_ASSERT(fixture_init(&a, intid), "fixture A");
    if (!fixture_init(&b, intid)) { kfree(a.pci); TEST_ASSERT(false, "fixture B"); }
    int err = 0;
    struct KObj_IRQ *ka = pci_irq_create(a.pci, PCI_IRQ_INTX, 0, &err);
    struct KObj_IRQ *kb = pci_irq_create(b.pci, PCI_IRQ_INTX, 0, &err);
    if (!ka || !kb) {
        if (ka) kobj_irq_unref(ka);
        if (kb) kobj_irq_unref(kb);
        kfree(a.pci); kfree(b.pci);
        TEST_ASSERT(false, "two functions can subscribe to one line");
    }
    bool good = masked(&a) && masked(&b);
    struct KObj_IRQ *raw = kobj_irq_create(intid);
    good = good && raw == NULL;
    if (raw) kobj_irq_unref(raw);
    struct pci_irq_event ea, eb, replay;
    good = good && pci_irq_wait(ka, 1, &ea) == 0 && masked(&a); // WAIT never arms
    good = good && pci_irq_arm(ka) == 0 && pci_irq_arm(kb) == 0;
    good = good && !masked(&a) && !masked(&b) && pci_irq_arm(ka) == -T_E_INVAL;
    // Dispatch observes both assertions and masks only those functions.
    a.config[PCI_CFG_STATUS] = PCI_STATUS_INTERRUPT | PCI_STATUS_CAP_LIST;
    b.config[PCI_CFG_STATUS] = PCI_STATUS_INTERRUPT;
    pci_irq_test_dispatch(intid);
    good = good && masked(&a) && masked(&b);
    good = good && a.config[PCI_CFG_STATUS] == (PCI_STATUS_INTERRUPT | PCI_STATUS_CAP_LIST);
    good = good && pci_irq_wait(ka, 1, &ea) == 1 && pci_irq_wait(kb, 1, &eb) == 1;
    good = good && ea.generation != eb.generation;
    good = good && pci_irq_wait(ka, 1, &replay) == 1 && replay.sequence == ea.sequence;
    good = good && pci_irq_complete(ka, eb.generation, eb.sequence) == -T_E_INVAL;
    good = good && pci_irq_complete(ka, ea.generation, ea.sequence) == -T_E_AGAIN;
    a.config[PCI_CFG_STATUS] = PCI_STATUS_CAP_LIST; // device acknowledgement
    good = good && pci_irq_wait(ka, 10000000, &replay) == 1;
    good = good && replay.reason == PCI_IRQ_RETRY;
    good = good && pci_irq_complete(ka, ea.generation, ea.sequence) == 0;
    good = good && !masked(&a) && masked(&b); // stalled peer cannot block A
    // A gets a second interrupt while B still holds its first ticket.
    a.config[PCI_CFG_STATUS] |= PCI_STATUS_INTERRUPT;
    pci_irq_test_dispatch(intid);
    good = good && pci_irq_wait(ka, 1, &replay) == 1 && replay.sequence > ea.sequence;
    good = good && pci_irq_complete(ka, ea.generation, ea.sequence) == -T_E_INVAL;
    good = good && pci_irq_wait(kb, 1, &replay) == 1 && replay.sequence == eb.sequence;
    // Busy guard is checked before tsleep's single-waiter assertion.
    ka->waiting = true;
    good = good && pci_irq_wait(ka, 1, &replay) == -T_E_BUSY;
    ka->waiting = false;
    good = good && pci_irq_disable(ka) == 0 && masked(&a);
    good = good && pci_irq_arm(ka) == -T_E_INVAL;
    good = good && pci_irq_wait(ka, 1, &replay) == -T_E_CANCELED;
    kobj_pci_quiesce(b.pci);
    good = good && pci_irq_wait(kb, 1, &replay) == -T_E_CANCELED && masked(&b);
    kobj_irq_unref(ka); kobj_irq_unref(kb);
    ka = pci_irq_create(b.pci, PCI_IRQ_INTX, 0, &err);
    good = good && ka == NULL && err == -T_E_CANCELED;
    if (ka) kobj_irq_unref(ka);
    good = good && a.pci->ref == 1 && b.pci->ref == 1;
    kfree(a.pci); kfree(b.pci);
    TEST_ASSERT(good, "shared isolation, explicit tickets, retries and terminal revoke");
}

// Controller allocation tests never publish a message address to a device,
// so source quiescence is known independently of device-reset machinery.
#include "../../arch/arm64/gic_msi.h"
void test_pci_msi_allocator(void);
void test_pci_msi_allocator(void) {
    struct dtb_pci_msi dt;
    if (!dtb_pci_msi_route(8, &dt)) return;
    struct gic_msi_route a, b;
    TEST_ASSERT(gic_msi_alloc(8, 0, &a), "firmware-selected MSI allocation");
    TEST_ASSERT(gic_msi_reserved(a.intid), "allocated SPI is reserved");
    TEST_ASSERT(!kobj_irq_create(a.intid), "MSI SPI cannot become a raw IRQ");
    TEST_ASSERT(!gic_intid_enabled(a.intid), "new vector is masked");
    TEST_ASSERT(gic_msi_set_pending(&a), "synthetic pending notification");
    TEST_ASSERT(gic_msi_retire(&a, true), "quiescent source drains controller");
    TEST_ASSERT(gic_msi_alloc(8, 0, &b), "reuse after proof of quiescence");
    TEST_EXPECT_EQ(a.intid, b.intid, "allocator reuses drained vector");
    TEST_ASSERT(a.generation != b.generation, "lease generation advances");
    TEST_ASSERT(!gic_msi_retire(&a, true), "stale lease cannot retire replacement");
    TEST_ASSERT(!gic_msi_retire(&b, false), "missing source proof quarantines");
    struct gic_msi_route c;
    TEST_ASSERT(gic_msi_alloc(8, 1, &c), "peer allocation while quarantined");
    TEST_ASSERT(c.intid != b.intid, "quarantine prevents reassignment");
    TEST_ASSERT(gic_msi_retire(&c, true), "peer cleanup");
    TEST_ASSERT(gic_msi_retire(&b, true), "later source proof permits reclamation");
    TEST_ASSERT(!gic_msi_retire(&b, true), "retire is not double-free");
    // Bound exhaustion and unwind every successful allocation.
    struct gic_msi_route routes[64];
    u32 n = 0;
    u32 bound = dt.kind == DTB_MSI_ITS ? 8 : 64;
    while (n < bound && gic_msi_alloc(8, n, &routes[n])) n++;
    struct gic_msi_route extra;
    bool exhausted = !gic_msi_alloc(8, bound, &extra);
    bool cleaned = true;
    if (!exhausted) cleaned = gic_msi_retire(&extra, true);
    for (u32 i = 0; i < n; i++) if (!gic_msi_retire(&routes[i], true)) cleaned = false;
    TEST_ASSERT(n == bound && exhausted && cleaned, "bounded pool exhaustion and complete unwind");
}

void test_pci_restart_placement(void);
void test_pci_restart_placement(void) {
    u64 pa[PCI_BAR_COUNT], size[PCI_BAR_COUNT];
    u64 before = kobj_pci_live_count();
    for (u32 round = 0; round < 16; round++) {
        struct KObj_PCI *k = kobj_pci_claim(4, 0); // unused virtio RNG function
        TEST_ASSERT(k != NULL, "RNG claim/restart");
        bool same = true;
        for (u32 bar = 0; bar < PCI_BAR_COUNT; bar++) {
            if (!round) { pa[bar] = k->bars[bar].pa; size[bar] = k->bars[bar].size; }
            else if (pa[bar] != k->bars[bar].pa || size[bar] != k->bars[bar].size) same = false;
        }
        kobj_pci_unref(k);
        TEST_ASSERT(same, "restart must reuse hardware BAR placement");
    }
    TEST_EXPECT_EQ(kobj_pci_live_count(), before, "repeated claim releases all live capabilities");
}

// Real virtio-RNG queue over mediated MSI-X. Device completion while the entry
// is masked must survive in PBA; no ISR read is used anywhere in this test.
#include <thylacine/dma_handle.h>
#include <thylacine/page.h>
#include "../../arch/arm64/mmu.h"
#include "../../arch/arm64/mmio.h"
#include "../../arch/arm64/timer.h"
static bool rng_used(volatile u16 *used, u16 expected) {
    u64 deadline = timer_now_ns() + 1000000000ull;
    while (*used != expected) {
        if (timer_now_ns() >= deadline) return false;
        __asm__ volatile("yield" ::: "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return true;
}
void test_pci_msix_rng(void);
void test_pci_msix_rng(void) {
    struct dtb_pci_msi route;
    if (!dtb_pci_msi_route(8, &route)) return;
    struct KObj_PCI *pci = kobj_pci_claim(4, 0);
    struct KObj_IRQ *irq = NULL;
    struct KObj_DMA *dma = NULL;
    const char *failure = NULL;
#define RNG_CHECK(c, message) do { if (!(c)) { failure = message; goto cleanup; } } while (0)
    RNG_CHECK(pci && pci->common_cfg, "MSI-X RNG claim/common mapping");
    int error;
    irq = pci_irq_create(pci, PCI_IRQ_MSIX, 0, &error);
    RNG_CHECK(irq, "MSI-X RNG endpoint creation");
    struct pci_irq_info info;
    RNG_CHECK(pci_irq_get_info(irq, &info) == 0 && info.mode == PCI_IRQ_MSIX &&
              info.table_index < pci->msix.entries, "MSI-X table index report");
    volatile u8 *c = pci->common_cfg;
#define RNG_W8(off, val) io_write8(c + (off), (val))
#define RNG_W16(off, val) io_write16(c + (off), (val))
#define RNG_W32(off, val) io_write32(c + (off), (val))
#define RNG_R16(off) io_read16(c + (off))
#define RNG_R32(off) io_read32(c + (off))
    RNG_W8(20, 3); RNG_W32(0, 1);
    RNG_CHECK(RNG_R32(4) & 1u, "RNG offers VERSION_1");
    RNG_W32(8, 0); RNG_W32(12, 0);
    RNG_W32(8, 1); RNG_W32(12, 1);
    RNG_W8(20, 11);
    RNG_CHECK(io_read8(c + 20) & 8u, "RNG accepts negotiated features");
    RNG_W16(16, (u16)info.table_index);
    RNG_W16(22, 0);
    RNG_CHECK(RNG_R16(24) >= 8, "RNG queue size");
    RNG_W16(24, 8); RNG_W16(26, (u16)info.table_index);
    RNG_CHECK(RNG_R16(16) == info.table_index && RNG_R16(26) == info.table_index,
              "RNG accepts config and queue vector selection");
    dma = kobj_dma_create(16384);
    RNG_CHECK(dma, "RNG test DMA allocation");
    u64 pa = kobj_dma_pa_at(dma, 0);
    volatile u8 *mem = pa_to_kva(pa);
    for (u32 reg = 32, page = 0; reg <= 48; reg += 8, page++) {
        u64 address = pa + page * 4096;
        RNG_W32(reg, (u32)address); RNG_W32(reg + 4, (u32)(address >> 32));
    }
    struct pci_region *notify = &pci->regions[VIRTIO_PCI_CAP_NOTIFY_CFG - 1];
    u64 offset = (u64)RNG_R16(30) * pci->notify_off_multiplier;
    RNG_CHECK(notify->present && offset <= notify->length && notify->length - offset >= 2,
              "RNG notify offset in capability window");
    volatile u16 *doorbell = mmu_map_mmio(pci->bars[notify->bar].pa + notify->offset + offset, 2);
    RNG_CHECK(doorbell, "RNG notify mapping");
    RNG_W16(28, 1); RNG_W8(20, 15);
    // Descriptor 0: one writable entropy buffer. Queue available/used rings
    // occupy separate zeroed pages; ring entries select descriptor zero twice.
    *(volatile u64 *)(mem + 0) = pa + 12288;
    *(volatile u32 *)(mem + 8) = 32;
    *(volatile u16 *)(mem + 12) = 2;
    *(volatile u16 *)(mem + 4096 + 4) = 0;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile u16 *)(mem + 4096 + 2) = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    io_write16(doorbell, 0);
    RNG_CHECK(rng_used((volatile u16 *)(mem + 8192 + 2), 1), "RNG DMA completes while MSI-X masked");
    struct pci_irq_event first, second;
    RNG_CHECK(pci_irq_wait(irq, 1000000, &first) == 0, "DISARMED WAIT does not unmask MSI-X");
    RNG_CHECK(pci_irq_arm(irq) == 0, "initial MSI-X ARM");
    RNG_CHECK(pci_irq_wait(irq, 1000000000, &first) == 1, "masked PBA notification delivered on ARM");
    RNG_CHECK(first.count >= 1 && first.sequence != 0, "MSI-X delivery ticket");
    // A second queue completion occurs while the first ticket owns the mask.
    *(volatile u16 *)(mem + 4096 + 6) = 0;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile u16 *)(mem + 4096 + 2) = 2;
    __asm__ volatile("dsb sy" ::: "memory");
    io_write16(doorbell, 0);
    RNG_CHECK(rng_used((volatile u16 *)(mem + 8192 + 2), 2), "RNG completes again under outstanding ticket");
    RNG_CHECK(pci_irq_wait(irq, 1000000, &second) == 1 && second.sequence == first.sequence,
              "MSI-X WAIT replays outstanding ticket");
    RNG_CHECK(pci_irq_complete(irq, first.generation, first.sequence) == 0,
              "MSI-X COMPLETE unmasks without ISR acknowledgement");
    RNG_CHECK(pci_irq_wait(irq, 1000000000, &second) == 1 && second.sequence > first.sequence,
              "second masked PBA notification survives COMPLETE");
    RNG_CHECK(pci_irq_complete(irq, first.generation, first.sequence) < 0,
              "old MSI-X ticket cannot complete new event");
    RNG_CHECK(pci_irq_complete(irq, second.generation, second.sequence) == 0,
              "second MSI-X completion");
cleanup:
    // Reset before releasing DMA, independent of which setup/assertion failed.
    if (pci) kobj_pci_quiesce(pci);
    if (irq) kobj_irq_unref(irq);
    if (dma) kobj_dma_unref(dma);
    if (pci) kobj_pci_unref(pci);
    if (failure) test_fail(failure);
#undef RNG_CHECK
#undef RNG_W8
#undef RNG_W16
#undef RNG_W32
#undef RNG_R16
#undef RNG_R32
}

// Fault injection leaves actual table MMIO in place, then rejects readback.
// Exercise rollback and peer wakeup, not a synthetic copy of the IRQ state.
#include <thylacine/thread.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
void pci_test_msix_fail(struct KObj_PCI *pci, u32 stages);
void pci_irq_test_msix_dispatch(u32 intid);
void pci_irq_test_controller_fault(u32 node);
bool gic_its_test_fault_notify(u32 node);
static struct KObj_IRQ *fault_wait_irq;
static volatile bool fault_wait_exited;
static int fault_wait_result;
static void fault_wait_entry(void) {
    struct pci_irq_event event;
    fault_wait_result = pci_irq_wait(fault_wait_irq, 1000000000ull, &event);
    test_kthread_park_terminal(&fault_wait_exited);
}
static bool fault_wait_parked(struct Thread *thread) {
    irq_state_t f = spin_lock_irqsave(&fault_wait_irq->rendez.lock);
    bool parked = fault_wait_irq->rendez.waiter == thread;
    spin_unlock_irqrestore(&fault_wait_irq->rendez.lock, f); return parked;
}
void test_pci_msix_failures(void);
void test_pci_msix_failures(void) {
    struct dtb_pci_msi route;
    if (!dtb_pci_msi_route(8, &route)) return;
    u32 baseline = gic_msi_live_count();
    struct KObj_PCI *pci = kobj_pci_claim(4, 0);
    struct KObj_IRQ *a = NULL, *b = NULL;
    struct Thread *waiter = NULL;
    const char *failure = NULL;
    int error;
#define FAULT_CHECK(c, text) do { if (!(c)) { failure = text; goto cleanup; } } while (0)
    FAULT_CHECK(pci && pci->msix.entries >= 2, "RNG exposes two MSI-X entries");
    pci_test_msix_fail(pci, 1);
    a = pci_irq_create(pci, PCI_IRQ_MSIX, 0, &error);
    FAULT_CHECK(!a && error == -T_E_IO, "program readback failure propagates");
    FAULT_CHECK(gic_msi_live_count() == baseline + 1, "uncertain installed route quarantined");
    u16 ctrl = virtio_pci_cfg_read16(pci->vpd, pci->msix.cap_offset + 2);
    FAULT_CHECK(!(ctrl & PCI_MSIX_ENABLE) && (ctrl & PCI_MSIX_MASK_ALL), "rollback disables MSI-X function");
    a = pci_irq_create(pci, PCI_IRQ_INTX, 0, &error);
    FAULT_CHECK(a && pci_irq_arm(a) == 0, "INTx fallback after failed initial programming");
    kobj_pci_quiesce(pci);
    kobj_irq_unref(a); a = NULL;
    kobj_pci_unref(pci); pci = NULL;
    FAULT_CHECK(gic_msi_live_count() == baseline, "reset reclaims failed route quarantine");
    for (u32 phase = 0; phase < 3; phase++) {
        pci = kobj_pci_claim(4, 0);
        FAULT_CHECK(pci, "fresh claim after reset");
        a = pci_irq_create(pci, PCI_IRQ_MSIX, 0, &error);
        b = pci_irq_create(pci, PCI_IRQ_MSIX, 1, &error);
        FAULT_CHECK(a && b && pci_irq_arm(a) == 0, "two vector endpoints configured");
        if (phase) FAULT_CHECK(pci_irq_arm(b) == 0, "peer armed for dispatch fault");
        fault_wait_irq = b; fault_wait_exited = false; fault_wait_result = 99;
        waiter = thread_create(kproc(), fault_wait_entry);
        FAULT_CHECK(waiter, "peer waiter thread");
        ready(waiter);
        TEST_YIELD_UNTIL_SOFT(fault_wait_parked(waiter) || fault_wait_exited);
        FAULT_CHECK(fault_wait_parked(waiter), "peer is blocked before fault");
        if (phase == 2) {
            struct dtb_pci_msi actual;
            u16 rid = ((u16)pci->bus << 8) | (pci->dev << 3) | pci->fn;
            FAULT_CHECK(dtb_pci_msi_route(rid, &actual), "fault fixture controller identity");
            if (actual.kind == DTB_MSI_ITS) {
                FAULT_CHECK(gic_its_test_fault_notify(actual.node), "ITS fault mailbox queued");
            } else pci_irq_test_controller_fault(actual.node);
        } else {
            pci_test_msix_fail(pci, 2);
            if (phase) pci_irq_test_msix_dispatch(a->intid);
            else FAULT_CHECK(pci_irq_arm(b) == -T_E_IO, "live ARM failure is I/O fault, not cancellation");
        }
        test_kthread_join_free(waiter, &fault_wait_exited); waiter = NULL;
        FAULT_CHECK(fault_wait_result == -T_E_IO, "function fault wakes blocked peer with EIO");
        struct pci_irq_info ai, bi;
        FAULT_CHECK(!pci_irq_get_info(a, &ai) && !pci_irq_get_info(b, &bi) &&
            ai.state == PCI_IRQ_FAULT && bi.state == PCI_IRQ_FAULT, "all function vectors fault together");
        ctrl = virtio_pci_cfg_read16(pci->vpd, pci->msix.cap_offset + 2);
        FAULT_CHECK(!(ctrl & PCI_MSIX_ENABLE) && (ctrl & PCI_MSIX_MASK_ALL), "failed entry mask contained by function mask");
        kobj_irq_unref(a); a = NULL; kobj_irq_unref(b); b = NULL;
        a = pci_irq_create(pci, PCI_IRQ_INTX, 0, &error);
        FAULT_CHECK(!a && error == -T_E_IO, "faulted claim cannot acquire new interrupt authority");
        kobj_pci_quiesce(pci); kobj_pci_unref(pci); pci = NULL;
        FAULT_CHECK(gic_msi_live_count() == baseline, "all faulted vector leases reclaimed after reset");
    }
cleanup:
    if (waiter) {
        pci_irq_disable(b);
        test_kthread_join_free(waiter, &fault_wait_exited);
    }
    if (pci) { pci_test_msix_fail(pci, 0); kobj_pci_quiesce(pci); }
    if (a) kobj_irq_unref(a);
    if (b) kobj_irq_unref(b);
    if (pci) kobj_pci_unref(pci);
    TEST_ASSERT(failure == NULL, failure ? failure : "MSI-X failure rollback and peer wake");
#undef FAULT_CHECK
}


void test_pci_intx_recovery(void);
void test_pci_intx_recovery(void) {
    u32 intid = 0;
    TEST_ASSERT(dtb_pci_intx_route(0, 1, &intid), "recovery fixture route");
    struct fixture a, b;
    TEST_ASSERT(fixture_init(&a, intid), "recovery fixture A");
    if (!fixture_init(&b, intid)) { kfree(a.pci); TEST_ASSERT(false, "recovery fixture B"); }
    int error = 0;
    bool good = true;
    struct KObj_IRQ *ka = pci_irq_create(a.pci, PCI_IRQ_INTX, 0, &error);
    struct KObj_IRQ *kb = NULL;
    if (!ka || pci_irq_arm(ka)) { good = false; goto out; }
    for (u32 round = 0; round < 2; round++) {
        // A synthetic unexplained wire assertion hits the same bounded domain
        // handler as hardware. Neither fixture claims an Interrupt Status bit.
        for (u32 i = 0; i < 32; i++) pci_irq_test_dispatch(intid);
        struct pci_irq_event event = {0};
        if (pci_irq_wait(ka, 1, &event) != -T_E_IO || !masked(&a) ||
            gic_intid_enabled(intid)) good = false;
        u64 until = timer_now_ns() + 110000000ull;
        if (round == 0) {
            while (timer_now_ns() < until) sched_yield_hint();
            kb = pci_irq_create(b.pci, PCI_IRQ_INTX, 0, &error);
            if (kb || error != -T_E_IO) good = false; // old subscriber still owns ticket
            if (kb) { kobj_irq_unref(kb); kb = NULL; }
        }
        kobj_irq_unref(ka); ka = NULL;
        if (round == 1) {
            kb = pci_irq_create(b.pci, PCI_IRQ_INTX, 0, &error);
            if (kb || error != -T_E_IO) good = false; // cooldown survives last close
            if (kb) { kobj_irq_unref(kb); kb = NULL; }
            while (timer_now_ns() < until) sched_yield_hint();
        }
        ka = pci_irq_create(a.pci, PCI_IRQ_INTX, 0, &error);
        if (!ka || pci_irq_arm(ka)) { good = false; goto out; }
        a.config[PCI_CFG_STATUS] = PCI_STATUS_INTERRUPT;
        pci_irq_test_dispatch(intid);
        if (pci_irq_wait(ka, 1, &event) != 1) good = false;
        a.config[PCI_CFG_STATUS] = 0;
        if (pci_irq_complete(ka, event.generation, event.sequence)) good = false;
    }
out:
    if (ka) kobj_irq_unref(ka);
    if (kb) kobj_irq_unref(kb);
    kfree(a.pci); kfree(b.pci);
    TEST_ASSERT(good, "domain recovery requires cooldown and closed subscribers; fresh ticket delivers");
}

// Last-close races with the permanent shared-domain dispatcher. The worker
// receives only a line number, never a borrowed endpoint. A continuously live
// peer must receive every assertion while the other endpoint is unpublished
// and freed; dispatch pins must drain before its allocation can be reused.
static u32 close_race_intid;
static u32 close_race_request, close_race_done;
static bool close_race_stop;
static volatile bool close_race_exited;
static void close_race_entry(void) {
    u32 seen = 0;
    while (!__atomic_load_n(&close_race_stop, __ATOMIC_ACQUIRE)) {
        u32 request = __atomic_load_n(&close_race_request, __ATOMIC_ACQUIRE);
        if (request == seen) { sched(); continue; }
        pci_irq_test_dispatch(close_race_intid);
        seen = request;
        __atomic_store_n(&close_race_done, seen, __ATOMIC_RELEASE);
    }
    test_kthread_park_terminal(&close_race_exited);
}
void test_pci_intx_close_dispatch(void);
void test_pci_intx_close_dispatch(void) {
    u64 baseline = kobj_irq_live_count();
    u32 intid = 0;
    TEST_ASSERT(dtb_pci_intx_route(0, 1, &intid), "shared close-race route");
    struct fixture peer = {0}, transient = {0};
    struct KObj_IRQ *live = NULL, *closing = NULL;
    struct Thread *worker = NULL;
    const char *failure = NULL;
    int error = 0;
#define RACE_CHECK(c, text) do { if (!(c)) { failure = text; goto cleanup_race; } } while (0)
    RACE_CHECK(fixture_init(&peer, intid), "live peer fixture");
    live = pci_irq_create(peer.pci, PCI_IRQ_INTX, 0, &error);
    RACE_CHECK(live && pci_irq_arm(live) == 0, "live peer armed");
    close_race_intid = intid;
    close_race_request = close_race_done = 0;
    close_race_stop = false; close_race_exited = false;
    worker = thread_create(kproc(), close_race_entry);
    RACE_CHECK(worker, "dispatch race worker");
    ready(worker);
    for (u32 round = 1; round <= 64; round++) {
        RACE_CHECK(fixture_init(&transient, intid), "transient fixture");
        closing = pci_irq_create(transient.pci, PCI_IRQ_INTX, 0, &error);
        RACE_CHECK(closing && pci_irq_arm(closing) == 0, "transient armed");
        peer.config[PCI_CFG_STATUS] = PCI_STATUS_INTERRUPT;
        transient.config[PCI_CFG_STATUS] = PCI_STATUS_INTERRUPT;
        __atomic_store_n(&close_race_request, round, __ATOMIC_RELEASE);
        // Alternate scheduling gives both early-close and dispatched-close
        // orders a chance; the test does not assume which CPU wins the lock.
        if (round & 1) sched();
        kobj_irq_unref(closing); closing = NULL;
        TEST_YIELD_UNTIL_SOFT(__atomic_load_n(&close_race_done, __ATOMIC_ACQUIRE) == round);
        RACE_CHECK(__atomic_load_n(&close_race_done, __ATOMIC_ACQUIRE) == round,
                   "dispatch completes despite concurrent last close");
        struct pci_irq_event event;
        RACE_CHECK(pci_irq_wait(live, 1, &event) == 1 && event.sequence == round,
                   "live neighbor receives every race assertion");
        peer.config[PCI_CFG_STATUS] = 0;
        RACE_CHECK(pci_irq_complete(live, event.generation, event.sequence) == 0,
                   "live neighbor rearms after race");
        RACE_CHECK(transient.pci->ref == 1, "last close releases PCI parent");
        kfree(transient.pci); transient.pci = NULL;
    }
cleanup_race:
    if (worker) {
        __atomic_store_n(&close_race_stop, true, __ATOMIC_RELEASE);
        test_kthread_join_free(worker, &close_race_exited);
    }
    if (closing) kobj_irq_unref(closing);
    if (live) kobj_irq_unref(live);
    if (transient.pci) kfree(transient.pci);
    if (peer.pci) kfree(peer.pci);
    TEST_ASSERT(failure == NULL, failure ? failure : "close/dispatch race");
    TEST_EXPECT_EQ(kobj_irq_live_count(), baseline, "close race leaks no IRQ handles");
#undef RACE_CHECK
}
