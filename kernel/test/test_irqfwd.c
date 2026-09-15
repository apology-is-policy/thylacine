// IRQ forwarding tests (P4-G).
//
// Verifies the kernel-internal kobj_irq_create/wait/destroy lifecycle
// + the GIC dispatch hook. Tests use SGI 1 (IPI_IRQFWD_TEST) as the
// fire source — software-triggered via gic_send_ipi(self, ...) so we
// don't need a real hardware device.

#include "test.h"

#include <thylacine/irqfwd.h>
#include <thylacine/smp.h>
#include <thylacine/types.h>

#include "../../arch/arm64/gic.h"
#include "../../arch/arm64/timer.h"   // F-A1 (C): timer_now_ns for the timeout test

// F-A1: an in-range test SPI (>= 32) for the level mask+ack test. 96 is not a
// PCI INTx line, so kobj_irq_create derives EDGE for it -- the test forces the
// level state (gic_set_spi_level_triggered + k->level) kobj_irq_create builds
// for a real PCI INTx line. Matches test_irq_probe's SPI (both create/destroy).
#define IRQFWD_LEVEL_TEST_INTID  96u

void test_irqfwd_create_destroy(void);
void test_irqfwd_refcount_lifecycle(void);
void test_irqfwd_wait_wakes_on_sgi(void);
void test_irqfwd_collapses_concurrent_fires(void);
void test_irqfwd_second_waiter_refused(void);
void test_irqfwd_level_mask_ack(void);
void test_irqfwd_wait_timeout(void);

// =============================================================================
// Helpers.
// =============================================================================

// Simple busy-wait via the timer counter. We use this instead of
// timer_busy_wait_ticks because we want sub-tick resolution for the
// "did the SGI deliver?" check after sending.
static void busy_spin_loops(unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        __asm__ __volatile__("yield" ::: "memory");
    }
}

// =============================================================================
// Tests.
// =============================================================================

void test_irqfwd_create_destroy(void) {
    u64 before = kobj_irq_live_count();

    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "kobj_irq_create(SGI 1) succeeds");

    TEST_EXPECT_EQ(kobj_irq_live_count(), before + 1,
                   "live count incremented");
    TEST_EXPECT_EQ(k->intid, (u32)IPI_IRQFWD_TEST, "intid recorded");
    TEST_EXPECT_EQ(k->ref, 1, "fresh ref=1");

    kobj_irq_destroy(k);
    TEST_EXPECT_EQ(kobj_irq_live_count(), before,
                   "live count restored after destroy");
}

void test_irqfwd_refcount_lifecycle(void) {
    u64 before = kobj_irq_live_count();

    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "create OK");

    kobj_irq_ref(k);
    TEST_EXPECT_EQ(k->ref, 2, "ref=2 after ref");
    TEST_EXPECT_EQ(kobj_irq_live_count(), before + 1,
                   "live count unchanged on ref");

    kobj_irq_unref(k);
    TEST_EXPECT_EQ(k->ref, 1, "ref=1 after first unref");
    TEST_EXPECT_EQ(kobj_irq_live_count(), before + 1,
                   "live count unchanged while ref > 0");

    kobj_irq_unref(k);
    TEST_EXPECT_EQ(kobj_irq_live_count(), before,
                   "live count drops on last unref");
}

void test_irqfwd_wait_wakes_on_sgi(void) {
    u64 fires_before = kobj_irq_total_fires();

    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "create OK");

    // Send SGI to self. The IRQ may fire before or after kobj_irq_wait
    // enters sleep — both paths are correct: pending_count is
    // incremented either way + sleep's cond catches the early-fire
    // case immediately.
    unsigned self = smp_cpu_idx_self();
    bool sent = gic_send_ipi(self, IPI_IRQFWD_TEST);
    TEST_ASSERT(sent, "gic_send_ipi(self, SGI 1) accepted");

    // Allow the SGI a few cycles to deliver before we hit kobj_irq_wait.
    // This isn't required for correctness (the cond loop handles
    // already-pending), but tightens timing for early-fire path.
    busy_spin_loops(50);

    u32 count = kobj_irq_wait(k);
    TEST_ASSERT(count >= 1, "wait returns at least 1 pending IRQ");

    TEST_ASSERT(kobj_irq_total_fires() >= fires_before + 1,
                "global fire counter incremented");

    kobj_irq_destroy(k);
}

void test_irqfwd_collapses_concurrent_fires(void) {
    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "create OK");

    // Send 3 IPIs in quick succession before waiting. The GIC may
    // collapse them at the redistributor level (SGI is edge-triggered;
    // re-sending while already pending may be coalesced). The kernel-
    // side counter tracks ACTUAL dispatch invocations.
    unsigned self = smp_cpu_idx_self();
    for (int i = 0; i < 3; i++) {
        gic_send_ipi(self, IPI_IRQFWD_TEST);
        busy_spin_loops(20);
    }
    busy_spin_loops(100);

    u32 count = kobj_irq_wait(k);
    // We can't guarantee exactly 3 fires due to GIC SGI coalescing, but
    // at least 1 must be observed (the IRQ delivered AT LEAST once).
    TEST_ASSERT(count >= 1, "at least 1 IRQ collapsed into wait");

    kobj_irq_destroy(k);
}

// RW-7 R1-F1: the KObj_IRQ Rendez is single-waiter -- sleep() extincts the
// kernel on a 2nd concurrent sleeper. The handle is shared across a multi-
// thread Proc's peer Threads, so kobj_irq_wait must REFUSE a 2nd concurrent
// waiter (return KOBJ_IRQ_WAIT_BUSY) rather than reach sleep(). A real first
// waiter is asleep inside sleep() and cannot be driven from this single-
// threaded test, so we simulate it by setting `waiting` directly: the guard
// is the same lock-protected check either way. Pre-fix, this path reached
// sleep() and extincted.
void test_irqfwd_second_waiter_refused(void) {
    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "create OK");

    k->waiting = true;          // a peer Thread "already holds" the waiter slot
    u32 r = kobj_irq_wait(k);
    TEST_EXPECT_EQ(r, (u32)KOBJ_IRQ_WAIT_BUSY,
                   "2nd concurrent waiter refused with BUSY (no extinction)");
    TEST_ASSERT(k->waiting, "the busy path leaves the first waiter's slot held");

    k->waiting = false;         // release the simulated slot before teardown
    kobj_irq_destroy(k);
}

// F-A1: the LEVEL mask+ack cycle. A level SPI (virtio-PCI INTx) must be MASKED
// by kobj_irq_dispatch (so a still-asserted line does not storm after EOI) and
// UNMASKED by the next kobj_irq_wait (re-arm) -- so a re-assertion arriving
// while masked re-fires on the next wait instead of being lost (the F-A1
// overlap). Force the level state kobj_irq_create builds for a PCI INTx line.
void test_irqfwd_level_mask_ack(void) {
    struct KObj_IRQ *k = kobj_irq_create(IRQFWD_LEVEL_TEST_INTID);
    TEST_ASSERT(k != NULL, "kobj_irq_create(SPI 96) succeeds");
    gic_set_spi_level_triggered(IRQFWD_LEVEL_TEST_INTID);
    k->level = true;
    TEST_ASSERT(gic_intid_enabled(IRQFWD_LEVEL_TEST_INTID),
                "SPI enabled after create");

    // Race-free: pend WHILE DISABLED so the assertion stays latched until the
    // wait's entry re-arm delivers it (test_irq_probe's pend-before-enable
    // pattern) -- otherwise an immediate delivery would run dispatch (and its
    // mask) BEFORE the wait's unmask, and the wait would then re-enable the
    // line, hiding the mask.
    gic_disable_irq(IRQFWD_LEVEL_TEST_INTID);
    TEST_ASSERT(!gic_intid_enabled(IRQFWD_LEVEL_TEST_INTID),
                "disabled before the latched pend");
    (void)gic_set_pending_spi(IRQFWD_LEVEL_TEST_INTID);

    // Wait: entry unmasks -> the latched pending delivers -> dispatch masks the
    // level line + counts -> the wait returns. 2 s guard so a broken re-arm
    // fails via timeout (count 0), never a suite hang.
    u32 c1 = kobj_irq_wait_timed(k, 2000000000ull);
    TEST_ASSERT(c1 >= 1, "level IRQ delivered on re-arm");
    TEST_ASSERT(!gic_intid_enabled(IRQFWD_LEVEL_TEST_INTID),
                "LEVEL line MASKED by dispatch (mask+ack) -- the discriminator");

    // Re-assert while masked, then wait: the unmask on re-entry re-fires the
    // latched assertion -- it is NOT lost (the property edge-forcing dropped).
    (void)gic_set_pending_spi(IRQFWD_LEVEL_TEST_INTID);
    u32 c2 = kobj_irq_wait_timed(k, 2000000000ull);
    TEST_ASSERT(c2 >= 1, "level IRQ re-fires after a re-assertion (unmask on re-wait)");
    TEST_ASSERT(!gic_intid_enabled(IRQFWD_LEVEL_TEST_INTID),
                "masked again after the re-fire");

    kobj_irq_destroy(k);   // gic_disable_irq + release the claim
}

// F-A1 (C): the timed wait bounds a no-IRQ wait instead of blocking forever.
void test_irqfwd_wait_timeout(void) {
    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "create OK");

    // Nothing fires. The wait must RETURN (count 0) near the deadline; the
    // pre-fix no-timeout kobj_irq_wait would block here forever.
    u64 t0 = timer_now_ns();
    u32 c = kobj_irq_wait_timed(k, 50000000ull);   // 50 ms
    u64 dt = timer_now_ns() - t0;
    TEST_EXPECT_EQ((u64)c, (u64)0u, "timeout returns count 0 (no IRQ)");
    TEST_ASSERT(dt >= 30000000ull, "waited ~ the deadline (not an instant return)");
    TEST_ASSERT(dt < 2000000000ull, "bounded -- did not block forever");

    kobj_irq_destroy(k);
}
