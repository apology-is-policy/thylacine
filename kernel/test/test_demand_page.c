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

#include <thylacine/dev.h>           // REVENANT R-2: struct Dev for the stub backing Dev
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>         // REVENANT R-2: spoor_alloc for the FILE Burrow's backing Chan
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
// REVENANT R-2: BURROW_TYPE_FILE demand-page fault arm.
void test_demand_page_file_smoke(void);
void test_demand_page_file_read_error_snare_bus(void);
void test_demand_page_file_multi_page(void);
// REVENANT R-5 audit SA-F1: the slow path must LOOP over a short-returning read.
void test_demand_page_file_short_read_fills_page(void);
// Overcommit / I-32 (ARCH section 6.5): the BURROW_TYPE_ANON_LAZY demand-zero arm.
void test_demand_page_lazy_zero_fill(void);
void test_demand_page_lazy_decommit_refault(void);
void test_demand_page_lazy_charge_on_fault_oom(void);
void test_demand_page_lazy_detach_uncharges_resident(void);

// The lazy attach/detach _for_proc inners (non-static cores in kernel/syscall.c),
// driven directly -- the same pattern test_sys_burrow / test_resource use.
extern s64 sys_burrow_attach_lazy_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);

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
    int rc = mmu_install_user_pte(p->pgtable_root, 0,
                                  USER_VA, backing_pa, VMA_PROT_RW,
                                  /*device_memory=*/false);
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
    rc = mmu_install_user_pte(p->pgtable_root, 0,
                              USER_VA + ONE_PAGE, code_pa, VMA_PROT_RX,
                              /*device_memory=*/false);
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
    TEST_EXPECT_EQ(mmu_install_user_pte(0, 0, USER_VA, backing_pa, VMA_PROT_RW, false), -1,
        "pgtable_root=0 rejected");

    // Unaligned vaddr.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, 0,
                                        USER_VA + 1, backing_pa, VMA_PROT_RW, false), -1,
        "unaligned vaddr rejected");

    // Unaligned pa.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, 0,
                                        USER_VA, backing_pa + 1, VMA_PROT_RW, false), -1,
        "unaligned pa rejected");

    // vaddr in TTBR1 high half — installer rejects (top bits set).
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, 0,
                                        0xFFFF000000000000ull, backing_pa, VMA_PROT_RW, false), -1,
        "high-VA vaddr rejected");

    // W+X prot.
    TEST_EXPECT_EQ(mmu_install_user_pte(p->pgtable_root, 0,
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
    int rc = mmu_install_user_pte(p->pgtable_root, 0,
                                  USER_VA, backing_pa, VMA_PROT_RW,
                                  /*device_memory=*/false);
    TEST_EXPECT_EQ(rc, 0, "first install ok");

    // Snapshot free pages — second identical install must not allocate
    // new sub-tables.
    u64 free_before_second = phys_free_pages();

    rc = mmu_install_user_pte(p->pgtable_root, 0,
                              USER_VA, backing_pa, VMA_PROT_RW, false);
    TEST_EXPECT_EQ(rc, 0, "idempotent second install returns 0");
    TEST_EXPECT_EQ(phys_free_pages(), free_before_second,
        "no buddy alloc on idempotent re-install");

    // Mismatching install (different PA) returns -1.
    paddr_t other_pa = page_to_pa(alloc_pages(0, KP_ZERO));
    TEST_ASSERT(other_pa != 0, "other backing alloc");
    rc = mmu_install_user_pte(p->pgtable_root, 0,
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

static long rev_test_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)c;
    g_rev_read_calls++;
    if (g_rev_read_fail) return -1;
    long m = n;
    if (g_rev_read_chunk > 0 && m > g_rev_read_chunk)
        m = g_rev_read_chunk;        // short-return (interior, NOT EOF)
    u8 *b = (u8 *)buf;
    for (long i = 0; i < m; i++) b[i] = (u8)(((u64)off + (u64)i) & 0xff);
    return m;
}

static struct Dev g_rev_test_dev = {
    .dc   = '?',
    .name = "revtest",
    .read = rev_test_read,
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
    struct Burrow *v = burrow_create_file(s, FILE_OFF, ONE_PAGE);
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
    u64 pte = walk_to_l3_entry(p->pgtable_root, USER_VA);
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

void test_demand_page_file_read_error_snare_bus(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = true;     // the stub .read returns -1 (a dead/wedged FS)
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    struct Burrow *v = burrow_create_file(s, 0, ONE_PAGE);
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
    TEST_ASSERT(walk_to_l3_entry(p->pgtable_root, USER_VA) == 0,
        "no PTE installed on a read error");

    g_rev_read_fail = false;     // reset for other tests
    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_file_multi_page(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    g_rev_read_fail  = false;
    g_rev_read_calls = 0;

    struct Spoor *s = spoor_alloc(&g_rev_test_dev);
    TEST_ASSERT(s != NULL, "spoor_alloc");
    const u64 FILE_OFF = 0x4000;
    struct Burrow *v = burrow_create_file(s, FILE_OFF, FOUR_PAGES);   // 4 pages
    TEST_ASSERT(v != NULL, "burrow_create_file 4-page");
    int rc = burrow_map(p, v, USER_VA, FOUR_PAGES, VMA_PROT_RX);
    TEST_EXPECT_EQ(rc, 0, "burrow_map 4-page FILE RX");

    // Fault page 2 FIRST (out of order) -> reads file offset FILE_OFF + 2 pages.
    // This pins the slot<->file-offset mapping (an off-by-one would read the
    // wrong file bytes into the wrong slot).
    struct fault_info fi;
    make_fi(&fi, USER_VA + 2 * ONE_PAGE + 0x10, /*is_write=*/false, /*is_instr=*/true);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "page 2 fault resolves");

    struct page *pg2 = burrow_file_slot_for_test(v, 2);
    TEST_ASSERT(pg2 != NULL, "slot 2 resident");
    u8 *b2 = (u8 *)pa_to_kva(page_to_pa(pg2));
    TEST_EXPECT_EQ((u64)b2[0], (u64)(u8)((FILE_OFF + 2 * ONE_PAGE) & 0xff),
        "slot 2 content = file bytes at FILE_OFF + 2 pages (slot<->offset map)");

    // Slots 0,1,3 stay empty (sparse -- only the faulted page reads).
    TEST_ASSERT(burrow_file_slot_for_test(v, 0) == NULL, "slot 0 sparse");
    TEST_ASSERT(burrow_file_slot_for_test(v, 1) == NULL, "slot 1 sparse");
    TEST_ASSERT(burrow_file_slot_for_test(v, 3) == NULL, "slot 3 sparse");
    TEST_EXPECT_EQ(g_rev_read_calls, 1, "only the faulted page read");

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
    struct Burrow *v = burrow_create_file(s, FILE_OFF, ONE_PAGE);
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
//     An over-PROC_PAGE_MAX commit fails the fault (FAULT_UNHANDLED_USER --
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
    TEST_EXPECT_EQ((u64)p->page_count, 0ull, "lazy attach charges no pages");

    // Write-fault page 0 -> demand-zero install + charge-on-fault.
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x100, /*is_write=*/true, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "lazy demand-page resolves a mapped VA");

    struct page *slot = burrow_lazy_slot_for_test(v, 0);
    TEST_ASSERT(slot != NULL, "slot 0 resident after fault");
    TEST_EXPECT_EQ((u64)p->page_count, 1ull, "charge-on-fault: page_count == 1");
    u8 *bytes = (u8 *)pa_to_kva(page_to_pa(slot));
    TEST_EXPECT_EQ((u64)bytes[0], 0ull,             "demand-zero: byte 0 is zero");
    TEST_EXPECT_EQ((u64)bytes[0x800], 0ull,         "demand-zero: mid byte is zero");
    TEST_EXPECT_EQ((u64)bytes[PAGE_SIZE - 1], 0ull, "demand-zero: last byte is zero");

    // PTE is RW/XN (W^X-clean: writable, never executable).
    u64 pte = walk_to_l3_entry(p->pgtable_root, USER_VA);
    TEST_ASSERT(pte != 0, "L3 PTE installed after lazy demand-page");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(slot), "PTE PA = the resident slot page");
    TEST_ASSERT((pte & BIT_PXN) != 0, "lazy page PXN (kernel never execs user)");
    TEST_ASSERT((pte & BIT_UXN) != 0, "lazy page UXN (W^X: never executable)");
    TEST_EXPECT_EQ(pte & BIT_AP_FIELD, BIT_AP_RW_ANY, "lazy page RW (AP_RW_ANY)");

    // A second fault to the same page hits resident -- no double-charge.
    make_fi(&fi, USER_VA + 0x40, /*is_write=*/false, /*is_instr=*/false);
    r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "second fault to resident lazy page resolves");
    TEST_EXPECT_EQ((u64)p->page_count, 1ull, "re-fault does NOT double-charge (still 1)");

    // Other slots stay sparse (only the faulted page committed).
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 1) == NULL, "slot 1 sparse");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 3) == NULL, "slot 3 sparse");

    drop_proc(p);
    burrow_unref(v);
}

void test_demand_page_lazy_decommit_refault(void) {
    u64 free_before = phys_free_pages();
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
    TEST_EXPECT_EQ((u64)p->page_count, 3ull, "3 faulted pages charged");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 2) != NULL, "slot 2 resident");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 3) == NULL, "slot 3 sparse");

    // Decommit the whole 4-page range (under vma_lock, like the syscall path).
    spin_lock(&p->vma_lock);
    int drc = burrow_decommit(p, USER_VA, FOUR_PAGES);
    spin_unlock(&p->vma_lock);
    TEST_EXPECT_EQ(drc, 0, "decommit succeeds");
    TEST_EXPECT_EQ((u64)p->page_count, 0ull, "decommit uncharges the 3 resident pages");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 0) == NULL, "slot 0 released");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 2) == NULL, "slot 2 released");
    TEST_ASSERT(walk_to_l3_entry(p->pgtable_root, USER_VA) == 0, "PTE cleared after decommit");

    // Re-fault page 0 -> a FRESH zero page, re-charged.
    struct fault_info fi;
    make_fi(&fi, USER_VA + 0x20, /*is_write=*/false, /*is_instr=*/false);
    TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "re-fault after decommit");
    TEST_EXPECT_EQ((u64)p->page_count, 1ull, "re-fault re-charges (1)");
    struct page *slot = burrow_lazy_slot_for_test(v, 0);
    TEST_ASSERT(slot != NULL, "slot 0 re-resident");
    TEST_EXPECT_EQ((u64)((u8 *)pa_to_kva(page_to_pa(slot)))[0], 0ull, "re-faulted page is fresh-zero");

    drop_proc(p);
    burrow_unref(v);
    TEST_EXPECT_EQ(phys_free_pages(), free_before,
        "lazy fault + decommit + re-fault + teardown leaks no pages "
        "(sparse pages freed; sub-tables freed by proc_pgtable_destroy)");
}

void test_demand_page_lazy_charge_on_fault_oom(void) {
    struct Proc *p = make_proc();
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    // A proc_alloc'd Proc is principal_id 0 (PRINCIPAL_INVALID) -> NOT exempt, so the
    // I-32 page cap applies. Park page_count AT the cap so the next commit fails.
    p->page_count = PROC_PAGE_MAX;

    struct Burrow *v = burrow_create_anon_lazy(ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon_lazy");
    int rc = burrow_map(p, v, USER_VA, ONE_PAGE, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "burrow_map lazy");

    // The fault tries to commit page #(PROC_PAGE_MAX+1) -> proc_page_charge refuses
    // -> graceful per-Proc terminate (FAULT_UNHANDLED_USER), NEVER an extinction.
    struct fault_info fi;
    make_fi(&fi, USER_VA, /*is_write=*/true, /*is_instr=*/false);
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_UNHANDLED_USER, "over-cap commit fails the fault (graceful OOM)");
    TEST_ASSERT(burrow_lazy_slot_for_test(v, 0) == NULL, "no page committed on a cap-hit");
    TEST_ASSERT(walk_to_l3_entry(p->pgtable_root, USER_VA) == 0, "no PTE on a cap-hit");
    TEST_EXPECT_EQ((u64)p->page_count, (u64)PROC_PAGE_MAX, "page_count unchanged (charge rolled back)");

    p->page_count = 0;     // reset (hygiene; drop_proc doesn't touch page_count)
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
    TEST_EXPECT_EQ((u64)p->page_count, 0ull, "lazy attach commits nothing");

    // Fault 2 of the 4 pages resident -> page_count 2.
    for (int i = 0; i < 2; i++) {
        struct fault_info fi;
        make_fi(&fi, va + (u64)i * ONE_PAGE + 0x8, /*is_write=*/true, /*is_instr=*/false);
        TEST_EXPECT_EQ(userland_demand_page(p, &fi), FAULT_HANDLED, "fault lazy page i");
    }
    TEST_EXPECT_EQ((u64)p->page_count, 2ull, "2 resident pages charged");

    // Detach the whole region. The uncharge must be ONLY the 2 RESIDENT pages -- the
    // lazy region charged per-fault, NOT the whole 4-page span (the eager detach
    // would over-uncharge by 2 if it used length/PAGE_SIZE here).
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, FOUR_PAGES), 0, "detach lazy region");
    TEST_EXPECT_EQ((u64)p->page_count, 0ull,
        "detach uncharges exactly the 2 resident pages (NOT the whole 4-page span)");
    TEST_ASSERT(vma_lookup(p, va) == NULL, "VMA gone after detach");

    drop_proc(p);
}
