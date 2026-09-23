// B-1a: the permission ceiling (ARCH 6.5 "The permission ceiling"; I-12 + I-44;
// specs/cow.tla's ALLOW_PROTECT extension). Drives the _for_proc inners of
// SYS_BURROW_RESERVE / SYS_BURROW_PROTECT, the fault arm, and addrspace_clone on
// fresh proc_alloc'd Procs.
//
//   protect.reserve_mints_exactly
//     a reservation is minted at the prot asked for -- none faults, R installs
//     read-only, RW writable -- under an RW ceiling; align_log2 aligns the base.
//   protect.reserve_refusals
//     X / W-only / stray bits / a bad alignment / length 0 / over the max, each
//     with its own errno, and nothing mapped afterwards.
//   protect.raise_and_write_keeps_contents
//     none -> RW -> write; then none again (the PTE is gone, the page and its
//     charge stay) -> RW again: the same page comes back with its bytes.
//   protect.x_refused_before_lookup
//     protect(X) over an UNMAPPED range answers EACCES where protect(R) answers
//     ENOMEM: the refusal precedes the lookup, observably.
//   protect.ceiling_bounds_raise
//     an eager mapping minted R (the vDSO's shape) cannot be raised; one minted
//     RX descends to R and never returns.
//   protect.seal_lowers_ceiling
//     PROTECT_SEAL makes the new prot the ceiling, irrevocably; sealed at none
//     the range is a guard.
//   protect.split_three_way_then_merge
//     a range inside one mapping cuts it into three pieces of one Burrow with
//     every surviving byte's identity unchanged; the reverse protect merges
//     them back into ONE mapping. Nothing leaks: pages and Vma structs return
//     to their counts.
//   protect.grow_ladder_stays_two_vmas
//     the engine pattern -- reserve none, commit page by page -- never holds
//     more than two VMAs (the merge pass), which is what keeps a 64 KiB-paged
//     4 GiB reservation off PROC_VMA_MAX.
//   protect.refusals_change_nothing
//     a guard, a shared-in mapping, a CODE alias, an unaligned address, unknown
//     flags, a zero length: each refused with its errno, and the mapping is
//     byte-identical afterwards.
//   protect.multi_vma_and_hole
//     a range across two Burrows changes both (and they do not merge); a range
//     with a hole -- leading, interior or trailing -- changes nothing.
//   protect.pte_uninstalled_then_reinstalled_at_prot
//     the D-3b rule as observed through the page table: a lowered prot takes
//     the writable PTE with it; the re-fault installs at the NEW prot.
//   protect.cow_split_then_break
//     a protect on a forked (COW) mapping cuts it soundly: the child's break on
//     a page in one piece copies, the parent's page is untouched.
//   cow.clone_dedupes_split_pieces
//     a Burrow split into pieces is cloned ONCE per fork: every child piece maps
//     the one clone, each page has exactly two holders, and the child is
//     charged the resident count once (cow.tla::BUGGY_CLONE_PER_PIECE is the
//     shape without the cursor).
//   cow.clone_refuses_eager_anon_with_writable_ceiling
//     an eager mapping protected down to R is NOT shared across a fork (its
//     ceiling says it can be raised back); one whose ceiling is R is.
//   sys_burrow.detach_piece_frees_only_its_pages
//     detaching one piece of a split lazy Burrow frees and uncharges exactly
//     that piece's resident pages; the other piece's pages stay resident.

#include "test.h"

#include "../../arch/arm64/fault.h"
#include "../../mm/magazines.h"
#include "../../mm/phys.h"

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/cow.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_protect_reserve_mints_exactly(void);
void test_protect_reserve_refusals(void);
void test_protect_raise_and_write_keeps_contents(void);
void test_protect_x_refused_before_lookup(void);
void test_protect_ceiling_bounds_raise(void);
void test_protect_seal_lowers_ceiling(void);
void test_protect_split_three_way_then_merge(void);
void test_protect_grow_ladder_stays_two_vmas(void);
void test_protect_refusals_change_nothing(void);
void test_protect_multi_vma_and_hole(void);
void test_protect_pte_uninstalled_then_reinstalled_at_prot(void);
void test_protect_cow_split_then_break(void);
void test_cow_clone_dedupes_split_pieces(void);
void test_cow_clone_refuses_eager_anon_with_writable_ceiling(void);
void test_sys_burrow_detach_piece_frees_only_its_pages(void);

// The non-static inners of the SVC handlers (kernel/syscall.c).
extern s64 sys_burrow_reserve_for_proc(struct Proc *p, u64 length_raw, u64 prot_raw,
                                       u64 align_log2);
extern s64 sys_burrow_protect_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw,
                                       u64 prot_raw, u64 flags_raw);
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);

#define P            PAGE_SIZE
#define PR_NONE      ((u64)BURROW_PROT_NONE)
#define PR_R         ((u64)BURROW_PROT_READ)
#define PR_RW        ((u64)(BURROW_PROT_READ | BURROW_PROT_WRITE))
#define PR_X         ((u64)BURROW_PROT_EXEC)
#define ERR(e)       (-(s64)(e))

// Explicit-VA mappings sit here, well inside the user half and clear of the
// burrow window the reserves land in.
#define EXPLICIT_VA  0x30000000ull
#define UNMAPPED_VA  0x38000000ull

// AP[2:1] at PTE bits 7:6 -- 0b01 user-RW, 0b11 user-RO.
#define AP_FIELD   (3ull << 6)
#define AP_RW_ANY  (1ull << 6)
#define AP_RO_ANY  (3ull << 6)

static struct Proc *mk(void) { return proc_alloc(); }

static void drop(struct Proc *p) {
    if (!p) return;
    p->state = 2;                        // PROC_STATE_ZOMBIE; proc_free drains VMAs
    proc_free(p);
}

static void mkfi(struct fault_info *fi, u64 vaddr, bool is_write) {
    fi->vaddr          = vaddr;
    fi->elr            = 0;
    fi->esr            = 0;
    fi->ec             = 0x24;          // EC_DATA_ABORT_LOWER
    fi->fsc            = 0x07;          // FSC_TRANS_FAULT_L3
    fi->fault_level    = 3;
    fi->from_user      = true;
    fi->is_instruction = false;
    fi->is_write       = is_write;
    fi->is_translation = true;
    fi->is_permission  = false;
    fi->is_access_flag = false;
}

static enum fault_result fault(struct Proc *p, u64 va, bool is_write) {
    struct fault_info fi;
    mkfi(&fi, va, is_write);
    return userland_demand_page(p, &fi);
}

// The raw L3 leaf covering `vaddr`, or 0 when any level is missing -- "gone"
// and "never there" read the same, which is what the uninstall assertions say.
static u64 pte_of(paddr_t pgtable_root, u64 vaddr) {
    const u64 VALID = 1ull << 0, TABLE = 1ull << 1;
    u64 *t = (u64 *)pa_to_kva(pgtable_root);
    for (int lvl = 0; lvl < 3; lvl++) {
        u64 e = t[(vaddr >> (39 - 9 * lvl)) & 0x1ff];
        if (!(e & VALID) || !(e & TABLE)) return 0;
        t = (u64 *)pa_to_kva(e & 0x0000FFFFFFFFF000ull);
    }
    u64 leaf = t[(vaddr >> 12) & 0x1ff];
    return (leaf & VALID) ? leaf : 0;
}

static u32 count_vmas(struct AddrSpace *as) {
    u32 n = 0;
    for (struct Vma *v = as->vmas; v; v = v->next) n++;
    return n;
}

// The property the whole split rests on (the D-3b tests' byte_identity).
static u64 ident(const struct Vma *v, u64 va) {
    return v->burrow_offset + (va - v->vaddr_start);
}

static s64 reserve(struct Proc *p, u64 len, u64 prot, u64 align_log2) {
    return sys_burrow_reserve_for_proc(p, len, prot, align_log2);
}
static s64 protect(struct Proc *p, u64 va, u64 len, u64 prot, u64 flags) {
    return sys_burrow_protect_for_proc(p, va, len, prot, flags);
}

// Map an EAGER anon Burrow at a chosen VA and prot (the vDSO / ring shapes).
static struct Burrow *map_eager(struct Proc *p, u64 va, u64 len, u32 prot) {
    struct Burrow *b = burrow_create_anon((size_t)len);
    if (!b) return NULL;
    spin_lock(&p->as->lock);
    int rc = burrow_map(p, b, va, (size_t)len, prot);
    spin_unlock(&p->as->lock);
    burrow_unref(b);                     // the mapping keeps it
    return rc == 0 ? b : NULL;
}

// The kernel-side view of a lazy mapping's slot page for `va` (NULL if not
// resident).
static struct page *slot_page(struct Vma *v, u64 va) {
    size_t slot = (size_t)(ident(v, va) / P);
    return burrow_lazy_slot_for_test(v->burrow, slot);
}

static u32 *slot_words(struct Vma *v, u64 va) {
    struct page *pg = slot_page(v, va);
    return pg ? (u32 *)pa_to_kva(page_to_pa(pg)) : NULL;
}

// proc_alloc_in takes its own reference, so the caller drops the one it holds
// and the child owns the space outright (test_cow.c's cow_adopt).
static struct Proc *adopt(struct AddrSpace *as) {
    struct Proc *p = proc_alloc_in(as, PROC_PAGE_MAX);
    if (p) addrspace_unref(as);
    return p;
}

void test_protect_reserve_mints_exactly(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // none: the mapping exists, has a Burrow, and every fault is refused.
    s64 r = reserve(p, 4 * P, PR_NONE, 0);
    TEST_ASSERT(r > 0, "reserve(none) returns a VA");
    u64 va = (u64)r;
    TEST_ASSERT(va >= EXEC_USER_BURROW_BASE && va < EXEC_USER_BURROW_TOP, "in the burrow window");
    struct Vma *v = vma_lookup(p, va);
    TEST_ASSERT(v != NULL, "VMA installed");
    TEST_EXPECT_EQ(v->prot, 0u, "minted at none");
    TEST_ASSERT(v->burrow != NULL && v->burrow->type == BURROW_TYPE_ANON_LAZY,
                "a lazy anon Burrow backs it (not a guard)");
    TEST_EXPECT_EQ(vma_prot_max(v), (u32)VMA_PROT_RW, "the ceiling is RW");
    TEST_EXPECT_EQ(v->flags & VMA_FLAG_STATE_MASK, 0u, "no state flags");
    TEST_EXPECT_EQ(fault(p, va, false), FAULT_UNHANDLED_USER, "a read of none is refused");
    TEST_EXPECT_EQ(fault(p, va, true),  FAULT_UNHANDLED_USER, "a write of none is refused");
    TEST_ASSERT(slot_page(v, va) == NULL, "a refused fault allocates nothing");

    // R: reads install read-only; writes are refused.
    r = reserve(p, P, PR_R, 0);
    TEST_ASSERT(r > 0, "reserve(R)");
    u64 vr = (u64)r;
    struct Vma *vv = vma_lookup(p, vr);
    TEST_EXPECT_EQ(vv->prot, (u32)VMA_PROT_READ, "minted at R");
    TEST_EXPECT_EQ(vma_prot_max(vv), (u32)VMA_PROT_RW, "R under an RW ceiling");
    TEST_EXPECT_EQ(fault(p, vr, true),  FAULT_UNHANDLED_USER, "a write of R is refused");
    TEST_EXPECT_EQ(fault(p, vr, false), FAULT_HANDLED,        "a read of R installs");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, vr) & AP_FIELD, AP_RO_ANY, "installed read-only");

    // RW: the ordinary mapping.
    r = reserve(p, P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve(RW)");
    u64 vw = (u64)r;
    TEST_EXPECT_EQ(vma_lookup(p, vw)->prot, (u32)VMA_PROT_RW, "minted at RW");
    TEST_EXPECT_EQ(fault(p, vw, true), FAULT_HANDLED, "a write of RW installs");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, vw) & AP_FIELD, AP_RW_ANY, "installed writable");

    // Alignment: two 1 MiB-aligned reservations are aligned and distinct.
    s64 a1 = reserve(p, P, PR_RW, 20);
    s64 a2 = reserve(p, P, PR_RW, 20);
    TEST_ASSERT(a1 > 0 && a2 > 0, "aligned reserves succeed");
    TEST_EXPECT_EQ((u64)a1 & ((1ull << 20) - 1), 0ull, "first base 1 MiB-aligned");
    TEST_EXPECT_EQ((u64)a2 & ((1ull << 20) - 1), 0ull, "second base 1 MiB-aligned");
    TEST_ASSERT((u64)a2 >= (u64)a1 + (1ull << 20), "the second lands past the first");
    // And a page-aligned one after them still first-fits into the gap the
    // alignment skipped -- alignment costs the aligned mapping only.
    s64 a3 = reserve(p, P, PR_RW, 0);
    TEST_ASSERT(a3 > 0 && (u64)a3 < (u64)a1, "a plain reserve takes the skipped gap");

    drop(p);
}

void test_protect_reserve_refusals(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    TEST_EXPECT_EQ(reserve(p, P, PR_X, 0),          ERR(T_E_ACCES), "X is never a mint (EACCES)");
    TEST_EXPECT_EQ(reserve(p, P, PR_R | PR_X, 0),   ERR(T_E_ACCES), "RX is never a mint (EACCES)");
    TEST_EXPECT_EQ(reserve(p, P, (u64)BURROW_PROT_WRITE, 0), ERR(T_E_INVAL), "W alone (EINVAL)");
    TEST_EXPECT_EQ(reserve(p, P, 8, 0),             ERR(T_E_INVAL), "a stray prot bit (EINVAL)");
    TEST_EXPECT_EQ(reserve(p, P, PR_RW, 11),        ERR(T_E_INVAL), "align below a page (EINVAL)");
    TEST_EXPECT_EQ(reserve(p, P, PR_RW, 31),        ERR(T_E_INVAL), "align above 1 GiB (EINVAL)");
    TEST_EXPECT_EQ(reserve(p, 0, PR_RW, 0),         ERR(T_E_INVAL), "length 0 (EINVAL)");
    TEST_EXPECT_EQ(reserve(p, BURROW_RESERVE_MAX + 1, PR_RW, 0), ERR(T_E_NOMEM),
                   "over the reservation max (ENOMEM)");
    TEST_EXPECT_EQ(reserve(NULL, P, PR_RW, 0),      ERR(T_E_INVAL), "no Proc (EINVAL)");
    TEST_EXPECT_EQ(count_vmas(p->as), 0u, "every refusal mapped nothing");

    drop(p);
}

void test_protect_raise_and_write_keeps_contents(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    s64 r = reserve(p, 2 * P, PR_NONE, 0);
    TEST_ASSERT(r > 0, "reserve(none)");
    u64 va = (u64)r;
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_RW, 0), 0, "raise none -> RW within the ceiling");
    struct Vma *v = vma_lookup(p, va);
    TEST_EXPECT_EQ(v->prot, (u32)VMA_PROT_RW, "prot is RW");
    TEST_EXPECT_EQ(fault(p, va, true), FAULT_HANDLED, "the write faults in");
    u32 *w = slot_words(v, va);
    TEST_ASSERT(w != NULL, "the page is resident");
    w[0] = 0x5a5a5a5au;
    struct page *pg = slot_page(v, va);
    u32 charged = p->as->page_count;

    // Lower to none: the PTE goes, the page and the charge stay.
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_NONE, 0), 0, "lower RW -> none");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, va), 0ull, "the PTE is uninstalled");
    TEST_EXPECT_EQ(fault(p, va, true), FAULT_UNHANDLED_USER, "a write is refused at none");
    TEST_ASSERT(slot_page(vma_lookup(p, va), va) == pg, "the page stays resident (Linux keeps a PROT_NONE mapping's contents)");
    TEST_EXPECT_EQ(p->as->page_count, charged, "and stays charged");

    // Raise again: the SAME page comes back with its bytes.
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_RW, 0), 0, "raise none -> RW again");
    TEST_EXPECT_EQ(fault(p, va, true), FAULT_HANDLED, "re-faults in");
    v = vma_lookup(p, va);
    TEST_ASSERT(slot_page(v, va) == pg, "the same page");
    TEST_EXPECT_EQ(slot_words(v, va)[0], 0x5a5a5a5au, "with its bytes");
    TEST_EXPECT_EQ(p->as->page_count, charged, "charged exactly once");

    drop(p);
}

void test_protect_x_refused_before_lookup(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_ASSERT(vma_lookup(p, UNMAPPED_VA) == NULL, "the range is unmapped");

    // The control: a lookup happens, and it finds nothing.
    TEST_EXPECT_EQ(protect(p, UNMAPPED_VA, P, PR_R, 0), ERR(T_E_NOMEM),
                   "protect(R) over nothing is ENOMEM -- the lookup ran");
    // The claim: X is refused before that lookup could have answered.
    TEST_EXPECT_EQ(protect(p, UNMAPPED_VA, P, PR_X, 0), ERR(T_E_ACCES),
                   "protect(X) over nothing is EACCES -- refused before the lookup");
    TEST_EXPECT_EQ(protect(p, UNMAPPED_VA, P, PR_R | PR_X, 0), ERR(T_E_ACCES),
                   "protect(RX) likewise");
    // The word checks precede it too.
    TEST_EXPECT_EQ(protect(p, UNMAPPED_VA, P, (u64)BURROW_PROT_WRITE, 0), ERR(T_E_INVAL),
                   "W alone is EINVAL before the lookup");
    TEST_EXPECT_EQ(protect(p, UNMAPPED_VA, P, 8, 0), ERR(T_E_INVAL),
                   "a stray bit is EINVAL before the lookup");

    drop(p);
}

void test_protect_ceiling_bounds_raise(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // The vDSO's shape: an eager anon mapping minted R. Its ceiling is R.
    struct Burrow *b = map_eager(p, EXPLICIT_VA, P, VMA_PROT_READ);
    TEST_ASSERT(b != NULL, "map an eager R mapping");
    struct Vma *v = vma_lookup(p, EXPLICIT_VA);
    TEST_EXPECT_EQ(vma_prot_max(v), (u32)VMA_PROT_READ, "an eager R mint has ceiling R");
    TEST_EXPECT_EQ(protect(p, EXPLICIT_VA, P, PR_RW, 0), ERR(T_E_ACCES), "cannot be raised to RW");
    TEST_EXPECT_EQ(vma_lookup(p, EXPLICIT_VA)->prot, (u32)VMA_PROT_READ, "unchanged after the refusal");
    TEST_EXPECT_EQ(protect(p, EXPLICIT_VA, P, PR_NONE, 0), 0, "can descend to none");
    TEST_EXPECT_EQ(protect(p, EXPLICIT_VA, P, PR_R, 0), 0, "and come back to R");
    TEST_EXPECT_EQ(protect(p, EXPLICIT_VA, P, PR_RW, 0), ERR(T_E_ACCES), "still not RW");

    // An RX mapping descends and never returns.
    u64 xva = EXPLICIT_VA + 4 * P;
    struct Burrow *bx = map_eager(p, xva, P, VMA_PROT_RX);
    TEST_ASSERT(bx != NULL, "map an eager RX mapping");
    TEST_EXPECT_EQ(vma_prot_max(vma_lookup(p, xva)), (u32)VMA_PROT_RX, "ceiling RX");
    TEST_EXPECT_EQ(protect(p, xva, P, PR_R, 0), 0, "RX descends to R");
    TEST_EXPECT_EQ(vma_lookup(p, xva)->prot, (u32)VMA_PROT_READ, "now R");
    TEST_EXPECT_EQ(protect(p, xva, P, PR_R | PR_X, 0), ERR(T_E_ACCES), "X never returns");
    TEST_EXPECT_EQ(protect(p, xva, P, PR_RW, 0), ERR(T_E_ACCES), "W was never in the ceiling");
    TEST_EXPECT_EQ(protect(p, xva, P, PR_NONE, 0), 0, "none is always reachable");

    drop(p);
}

void test_protect_seal_lowers_ceiling(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    s64 r = reserve(p, 2 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve(RW)");
    u64 va = (u64)r;
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_R, BURROW_PROTECT_SEAL), 0, "seal at R");
    TEST_EXPECT_EQ(vma_prot_max(vma_lookup(p, va)), (u32)VMA_PROT_READ, "the ceiling is now R");
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_RW, 0), ERR(T_E_ACCES), "RW is gone for good");
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_RW, BURROW_PROTECT_SEAL), ERR(T_E_ACCES),
                   "sealing cannot raise either");
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_NONE, 0), 0, "descent stays open");
    TEST_EXPECT_EQ(protect(p, va, 2 * P, PR_R, 0), 0, "and R is still inside the ceiling");

    // Sealed at none: a guard page, by construction (ARCH 6.5).
    TEST_EXPECT_EQ(protect(p, va, P, PR_NONE, BURROW_PROTECT_SEAL), 0, "seal page 0 at none");
    TEST_EXPECT_EQ(vma_prot_max(vma_lookup(p, va)), 0u, "ceiling none");
    TEST_EXPECT_EQ(protect(p, va, P, PR_R, 0), ERR(T_E_ACCES), "a guard cannot be raised");
    TEST_EXPECT_EQ(vma_prot_max(vma_lookup(p, va + P)), (u32)VMA_PROT_READ,
                   "the seal reached only its range: page 1 keeps ceiling R");

    drop(p);
}

void test_protect_split_three_way_then_merge(void) {
    magazines_drain_all();
    u64 free0  = phys_free_pages();
    u64 vmas0  = vma_total_allocated() - vma_total_freed();

    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    s64 r = reserve(p, 8 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 8 pages RW");
    u64 va = (u64)r;
    struct Burrow *b = vma_lookup(p, va)->burrow;
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "one mapping");

    // Cut [2, 5) out of [0, 8): three pieces of one Burrow.
    TEST_EXPECT_EQ(protect(p, va + 2 * P, 3 * P, PR_NONE, 0), 0, "protect the middle");
    TEST_EXPECT_EQ(count_vmas(p->as), 3u, "three VMAs");
    struct Vma *v0 = vma_lookup(p, va);
    struct Vma *v1 = vma_lookup(p, va + 2 * P);
    struct Vma *v2 = vma_lookup(p, va + 5 * P);
    TEST_ASSERT(v0 && v1 && v2 && v0 != v1 && v1 != v2, "three distinct pieces");
    TEST_EXPECT_EQ(v0->vaddr_start, va,         "left starts at the base");
    TEST_EXPECT_EQ(v0->vaddr_end,   va + 2 * P, "left ends at the cut");
    TEST_EXPECT_EQ(v1->vaddr_start, va + 2 * P, "middle starts at the cut");
    TEST_EXPECT_EQ(v1->vaddr_end,   va + 5 * P, "middle ends at the second cut");
    TEST_EXPECT_EQ(v2->vaddr_start, va + 5 * P, "right starts at the second cut");
    TEST_EXPECT_EQ(v2->vaddr_end,   va + 8 * P, "right ends at the old end");
    TEST_EXPECT_EQ(v0->prot, (u32)VMA_PROT_RW, "left keeps RW");
    TEST_EXPECT_EQ(v1->prot, 0u,               "middle is none");
    TEST_EXPECT_EQ(v2->prot, (u32)VMA_PROT_RW, "right keeps RW");
    TEST_ASSERT(v0->burrow == b && v1->burrow == b && v2->burrow == b, "one Burrow");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 3, "three mappings of it");
    TEST_EXPECT_EQ(ident(v0, va),         0ull,            "identity: left");
    TEST_EXPECT_EQ(ident(v1, va + 3 * P), (u64)(3 * P),    "identity: middle");
    TEST_EXPECT_EQ(ident(v2, va + 7 * P), (u64)(7 * P),    "identity: right");
    TEST_EXPECT_EQ(vma_prot_max(v0), (u32)VMA_PROT_RW, "ceiling carried: left");
    TEST_EXPECT_EQ(vma_prot_max(v1), (u32)VMA_PROT_RW, "ceiling carried: middle");
    TEST_EXPECT_EQ(vma_prot_max(v2), (u32)VMA_PROT_RW, "ceiling carried: right");
    TEST_EXPECT_EQ(p->as->vma_count, 3u, "the I-32 VMA count follows");

    // An interior cut inside the middle piece: in place, then a five-way.
    TEST_EXPECT_EQ(protect(p, va + 3 * P, P, PR_NONE, 0), 0, "the same prot inside a piece");
    TEST_EXPECT_EQ(count_vmas(p->as), 3u, "changes nothing and merges nothing");
    TEST_EXPECT_EQ(protect(p, va + 3 * P, P, PR_R, 0), 0, "a different prot inside a piece");
    TEST_EXPECT_EQ(count_vmas(p->as), 5u, "five VMAs");
    TEST_EXPECT_EQ(vma_lookup(p, va + 3 * P)->prot, (u32)VMA_PROT_READ, "the cut page is R");
    TEST_EXPECT_EQ(ident(vma_lookup(p, va + 4 * P), va + 4 * P), (u64)(4 * P), "identity holds");

    // The reverse protect merges everything back into ONE mapping.
    TEST_EXPECT_EQ(protect(p, va, 8 * P, PR_RW, 0), 0, "RW over the whole range");
    TEST_EXPECT_EQ(count_vmas(p->as), 1u, "merged back to one VMA");
    struct Vma *m = vma_lookup(p, va);
    TEST_EXPECT_EQ(m->vaddr_start, va,         "merged start");
    TEST_EXPECT_EQ(m->vaddr_end,   va + 8 * P, "merged end");
    TEST_EXPECT_EQ(m->burrow_offset, 0ull,     "merged offset");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "one mapping again");
    TEST_EXPECT_EQ(p->as->vma_count, 1u,       "count follows the merge");

    drop(p);
    magazines_drain_all();
    TEST_EXPECT_EQ(phys_free_pages(), free0, "no page leaked across split + merge + teardown");
    TEST_EXPECT_EQ(vma_total_allocated() - vma_total_freed(), vmas0, "no Vma struct leaked");
}

void test_protect_grow_ladder_stays_two_vmas(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    s64 r = reserve(p, 8 * P, PR_NONE, 0);
    TEST_ASSERT(r > 0, "reserve 8 pages none");
    u64 va = (u64)r;
    // Commit page by page from the bottom: rw grows, none shrinks, two VMAs.
    for (u64 i = 0; i < 8; i++) {
        TEST_EXPECT_EQ(protect(p, va + i * P, P, PR_RW, 0), 0, "commit one page");
        TEST_EXPECT_EQ(count_vmas(p->as), i < 7 ? 2u : 1u, "the ladder holds two VMAs");
        struct Vma *rw = vma_lookup(p, va);
        TEST_EXPECT_EQ(rw->vaddr_end, va + (i + 1) * P, "the rw piece grew");
        TEST_EXPECT_EQ(rw->burrow_offset, 0ull, "the rw piece keeps offset 0");
    }
    // Shrink from the top: the released pages merge into one none piece.
    for (u64 i = 7; i >= 1; i--) {
        TEST_EXPECT_EQ(protect(p, va + i * P, P, PR_NONE, 0), 0, "release one page");
        TEST_EXPECT_EQ(count_vmas(p->as), 2u, "two VMAs on the way down");
        struct Vma *none = vma_lookup(p, va + 7 * P);
        TEST_EXPECT_EQ(none->vaddr_start, va + i * P, "the none piece grew downward");
        TEST_EXPECT_EQ(none->vaddr_end,   va + 8 * P, "to the end");
        TEST_EXPECT_EQ(ident(none, va + 7 * P), (u64)(7 * P), "identity across the merges");
    }

    drop(p);
}

void test_protect_refusals_change_nothing(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    s64 r = reserve(p, 4 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 4 pages RW");
    u64 va = (u64)r;
    struct Vma *v = vma_lookup(p, va);
    struct Burrow *b = v->burrow;

    // A guard in the range: reserved address space is not a mapping.
    struct Vma *g = vma_alloc_guard(EXPLICIT_VA, EXPLICIT_VA + P);
    TEST_ASSERT(g != NULL, "guard alloc");
    spin_lock(&p->as->lock);
    TEST_EXPECT_EQ(vma_insert(p, g), 0, "guard insert");
    spin_unlock(&p->as->lock);
    TEST_EXPECT_EQ(protect(p, EXPLICIT_VA, P, PR_R, 0), ERR(T_E_NOMEM), "a guard is ENOMEM");
    TEST_EXPECT_EQ(vma_lookup(p, EXPLICIT_VA)->prot, 0u, "and stays a guard");

    // Shared-in: another Proc's memory. Set the flag directly (a real share
    // would test burrow_share_into, not this).
    v->flags |= VMA_FLAG_SHARED_IN;
    TEST_EXPECT_EQ(protect(p, va, P, PR_R, 0), ERR(T_E_ACCES), "shared-in is EACCES");
    v->flags &= ~VMA_FLAG_SHARED_IN;

    // A CODE alias (the I-42 pair).
    u64 cva = EXPLICIT_VA + 4 * P;
    struct Burrow *bc = burrow_create_code(P);
    TEST_ASSERT(bc != NULL, "code Burrow");
    spin_lock(&p->as->lock);
    TEST_EXPECT_EQ(burrow_map(p, bc, cva, P, VMA_PROT_RW), 0, "map the writer alias");
    spin_unlock(&p->as->lock);
    burrow_unref(bc);
    TEST_EXPECT_EQ(protect(p, cva, P, PR_R, 0), ERR(T_E_ACCES), "a CODE alias is EACCES");
    TEST_EXPECT_EQ(vma_lookup(p, cva)->prot, (u32)VMA_PROT_RW, "and is unchanged");

    // Malformed calls.
    TEST_EXPECT_EQ(protect(p, va + 1, P, PR_R, 0), ERR(T_E_INVAL), "unaligned vaddr");
    TEST_EXPECT_EQ(protect(p, va, P, PR_R, 2),     ERR(T_E_INVAL), "unknown flags");
    TEST_EXPECT_EQ(protect(p, va, 0, PR_R, 0),     ERR(T_E_INVAL), "zero length");
    TEST_EXPECT_EQ(protect(p, va, P, 8, 0),        ERR(T_E_INVAL), "stray prot bit");
    TEST_EXPECT_EQ(protect(p, va, P, (u64)BURROW_PROT_WRITE, 0), ERR(T_E_INVAL), "W alone");
    TEST_EXPECT_EQ(protect(p, va, (u64)-1 - P, PR_R, 0), ERR(T_E_INVAL), "a length that wraps");

    // A range running off the end of the mapping into a hole: refused whole.
    TEST_EXPECT_EQ(protect(p, va + 2 * P, 4 * P, PR_R, 0), ERR(T_E_NOMEM), "a trailing hole");

    // NOTHING above may have cut. One VMA, original bounds, original prot.
    struct Vma *still = vma_lookup(p, va);
    TEST_ASSERT(still == v, "the same Vma struct");
    TEST_EXPECT_EQ(still->vaddr_start, va,          "start untouched");
    TEST_EXPECT_EQ(still->vaddr_end,   va + 4 * P,  "end untouched");
    TEST_EXPECT_EQ(still->burrow_offset, 0ull,      "offset untouched");
    TEST_EXPECT_EQ(still->prot, (u32)VMA_PROT_RW,   "prot untouched");
    TEST_EXPECT_EQ(still->flags & VMA_FLAG_STATE_MASK, 0u, "flags untouched");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1,      "one mapping -- no refusal left a seam");
    TEST_ASSERT(vma_lookup(p, va + 3 * P) == v,     "the whole span is still ONE VMA");

    drop(p);
}

void test_protect_multi_vma_and_hole(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // Two reservations first-fit adjacently in an empty window.
    s64 a = reserve(p, 2 * P, PR_RW, 0);
    s64 bb = reserve(p, 2 * P, PR_RW, 0);
    TEST_ASSERT(a > 0 && bb > 0, "two reserves");
    TEST_EXPECT_EQ((u64)bb, (u64)a + 2 * P, "adjacent (first-fit)");
    TEST_EXPECT_EQ(protect(p, (u64)a, 4 * P, PR_R, 0), 0, "one protect across two Burrows");
    TEST_EXPECT_EQ(vma_lookup(p, (u64)a)->prot,  (u32)VMA_PROT_READ, "the first changed");
    TEST_EXPECT_EQ(vma_lookup(p, (u64)bb)->prot, (u32)VMA_PROT_READ, "the second changed");
    TEST_EXPECT_EQ(count_vmas(p->as), 2u, "different Burrows never merge");

    // Holes: X at [0,1), Y at [3,4) of a 4-page span.
    u64 h = EXPLICIT_VA;
    TEST_ASSERT(map_eager(p, h, P, VMA_PROT_RW) != NULL,         "map X");
    TEST_ASSERT(map_eager(p, h + 3 * P, P, VMA_PROT_RW) != NULL, "map Y");
    TEST_EXPECT_EQ(protect(p, h, 4 * P, PR_R, 0),         ERR(T_E_NOMEM), "an interior hole");
    TEST_EXPECT_EQ(protect(p, h, 2 * P, PR_R, 0),         ERR(T_E_NOMEM), "a trailing hole");
    TEST_EXPECT_EQ(protect(p, h + P, 3 * P, PR_R, 0),     ERR(T_E_NOMEM), "a leading hole");
    TEST_EXPECT_EQ(vma_lookup(p, h)->prot,         (u32)VMA_PROT_RW, "X unchanged by every hole refusal");
    TEST_EXPECT_EQ(vma_lookup(p, h + 3 * P)->prot, (u32)VMA_PROT_RW, "Y unchanged by every hole refusal");
    TEST_EXPECT_EQ(protect(p, h, P, PR_R, 0), 0, "exactly X alone is fine");

    drop(p);
}

void test_protect_pte_uninstalled_then_reinstalled_at_prot(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    s64 r = reserve(p, P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve RW");
    u64 va = (u64)r;
    TEST_EXPECT_EQ(fault(p, va, true), FAULT_HANDLED, "write faults in");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, va) & AP_FIELD, AP_RW_ANY, "writable PTE");

    // Lower: the writable PTE MUST go (cow.tla::NoWritablePteBeyondProt).
    TEST_EXPECT_EQ(protect(p, va, P, PR_R, 0), 0, "lower to R");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, va), 0ull, "the PTE is gone");
    TEST_EXPECT_EQ(fault(p, va, false), FAULT_HANDLED, "a read re-faults");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, va) & AP_FIELD, AP_RO_ANY, "installed read-only");
    TEST_EXPECT_EQ(fault(p, va, true), FAULT_UNHANDLED_USER, "a write is refused at R");

    // Raise: the read-only PTE goes too, so the install is not a mismatch.
    TEST_EXPECT_EQ(protect(p, va, P, PR_RW, 0), 0, "raise to RW");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, va), 0ull, "the PTE is gone again");
    TEST_EXPECT_EQ(fault(p, va, true), FAULT_HANDLED, "a write re-faults");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, va) & AP_FIELD, AP_RW_ANY, "installed writable");

    drop(p);
}

void test_protect_cow_split_then_break(void) {
    struct Proc *parent = mk();
    TEST_ASSERT(parent != NULL, "proc_alloc parent");

    s64 r = reserve(parent, 4 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 4 pages RW");
    u64 va = (u64)r;
    for (u64 i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(fault(parent, va + i * P, true), FAULT_HANDLED, "parent faults a page in");
        slot_words(vma_lookup(parent, va), va + i * P)[0] = 0x100u + (u32)i;
    }
    struct Vma *pv = vma_lookup(parent, va);
    struct page *pg0 = slot_page(pv, va);

    struct AddrSpace *cas = addrspace_clone(parent->as, /*exempt=*/true);
    TEST_ASSERT(cas != NULL, "addrspace_clone");
    struct Proc *child = adopt(cas);
    TEST_ASSERT(child != NULL, "adopt");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg0), 2ull, "page 0 has two holders");

    // The child protects page 1 to none: a cut through a COW mapping.
    TEST_EXPECT_EQ(protect(child, va + P, P, PR_NONE, 0), 0, "protect inside the COW mapping");
    TEST_EXPECT_EQ(count_vmas(child->as), 3u, "three child pieces");
    struct Vma *c0 = vma_lookup(child, va);
    struct Vma *c1 = vma_lookup(child, va + P);
    struct Vma *c2 = vma_lookup(child, va + 2 * P);
    TEST_ASSERT(c0->burrow == c1->burrow && c1->burrow == c2->burrow, "one clone Burrow");
    TEST_ASSERT((c0->flags & VMA_FLAG_COW) && (c1->flags & VMA_FLAG_COW) && (c2->flags & VMA_FLAG_COW),
                "the COW routing survives the cut on every piece");
    TEST_EXPECT_EQ(count_vmas(parent->as), 1u, "the parent's mapping is untouched");

    // The child writes page 0 (in the first piece): a copy, since it is shared.
    TEST_EXPECT_EQ(fault(child, va, true), FAULT_HANDLED, "child write breaks");
    struct page *cpg0 = slot_page(vma_lookup(child, va), va);
    TEST_ASSERT(cpg0 != NULL && cpg0 != pg0, "the child got a PRIVATE page");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg0), 1ull, "the parent is the sole holder now");
    slot_words(vma_lookup(child, va), va)[0] = 0xC0DEu;
    TEST_EXPECT_EQ(slot_words(vma_lookup(parent, va), va)[0], 0x100u, "the parent's bytes are untouched");

    // Page 1 is none in the child: refused, no break, no copy.
    TEST_EXPECT_EQ(fault(child, va + P, true), FAULT_UNHANDLED_USER, "a write at none is refused");
    struct page *pg1 = slot_page(pv, va + P);
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg1), 2ull, "page 1 still has two holders -- no break ran");

    // Page 2 (the last piece) breaks like the first.
    TEST_EXPECT_EQ(fault(child, va + 2 * P, true), FAULT_HANDLED, "child write on the last piece");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(slot_page(pv, va + 2 * P)), 1ull, "copied");

    drop(child);
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg1), 1ull, "the child's teardown released its share of page 1");
    drop(parent);
}

void test_cow_clone_dedupes_split_pieces(void) {
    struct Proc *parent = mk();
    TEST_ASSERT(parent != NULL, "proc_alloc parent");

    s64 r = reserve(parent, 4 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 4 pages RW");
    u64 va = (u64)r;
    for (u64 i = 0; i < 4; i++)
        TEST_EXPECT_EQ(fault(parent, va + i * P, true), FAULT_HANDLED, "parent faults a page in");
    struct Burrow *pb = vma_lookup(parent, va)->burrow;
    u32 pc_before = parent->as->page_count;

    // Four pieces of one Burrow: RW | R | none | RW.
    TEST_EXPECT_EQ(protect(parent, va + P, P, PR_R, 0), 0, "piece 1 -> R");
    TEST_EXPECT_EQ(protect(parent, va + 2 * P, P, PR_NONE, 0), 0, "piece 2 -> none");
    TEST_EXPECT_EQ(count_vmas(parent->as), 4u, "four parent pieces");

    struct AddrSpace *child = addrspace_clone(parent->as, /*exempt=*/true);
    TEST_ASSERT(child != NULL, "addrspace_clone");
    TEST_EXPECT_EQ(count_vmas(child), 4u, "four child pieces");

    struct Vma *c0 = vma_lookup_in(child, va);
    struct Burrow *cb = c0->burrow;
    TEST_ASSERT(cb != NULL && cb != pb, "the child maps a clone");
    for (u64 i = 0; i < 4; i++) {
        struct Vma *cv = vma_lookup_in(child, va + i * P);
        TEST_ASSERT(cv->burrow == cb, "every child piece maps the ONE clone");
        TEST_EXPECT_EQ(cv->prot, vma_lookup(parent, va + i * P)->prot, "prot mirrors the parent's");
        TEST_EXPECT_EQ(vma_prot_max(cv), (u32)VMA_PROT_RW, "the ceiling is inherited");
        TEST_ASSERT(cv->flags & VMA_FLAG_COW, "flagged COW");
        TEST_EXPECT_EQ(ident(cv, va + i * P), (u64)(i * P), "identity mirrors the parent's");
        TEST_EXPECT_EQ((u64)cow_page_share_for_test(burrow_lazy_slot_for_test(pb, (size_t)i)), 2ull,
                       "each page has exactly TWO holders, not one per piece");
    }
    TEST_EXPECT_EQ(burrow_mapping_count(cb), 4, "the clone has four mappings");
    TEST_EXPECT_EQ(child->page_count, 4u, "the child is charged the resident count ONCE");
    TEST_EXPECT_EQ(parent->as->page_count, pc_before, "the parent's charge is unchanged");
    TEST_ASSERT(pb->clone_cursor == NULL, "the dedupe cursor is retired before the clone returns");

    addrspace_unref(child);
    for (u64 i = 0; i < 4; i++)
        TEST_EXPECT_EQ((u64)cow_page_share_for_test(burrow_lazy_slot_for_test(pb, (size_t)i)), 1ull,
                       "the child's teardown returns every page to a single holder");
    drop(parent);
}

void test_cow_clone_refuses_eager_anon_with_writable_ceiling(void) {
    // An eager attach (RW ceiling) protected down to R: the fork is REFUSED --
    // either side could raise it back and then share writes across the fork.
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    s64 r = sys_burrow_attach_for_proc(p, P);
    TEST_ASSERT(r > 0, "eager attach");
    u64 va = (u64)r;
    TEST_EXPECT_EQ(protect(p, va, P, PR_R, 0), 0, "protect the eager mapping down to R");
    TEST_EXPECT_EQ(vma_lookup(p, va)->prot, (u32)VMA_PROT_READ, "it reads as R");
    TEST_EXPECT_EQ(vma_prot_max(vma_lookup(p, va)), (u32)VMA_PROT_RW, "but its ceiling is RW");
    TEST_ASSERT(addrspace_clone(p->as, /*exempt=*/true) == NULL,
                "a fork of an R mapping under an RW ceiling is refused");
    TEST_EXPECT_EQ(count_vmas(p->as), 1u, "and the parent is untouched");
    drop(p);

    // The control, one variable away: the same eager mapping minted R (the vDSO
    // shape, ceiling R) IS shared.
    struct Proc *q = mk();
    TEST_ASSERT(q != NULL, "proc_alloc");
    struct Burrow *b = map_eager(q, EXPLICIT_VA, P, VMA_PROT_READ);
    TEST_ASSERT(b != NULL, "map an eager R mapping");
    struct AddrSpace *child = addrspace_clone(q->as, /*exempt=*/true);
    TEST_ASSERT(child != NULL, "a fork of a ceiling-R eager mapping succeeds");
    TEST_ASSERT(vma_lookup_in(child, EXPLICIT_VA)->burrow == b, "shared, not cloned");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 2, "two mappings of one Burrow");
    addrspace_unref(child);
    drop(q);
}

void test_sys_burrow_detach_piece_frees_only_its_pages(void) {
    magazines_drain_all();
    u64 free0 = phys_free_pages();

    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    s64 r = reserve(p, 4 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 4 pages RW");
    u64 va = (u64)r;
    for (u64 i = 0; i < 4; i++)
        TEST_EXPECT_EQ(fault(p, va + i * P, true), FAULT_HANDLED, "fault a page in");
    struct Burrow *b = vma_lookup(p, va)->burrow;
    u32 pc0 = p->as->page_count;
    TEST_ASSERT(pc0 >= 4, "four pages charged");

    TEST_EXPECT_EQ(protect(p, va + 2 * P, 2 * P, PR_NONE, 0), 0, "split into two pieces");
    TEST_EXPECT_EQ(count_vmas(p->as), 2u, "two pieces");

    // Detach the first piece: exactly ITS two pages go, the other two stay.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va, 2 * P), 0, "detach piece 0");
    TEST_EXPECT_EQ(p->as->page_count, pc0 - 2, "uncharged exactly the piece's two pages");
    TEST_ASSERT(burrow_lazy_slot_for_test(b, 0) == NULL && burrow_lazy_slot_for_test(b, 1) == NULL,
                "the detached piece's pages are freed");
    TEST_ASSERT(burrow_lazy_slot_for_test(b, 2) != NULL && burrow_lazy_slot_for_test(b, 3) != NULL,
                "the surviving piece's pages stay resident");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "one mapping keeps the Burrow");
    TEST_EXPECT_EQ(protect(p, va + 2 * P, 2 * P, PR_RW, 0), 0, "the survivor still protects");
    TEST_EXPECT_EQ(fault(p, va + 2 * P, false), FAULT_HANDLED, "and still faults");

    // A wrong-length detach of the survivor frees nothing.
    TEST_ASSERT(sys_burrow_detach_for_proc(p, va + 2 * P, P) != 0, "a partial detach is refused");
    TEST_ASSERT(burrow_lazy_slot_for_test(b, 2) != NULL, "a refused detach freed nothing");
    TEST_EXPECT_EQ(p->as->page_count, pc0 - 2, "and uncharged nothing");

    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, va + 2 * P, 2 * P), 0, "detach the survivor");
    TEST_EXPECT_EQ(p->as->page_count, pc0 - 4, "everything uncharged");
    TEST_EXPECT_EQ(count_vmas(p->as), 0u, "nothing mapped");

    drop(p);
    magazines_drain_all();
    TEST_EXPECT_EQ(phys_free_pages(), free0, "every page came back");
}
