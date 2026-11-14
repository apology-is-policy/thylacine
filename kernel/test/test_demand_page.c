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
#include "../../mm/magazines.h"       // B-1a' round 3: the physical control's drain

#include <thylacine/dev.h>           // REVENANT R-2: struct Dev for the stub backing Dev
#include <thylacine/extinction.h>
#include <thylacine/memaccount.h>
#include <thylacine/notes.h>          // groupstatus r1 P3-2: a latch revoked mid-read
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>      // C3: the IRQ mask around the forged EXITING state
#include <thylacine/spoor.h>         // REVENANT R-2: spoor_alloc for the FILE Burrow's backing Chan
#include <thylacine/thread.h>        // ARCH 8.8.3: current_thread + note_interruptible
#include <thylacine/types.h>
#include <thylacine/vma.h>
#include <thylacine/burrow.h>
#include <thylacine/addrspace.h>     // B-1a' audit F8: the holder's file_pages
#include <thylacine/image.h>         // B-1a' audit F8: the cache's reclaim
#include <thylacine/syscall.h>

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
    int rc = mmu_install_user_pte(p->as, proc_resource_exempt(p),
                                  USER_VA, backing_pa, VMA_PROT_RW,
                                  /*device_memory=*/false);
    TEST_EXPECT_EQ(rc, 0, "install RW PTE should succeed");

    u64 pte = walk_to_l3_entry(p->as->pgtable_root, USER_VA);
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
    rc = mmu_install_user_pte(p->as, proc_resource_exempt(p),
                              USER_VA + ONE_PAGE, code_pa, VMA_PROT_RX,
                              /*device_memory=*/false);
    TEST_EXPECT_EQ(rc, 0, "install RX PTE should succeed");

    u64 pte_rx = walk_to_l3_entry(p->as->pgtable_root, USER_VA + ONE_PAGE);
    TEST_ASSERT(pte_rx != 0, "walk to L3 for RX returned 0");
    TEST_ASSERT((pte_rx & BIT_PXN) != 0,    "RX still PXN (kernel never execs)");
    TEST_ASSERT((pte_rx & BIT_UXN) == 0,    "RX clears UXN (user can exec)");
    TEST_EXPECT_EQ(pte_rx & BIT_AP_FIELD, BIT_AP_RO_ANY,
        "AP[2:1] = 0b11 (AP_RO_ANY) for VMA_PROT_RX");

    drop_proc(p);
}

// V-2: the attr-index widening. mmu_install_user_pte_attr selects the MAIR
// index directly, so a mapping can be Normal-NC (write-combining host memory),
// which the old bool device-flag could not express. The bool wrapper must
// preserve the old semantics EXACTLY -- false -> WB, true -> Device -- which is
// the guard against the false==0==MAIR_IDX_DEVICE inversion.
void test_pgtable_install_user_pte_attr_index(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // _attr with NORMAL_NC: the new capability -- an AttrIndx == 1 (NC) PTE.
    paddr_t nc_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(nc_pa != 0, "alloc NC backing");
    TEST_EXPECT_EQ(mmu_install_user_pte_attr(p->as, proc_resource_exempt(p), USER_VA,
                       nc_pa, VMA_PROT_RW, MAIR_IDX_NORMAL_NC), 0,
        "install NC PTE via _attr");
    u64 nc = walk_to_l3_entry(p->as->pgtable_root, USER_VA);
    TEST_ASSERT(nc != 0, "walk to NC L3 entry");
    TEST_EXPECT_EQ((nc >> 2) & 0x7u, (u64)MAIR_IDX_NORMAL_NC,
        "AttrIndx == NORMAL_NC (the write-combining index the bool could not reach)");

    // _attr with NORMAL_WB.
    paddr_t wb_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(wb_pa != 0, "alloc WB backing");
    TEST_EXPECT_EQ(mmu_install_user_pte_attr(p->as, proc_resource_exempt(p),
                       USER_VA + ONE_PAGE, wb_pa, VMA_PROT_RW, MAIR_IDX_NORMAL_WB), 0,
        "install WB PTE via _attr");
    u64 wb = walk_to_l3_entry(p->as->pgtable_root, USER_VA + ONE_PAGE);
    TEST_EXPECT_EQ((wb >> 2) & 0x7u, (u64)MAIR_IDX_NORMAL_WB, "AttrIndx == NORMAL_WB");

    // The bool wrapper preserves the old semantics -- the inversion guard:
    // false is NOT MAIR_IDX_DEVICE (== 0), it maps to NORMAL_WB.
    paddr_t w2_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(w2_pa != 0, "alloc wrapper-WB backing");
    TEST_EXPECT_EQ(mmu_install_user_pte(p->as, proc_resource_exempt(p),
                       USER_VA + 2u * ONE_PAGE, w2_pa, VMA_PROT_RW, false), 0,
        "install via bool wrapper (false)");
    u64 w2 = walk_to_l3_entry(p->as->pgtable_root, USER_VA + 2u * ONE_PAGE);
    TEST_EXPECT_EQ((w2 >> 2) & 0x7u, (u64)MAIR_IDX_NORMAL_WB,
        "bool false -> NORMAL_WB (NOT Device -- the inversion guard)");

    paddr_t dev_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(dev_pa != 0, "alloc Device backing");
    TEST_EXPECT_EQ(mmu_install_user_pte(p->as, proc_resource_exempt(p),
                       USER_VA + 3u * ONE_PAGE, dev_pa, VMA_PROT_RW, true), 0,
        "install via bool wrapper (true)");
    u64 dv = walk_to_l3_entry(p->as->pgtable_root, USER_VA + 3u * ONE_PAGE);
    TEST_EXPECT_EQ((dv >> 2) & 0x7u, (u64)MAIR_IDX_DEVICE, "bool true -> DEVICE");

    drop_proc(p);
}

void test_pgtable_install_user_pte_constraints(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    paddr_t backing_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(backing_pa != 0, "backing alloc");

    // Zero pgtable_root.
    TEST_EXPECT_EQ(mmu_install_user_pte(NULL, false, USER_VA, backing_pa, VMA_PROT_RW, false), -1,
        "pgtable_root=0 rejected");

    // Unaligned vaddr.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->as, proc_resource_exempt(p),
                                        USER_VA + 1, backing_pa, VMA_PROT_RW, false), -1,
        "unaligned vaddr rejected");

    // Unaligned pa.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->as, proc_resource_exempt(p),
                                        USER_VA, backing_pa + 1, VMA_PROT_RW, false), -1,
        "unaligned pa rejected");

    // vaddr in TTBR1 high half — installer rejects (top bits set).
    TEST_EXPECT_EQ(mmu_install_user_pte(p->as, proc_resource_exempt(p),
                                        0xFFFF000000000000ull, backing_pa, VMA_PROT_RW, false), -1,
        "high-VA vaddr rejected");

    // W+X prot.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->as, proc_resource_exempt(p),
                                        USER_VA, backing_pa,
                                        VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC, false), -1,
        "W+X rejected at PTE installer (defense-in-depth)");

    drop_proc(p);
}

void test_pgtable_install_user_pte_idempotent(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    paddr_t backing_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(backing_pa != 0, "backing alloc");

    // First install.
    int rc = mmu_install_user_pte(p->as, proc_resource_exempt(p),
                                  USER_VA, backing_pa, VMA_PROT_RW,
                                  /*device_memory=*/false);
    TEST_EXPECT_EQ(rc, 0, "first install ok");

    // Snapshot free pages — second identical install must not allocate
    // new sub-tables.
    u64 free_before_second = test_free_pages_settled();

    rc = mmu_install_user_pte(p->as, proc_resource_exempt(p),
                              USER_VA, backing_pa, VMA_PROT_RW, false);
    TEST_EXPECT_EQ(rc, 1, "idempotent second install returns 1: already there, nothing written");
    TEST_EXPECT_EQ(test_free_pages_settled(), free_before_second,
        "no buddy alloc on idempotent re-install");

    // Mismatching install (different PA) returns -1.
    paddr_t other_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(other_pa != 0, "other backing alloc");
    rc = mmu_install_user_pte(p->as, proc_resource_exempt(p),
                              USER_VA, other_pa, VMA_PROT_RW, false);
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
    fi->is_alignment  = false;
    fi->is_external  = false;
    if (is_instr) {
        fi->ec = 0x20;     // EC_INST_ABORT_LOWER
    }
}

void test_demand_page_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon(ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_anon failed");

    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map");

    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x100, /*is_write=*/false, /*is_instr=*/false);

    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "demand_page should resolve a mapped VA");

    // Verify the L3 entry is now installed at USER_VA pointing at burrow->pages.
    u64 pte = walk_to_l3_entry(p->as->pgtable_root, USER_VA);
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
    struct Burrow *v = burrow_create_anon(ONE_PAGE, false);
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
    u64 free_before = test_free_pages_settled();

    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // Allocate a 4-page BURROW. burrow_create_anon rounds up to 2^order;
    // page_count=4 → order=2 → exactly 4 pages.
    struct Burrow *v = burrow_create_anon(FOUR_PAGES, false);
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
        u64 pte = walk_to_l3_entry(p->as->pgtable_root, va);
        TEST_ASSERT(pte != 0, "L3 entry not installed for page i");
    }

    // Drop the Proc. proc_free → vma_drain → burrow_release_mapping;
    // proc_pgtable_destroy walks + frees L0 + L1 + L2 + L3.
    drop_proc(p);
    // Drop the BURROW's caller-held handle. Both counts → 0; burrow_free_internal
    // returns the 4 backing pages.
    burrow_unref(v);

    u64 free_after = test_free_pages_settled();
    TEST_EXPECT_EQ(free_after, free_before,
        "phys_free_pages must return to baseline (no leak in demand-page "
        "lifecycle: VMA → burrow_release_mapping; sub-tables → "
        "proc_pgtable_destroy walker; backing pages → burrow_free_internal)");
}

// =============================================================================
// REVENANT R-2: BURROW_TYPE_FILE demand-page fault arm.
//
// A stub backing Dev whose .read fills the buffer with a deterministic
// per-file-offset pattern (so a test verifies the RIGHT file bytes landed in
// the page -- proving the file_offset / slot mapping is honored), or fails
// (-1) when g_rev_read_fail is set (the fail-closed snare:bus path).
// g_rev_read_calls counts reads so a resident fast-hit is proven to NOT re-read.
// =============================================================================

static bool g_rev_read_fail  = false;
static int  g_rev_read_calls = 0;
static long g_rev_read_chunk = 0;    // 0 = full count; >0 = cap each read (force a
                                     // partial Rread -- a conforming 9P server's
                                     // choice, which the slow path must loop over)
static s64  g_rev_read_eof   = -1;   // <0 = no EOF; >=0 = absolute file offset at
                                     // which the backing "file" ends (a read at or
                                     // past it returns 0; a straddling read clamps)

// DISTRO D-3 / #190: the mid-fault geometry shift. The FILE slow path drops
// vma_lock across dev->read precisely so the read can block -- which makes the
// stub read the one deterministic place a test can stand in for "a sibling
// thread re-shaped the mapping while we slept". Set g_rev_shift_vma to a live
// VMA and its burrow_offset moves by one page on the next read, exactly as a
// MAP_FIXED split would move it.
static struct Vma *g_rev_shift_vma = NULL;

// B-1a audit F1: a sibling thread's SYS_BURROW_PROTECT landing while the
// page-in sleeps. Same stand-in as g_rev_shift_vma -- the stub read runs with
// vma_lock DROPPED, so it is the one deterministic place a protect can be
// interposed between the fault's admission and its install.
static struct Proc *g_rev_protect_proc = NULL;
static u64          g_rev_protect_va   = 0;
static u64          g_rev_protect_prot = 0;

// ARCH 8.8.3: what note_interruptible read DURING the page-in -- the stub read
// is the one place that runs inside it. -1 = not recorded.
static bool g_rev_record_intr = false;
static int  g_rev_saw_intr    = -1;

// groupstatus r1 P3-2: a peer registering a note handler while the page-in's
// read runs, which clears the Proc's terminate latches. Set to the Proc; the
// stub registers on it once.
static struct Proc *g_rev_revoke_proc = NULL;

// Round 34 F1: what the payer could still charge WHILE a fill's read sleeps.
// Set to the payer; the stub charges and returns one page on it, once.
static struct AddrSpace *g_rev_probe_as = NULL;
static int               g_rev_probe_ok = -1;

static long rev_test_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c;
    g_rev_read_calls++;
    if (g_rev_record_intr) {
        struct Thread *ct = current_thread();
        g_rev_saw_intr = (ct && ct->note_interruptible) ? 1 : 0;
    }
    if (g_rev_shift_vma) {
        g_rev_shift_vma->burrow_offset += PAGE_SIZE;
        g_rev_shift_vma = NULL;          // shift ONCE, so a re-fault can settle
    }
    if (g_rev_protect_proc) {
        (void)sys_burrow_protect_for_proc(g_rev_protect_proc, g_rev_protect_va,
                                          PAGE_SIZE, g_rev_protect_prot, 0);
        g_rev_protect_proc = NULL;       // protect ONCE
    }
    if (g_rev_revoke_proc) {
        notes_set_handler(g_rev_revoke_proc, 0x1000u);
        g_rev_revoke_proc = NULL;        // register ONCE
    }
    if (g_rev_probe_as) {
        g_rev_probe_ok = addrspace_charge_pages(g_rev_probe_as, 1, false) ? 1 : 0;
        if (g_rev_probe_ok) addrspace_uncharge_pages(g_rev_probe_as, 1);
        g_rev_probe_as = NULL;           // probe ONCE
    }
    if (g_rev_read_fail) return -1;
    long m = n;
    if (g_rev_read_eof >= 0) {
        if (off >= g_rev_read_eof) return 0;             // true EOF
        if ((s64)m > g_rev_read_eof - off)
            m = (long)(g_rev_read_eof - off);            // clamp to EOF
    }
    if (g_rev_read_chunk > 0 && m > g_rev_read_chunk)
        m = g_rev_read_chunk;        // short-return (interior, NOT EOF)
    u8 *b = (u8 *)buf;
    for (long i = 0; i < m; i++) b[i] = (u8)(((u64)off + (u64)i) & 0xff);
    return m;
}

// Models a content server: its images are shared and cached (vote 114).
static bool fake_content_versioned(struct Spoor *c) {
    (void)c;
    return true;
}

static struct Dev g_rev_test_dev = {
    .dc   = '?',
    .name = "revtest",
    .read = rev_test_read,
    .content_versioned = fake_content_versioned,
};

void test_demand_page_file_smoke(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;

    // A FILE Burrow over the stub Dev, backing file bytes [0x2000, 0x3000) --
    // a non-zero file_offset, so the content pattern is offset-keyed.
    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    const u64 FILE_OFF = 0x2000;
    struct Burrow *v = burrow_create_file(s, FILE_OFF, ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file");

    // Map it R+X (text) at USER_VA.
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RX");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) == NULL, "slot 0 starts empty");

    // Instruction fault (text) -> demand-read one page.
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/true);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "FILE demand-page resolves a mapped text VA");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "exactly one dev->read for the miss");

    // The slot is now resident; the PTE points at it; the PTE is R+X (W^X).
    struct page *slotpg = burrow_file_slot_for_test(v, 0);
    TEST_ASSERT(slotpg != NULL, "slot 0 resident after fault");
    TEST_ASSERT(slotpg->holder.sponsor == account_system_cache(),
        "a file page is the system cache's, not the first faulter's (the single path)");
    u64 pte = walk_to_l3_entry(p->as->pgtable_root, USER_VA);
    TEST_ASSERT(pte != 0, "L3 PTE installed after FILE demand-page");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(slotpg),
        "PTE PA equals the resident FILE slot page");
    TEST_ASSERT((pte & BIT_UXN) == 0, "R+X text clears UXN (user can exec)");
    TEST_EXPECT_EQ(pte & BIT_AP_FIELD, BIT_AP_RO_ANY,
        "R+X text is RO_ANY (W^X-clean: text never writable)");

    // Content: the stub filled the page with (FILE_OFF + i) & 0xff -- proves the
    // right file bytes landed (file_offset honored, NOT a silent zero page).
    u8 *bytes = (u8 *)pa_to_kva(page_to_pa(slotpg));
    TEST_EXPECT_EQ((u64)bytes[0],    (u64)(u8)(FILE_OFF & 0xff),         "byte 0 = file_offset pattern");
    TEST_EXPECT_EQ((u64)bytes[5],    (u64)(u8)((FILE_OFF + 5) & 0xff),   "byte 5 matches pattern");
    TEST_EXPECT_EQ((u64)bytes[0xff], (u64)(u8)((FILE_OFF + 0xff) & 0xff),"byte 255 matches pattern");

    // A SECOND fault to the same page hits the resident fast path -- no re-read.
    make_fi(&fi, USER_VA + 0x800, /*is_write=*/false, /*is_instr=*/true);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "second fault to resident page resolves");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "fast-hit must NOT re-read (still 1)");

    drop_proc(p);
    burrow_unref(v);
}

// A map past the inline size has node pages, and they outlive the first faulter just as
// the data pages do, so they are not that faulter's to pay for either.
void test_demand_page_file_nodes_are_the_system_caches(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_chunk = 0;
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    const u64 size = (PAGEMAP_INLINE_MAX + 8u) * PAGE_SIZE;
    struct Burrow *v = burrow_create_file(s, 0, size, true);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    TEST_ASSERT(v->pm.depth > 0, "the premise: a radix map");
    int rc = burrow_map(p, v, USER_VA, size, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RX");

    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "a fault installs a page");
    TEST_ASSERT(v->pm.nodes > 0, "and so a node");
    const struct page *node = pa_to_page(kva_to_pa(v->pm.root));
    TEST_ASSERT(node->flags & PG_SPONSORED, "the node is on the ledger");
    TEST_ASSERT(node->holder.sponsor == account_system_cache(),
        "and it is the system cache's, not the first faulter's");

    drop_proc(p);
    burrow_unref(v);
}

// ARCH 8.8.3 (signal(7)'s list): a page-in waits killable, as Linux's
// filemap_fault does. A caught note that cut it short would be a SIGBUS at EL0,
// or an EFAULT from inside an interruptible read's copy-out -- so
// userland_demand_page clears note_interruptible around the page-in and
// RESTORES it after, since the fault may sit inside a syscall that is itself
// interruptible. The thread starts interruptible, as it would inside a pipe
// read whose buffer is a not-yet-resident file page.
void test_demand_page_file_pagein_not_note_interruptible(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX), 0,
                   "burrow_map RX");

    struct Thread *t = current_thread();
    TEST_ASSERT(t != NULL, "a current thread");
    bool saved = t->note_interruptible;
    t->note_interruptible = true;
    g_rev_saw_intr    = -1;
    g_rev_record_intr = true;

    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
    enum fault_result r = userland_demand_page(p, &fi);

    g_rev_record_intr = false;
    bool after = t->note_interruptible;
    t->note_interruptible = saved;

    TEST_EXPECT_EQ(r, FAULT_HANDLED, "the page-in resolves");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "the page-in read ran");
    TEST_EXPECT_EQ(g_rev_saw_intr, 0,
                   "the page-in ran with note_interruptible CLEAR (killable only)");
    TEST_ASSERT(after, "the fault RESTORED the flag for the syscall it sits in");

    drop_proc(p);
    burrow_unref(v);
}

// DISTRO D-3 / #190: a FILE fault whose VMA GEOMETRY MOVES while the page-in
// sleeps must BAIL, not install.
//
// The R-5 audit filed this as F2 [P3, unreachable at v1.0] on the premise that a
// FILE Burrow "is created only by exec and mapped exactly once at burrow_offset
// 0". D-3 retires that premise -- userspace mmap creates FILE Burrows, and its
// MAP_FIXED replacement splits a FILE VMA so a tail piece carries a non-zero
// burrow_offset. This is the regression test for the retirement.
//
// It fails on the pre-fix code and passes on the post-fix code. Pre-fix, the
// install trusted freq->slot: with the geometry shifted one page, the bytes read
// for slot 0 were filed under slot 0 while the faulting VA now names slot 1 --
// so the fault "succeeded" and EL0 got the WRONG FILE PAGE at that address. The
// only re-validation was `vma->burrow == freq->burrow`, which a shifted VMA over
// the same Burrow satisfies, so nothing caught it.
//
// Note what is asserted: NOT that the fault fails forever, but that it refuses
// to install STALE bytes. The re-fault below is the other half -- a bail must
// leave the mapping usable, since a real racing split ends with a valid
// geometry that the next fault resolves against.
//
// THIS TEST COVERS THE CLUSTER INSTALL ONLY, and the split is not cosmetic. The
// two install paths carry SEPARATE copies of the check, so one test cannot
// cover both -- and which path a fault takes is not a knob but a consequence of
// page_count: file_demand_page_cluster degrades to the single path only when
// ncluster <= 1, which needs a ONE-page Burrow. Hence the sibling test
// file_geometry_shift_bails_single, whose Burrow is one page for exactly that
// reason. Sabotaging one check must redden one test and not the other; if a
// single sabotage reddens both, they are not covering what they claim.
void test_demand_page_file_geometry_shift_bails(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    g_rev_read_eof   = -1;
    g_rev_shift_vma  = NULL;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    // TWO pages: enough for a slot 1 to exist, and enough that ncluster == 2 so
    // the fault takes the CLUSTER path.
    struct Burrow *v = burrow_create_file(s, 0, 2 * ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_file");

    int rc = burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map R-only 2 pages");

    struct Vma *vma = vma_lookup(p, USER_VA);
    TEST_ASSERT(vma != NULL, "the VMA is findable");
    TEST_EXPECT_EQ((u64)vma->burrow_offset, 0ull, "starts at burrow_offset 0");

    // Arm the shift, then fault. The read runs with vma_lock DROPPED, moves the
    // VMA's burrow_offset by a page, and returns its bytes; the install then
    // finds the faulting VA naming a different slot than the one those bytes
    // belong to.
    g_rev_shift_vma = vma;
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "a mid-fault geometry shift must BAIL, not install stale bytes");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "the read did happen (the bail is post-read)");

    // Nothing was filed. This is the assertion that separates the fix from the
    // bug: pre-fix, slot 0 held the read page and the PTE was live.
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) == NULL, "slot 0 still empty");
    TEST_ASSERT(burrow_file_slot_for_test(v, 1) == NULL, "slot 1 still empty");
    TEST_EXPECT_EQ(walk_to_l3_entry(p->as->pgtable_root, USER_VA), 0ull,
        "no PTE installed on the bail");

    // The shift already fired and disarmed. A re-fault now sees a SETTLED
    // geometry (burrow_offset one page in) and must resolve normally -- proving
    // the bail is a retry, not a wedge, and that the check is not simply
    // rejecting every non-zero burrow_offset.
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "the re-fault resolves against the new geometry");
    TEST_ASSERT(burrow_file_slot_for_test(v, 1) != NULL,
        "the settled geometry files the faulting VA under slot 1");

    // Slot 0 is ALSO filled here, and that is read-ahead doing its job rather
    // than the stale page reappearing: the faulting slot is 1, so the cluster is
    // [0, 2) and one batched read fills both. (An earlier draft asserted slot 0
    // stayed empty and FAILED for exactly this reason -- the test was asserting
    // a property the design never promised.) Content cannot discriminate the two
    // stories, since both would put file-offset-0 bytes here; what discriminates
    // is the BAIL assertions above, which are the ones that redden pre-fix.

    // The faulting slot holds the RIGHT file window -- the stub's pattern is
    // offset-keyed, so this proves the shifted burrow_offset was honoured rather
    // than merely that SOME page arrived.
    u8 *bytes = (u8 *)pa_to_kva(page_to_pa(burrow_file_slot_for_test(v, 1)));
    TEST_EXPECT_EQ((u64)bytes[0], (u64)(u8)(ONE_PAGE & 0xff),
        "slot 1 holds file offset ONE_PAGE's bytes, not slot 0's");

    drop_proc(p);
    burrow_unref(v);
}

// DISTRO D-3 / #190, the SINGLE-page install path. Same defect, different copy
// of the check (file_install_locked vs file_install_cluster_locked), so it needs
// its own test -- see the sibling above for why one cannot reach both.
//
// A ONE-page Burrow is what forces the single path: ncluster == 1 makes
// file_demand_page_cluster degrade immediately. And a one-page Burrow is what
// makes this leg sharpest, because the pre-existing `freq->slot >= page_count`
// guard reads the CACHED slot (0, in range) and therefore does NOT catch the
// shift -- it is the geometry check or nothing.
void test_demand_page_file_geometry_shift_bails_single(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    g_rev_read_eof   = -1;
    g_rev_shift_vma  = NULL;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_file");

    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map R-only 1 page");

    struct Vma *vma = vma_lookup(p, USER_VA);
    TEST_ASSERT(vma != NULL, "the VMA is findable");

    g_rev_shift_vma = vma;
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "single-path: a mid-fault geometry shift must BAIL");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "exactly one read (the single path)");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) == NULL,
        "single-path: slot 0 not filled with the stale page");
    TEST_EXPECT_EQ(walk_to_l3_entry(p->as->pgtable_root, USER_VA), 0ull,
        "single-path: no PTE installed on the bail");

    drop_proc(p);
    burrow_unref(v);
}

// B-1a audit F1 (P1): a FILE read fault admitted at R, whose page-in sleeps
// while a sibling protects the page to NONE, must NOT install. The FILE slow
// path is the one arm whose admission and install span an unlock; the
// re-lookup after the sleep verifies the geometry (same Burrow, same slot),
// which a whole-mapping protect leaves intact, so only a second admission
// check stands between the fault and a user-readable RO leaf on a none page
// -- a guard that does not guard, with no fault ever running step 2 again.
// Pre-fix: FAULT_HANDLED and a live RO PTE. The page-in is KEPT (the bytes
// are the Burrow's), so a raise back to R resolves from the slot with no
// second read. Single-path twin (a ONE-page Burrow degrades the cluster fill
// to the single fill -- the same knob the geometry-shift pair uses).
void test_protect_file_pagein_racing_protect_bails_single(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    g_rev_read_eof   = -1;
    g_rev_shift_vma  = NULL;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map R-only 1 page");

    // Arm: the read protects the page to NONE while the fault sleeps.
    g_rev_protect_proc = p;
    g_rev_protect_va   = USER_VA;
    g_rev_protect_prot = 0;
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "single-path: a read admitted at R and installed after a protect to none is REFUSED");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "the read happened (the refusal is post-read)");
    TEST_EXPECT_EQ(walk_to_l3_entry(p->as->pgtable_root, USER_VA), 0ull,
        "NO PTE on a none page (pre-fix: a user-readable RO leaf)");
    struct Vma *vma = vma_lookup(p, USER_VA);
    TEST_ASSERT(vma != NULL && vma->prot == 0, "the mapping reads none");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) != NULL,
        "the page-in is kept: the bytes are the Burrow's, not the prot's");

    // Raise back to R: the re-fault installs from the resident slot, no re-read.
    TEST_EXPECT_EQ(sys_burrow_protect_for_proc(p, USER_VA, ONE_PAGE, 1 /*R*/, 0), 0,
                   "protect back to R");
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "the re-fault at R resolves");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "from the resident slot: no second read");
    u64 pte = walk_to_l3_entry(p->as->pgtable_root, USER_VA);
    TEST_ASSERT(pte != 0, "installed");
    TEST_EXPECT_EQ(pte & BIT_AP_FIELD, BIT_AP_RO_ANY, "read-only, as the mapping says");

    drop_proc(p);
    burrow_unref(v);
}

// The cluster-path twin (a two-page Burrow takes the read-ahead fill). The two
// install paths carry SEPARATE copies of the admission re-check, as they do of
// the geometry check, so one test cannot cover both.
void test_protect_file_pagein_racing_protect_bails_cluster(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    g_rev_read_eof   = -1;
    g_rev_shift_vma  = NULL;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, 2 * ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    int rc = burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map R-only 2 pages");

    g_rev_protect_proc = p;
    g_rev_protect_va   = USER_VA;
    g_rev_protect_prot = 0;
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER,
        "cluster-path: a read admitted at R and installed after a protect to none is REFUSED");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "one batched read (the cluster fill)");
    TEST_EXPECT_EQ(walk_to_l3_entry(p->as->pgtable_root, USER_VA), 0ull,
        "NO PTE on the none page");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) != NULL, "slot 0 adopted (the cluster is kept)");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0)->holder.sponsor == account_system_cache(),
        "a file page is the system cache's, not the first faulter's (the cluster path)");
    TEST_ASSERT(burrow_file_slot_for_test(v, 1) != NULL, "slot 1 adopted too");
    struct Vma *vma = vma_lookup(p, USER_VA);
    TEST_ASSERT(vma != NULL && vma->prot == 0, "page 0's piece reads none");
    struct Vma *vma1 = vma_lookup(p, USER_VA + ONE_PAGE);
    TEST_ASSERT(vma1 != NULL && vma1->prot == VMA_PROT_READ, "page 1's piece still reads R");

    // Page 1 was never protected: its fault resolves from the adopted slot.
    make_fi(&fi, USER_VA + ONE_PAGE + 0x10, /*is_write=*/false, /*is_instr=*/false);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "the untouched page resolves");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "from the resident slot: no second read");

    drop_proc(p);
    burrow_unref(v);
}

// I-36 (round 32 F2): an RX file mapping protected down to R in part fills pages
// through the R piece that the RX piece then executes from the resident slot,
// with no sync of its own. Every fill of an exec-class Burrow syncs, whichever
// piece faulted; a Burrow outside the class syncs nothing and maps no X.
void test_demand_page_file_exec_class_syncs_every_fill(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    g_rev_read_eof   = -1;
    g_rev_shift_vma  = NULL;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, 2 * ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file (exec class)");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_RX), 0, "burrow_map RX");
    TEST_EXPECT_EQ(sys_burrow_protect_for_proc(p, USER_VA + ONE_PAGE, ONE_PAGE, 1 /*R*/, 0), 0,
                   "protect page 1 down to R");
    struct Vma *v1 = vma_lookup(p, USER_VA + ONE_PAGE);
    TEST_ASSERT(v1 != NULL && v1->prot == VMA_PROT_READ && v1->burrow == v,
                "page 1's piece is R over the same Burrow");

    // A data read through the R piece takes the cluster fill: both slots.
    u64 syncs0 = __atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED);
    struct fault_info fi;
    make_fi(&fi, USER_VA + ONE_PAGE + 0x10, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "the R piece's read resolves");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) != NULL && burrow_file_slot_for_test(v, 1) != NULL,
                "both slots filled by the R piece's fault");
    TEST_EXPECT_EQ(__atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED) - syncs0, 2ull,
                   "both fills synced: the class decides, not the R piece's prot");

    // The RX piece executes slot 0 from the resident hit, which syncs nothing.
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "the RX piece's fetch resolves");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "from the resident slot: no second read");
    drop_proc(p);
    burrow_unref(v);

    // Control: outside the class, fills skip the sync and no X mapping is admitted.
    p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed (control)");
    s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc (control)");
    v = burrow_create_file(s, 0, 2 * ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_file (no exec class)");
    TEST_ASSERT(burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_RX) != 0,
                "an RX mapping outside the exec class is refused");
    TEST_ASSERT(vma_lookup(p, USER_VA) == NULL, "and installs nothing");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_READ), 0, "burrow_map R");
    syncs0 = __atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED);
    make_fi(&fi, USER_VA + 0x10, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "the R read resolves");
    TEST_ASSERT(burrow_file_slot_for_test(v, 1) != NULL, "the cluster filled slot 1 too");
    TEST_EXPECT_EQ(__atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED) - syncs0, 0ull,
                   "no sync outside the class");
    drop_proc(p);
    burrow_unref(v);

    // The single fill carries its own copy of the sync (round 33 F1): a one-page
    // Burrow degrades the cluster to it. An RX page protected whole to R, read.
    p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed (single)");
    s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc (single)");
    v = burrow_create_file(s, 0, ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file (single, exec class)");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX), 0, "burrow_map RX (single)");
    TEST_EXPECT_EQ(sys_burrow_protect_for_proc(p, USER_VA, ONE_PAGE, 1 /*R*/, 0), 0,
                   "protect the page to R");
    g_rev_read_calls = 0;
    syncs0 = __atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED);
    make_fi(&fi, USER_VA + 0x20, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "the single-path read resolves");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "one read (the single fill)");
    TEST_EXPECT_EQ(__atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED) - syncs0, 1ull,
                   "the single fill synced through the R mapping");
    drop_proc(p);
    burrow_unref(v);

    // Its control: outside the class, the single fill syncs nothing.
    p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed (single control)");
    s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc (single control)");
    v = burrow_create_file(s, 0, ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_file (single, no exec class)");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_READ), 0, "burrow_map R (single)");
    syncs0 = __atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED);
    make_fi(&fi, USER_VA + 0x20, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "the single-path R read resolves");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) != NULL, "slot 0 filled");
    TEST_EXPECT_EQ(__atomic_load_n(&g_file_fill_syncs_for_test, __ATOMIC_RELAXED) - syncs0, 0ull,
                   "no single-fill sync outside the class");
    drop_proc(p);
    burrow_unref(v);
}

// #45 / REVENANT 4.6: an R-only (rodata) FILE mapping demand-pages with a
// NON-EXECUTABLE read-only PTE -- the prot-generality the rodata dispatch
// relies on. Mirrors file_smoke with VMA_PROT_READ + a data-read fault; the
// UXN assertion is the inverse of file_smoke's (text clears UXN, rodata sets
// it), pinning that the arm installs the VMA's own prot, not a hardcoded R+X.
void test_demand_page_file_rodata_prot(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    g_rev_read_eof   = -1;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    const u64 FILE_OFF = 0x7000;
    struct Burrow *v = burrow_create_file(s, FILE_OFF, ONE_PAGE, false);
    TEST_ASSERT(v != NULL, "burrow_create_file");

    // Map it R-only (rodata) at USER_VA.
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_READ);
    TEST_EXPECT_EQ(rc, 0, "burrow_map R-only");

    // Data-read fault (NOT instruction) -> demand-read one page.
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x10, /*is_write=*/false, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "FILE demand-page resolves an R-only VA");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "exactly one dev->read for the miss");

    struct page *slotpg = burrow_file_slot_for_test(v, 0);
    TEST_ASSERT(slotpg != NULL, "slot 0 resident after fault");
    u64 pte = walk_to_l3_entry(p->as->pgtable_root, USER_VA);
    TEST_ASSERT(pte != 0, "L3 PTE installed after FILE demand-page");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(slotpg),
        "PTE PA equals the resident FILE slot page");
    TEST_ASSERT((pte & BIT_UXN) != 0, "R-only rodata SETS UXN (never executable)");
    TEST_ASSERT((pte & BIT_PXN) != 0, "PXN set (kernel never execs user)");
    TEST_EXPECT_EQ(pte & BIT_AP_FIELD, BIT_AP_RO_ANY,
        "R-only rodata is RO_ANY (W^X: never writable)");

    // The right FILE bytes landed (offset-keyed pattern), not a zero page.
    // #45 audit F3: probe NON-zero-coinciding offsets -- byte 0 of FILE_OFF
    // 0x7000 is pattern 0x00, indistinguishable from a zero fill, so it carries
    // no signal (the R-5 R2-F1 vacuity class). byte 1 (0x01) + byte 0x9f (0x9f)
    // both genuinely discriminate file-data from zero-fill.
    u8 *bytes = (u8 *)pa_to_kva(page_to_pa(slotpg));
    TEST_EXPECT_EQ((u64)bytes[1],    (u64)(u8)((FILE_OFF + 1) & 0xff),
        "byte 1 = file_offset pattern (non-zero -> discriminates zero-fill)");
    TEST_EXPECT_EQ((u64)bytes[0x9f], (u64)(u8)((FILE_OFF + 0x9f) & 0xff),
        "byte 0x9f matches pattern");

    // W^X on the R-only VMA: an instruction fault and a write fault are both
    // REFUSED (the prot gate runs before the backing dispatch).
    make_fi(&fi, USER_VA + 0x20, /*is_write=*/false, /*is_instr=*/true);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER, "instr fault on R-only VMA refused");
    make_fi(&fi, USER_VA + 0x30, /*is_write=*/true, /*is_instr=*/false);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER, "write fault on R-only VMA refused");

    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_file_read_error_snare_bus(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = true;     // the stub .read returns -1 (a dead/wedged FS)
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RX");

    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
    enum fault_result r = userland_demand_page(p, &fi);
    // Fail closed (I-36 condition 6): a page-in I/O error -> FAULT_USER_BUS
    // (snare:bus), NEVER a FAULT_HANDLED with a zero-filled text page.
    TEST_EXPECT_EQ(r, FAULT_USER_BUS, "FILE read error -> FAULT_USER_BUS (snare:bus)");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "the read was attempted once");
    // No page installed (no silent zero-fill) and no PTE.
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) == NULL,
        "no page installed on a read error (no silent zero-fill of text)");
    TEST_ASSERT(walk_to_l3_entry(p->as->pgtable_root, USER_VA) == 0,
        "no PTE installed on a read error");

    g_rev_read_fail = false;     // reset for other tests
    drop_proc(p);
    burrow_unref(v);
}

// I-24 + I-36 condition 6: a page-in that fails while the faulting thread's
// death is pending is answered by the death, not by snare:bus -- the common
// case is the group's own death abandoning the 9P read, and reporting it as a
// bus error both prints a false user fault and races the claimed status.
// Runs INSIDE an rfork'd child, because the pager asks the faulting thread.
// Control first, one variable away: the same failing read with no death
// pending is FAULT_USER_BUS. Then the child claims its group's termination
// (an exit_group(5)) and the same fault answers FAULT_USER_DYING; the child
// leaves as a peer does and reaps with 5.
static volatile int g_dying_ctl_r;
static volatile int g_dying_r;

static void dying_page_in_entry(void *arg) {
    (void)arg;
    struct Proc *p = current_thread()->proc;
    g_dying_ctl_r = g_dying_r = -1;
    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    struct Burrow *v = s ? burrow_create_file(s, 0, ONE_PAGE, true) : NULL;
    if (v && burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX) == 0) {
        struct fault_info fi;
        make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
        g_dying_ctl_r = (int)userland_demand_page(p, &fi);
        irq_state_t st = proc_table_lock_acquire();
        proc_group_terminate_code(p, 5, "five");
        proc_table_lock_release(st);
        make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
        g_dying_r = (int)userland_demand_page(p, &fi);
    }
    if (v) burrow_unref(v);
    thread_exit_self();
}

void test_demand_page_file_read_error_dying_is_death(void) {
    g_rev_read_fail  = true;
    g_rev_read_calls = 0;
    int pid = rfork(RFPROC, dying_page_in_entry, NULL);
    int status = -1;
    int reaped = pid > 0 ? wait_pid(&status) : -1;
    g_rev_read_fail = false;     // reset for other tests (before any assert returns)
    TEST_ASSERT(pid > 0, "rfork");
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaped the child");
    TEST_EXPECT_EQ(g_dying_ctl_r, (int)FAULT_USER_BUS,
        "control: a failed page-in with no death pending is snare:bus");
    TEST_EXPECT_EQ(g_dying_r, (int)FAULT_USER_DYING,
        "a failed page-in with the thread's death pending is answered by the death");
    TEST_EXPECT_EQ(g_rev_read_calls, 2, "both faults attempted the read");
    TEST_EXPECT_EQ(status, 5, "the child reaped with its group's claimed status");
}

// Nothing past the EXITING commit sleeps (ARCH 7.3.1): the exit's
// clear-child-tid store follows the commit, and a FILE miss is the one fault
// arm that sleeps, so it refuses a thread that is not RUNNING -- nothing read,
// nothing installed, the store fails as on an unmapped page. No writable FILE
// mapping exists, so no store reaches the arm today; a read fault with the
// state forged EXITING does. Runs in an rfork'd child, because the pager asks
// the faulting thread; IRQs stay masked while the state says EXITING, so
// nothing switches the thread out on its word. Control one variable away: the
// same fault RUNNING reads the page and installs it.
static volatile int g_ns_r, g_ns_reads, g_ns_installed;
static volatile int g_ns_ctl_r, g_ns_ctl_reads, g_ns_ctl_installed;

static void no_sleep_after_commit_entry(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    struct Proc *p = t->proc;
    g_ns_r = g_ns_ctl_r = -1;
    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    struct Burrow *v = s ? burrow_create_file(s, 0, ONE_PAGE, true) : NULL;
    if (v && burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX) == 0) {
        struct fault_info fi;
        make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/false);
        int calls = g_rev_read_calls;
        irq_state_t is = spin_lock_irqsave(NULL);
        t->state = THREAD_EXITING;
        g_ns_r = (int)userland_demand_page(p, &fi);
        t->state = THREAD_RUNNING;
        spin_unlock_irqrestore(NULL, is);
        g_ns_reads     = g_rev_read_calls - calls;
        g_ns_installed = burrow_file_slot_for_test(v, 0) != NULL;

        make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/false);
        calls = g_rev_read_calls;
        g_ns_ctl_r         = (int)userland_demand_page(p, &fi);
        g_ns_ctl_reads     = g_rev_read_calls - calls;
        g_ns_ctl_installed = burrow_file_slot_for_test(v, 0) != NULL;
    }
    if (v) burrow_unref(v);
    thread_exit_self();
}

void test_demand_page_file_miss_refuses_exiting_thread(void) {
    g_rev_read_calls = 0;
    int pid = rfork(RFPROC, no_sleep_after_commit_entry, NULL);
    int status = -1;
    int reaped = pid > 0 ? wait_pid(&status) : -1;
    TEST_ASSERT(pid > 0, "rfork");
    TEST_EXPECT_EQ(reaped, pid, "wait_pid reaped the child");
    TEST_EXPECT_EQ(g_ns_r, (int)FAULT_UNHANDLED_USER,
        "a FILE miss refuses a thread past its EXITING commit");
    TEST_EXPECT_EQ(g_ns_reads, 0, "the refused fault read nothing");
    TEST_EXPECT_EQ(g_ns_installed, 0, "the refused fault installed nothing");
    TEST_EXPECT_EQ(g_ns_ctl_r, (int)FAULT_HANDLED, "control: the same fault RUNNING is handled");
    TEST_EXPECT_NE(g_ns_ctl_reads, 0, "control: it read the page");
    TEST_EXPECT_EQ(g_ns_ctl_installed, 1, "control: it installed the page");
}

// A kernel access holding a counted lock cannot wait for a page-in, since sched()
// refuses a held lock, and torpor_wait's load under its bucket lock can land on an
// unfaulted file page. So the miss refuses, with nothing read and nothing pinned, and
// the same fault with no lock pages in. The stub does not sleep, so the read count is
// what tells: on a real server the read would be the extinction.
void test_demand_page_file_miss_under_a_lock_refuses(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX), 0, "burrow_map RX");
    int handles = __atomic_load_n(&v->handle_count, __ATOMIC_ACQUIRE);

    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/false);
    spin_lock_t held = SPIN_LOCK_INIT;
    spin_lock(&held);
    enum fault_result r = userland_demand_page(p, &fi);
    spin_unlock(&held);

    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER, "the miss refuses under a held lock");
    TEST_EXPECT_EQ(g_rev_read_calls, 0, "the refused fault read nothing");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) == NULL, "and installed nothing");
    TEST_EXPECT_EQ(__atomic_load_n(&v->handle_count, __ATOMIC_ACQUIRE), handles,
                   "and left no pin behind");

    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "control: with no lock held it pages in");
    TEST_EXPECT_NE(g_rev_read_calls, 0, "control: it read the page");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) != NULL, "control: it installed the page");

    drop_proc(p);
    burrow_unref(v);
}

// Pending is sampled before the page-in as well as after. A terminate latch
// that a peer revokes while the read runs (a handler registered, interposed in
// the stub read) leaves only the first sample true; the failed read is still
// answered FAULT_USER_DYING -- the EL0 arm then finds the latch stale and
// re-executes -- never FAULT_USER_BUS, a false snare:bus for a read the latch
// abandoned.
static volatile int g_revoke_r;
static volatile int g_revoke_armed;
static volatile int g_revoke_after;

static void revoked_latch_page_in_entry(void *arg) {
    (void)arg;
    struct Thread *t = current_thread();
    struct Proc *p = t->proc;
    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    struct Burrow *v = s ? burrow_create_file(s, 0, ONE_PAGE, true) : NULL;
    if (v && burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX) == 0) {
        (void)notes_post(p, "interrupt", 0u, NULL, true);
        g_revoke_armed = thread_die_pending(t) ? 1 : 0;
        g_rev_revoke_proc = p;
        struct fault_info fi;
        make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
        g_revoke_r = (int)userland_demand_page(p, &fi);
        g_revoke_after = thread_die_pending(t) ? 1 : 0;
    }
    if (v) burrow_unref(v);
    thread_exit_self();
}

void test_demand_page_file_read_revoked_latch_is_dying(void) {
    g_rev_read_fail  = true;
    g_rev_read_calls = 0;
    g_revoke_r = g_revoke_armed = g_revoke_after = -1;
    int pid = rfork(RFPROC, revoked_latch_page_in_entry, NULL);
    int status = -1;
    int reaped = pid > 0 ? wait_pid_for(pid, 0, &status) : -1;
    g_rev_read_fail   = false;   // reset for other tests (before any assert returns)
    g_rev_revoke_proc = NULL;
    TEST_ASSERT(pid > 0, "rfork");
    TEST_EXPECT_EQ(reaped, pid, "reaped the child");
    TEST_EXPECT_EQ(g_revoke_armed, 1, "the interrupt armed a latch before the page-in");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "the page-in attempted the read");
    TEST_EXPECT_EQ(g_revoke_after, 0, "the handler registered during the read revoked it");
    TEST_EXPECT_EQ(g_revoke_r, (int)FAULT_USER_DYING,
        "a failed read under a latch revoked mid-read is not a snare:bus");
}

// REVENANT read-ahead (#343): a FILE fault fills a CLUSTER of pages from ONE
// batched dev->read -- the Linux page-cache readahead model that closes the
// 4 KiB-fault-vs-8-MiB-Stratum-extent amplification (~400x slower than the host).
// Verify: the faulted slot + its cluster-mates carry the CORRECT per-slot file
// bytes (the slot<->offset map -- an off-by-one would corrupt a slot); read-ahead
// is BOUNDED (a far page stays sparse -- it does NOT slurp the whole file, which
// would re-introduce the 1-MiB-slurp cost REVENANT retired); and the whole
// cluster comes from ONE batched read (g_rev_read_calls == 1). The 200-page
// Burrow spans more than one read-ahead cluster, so the boundary is exercised
// without coupling the test to the exact REVENANT_READAHEAD_PAGES constant.
void test_demand_page_file_multi_page(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;            // full-count reads (defensive; a sibling may have set it)

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    const u64    FILE_OFF = 0x4000;
    const size_t NPAGES   = 200;                    // > one read-ahead cluster
    const u64    SPAN     = (u64)NPAGES * ONE_PAGE;
    struct Burrow *v = burrow_create_file(s, FILE_OFF, SPAN, true);
    TEST_ASSERT(v != NULL, "burrow_create_file 200-page");
    int rc = burrow_map(p, v, USER_VA, SPAN, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map 200-page FILE RX");

    // Fault page 2 (out of order -> the cluster aligns DOWN to slot 0; an
    // off-by-one in the per-slot offset would read the wrong file bytes).
    struct fault_info fi;
    make_fi(&fi, USER_VA + 2 * ONE_PAGE + 0x10, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 2 fault resolves");

    // The faulted slot + a cluster-mate (slot 0) are resident with the CORRECT
    // per-slot file bytes (slot k holds the file byte at FILE_OFF + k pages).
    struct page *pg2 = burrow_file_slot_for_test(v, 2);
    TEST_ASSERT(pg2 != NULL, "slot 2 resident");
    TEST_EXPECT_EQ((u64)((u8 *)pa_to_kva(page_to_pa(pg2)))[0],
        (u64)(u8)((FILE_OFF + 2 * ONE_PAGE) & 0xff),
        "slot 2 content = file bytes at FILE_OFF + 2 pages (slot<->offset map)");
    struct page *pg0 = burrow_file_slot_for_test(v, 0);
    TEST_ASSERT(pg0 != NULL, "slot 0 resident (read-ahead cluster-mate)");
    TEST_EXPECT_EQ((u64)((u8 *)pa_to_kva(page_to_pa(pg0)))[0],
        (u64)(u8)(FILE_OFF & 0xff),
        "slot 0 content = file bytes at FILE_OFF (cluster start)");

    // Read-ahead is BOUNDED: a far page stays sparse -- the fault does NOT slurp
    // the whole file.
    TEST_ASSERT(burrow_file_slot_for_test(v, NPAGES - 1) == NULL,
        "far slot 199 sparse (read-ahead is a bounded cluster, not the whole file)");

    // The whole cluster came from ONE batched read (the amplification fix).
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "the cluster filled in ONE batched read");

    drop_proc(p);
    burrow_unref(v);
}

// REVENANT R-5 audit SA-F1: an INTERIOR page whose backing dev->read short-returns
// (a conforming 9P server may return a partial Rread mid-file -- dev9p_read issues
// ONE Tread and returns its count) must still be FULLY filled. The pre-fix single
// read filled only the first chunk and left the tail KP_ZERO -> a corrupt interior
// text page on a Stratum-backed binary. The fixed slow path loops until the page
// is full or true EOF. Assert EVERY byte (incl. the last) carries the file pattern,
// and that the loop iterated PAGE_SIZE/chunk times.
void test_demand_page_file_short_read_fills_page(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 256;          // each dev->read returns <=256 bytes (partial Rread)

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    const u64 FILE_OFF = 0x6000;
    struct Burrow *v = burrow_create_file(s, FILE_OFF, ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RX");

    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/true);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "FILE demand-page resolves despite partial reads");

    struct page *slotpg = burrow_file_slot_for_test(v, 0);
    TEST_ASSERT(slotpg != NULL, "slot 0 resident");
    u8 *bytes = (u8 *)pa_to_kva(page_to_pa(slotpg));
    TEST_EXPECT_EQ((u64)bytes[0],            (u64)(u8)(FILE_OFF & 0xff),
        "first byte = file_offset pattern");
    // byte 257 (NOT 256): the pattern at FILE_OFF+256 is (0x6000+256)&0xff == 0x00,
    // which coincides with the zero-fill -> a vacuous assertion. FILE_OFF+257 ->
    // 0x01, a NON-ZERO value that genuinely distinguishes file-data from a
    // zero-filled tail (R-5 R2 audit F1).
    TEST_EXPECT_EQ((u64)bytes[257],          (u64)(u8)((FILE_OFF + 257) & 0xff),
        "byte 257 (PAST the first short read, non-zero pattern) is file data, NOT a zero-filled tail");
    TEST_EXPECT_EQ((u64)bytes[2000],         (u64)(u8)((FILE_OFF + 2000) & 0xff),
        "mid-page byte is file data");
    TEST_EXPECT_EQ((u64)bytes[PAGE_SIZE - 1], (u64)(u8)((FILE_OFF + PAGE_SIZE - 1) & 0xff),
        "LAST byte is file data, NOT a zero-filled tail (the SA-F1 corruption)");
    TEST_EXPECT_EQ(g_rev_read_calls, (int)(PAGE_SIZE / 256),
        "the slow path looped over the short reads to fill the whole page");

    g_rev_read_chunk = 0;            // reset (self-contained; siblings run with 0)
    drop_proc(p);
    burrow_unref(v);
}

// Read-ahead audit F1: the CLUSTER short-read loop -- the SA-F1 recurrence
// surface. The single-path short-read regression above uses a 1-page Burrow,
// which degrades (ncluster <= 1) to the single path, so the cluster's OWN
// loop (one batched read filled by partial Rreads) was unpinned. A multi-page
// Burrow + a 256-byte read cap forces the cluster loop to iterate; assert the
// faulted slot AND the cluster-mates carry per-slot file bytes to their LAST
// byte (a broken loop leaves a KP_ZERO tail somewhere in the batch), and that
// the call count equals resident_span/chunk (proving the loop, without
// coupling to the REVENANT_READAHEAD_PAGES constant).
void test_demand_page_file_cluster_short_read(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 256;
    g_rev_read_eof   = -1;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    const u64    FILE_OFF = 0x8000;
    const size_t NPAGES   = 200;                    // > one read-ahead cluster
    const u64    SPAN     = (u64)NPAGES * ONE_PAGE;
    struct Burrow *v = burrow_create_file(s, FILE_OFF, SPAN, true);
    TEST_ASSERT(v != NULL, "burrow_create_file 200-page");
    int rc = burrow_map(p, v, USER_VA, SPAN, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map 200-page FILE RX");

    struct fault_info fi;
    make_fi(&fi, USER_VA + 2 * ONE_PAGE + 0x10, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED,
        "cluster fault resolves despite partial reads");

    // Count the resident prefix (the cluster) without assuming its size.
    size_t resident = 0;
    while (resident < NPAGES && burrow_file_slot_for_test(v, resident) != NULL)
        resident++;
    TEST_ASSERT(resident >= 3, "the cluster spans at least the faulted slot's neighborhood");
    TEST_ASSERT(resident < NPAGES, "read-ahead stays bounded under short reads");

    // Every resident slot's FIRST and LAST byte carries the per-slot file
    // pattern -- a broken batched-read loop strands a zero tail in the batch.
    // (Pattern bytes at these offsets are non-zero for FILE_OFF = 0x8000:
    // first byte of slot k = (0x8000 + k*0x1000) & 0xff = 0x00 -- VACUOUS, the
    // R-5 R2-F1 lesson -- so check byte 1 [0x01] and the last byte [0xff].)
    for (size_t k = 0; k < resident; k++) {
        u8 *b = (u8 *)pa_to_kva(page_to_pa(burrow_file_slot_for_test(v, k)));
        TEST_EXPECT_EQ((u64)b[1],
            (u64)(u8)((FILE_OFF + k * ONE_PAGE + 1) & 0xff),
            "cluster slot byte 1 is file data");
        TEST_EXPECT_EQ((u64)b[PAGE_SIZE - 1],
            (u64)(u8)((FILE_OFF + k * ONE_PAGE + PAGE_SIZE - 1) & 0xff),
            "cluster slot LAST byte is file data (no zero tail in the batch)");
    }

    // The batched read looped: one call per 256-byte chunk of the resident span.
    TEST_EXPECT_EQ(g_rev_read_calls, (int)(resident * (PAGE_SIZE / 256)),
        "the cluster loop iterated over every partial Rread");

    g_rev_read_chunk = 0;
    drop_proc(p);
    burrow_unref(v);
}

// Read-ahead audit F2: the EOF (n == 0) zero-tail arm, for BOTH fill paths.
// The stub's EOF mode ends the backing file mid-cluster: pre-EOF bytes carry
// the file pattern, the post-EOF remainder of the batch stays KP_ZERO (the
// .bss-style fill), and slots wholly past EOF are adopted as zero pages --
// byte-identical to N sequential single-page reads. Part (b) drives the
// SAME arm on the single path (1-page Burrow, EOF mid-page).
void test_demand_page_file_eof_tail_zero(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // (a) Cluster: an 8-page Burrow whose backing file ends mid-slot-2.
    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    const u64    FILE_OFF = 0xA000;
    const size_t NPAGES   = 8;
    const u64    EOF_OFF  = FILE_OFF + 2 * ONE_PAGE + 0x800;   // mid slot 2
    g_rev_read_eof = (s64)EOF_OFF;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, FILE_OFF, (u64)NPAGES * ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file 8-page");
    int rc = burrow_map(p, v, USER_VA, (u64)NPAGES * ONE_PAGE, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map 8-page FILE RX");

    struct fault_info fi;
    make_fi(&fi, USER_VA + ONE_PAGE + 0x10, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED,
        "cluster fault resolves across the EOF boundary");

    // Slot 1 (wholly pre-EOF): full file data to the last byte.
    struct page *pg1 = burrow_file_slot_for_test(v, 1);
    TEST_ASSERT(pg1 != NULL, "slot 1 resident");
    u8 *b1 = (u8 *)pa_to_kva(page_to_pa(pg1));
    TEST_EXPECT_EQ((u64)b1[PAGE_SIZE - 1],
        (u64)(u8)((FILE_OFF + ONE_PAGE + PAGE_SIZE - 1) & 0xff),
        "pre-EOF slot 1 last byte is file data");

    // Slot 2 (straddles EOF): file data before, ZERO after. The pre-EOF probe
    // byte 0x7ff has pattern 0xff (non-zero, non-vacuous); the post-EOF probe
    // byte 0x801 WOULD be 0x01 if the fill overran EOF -- assert it is 0.
    struct page *pg2 = burrow_file_slot_for_test(v, 2);
    TEST_ASSERT(pg2 != NULL, "slot 2 (EOF-straddling) resident");
    u8 *b2 = (u8 *)pa_to_kva(page_to_pa(pg2));
    TEST_EXPECT_EQ((u64)b2[0x7ff],
        (u64)(u8)((FILE_OFF + 2 * ONE_PAGE + 0x7ff) & 0xff),
        "slot 2 pre-EOF byte is file data");
    TEST_EXPECT_EQ((u64)b2[0x801], 0ull, "slot 2 post-EOF byte is ZERO (bss tail)");
    TEST_EXPECT_EQ((u64)b2[PAGE_SIZE - 1], 0ull, "slot 2 last byte is ZERO");

    // A slot wholly past EOF is a zero page (identical to the single path's
    // n==0 -> KP_ZERO semantics for that slot).
    struct page *pg4 = burrow_file_slot_for_test(v, 4);
    if (pg4 != NULL) {
        u8 *b4 = (u8 *)pa_to_kva(page_to_pa(pg4));
        TEST_EXPECT_EQ((u64)b4[0], 0ull, "post-EOF slot is a zero page");
        TEST_EXPECT_EQ((u64)b4[PAGE_SIZE - 1], 0ull, "post-EOF slot tail is zero");
    }

    drop_proc(p);
    burrow_unref(v);

    // (b) Single path: a 1-page Burrow (degrades to file_demand_page_single)
    // whose file ends mid-page -- the n==0 break leaves the KP_ZERO tail.
    p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed (part b)");
    g_rev_read_calls = 0;
    const u64 FILE_OFF_B = 0xC000;
    g_rev_read_eof = (s64)(FILE_OFF_B + 0x800);                // mid-page EOF

    struct Spoor *s2 = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s2 != NULL, "spoor_alloc (part b)");
    struct Burrow *v2 = burrow_create_file(s2, FILE_OFF_B, ONE_PAGE, true);
    TEST_ASSERT(v2 != NULL, "burrow_create_file 1-page");
    rc = burrow_map(p, v2, USER_VA, ONE_PAGE, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map 1-page FILE RX");

    make_fi(&fi, USER_VA + 0x10, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED,
        "single-path fault resolves at EOF");
    struct page *pgb = burrow_file_slot_for_test(v2, 0);
    TEST_ASSERT(pgb != NULL, "slot 0 resident (part b)");
    u8 *bb = (u8 *)pa_to_kva(page_to_pa(pgb));
    TEST_EXPECT_EQ((u64)bb[0x7ff],
        (u64)(u8)((FILE_OFF_B + 0x7ff) & 0xff),
        "single-path pre-EOF byte is file data");
    TEST_EXPECT_EQ((u64)bb[0x801], 0ull, "single-path post-EOF byte is ZERO");
    TEST_EXPECT_EQ((u64)bb[PAGE_SIZE - 1], 0ull, "single-path last byte is ZERO (bss tail)");
    TEST_EXPECT_EQ(g_rev_read_calls, 2,
        "single path: one clamped read + one 0-return (true EOF) ended the loop");

    g_rev_read_eof = -1;             // reset (self-contained; siblings run without EOF)
    drop_proc(p);
    burrow_unref(v2);
}

// =============================================================================
// Overcommit / I-32 (ARCH section 6.5): the BURROW_TYPE_ANON_LAZY demand-zero arm.
//
//   demand_page.lazy_zero_fill
//     A lazy attach commits no pages; the first touch demand-zeroes one page,
//     installs it RW/XN (W^X-clean), and charges page_count at FAULT time. A
//     re-fault hits resident -- no double-charge.
//   demand_page.lazy_decommit_refault
//     burrow_decommit releases the resident pages (PTE cleared, page freed,
//     page_count uncharged) but keeps the VMA; a later touch re-faults a fresh
//     zero page + re-charges. No page leak across the whole cycle.
//   demand_page.lazy_charge_on_fault_oom
//     An over-budget commit fails the fault (FAULT_UNHANDLED_USER --
//     graceful per-Proc terminate, NEVER an extinction) with no page committed +
//     page_count unchanged (the charge rolled back).
//   demand_page.lazy_detach_uncharges_resident
//     SYS_BURROW_DETACH on a lazy region uncharges ONLY the resident pages (the
//     lazy region charged per-fault, not the whole reservation span).
// =============================================================================

void test_demand_page_lazy_zero_fill(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon_lazy(FOUR_PAGES);
    TEST_ASSERT(v != NULL, "burrow_create_anon_lazy failed");
    int rc = burrow_map(p, v, USER_VA, FOUR_PAGES, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map RW lazy");

    // No page committed yet: slot empty + page_count 0 (the free reservation).
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 0) == NULL, "slot 0 starts empty (lazy)");
    TEST_EXPECT_EQ((u64)p->as->page_count, 0ull, "lazy attach charges no pages");

    // Write-fault page 0 -> demand-zero install + charge-on-fault.
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x100, /*is_write=*/true, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "lazy demand-page resolves a mapped VA");

    struct page *slot = burrow_lazy_slot_for_test(v, 0);
    TEST_ASSERT(slot != NULL, "slot 0 resident after fault");
    TEST_EXPECT_EQ((u64)(p->as->page_count - p->as->pgtable_pages), 1ull,
                   "charge-on-fault: one data page charged (its tables aside)");
    u8 *bytes = (u8 *)pa_to_kva(page_to_pa(slot));
    TEST_EXPECT_EQ((u64)bytes[0], 0ull,             "demand-zero: byte 0 is zero");
    TEST_EXPECT_EQ((u64)bytes[0x800], 0ull,         "demand-zero: mid byte is zero");
    TEST_EXPECT_EQ((u64)bytes[PAGE_SIZE - 1], 0ull, "demand-zero: last byte is zero");

    // PTE is RW/XN (W^X-clean: writable, never executable).
    u64 pte = walk_to_l3_entry(p->as->pgtable_root, USER_VA);
    TEST_ASSERT(pte != 0, "L3 PTE installed after lazy demand-page");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(slot), "PTE PA = the resident slot page");
    TEST_ASSERT((pte & BIT_PXN) != 0, "lazy page PXN (kernel never execs user)");
    TEST_ASSERT((pte & BIT_UXN) != 0, "lazy page UXN (W^X: never executable)");
    TEST_EXPECT_EQ(pte & BIT_AP_FIELD, BIT_AP_RW_ANY, "lazy page RW (AP_RW_ANY)");

    // A second fault to the same page hits resident -- no double-charge.
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/false);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "second fault to resident lazy page resolves");
    TEST_EXPECT_EQ((u64)(p->as->page_count - p->as->pgtable_pages), 1ull, "re-fault does NOT double-charge (still 1)");

    // Other slots stay sparse (only the faulted page committed).
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 1) == NULL, "slot 1 sparse");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 3) == NULL, "slot 3 sparse");

    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_lazy_decommit_refault(void) {
    u64 free_before = test_free_pages_settled();
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon_lazy(FOUR_PAGES);
    TEST_ASSERT(v != NULL, "burrow_create_anon_lazy");
    int rc = burrow_map(p, v, USER_VA, FOUR_PAGES, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map lazy");

    // Fault pages 0,1,2 (leave 3 sparse) -> page_count 3.
    for (int i = 0; i < 3; i++) {
        struct fault_info fi;
        make_fi(&fi, USER_VA + (u64)i * ONE_PAGE + 0x10, /*is_write=*/true, /*is_instr=*/false);
        TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "fault page i");
    }
    TEST_EXPECT_EQ((u64)(p->as->page_count - p->as->pgtable_pages), 3ull, "3 faulted pages charged");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 2) != NULL, "slot 2 resident");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 3) == NULL, "slot 3 sparse");

    // Decommit the whole 4-page range (under vma_lock, like the syscall path).
    spin_lock(&p->as->lock);
    int drc = burrow_decommit(p, USER_VA, FOUR_PAGES);
    spin_unlock(&p->as->lock);
    TEST_EXPECT_EQ(drc, 0, "decommit succeeds");
    TEST_EXPECT_EQ((u64)p->as->page_count, 0ull, "decommit uncharges the 3 resident pages");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 0) == NULL, "slot 0 released");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 2) == NULL, "slot 2 released");
    TEST_ASSERT(walk_to_l3_entry(p->as->pgtable_root, USER_VA) == 0, "PTE cleared after decommit");

    // Re-fault page 0 -> a FRESH zero page, re-charged.
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x20, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "re-fault after decommit");
    TEST_EXPECT_EQ((u64)(p->as->page_count - p->as->pgtable_pages), 1ull, "re-fault re-charges (1)");
    struct page *slot = burrow_lazy_slot_for_test(v, 0);
    TEST_ASSERT(slot != NULL, "slot 0 re-resident");
    TEST_EXPECT_EQ((u64)((u8 *)pa_to_kva(page_to_pa(slot)))[0], 0ull, "re-faulted page is fresh-zero");

    drop_proc(p);
    burrow_unref(v);
    TEST_EXPECT_EQ(test_free_pages_settled(), free_before,
        "lazy fault + decommit + re-fault + teardown leaks no pages "
        "(sparse pages freed; sub-tables freed by proc_pgtable_destroy)");
}

void test_demand_page_lazy_charge_on_fault_oom(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    // A proc_alloc'd Proc is principal_id 0 (PRINCIPAL_INVALID) -> NOT exempt, so the
    // I-32 page cap applies. Park page_count AT the cap so the next commit fails.
    p->as->page_count = p->as->page_budget;

    struct Burrow *v = burrow_create_anon_lazy(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon_lazy");
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map lazy");

    // The fault tries to commit page #(budget+1) -> proc_page_charge refuses
    // -> graceful per-Proc terminate (FAULT_UNHANDLED_USER), NEVER an extinction.
    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/true, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER, "over-cap commit fails the fault (graceful OOM)");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 0) == NULL, "no page committed on a cap-hit");
    TEST_ASSERT(walk_to_l3_entry(p->as->pgtable_root, USER_VA) == 0, "no PTE on a cap-hit");
    TEST_EXPECT_EQ((u64)p->as->page_count, (u64)p->as->page_budget, "page_count unchanged (charge rolled back)");

    p->as->page_count = 0;     // reset (hygiene; drop_proc doesn't touch page_count)
    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_lazy_detach_uncharges_resident(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    // Lazy-attach a 4-page region via the syscall path (commits nothing).
    s64 r = sys_burrow_attach_lazy_for_proc(p, FOUR_PAGES);
    TEST_ASSERT(r > 0, "lazy attach failed");
    u64 va = (u64)r;
    TEST_EXPECT_EQ((u64)p->as->page_count, 0ull, "lazy attach commits nothing");

    // Fault 2 of the 4 pages resident -> page_count 2.
    for (int i = 0; i < 2; i++) {
        struct fault_info fi;
        make_fi(&fi, va + (u64)i * ONE_PAGE + 0x8, /*is_write=*/true, /*is_instr=*/false);
        TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "fault lazy page i");
    }
    TEST_EXPECT_EQ((u64)(p->as->page_count - p->as->pgtable_pages), 2ull, "2 resident pages charged");

    // Detach the whole region. The uncharge must be ONLY the 2 RESIDENT pages -- the
    // lazy region charged per-fault, NOT the whole 4-page span (the eager detach
    // would over-uncharge by 2 if it used length/PAGE_SIZE here).
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, FOUR_PAGES), 0, "detach lazy region");
    TEST_EXPECT_EQ((u64)p->as->page_count, 0ull,
        "detach uncharges exactly the 2 resident pages (NOT the whole 4-page span)");
    TEST_ASSERT(vma_lookup(p, va) == NULL, "VMA gone after detach");

    drop_proc(p);
}

// #197: mmap_eager_copy must return its I-32 page charge on EVERY failure past
// the populate. burrow_lazy_populate charges the whole run on SUCCESS and the
// caller owns it from then on -- nothing in the Burrow free path gives it back
// (grep addrspace_uncharge_pages: only populate's own failure arm and
// burrow_decommit). Without the unwind, as->page_count inflates permanently
// while the pages themselves are freed, so it stops meaning true RSS
// (ARCH section 6.5) and a guest that can drive a read error walks its own
// counter to its budget.
//
// The POSITIVE control comes first and is what stops this passing vacuously: a
// test that only watched the failure path would pass identically against a
// mmap_eager_copy that charged nothing at all.
void test_mmap_eager_copy_charge_pairing(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    const u64 LEN    = 4 * PAGE_SIZE;
    const u32 NPAGES = 4;

    g_rev_read_fail  = false;
    g_rev_read_eof   = -1;
    g_rev_read_chunk = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");

    u32 before = p->as->page_count;

    // CONTROL: a SUCCEEDING copy charges, and HOLDS the charge -- the caller
    // (or the mapping that adopts the Burrow) owns it now.
    struct Burrow *ok = mmap_eager_copy_for_test(p->as, false, s, 0, LEN);
    TEST_ASSERT(ok != NULL, "a clean eager copy must succeed");
    TEST_EXPECT_EQ(p->as->page_count, before + NPAGES,
                   "a successful copy CHARGES every page it populated");
    // Dropping the Burrow frees the pages but NOT the counter -- that asymmetry
    // is the whole reason the unwind has to be explicit, so assert it rather
    // than leave it as an assumption.
    burrow_unref(ok);
    TEST_EXPECT_EQ(p->as->page_count, before + NPAGES,
                   "burrow_unref frees pages but does NOT uncharge");
    // Hand the control's charge back so the failure case starts from `before`.
    spin_lock(&p->as->lock);
    addrspace_uncharge_pages(p->as, NPAGES);
    spin_unlock(&p->as->lock);
    TEST_EXPECT_EQ(p->as->page_count, before, "control unwound");

    // THE REGRESSION: a read error partway through must leave the counter
    // exactly where it started.
    g_rev_read_fail = true;
    struct Burrow *bad = mmap_eager_copy_for_test(p->as, false, s, 0, LEN);
    g_rev_read_fail = false;
    TEST_ASSERT(bad == NULL, "a failing read must fail the copy");
    TEST_EXPECT_EQ(p->as->page_count, before,
                   "a FAILED eager copy returns its whole page charge (#197)");

    spoor_clunk(s);
    drop_proc(p);
}

// =============================================================================
// #194 (D-3c): the file_limit bound -- Linux SIGBUS past the last file page.
//
// A FILE Burrow stamped with a KNOWN backing size must REFUSE to demand-page a
// page wholly past round_up(file_limit): no page allocated, no read issued, the
// fault answers FAULT_USER_BUS (snare:bus). Without the bound (the pre-#194
// state, preserved under BURROW_FILE_LIMIT_UNKNOWN -- see file_eof_tail_zero
// above, which pins THAT posture), every such fault minted an uncharged
// demand-zero page: anonymous in effect, accounted as FILE, i.e. not at all --
// an I-32 bypass bounded only by BURROW_ATTACH_MAX x PROC_VMA_MAX.
// =============================================================================

void test_demand_page_file_past_limit_bus(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    g_rev_read_eof   = (s64)ONE_PAGE;        // the "file" is exactly one page

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, 4 * ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file 4-page");
    v->file_limit = ONE_PAGE;                // the image.c stamp, done by hand

    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 4 * ONE_PAGE, VMA_PROT_RX), 0,
                   "burrow_map 4-page FILE RX");

    // Slot 0 holds file bytes: serves normally. (The cluster clamp bounds the
    // fill to slot_lim=1, so this also proves a clamped 1-slot cluster
    // degrades to the single path and still makes progress.)
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED,
                   "in-file slot 0 serves under a known limit");
    struct page *pg0 = burrow_file_slot_for_test(v, 0);
    TEST_ASSERT(pg0 != NULL, "slot 0 resident");
    u8 *b0 = (u8 *)pa_to_kva(page_to_pa(pg0));
    TEST_EXPECT_EQ((u64)b0[5], 5ull, "slot 0 carries file bytes (pattern)");

    // Slot 2 is WHOLLY past the file: refused before any allocation or read.
    int calls_before = g_rev_read_calls;
    make_fi(&fi, USER_VA + 2 * ONE_PAGE + 0x40, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_USER_BUS,
                   "wholly-past-limit page answers BUS (Linux SIGBUS past EOF)");
    TEST_ASSERT(burrow_file_slot_for_test(v, 2) == NULL,
                "no page minted for the refused slot");
    TEST_EXPECT_EQ(g_rev_read_calls, calls_before,
                   "refused BEFORE any read was issued");

    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_file_partial_page_zero_with_limit(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    const u64 LIMIT  = ONE_PAGE + 0x10;      // 16 bytes into page 1
    g_rev_read_eof   = (s64)LIMIT;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, 4 * ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file 4-page");
    v->file_limit = LIMIT;

    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 4 * ONE_PAGE, VMA_PROT_RX), 0,
                   "burrow_map 4-page FILE RX");

    // Slot 1 STRADDLES the limit (holds 16 file bytes): admitted, data then
    // zero -- the read-short path, byte-identical to Linux's partial last page.
    struct fault_info fi;
    make_fi(&fi, USER_VA + ONE_PAGE + 0x8, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED,
                   "the partial final page is admitted under the limit");
    struct page *pg1 = burrow_file_slot_for_test(v, 1);
    TEST_ASSERT(pg1 != NULL, "slot 1 resident");
    u8 *b1 = (u8 *)pa_to_kva(page_to_pa(pg1));
    TEST_EXPECT_EQ((u64)b1[0xf], (u64)(u8)((ONE_PAGE + 0xf) & 0xff),
                   "pre-EOF byte 15 is file data");
    TEST_EXPECT_EQ((u64)b1[0x10], 0ull, "byte 16 (EOF) is ZERO");
    TEST_EXPECT_EQ((u64)b1[PAGE_SIZE - 1], 0ull, "page tail stays ZERO");

    // Slot 2 (the first page with NO file byte) refuses.
    make_fi(&fi, USER_VA + 2 * ONE_PAGE, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_USER_BUS,
                   "first wholly-past page refuses");

    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_file_cluster_clamps_at_limit(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;
    g_rev_read_chunk = 0;
    const u64 LIMIT  = 3 * ONE_PAGE;         // exactly three pages of data
    g_rev_read_eof   = (s64)LIMIT;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, 8 * ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file 8-page");
    v->file_limit = LIMIT;

    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 8 * ONE_PAGE, VMA_PROT_RX), 0,
                   "burrow_map 8-page FILE RX");

    // Fault slot 0: read-ahead runs, CLAMPED to the 3 in-file slots. Without
    // the clamp, slots 3..7 would be pre-filled as resident ZERO pages -- and
    // the resident fast path installs those with NO limit check, so the mint
    // would return through the side door. The two assertions below redden in
    // exactly that revert.
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED,
                   "clamped cluster fault serves the in-file slots");
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) != NULL, "slot 0 resident");
    TEST_ASSERT(burrow_file_slot_for_test(v, 2) != NULL,
                "read-ahead reached the last in-file slot");
    TEST_ASSERT(burrow_file_slot_for_test(v, 3) == NULL,
                "read-ahead did NOT pre-fill past the limit");
    TEST_ASSERT(burrow_file_slot_for_test(v, 7) == NULL,
                "the cluster tail stays empty past the limit");

    // The past-limit slot then refuses on its own fault (it cannot ride the
    // fast path, because nothing resident exists for it).
    make_fi(&fi, USER_VA + 3 * ONE_PAGE, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_USER_BUS,
                   "past-limit slot refuses after the clamped cluster");

    drop_proc(p);
    burrow_unref(v);
}


// =============================================================================
// B-1a' audit F8: FILE pages are the Image cache's; their MAPPINGS are the
// holder's, and an idle image is the pool's reserve under pressure.
// =============================================================================

// Each leaf the fault installs over a FILE mapping is charged to the mapping
// space (page_count and file_pages); a re-fault charges nothing; the detach
// refunds per leaf cleared while the pages stay resident in the Burrow.
void test_demand_page_file_pages_charge_the_holder(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, 2 * ONE_PAGE, true);
    TEST_ASSERT(v != NULL, "burrow_create_file");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_RX), 0, "burrow_map RX");

    const u32 data0 = p->as->page_count - p->as->pgtable_pages;
    TEST_EXPECT_EQ(p->as->file_pages, 0u, "no file page charged before a fault");

    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 0 faults in");
    TEST_EXPECT_EQ(p->as->page_count - p->as->pgtable_pages, data0 + 1, "one file page charged to the holder");
    TEST_EXPECT_EQ(p->as->file_pages, 1u, "file: 1");

    make_fi(&fi, USER_VA + ONE_PAGE, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 1 faults in");
    TEST_EXPECT_EQ(p->as->page_count - p->as->pgtable_pages, data0 + 2, "two file pages charged");
    TEST_EXPECT_EQ(p->as->file_pages, 2u, "file: 2");

    // A re-fault on a mapped page charges nothing more: the install finds the
    // leaf already there (1) and the charge taken for it goes back.
    make_fi(&fi, USER_VA + 0x100, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 0 re-faults");
    TEST_EXPECT_EQ(p->as->page_count - p->as->pgtable_pages, data0 + 2, "a re-fault charges nothing");
    TEST_EXPECT_EQ(p->as->file_pages, 2u, "file: still 2");

    // The detach clears both leaves and refunds both; the pages stay the
    // Burrow's (resident: 2) -- the cache's posture, not the holder's.
    struct Burrow *dead = NULL;
    spin_lock(&p->as->lock);
    int drc = vma_detach_range_in(p->as, false, NULL, USER_VA, 2 * ONE_PAGE, 0, &dead);
    spin_unlock(&p->as->lock);
    burrow_free_deferred(dead);
    TEST_EXPECT_EQ(drc, 0, "detach (in-kernel: USER_VA sits below the burrow window the syscall form admits)");
    TEST_EXPECT_EQ(p->as->page_count - p->as->pgtable_pages, data0, "the holder charge is refunded per leaf");
    TEST_EXPECT_EQ(p->as->file_pages, 0u, "file: 0");
    TEST_EXPECT_EQ(burrow_image_resident_count(v), 2u, "the pages stay resident in the Burrow");

    // The refund follows the LEAVES cleared, not the span detached (audit F16):
    // map the image again, fault ONE page, and detach the unfaulted half first
    // -- a refund by span would take back a charge that was never made.
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_RX), 0, "burrow_map RX again");
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 0 faults in (a resident hit)");
    TEST_EXPECT_EQ(p->as->page_count - p->as->pgtable_pages, data0 + 1, "one leaf, one charge");
    TEST_EXPECT_EQ(p->as->file_pages, 1u, "file: 1");
    dead = NULL;
    spin_lock(&p->as->lock);
    drc = vma_detach_range_in(p->as, false, NULL, USER_VA + ONE_PAGE, ONE_PAGE, 0, &dead);
    spin_unlock(&p->as->lock);
    burrow_free_deferred(dead);
    TEST_EXPECT_EQ(drc, 0, "detach the UNFAULTED page");
    TEST_EXPECT_EQ(p->as->page_count - p->as->pgtable_pages, data0 + 1,
                   "no leaf was cleared, so nothing is refunded (a refund by span would be)");
    TEST_EXPECT_EQ(p->as->file_pages, 1u, "file: still 1");
    dead = NULL;
    spin_lock(&p->as->lock);
    drc = vma_detach_range_in(p->as, false, NULL, USER_VA, ONE_PAGE, 0, &dead);
    spin_unlock(&p->as->lock);
    burrow_free_deferred(dead);
    TEST_EXPECT_EQ(drc, 0, "detach the faulted page");
    TEST_EXPECT_EQ(p->as->page_count - p->as->pgtable_pages, data0, "one leaf cleared, one refund");
    TEST_EXPECT_EQ(p->as->file_pages, 0u, "file: 0");

    drop_proc(p);
    burrow_unref(v);
}

// =============================================================================
// Vote 118: a private image's pages are its payer's (ARCH 25.4, the REVENANT
// private-page charge addendum). Each test collects what it reads and releases
// every reference BEFORE its first assertion: an assertion returns, and a ref
// stranded by it would leave the next test a Burrow it did not make.
// =============================================================================

// A server that does not version its content: its files map privately.
static struct Dev g_rev_private_dev = {
    .dc   = '?',
    .name = "revpriv",
    .read = rev_test_read,
};

#define V118_PAGES 128u                   // two read-ahead clusters
static u32 v118_data(struct AddrSpace *as) { return as->page_count - as->pgtable_pages; }

// A fresh space's data count is 0, and a refund past 0 clamps there, so a
// baseline of 0 cannot show an over-refund. These pages lift it off the floor.
#define V118_FLOOR 4u
static bool v118_floor(struct AddrSpace *as) { return addrspace_charge_pages(as, V118_FLOOR, false); }
static void v118_unfloor(struct AddrSpace *as, bool floored) {
    if (floored) addrspace_uncharge_pages(as, V118_FLOOR);
}

static struct Burrow *v118_private_image(struct AddrSpace *payer) {
    struct Spoor *s = spoor_alloc(&g_rev_private_dev);
    if (!s) return NULL;
    struct Burrow *v = image_lookup_or_create(s, 0, V118_PAGES * ONE_PAGE, /*exec=*/false,
                                              BURROW_FILE_LIMIT_UNKNOWN, payer, false);
    if (!v) spoor_clunk(s);
    return v;
}

static enum fault_result v118_fault(struct Proc *p, u64 slot) {
    struct fault_info fi;
    make_fi(&fi, USER_VA + slot * ONE_PAGE, /*is_write=*/false, /*is_instr=*/false);
    return userland_demand_page(p, &fi);
}

static int v118_detach(struct Proc *p, u64 slot, u64 npages) {
    struct Burrow *dead = NULL;
    spin_lock(&p->as->lock);
    int rc = vma_detach_range_in(p->as, false, NULL, USER_VA + slot * ONE_PAGE,
                                 npages * ONE_PAGE, 0, &dead);
    spin_unlock(&p->as->lock);
    burrow_free_deferred(dead);
    return rc;
}

// The payer is charged every page a fault mints -- the faulting page and its
// read-ahead -- and nothing for its own leaves; a partial detach leaves the
// pages resident and paid; the free refunds the record whole.
void test_demand_page_file_private_charges_its_payer(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Burrow *v = v118_private_image(p->as);
    if (!v) { drop_proc(p); TEST_ASSERT(false, "a private image"); }

    bool payer_set = burrow_file_payer_for_test(v) == p->as;
    int  mrc       = burrow_map(p, v, USER_VA, V118_PAGES * ONE_PAGE, VMA_PROT_READ);
    struct Vma *vma = vma_lookup(p, USER_VA);
    bool flagged   = vma && (vma->flags & VMA_FLAG_FILE_PAYER);
    burrow_unref(v);                      // the mapping holds it now
    const bool floored = v118_floor(p->as);
    const u32 d0 = v118_data(p->as);
    const u32 f0 = p->as->file_pages;

    enum fault_result r0 = v118_fault(p, 0);
    u32 res1 = burrow_image_resident_count(v), rec1 = burrow_file_charge_pages_for_test(v);
    u32 d1 = v118_data(p->as), f1 = p->as->file_pages;
    enum fault_result r1 = v118_fault(p, 1);          // a resident hit
    u32 d2 = v118_data(p->as), f2 = p->as->file_pages;
    int drc1 = v118_detach(p, 0, 1);                   // a payer leaf: no refund
    u32 d3 = v118_data(p->as), rec3 = burrow_file_charge_pages_for_test(v);
    int drc2 = v118_detach(p, 1, V118_PAGES - 1);      // the last mapping: the free
    u32 d4 = v118_data(p->as);
    v118_unfloor(p->as, floored);
    drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_ASSERT(floored, "the floor under the payer's count");
    TEST_ASSERT(payer_set, "the private arm records the mapper's space as the payer");
    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_ASSERT(flagged, "a fresh mapping into the payer's space is a payer mapping");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "slot 0 faults in");
    TEST_EXPECT_EQ(res1, 64u, "the cluster minted 64 pages");
    TEST_EXPECT_EQ(rec1, 64u, "the record is the resident count");
    TEST_EXPECT_EQ(d1, d0 + 64u, "the payer paid every minted page and no leaf");
    TEST_EXPECT_EQ(f1, f0, "no file leaf was charged to the payer");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "slot 1 resolves on a resident hit");
    TEST_EXPECT_EQ(d2, d1, "a payer's resident hit charges nothing");
    TEST_EXPECT_EQ(f2, f0, "and no leaf");
    TEST_EXPECT_EQ(drc1, 0, "detach slot 0");
    TEST_EXPECT_EQ(d3, d1, "a partial detach refunds nothing: the pages stay resident and paid");
    TEST_EXPECT_EQ(rec3, 64u, "the record is unchanged");
    TEST_EXPECT_EQ(drc2, 0, "detach the rest");
    TEST_EXPECT_EQ(d4, d0, "the free refunded the record whole");
}

// A one-page image has no read-ahead: the single fill charges its one mint
// to the payer, and its leaf nothing.
void test_demand_page_file_private_single_fill_charges(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Spoor *s = spoor_alloc(&g_rev_private_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = image_lookup_or_create(s, 0, ONE_PAGE, /*exec=*/false,
                                              BURROW_FILE_LIMIT_UNKNOWN, p->as, false);
    if (!v) { spoor_clunk(s); drop_proc(p); TEST_ASSERT(false, "a one-page private image"); }
    bool payer_set = burrow_file_payer_for_test(v) == p->as;
    int  mrc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_READ);
    burrow_unref(v);
    const bool floored = v118_floor(p->as);
    const u32 d0 = v118_data(p->as), f0 = p->as->file_pages;
    int calls0 = g_rev_read_calls;
    enum fault_result r0 = v118_fault(p, 0);
    int reads = g_rev_read_calls - calls0;
    u32 res = burrow_image_resident_count(v), rec = burrow_file_charge_pages_for_test(v);
    u32 d1 = v118_data(p->as), f1 = p->as->file_pages;
    int drc = v118_detach(p, 0, 1);
    u32 d2 = v118_data(p->as);
    v118_unfloor(p->as, floored);
    drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_ASSERT(floored, "the floor under the payer's count");
    TEST_ASSERT(payer_set, "the private arm records the mapper's space as the payer");
    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "the page faults in");
    TEST_EXPECT_EQ(reads, 1, "one read: the single fill, not a cluster");
    TEST_EXPECT_EQ(res, 1u, "one page resident");
    TEST_EXPECT_EQ(rec, 1u, "the record holds it");
    TEST_EXPECT_EQ(d1, d0 + 1u, "the payer paid the mint, and no leaf");
    TEST_EXPECT_EQ(f1, f0, "no file leaf");
    TEST_EXPECT_EQ(drc, 0, "detach");
    TEST_EXPECT_EQ(d2, d0, "the free refunded it");
}

// The read-ahead is cut to the payer's headroom, restarting at the faulting
// slot; with no headroom the fault is refused and nothing is minted.
void test_demand_page_file_private_readahead_clamps_to_headroom(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Burrow *v = v118_private_image(p->as);
    if (!v) { drop_proc(p); TEST_ASSERT(false, "a private image"); }
    int mrc = burrow_map(p, v, USER_VA, V118_PAGES * ONE_PAGE, VMA_PROT_READ);
    burrow_unref(v);

    enum fault_result r0 = v118_fault(p, 0);         // builds the tables, mints 0..63
    const u32 budget0 = p->as->page_budget;
    const u32 tables0 = p->as->pgtable_pages;
    p->as->page_budget = p->as->page_count + 5u;     // five pages of headroom
    enum fault_result r1 = v118_fault(p, 80);        // unaligned: its cluster starts at 64
    u32 res1 = burrow_image_resident_count(v), rec1 = burrow_file_charge_pages_for_test(v);
    bool win_starts_at_fault = burrow_file_slot_for_test(v, 64) == NULL &&
                               burrow_file_slot_for_test(v, 79) == NULL &&
                               burrow_file_slot_for_test(v, 80) != NULL &&
                               burrow_file_slot_for_test(v, 84) != NULL &&
                               burrow_file_slot_for_test(v, 85) == NULL;
    bool at_cap = p->as->page_count == p->as->page_budget;
    u32 tables1 = p->as->pgtable_pages;
    enum fault_result r2 = v118_fault(p, 90);         // no headroom left
    u32 res2 = burrow_image_resident_count(v), rec2 = burrow_file_charge_pages_for_test(v);
    p->as->page_budget = budget0;
    // A cluster over a partly resident window: 80..84 are there, so 59 mint.
    const u32 d3a = v118_data(p->as);
    enum fault_result r3 = v118_fault(p, 100);
    u32 res3 = burrow_image_resident_count(v), rec3 = burrow_file_charge_pages_for_test(v);
    u32 d3 = v118_data(p->as);
    int drc = v118_detach(p, 0, V118_PAGES);
    drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "slot 0 faults in");
    TEST_EXPECT_EQ(tables1, tables0, "the clamped fault built no table (the headroom is the pages')");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "slot 80 faults in under five pages of headroom");
    TEST_EXPECT_EQ(res1, 69u, "the cluster minted five pages, not 64");
    TEST_EXPECT_EQ(rec1, 69u, "the record follows");
    TEST_ASSERT(win_starts_at_fault, "the cut window runs from the faulting slot: 80..84, not the cluster's 64");
    TEST_ASSERT(at_cap, "the payer sits exactly at its cap");
    TEST_EXPECT_EQ(r2, FAULT_UNHANDLED_USER, "with no headroom the fault is refused");
    TEST_EXPECT_EQ(res2, 69u, "nothing was minted");
    TEST_EXPECT_EQ(rec2, 69u, "nothing was charged");
    TEST_EXPECT_EQ(r3, FAULT_HANDLED, "slot 100 faults in with the budget restored");
    TEST_EXPECT_EQ(res3, 128u, "the window 64..127 minted only its 59 missing pages");
    TEST_EXPECT_EQ(rec3, 128u, "the record is the resident count");
    TEST_EXPECT_EQ(d3, d3a + 59u, "the payer paid the 59 it minted, not the window's 64");
    TEST_EXPECT_EQ(drc, 0, "detach");
}

// Round 34 F1: a fill holds no charge across its read. With three pages of
// headroom, a charge the payer makes while the read sleeps still lands; the
// pages are charged as the install adopts them.
void test_demand_page_file_private_read_holds_no_charge(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Burrow *v = v118_private_image(p->as);
    if (!v) { drop_proc(p); TEST_ASSERT(false, "a private image"); }
    int mrc = burrow_map(p, v, USER_VA, V118_PAGES * ONE_PAGE, VMA_PROT_READ);
    burrow_unref(v);

    enum fault_result r0 = v118_fault(p, 0);         // builds the tables, mints 0..63
    const u32 budget0 = p->as->page_budget;
    p->as->page_budget = p->as->page_count + 3u;
    g_rev_probe_ok = -1;
    g_rev_probe_as = p->as;
    enum fault_result r1 = v118_fault(p, 64);        // a three-page window
    int probe = g_rev_probe_ok;
    g_rev_probe_as = NULL;
    u32 res1 = burrow_image_resident_count(v), rec1 = burrow_file_charge_pages_for_test(v);
    p->as->page_budget = budget0;
    int drc = v118_detach(p, 0, V118_PAGES);
    drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "slot 0 faults in");
    TEST_EXPECT_EQ(probe, 1, "a charge made while the read sleeps lands: the fill holds none");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "slot 64 faults in");
    TEST_EXPECT_EQ(res1, 67u, "the window minted its three pages");
    TEST_EXPECT_EQ(rec1, 67u, "and the record holds them");
    TEST_EXPECT_EQ(drc, 0, "detach");
}

// Round 34 F6: an exempt payer's cap still binds a holder that is not exempt:
// the waiver needs both verdicts.
void test_demand_page_file_private_exempt_payer_binds_holder(void) {
    struct Proc *p = make_proc();
    struct Proc *q = make_proc();
    if (!p || !q) { drop_proc(q); drop_proc(p); TEST_ASSERT(false, "proc_alloc failed"); }
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Spoor *s = spoor_alloc(&g_rev_private_dev);
    struct Burrow *v = s ? image_lookup_or_create(s, 0, V118_PAGES * ONE_PAGE, /*exec=*/false,
                                                  BURROW_FILE_LIMIT_UNKNOWN, p->as,
                                                  /*payer_exempt=*/true) : NULL;
    if (!v) { if (s) spoor_clunk(s); drop_proc(q); drop_proc(p); TEST_ASSERT(false, "a private image"); }
    int mrc = burrow_map(p, v, USER_VA, V118_PAGES * ONE_PAGE, VMA_PROT_READ);
    burrow_unref(v);

    struct AddrSpace *child = addrspace_clone(p->as, false);
    struct AddrSpace *qown  = q->as;
    enum fault_result r = FAULT_UNHANDLED_USER;
    u32 res = 0, rec = 0;
    bool q_exempt = proc_resource_exempt(q);
    if (child) {
        const u32 budget0 = p->as->page_budget;
        p->as->page_budget = p->as->page_count + 2u;  // the exempt payer: two pages under its cap
        q->as = child;
        r   = v118_fault(q, 0);                     // the holder is not exempt
        res = burrow_image_resident_count(v);
        rec = burrow_file_charge_pages_for_test(v);
        q->as = qown;
        p->as->page_budget = budget0;
        addrspace_unref(child);
    }
    int drc = v118_detach(p, 0, V118_PAGES);
    drop_proc(q);
    drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_ASSERT(child != NULL, "the fork");
    TEST_ASSERT(!q_exempt, "the holder is not exempt");
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "the holder's fault is served");
    TEST_EXPECT_EQ(res, 2u, "its read-ahead is held to the payer's cap: two pages, not 64");
    TEST_EXPECT_EQ(rec, 2u, "and the record holds them");
    TEST_EXPECT_EQ(drc, 0, "detach");
}

// A fork's child is not the payer: its mints are charged to the payer, its
// leaves to itself.
void test_demand_page_file_private_holder_pays_leaves(void) {
    struct Proc *p = make_proc();
    struct Proc *q = make_proc();
    if (!p || !q) { drop_proc(q); drop_proc(p); TEST_ASSERT(false, "proc_alloc failed"); }
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Burrow *v = v118_private_image(p->as);
    if (!v) { drop_proc(q); drop_proc(p); TEST_ASSERT(false, "a private image"); }
    int mrc = burrow_map(p, v, USER_VA, V118_PAGES * ONE_PAGE, VMA_PROT_READ);
    burrow_unref(v);

    struct AddrSpace *child = addrspace_clone(p->as, false);
    struct AddrSpace *qown  = q->as;
    struct Vma *cv = NULL;
    u32 dp0 = v118_data(p->as), dq0 = 0, fq0 = 0, dp1 = 0, dq1 = 0, fq1 = 0, dp2 = 0;
    enum fault_result rq = FAULT_UNHANDLED_USER, rp = FAULT_UNHANDLED_USER;
    bool child_flagged = true;
    if (child) {
        q->as = child;
        cv = vma_lookup(q, USER_VA);
        child_flagged = cv && (cv->flags & VMA_FLAG_FILE_PAYER);
        dq0 = v118_data(child); fq0 = child->file_pages;
        rq  = v118_fault(q, 0);                       // the child mints
        dp1 = v118_data(p->as); dq1 = v118_data(child); fq1 = child->file_pages;
        rp  = v118_fault(p, 1);                       // the payer's resident hit
        dp2 = v118_data(p->as);
        q->as = qown;
        addrspace_unref(child);
    }
    int drc = v118_detach(p, 0, V118_PAGES);
    drop_proc(q);
    drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_ASSERT(child != NULL, "the fork");
    TEST_ASSERT(!child_flagged, "a fork's child does not inherit the payer mapping");
    TEST_EXPECT_EQ(rq, FAULT_HANDLED, "the child's fault mints");
    TEST_EXPECT_EQ(dp1, dp0 + 64u, "the payer paid the child's 64 mints");
    TEST_EXPECT_EQ(dq1, dq0 + 1u, "the child paid its one leaf");
    TEST_EXPECT_EQ(fq1, fq0 + 1u, "as a file leaf");
    TEST_EXPECT_EQ(rp, FAULT_HANDLED, "the payer's resident hit");
    TEST_EXPECT_EQ(dp2, dp1, "the payer's leaf costs nothing more");
    TEST_EXPECT_EQ(drc, 0, "detach");
}

// A dying payer lapses the record; the next faulter that mints becomes the
// payer, charged the whole residency, and its own mappings of the image
// become payer mappings (their charged leaves refunded and re-faulted).
void test_demand_page_file_private_payer_death_hands_over(void) {
    struct Proc *p = make_proc();
    struct Proc *q = make_proc();
    if (!p || !q) { drop_proc(q); drop_proc(p); TEST_ASSERT(false, "proc_alloc failed"); }
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Burrow *v = v118_private_image(p->as);
    if (!v) { drop_proc(q); drop_proc(p); TEST_ASSERT(false, "a private image"); }
    int mrc = burrow_map(p, v, USER_VA, V118_PAGES * ONE_PAGE, VMA_PROT_READ);
    burrow_unref(v);
    enum fault_result r0 = v118_fault(p, 0);         // the payer mints 0..63

    struct AddrSpace *child = addrspace_clone(p->as, false);
    struct AddrSpace *qown  = q->as;
    u32 dq0 = 0, fq0 = 0, fq1 = 0, rec_lapsed = 0, dq2 = 0, fq2 = 0, rec2 = 0, dq3 = 0;
    enum fault_result r1 = FAULT_UNHANDLED_USER, r2 = FAULT_UNHANDLED_USER,
                      r3 = FAULT_UNHANDLED_USER;
    bool lapsed = false, handed = false, flagged = false, old_leaf_gone = false;
    if (child) {
        q->as = child;
        dq0 = v118_data(child); fq0 = child->file_pages;
        r1  = v118_fault(q, 1);                       // a resident hit: a charged leaf
        fq1 = child->file_pages;
        // The Burrow outlives the payer in the child's mapping; read it through
        // the child's VMA, which keeps it alive.
        struct Vma *cv = vma_lookup(q, USER_VA);
        struct Burrow *cb = cv ? cv->burrow : NULL;
        drop_proc(p);                                 // the payer dies
        p = NULL;
        lapsed     = cb && burrow_file_payer_for_test(cb) == NULL;
        rec_lapsed = cb ? burrow_file_charge_pages_for_test(cb) : 0;
        r2  = v118_fault(q, 64);                      // a mint into a lapsed record
        handed  = cb && burrow_file_payer_for_test(cb) == child;
        rec2    = cb ? burrow_file_charge_pages_for_test(cb) : 0;
        flagged = cv && (cv->flags & VMA_FLAG_FILE_PAYER);
        old_leaf_gone = walk_to_l3_entry(child->pgtable_root, USER_VA + ONE_PAGE) == 0;
        dq2 = v118_data(child); fq2 = child->file_pages;
        r3  = v118_fault(q, 1);                       // re-faults as a payer leaf
        dq3 = v118_data(child);
        q->as = qown;
        addrspace_unref(child);                       // the last mapping: the free
    }
    drop_proc(q);
    if (p) drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "the payer mints");
    TEST_ASSERT(child != NULL, "the fork");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "the child's resident hit");
    TEST_EXPECT_EQ(fq1, fq0 + 1u, "the child paid that leaf");
    TEST_ASSERT(lapsed, "the payer's death lapses the record");
    TEST_EXPECT_EQ(rec_lapsed, 64u, "the lapsed record keeps the resident count");
    TEST_EXPECT_EQ(r2, FAULT_HANDLED, "the child's mint is served");
    TEST_ASSERT(handed, "the minting faulter became the payer");
    TEST_EXPECT_EQ(rec2, 128u, "the record: 64 inherited + 64 minted");
    TEST_ASSERT(flagged, "the new payer's mapping became a payer mapping");
    TEST_ASSERT(old_leaf_gone, "the adoption uninstalled the leaf it had paid for");
    TEST_EXPECT_EQ(fq2, fq0, "and refunded it");
    TEST_EXPECT_EQ(dq2, dq0 + 128u, "the new payer pays the whole residency, and no leaf");
    TEST_EXPECT_EQ(r3, FAULT_HANDLED, "the refunded page re-faults");
    TEST_EXPECT_EQ(dq3, dq2, "as a payer leaf, free");
}

// The control: a cached image takes no payer and keeps the system cache's
// posture -- one leaf charged per page mapped, no mint charge.
void test_demand_page_file_cached_image_takes_no_payer(void) {
    (void)image_cache_evict_idle_for_test();
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_eof  = -1;
    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = image_lookup_or_create(s, 0, V118_PAGES * ONE_PAGE, /*exec=*/false,
                                              BURROW_FILE_LIMIT_UNKNOWN, p->as, false);
    if (!v) { spoor_clunk(s); drop_proc(p); TEST_ASSERT(false, "a cached image"); }
    bool no_payer = !v->file_private && burrow_file_payer_for_test(v) == NULL;
    int mrc = burrow_map(p, v, USER_VA, V118_PAGES * ONE_PAGE, VMA_PROT_READ);
    struct Vma *vma = vma_lookup(p, USER_VA);
    bool flagged = vma && (vma->flags & VMA_FLAG_FILE_PAYER);
    const u32 d0 = v118_data(p->as), f0 = p->as->file_pages;
    enum fault_result r0 = v118_fault(p, 0);
    u32 d1 = v118_data(p->as), f1 = p->as->file_pages;
    u32 res = burrow_image_resident_count(v), rec = burrow_file_charge_pages_for_test(v);
    int drc = v118_detach(p, 0, V118_PAGES);
    burrow_unref(v);
    drop_proc(p);
    (void)image_cache_evict_idle_for_test();

    TEST_ASSERT(no_payer, "a cached image is not private and has no payer");
    TEST_EXPECT_EQ(mrc, 0, "burrow_map R");
    TEST_ASSERT(!flagged, "its mapping is no payer mapping");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "slot 0 faults in");
    TEST_EXPECT_EQ(res, 64u, "the cluster minted 64 pages");
    TEST_EXPECT_EQ(rec, 0u, "and charged no record");
    TEST_EXPECT_EQ(d1, d0 + 1u, "the holder paid its one leaf, as before");
    TEST_EXPECT_EQ(f1, f0 + 1u, "as a file leaf");
    TEST_EXPECT_EQ(drc, 0, "detach");
}

// An idle image (cached, mapped by no one) holding pool pages is stripped of
// them when a user allocation would otherwise be refused; the allocation is
// served, the entry stays cached, and the next lookup hits it.
void test_demand_page_idle_image_reclaimed_under_pressure(void) {
    (void)image_cache_evict_idle_for_test();      // start from no idle image
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = image_lookup_or_create(s, 0, 2 * ONE_PAGE, /*exec=*/false,
                                              BURROW_FILE_LIMIT_UNKNOWN, NULL, false);
    TEST_ASSERT(v != NULL, "image_lookup_or_create");
    TEST_EXPECT_EQ(burrow_handle_count(v), 2, "the cache's ref and ours");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_READ), 0, "burrow_map R");

    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 0 in");
    make_fi(&fi, USER_VA + ONE_PAGE, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 1 in");
    TEST_EXPECT_EQ(burrow_image_resident_count(v), 2u, "two resident pages");

    // The mapper dies and its handle goes: the image is idle, {1,0}, holding
    // two pool pages no address space is charged for.
    drop_proc(p);
    burrow_unref(v);
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "the cache's ref alone");
    TEST_EXPECT_EQ(burrow_mapping_count(v), 0, "unmapped");
    TEST_ASSERT(image_cache_live_count_for_test() >= 1, "still cached");

    // The physical control (magazines drained): what the pool releases the
    // buddy must GAIN -- a strip that only uncharged would leave it where it was.
    const u64 pf0 = test_free_pages_settled();
    const u32 ch0 = capacity_pool_charged();

    // Park the pool full: a user allocation must now be served by the reclaim,
    // not refused, and it is the idle image that pays -- one page per page
    // asked (audit F14: a strip takes what is wanted, not the whole image).
    const u32 pool = capacity_pool_pages();
    const u32 room = pool - ch0;
    TEST_ASSERT(room > 0, "the pool has room to park");
    capacity_pool_park_for_test(room);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool, "the pool is full");
    const u64 r0  = image_cache_reclaims_for_test();
    const u64 rp0 = image_cache_reclaimed_pages_for_test();
    struct page *pg = alloc_user_pages(0, 0, /*exempt=*/false);
    TEST_ASSERT(pg != NULL, "a user allocation at the pool's edge is served by stripping the idle image");
    TEST_EXPECT_EQ(image_cache_reclaims_for_test(), r0 + 1, "one reclaim");
    TEST_EXPECT_EQ(image_cache_reclaimed_pages_for_test(), rp0 + 1, "it freed ONE page: what was asked, not the image");
    TEST_EXPECT_EQ(burrow_image_resident_count(v), 1u, "the image keeps its other page");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool, "one page out, one in");
    // The next allocation strips the rest of the same (still least recently
    // used) image.
    struct page *pg2 = alloc_user_pages(0, 0, /*exempt=*/false);
    TEST_ASSERT(pg2 != NULL, "served again");
    TEST_EXPECT_EQ(image_cache_reclaims_for_test(), r0 + 2, "a second reclaim");
    TEST_EXPECT_EQ(image_cache_reclaimed_pages_for_test(), rp0 + 2, "the image's second page");
    TEST_EXPECT_EQ(burrow_image_resident_count(v), 0u, "the image is stripped");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool, "still at the edge");
    free_pages(pg, 0);
    free_pages(pg2, 0);
    capacity_pool_unpark_for_test(room);
    magazines_drain_all();
    const u32 ch1 = capacity_pool_charged();
    TEST_ASSERT(ch1 + 2u <= ch0, "the image's two pages left the pool");
    TEST_EXPECT_EQ(test_free_pages_settled(), pf0 + (u64)(ch0 - ch1),
                   "CONTROL: the buddy gained exactly what the pool released -- the pages "
                   "were freed, not merely uncharged");

    // The entry survived: the next lookup HITS the same Burrow, empty, which
    // pages in again on its next fault.
    const u64 hits0 = image_cache_hits_for_test();
    struct Spoor *s2 = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s2 != NULL, "spoor_alloc");
    struct Burrow *again = image_lookup_or_create(s2, 0, 2 * ONE_PAGE, /*exec=*/false,
                                                  BURROW_FILE_LIMIT_UNKNOWN, NULL, false);
    TEST_ASSERT(again == v, "the stripped image is still the cached one");
    TEST_EXPECT_EQ(image_cache_hits_for_test(), hits0 + 1, "a hit");
    burrow_unref(again);
    TEST_ASSERT(image_cache_evict_idle_for_test() >= 1, "and it is evicted at the end");
}

// B-1a' audit F19: a refused charge asks the reclaim for its SHORTFALL in one
// request. An exempt overshoot (the pool charged past its size by the TCB) is
// folded in: one scan of the cache frees what the allocation is short by,
// rather than one scan per page of the overshoot -- each of those under the
// faulter's address-space lock.
void test_demand_page_reclaim_asks_for_the_shortfall(void) {
    (void)image_cache_evict_idle_for_test();      // start from no idle image
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    g_rev_read_fail = false;
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = image_lookup_or_create(s, 0, 2 * ONE_PAGE, /*exec=*/false,
                                              BURROW_FILE_LIMIT_UNKNOWN, NULL, false);
    TEST_ASSERT(v != NULL, "image_lookup_or_create");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, 2 * ONE_PAGE, VMA_PROT_READ), 0, "burrow_map R");
    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 0 in");
    make_fi(&fi, USER_VA + ONE_PAGE, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 1 in");
    drop_proc(p);
    burrow_unref(v);
    TEST_EXPECT_EQ(burrow_handle_count(v), 1, "the cache's ref alone");
    TEST_EXPECT_EQ(burrow_image_resident_count(v), 2u, "an idle image holding two pages");

    // Park the pool ONE PAST full (an exempt overshoot): a one-page allocation
    // is short by two. Asked per page, the reclaim would strip one, the charge
    // would be refused again, and a second scan would strip the other.
    const u32 pool = capacity_pool_pages();
    const u32 room = pool - capacity_pool_charged();
    TEST_ASSERT(room > 0, "the pool has room to park");
    capacity_pool_park_for_test(room + 1);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool + 1, "over-full by one");
    const u64 r0  = image_cache_reclaims_for_test();
    const u64 rp0 = image_cache_reclaimed_pages_for_test();
    struct page *pg = alloc_user_pages(0, 0, /*exempt=*/false);
    TEST_ASSERT(pg != NULL, "served at the overshoot");
    TEST_EXPECT_EQ(image_cache_reclaims_for_test(), r0 + 1,
                   "ONE reclaim call: the shortfall was asked for whole (per page it would be two)");
    TEST_EXPECT_EQ(image_cache_reclaimed_pages_for_test(), rp0 + 2,
                   "and it freed the two pages the charge was short by");
    TEST_EXPECT_EQ(burrow_image_resident_count(v), 0u, "the image is stripped");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool, "over-full + 1 - 2 = at the edge");
    free_pages(pg, 0);
    capacity_pool_unpark_for_test(room + 1);
    TEST_ASSERT(image_cache_evict_idle_for_test() >= 1, "evicted at the end");
}

// B-1a' audit F17: an abort that is neither a translation, an access-flag nor
// a permission fault -- an alignment fault (an exclusive, an acquire/release
// or a Device access at an address the instruction cannot take) or a
// synchronous external abort -- is raised on a MAPPED page by the instruction
// itself. A pager that answers it HANDLED sends the ERET back into the same
// abort, forever; the fault's class alone decides it: snare:bus, before any
// lookup. The EL0 witness is joey's /bus-probe-child (a misaligned
// load-exclusive must die, not hang the boot).
void test_demand_page_alignment_abort_is_bus_not_handled(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    struct Burrow *v = burrow_create_anon_lazy(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon_lazy failed");
    TEST_EXPECT_EQ(burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RW), 0, "burrow_map RW");
    burrow_unref(v);

    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x11, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "the page is faulted in");
    TEST_ASSERT(mmu_user_pte_admits(p->as, USER_VA, /*write=*/false, /*exec=*/false),
                "a valid leaf admits the read: an install has nothing left to do");

    // An alignment fault on the mapped, admitting page (a misaligned ldxr).
    make_fi(&fi, USER_VA + 0x11, /*is_write=*/false, /*is_instr=*/false);
    fi.fsc            = 0x21;                     // FSC_ALIGN_FAULT
    fi.fault_level    = 1;
    fi.is_translation = false;
    fi.is_alignment   = true;
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_USER_BUS,
                   "an alignment fault on a mapped page is snare:bus, not HANDLED "
                   "(HANDLED re-executes the same instruction into the same abort)");
    // A synchronous external abort: the same disposition.
    make_fi(&fi, USER_VA + 0x10, /*is_write=*/true, /*is_instr=*/false);
    fi.fsc            = 0x10;                     // FSC_EXT_ABORT
    fi.fault_level    = 0;
    fi.is_translation = false;
    fi.is_external    = true;
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_USER_BUS, "an external abort is snare:bus");
    // CONTROL: the same VA, a translation fault -> HANDLED by the leaf that is
    // there. The class decides, not the page.
    make_fi(&fi, USER_VA + 0x10, /*is_write=*/true, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED,
                   "CONTROL: a translation fault on the same VA is HANDLED");
    drop_proc(p);
}
