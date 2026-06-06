// P4-Ib: hw-handle integration tests via the handle table.
//
// Per specs/handles.tla NoHwDup + HwResourceExclusive. The handle-table
// side enforces:
//   - handle_dup of KOBJ_MMIO / KOBJ_IRQ returns -1 (NoHwDup);
//   - handle_close releases the underlying KObj refcount, which for
//     KObj_MMIO releases the PA claim and for KObj_IRQ disables the GIC line
//     + clears the INTID claim.

#include "test.h"

#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/proc.h>
#include <thylacine/smp.h>          // IPI_RESCHED (R9 F142 test)
#include <thylacine/types.h>

#include "../../arch/arm64/uart.h"
#include "../../arch/arm64/timer.h" // TIMER_INTID_EL1_VIRT (R9 F142 test)
#include "../../arch/arm64/gic.h"   // gic_max_intid (R12-gic-edge F205 test)

void test_handle_hw_mmio_dup_rejected(void);
void test_handle_hw_irq_dup_rejected(void);
void test_handle_hw_mmio_close_releases_claim(void);
void test_handle_hw_irq_close_releases_intid(void);
void test_handle_hw_irq_kernel_reserved_rejected(void);
void test_handle_hw_irq_out_of_range_rejected(void);

#define TEST_PA_C   0x100020000ull
#define TEST_PA_D   0x100030000ull

// handle_dup of a KOBJ_MMIO handle returns -1 (NoHwDup invariant).
void test_handle_hw_mmio_dup_rejected(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc NULL");

    struct KObj_MMIO *k = kobj_mmio_create(TEST_PA_C, PAGE_SIZE);
    TEST_ASSERT(k != NULL, "mmio create failed");

    hidx_t h = handle_alloc(kp, KOBJ_MMIO, RIGHT_READ | RIGHT_WRITE, k);
    TEST_ASSERT(h >= 0, "handle_alloc(KOBJ_MMIO) failed");

    hidx_t dup = handle_dup(kp, h, RIGHT_READ);
    TEST_ASSERT(dup < 0, "handle_dup(KOBJ_MMIO) should be rejected (NoHwDup)");

    int rc = handle_close(kp, h);
    TEST_EXPECT_EQ(rc, 0, "handle_close failed");
}

// handle_dup of a KOBJ_IRQ handle returns -1 (NoHwDup invariant).
// Uses SGI 1 (IPI_IRQFWD_TEST) — same INTID as the irqfwd suite. The
// previous test's kobj_irq_create may have left state; we destroy
// before re-creating to ensure a clean slot.
void test_handle_hw_irq_dup_rejected(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc NULL");

    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "irq create failed (intid already claimed?)");

    hidx_t h = handle_alloc(kp, KOBJ_IRQ, RIGHT_SIGNAL, k);
    TEST_ASSERT(h >= 0, "handle_alloc(KOBJ_IRQ) failed");

    hidx_t dup = handle_dup(kp, h, RIGHT_SIGNAL);
    TEST_ASSERT(dup < 0, "handle_dup(KOBJ_IRQ) should be rejected (NoHwDup)");

    int rc = handle_close(kp, h);
    TEST_EXPECT_EQ(rc, 0, "handle_close failed");
    // After close: the KObj_IRQ refcount drops to 0, gic_disable_irq +
    // intid_release fire, so a subsequent kobj_irq_create can succeed.
}

// handle_close on a KOBJ_MMIO handle releases the underlying claim:
// the same PA can be re-claimed immediately after close.
void test_handle_hw_mmio_close_releases_claim(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc NULL");

    struct KObj_MMIO *k1 = kobj_mmio_create(TEST_PA_D, PAGE_SIZE);
    TEST_ASSERT(k1 != NULL, "first mmio create failed");

    hidx_t h1 = handle_alloc(kp, KOBJ_MMIO, RIGHT_READ, k1);
    TEST_ASSERT(h1 >= 0, "handle_alloc failed");

    // Direct kobj_mmio_create for the same PA should fail (claim held).
    struct KObj_MMIO *blocked = kobj_mmio_create(TEST_PA_D, PAGE_SIZE);
    TEST_ASSERT(blocked == NULL, "claim should be held by h1");

    // Close the handle — releases k1's refcount → claim returned to pool.
    int rc = handle_close(kp, h1);
    TEST_EXPECT_EQ(rc, 0, "handle_close failed");

    // Now the claim is free; a fresh create should succeed.
    struct KObj_MMIO *k2 = kobj_mmio_create(TEST_PA_D, PAGE_SIZE);
    TEST_ASSERT(k2 != NULL, "create after close should succeed");
    kobj_mmio_unref(k2);
}

// R9 F142 (P0) regression: kobj_irq_create rejects kernel-reserved
// INTIDs. irqfwd_init pre-marks IPI_RESCHED (SGI 0) and
// TIMER_INTID_EL1_VIRT (PPI 27) in g_intid_claimed[]. A caller
// (kernel or syscall path with CAP_HW_CREATE) attempting either MUST
// receive NULL. Without this, any cap-holding userspace driver could
// overwrite the timer handler or IPI dispatch entry.
void test_handle_hw_irq_kernel_reserved_rejected(void) {
    struct KObj_IRQ *k0 = kobj_irq_create(IPI_RESCHED);
    TEST_ASSERT(k0 == NULL, "kobj_irq_create(IPI_RESCHED=0) must be rejected");
    struct KObj_IRQ *ktimer = kobj_irq_create(TIMER_INTID_EL1_VIRT);
    TEST_ASSERT(ktimer == NULL,
                "kobj_irq_create(TIMER=27) must be rejected");
}

// handle_close on a KOBJ_IRQ handle releases the INTID claim: a fresh
// kobj_irq_create on the same INTID should succeed.
void test_handle_hw_irq_close_releases_intid(void) {
    struct Proc *kp = kproc();
    TEST_ASSERT(kp != NULL, "kproc NULL");

    struct KObj_IRQ *k = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k != NULL, "irq create failed");

    hidx_t h = handle_alloc(kp, KOBJ_IRQ, RIGHT_SIGNAL, k);
    TEST_ASSERT(h >= 0, "handle_alloc failed");

    // Another create on the same INTID should fail (claim held).
    struct KObj_IRQ *blocked = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(blocked == NULL, "intid claim should be held");

    int rc = handle_close(kp, h);
    TEST_EXPECT_EQ(rc, 0, "handle_close failed");

    // Now a fresh create on the same INTID should succeed.
    struct KObj_IRQ *k2 = kobj_irq_create(IPI_IRQFWD_TEST);
    TEST_ASSERT(k2 != NULL, "create after close should succeed");
    kobj_irq_unref(k2);
}

// R12-gic-edge audit close (F205 P3): intid > g_max_intid (runtime
// distributor line count from GICD_TYPER.ITLinesNumber) must be
// rejected by intid_try_claim before reaching ICFGR / ISENABLER
// writes. Without this tighter bound, a syscall caller with
// CAP_HW_CREATE could pass intid in (g_max_intid, GIC_NUM_INTIDS]
// and the helpers would issue UNPREDICTABLE register writes for
// unimplemented INTIDs per IHI 0069 §12.9.7.
void test_handle_hw_irq_out_of_range_rejected(void) {
    u32 max = gic_max_intid();
    TEST_ASSERT(max > 0,
                "gic_max_intid() must be > 0 after gic_init");
    TEST_ASSERT(max + 1u < GIC_NUM_INTIDS,
                "test requires g_max_intid < GIC_NUM_INTIDS - 1 headroom");
    struct KObj_IRQ *k = kobj_irq_create(max + 1u);
    TEST_ASSERT(k == NULL,
                "kobj_irq_create(g_max_intid + 1) must be rejected by intid_try_claim");
}
