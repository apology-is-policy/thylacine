// Hardware allowance tests (ARCH section 28 I-34; specs/allowance.tla;
// docs/MENAGERIE.md section 4). Covers the CreateBegin gate
// (allowance_permits: MMIO containment / IRQ membership / DMA cap / broad /
// revoked), the CreateCommit re-check (allowance_handle_alloc), Confer
// (over-cap reject), Revoke, the rfork clone-inherit, and the free.
//
// The headline runtime regression for the spec's BUGGY_COMMIT_NO_RECHECK
// counterexample (the revoke-vs-create SMP race) is
// allowance.handle_alloc_revoked_aborts -- the in-flight create's commit
// aborts because the allowance was revoked between the gate check and the
// install. Maps to specs/allowance.tla::HandlesWithinAllowance.

#include "test.h"

#include <thylacine/allowance.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>

void test_allowance_null_is_broad(void);
void test_allowance_mmio_containment(void);
void test_allowance_irq_membership(void);
void test_allowance_dma_cap(void);
void test_allowance_revoked_permits_nothing(void);
void test_allowance_confer_rejects_overcap(void);
void test_allowance_handle_alloc_broad(void);
void test_allowance_handle_alloc_revoked_aborts(void);
void test_allowance_clone_inherit(void);
void test_allowance_free_null_tolerant(void);
void test_allowance_pci_claim_gate(void);

// Mirror the test_handle.c proc make/drop: proc_alloc gives a fresh Proc with
// an empty handle table + a NULL (broad) allowance; test_proc_drop ZOMBIEs +
// proc_free's it (which allowance_free's any conferred allowance).
static struct Proc *amk(void) { return proc_alloc(); }
static void adrop(struct Proc *p) { if (!p) return; p->state = PROC_STATE_ZOMBIE; proc_free(p); }

void test_allowance_null_is_broad(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(p->allowance == NULL, "fresh Proc is broad (allowance NULL)");
    // Broad -> permits any MMIO/IRQ/DMA (the as-built v1.0 path).
    TEST_ASSERT(allowance_permits(p, HW_RES_MMIO, 0x0a000000, 0x1000), "broad MMIO");
    TEST_ASSERT(allowance_permits(p, HW_RES_IRQ, 48, 0), "broad IRQ");
    TEST_ASSERT(allowance_permits(p, HW_RES_DMA, 0x100000, 0), "broad DMA");
    adrop(p);
}

void test_allowance_mmio_containment(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct hw_window w[1] = { { .base = 0x1000, .size = 0x1000 } };  // [0x1000, 0x2000)
    TEST_ASSERT(proc_confer_allowance(p, w, 1, NULL, 0, 0) == 0, "confer mmio");
    // Within -> permit.
    TEST_ASSERT(allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "exact window");
    TEST_ASSERT(allowance_permits(p, HW_RES_MMIO, 0x1400, 0x400), "sub-window");
    // Partial / outside -> deny.
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x1800, 0x1000), "straddles top edge");
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x3000, 0x100), "fully outside");
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x1000, 0), "zero size denied");
    // Overflow guard: base + size wraps -> deny (never a spurious permit).
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, ~(u64)0 - 0x10, 0x100), "base+size overflow");
    // A narrowed allowance does NOT permit a kind it never conferred.
    TEST_ASSERT(!allowance_permits(p, HW_RES_IRQ, 48, 0), "no irq conferred");
    TEST_ASSERT(!allowance_permits(p, HW_RES_DMA, 0x1000, 0), "no dma conferred");
    adrop(p);
}

void test_allowance_irq_membership(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 irq[2] = { 40, 50 };
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, irq, 2, 0) == 0, "confer irq");
    TEST_ASSERT(allowance_permits(p, HW_RES_IRQ, 40, 0), "irq 40 conferred");
    TEST_ASSERT(allowance_permits(p, HW_RES_IRQ, 50, 0), "irq 50 conferred");
    TEST_ASSERT(!allowance_permits(p, HW_RES_IRQ, 41, 0), "irq 41 not conferred");
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "no mmio conferred");
    adrop(p);
}

void test_allowance_dma_cap(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, 0, 0x1000) == 0, "confer dma cap");
    TEST_ASSERT(allowance_permits(p, HW_RES_DMA, 0x1000, 0), "dma at cap");
    TEST_ASSERT(allowance_permits(p, HW_RES_DMA, 0x800, 0), "dma under cap");
    TEST_ASSERT(!allowance_permits(p, HW_RES_DMA, 0x1001, 0), "dma over cap");
    TEST_ASSERT(!allowance_permits(p, HW_RES_DMA, 0, 0), "dma zero denied");
    adrop(p);

    // dma_max == 0 -> NO dma permitted.
    struct Proc *q = amk();
    TEST_ASSERT(q != NULL, "proc_alloc q");
    TEST_ASSERT(proc_confer_allowance(q, NULL, 0, NULL, 0, 0) == 0, "confer no-dma");
    TEST_ASSERT(!allowance_permits(q, HW_RES_DMA, 0x100, 0), "dma denied when cap 0");
    adrop(q);
}

void test_allowance_revoked_permits_nothing(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct hw_window w[1] = { { .base = 0x1000, .size = 0x1000 } };
    u32 irq[1] = { 40 };
    TEST_ASSERT(proc_confer_allowance(p, w, 1, irq, 1, 0x1000) == 0, "confer");
    TEST_ASSERT(allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "permits pre-revoke");
    proc_revoke_allowance(p);
    // The spec's Revoke: allowance[d] = {} -> permits nothing.
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "mmio denied post-revoke");
    TEST_ASSERT(!allowance_permits(p, HW_RES_IRQ, 40, 0), "irq denied post-revoke");
    TEST_ASSERT(!allowance_permits(p, HW_RES_DMA, 0x100, 0), "dma denied post-revoke");
    adrop(p);
}

void test_allowance_confer_rejects_overcap(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    // count > cap is checked before the NULL-pointer guard, so NULL is fine.
    TEST_ASSERT(proc_confer_allowance(p, NULL, ALLOWANCE_MMIO_MAX + 1, NULL, 0, 0) == -1,
        "mmio_count over cap rejected");
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, ALLOWANCE_IRQ_MAX + 1, 0) == -1,
        "irq_count over cap rejected");
    // A positive count with a NULL pointer is rejected.
    TEST_ASSERT(proc_confer_allowance(p, NULL, 1, NULL, 0, 0) == -1, "mmio NULL+count rejected");
    TEST_ASSERT(p->allowance == NULL, "no allowance installed on any reject");
    adrop(p);
}

void test_allowance_handle_alloc_broad(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    // Broad (NULL allowance) -> allowance_handle_alloc behaves like handle_alloc.
    hidx_t h = allowance_handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(h >= 0, "broad install succeeds");
    adrop(p);
}

void test_allowance_handle_alloc_revoked_aborts(void) {
    // THE revoke-vs-create race regression (specs/allowance.tla
    // BUGGY_COMMIT_NO_RECHECK). The CreateCommit re-check aborts an in-flight
    // create when the allowance was revoked between the gate check and the
    // install -- otherwise a handle slips through a being-revoked allowance.
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 irq[1] = { 40 };
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, irq, 1, 0) == 0, "confer");
    // Non-revoked narrowed allowance -> install succeeds (CreateCommit passes).
    hidx_t h = allowance_handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(h >= 0, "narrowed non-revoked install succeeds");
    // Revoke, then attempt a commit -> aborts (-1). The buggy variant (no
    // re-check) would install here and violate HandlesWithinAllowance.
    proc_revoke_allowance(p);
    hidx_t h2 = allowance_handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(h2 < 0, "revoked commit aborts (-1) -- the race regression");
    adrop(p);
}

void test_allowance_clone_inherit(void) {
    // Broad parent -> child stays NULL (broad).
    struct Proc *bp = amk(); struct Proc *bc = amk();
    TEST_ASSERT(bp && bc, "proc_alloc broad pair");
    TEST_ASSERT(bp->allowance == NULL, "broad parent");
    TEST_ASSERT(allowance_clone_into(bc, bp) == 0, "clone broad");
    TEST_ASSERT(bc->allowance == NULL, "broad parent -> child NULL");
    adrop(bc); adrop(bp);

    // Narrowed parent -> child gets an equally-narrow OWN copy.
    struct Proc *np = amk(); struct Proc *nc = amk();
    TEST_ASSERT(np && nc, "proc_alloc narrowed pair");
    struct hw_window w[1] = { { .base = 0x2000, .size = 0x1000 } };
    u32 irq[1] = { 42 };
    TEST_ASSERT(proc_confer_allowance(np, w, 1, irq, 1, 0x800) == 0, "confer parent");
    TEST_ASSERT(allowance_clone_into(nc, np) == 0, "clone narrowed");
    TEST_ASSERT(nc->allowance != NULL, "child narrowed");
    TEST_ASSERT(nc->allowance != np->allowance, "child has its OWN allowance (deep copy)");
    TEST_ASSERT(allowance_permits(nc, HW_RES_MMIO, 0x2000, 0x1000), "child inherits mmio");
    TEST_ASSERT(allowance_permits(nc, HW_RES_IRQ, 42, 0), "child inherits irq");
    TEST_ASSERT(allowance_permits(nc, HW_RES_DMA, 0x800, 0), "child inherits dma cap");
    TEST_ASSERT(!allowance_permits(nc, HW_RES_MMIO, 0x4000, 0x1000), "child not broader");
    adrop(nc); adrop(np);

    // Revoked parent -> child born revoked (permits nothing).
    struct Proc *rp = amk(); struct Proc *rc = amk();
    TEST_ASSERT(rp && rc, "proc_alloc revoked pair");
    TEST_ASSERT(proc_confer_allowance(rp, w, 1, irq, 1, 0x800) == 0, "confer revoked parent");
    proc_revoke_allowance(rp);
    TEST_ASSERT(allowance_clone_into(rc, rp) == 0, "clone revoked");
    TEST_ASSERT(rc->allowance != NULL, "revoked child has an allowance struct");
    TEST_ASSERT(!allowance_permits(rc, HW_RES_MMIO, 0x2000, 0x1000),
        "child born revoked permits nothing");
    adrop(rc); adrop(rp);
}

void test_allowance_free_null_tolerant(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(p->allowance == NULL, "broad");
    allowance_free(p);   // NULL allowance -> no-op, no crash
    TEST_ASSERT(p->allowance == NULL, "still NULL");
    // confer then free -> NULLs it.
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, 0, 0x1000) == 0, "confer");
    TEST_ASSERT(p->allowance != NULL, "conferred");
    allowance_free(p);
    TEST_ASSERT(p->allowance == NULL, "freed -> NULL");
    adrop(p);
}

void test_allowance_pci_claim_gate(void) {
    // I-34 audit F1: SYS_PCI_CLAIM is fail-closed for a NARROWED Proc -- the
    // v1.0 allowance has no per-(bus,dev,fn) PCI axis, so a narrowed driver
    // cannot claim PCI at all (closing the bypass where a driver narrowed to
    // one device's MMIO could SYS_PCI_CLAIM another's PCI function). The gate
    // predicate is allowance_is_narrowed: broad -> false (claim allowed),
    // narrowed -> true (claim rejected). The per-device PCI axis lands with
    // the PCIe source (build-arc step 6).
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(!allowance_is_narrowed(p), "broad Proc -> not narrowed (PCI claim allowed)");
    TEST_ASSERT(!allowance_is_narrowed(NULL), "NULL Proc -> not narrowed (defensive)");
    u32 irq[1] = { 40 };
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, irq, 1, 0) == 0, "confer");
    TEST_ASSERT(allowance_is_narrowed(p), "narrowed Proc -> narrowed (PCI claim rejected)");
    // A revoked-but-not-yet-reaped narrowed Proc is STILL narrowed (the pointer
    // is non-NULL until allowance_free at reap) -> still PCI-rejected.
    proc_revoke_allowance(p);
    TEST_ASSERT(allowance_is_narrowed(p), "revoked-narrowed Proc -> still narrowed (still rejected)");
    adrop(p);
}
