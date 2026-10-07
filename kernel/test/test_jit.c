// I-42 / CL-7k: the JIT capability (docs/JIT-ON-WX-DESIGN.md; LLVM-DESIGN.md
// section 8). SYS_JIT_CREATE / SYS_JIT_DESTROY / SYS_ICACHE_SYNC.
//
// The central test is jit_dual_alias_pte_wx_clean, and it deliberately asserts
// on the REAL L3 page-table entries rather than on the VMA prots. The VMA prot
// is what the kernel INTENDED; the PTE is what the MMU will actually consult,
// and I-12 is a statement about the latter. Checking prots would pass even if
// the PTE encoder learned to set both AP_RW and clear UXN for a code mapping --
// which is precisely the regression a W^X surface must not be blind to.
//
// It also asserts the two aliases resolve to the SAME physical page. Without
// that, "dual-mapped region" would be unproven: two mappings of two different
// pages would satisfy every permission check and still not be a JIT primitive,
// because bytes written through the writer would never appear under the exec
// alias.

#include "test.h"

#include "../../arch/arm64/fault.h"
#include "../../arch/arm64/hwfeat.h"
#include "../../arch/arm64/mmu.h"
#include "../../mm/phys.h"

#include <thylacine/burrow.h>
#include <thylacine/caps.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

// The _for_proc inners (non-static cores in kernel/syscall.c) -- the kernel
// tests drive these directly on a fresh proc_alloc'd Proc, exactly as the
// burrow-attach tests do, so no EL0 thread context is needed.
// sys_jit_create_REGION is the mechanism (kernel out-pointers); the _for_proc
// wrapper adds only the user copy-out, which a kproc-context test cannot
// exercise (any address it can pass is a kernel VA, correctly refused by
// sys_validate_user_buf). The copy-out arm is proven by the in-guest prover.
s64 sys_jit_create_region(struct Proc *p, u64 length_raw, u64 *out_w, u64 *out_x);
s64 sys_jit_create_for_proc(struct Proc *p, u64 length_raw, u64 out_va);
s64 sys_jit_destroy_for_proc(struct Proc *p, u64 writer_va);
s64 sys_icache_sync_for_proc(struct Proc *p, u64 vaddr, u64 length);
s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
s64 sys_burrow_attach_lazy_for_proc(struct Proc *p, u64 length_raw);
s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);

// What the address space is charged for MEMORY: its count less the page-table
// pages the faults built, which the I-32 count also carries and which outlive a
// region's leaves until exit.
static u32 jit_pages(struct Proc *p) {
    return p->as->page_count - p->as->pgtable_pages;
}

// The user pool's charge, less this space's page tables (the same reason).
static u32 jit_pool(struct Proc *p) {
    return capacity_pool_charged() - p->as->pgtable_pages;
}

// The page a code region's slot holds, or NULL while it is uncommitted.
static struct page *jit_slot(const struct Burrow *b, size_t idx) {
    return burrow_lazy_slot_for_test(b, idx);
}

#define JIT_LEN     (2u * 4096u)     // two pages -- exercises the per-page loop
#define ONE_PAGE    4096ull

// PTE bit positions (mirrors test_demand_page.c -- kept local so a change to
// the encoder is caught here rather than silently shared).
#define BIT_VALID       (1ull << 0)
#define BIT_TYPE_PAGE   (1ull << 1)
#define BIT_TYPE_TABLE  (1ull << 1)
#define BIT_AF          (1ull << 10)
#define BIT_PXN         (1ull << 53)
#define BIT_UXN         (1ull << 54)
#define BIT_AP_FIELD    (3ull << 6)
#define BIT_AP_RW_ANY   (1ull << 6)        // 0b01 -- writable
#define BIT_AP_RO_ANY   (3ull << 6)        // 0b11 -- read-only

#define PTE_PA_MASK     0x0000FFFFFFFFF000ull

static struct Proc *jit_make_proc(bool with_cap) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    // proc_alloc gives CAP_NONE. Grant explicitly so the capless case is the
    // DEFAULT and the granted case is the deviation -- a test that had to
    // remove the cap to check denial would pass even if the gate were absent.
    if (with_cap) p->caps |= CAP_JIT;
    return p;
}

static void jit_drop_proc(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

static u64 jit_walk_l3(paddr_t pgtable_root, u64 vaddr) {
    u32 idx0 = (u32)((vaddr >> 39) & 0x1ff);
    u32 idx1 = (u32)((vaddr >> 30) & 0x1ff);
    u32 idx2 = (u32)((vaddr >> 21) & 0x1ff);
    u32 idx3 = (u32)((vaddr >> 12) & 0x1ff);

    u64 *l0 = (u64 *)pa_to_kva(pgtable_root);
    u64 e0 = l0[idx0];
    if (!(e0 & BIT_VALID) || !(e0 & BIT_TYPE_TABLE)) return 0;
    u64 *l1 = (u64 *)pa_to_kva(e0 & ~0xFFFull);
    u64 e1 = l1[idx1];
    if (!(e1 & BIT_VALID) || !(e1 & BIT_TYPE_TABLE)) return 0;
    u64 *l2 = (u64 *)pa_to_kva(e1 & ~0xFFFull);
    u64 e2 = l2[idx2];
    if (!(e2 & BIT_VALID) || !(e2 & BIT_TYPE_TABLE)) return 0;
    u64 *l3 = (u64 *)pa_to_kva(e2 & ~0xFFFull);
    return l3[idx3];
}

static void jit_fault_in(struct Proc *p, u64 vaddr, bool is_write) {
    struct fault_info fi;
    fi.vaddr           = vaddr;
    fi.elr             = 0;
    fi.esr             = 0;
    fi.ec              = 0x24;
    fi.fsc             = 0x07;
    fi.fault_level     = 3;
    fi.from_user       = true;
    fi.is_instruction  = false;
    fi.is_write        = is_write;
    fi.is_translation  = true;
    fi.is_permission   = false;
    fi.is_access_flag  = false;
    fi.is_alignment  = false;
    fi.is_external  = false;
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "demand_page must resolve a code-region VA");
}

// Count this Proc's VMAs backed by `b`. The dual-map property in one number.
static int jit_alias_count(struct Proc *p, const struct Burrow *b) {
    int n = 0;
    for (struct Vma *v = p->as->vmas; v; v = v->next)
        if (v->burrow == b) n++;
    return n;
}

// ---------------------------------------------------------------------------
// The CAP_JIT gate.
// ---------------------------------------------------------------------------
void test_jit_create_requires_cap(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/false);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg;
    u64 out = (u64)(uintptr_t)&reg;

    // A capless Proc is refused. Note this is the DEFAULT capability state --
    // an absent gate would let this through.
    TEST_EXPECT_EQ(sys_jit_create_for_proc(p, JIT_LEN, out), -T_E_ACCES,
        "a Proc without CAP_JIT must be refused (-EACCES)");

    // The cap is checked BEFORE argument validation, so a capless caller
    // cannot probe which lengths would have been acceptable. Both of these
    // are invalid-length requests; both must still report EPERM, not EINVAL.
    TEST_EXPECT_EQ(sys_jit_create_for_proc(p, 0, out), -T_E_ACCES,
        "capless + zero length -> EACCES (cap checked before args)");
    TEST_EXPECT_EQ(sys_jit_create_for_proc(p, JIT_REGION_MAX + 1, out), -T_E_ACCES,
        "capless + oversize -> EACCES (cap checked before args)");

    // No region was created, so nothing was charged.
    TEST_EXPECT_EQ(jit_pages(p), 0u,
        "a refused create must charge nothing");

    jit_drop_proc(p);
}

void test_jit_create_rejects_bad_args(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    u64 w = 0, x = 0;

    TEST_EXPECT_EQ(sys_jit_create_region(p, 0, &w, &x), -T_E_INVAL,
        "zero length rejected");
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_REGION_MAX + 1, &w, &x), -T_E_INVAL,
        "length above JIT_REGION_MAX rejected");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "no charge on a rejected create");

    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// I-42's core: two aliases, W^X-clean AT THE PTE, over ONE physical region.
// ---------------------------------------------------------------------------
void test_jit_dual_alias_pte_wx_clean(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "SYS_JIT_CREATE with CAP_JIT succeeds");

    TEST_ASSERT(reg.writer_va != 0, "writer_va returned");
    TEST_ASSERT(reg.exec_va != 0,   "exec_va returned");
    TEST_ASSERT(reg.writer_va != reg.exec_va,
        "the two aliases must be DISTINCT VAs -- one VA cannot be both RW and RX");
    TEST_EXPECT_EQ(reg.writer_va & (ONE_PAGE - 1), 0ull, "writer_va page-aligned");
    TEST_EXPECT_EQ(reg.exec_va & (ONE_PAGE - 1), 0ull,   "exec_va page-aligned");

    // Both aliases exist and are backed by ONE CODE Burrow.
    struct Vma *w = vma_lookup(p, reg.writer_va);
    struct Vma *x = vma_lookup(p, reg.exec_va);
    TEST_ASSERT(w != NULL, "writer VMA installed");
    TEST_ASSERT(x != NULL, "exec VMA installed");
    TEST_ASSERT(w->burrow == x->burrow,
        "both aliases must share ONE Burrow -- otherwise it is not a dual map");
    TEST_EXPECT_EQ(w->burrow->type, BURROW_TYPE_CODE, "backing is a CODE Burrow");
    TEST_EXPECT_EQ(jit_alias_count(p, w->burrow), 2,
        "exactly two aliases -- no more, no fewer");
    TEST_EXPECT_EQ(burrow_mapping_count(w->burrow), 2,
        "#847 mapping_count reflects both aliases");
    TEST_EXPECT_EQ(burrow_handle_count(w->burrow), 0,
        "construction handle dropped -- the mappings own the Burrow");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(w->burrow), 0u,
        "B-2a: a fresh region commits nothing");

    // Fault BOTH aliases in through the real fault path, then read the actual
    // hardware descriptors. This is the assertion that matters: the MMU
    // consults PTEs, not VMA prots.
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    jit_fault_in(p, reg.exec_va,   /*is_write=*/false);

    u64 pte_w = jit_walk_l3(p->as->pgtable_root, reg.writer_va);
    u64 pte_x = jit_walk_l3(p->as->pgtable_root, reg.exec_va);
    TEST_ASSERT(pte_w != 0, "writer L3 PTE installed");
    TEST_ASSERT(pte_x != 0, "exec L3 PTE installed");

    // The writer alias: writable, NOT executable.
    TEST_EXPECT_EQ(pte_w & BIT_AP_FIELD, BIT_AP_RW_ANY,
        "writer PTE is AP_RW (writable)");
    TEST_ASSERT((pte_w & BIT_UXN) != 0,
        "writer PTE has UXN SET -- the writable alias must never be executable");

    // The exec alias: executable, NOT writable.
    TEST_EXPECT_EQ(pte_x & BIT_AP_FIELD, BIT_AP_RO_ANY,
        "exec PTE is AP_RO (not writable)");
    TEST_ASSERT((pte_x & BIT_UXN) == 0,
        "exec PTE has UXN CLEAR -- the executable alias is fetchable");

    // I-12 stated directly over both descriptors: neither is W AND X.
    // (writable == AP_RW; user-executable == UXN clear.)
    TEST_ASSERT(!(((pte_w & BIT_AP_FIELD) == BIT_AP_RW_ANY) && ((pte_w & BIT_UXN) == 0)),
        "I-12: writer PTE must not be both writable and user-executable");
    TEST_ASSERT(!(((pte_x & BIT_AP_FIELD) == BIT_AP_RW_ANY) && ((pte_x & BIT_UXN) == 0)),
        "I-12: exec PTE must not be both writable and user-executable");

    // Both PTEs stay PXN: the kernel never executes user pages, code Burrow or
    // not. A code region is EL0-executable only.
    TEST_ASSERT((pte_w & BIT_PXN) != 0, "writer PTE keeps PXN");
    TEST_ASSERT((pte_x & BIT_PXN) != 0, "exec PTE keeps PXN -- EL1 never fetches JIT code");

    // The dual-map property itself: the two aliases resolve to the SAME
    // physical page. Without this the permission split above would be a
    // decoration on two unrelated regions.
    TEST_EXPECT_EQ(pte_w & PTE_PA_MASK, pte_x & PTE_PA_MASK,
        "both aliases must map the SAME physical page -- that is the dual map");
    TEST_ASSERT(jit_slot(w->burrow, 0) != NULL, "the first touch committed slot 0");
    TEST_EXPECT_EQ(pte_w & PTE_PA_MASK, page_to_pa(jit_slot(w->burrow, 0)),
        "and that page is the Burrow's own backing");

    // Page 2 as well, so the property is not an artifact of the first page --
    // and touched through the EXEC alias first, so the commit is not the
    // writer's alone: whichever alias touches a page first commits it, and the
    // other maps the same one.
    jit_fault_in(p, reg.exec_va + ONE_PAGE,   /*is_write=*/false);
    jit_fault_in(p, reg.writer_va + ONE_PAGE, /*is_write=*/true);
    u64 pte_w2 = jit_walk_l3(p->as->pgtable_root, reg.writer_va + ONE_PAGE);
    u64 pte_x2 = jit_walk_l3(p->as->pgtable_root, reg.exec_va + ONE_PAGE);
    TEST_EXPECT_EQ(pte_w2 & PTE_PA_MASK, pte_x2 & PTE_PA_MASK,
        "page 2: aliases still map one physical page");
    TEST_ASSERT((pte_w2 & BIT_UXN) != 0, "page 2: writer still non-executable");
    TEST_ASSERT((pte_x2 & BIT_UXN) == 0, "page 2: exec still executable");

    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// I-32: a page is charged ONCE, when first touched -- not once per alias, and
// not at create (B-2a: the region is a reservation).
// ---------------------------------------------------------------------------
void test_jit_charges_once_per_page(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "fresh Proc charged nothing");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "create charges nothing: a reservation is free");
    struct Burrow *b = vma_lookup(p, reg.writer_va)->burrow;

    // The first touch commits and charges the page, plus the map nodes it needed.
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    u32 one = jit_pages(p);
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 1u, "one page committed");
    TEST_EXPECT_EQ(one, burrow_lazy_footprint(b),
        "the charge is the footprint: the page and its nodes");

    // The SAME page through the other alias: mapped, not charged again. A
    // per-alias charge would bill a JIT twice for memory it holds once.
    jit_fault_in(p, reg.exec_va, /*is_write=*/false);
    TEST_EXPECT_EQ(jit_pages(p), one,
        "the exec alias maps the committed page without a second charge");

    // The second page, first touched through the exec alias.
    jit_fault_in(p, reg.exec_va + ONE_PAGE, /*is_write=*/false);
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 2u, "two pages committed");
    TEST_EXPECT_EQ(jit_pages(p), burrow_lazy_footprint(b),
        "still exactly the footprint");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "destroy succeeds");
    TEST_EXPECT_EQ(jit_pages(p), 0u,
        "destroy refunds exactly what the touches charged");

    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// B-2a: the largest region is a reservation -- created without a physical
// page, committed one touched page at a time, and refunded whole.
// ---------------------------------------------------------------------------
void test_jit_max_region_is_a_reservation(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    u32 pool0 = jit_pool(p);

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_REGION_MAX, &reg.writer_va, &reg.exec_va), 0,
        "a JIT_REGION_MAX region is created");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "and charged nothing");
    TEST_EXPECT_EQ(jit_pool(p), pool0, "and took no user page");
    struct Burrow *b = vma_lookup(p, reg.writer_va)->burrow;
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 0u, "nothing committed");

    // The two ends, far apart in the map, so the refund below has nodes to give
    // back as well as pages.
    u64 last = JIT_REGION_MAX - ONE_PAGE;
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    jit_fault_in(p, reg.writer_va + last, /*is_write=*/true);
    jit_fault_in(p, reg.exec_va + last, /*is_write=*/false);
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 2u, "two pages committed of 16384");
    u32 fp = burrow_lazy_footprint(b);
    TEST_ASSERT(fp > 2u, "the footprint includes the map's nodes");
    TEST_EXPECT_EQ(jit_pages(p), fp, "the charge is that footprint");
    TEST_EXPECT_EQ(jit_pool(p), pool0 + fp,
        "and the pool paid for exactly it: two pages and their nodes");
    TEST_EXPECT_EQ(jit_walk_l3(p->as->pgtable_root, reg.exec_va + last) & PTE_PA_MASK,
                   page_to_pa(jit_slot(b, (size_t)(last / ONE_PAGE))),
        "the exec alias of the last page maps the committed page");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "destroy succeeds");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "the pages and the nodes are refunded");
    TEST_EXPECT_EQ(jit_pool(p), pool0, "and returned to the pool");

    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// B-2a: a code page is released only with its region. SYS_BURROW_DECOMMIT
// releases a lazy page through ONE mapping; on a code region the other alias's
// leaf would still name the freed page.
// ---------------------------------------------------------------------------
void test_jit_decommit_refuses_code(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");
    struct Burrow *b = vma_lookup(p, reg.writer_va)->burrow;
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    jit_fault_in(p, reg.exec_va,   /*is_write=*/false);
    u64 pte_x = jit_walk_l3(p->as->pgtable_root, reg.exec_va);
    TEST_ASSERT(pte_x != 0, "the exec alias maps the page");
    u32 charged = jit_pages(p);

    TEST_EXPECT_EQ(burrow_decommit(p, reg.writer_va, ONE_PAGE), -(int)T_E_INVAL,
        "decommit through the writer alias is refused");
    TEST_EXPECT_EQ(burrow_decommit(p, reg.exec_va, ONE_PAGE), -(int)T_E_INVAL,
        "and through the exec alias");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 1u, "the page is still committed");
    TEST_EXPECT_EQ(jit_walk_l3(p->as->pgtable_root, reg.exec_va), pte_x,
        "the exec leaf is untouched");
    TEST_ASSERT(jit_walk_l3(p->as->pgtable_root, reg.writer_va) != 0,
        "the writer leaf is untouched");
    TEST_EXPECT_EQ(jit_pages(p), charged, "and nothing was refunded");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "cleanup");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "destroy refunds it");
    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// Teardown: both aliases, and the pages.
// ---------------------------------------------------------------------------
void test_jit_destroy_tears_down_both(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");

    u64 destroyed_before = burrow_total_destroyed();

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0,
        "destroy by writer_va succeeds");

    // BOTH aliases gone -- a destroy that removed only the named one would
    // leave an executable mapping with no writer, the worst residue possible.
    TEST_ASSERT(vma_lookup(p, reg.writer_va) == NULL, "writer alias removed");
    TEST_ASSERT(vma_lookup(p, reg.exec_va) == NULL,
        "EXEC alias removed too -- an orphaned RX mapping must never survive");

    // And the Burrow itself was freed: the #847 dual count reached {0,0} only
    // because both mappings dropped.
    TEST_EXPECT_EQ(burrow_total_destroyed(), destroyed_before + 1,
        "the code Burrow is freed once both aliases are gone");

    // A second destroy fails cleanly rather than tearing down something else.
    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), -T_E_INVAL,
        "destroying an already-destroyed region fails cleanly");

    jit_drop_proc(p);
}

void test_jit_destroy_rejects_non_writer(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");

    // The EXEC alias is not a valid destroy handle. Accepting it would make
    // "which alias did you mean" ambiguous, and the writer-only rule is what
    // makes a half-teardown unrepresentable.
    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.exec_va), -T_E_INVAL,
        "destroy must name the WRITER alias, not the exec alias");
    TEST_ASSERT(vma_lookup(p, reg.writer_va) != NULL,
        "a rejected destroy leaves the region intact");
    TEST_ASSERT(vma_lookup(p, reg.exec_va) != NULL,
        "a rejected destroy leaves the exec alias intact");

    // Interior VA of the writer alias: also refused (must be the base).
    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va + 4096ull), -T_E_INVAL,
        "destroy must name the BASE of the writer alias");

    // An ordinary anon mapping is not a code region and must not be
    // destroyable through this path -- the JIT syscalls act only on CODE.
    s64 anon = sys_burrow_attach_for_proc(p, 4096);
    TEST_ASSERT(anon > 0, "burrow_attach for the negative case");
    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, (u64)anon), -T_E_INVAL,
        "SYS_JIT_DESTROY must refuse a plain anon mapping");
    TEST_ASSERT(vma_lookup(p, (u64)anon) != NULL,
        "the anon mapping survives the refused destroy");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "cleanup");
    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// A code alias is not detachable through SYS_BURROW_DETACH (self-audit F1).
//
// The pair carries ONE I-32 charge. Detach knows nothing about the pair, so
// letting it through would refund that charge TWICE -- and a CAP_JIT holder
// looping create-then-detach-both could drive its page_count to zero while its
// real usage never moved, then allocate a fresh budget's worth. A bound a
// capability holder can zero is not a bound. It would also orphan the surviving
// alias, which SYS_JIT_DESTROY then refuses (no peer) -- unreleasable until
// Proc exit.
// ---------------------------------------------------------------------------
void test_jit_alias_not_detachable(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");
    // Touch both pages: a region that has charged nothing would make the
    // "nothing refunded" check below unable to fail.
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    jit_fault_in(p, reg.writer_va + ONE_PAGE, /*is_write=*/true);
    u32 charged = jit_pages(p);
    TEST_EXPECT_EQ(charged, burrow_lazy_footprint(vma_lookup(p, reg.writer_va)->burrow),
        "the region is charged its footprint");
    TEST_ASSERT(charged >= 2u, "both pages charged");

    // BOTH aliases must be refused -- either one alone breaks the pair.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, reg.exec_va, JIT_LEN), -1,
        "SYS_BURROW_DETACH must refuse the EXEC alias of a code region");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, reg.writer_va, JIT_LEN), -1,
        "SYS_BURROW_DETACH must refuse the WRITER alias of a code region");

    // Refused means untouched: both aliases live, the charge unchanged. If the
    // gate were absent, the two calls above would have refunded 2x one charge
    // and page_count would read 0 here.
    TEST_ASSERT(vma_lookup(p, reg.writer_va) != NULL, "writer alias survives");
    TEST_ASSERT(vma_lookup(p, reg.exec_va) != NULL,   "exec alias survives");
    TEST_EXPECT_EQ(jit_pages(p), charged,
        "a refused detach must not refund the region's charge");

    // And the region is still destroyable the ONLY correct way.
    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0,
        "SYS_JIT_DESTROY still works after the refused detaches");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "destroy refunds exactly once");

    // A plain anon mapping is of course still detachable -- the gate is narrow.
    s64 anon = sys_burrow_attach_for_proc(p, 4096);
    TEST_ASSERT(anon > 0, "burrow_attach");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, (u64)anon, 4096), 0,
        "an ordinary anon mapping is still detachable");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "anon detach refunds");

    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// SYS_ICACHE_SYNC: region-confined publish.
// ---------------------------------------------------------------------------
void test_jit_icache_sync_gate(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");
    struct Burrow *b = vma_lookup(p, reg.writer_va)->burrow;

    // B-2a: over pages nothing has touched, a sync has nothing to publish and
    // must not commit them -- a publish that faulted pages in would charge a
    // JIT for its whole reservation.
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va, JIT_LEN), 0,
        "sync over an untouched region succeeds");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 0u, "and commits nothing");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "and charges nothing");
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);

    // Either alias names the range legitimately -- both map the same physical
    // pages, so a JIT may publish through whichever pointer it holds. The
    // region is half committed now: the walk must take the committed page and
    // step over the other.
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va, JIT_LEN), 0,
        "sync over the whole region via the writer alias");
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.exec_va, JIT_LEN), 0,
        "sync over the whole region via the exec alias");
    // A sub-range, unaligned, spanning the page boundary -- the per-page loop.
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va + 17, 8000), 0,
        "sync an unaligned sub-range spanning a page boundary");

    // Degenerate ranges.
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va, 0), -T_E_INVAL,
        "zero length rejected");
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va, ~0ull), -T_E_INVAL,
        "a wrapping range rejected");

    // The containment property: a range that starts inside an alias must not
    // spill past its end, even by one page, and even though the pages just
    // past it belong to the SAME region (see below). A sync spanning two VMAs
    // is refused outright rather than clipped.
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va, JIT_LEN + 4096), -T_E_INVAL,
        "a range extending past the alias is rejected (no spanning, no clipping)");
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va + JIT_LEN - 4096, 8192),
        -T_E_INVAL, "a range straddling the alias end is rejected");

    // NOTE for the next reader: `writer_va + JIT_LEN` is NOT a valid negative
    // case. vma_find_gap is first-fit and the writer VMA is inserted before the
    // second gap search, so the exec alias lands IMMEDIATELY after the writer
    // alias -- that address is the base of a legitimately syncable alias, not
    // unmapped space. The adjacency is sound (each VMA carries its own prot, and
    // a crossing in either direction hits a permission boundary that faults:
    // writing past the writer's end lands in the read-only exec alias, and
    // executing past the exec alias' end lands in the UXN writer alias), but it
    // means "one page past the region" must be tested with a genuinely unmapped
    // VA -- which is the next case.

    // The region confinement that gives I-42 its "specific Burrow, not
    // arbitrary process memory" property: an ordinary anon mapping is NOT
    // syncable, even though the caller owns it.
    s64 anon = sys_burrow_attach_for_proc(p, 4096);
    TEST_ASSERT(anon > 0, "burrow_attach for the negative case");
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, (u64)anon, 4096), -T_E_INVAL,
        "SYS_ICACHE_SYNC must refuse a non-CODE mapping");

    // An unmapped VA.
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, EXEC_USER_BURROW_TOP - 4096, 4096),
        -T_E_INVAL, "an unmapped VA is rejected");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "cleanup");
    // After teardown the range is no longer syncable.
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va, JIT_LEN), -T_E_INVAL,
        "a destroyed region is no longer syncable");

    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// The write-then-read path through both aliases: what a JIT actually does.
// ---------------------------------------------------------------------------
void test_jit_write_through_writer_visible_at_exec(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");

    struct Vma *w = vma_lookup(p, reg.writer_va);
    TEST_ASSERT(w != NULL && w->burrow != NULL, "writer VMA");
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    jit_fault_in(p, reg.writer_va + ONE_PAGE, /*is_write=*/true);

    // Committed code pages are ZERO. That is load-bearing, not hygiene: zero
    // decodes as AArch64 UDF #0, so an un-emitted page traps instead of
    // executing whatever the previous owner of the page left behind.
    u32 *kva  = (u32 *)pa_to_kva(page_to_pa(jit_slot(w->burrow, 0)));
    u32 *kva1 = (u32 *)pa_to_kva(page_to_pa(jit_slot(w->burrow, 1)));
    TEST_EXPECT_EQ(kva[0], 0u, "a fresh code page is zero (UDF #0, not residue)");
    TEST_EXPECT_EQ(kva1[(ONE_PAGE / 4) - 1], 0u, "the whole region is zero");

    // Emit through the writer alias' backing and observe it under the exec
    // alias' backing -- the aliases share pages, so this is the same store the
    // userspace JIT makes. `ret` (0xd65f03c0) is the real instruction the
    // CL-7k-2 prover ends its emitted function with.
    kva[0] = 0xd65f03c0u;
    TEST_EXPECT_EQ(sys_icache_sync_for_proc(p, reg.writer_va, JIT_LEN), 0,
        "publish the emitted bytes");

    // Read back via the EXEC alias' own VMA -> same Burrow -> same page.
    struct Vma *x = vma_lookup(p, reg.exec_va);
    TEST_ASSERT(x != NULL && x->burrow == w->burrow, "exec VMA shares the Burrow");
    jit_fault_in(p, reg.exec_va, /*is_write=*/false);
    u32 *kva_x = (u32 *)pa_to_kva(jit_walk_l3(p->as->pgtable_root, reg.exec_va) & PTE_PA_MASK);
    TEST_EXPECT_EQ(kva_x[0], 0xd65f03c0u,
        "bytes written through the writer alias are visible under the exec alias");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "cleanup");
    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// SYS_ICACHE_SYNC's I-side policy: which CTR_EL0 values make the sync
// invalidate the whole I-cache. A VA-indexed invalidate by the direct-map VA is
// exact only on a PIPT I-cache.
// ---------------------------------------------------------------------------
void test_jit_icache_policy_decode(void) {
    // L1Ip is CTR_EL0 bits 15:14. The other fields are a Cortex-A53's (VIPT,
    // 0x84448004) and a Cortex-A72's (PIPT, 0x8444c004).
    TEST_ASSERT(!hw_ctr_icache_aliases(0x8444c004ull), "PIPT (0b11) does not alias");
    TEST_ASSERT(hw_ctr_icache_aliases(0x84448004ull),  "VIPT (0b10) aliases");
    TEST_ASSERT(hw_ctr_icache_aliases(0x84444004ull),  "AIVIVT (0b01) is treated as aliasing");
    TEST_ASSERT(hw_ctr_icache_aliases(0x84440004ull),  "VPIPT (0b00) is treated as aliasing");

    // The boot CPU recorded its own: the policy agrees with what it reports.
    u64 ctr;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    if (hw_ctr_icache_aliases(ctr))
        TEST_ASSERT(hw_icache_aliasing(), "an aliasing CPU set the policy");
}

// ---------------------------------------------------------------------------
// The commit of a code page invalidates the I-cache over it (CL-7k-3 F1, at the
// commit since B-2a); an anonymous commit does not. The QEMU targets model no
// I-cache, so the witness is that the sync RAN: counted, with the anonymous
// commit as the control that the count is not simply every fault's.
// ---------------------------------------------------------------------------
void test_jit_commit_invalidates_icache(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");
    s64 anon = sys_burrow_attach_lazy_for_proc(p, ONE_PAGE);
    TEST_ASSERT(anon > 0, "a lazy anonymous page for the control");

    u64 c0 = __atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED);
    jit_fault_in(p, (u64)anon, /*is_write=*/true);
    TEST_EXPECT_EQ(__atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED), c0,
        "control: an anonymous commit runs no I-cache sync");

    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    u64 c1 = __atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED);
    TEST_EXPECT_EQ(c1, c0 + 1, "a code commit through the writer invalidates once");

    jit_fault_in(p, reg.exec_va, /*is_write=*/false);
    TEST_EXPECT_EQ(__atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED), c1,
        "the other alias maps the committed page without another");

    jit_fault_in(p, reg.exec_va + ONE_PAGE, /*is_write=*/false);
    TEST_EXPECT_EQ(__atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED), c1 + 1,
        "a commit through the exec alias invalidates too");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "cleanup");
    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// An instruction fetch is admitted by the exec alias's EXEC bit and refused
// through the writer alias before it can commit, charge or sync a page. The
// other first-touch legs in this file are reads, which the exec alias's READ
// bit admits as well, so they cannot tell the two bits apart.
// ---------------------------------------------------------------------------
static enum fault_result jit_fetch(struct Proc *p, u64 vaddr) {
    struct fault_info fi;
    fi.vaddr           = vaddr;
    fi.elr             = vaddr;
    fi.esr             = 0;
    fi.ec              = 0x20;          // instruction abort from EL0
    fi.fsc             = 0x07;
    fi.fault_level     = 3;
    fi.from_user       = true;
    fi.is_instruction  = true;
    fi.is_write        = false;
    fi.is_translation  = true;
    fi.is_permission   = false;
    fi.is_access_flag  = false;
    fi.is_alignment    = false;
    fi.is_external     = false;
    return userland_demand_page(p, &fi);
}

void test_jit_fetch_admission(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");
    struct Burrow *b = vma_lookup(p, reg.writer_va)->burrow;
    u64 c0 = __atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED);

    TEST_EXPECT_EQ(jit_fetch(p, reg.writer_va), FAULT_UNHANDLED_USER,
        "a fetch through the writer alias is refused");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 0u, "and commits nothing");
    TEST_EXPECT_EQ(jit_pages(p), 0u, "and charges nothing");
    TEST_EXPECT_EQ(__atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED), c0,
        "and syncs nothing");

    TEST_EXPECT_EQ(jit_fetch(p, reg.exec_va), FAULT_HANDLED,
        "a fetch through the exec alias commits the page");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 1u, "one page committed");
    TEST_EXPECT_EQ(jit_pages(p), burrow_lazy_footprint(b), "and charged once");
    TEST_EXPECT_EQ(__atomic_load_n(&g_icache_sync_calls_for_test, __ATOMIC_RELAXED), c0 + 1,
        "and invalidated before the leaf");
    u64 pte_x = jit_walk_l3(p->as->pgtable_root, reg.exec_va);
    TEST_ASSERT(pte_x != 0 && (pte_x & BIT_UXN) == 0, "the fetch installed an executable leaf");
    TEST_EXPECT_EQ(pte_x & PTE_PA_MASK, page_to_pa(jit_slot(b, 0)),
        "over the committed page");

    // Residency changes nothing about the writer: the page it would fetch is
    // committed now, and the fetch is still refused.
    TEST_EXPECT_EQ(jit_fetch(p, reg.writer_va), FAULT_UNHANDLED_USER,
        "a fetch through the writer alias of a committed page is refused");
    TEST_EXPECT_EQ(jit_walk_l3(p->as->pgtable_root, reg.writer_va), 0ull,
        "and installs no writer leaf");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "cleanup");
    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// On an aliasing I-cache every sync invalidates the whole I-cache. Forced on
// here, since no target this suite boots on reports one.
// ---------------------------------------------------------------------------
void test_jit_icache_aliasing_invalidates_all(void) {
    static u8 buf[64];
    bool was = hw_icache_aliasing();

    u64 a0 = __atomic_load_n(&g_icache_sync_all_for_test, __ATOMIC_RELAXED);
    hw_icache_aliasing_force_for_test(false);
    arch_icache_sync_range(buf, sizeof buf);
    TEST_EXPECT_EQ(__atomic_load_n(&g_icache_sync_all_for_test, __ATOMIC_RELAXED), a0,
        "control: a PIPT I-cache is invalidated by VA");

    hw_icache_aliasing_force_for_test(true);
    arch_icache_sync_range(buf, sizeof buf);
    u64 a1 = __atomic_load_n(&g_icache_sync_all_for_test, __ATOMIC_RELAXED);
    hw_icache_aliasing_force_for_test(was);
    TEST_EXPECT_EQ(a1, a0 + 1, "an aliasing I-cache is invalidated whole");
}
