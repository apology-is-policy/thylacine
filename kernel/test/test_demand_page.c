// P3-Dc: demand-paging tests.
//
// Six tests exercise mmu_install_user_pte + userland_demand_page:
//
//   pgtable.install_user_pte_smoke
//     mmu_install_user_pte installs a leaf L3 PTE; sub-tables are
//     allocated KP_ZERO; the PTE bits encode the requested prot
//     (AP / UXN / PXN / nG / SH_INNER / AF). Verifies all 4
//     translation-table levels by walking the tree manually.
//
//   pgtable.install_user_pte_constraints
//     Invalid args (zero pgtable_root, unaligned vaddr/pa, out-of-range
//     vaddr/pa, W+X prot) all return -1.
//
//   pgtable.install_user_pte_idempotent
//     Calling twice with identical args returns 0 both times. Repeated
//     install doesn't double-allocate sub-tables. Mismatching install
//     (different PA at same vaddr) returns -1.
//
//   demand_page.smoke
//     Build a synthetic fault_info (from_user=true; vaddr inside a
//     mapped VMA); call userland_demand_page; verify FAULT_HANDLED +
//     L3 PTE installed at the expected index pointing at the BURROW's
//     backing page.
//
//   demand_page.no_vma
//     Fault on a vaddr with no VMA → FAULT_UNHANDLED_USER.
//
//   demand_page.permission_denied
//     Write fault on a VMA_PROT_READ-only VMA → FAULT_UNHANDLED_USER.
//     Instruction fault on a VMA without VMA_PROT_EXEC →
//     FAULT_UNHANDLED_USER.
//
//   demand_page.lifecycle_round_trip
//     Demand-page several pages of a VMA; proc_free walker frees ALL
//     installed sub-tables (L1 + L2 + L3); free count returns to
//     baseline. Closes the lifecycle gap that #134 (P3-Db) wired in
//     advance.

#include "test.h"

#include "../../arch/arm64/fault.h"
#include "../../arch/arm64/mmu.h"
#include "../../mm/phys.h"

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>

void test_pgtable_install_user_pte_smoke(void);
void test_pgtable_install_user_pte_constraints(void);
void test_pgtable_install_user_pte_idempotent(void);
void test_demand_page_smoke(void);
void test_demand_page_no_vma(void);
void test_demand_page_permission_denied(void);
void test_demand_page_lifecycle_round_trip(void);

#define USER_VA   0x10000000ull           // 256 MiB; well within user-half
#define ONE_PAGE  PAGE_SIZE
#define FOUR_PAGES (4ull * PAGE_SIZE)

// ---- Local PTE-bit duplicates for table walk verification ----
// We duplicate just the bits the tests need so the test file stays
// self-contained against future PTE-bit-layout shuffles.
#define BIT_VALID       (1ull << 0)
#define BIT_TYPE_TABLE  (1ull << 1)
#define BIT_TYPE_PAGE   (1ull << 1)
#define BIT_AF          (1ull << 10)
#define BIT_NG          (1ull << 11)
#define BIT_PXN         (1ull << 53)
#define BIT_UXN         (1ull << 54)
// AP[2:1] is the 2-bit access-permissions field at PTE bits 7:6.
//   0b00 = RW_EL1   (kernel-only RW; not used for user PTEs)
//   0b01 = RW_ANY   (user-RW)
//   0b10 = RO_EL1   (kernel-only RO)
//   0b11 = RO_ANY   (user-RO)
// To test which encoding is set, mask AP_FIELD then compare.
#define BIT_AP_FIELD    (3ull << 6)
#define BIT_AP_RW_ANY   (1ull << 6)        // 0b01
#define BIT_AP_RO_ANY   (3ull << 6)        // 0b11

#define ENTRIES_PER_TBL 512
#define BLOCK_SHIFT_L1  30
#define BLOCK_SHIFT_L2  21

static struct Proc *make_proc(void) {
    struct Proc *p = proc_alloc();
    return p;
}

static void drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = 2;     // PROC_STATE_ZOMBIE
    proc_free(p);
}

// Walk to the L3 entry covering `vaddr`. Returns NULL if any level is
// invalid or non-table.
static u64 walk_to_l3_entry(paddr_t pgtable_root, u64 vaddr) {
    u32 idx0 = (u32)((vaddr >> 39) & 0x1ff);
    u32 idx1 = (u32)((vaddr >> BLOCK_SHIFT_L1) & 0x1ff);
    u32 idx2 = (u32)((vaddr >> BLOCK_SHIFT_L2) & 0x1ff);
    u32 idx3 = (u32)((vaddr >> 12) & 0x1ff);

    u64 *l0 = (u64 *)pa_to_kva(pgtable_root);
    u64 e0 = l0[idx0];
    if (!(e0 & BIT_VALID) || !(e0 & BIT_TYPE_TABLE)) return 0;

    paddr_t l1_pa = e0 & ~0xFFFull;
    u64 *l1 = (u64 *)pa_to_kva(l1_pa);
    u64 e1 = l1[idx1];
    if (!(e1 & BIT_VALID) || !(e1 & BIT_TYPE_TABLE)) return 0;

    paddr_t l2_pa = e1 & ~0xFFFull;
    u64 *l2 = (u64 *)pa_to_kva(l2_pa);
    u64 e2 = l2[idx2];
    if (!(e2 & BIT_VALID) || !(e2 & BIT_TYPE_TABLE)) return 0;

    paddr_t l3_pa = e2 & ~0xFFFull;
    u64 *l3 = (u64 *)pa_to_kva(l3_pa);
    return l3[idx3];
}

void test_pgtable_install_user_pte_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // Allocate a backing page from buddy directly (the PA we'll install
    // into the leaf PTE).
    struct page *backing_pg = alloc_pages(0, KP_ZERO);
    TEST_ASSERT(backing_pg != NULL, "alloc_pages backing page failed");
    paddr_t backing_pa = page_to_pa(backing_pg);

    // Install RW + R (no exec).
    int rc = mmu_install_user_pte(p->pgtable_root, p->asid,
                                  USER_VA, backing_pa, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "install RW PTE should succeed");

    u64 pte = walk_to_l3_entry(p->pgtable_root, USER_VA);
    TEST_ASSERT(pte != 0, "walk to L3 entry returned 0 (sub-tables not installed)");
    TEST_ASSERT((pte & BIT_VALID) != 0,    "L3 entry valid");
    TEST_ASSERT((pte & BIT_TYPE_PAGE) != 0, "L3 entry is page descriptor");
    TEST_ASSERT((pte & BIT_AF) != 0,        "AF set");
    TEST_ASSERT((pte & BIT_NG) != 0,        "nG set (ASID-tagged)");
    TEST_ASSERT((pte & BIT_PXN) != 0,       "PXN set (kernel never execs user)");
    TEST_ASSERT((pte & BIT_UXN) != 0,       "UXN set for non-EXEC mapping");
    TEST_EXPECT_EQ(pte & BIT_AP_FIELD, BIT_AP_RW_ANY,
        "AP[2:1] = 0b01 (AP_RW_ANY) for VMA_PROT_RW");
    // PTE PA = bits 47:12 (page frame). Leaf PTEs carry attribute bits at
    // 53/54 (PXN/UXN); mask them off explicitly.
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, backing_pa,
        "PTE PA matches the backing page (mask: 47:12 page-frame bits)");

    // Install RX (read-only + exec) at a different vaddr.
    paddr_t code_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(code_pa != 0, "alloc_pages code backing failed");
    rc = mmu_install_user_pte(p->pgtable_root, p->asid,
                              USER_VA + ONE_PAGE, code_pa, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "install RX PTE should succeed");

    u64 pte_rx = walk_to_l3_entry(p->pgtable_root, USER_VA + ONE_PAGE);
    TEST_ASSERT(pte_rx != 0, "walk to L3 for RX returned 0");
    TEST_ASSERT((pte_rx & BIT_PXN) != 0,    "RX still PXN (kernel never execs)");
    TEST_ASSERT((pte_rx & BIT_UXN) == 0,    "RX clears UXN (user can exec)");
    TEST_EXPECT_EQ(pte_rx & BIT_AP_FIELD, BIT_AP_RO_ANY,
        "AP[2:1] = 0b11 (AP_RO_ANY) for VMA_PROT_RX");

    drop_proc(p);
}

void test_pgtable_install_user_pte_constraints(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    paddr_t backing_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(backing_pa != 0, "backing alloc");

    // Zero pgtable_root.
    TEST_EXPECT_EQ(mmu_install_user_pte(0, 0, USER_VA, backing_pa, VMA_PROT_RW), -1,
        "pgtable_root=0 rejected");

    // Unaligned vaddr.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, p->asid,
                                        USER_VA + 1, backing_pa, VMA_PROT_RW), -1,
        "unaligned vaddr rejected");

    // Unaligned pa.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, p->asid,
                                        USER_VA, backing_pa + 1, VMA_PROT_RW), -1,
        "unaligned pa rejected");

    // vaddr in TTBR1 high half — installer rejects (top bits set).
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, p->asid,
                                        0xFFFF000000000000ull, backing_pa, VMA_PROT_RW), -1,
        "high-VA vaddr rejected");

    // W+X prot.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, p->asid,
                                        USER_VA, backing_pa,
                                        VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC), -1,
        "W+X rejected at PTE installer (defense-in-depth)");

    drop_proc(p);
}

void test_pgtable_install_user_pte_idempotent(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    paddr_t backing_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(backing_pa != 0, "backing alloc");

    // First install.
    int rc = mmu_install_user_pte(p->pgtable_root, p->asid,
                                  USER_VA, backing_pa, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "first install ok");

    // Snapshot free pages — second identical install must not allocate
    // new sub-tables.
    u64 free_before_second = phys_free_pages();

    rc = mmu_install_user_pte(p->pgtable_root, p->asid,
                              USER_VA, backing_pa, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "idempotent second install returns 0");
    TEST_EXPECT_EQ(phys_free_pages(), free_before_second,
        "no buddy alloc on idempotent re-install");

    // Mismatching install (different PA) returns -1.
    paddr_t other_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(other_pa != 0, "other backing alloc");
    rc = mmu_install_user_pte(p->pgtable_root, p->asid,
                              USER_VA, other_pa, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, -1, "mismatching install rejected");

    drop_proc(p);
}

// Build a synthetic fault_info as if the MMU had just delivered an
// EL0 data abort with the given access type at the given vaddr.
static void make_fi(struct fault_info *fi, u64 vaddr, bool is_write, bool is_instr) {
    fi->vaddr           = vaddr;
    fi->elr             = 0;
    fi->esr             = 0;
    fi->ec              = 0x24;            // EC_DATA_ABORT_LOWER (or 0x20 for instr)
    fi->fsc             = 0x07;            // FSC_TRANS_FAULT_L3 (placeholder)
    fi->fault_level     = 3;
    fi->from_user       = true;
    fi->is_instruction  = is_instr;
    fi->is_write        = is_write;
    fi->is_translation  = true;
    fi->is_permission   = false;
    fi->is_access_flag  = false;
    if (is_instr) {
        fi->ec = 0x20;     // EC_INST_ABORT_LOWER
    }
}

void test_demand_page_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map");

    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x100, /*is_write=*/false, /*is_instr=*/false);

    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "demand_page should resolve a mapped VA");

    // Verify the L3 entry is now installed at USER_VA pointing at burrow->pages.
    u64 pte = walk_to_l3_entry(p->pgtable_root, USER_VA);
    TEST_ASSERT(pte != 0, "L3 entry not installed after demand_page");
    paddr_t expected_pa = page_to_pa(v->pages);
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, expected_pa,
        "PTE PA must equal BURROW's page 0 PA");

    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_no_vma(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct fault_info fi;
    make_fi(&fi, USER_VA, false, false);

    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "no VMA at vaddr should return FAULT_UNHANDLED_USER");

    drop_proc(p);
}

void test_demand_page_permission_denied(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    // RO mapping.
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RO");

    // Write fault on a RO mapping → denied.
    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/true, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "write fault on RO VMA → FAULT_UNHANDLED_USER");

    // Instruction fault on a non-EXEC mapping → denied.
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "instruction fault on non-EXEC VMA → FAULT_UNHANDLED_USER");

    // Read fault on a R-only VMA succeeds.
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/false);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED,
        "read fault on R VMA → FAULT_HANDLED");

    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_lifecycle_round_trip(void) {
    // Snapshot before any allocation. proc_free + burrow_unref must
    // restore phys_free_pages to baseline.
    u64 free_before = phys_free_pages();

    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // Allocate a 4-page BURROW. burrow_create_anon rounds up to 2^order;
    // page_count=4 → order=2 → exactly 4 pages.
    struct Burrow *v = burrow_create_anon(FOUR_PAGES);
    TEST_ASSERT(v != NULL, "burrow_create_anon");
    int rc = burrow_map(p, v, USER_VA, FOUR_PAGES, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map");

    // Demand-page each of the 4 pages.
    for (int i = 0; i < 4; i++) {
        u64 va = USER_VA + (u64)i * ONE_PAGE;
        struct fault_info fi;
        make_fi(&fi, va, /*is_write=*/false, /*is_instr=*/false);
        enum fault_result r = userland_demand_page(p, &fi);
        TEST_EXPECT_EQ(r, FAULT_HANDLED, "demand_page mid-loop");
    }

    // All 4 pages map into a single L1 → L2 → L3 chain (4-page span
    // sits within a single 2 MiB L3-coverage region for this USER_VA).
    // Verify the leaf PTEs are installed.
    for (int i = 0; i < 4; i++) {
        u64 va = USER_VA + (u64)i * ONE_PAGE;
        u64 pte = walk_to_l3_entry(p->pgtable_root, va);
        TEST_ASSERT(pte != 0, "L3 entry not installed for page i");
    }

    // Drop the Proc. proc_free → vma_drain → burrow_release_mapping;
    // proc_pgtable_destroy walks + frees L0 + L1 + L2 + L3.
    drop_proc(p);
    // Drop the BURROW's caller-held handle. Both counts → 0; burrow_free_internal
    // returns the 4 backing pages.
    burrow_unref(v);

    u64 free_after = phys_free_pages();
    TEST_EXPECT_EQ(free_after, free_before,
        "phys_free_pages must return to baseline (no leak in demand-page "
        "lifecycle: VMA → burrow_release_mapping; sub-tables → "
        "proc_pgtable_destroy walker; backing pages → burrow_free_internal)");
}
