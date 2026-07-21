// Hardware allowance tests (ARCH section 28 I-34; specs/allowance.tla;
// docs/MENAGERIE.md section 4). Covers the CreateBegin gate
// (allowance_permits: MMIO containment / IRQ membership / DMA cap / PCI bdf
// membership / broad / revoked), the CreateCommit re-check
// (allowance_handle_alloc), Confer (over-cap reject), Revoke, the rfork
// clone-inherit, and the free.
//
// The headline runtime regression for the spec's BUGGY_COMMIT_NO_RECHECK
// counterexample (the revoke-vs-create SMP race) is
// allowance.handle_alloc_revoked_aborts -- the in-flight create's commit
// aborts because the allowance was revoked between the gate check and the
// install. Maps to specs/allowance.tla::HandlesWithinAllowance. (Kind-agnostic:
// the same re-check guards a KObj_PCI commit, the build-arc step-6 PCI axis.)

#include "test.h"

#include <thylacine/allowance.h>
#include <thylacine/handle.h>
#include <thylacine/pci_handle.h>   // kobj_pci_resolve_bdf (the PCI gate leg)
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/virtio.h>       // VIRTIO_DEVICE_ID_RNG

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
void test_allowance_pci_membership(void);
void test_allowance_pci_claim_handler_gate(void);
void test_allowance_confer_within_parent(void);

// The SYS_PCI_CLAIM handler is not header-declared (a syscall handler); the
// F2 regression drives it directly (the established test-TU extern pattern).
extern s64 sys_pci_claim_handler(u64 virtio_device_id, u64 a1);
void test_allowance_revoke_on_group_terminate(void);
void test_allowance_narrowed_proc_cannot_spawn(void);

// Mirror the test_handle.c proc make/drop: proc_alloc gives a fresh Proc with
// an empty handle table + a NULL (broad) allowance; test_proc_drop ZOMBIEs +
// proc_free's it (which allowance_free's any conferred allowance).
static struct Proc *amk(void) { return proc_alloc(); }
static void adrop(struct Proc *p) { if (!p) return; p->state = PROC_STATE_ZOMBIE; proc_free(p); }

void test_allowance_null_is_broad(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(p->allowance == NULL, "fresh Proc is broad (allowance NULL)");
    // Broad -> permits any MMIO/IRQ/DMA/PCI (the as-built v1.0 path).
    TEST_ASSERT(allowance_permits(p, HW_RES_MMIO, 0x0a000000, 0x1000), "broad MMIO");
    TEST_ASSERT(allowance_permits(p, HW_RES_IRQ, 48, 0), "broad IRQ");
    TEST_ASSERT(allowance_permits(p, HW_RES_DMA, 0x100000, 0), "broad DMA");
    TEST_ASSERT(allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(0, 5, 0), 0), "broad PCI");
    adrop(p);
}

void test_allowance_mmio_containment(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct hw_window w[1] = { { .base = 0x1000, .size = 0x1000 } };  // [0x1000, 0x2000)
    TEST_ASSERT(proc_confer_allowance(p, w, 1, NULL, 0, 0, NULL, 0) == 0, "confer mmio");
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
    TEST_ASSERT(!allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(0, 5, 0), 0), "no pci conferred");
    adrop(p);
}

void test_allowance_irq_membership(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 irq[2] = { 40, 50 };
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, irq, 2, 0, NULL, 0) == 0, "confer irq");
    TEST_ASSERT(allowance_permits(p, HW_RES_IRQ, 40, 0), "irq 40 conferred");
    TEST_ASSERT(allowance_permits(p, HW_RES_IRQ, 50, 0), "irq 50 conferred");
    TEST_ASSERT(!allowance_permits(p, HW_RES_IRQ, 41, 0), "irq 41 not conferred");
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "no mmio conferred");
    adrop(p);
}

void test_allowance_dma_cap(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, 0, 0x1000, NULL, 0) == 0, "confer dma cap");
    TEST_ASSERT(allowance_permits(p, HW_RES_DMA, 0x1000, 0), "dma at cap");
    TEST_ASSERT(allowance_permits(p, HW_RES_DMA, 0x800, 0), "dma under cap");
    TEST_ASSERT(!allowance_permits(p, HW_RES_DMA, 0x1001, 0), "dma over cap");
    TEST_ASSERT(!allowance_permits(p, HW_RES_DMA, 0, 0), "dma zero denied");
    adrop(p);

    // dma_max == 0 -> NO dma permitted.
    struct Proc *q = amk();
    TEST_ASSERT(q != NULL, "proc_alloc q");
    TEST_ASSERT(proc_confer_allowance(q, NULL, 0, NULL, 0, 0, NULL, 0) == 0, "confer no-dma");
    TEST_ASSERT(!allowance_permits(q, HW_RES_DMA, 0x100, 0), "dma denied when cap 0");
    adrop(q);
}

void test_allowance_revoked_permits_nothing(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct hw_window w[1] = { { .base = 0x1000, .size = 0x1000 } };
    u32 irq[1] = { 40 };
    u32 pci[1] = { PCI_BDF_PACK(0, 5, 0) };
    TEST_ASSERT(proc_confer_allowance(p, w, 1, irq, 1, 0x1000, pci, 1) == 0, "confer");
    TEST_ASSERT(allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "permits pre-revoke");
    proc_revoke_allowance(p);
    // The spec's Revoke: allowance[d] = {} -> permits nothing.
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "mmio denied post-revoke");
    TEST_ASSERT(!allowance_permits(p, HW_RES_IRQ, 40, 0), "irq denied post-revoke");
    TEST_ASSERT(!allowance_permits(p, HW_RES_DMA, 0x100, 0), "dma denied post-revoke");
    TEST_ASSERT(!allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(0, 5, 0), 0), "pci denied post-revoke");
    adrop(p);
}

void test_allowance_confer_rejects_overcap(void) {
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    // count > cap is checked before the NULL-pointer guard, so NULL is fine.
    TEST_ASSERT(proc_confer_allowance(p, NULL, ALLOWANCE_MMIO_MAX + 1, NULL, 0, 0, NULL, 0) == -1,
        "mmio_count over cap rejected");
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, ALLOWANCE_IRQ_MAX + 1, 0, NULL, 0) == -1,
        "irq_count over cap rejected");
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, 0, 0, NULL, ALLOWANCE_PCI_MAX + 1) == -1,
        "pci_count over cap rejected");
    // A positive count with a NULL pointer is rejected.
    TEST_ASSERT(proc_confer_allowance(p, NULL, 1, NULL, 0, 0, NULL, 0) == -1, "mmio NULL+count rejected");
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, 0, 0, NULL, 1) == -1, "pci NULL+count rejected");
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
    // Kind-agnostic: the same re-check guards the step-6 KObj_PCI commit.
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 irq[1] = { 40 };
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, irq, 1, 0, NULL, 0) == 0, "confer");
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
    u32 pci[1] = { PCI_BDF_PACK(0, 6, 0) };
    TEST_ASSERT(proc_confer_allowance(np, w, 1, irq, 1, 0x800, pci, 1) == 0, "confer parent");
    TEST_ASSERT(allowance_clone_into(nc, np) == 0, "clone narrowed");
    TEST_ASSERT(nc->allowance != NULL, "child narrowed");
    TEST_ASSERT(nc->allowance != np->allowance, "child has its OWN allowance (deep copy)");
    TEST_ASSERT(allowance_permits(nc, HW_RES_MMIO, 0x2000, 0x1000), "child inherits mmio");
    TEST_ASSERT(allowance_permits(nc, HW_RES_IRQ, 42, 0), "child inherits irq");
    TEST_ASSERT(allowance_permits(nc, HW_RES_DMA, 0x800, 0), "child inherits dma cap");
    TEST_ASSERT(allowance_permits(nc, HW_RES_PCI, PCI_BDF_PACK(0, 6, 0), 0), "child inherits pci bdf");
    TEST_ASSERT(!allowance_permits(nc, HW_RES_MMIO, 0x4000, 0x1000), "child not broader");
    TEST_ASSERT(!allowance_permits(nc, HW_RES_PCI, PCI_BDF_PACK(0, 7, 0), 0), "child no other pci");
    adrop(nc); adrop(np);

    // Revoked parent -> child born revoked (permits nothing).
    struct Proc *rp = amk(); struct Proc *rc = amk();
    TEST_ASSERT(rp && rc, "proc_alloc revoked pair");
    TEST_ASSERT(proc_confer_allowance(rp, w, 1, irq, 1, 0x800, NULL, 0) == 0, "confer revoked parent");
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
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, 0, 0x1000, NULL, 0) == 0, "confer");
    TEST_ASSERT(p->allowance != NULL, "conferred");
    allowance_free(p);
    TEST_ASSERT(p->allowance == NULL, "freed -> NULL");
    adrop(p);
}

void test_allowance_pci_membership(void) {
    // Build-arc step 6 (#159): the per-(bus,dev,fn) PCI axis replaces the v1.0
    // fail-closed reject. A NARROWED driver may SYS_PCI_CLAIM a (bus,dev,fn) IN
    // its allowance and is denied one OUTSIDE it ("a PCI device's allowance IS
    // its claimed BARs", MENAGERIE.md section 4). This tests the gate PREDICATE
    // (allowance_permits HW_RES_PCI) the SYS_PCI_CLAIM handler checks, + the
    // resolve leg (kobj_pci_resolve_bdf) it feeds.
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 bdf = PCI_BDF_PACK(0, 5, 0);
    u32 pci[1] = { bdf };
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, NULL, 0, 0, pci, 1) == 0, "confer pci bdf");
    TEST_ASSERT(allowance_permits(p, HW_RES_PCI, bdf, 0), "permits the conferred bdf");
    TEST_ASSERT(!allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(0, 6, 0), 0), "denies a different bdf");
    TEST_ASSERT(!allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(1, 5, 0), 0), "bus distinguishes");
    TEST_ASSERT(!allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(0, 5, 1), 0), "fn distinguishes");
    TEST_ASSERT(!allowance_permits(p, HW_RES_MMIO, 0x1000, 0x1000), "no mmio conferred");
    // A revoked narrowed allowance permits no PCI claim.
    proc_revoke_allowance(p);
    TEST_ASSERT(!allowance_permits(p, HW_RES_PCI, bdf, 0), "revoked permits no pci");
    adrop(p);

    // The pack is injective over (u8,u8,u8): no two distinct triples collide.
    TEST_ASSERT(PCI_BDF_PACK(0, 5, 0) != PCI_BDF_PACK(0, 0, 5), "dev<<8 != fn distinct");
    TEST_ASSERT(PCI_BDF_PACK(1, 0, 0) != PCI_BDF_PACK(0, 1, 0), "bus<<16 != dev<<8 distinct");
    TEST_ASSERT(PCI_BDF_PACK(0xFF, 0x1F, 0x07) == ((0xFFu << 16) | (0x1Fu << 8) | 0x07u),
        "full-range pack matches the bit layout");

    // The resolve leg, IF the live rng-pci is present (some configs have no
    // PCIe root). Confer the rng's OWN bdf -> permits; confer a fabricated bdf
    // -> denies the rng's. The SYS_PCI_CLAIM handler resolves the same bdf
    // (kobj_pci_resolve_bdf == the first match kobj_pci_claim picks).
    u8 rb, rd, rf;
    if (kobj_pci_resolve_bdf(VIRTIO_DEVICE_ID_RNG, 0, &rb, &rd, &rf) == 0) {
        u32 rng_bdf = PCI_BDF_PACK(rb, rd, rf);
        struct Proc *q = amk();
        TEST_ASSERT(q != NULL, "proc_alloc q");
        u32 qpci[1] = { rng_bdf };
        TEST_ASSERT(proc_confer_allowance(q, NULL, 0, NULL, 0, 0, qpci, 1) == 0, "confer rng bdf");
        TEST_ASSERT(allowance_permits(q, HW_RES_PCI, rng_bdf, 0),
            "narrowed-to-rng permits the rng claim (SYS_PCI_CLAIM would pass)");
        adrop(q);

        // A driver narrowed to a DIFFERENT function is denied the rng claim --
        // the bypass the v1.0 fail-closed reject crudely covered, now precise.
        struct Proc *r = amk();
        TEST_ASSERT(r != NULL, "proc_alloc r");
        u32 other = PCI_BDF_PACK((u8)(rb + 1), rd, rf);   // a neighbouring bus
        u32 rpci[1] = { other };
        TEST_ASSERT(proc_confer_allowance(r, NULL, 0, NULL, 0, 0, rpci, 1) == 0, "confer other bdf");
        TEST_ASSERT(!allowance_permits(r, HW_RES_PCI, rng_bdf, 0),
            "narrowed-to-other denied the rng claim (SYS_PCI_CLAIM would reject)");
        adrop(r);
    }
}

void test_allowance_pci_claim_handler_gate(void) {
    // 6a audit F2: drive the LIVE sys_pci_claim_handler end-to-end for a NARROWED
    // Proc -- the wiring (resolve -> allowance_permits(HW_RES_PCI) -> claim/deny)
    // the pci_membership unit test (predicate + resolve in isolation) leaves
    // uncovered. A refactor that dropped the gate call at syscall.c would pass
    // the predicate test but FAIL here. Guarded on the live rng-pci (no PCIe root
    // -> SKIP). Drives it on kproc (CAP_ALL, so the CAP_HW_CREATE gate passes),
    // temporarily narrowed + restored to broad before return (the
    // narrowed_proc_cannot_spawn discipline; single-threaded test window).
    u8 rb, rd, rf;
    if (kobj_pci_resolve_bdf(VIRTIO_DEVICE_ID_RNG, 0, &rb, &rd, &rf) != 0) return;  // SKIP

    struct Proc *me = kproc();
    TEST_ASSERT(me != NULL && me->allowance == NULL, "kproc broad");
    u64 before = kobj_pci_live_count();

    // DENIED: narrow kproc to a DIFFERENT function of the same device (fn^1,
    // guaranteed distinct + no u8 overflow) -> the handler rejects at the gate
    // BEFORE claiming; no claim leaks (the cross-function leak the axis forbids).
    u32 other[1] = { PCI_BDF_PACK(rb, rd, (u8)(rf ^ 1)) };
    TEST_ASSERT(proc_confer_allowance(me, NULL, 0, NULL, 0, 0, other, 1) == 0, "narrow to other fn");
    TEST_ASSERT(sys_pci_claim_handler(VIRTIO_DEVICE_ID_RNG, 0) < 0,
        "narrowed-to-other DENIED at the SYS_PCI_CLAIM gate");
    TEST_ASSERT(kobj_pci_live_count() == before, "no claim leaked through the denied gate");
    proc_revoke_allowance(me);
    allowance_free(me);

    // ALLOWED: narrow kproc to the rng's OWN bdf -> the handler claims; the live
    // count rises by one, then drops back when the handle is closed.
    u32 own[1] = { PCI_BDF_PACK(rb, rd, rf) };
    TEST_ASSERT(proc_confer_allowance(me, NULL, 0, NULL, 0, 0, own, 1) == 0, "narrow to rng bdf");
    s64 h = sys_pci_claim_handler(VIRTIO_DEVICE_ID_RNG, 0);
    TEST_ASSERT(h >= 0, "narrowed-to-rng ALLOWED at the SYS_PCI_CLAIM gate");
    TEST_ASSERT(kobj_pci_live_count() == before + 1, "the permitted claim landed");
    handle_close(me, (hidx_t)h);
    TEST_ASSERT(kobj_pci_live_count() == before, "claim released on handle close");

    // Restore kproc to broad for the live system + subsequent tests.
    proc_revoke_allowance(me);
    allowance_free(me);
    TEST_ASSERT(me->allowance == NULL, "kproc restored broad");
}

void test_allowance_confer_within_parent(void) {
    // Menagerie step 5: the confer gate (I-2's hardware-axis analog). A confer
    // must be a NARROWING of the spawning Proc's OWN allowance: a broad parent
    // (the warden) may confer anything; a narrowed parent only a subset of its
    // own (a bus driver spawning a sub-driver). Maps to specs/allowance.tla's
    // monotonic-reduction (the property allowance_clone_into enforces for rfork).

    // A BROAD parent confers anything.
    struct Proc *bp = amk();
    TEST_ASSERT(bp && bp->allowance == NULL, "broad parent");
    struct hw_window any[1] = { { .base = 0x9999000, .size = 0x1000 } };
    u32 anyirq[1] = { 200 };
    u32 anypci[1] = { PCI_BDF_PACK(3, 1, 0) };
    TEST_ASSERT(allowance_confer_within_parent(bp, any, 1, anyirq, 1, 0x100000, anypci, 1),
        "broad parent confers any mmio/irq/dma/pci");
    TEST_ASSERT(allowance_confer_within_parent(bp, NULL, 0, NULL, 0, 0, NULL, 0),
        "broad parent confers the empty allowance");
    adrop(bp);

    // A NARROWED parent: [0x1000,0x3000) MMIO, irq {40,41}, dma <= 0x1000,
    // pci {(0,5,0)}.
    struct Proc *np = amk();
    TEST_ASSERT(np != NULL, "proc_alloc");
    struct hw_window pw[1] = { { .base = 0x1000, .size = 0x2000 } };
    u32 pirq[2] = { 40, 41 };
    u32 ppci[1] = { PCI_BDF_PACK(0, 5, 0) };
    TEST_ASSERT(proc_confer_allowance(np, pw, 1, pirq, 2, 0x1000, ppci, 1) == 0, "confer parent");

    // Subset / exact / empty -> conferrable.
    struct hw_window sub[1] = { { .base = 0x1400, .size = 0x400 } };
    u32 subirq[1] = { 40 };
    TEST_ASSERT(allowance_confer_within_parent(np, sub, 1, subirq, 1, 0x800, NULL, 0),
        "strict subset -> ok");
    TEST_ASSERT(allowance_confer_within_parent(np, pw, 1, pirq, 2, 0x1000, ppci, 1),
        "exact parent set -> ok");
    TEST_ASSERT(allowance_confer_within_parent(np, NULL, 0, NULL, 0, 0, NULL, 0),
        "empty allowance -> ok");

    // Wider on ANY axis -> rejected (the monotonic-reduction violation).
    struct hw_window wide_mmio[1] = { { .base = 0x1000, .size = 0x4000 } };
    TEST_ASSERT(!allowance_confer_within_parent(np, wide_mmio, 1, NULL, 0, 0, NULL, 0),
        "wider mmio window -> reject");
    struct hw_window out_mmio[1] = { { .base = 0x9000, .size = 0x100 } };
    TEST_ASSERT(!allowance_confer_within_parent(np, out_mmio, 1, NULL, 0, 0, NULL, 0),
        "mmio window outside parent -> reject");
    u32 wide_irq[1] = { 99 };
    TEST_ASSERT(!allowance_confer_within_parent(np, NULL, 0, wide_irq, 1, 0, NULL, 0),
        "irq not in parent -> reject");
    TEST_ASSERT(!allowance_confer_within_parent(np, NULL, 0, NULL, 0, 0x2000, NULL, 0),
        "dma over parent cap -> reject");
    u32 other_pci[1] = { PCI_BDF_PACK(0, 6, 0) };
    TEST_ASSERT(!allowance_confer_within_parent(np, NULL, 0, NULL, 0, 0, other_pci, 1),
        "pci bdf not in parent -> reject");

    // A size-0 conferred window confers nothing on that axis -> skipped -> ok.
    struct hw_window zero_w[1] = { { .base = 0x99999, .size = 0 } };
    TEST_ASSERT(allowance_confer_within_parent(np, zero_w, 1, NULL, 0, 0, NULL, 0),
        "size-0 window confers nothing -> within");

    // A REVOKED parent confers nothing (allowance_permits is false post-revoke).
    proc_revoke_allowance(np);
    TEST_ASSERT(!allowance_confer_within_parent(np, sub, 1, subirq, 1, 0, NULL, 0),
        "revoked parent confers nothing");
    TEST_ASSERT(!allowance_confer_within_parent(np, NULL, 0, NULL, 0, 0, ppci, 1),
        "revoked parent confers no pci");
    adrop(np);
}

void test_allowance_revoke_on_group_terminate(void) {
    // Menagerie step 5 / #160: proc_group_terminate revokes the Proc's
    // allowance as its first step, so the warden's killgrp of a removed driver
    // is revoke-then-terminate atomically -- an in-flight SYS_*_CREATE racing
    // the removal aborts at its CreateCommit re-check rather than slipping a
    // handle onto a gone device (allowance.tla revoke_race, universalized).
    struct Proc *p = amk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 irq[1] = { 40 };
    TEST_ASSERT(proc_confer_allowance(p, NULL, 0, irq, 1, 0, NULL, 0) == 0, "confer");
    TEST_ASSERT(allowance_permits(p, HW_RES_IRQ, 40, 0), "permits pre-terminate");
    // The fold-in: group_terminate revokes the allowance (struct survives until
    // reap; only `revoked` flips). audit F4: production's proc_group_terminate
    // runs UNDER g_proc_table_lock, but that lock is file-static in proc.c
    // (untakeable from a test TU); the bare call is sound HERE because this test
    // Proc has no threads (the peer-walk + wake + IPI machinery no-op) and the
    // single-threaded test context has no concurrent killer or confer -- so the
    // lock guards nothing reachable. The locked path is exercised in production
    // (the warden's killgrp) + by the SMP gate.
    proc_group_terminate(p, "test-devremoved");
    TEST_ASSERT(p->allowance != NULL, "allowance struct survives terminate (freed at reap)");
    TEST_ASSERT(!allowance_permits(p, HW_RES_IRQ, 40, 0), "revoked by group_terminate");
    hidx_t h = allowance_handle_alloc(p, KOBJ_THREAD, RIGHT_READ, NULL);
    TEST_ASSERT(h < 0, "in-flight create aborts after the group_terminate revoke");
    adrop(p);

    // A broad (NULL allowance) Proc terminate is a safe no-op for the revoke.
    struct Proc *b = amk();
    TEST_ASSERT(b && b->allowance == NULL, "broad proc");
    proc_group_terminate(b, "test-broad");
    TEST_ASSERT(b->allowance == NULL, "broad terminate: no allowance, no crash");
    adrop(b);
}

// F2 (5e-4 audit) thunk: a child that exits cleanly. Used only by the control
// (broad-parent) spawns -- the narrowed-parent spawn is denied BEFORE any child
// is created, so this never runs on the denied path.
static void aspawn_thunk(void *arg) { (void)arg; exits("ok"); }

void test_allowance_narrowed_proc_cannot_spawn(void) {
    // F2 / MENAGERIE section 13.2 + section 5: "bus drivers are sources, not
    // spawners; one auditable chokepoint." rfork_internal denies a child Proc to
    // a hardware-allowance-NARROWED parent. Without the gate, the child inherits
    // a CLONE of the narrowed allowance (or is conferred a subset) as an
    // INDEPENDENT allowance (revoked == 0) that survives the parent's
    // DeviceRemoved (per-Proc revoke + thread-group-scoped terminate -> the child
    // reparents to init), leaving a surviving hw-capable Proc the warden never
    // tracks. The gate keys on the ALLOWANCE, not the identity: a SYSTEM-identity
    // driver is still a sandboxed leaf, so there is NO SYSTEM exemption (unlike
    // the #65 resource caps).
    //
    // Driven through a REAL rfork from the current (kproc) Proc, temporarily
    // narrowed: with the gate, rfork returns -1 BEFORE proc_alloc (no child, no
    // side effect); pre-fix, a real child would spawn (pid > 0) -- the
    // regression. kproc is SYSTEM, so the #65 child cap is exempt and never masks
    // the allowance gate; kproc is restored to broad before return.
    struct Proc *me = kproc();
    TEST_ASSERT(me != NULL, "kproc");
    TEST_ASSERT(me->allowance == NULL, "kproc starts broad (allowance NULL)");

    // Control: a BROAD parent spawns fine -- the gate must not break normal
    // spawning (the entire non-driver process tree is broad).
    int cpid = rfork(RFPROC, aspawn_thunk, NULL);
    TEST_ASSERT(cpid > 0, "broad parent spawns a child");
    int cst = -1;
    TEST_EXPECT_EQ(wait_pid_for(cpid, 0, &cst), cpid, "reap the control child");

    // Narrow self, then attempt to spawn -> denied (the F2 gate).
    u32 irq[1] = { 40 };
    TEST_ASSERT(proc_confer_allowance(me, NULL, 0, irq, 1, 0, NULL, 0) == 0, "narrow self");
    TEST_ASSERT(allowance_is_narrowed(me), "self is now narrowed");
    int npid = rfork(RFPROC, aspawn_thunk, NULL);
    TEST_EXPECT_EQ(npid, -1, "narrowed parent denied a child (MENAGERIE 13.2)");

    // Restore broad so the live kproc + subsequent tests are unaffected.
    proc_revoke_allowance(me);
    allowance_free(me);
    TEST_ASSERT(me->allowance == NULL, "kproc restored to broad");

    // Broad-spawn works again after the restore.
    int rpid = rfork(RFPROC, aspawn_thunk, NULL);
    TEST_ASSERT(rpid > 0, "broad spawn works again after restore");
    int rst = -1;
    TEST_EXPECT_EQ(wait_pid_for(rpid, 0, &rst), rpid, "reap the post-restore child");
}
