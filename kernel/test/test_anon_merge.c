// B-3e (ARCH 6.5 "Adjacent anonymous mappings merge"; operator vote
// 2026-10-18): a lazy anonymous reservation next to a qualifying lazy mapping
// extends that mapping's Burrow instead of minting a Burrow and a VMA.
//
//   - pagemap.grow_keeps_every_page: every growth shape (inline -> inline,
//     inline -> radix with pages, a radix that deepens under a present root,
//     an empty inline that turns radix) keeps each page at its index, counts
//     its nodes, and lands on the shape pagemap_init picks -- a clone's mirror
//     needs that. The walk finds the next resident slot without taking it.
//   - anon_merge.adjacent_reserves_share_one_mapping: two reserves and a lazy
//     attach at RW are one mapping and one Burrow; a reserve at another prot
//     is not.
//   - anon_merge.growth_crosses_every_shape: 600 one-page reserves, each
//     touched as it lands, cross the inline -> radix and depth 1 -> 2 growths
//     with pages in the map; every page keeps its contents.
//   - anon_merge.hole_refill_joins_the_pieces: a reserve into the hole a
//     detach cut joins the head and the tail back into one mapping; the
//     refilled pages read zero.
//   - anon_merge.successor_extends_into_its_offset_room: a reserve just below
//     a head-trimmed mapping extends it backward into the offsets it lost.
//   - anon_merge.never_across_a_boundary: a sealed ceiling, a fork's COW
//     mapping, an eager neighbour, a mapping that leaves the window, and a
//     Burrow a handle names are never extended.
//   - anon_merge.refused_growth_falls_back: a growth whose node the budget
//     refuses leaves the neighbour as it was and mints a mapping instead.
//   - anon_merge.fork_clones_the_merged_burrow: one clone per merged Burrow,
//     at the parent's bias.
//   - anon_merge.unmap_releases_lazy_slots: the exact-match unmap frees a lazy
//     piece's pages (NoOrphan), so a later extension over those offsets finds
//     them empty -- without the release it would extinct there.

#include "test.h"

#include "../../arch/arm64/fault.h"
#include "../../mm/phys.h"

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/exec.h>
#include <thylacine/page.h>
#include <thylacine/pagemap.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_pagemap_grow_keeps_every_page(void);
void test_anon_merge_adjacent_reserves_share_one_mapping(void);
void test_anon_merge_growth_crosses_every_shape(void);
void test_anon_merge_hole_refill_joins_the_pieces(void);
void test_anon_merge_successor_extends_into_its_offset_room(void);
void test_anon_merge_never_across_a_boundary(void);
void test_anon_merge_refused_growth_falls_back(void);
void test_anon_merge_fork_clones_the_merged_burrow(void);
void test_anon_merge_unmap_releases_lazy_slots(void);
void test_anon_merge_second_bias_is_never_extended(void);

extern s64 sys_burrow_reserve_for_proc(struct Proc *p, u64 length_raw, u64 prot_raw,
                                       u64 align_log2);
extern s64 sys_burrow_attach_lazy_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);
extern s64 sys_burrow_protect_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw,
                                       u64 prot_raw, u64 flags_raw);

#define P            PAGE_SIZE
#define PR_R         ((u64)BURROW_PROT_READ)
#define PR_RW        ((u64)(BURROW_PROT_READ | BURROW_PROT_WRITE))
#define BASE         EXEC_USER_BURROW_BASE

// The first failed check. The fixture is released before the verdict:
// TEST_ASSERT returns at once, and a Proc or a page left behind would move
// every later test's baseline.
struct verdict { const char *why; };
#define CHECK(vd, cond, msg) do { if (!(cond) && !(vd).why) (vd).why = (msg); } while (0)

static struct Proc *mk(void) { return proc_alloc(); }

static void drop(struct Proc *p) {
    if (!p) return;
    p->state = 2;                        // PROC_STATE_ZOMBIE; proc_free drains VMAs
    proc_free(p);
}

static enum fault_result fault(struct Proc *p, u64 va, bool is_write) {
    struct fault_info fi;
    fi.vaddr          = va;
    fi.elr            = 0;
    fi.esr            = 0;
    fi.ec             = 0x24;            // EC_DATA_ABORT_LOWER
    fi.fsc            = 0x07;            // FSC_TRANS_FAULT_L3
    fi.fault_level    = 3;
    fi.from_user      = true;
    fi.is_instruction = false;
    fi.is_write       = is_write;
    fi.is_translation = true;
    fi.is_permission  = false;
    fi.is_access_flag = false;
    fi.is_alignment   = false;
    fi.is_external    = false;
    return userland_demand_page(p, &fi);
}

static s64 reserve(struct Proc *p, u64 len, u64 prot) {
    return sys_burrow_reserve_for_proc(p, len, prot, 0);
}

static u32 count_vmas(struct AddrSpace *as) {
    u32 n = 0;
    for (struct Vma *v = as->vmas; v; v = v->next) n++;
    return n;
}

// page_count less the page tables: data pages and pagemap nodes.
static u32 data_pages(struct AddrSpace *as) {
    return __atomic_load_n(&as->page_count, __ATOMIC_ACQUIRE) -
           __atomic_load_n(&as->pgtable_pages, __ATOMIC_ACQUIRE);
}

// The words of the page a lazy mapping holds for `va`, or NULL when absent.
static u32 *words_at(struct Proc *p, u64 va) {
    struct Vma *v = vma_lookup(p, va);
    if (!v || !v->burrow) return NULL;
    size_t slot = (size_t)((v->burrow_offset + (va - v->vaddr_start)) / P);
    struct page *pg = burrow_lazy_slot_for_test(v->burrow, slot);
    return pg ? (u32 *)pa_to_kva(page_to_pa(pg)) : NULL;
}

static u32 tag_of(u64 i) { return 0xA11A0000u + (u32)i; }

// Write-fault page `i` of the mapping at `base` and stamp it.
static bool touch_tag(struct Proc *p, u64 base, u64 i) {
    if (fault(p, base + i * P, true) != FAULT_HANDLED) return false;
    u32 *w = words_at(p, base + i * P);
    if (!w) return false;
    w[0] = tag_of(i);
    return true;
}

static bool has_tag(struct Proc *p, u64 base, u64 i) {
    u32 *w = words_at(p, base + i * P);
    return w && w[0] == tag_of(i);
}

// A fresh touch of page `i` reads zero: first and last word.
static bool reads_zero(struct Proc *p, u64 base, u64 i) {
    if (fault(p, base + i * P, true) != FAULT_HANDLED) return false;
    u32 *w = words_at(p, base + i * P);
    return w && w[0] == 0 && w[P / sizeof(u32) - 1u] == 0;
}

// The bias every mapping of a lazy Burrow shares (struct Burrow, lazy_va0).
static bool bias_is(const struct Vma *v, u64 va0) {
    return v && v->burrow && v->burrow->lazy_va0_set && v->burrow->lazy_va0 == va0 &&
           v->vaddr_start - v->burrow_offset == va0;
}

// =============================================================================
// The pagemap's growth.
// =============================================================================

static void put_free(struct page *pg, void *ctx) { (void)ctx; free_pages(pg, 0); }

void test_pagemap_grow_keeps_every_page(void) {
    u64 free_before = phys_free_pages();
    struct verdict vd = { NULL };
    spin_lock_t lk = SPIN_LOCK_INIT;
    struct page *pg[4] = { NULL, NULL, NULL, NULL };
    for (int i = 0; i < 4; i++) pg[i] = alloc_user_pages(0, KP_ZERO, true);
    TEST_ASSERT(pg[0] && pg[1] && pg[2] && pg[3], "four pages");

    struct pagemap pm;
    TEST_EXPECT_EQ(pagemap_init(&pm, 4), 0, "an inline map of 4");
    CHECK(vd, pagemap_install(&pm, &lk, 0, pg[0], NULL, true, NULL) == 0, "install slot 0");
    CHECK(vd, pagemap_install(&pm, &lk, 3, pg[1], NULL, true, NULL) == 0, "install slot 3");

    // Refusals change nothing.
    CHECK(vd, pagemap_grow(&pm, &lk, 4, NULL, true) == -1, "the same count is not a growth");
    CHECK(vd, pagemap_grow(&pm, &lk, 2, NULL, true) == -1, "a smaller count is not a growth");

    // Inline -> inline.
    CHECK(vd, pagemap_grow(&pm, &lk, 20, NULL, true) == 0, "grow to 20");
    CHECK(vd, pm.depth == 0 && pm.count == 20 && pm.nodes == 0, "still inline, 20 slots");
    CHECK(vd, pagemap_get(&pm, 0) == pg[0] && pagemap_get(&pm, 3) == pg[1], "pages kept (inline)");
    CHECK(vd, pagemap_resident(&pm) == 2, "two resident");

    // Inline -> radix with pages in it: one node, the depth-1 root and leaf.
    CHECK(vd, pagemap_grow(&pm, &lk, 40, NULL, true) == 0, "grow to 40");
    CHECK(vd, pm.depth == 1 && pm.count == 40 && pm.nodes == 1, "radix depth 1, one node");
    CHECK(vd, pagemap_get(&pm, 0) == pg[0] && pagemap_get(&pm, 3) == pg[1], "pages kept (radix)");
    CHECK(vd, pagemap_install(&pm, &lk, 39, pg[2], NULL, true, NULL) == 0, "install slot 39");
    CHECK(vd, pm.nodes == 1, "slot 39 shares the leaf");

    // A radix that deepens under a present root: one new top node.
    CHECK(vd, pagemap_grow(&pm, &lk, 600, NULL, true) == 0, "grow to 600");
    CHECK(vd, pm.depth == 2 && pm.count == 600 && pm.nodes == 2, "depth 2, the old root under a new top");
    CHECK(vd, pagemap_get(&pm, 0) == pg[0] && pagemap_get(&pm, 3) == pg[1] &&
              pagemap_get(&pm, 39) == pg[2], "pages kept (deeper)");
    CHECK(vd, pagemap_install(&pm, &lk, 599, pg[3], NULL, true, NULL) == 0, "install slot 599");
    CHECK(vd, pm.nodes == 3, "slot 599 under a second leaf");

    // The walk names the next resident slot and takes nothing.
    CHECK(vd, pagemap_next_resident(&pm, &lk, 0, 600) == 0,     "next from 0");
    CHECK(vd, pagemap_next_resident(&pm, &lk, 4, 600) == 39,    "next from 4");
    CHECK(vd, pagemap_next_resident(&pm, &lk, 40, 600) == 599,  "next from 40");
    CHECK(vd, pagemap_next_resident(&pm, &lk, 40, 599) == 599,  "none in [40, 599): hi");
    CHECK(vd, pagemap_resident(&pm) == 4, "the walk took nothing");

    // A clone inits at the grown count and finds the same shape to mirror.
    struct pagemap cl;
    struct page *pool[3] = { NULL, NULL, NULL };
    bool cl_ok = pagemap_init(&cl, 600) == 0;
    CHECK(vd, cl_ok && cl.depth == pm.depth, "init at 600 picks the grown depth");
    if (cl_ok) {
        u32 got = pagemap_pool_alloc(pool, 3, true);
        CHECK(vd, got == 3, "a pool for the mirror");
        CHECK(vd, got == 3 && pagemap_mirror(&cl, &pm, pool, 3, NULL, NULL) == 0, "the mirror");
        CHECK(vd, pagemap_get(&cl, 599) == pg[3] && pagemap_get(&cl, 39) == pg[2], "the mirror's pages");
        pagemap_pool_free(pool, 3);
        pagemap_destroy(&cl, NULL, NULL);          // the pages stay pm's
    }

    // Take everything: every node goes with the last page beneath it.
    size_t slot = 0;
    u32 taken = 0;
    for (;;) {
        struct page *t = NULL;
        struct page *fr[PAGEMAP_MAX_DEPTH];
        u32 nf = 0;
        size_t at = pagemap_take_next(&pm, &lk, slot, 600, NULL, &t, fr, &nf);
        for (u32 k = 0; k < nf; k++) free_pages(fr[k], 0);
        if (!t) break;
        free_pages(t, 0);
        taken++;
        slot = at + 1;
    }
    CHECK(vd, taken == 4 && pm.nodes == 0 && pm.root == NULL, "all four taken, every node freed");
    pagemap_destroy(&pm, put_free, NULL);

    // An empty inline map turns radix with no node at all.
    struct pagemap em;
    if (pagemap_init(&em, 8) == 0) {
        CHECK(vd, pagemap_grow(&em, &lk, 100, NULL, true) == 0, "grow an empty map to 100");
        CHECK(vd, em.depth == 1 && em.nodes == 0 && em.root == NULL, "radix, no node until a page");
        pagemap_destroy(&em, NULL, NULL);
    } else {
        CHECK(vd, false, "an inline map of 8");
    }

    TEST_ASSERT(vd.why == NULL, vd.why);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

// =============================================================================
// The merge.
// =============================================================================

void test_anon_merge_adjacent_reserves_share_one_mapping(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct verdict vd = { NULL };

    s64 a = reserve(p, 2 * P, PR_RW);
    s64 b = sys_burrow_attach_lazy_for_proc(p, 3 * P);
    CHECK(vd, a > 0 && b > 0, "a reserve and a lazy attach");
    CHECK(vd, (u64)b == (u64)a + 2 * P, "adjacent (first fit)");
    struct Vma *v = vma_lookup(p, (u64)a);
    CHECK(vd, v != NULL && v == vma_lookup(p, (u64)b + 2 * P), "one mapping covers both");
    CHECK(vd, v && v->vaddr_start == (u64)a && v->vaddr_end == (u64)a + 5 * P, "exactly both");
    CHECK(vd, v && v->burrow && v->burrow->page_count == 5, "one Burrow of five pages");
    CHECK(vd, count_vmas(p->as) == 1 && p->as->vma_count == 1, "one VMA charged");
    CHECK(vd, bias_is(v, (u64)a), "offset 0 at the first reserve's address");
    CHECK(vd, touch_tag(p, (u64)a, 0) && touch_tag(p, (u64)a, 4), "both halves fault in");
    CHECK(vd, has_tag(p, (u64)a, 0) && has_tag(p, (u64)a, 4), "and keep what was written");

    // Another prot is another mapping.
    s64 c = reserve(p, P, PR_R);
    CHECK(vd, c == a + 5 * (s64)P, "the R reserve lands next");
    struct Vma *vc = c > 0 ? vma_lookup(p, (u64)c) : NULL;
    CHECK(vd, vc != NULL && vc != v && vc->burrow != (v ? v->burrow : NULL), "its own mapping and Burrow");
    CHECK(vd, count_vmas(p->as) == 2 && p->as->vma_count == 2, "two VMAs charged");

    drop(p);
    TEST_ASSERT(vd.why == NULL, vd.why);
}

void test_anon_merge_growth_crosses_every_shape(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct verdict vd = { NULL };
    enum { K = 600 };

    s64 base = reserve(p, P, PR_RW);
    CHECK(vd, base > 0 && touch_tag(p, (u64)base, 0), "the first page");
    bool placed = true, touched = true;
    for (u64 i = 1; i < K && base > 0; i++) {
        s64 va = reserve(p, P, PR_RW);
        if (va != base + (s64)(i * P)) { placed = false; break; }
        if (!touch_tag(p, (u64)base, i)) { touched = false; break; }
    }
    CHECK(vd, placed, "each reserve lands next to the last");
    CHECK(vd, touched, "each page faults in after its growth");
    bool kept = base > 0;
    for (u64 i = 0; kept && i < K; i++) kept = has_tag(p, (u64)base, i);
    CHECK(vd, kept, "every page kept its contents across inline -> radix -> depth 2");
    struct Vma *v = base > 0 ? vma_lookup(p, (u64)base) : NULL;
    CHECK(vd, v && v->vaddr_end == (u64)base + K * P, "one mapping");
    CHECK(vd, count_vmas(p->as) == 1, "one VMA");
    CHECK(vd, v && v->burrow->page_count == K && v->burrow->pm.depth == 2, "600 slots at depth 2");
    CHECK(vd, v && pagemap_node_count(&v->burrow->pm) == 3, "a root and two leaves");
    CHECK(vd, v && burrow_lazy_footprint(v->burrow) == K + 3u, "600 pages and 3 nodes");

    drop(p);
    TEST_ASSERT(vd.why == NULL, vd.why);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_anon_merge_hole_refill_joins_the_pieces(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct verdict vd = { NULL };

    s64 r = reserve(p, 8 * P, PR_RW);
    CHECK(vd, r > 0, "reserve 8");
    u64 a = (u64)r;
    bool all = r > 0;
    for (u64 i = 0; all && i < 8; i++) all = touch_tag(p, a, i);
    CHECK(vd, all, "touch all 8");
    struct Burrow *b = r > 0 ? vma_lookup(p, a)->burrow : NULL;

    CHECK(vd, sys_burrow_detach_for_proc(p, a + 2 * P, 3 * P) == 0, "cut [2, 5)");
    CHECK(vd, count_vmas(p->as) == 2, "head and tail");
    s64 h = reserve(p, 3 * P, PR_RW);
    CHECK(vd, h == r + 2 * (s64)P, "the reserve refills the hole");
    struct Vma *v = vma_lookup(p, a);
    CHECK(vd, v && v->vaddr_end == a + 8 * P && v->burrow == b, "one mapping again, the same Burrow");
    CHECK(vd, count_vmas(p->as) == 1 && p->as->vma_count == 1, "the join gave its VMA back");
    CHECK(vd, b && b->page_count == 8, "no growth: the hole's offsets were the Burrow's");
    CHECK(vd, bias_is(v, a), "one bias");
    CHECK(vd, has_tag(p, a, 0) && has_tag(p, a, 1) && has_tag(p, a, 5) &&
              has_tag(p, a, 7), "the head's and the tail's pages kept");
    CHECK(vd, reads_zero(p, a, 2) && reads_zero(p, a, 4), "the refilled pages read zero");

    drop(p);
    TEST_ASSERT(vd.why == NULL, vd.why);
}

void test_anon_merge_successor_extends_into_its_offset_room(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct verdict vd = { NULL };

    s64 r = reserve(p, 8 * P, PR_RW);
    CHECK(vd, r == (s64)BASE, "reserve 8 at the window's base");
    u64 a = (u64)r;
    bool all = r > 0;
    for (u64 i = 0; all && i < 8; i++) all = touch_tag(p, a, i);
    CHECK(vd, all, "touch all 8");

    CHECK(vd, sys_burrow_detach_for_proc(p, a, 3 * P) == 0, "trim the head [0, 3)");
    struct Vma *t = vma_lookup(p, a + 3 * P);
    CHECK(vd, t && t->burrow_offset == 3 * P, "the survivor keeps offset 3 pages");
    s64 h = reserve(p, 3 * P, PR_RW);
    CHECK(vd, h == r, "the reserve takes the trimmed head's addresses");
    struct Vma *v = vma_lookup(p, a);
    CHECK(vd, v && v == t && v->vaddr_start == a && v->burrow_offset == 0, "the successor grew back down");
    CHECK(vd, count_vmas(p->as) == 1, "one mapping");
    CHECK(vd, bias_is(v, a), "one bias");
    CHECK(vd, has_tag(p, a, 3) && has_tag(p, a, 7), "its pages kept");
    CHECK(vd, reads_zero(p, a, 0) && reads_zero(p, a, 2), "the offsets it took back read zero");

    // Two pages below a three-page gap is not adjacent: no extension.
    CHECK(vd, sys_burrow_detach_for_proc(p, a, 3 * P) == 0, "trim the head again");
    s64 s = reserve(p, 2 * P, PR_RW);
    CHECK(vd, s == r && count_vmas(p->as) == 2, "a reserve short of the successor mints");

    drop(p);
    TEST_ASSERT(vd.why == NULL, vd.why);
}

void test_anon_merge_never_across_a_boundary(void) {
    struct verdict vd = { NULL };

    // A sealed ceiling: equal prot, another flags word.
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    s64 a = reserve(p, 2 * P, PR_RW);
    CHECK(vd, a > 0 && sys_burrow_protect_for_proc(p, (u64)a, 2 * P, PR_R, BURROW_PROTECT_SEAL) == 0,
          "seal at R");
    s64 s = reserve(p, P, PR_R);
    CHECK(vd, s == a + 2 * (s64)P && count_vmas(p->as) == 2, "no merge past a sealed ceiling");
    // An unsealed R next to it does merge with the new R mapping.
    s64 s2 = reserve(p, P, PR_R);
    CHECK(vd, s2 == s + (s64)P && count_vmas(p->as) == 2, "R next to an unsealed R merges");
    drop(p);

    // A fork's mappings carry the COW routing bit and never extend.
    p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    a = reserve(p, 2 * P, PR_RW);
    CHECK(vd, a > 0 && touch_tag(p, (u64)a, 0), "a touched mapping");
    struct AddrSpace *child = addrspace_clone(p->as, false);
    CHECK(vd, child != NULL, "the fork");
    struct Vma *pv = a > 0 ? vma_lookup(p, (u64)a) : NULL;
    CHECK(vd, pv && (pv->flags & VMA_FLAG_COW), "the parent's mapping is COW-routed");
    s = reserve(p, P, PR_RW);
    CHECK(vd, s == a + 2 * (s64)P && count_vmas(p->as) == 2, "no merge into a COW mapping");
    if (child) addrspace_unref(child);
    drop(p);

    // An eager neighbour.
    p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    s64 e = sys_burrow_attach_for_proc(p, 2 * P);
    s = reserve(p, P, PR_RW);
    CHECK(vd, e > 0 && s == e + 2 * (s64)P && count_vmas(p->as) == 2, "no merge into eager memory");
    drop(p);

    // A neighbour that leaves the window.
    p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct Burrow *lb = burrow_create_anon_lazy(2 * P);
    CHECK(vd, lb != NULL, "a lazy Burrow");
    if (lb) {
        spin_lock(&p->as->lock);
        int rc = burrow_map(p, lb, BASE - P, 2 * P, VMA_PROT_RW);
        spin_unlock(&p->as->lock);
        burrow_unref(lb);                // the mapping holds it now
        CHECK(vd, rc == 0, "mapped across the window's base");
    }
    s = reserve(p, P, PR_RW);
    CHECK(vd, s == (s64)(BASE + P) && count_vmas(p->as) == 2, "no merge into a mapping outside the window");
    drop(p);

    // A Burrow a handle still names.
    p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct Burrow *hb = burrow_create_anon_lazy(2 * P);
    CHECK(vd, hb != NULL, "a lazy Burrow, its construction ref held");
    if (hb) {
        spin_lock(&p->as->lock);
        int rc = burrow_map(p, hb, BASE, 2 * P, VMA_PROT_RW);
        spin_unlock(&p->as->lock);
        CHECK(vd, rc == 0, "mapped at the base");
    }
    s = reserve(p, P, PR_RW);
    struct Vma *hv = vma_lookup(p, BASE);
    CHECK(vd, s == (s64)(BASE + 2 * P) && count_vmas(p->as) == 2, "no merge into a Burrow a handle names");
    CHECK(vd, hv && hb && hb->page_count == 2 && hv->vaddr_end == BASE + 2 * P, "it is unchanged");
    if (hb) burrow_unref(hb);
    drop(p);

    TEST_ASSERT(vd.why == NULL, vd.why);
}

void test_anon_merge_refused_growth_falls_back(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct verdict vd = { NULL };
    CHECK(vd, !proc_resource_exempt(p), "a budgeted Proc");

    // 32 resident slots: the inline map's whole room.
    s64 a = reserve(p, 32 * P, PR_RW);
    bool all = a > 0;
    for (u64 i = 0; all && i < 32; i++) all = touch_tag(p, (u64)a, i);
    CHECK(vd, all, "touch all 32");
    struct Burrow *b = a > 0 ? vma_lookup(p, (u64)a)->burrow : NULL;

    // The 33rd slot turns the map radix, which needs a node: refuse it.
    u32 budget = __atomic_load_n(&p->as->page_budget, __ATOMIC_ACQUIRE);
    __atomic_store_n(&p->as->page_budget, __atomic_load_n(&p->as->page_count, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
    s64 s = reserve(p, P, PR_RW);
    __atomic_store_n(&p->as->page_budget, budget, __ATOMIC_RELEASE);
    CHECK(vd, s == a + 32 * (s64)P, "the reserve is placed as before");
    CHECK(vd, count_vmas(p->as) == 2, "and minted, since the neighbour could not grow");
    CHECK(vd, b && b->page_count == 32 && b->pm.depth == 0 && b->pm.count == 32,
          "the neighbour is as it was");
    CHECK(vd, has_tag(p, (u64)a, 0) && has_tag(p, (u64)a, 31), "its pages kept");

    drop(p);
    TEST_ASSERT(vd.why == NULL, vd.why);
}

void test_anon_merge_fork_clones_the_merged_burrow(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct verdict vd = { NULL };

    s64 a = reserve(p, 2 * P, PR_RW);
    s64 b = reserve(p, 2 * P, PR_RW);
    CHECK(vd, a > 0 && b == a + 2 * (s64)P && count_vmas(p->as) == 1, "one merged mapping");
    CHECK(vd, touch_tag(p, (u64)a, 0) && touch_tag(p, (u64)a, 3), "a page in each half");
    struct AddrSpace *child = addrspace_clone(p->as, false);
    CHECK(vd, child != NULL, "the fork");
    if (child) {
        struct Vma *pv = vma_lookup(p, (u64)a);
        struct Vma *cv = vma_lookup_in(child, (u64)a);
        CHECK(vd, cv && cv->vaddr_end == (u64)a + 4 * P && count_vmas(child) == 1, "the child: one mapping");
        CHECK(vd, pv && cv && cv->burrow != pv->burrow && cv->burrow->page_count == 4,
              "of its own clone, the merged size");
        CHECK(vd, pv && cv && bias_is(cv, (u64)a), "at the parent's bias");
        CHECK(vd, pv && cv && burrow_lazy_slot_for_test(cv->burrow, 3) ==
                                 burrow_lazy_slot_for_test(pv->burrow, 3), "sharing the page COW");
        addrspace_unref(child);
    }

    drop(p);
    TEST_ASSERT(vd.why == NULL, vd.why);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_anon_merge_unmap_releases_lazy_slots(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct verdict vd = { NULL };

    // Two pieces of one Burrow: [0, 2) at RW, touched; [2, 4) at R.
    s64 r = reserve(p, 4 * P, PR_RW);
    u64 a = (u64)r;
    CHECK(vd, r > 0 && touch_tag(p, a, 0) && touch_tag(p, a, 1), "touch the head");
    CHECK(vd, r > 0 && sys_burrow_protect_for_proc(p, a + 2 * P, 2 * P, PR_R, 0) == 0, "split at R");
    struct Burrow *b = r > 0 ? vma_lookup(p, a)->burrow : NULL;
    u32 held = data_pages(p->as);

    // The exact-match unmap of the head piece releases its pages with it.
    spin_lock(&p->as->lock);
    int rc = burrow_unmap_in(p->as, a, 2 * P);
    spin_unlock(&p->as->lock);
    CHECK(vd, rc == 0, "the head unmapped");
    CHECK(vd, b && burrow_lazy_resident_count(b) == 0, "its pages left the Burrow");
    CHECK(vd, data_pages(p->as) == held - 2u, "and their charge");

    // So a reserve that extends the tail backward over those offsets finds
    // them empty (an orphan there would extinct in burrow_lazy_extend).
    s64 s = reserve(p, 2 * P, PR_R);
    struct Vma *v = vma_lookup(p, a);
    CHECK(vd, s == r && v && v->burrow == b && v->vaddr_end == a + 4 * P, "the tail extended back");
    CHECK(vd, v && fault(p, a, false) == FAULT_HANDLED, "a read faults in");
    u32 *w = words_at(p, a);
    CHECK(vd, w && w[0] == 0, "a zero page, not the old one");

    drop(p);
    TEST_ASSERT(vd.why == NULL, vd.why);
}

// A lazy Burrow a kernel caller maps at a second bias is never extended: its
// free addresses no longer say which of its offsets are free. The control is
// the same layout mapped once, one variable away, which merges.
void test_anon_merge_second_bias_is_never_extended(void) {
    struct verdict vd = { NULL };
    s64 s_two = -1, s_one = -1;
    u32 vmas_two = 0, vmas_one = 0;
    bool marked = false, two_mapped = false, one_mapped = false;

    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct Burrow *b = burrow_create_anon_lazy(2 * P);
    if (b) {
        spin_lock(&p->as->lock);
        two_mapped = burrow_map(p, b, BASE, 2 * P, VMA_PROT_RW) == 0 &&
                     burrow_map(p, b, BASE + 8 * P, 2 * P, VMA_PROT_RW) == 0;
        spin_unlock(&p->as->lock);
        marked = b->lazy_multi_bias;
        burrow_unref(b);                    // no handle left: only the bias refuses
    }
    s_two    = reserve(p, P, PR_RW);
    vmas_two = count_vmas(p->as);
    drop(p);

    p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct Burrow *c = burrow_create_anon_lazy(2 * P);
    if (c) {
        spin_lock(&p->as->lock);
        one_mapped = burrow_map(p, c, BASE, 2 * P, VMA_PROT_RW) == 0;
        spin_unlock(&p->as->lock);
        burrow_unref(c);
    }
    s_one    = reserve(p, P, PR_RW);
    vmas_one = count_vmas(p->as);
    drop(p);

    CHECK(vd, two_mapped, "one lazy Burrow mapped at two biases");
    CHECK(vd, marked, "the second insert marked it");
    CHECK(vd, s_two == (s64)(BASE + 2 * P) && vmas_two == 3u, "no merge into a Burrow mapped at two biases");
    CHECK(vd, one_mapped, "the control's Burrow mapped once");
    CHECK(vd, s_one == (s64)(BASE + 2 * P) && vmas_one == 1u, "control: the same layout mapped once merges");
    TEST_ASSERT(vd.why == NULL, vd.why);
}
