// P3-Db: burrow_map(Proc*, ...) / burrow_unmap(Proc*, ...) tests.
//
// Five tests exercising the high-level address-space-installation API:
//
//   burrow.map_proc_smoke
//     burrow_map(p, v, vaddr, length, prot) installs a VMA visible via
//     vma_lookup. mapping_count tracks. vma_drain releases.
//
//   burrow.map_proc_constraints
//     Bad arguments return -1: NULL inputs, zero length, unaligned
//     vaddr, unaligned length, W+X prot. mapping_count unchanged after
//     each rejection.
//
//   burrow.map_proc_overlap_rejected
//     Two non-overlapping ranges accepted; overlap rejected. The
//     rejection MUST roll back the mapping_count++ that vma_alloc took
//     before vma_insert was called (verified via mapping_count delta).
//
//   burrow.unmap_proc_smoke
//     burrow_map followed by burrow_unmap removes the VMA + decrements
//     mapping_count.
//
//   burrow.unmap_proc_no_match
//     burrow_unmap with non-matching range returns -1 without disturbing
//     existing VMAs or mapping_count.

#include "test.h"

#include "../../arch/arm64/mmu.h"     // R12-vaddr: USER_VA_TOP
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

void test_vmo_map_proc_smoke(void);
void test_vmo_map_proc_constraints(void);
void test_vmo_map_proc_overlap_rejected(void);
void test_vmo_map_proc_user_va_top_boundary(void);
void test_vmo_unmap_proc_smoke(void);
void test_vmo_unmap_proc_no_match(void);

// Weft-2 / I-37: cross-Proc Burrow share (burrow_share_into).
void test_burrow_share_into_cross_proc(void);
void test_burrow_share_into_alive_while_either_maps(void);
void test_burrow_share_into_frees_on_last_drop(void);
void test_burrow_share_into_constraints(void);

#define TEST_VA   0x10000000ull           // 256 MiB; well inside user-VA
#define ONE_PAGE  PAGE_SIZE
#define TWO_PAGES (2ull * PAGE_SIZE)

static struct Proc *make_proc(void) {
    struct Proc *p = proc_alloc();
    return p;
}

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;             // PROC_STATE_ZOMBIE
    proc_free(p);
}

void test_vmo_map_proc_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    int rc = burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map should succeed on a clean Proc");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1,
        "burrow_map should increment mapping_count via vma_alloc");

    // VMA visible via vma_lookup at the start, end-1, and middle.
    struct Vma *vma = vma_lookup(p, TEST_VA);
    TEST_ASSERT(vma != NULL, "vma_lookup at start returned NULL");
    TEST_EXPECT_EQ(vma->vaddr_start, TEST_VA,                "vaddr_start");
    TEST_EXPECT_EQ(vma->vaddr_end,   TEST_VA + ONE_PAGE,     "vaddr_end");
    TEST_EXPECT_EQ(vma->prot,        VMA_PROT_RW,            "prot");
    TEST_EXPECT_EQ(vma->burrow,         v,                      "burrow backref");

    // vma_drain handles the cleanup — also exercised by proc_free below.
    vma_drain(p);
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "vma_drain returns mapping_count to baseline");

    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_map_proc_constraints(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    // NULL Proc.
    TEST_EXPECT_EQ(burrow_map(NULL, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), -1,
        "NULL Proc rejected");

    // NULL BURROW.
    TEST_EXPECT_EQ(burrow_map(p, NULL, TEST_VA, ONE_PAGE, VMA_PROT_RW), -1,
        "NULL BURROW rejected");

    // Zero length.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, 0, VMA_PROT_RW), -1,
        "zero length rejected");

    // Unaligned vaddr.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA + 1, ONE_PAGE, VMA_PROT_RW), -1,
        "unaligned vaddr rejected");

    // Unaligned length.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE + 1, VMA_PROT_RW), -1,
        "unaligned length rejected");

    // W+X prot.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE,
                           VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC), -1,
        "W+X prot rejected");

    // R12-vaddr (F180): vaddr at USER_VA_TOP — first non-user page.
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA_TOP, ONE_PAGE, VMA_PROT_RW), -1,
        "vaddr == USER_VA_TOP rejected");

    // R12-vaddr (F180): vaddr clearly past USER_VA_TOP.
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA_TOP + ONE_PAGE, ONE_PAGE,
                              VMA_PROT_RW), -1,
        "vaddr > USER_VA_TOP rejected");

    // R12-vaddr (F180): range straddling USER_VA_TOP (last page lands at top).
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA_TOP - ONE_PAGE, TWO_PAGES,
                              VMA_PROT_RW), -1,
        "range straddling USER_VA_TOP rejected");

    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "rejected map calls must NOT touch mapping_count");
    TEST_ASSERT(p->vmas == NULL, "rejected map calls must NOT install a VMA");

    drop_proc(p);
    burrow_unref(v);
}

// R12-vaddr positive boundary: the highest legal page begins at
// USER_VA_TOP - PAGE_SIZE. burrow_map must accept this exact corner —
// rejecting it would falsely block legitimate top-of-user-VA mappings.
void test_vmo_map_proc_user_va_top_boundary(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    u64 va = USER_VA_TOP - ONE_PAGE;
    TEST_EXPECT_EQ(burrow_map(p, v, va, ONE_PAGE, VMA_PROT_RW), 0,
        "highest-legal user page accepted");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1,
        "accepted map increments mapping_count");

    struct Vma *vma = vma_lookup(p, va);
    TEST_ASSERT(vma != NULL, "vma_lookup at USER_VA_TOP - PAGE_SIZE");
    TEST_EXPECT_EQ(vma->vaddr_end, USER_VA_TOP, "vaddr_end == USER_VA_TOP");

    vma_drain(p);
    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_map_proc_overlap_rejected(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(TWO_PAGES);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    // First map: succeeds.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0,
        "first burrow_map should succeed");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1, "mapping_count = +1");

    // Adjacent (touching at boundary) — half-open ranges, NOT overlap.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA + ONE_PAGE, ONE_PAGE, VMA_PROT_RW), 0,
        "adjacent VMA accepted");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 2, "mapping_count = +2");

    // Exact overlap with first VMA — rejected, mapping_count unchanged.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), -1,
        "exact overlap rejected");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 2,
        "rollback after vma_insert overlap: mapping_count UNCHANGED");

    // Partial overlap — rejected, mapping_count unchanged.
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA - ONE_PAGE, TWO_PAGES, VMA_PROT_RW), -1,
        "partial overlap rejected");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 2,
        "rollback after partial overlap: mapping_count UNCHANGED");

    vma_drain(p);
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "drain restores mapping_count baseline");

    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_unmap_proc_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0,
        "burrow_map");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1, "+1 after map");

    // Exact unmap.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA, ONE_PAGE), 0,
        "burrow_unmap exact match should succeed");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "burrow_unmap returns mapping_count to baseline");
    TEST_ASSERT(vma_lookup(p, TEST_VA) == NULL,
        "VMA gone after burrow_unmap");

    drop_proc(p);
    burrow_unref(v);
}

void test_vmo_unmap_proc_no_match(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);
    TEST_EXPECT_EQ(burrow_map(p, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0, "map");

    // No VMA at this address.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA + ONE_PAGE, ONE_PAGE), -1,
        "unmap miss returns -1");

    // Wrong start within an existing VMA's range.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA + 1, ONE_PAGE), -1,
        "unmap unaligned in existing VMA returns -1");

    // Wrong length on an existing VMA.
    TEST_EXPECT_EQ(burrow_unmap(p, TEST_VA, TWO_PAGES), -1,
        "unmap with mismatched length returns -1");

    // mapping_count untouched throughout.
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1,
        "failed unmap must not touch mapping_count");

    vma_drain(p);
    drop_proc(p);
    burrow_unref(v);
}

// =============================================================================
// Weft-2 / I-37: cross-Proc Burrow share (burrow_share_into).
//
//   burrow.share_into_cross_proc
//     burrow_share_into maps the WHOLE ANON Burrow into a SECOND Proc; both
//     Procs' VMAs reference the IDENTICAL Burrow (hence the identical backing
//     page) -- a byte on the shared page is visible to both mappings.
//     mapping_count tracks both. (The tree's first 2-Proc-reachable Burrow.)
//
//   burrow.share_into_alive_while_either_maps
//     The #847 dual-refcount, cross-Proc: drop the construction HANDLE while
//     two mappings remain -> v stays alive (mappings alone keep it -- the
//     grant-is-the-share guest-holds-only-a-mapping case); v frees only when
//     the LAST mapping drops.
//
//   burrow.share_into_frees_on_last_drop
//     The reverse teardown order: drop both mappings, then the handle last ->
//     v frees on the handle drop. Together with the test above this witnesses
//     order-independence -- free iff ALL refs gone, in any interleaving (the
//     cross-Proc #847 proof, weft.tla ShareBoundedByFlow).
//
//   burrow.share_into_constraints
//     NULL inputs, W+X prot, and a same-VA overlap within one Proc are each
//     rejected; a rejected share never disturbs mapping_count.

void test_burrow_share_into_cross_proc(void) {
    struct Proc *netd  = make_proc();   // owns the per-flow ring (handle + its own map)
    struct Proc *guest = make_proc();   // receives the share (a mapping only)
    TEST_ASSERT(netd != NULL && guest != NULL, "proc_alloc failed");

    struct Burrow *v = burrow_create_anon(ONE_PAGE);   // {h:1, m:0}
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    // netd maps the ring into its own AS: {h:1, m:1}.
    TEST_EXPECT_EQ(burrow_map(netd, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0,
        "netd burrow_map");
    // The guest opening the flow's data fid -> the kernel shares the WHOLE ring
    // into the guest's AS: {h:1, m:2}. Same VA value, distinct address space.
    TEST_EXPECT_EQ(burrow_share_into(guest, v, TEST_VA, VMA_PROT_RW), 0,
        "burrow_share_into guest");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 2,
        "two mappings (netd + guest) on one Burrow");
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "one construction handle");

    // Both Procs' VMAs reference the IDENTICAL Burrow -> the identical backing
    // page. (PTE install is the fault path's job, P3-Dc; the substrate's
    // guarantee is that both user VAs resolve to the SAME Burrow v.)
    struct Vma *va_netd  = vma_lookup(netd,  TEST_VA);
    struct Vma *va_guest = vma_lookup(guest, TEST_VA);
    TEST_ASSERT(va_netd != NULL && va_guest != NULL, "both VMAs present");
    TEST_EXPECT_EQ(va_netd->burrow,  v, "netd VMA backs v");
    TEST_EXPECT_EQ(va_guest->burrow, v, "guest VMA backs v (same Burrow)");

    // A byte written to the shared backing page (as a guest write would land,
    // post-demand-page) is the byte netd reads -- the shared-page semantics.
    u8 *shared = (u8 *)pa_to_kva(page_to_pa(v->pages));
    shared[0]            = 0x5A;
    shared[ONE_PAGE - 1] = 0xA5;
    TEST_EXPECT_EQ(shared[0],            0x5A, "shared page head holds the sentinel");
    TEST_EXPECT_EQ(shared[ONE_PAGE - 1], 0xA5, "shared page tail holds the sentinel");

    // Teardown: drop the guest mapping, the netd mapping, then the handle.
    u64 destroyed_before = burrow_total_destroyed();
    vma_drain(guest);
    TEST_EXPECT_EQ(burrow_mapping_count(v), 1, "guest mapping dropped");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before,
        "v alive while netd still maps");
    vma_drain(netd);
    TEST_EXPECT_EQ(burrow_mapping_count(v), 0, "netd mapping dropped");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before,
        "v alive while the construction handle is held");
    burrow_unref(v);                                   // {h:0, m:0} -> free
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "v freed when the LAST ref drops");

    drop_proc(netd);
    drop_proc(guest);
}

void test_burrow_share_into_alive_while_either_maps(void) {
    struct Proc *netd  = make_proc();
    struct Proc *guest = make_proc();
    TEST_ASSERT(netd != NULL && guest != NULL, "proc_alloc failed");

    struct Burrow *v = burrow_create_anon(ONE_PAGE);   // {h:1, m:0}
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");
    TEST_EXPECT_EQ(burrow_map(netd, v, TEST_VA, ONE_PAGE, VMA_PROT_RW), 0, "netd map");
    TEST_EXPECT_EQ(burrow_share_into(guest, v, TEST_VA, VMA_PROT_RW), 0, "guest share");
    // {h:1, m:2}

    u64 destroyed_before = burrow_total_destroyed();

    // Drop the construction HANDLE first. Grant-is-the-share: the guest holds
    // ONLY a mapping, so a Burrow must survive on mappings alone (h == 0).
    burrow_unref(v);                                   // {h:0, m:2}
    TEST_EXPECT_EQ(burrow_mapping_count(v), 2, "two mappings still hold v");
    TEST_EXPECT_EQ(burrow_handle_count(v),  0, "no handle, but v alive");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before, "v alive on mappings");

    vma_drain(guest);                                  // {h:0, m:1}
    TEST_EXPECT_EQ(burrow_mapping_count(v), 1, "one mapping holds v");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before, "v alive on netd's mapping");

    vma_drain(netd);                                   // {h:0, m:0} -> free
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "v frees when the last mapping drops (handle already gone)");

    drop_proc(netd);
    drop_proc(guest);
}

void test_burrow_share_into_frees_on_last_drop(void) {
    struct Proc *netd  = make_proc();
    struct Proc *guest = make_proc();
    TEST_ASSERT(netd != NULL && guest != NULL, "proc_alloc failed");

    // A 2-page ring: burrow_share_into maps the WHOLE Burrow (v->size), so it
    // also exercises the multi-page whole-ring share path.
    struct Burrow *v = burrow_create_anon(TWO_PAGES);  // {h:1, m:0}
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");
    TEST_EXPECT_EQ(burrow_map(netd, v, TEST_VA, TWO_PAGES, VMA_PROT_RW), 0, "netd map");
    TEST_EXPECT_EQ(burrow_share_into(guest, v, TEST_VA, VMA_PROT_RW), 0, "guest whole-ring share");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 2, "two mappings");

    u64 destroyed_before = burrow_total_destroyed();

    vma_drain(guest);                                  // {h:1, m:1}
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before, "alive: netd map + handle");
    vma_drain(netd);                                   // {h:1, m:0}
    TEST_EXPECT_EQ(burrow_mapping_count(v), 0, "no mappings");
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before,
        "alive: the construction handle alone keeps v (handle-only liveness)");
    burrow_unref(v);                                   // {h:0, m:0} -> free
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "v frees on the handle drop (the other teardown order)");

    drop_proc(netd);
    drop_proc(guest);
}

void test_burrow_share_into_constraints(void) {
    struct Proc *guest = make_proc();
    TEST_ASSERT(guest != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int mapping_before = burrow_mapping_count(v);

    // NULL dst.
    TEST_EXPECT_EQ(burrow_share_into(NULL, v, TEST_VA, VMA_PROT_RW), -1,
        "NULL dst rejected");
    // NULL Burrow.
    TEST_EXPECT_EQ(burrow_share_into(guest, NULL, TEST_VA, VMA_PROT_RW), -1,
        "NULL Burrow rejected");
    // W+X prot (I-12; delegated to vma_alloc).
    TEST_EXPECT_EQ(burrow_share_into(guest, v, TEST_VA,
                       VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC), -1,
        "W+X prot rejected");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before,
        "rejected shares must NOT touch mapping_count");
    TEST_ASSERT(guest->vmas == NULL, "rejected shares must NOT install a VMA");

    // A second share at the SAME VA within one Proc -> overlap rejected, the
    // mapping_count++ that vma_alloc took rolled back.
    TEST_EXPECT_EQ(burrow_share_into(guest, v, TEST_VA, VMA_PROT_RW), 0, "first share");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1, "+1 after first share");
    TEST_EXPECT_EQ(burrow_share_into(guest, v, TEST_VA, VMA_PROT_RW), -1,
        "same-VA overlap within one Proc rejected");
    TEST_EXPECT_EQ(burrow_mapping_count(v), mapping_before + 1,
        "overlap rollback: mapping_count UNCHANGED");

    vma_drain(guest);
    drop_proc(guest);
    burrow_unref(v);
}
