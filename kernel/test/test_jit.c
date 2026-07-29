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
    enum fault_result r = userland_demand_page(p, &fi);
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "demand_page must resolve a code-region VA");
}

// Count this Proc's VMAs backed by `b`. The dual-map property in one number.
static int jit_alias_count(struct Proc *p, const struct Burrow *b) {
    int n = 0;
    for (struct Vma *v = p->vmas; v; v = v->next)
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
    TEST_EXPECT_EQ(p->page_count, 0u,
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
    TEST_EXPECT_EQ(p->page_count, 0u, "no charge on a rejected create");

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

    // Fault BOTH aliases in through the real fault path, then read the actual
    // hardware descriptors. This is the assertion that matters: the MMU
    // consults PTEs, not VMA prots.
    jit_fault_in(p, reg.writer_va, /*is_write=*/true);
    jit_fault_in(p, reg.exec_va,   /*is_write=*/false);

    u64 pte_w = jit_walk_l3(p->pgtable_root, reg.writer_va);
    u64 pte_x = jit_walk_l3(p->pgtable_root, reg.exec_va);
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
    TEST_EXPECT_EQ(pte_w & PTE_PA_MASK, page_to_pa(w->burrow->pages),
        "and that page is the Burrow's own backing");

    // Page 2 as well, so the property is not an artifact of the first page.
    jit_fault_in(p, reg.writer_va + ONE_PAGE, /*is_write=*/true);
    jit_fault_in(p, reg.exec_va + ONE_PAGE,   /*is_write=*/false);
    u64 pte_w2 = jit_walk_l3(p->pgtable_root, reg.writer_va + ONE_PAGE);
    u64 pte_x2 = jit_walk_l3(p->pgtable_root, reg.exec_va + ONE_PAGE);
    TEST_EXPECT_EQ(pte_w2 & PTE_PA_MASK, pte_x2 & PTE_PA_MASK,
        "page 2: aliases still map one physical page");
    TEST_ASSERT((pte_w2 & BIT_UXN) != 0, "page 2: writer still non-executable");
    TEST_ASSERT((pte_x2 & BIT_UXN) == 0, "page 2: exec still executable");

    jit_drop_proc(p);
}

// ---------------------------------------------------------------------------
// I-32: one region, ONE charge -- not one per alias.
// ---------------------------------------------------------------------------
void test_jit_charges_once_per_region(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");
    TEST_EXPECT_EQ(p->page_count, 0u, "fresh Proc charged nothing");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");

    // Two aliases, ONE set of physical pages -> JIT_LEN/PAGE_SIZE charged, not
    // twice that. A double charge would bill a JIT for memory it holds once and
    // would leave the destroy-side refund wrong in the other direction.
    TEST_EXPECT_EQ(p->page_count, (u32)(JIT_LEN / 4096u),
        "one region charges its page count ONCE, not once per alias");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "destroy succeeds");
    TEST_EXPECT_EQ(p->page_count, 0u,
        "destroy refunds exactly what create charged");

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
// SYS_ICACHE_SYNC: region-confined publish.
// ---------------------------------------------------------------------------
void test_jit_icache_sync_gate(void) {
    struct Proc *p = jit_make_proc(/*with_cap=*/true);
    TEST_ASSERT(p != NULL, "proc_alloc failed");

    struct t_jit_region reg = { 0, 0 };
    TEST_EXPECT_EQ(sys_jit_create_region(p, JIT_LEN, &reg.writer_va, &reg.exec_va), 0,
        "create succeeds");

    // Either alias names the range legitimately -- both map the same physical
    // pages, so a JIT may publish through whichever pointer it holds.
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

    // Fresh code pages are ZERO. That is load-bearing, not hygiene: zero
    // decodes as AArch64 UDF #0, so an un-emitted page traps instead of
    // executing whatever the previous owner of the page left behind.
    u32 *kva = (u32 *)pa_to_kva(page_to_pa(w->burrow->pages));
    TEST_EXPECT_EQ(kva[0], 0u, "a fresh code page is zero (UDF #0, not residue)");
    TEST_EXPECT_EQ(kva[(JIT_LEN / 4) - 1], 0u, "the whole region is zero");

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
    u32 *kva_x = (u32 *)pa_to_kva(page_to_pa(x->burrow->pages));
    TEST_EXPECT_EQ(kva_x[0], 0xd65f03c0u,
        "bytes written through the writer alias are visible under the exec alias");

    TEST_EXPECT_EQ(sys_jit_destroy_for_proc(p, reg.writer_va), 0, "cleanup");
    jit_drop_proc(p);
}
