// B-1a' (capacity): the range detach, the charged pagemap, the user pool.
//
// The claims, one test each (ARCH 6.5 "Range detach" + "Capacity, and the I-32
// default"; specs/capacity.tla is the accounting law they keep):
//   detach.range_trims_left_right_middle
//     a range cutting a lazy mapping's head, tail or middle leaves survivors
//     whose byte identity, pages and PTEs are untouched, releases exactly the
//     cut pages (page_count, resident count, PTEs), and a range that maps
//     nothing answers 0.
//   detach.range_across_burrows_and_holes
//     one range trims a mapping's tail, removes a whole one and trims another's
//     head across two holes; the whole one's slots are released BEFORE its
//     mapping goes (a held ref sees it alive with nothing resident).
//   detach.range_refusals_change_nothing
//     a CODE alias anywhere (-1 / EACCES), a cut shared-in mapping (-1 /
//     EACCES) and a middle cut at PROC_VMA_MAX (-1 / ENOMEM) each leave pages,
//     PTEs, counts and geometry as they were; a whole shared-in mapping goes
//     and refunds shared_map_pages; a head trim at the cap is served (no slot
//     needed); malformed / below-window munmaps answer EINVAL / ENOSYS.
//   detach.eager_pages_go_with_the_last_piece
//     an eager region cut into pieces stays charged until its LAST piece goes.
//   detach.lazy_over_256mib_detaches
//     a 512 MiB lazy attach and a 512 MiB reserve both detach (the old cap
//     refused any detach over BURROW_ATTACH_MAX).
//   detach.four_gib_reservation_round_trips
//     the browser-status exit: 4 GiB reserved, touched every 512 MiB (8 pages
//     + 13 nodes charged), a GiB protected to R, a 2 GiB range detached across
//     the pieces (4 pages + 6 nodes back), then the rest -- 0 mappings, 0
//     charged, phys free back.
//   capacity.window_sized_reservation_releases_in_bounded_steps
//     the whole window (64 TiB) reserved, two pages touched, detached: the
//     release visits present nodes, never slots (pagemap_walk_steps is a few
//     thousand, not 2^34).
//   capacity.replace_window_releases_orphans
//     a MAP_FIXED window inside a touched lazy mapping releases the window's
//     slots before the swap (the B-1a audit's F5).
//   capacity.pagemap_nodes_charged_and_reclaimed
//     a 1 GiB map's nodes are charged as touched (root + leaf, then a second
//     leaf) and reclaimed as decommitted, root included.
//   capacity.default_is_ram_minus_reserve
//     pool + reserve == RAM, the pool is never empty, the reserve is between
//     RAM/8 and RAM/2 and at least 256 MiB when RAM can spare it (facts a
//     wrong formula would break, none of them the formula itself); the CI
//     guest's concrete figures pinned; the default budget and the hard max
//     are the pool; a fresh Proc and its address space carry it.
//   capacity.pool_refuses_users_keeps_tcb
//     with the pool parked to K pages, a user Proc far below its own budget
//     is refused at exactly K -- pages, nodes and tables all counted; the
//     TCB is not; every page returns.
//   capacity.page_tables_charged_and_reclaimed
//     every L1 / L2 / L3 a touch grows is charged to the space and the pool;
//     a decommit that empties a table frees it, its ancestors too when they
//     empty, down to the L0 entry; a touch rebuilds the path; the range form
//     reclaims as the single form does (the round-1 audit's F1).
//   capacity.memory_bomb_leaves_the_reserve
//     one page per 2 MiB of a large reservation is refused at exactly the
//     pool's room, tables included, the physical footprint bounded by that
//     room; the TCB keeps allocating; a decommit returns everything, tables
//     included, and the same attack is refused at the same point.
//   capacity.fork_costs_the_pool_only_its_nodes
//     a fork whose resident set exceeds the pool's room is served: the child
//     COUNTS every page it maps but the pool pays only the node mirror (the
//     round-1 audit's F5); the child's death returns its nodes alone.
//   capacity.fork_clone_charges_pages_and_nodes
//     a forked child is charged the mirror's pages AND node pages, so its own
//     releases refund exactly what it paid and the pool never drops below what
//     is held (the clone charged resident pages only, once).
//   capacity.death_returns_charges_to_pool
//     a Proc that dies holding pages, nodes and an eager region returns all
//     of it to the pool (the drain frees Proc-agnostically; the pool outlives
//     the address space).

#include "test.h"

#include "../../arch/arm64/fault.h"
#include "../../arch/arm64/mmu.h"
#include "../../mm/phys.h"

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>
#include <thylacine/exec.h>
#include <thylacine/image.h>
#include <thylacine/page.h>
#include <thylacine/pagemap.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

void test_detach_range_trims_left_right_middle(void);
void test_detach_range_across_burrows_and_holes(void);
void test_detach_range_refusals_change_nothing(void);
void test_detach_eager_pages_go_with_the_last_piece(void);
void test_detach_lazy_over_256mib_detaches(void);
void test_detach_four_gib_reservation_round_trips(void);
void test_capacity_window_sized_reservation_releases_in_bounded_steps(void);
void test_capacity_replace_window_releases_orphans(void);
void test_capacity_pagemap_nodes_charged_and_reclaimed(void);
void test_capacity_default_is_ram_minus_reserve(void);
void test_capacity_pool_refuses_users_keeps_tcb(void);
void test_capacity_death_returns_charges_to_pool(void);
void test_capacity_page_tables_charged_and_reclaimed(void);
void test_capacity_memory_bomb_leaves_the_reserve(void);
void test_capacity_fork_costs_the_pool_only_its_nodes(void);
void test_vma_range_is_mapped(void);

// The non-static inners of the SVC handlers (kernel/syscall.c).
extern s64 sys_burrow_reserve_for_proc(struct Proc *p, u64 length_raw, u64 prot_raw,
                                       u64 align_log2);
extern s64 sys_burrow_protect_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw,
                                       u64 prot_raw, u64 flags_raw);
extern s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_attach_lazy_for_proc(struct Proc *p, u64 length_raw);
extern s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);
extern s64 sys_burrow_decommit_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);
extern s64 sys_munmap_range_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw);

#define P            PAGE_SIZE
#define PR_R         ((u64)BURROW_PROT_READ)
#define PR_RW        ((u64)(BURROW_PROT_READ | BURROW_PROT_WRITE))
#define ERR(e)       (-(s64)(e))
#define MIB(n)       ((u64)(n) << 20)
#define GIB(n)       ((u64)(n) << 30)

// Explicit placements go 1 TiB into the window: above anything the first-fit
// gap search hands a reserve or an attach during a test, so the two never meet.
#define HI_VA        (EXEC_USER_BURROW_BASE + (1ull << 40))

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
    fi->is_alignment = false;
    fi->is_external = false;
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

// page_count is data pages + pagemap nodes + page tables (the round-1 audit's
// F1). The tests reason about data and nodes with pages_of, about tables with
// tables_of, and about the whole with count_of.
static u32 count_of(struct AddrSpace *as) {
    return __atomic_load_n(&as->page_count, __ATOMIC_ACQUIRE);
}
static u32 tables_of(struct AddrSpace *as) {
    return __atomic_load_n(&as->pgtable_pages, __ATOMIC_ACQUIRE);
}
static u32 pages_of(struct AddrSpace *as) {
    return count_of(as) - tables_of(as);
}

static u32 shared_of(struct AddrSpace *as) {
    return __atomic_load_n(&as->shared_map_pages, __ATOMIC_ACQUIRE);
}

// The property every cut rests on: for every VA a surviving mapping still
// covers, the byte it names is unchanged (the D-3b tests' byte_identity).
static u64 ident(const struct Vma *v, u64 va) {
    return v->burrow_offset + (va - v->vaddr_start);
}

static s64 reserve(struct Proc *p, u64 len, u64 prot, u64 align_log2) {
    return sys_burrow_reserve_for_proc(p, len, prot, align_log2);
}
static s64 protect(struct Proc *p, u64 va, u64 len, u64 prot, u64 flags) {
    return sys_burrow_protect_for_proc(p, va, len, prot, flags);
}
static s64 detach(struct Proc *p, u64 va, u64 len) {
    return sys_burrow_detach_for_proc(p, va, len);
}
static s64 munmap_range(struct Proc *p, u64 va, u64 len) {
    return sys_munmap_range_for_proc(p, va, len);
}

// Map a Burrow at a chosen VA under the lock its contract requires. The
// mapping takes its own ref; the construction ref stays the caller's.
static int map_at(struct Proc *p, struct Burrow *b, u64 va, u64 len, u32 prot) {
    spin_lock(&p->as->lock);
    int rc = burrow_map(p, b, va, (size_t)len, prot);
    spin_unlock(&p->as->lock);
    return rc;
}

// burrow_map_fixed under the lock, the replaced chain freed after the unlock
// (the production arms' shape; a FILE free may sleep).
static int map_fixed_locked(struct Proc *p, struct Burrow *b, u64 va,
                            size_t len, u32 prot, u64 off) {
    struct Burrow *tf = NULL;
    spin_lock(&p->as->lock);
    int rc = burrow_map_fixed(p, b, va, len, prot, off, &tf);
    spin_unlock(&p->as->lock);
    if (tf) burrow_free_deferred(tf);
    return rc;
}

// The kernel-side view of a lazy mapping's slot page for `va` (NULL if not
// resident), and the words in it.
static struct page *slot_page(const struct Vma *v, u64 va) {
    size_t slot = (size_t)(ident(v, va) / P);
    return burrow_lazy_slot_for_test(v->burrow, slot);
}

static u32 *slot_words(const struct Vma *v, u64 va) {
    struct page *pg = slot_page(v, va);
    return pg ? (u32 *)pa_to_kva(page_to_pa(pg)) : NULL;
}

// Touch page `i` of a lazy mapping at `base` and stamp it, so a surviving page
// can later be told from a fresh zero page.
static bool touch_tag(struct Proc *p, u64 base, u64 i) {
    if (fault(p, base + i * P, true) != FAULT_HANDLED) return false;
    struct Vma *v = vma_lookup(p, base + i * P);
    u32 *w = v ? slot_words(v, base + i * P) : NULL;
    if (!w) return false;
    w[0] = 0xC0DE0000u + (u32)i;
    return true;
}

// =============================================================================
// The range detach.
// =============================================================================

void test_detach_range_trims_left_right_middle(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();

    s64 r = reserve(p, 8 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 8 pages RW");
    u64 va = (u64)r;
    for (u64 i = 0; i < 8; i++) TEST_ASSERT(touch_tag(p, va, i), "touch + tag every page");
    struct Vma *v = vma_lookup(p, va);
    TEST_ASSERT(v != NULL, "the reservation's mapping");
    struct Burrow *b = v->burrow;
    struct page *keep[8];
    for (u64 i = 0; i < 8; i++) keep[i] = slot_page(v, va + i * P);
    TEST_EXPECT_EQ(pages_of(p->as), 8u, "8 pages charged (an inline leaf: no nodes)");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 8u, "8 resident");

    // HEAD: [va, va+2P) goes; the survivor is the tail, identity kept.
    TEST_EXPECT_EQ(detach(p, va, 2 * P), (s64)0, "detach the head");
    TEST_ASSERT(vma_lookup(p, va) == NULL && vma_lookup(p, va + P) == NULL, "the head is unmapped");
    v = vma_lookup(p, va + 2 * P);
    TEST_ASSERT(v != NULL, "the tail survives");
    TEST_EXPECT_EQ(v->vaddr_start, va + 2 * P, "tail start");
    TEST_EXPECT_EQ(v->vaddr_end,   va + 8 * P, "tail end");
    TEST_EXPECT_EQ(ident(v, va + 2 * P), 2 * P, "identity preserved across the head trim");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 1u, "one mapping");
    TEST_EXPECT_EQ(pages_of(p->as), 6u, "the two cut pages uncharged");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 6u, "and released");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va) == 0 && pte_of(p->as->pgtable_root, va + P) == 0,
                "the cut pages' PTEs are gone");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + 2 * P) != 0, "the survivor's PTE stays");
    TEST_ASSERT(slot_page(v, va + 2 * P) == keep[2], "page 2 is the same page");
    TEST_EXPECT_EQ(slot_words(v, va + 2 * P)[0], 0xC0DE0002u, "with its contents");

    // TAIL: [va+6P, va+8P) goes; the survivor is the head.
    TEST_EXPECT_EQ(detach(p, va + 6 * P, 2 * P), (s64)0, "detach the tail");
    v = vma_lookup(p, va + 2 * P);
    TEST_ASSERT(v != NULL, "the head survives");
    TEST_EXPECT_EQ(v->vaddr_end, va + 6 * P, "trimmed to 6P");
    TEST_ASSERT(vma_lookup(p, va + 6 * P) == NULL, "the tail is unmapped");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 1u, "still one mapping");
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "4 charged");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 4u, "4 resident");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + 6 * P) == 0, "page 6's PTE is gone");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + 5 * P) != 0, "page 5's PTE stays");

    // MIDDLE: [va+3P, va+4P) goes; a head and a tail piece, one allocation.
    TEST_EXPECT_EQ(detach(p, va + 3 * P, P), (s64)0, "detach the middle");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "two pieces");
    struct Vma *h = vma_lookup(p, va + 2 * P);
    struct Vma *t = vma_lookup(p, va + 4 * P);
    TEST_ASSERT(h != NULL && t != NULL, "both pieces");
    TEST_ASSERT(vma_lookup(p, va + 3 * P) == NULL, "the cut is unmapped");
    TEST_EXPECT_EQ(h->vaddr_start, va + 2 * P, "head start");
    TEST_EXPECT_EQ(h->vaddr_end,   va + 3 * P, "head end");
    TEST_EXPECT_EQ(t->vaddr_start, va + 4 * P, "tail start");
    TEST_EXPECT_EQ(t->vaddr_end,   va + 6 * P, "tail end");
    TEST_ASSERT(t->burrow == b, "the tail piece names the same Burrow");
    TEST_EXPECT_EQ(ident(h, va + 2 * P), 2 * P, "head identity");
    TEST_EXPECT_EQ(ident(t, va + 4 * P), 4 * P, "tail identity");
    TEST_EXPECT_EQ(t->prot,  h->prot,  "the piece carries the prot");
    TEST_EXPECT_EQ(t->flags, h->flags, "and the flags");
    TEST_EXPECT_EQ(pages_of(p->as), 3u, "3 charged");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 3u, "3 resident");
    TEST_ASSERT(slot_page(t, va + 5 * P) == keep[5], "page 5 is the same page");
    TEST_EXPECT_EQ(slot_words(t, va + 5 * P)[0], 0xC0DE0005u, "with its contents");
    TEST_ASSERT(slot_page(h, va + 2 * P) == keep[2], "page 2 too");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + 3 * P) == 0, "page 3's PTE is gone");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + 4 * P) != 0, "page 4's PTE stays");

    // A range that maps nothing answers 0 and changes nothing.
    TEST_EXPECT_EQ(detach(p, va + 3 * P, P), (s64)0, "an empty range is 0");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "and cuts nothing");
    TEST_EXPECT_EQ(pages_of(p->as), 3u, "and releases nothing");

    // The rest, in one range spanning the hole and both pieces.
    TEST_EXPECT_EQ(detach(p, va, 8 * P), (s64)0, "detach the rest");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back where it started");

    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_detach_range_across_burrows_and_holes(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();
    const u64 X = HI_VA;

    // b1 [X, X+4P) | hole 2P | b2 [X+6P, X+10P) | hole 2P | b3 [X+12P, X+14P).
    // The construction refs are HELD, so every Burrow outlives its mappings
    // and its resident count can be read after the cut.
    struct Burrow *b1 = burrow_create_anon_lazy(4 * P);
    struct Burrow *b2 = burrow_create_anon_lazy(4 * P);
    struct Burrow *b3 = burrow_create_anon_lazy(2 * P);
    TEST_ASSERT(b1 && b2 && b3, "three lazy Burrows");
    TEST_EXPECT_EQ(map_at(p, b1, X,          4 * P, VMA_PROT_RW), 0, "map b1");
    TEST_EXPECT_EQ(map_at(p, b2, X + 6 * P,  4 * P, VMA_PROT_RW), 0, "map b2");
    TEST_EXPECT_EQ(map_at(p, b3, X + 12 * P, 2 * P, VMA_PROT_RW), 0, "map b3");
    TEST_ASSERT(touch_tag(p, X, 1) && touch_tag(p, X, 3), "touch b1 pages 1, 3");
    TEST_ASSERT(touch_tag(p, X + 6 * P, 0) && touch_tag(p, X + 6 * P, 3), "touch b2 pages 0, 3");
    TEST_ASSERT(touch_tag(p, X + 12 * P, 0) && touch_tag(p, X + 12 * P, 1), "touch b3 pages 0, 1");
    TEST_EXPECT_EQ(pages_of(p->as), 6u, "6 pages charged");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 3u, "three mappings");
    struct page *keep13 = slot_page(vma_lookup(p, X + 13 * P), X + 13 * P);

    // One range: b1's tail, b2 whole, b3's head, across both holes.
    TEST_EXPECT_EQ(detach(p, X + 2 * P, 11 * P), (s64)0, "detach [X+2P, X+13P)");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "b2's mapping went; b1 and b3 trimmed");
    struct Vma *v1 = vma_lookup(p, X);
    struct Vma *v3 = vma_lookup(p, X + 13 * P);
    TEST_ASSERT(v1 && v1->burrow == b1, "b1 survives");
    TEST_ASSERT(v3 && v3->burrow == b3, "b3 survives");
    TEST_ASSERT(vma_lookup(p, X + 6 * P) == NULL && vma_lookup(p, X + 9 * P) == NULL, "b2 is unmapped");
    TEST_EXPECT_EQ(v1->vaddr_end,   X + 2 * P,  "b1 trimmed to its head");
    TEST_EXPECT_EQ(v3->vaddr_start, X + 13 * P, "b3 trimmed to its tail");
    TEST_EXPECT_EQ(ident(v3, X + 13 * P), P, "b3's identity preserved");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b1), 1u, "b1: page 3 released, page 1 kept");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b2), 0u, "b2: released BEFORE its mapping went (alive on our ref)");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b3), 1u, "b3: page 0 released, page 1 kept");
    TEST_EXPECT_EQ(pages_of(p->as), 2u, "2 pages charged");
    TEST_ASSERT(slot_page(v3, X + 13 * P) == keep13, "b3's page 1 is the same page");
    TEST_EXPECT_EQ(slot_words(v3, X + 13 * P)[0], 0xC0DE0001u, "with its contents");
    TEST_ASSERT(pte_of(p->as->pgtable_root, X + 12 * P) == 0, "b3 page 0's PTE is gone");
    TEST_ASSERT(pte_of(p->as->pgtable_root, X + 13 * P) != 0, "b3 page 1's PTE stays");
    TEST_ASSERT(pte_of(p->as->pgtable_root, X + P) != 0, "b1 page 1's PTE stays");

    // A range over a hole only.
    TEST_EXPECT_EQ(detach(p, X + 4 * P, 2 * P), (s64)0, "a hole detaches as 0");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "and changes nothing");

    TEST_EXPECT_EQ(detach(p, X, 14 * P), (s64)0, "the rest");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    burrow_unref(b1);
    burrow_unref(b2);
    burrow_unref(b3);
    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_detach_range_refusals_change_nothing(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();
    const u64 X = HI_VA;

    // (a) A CODE alias: the I-42 pair's lifetime belongs to the JIT syscalls.
    struct Burrow *code = burrow_create_code(P, false);
    TEST_ASSERT(code != NULL, "burrow_create_code");
    TEST_EXPECT_EQ(map_at(p, code, X, P, VMA_PROT_READ | VMA_PROT_EXEC), 0, "map the CODE burrow RX");
    burrow_unref(code);                  // the mapping holds it
    struct Burrow *nb = burrow_create_anon_lazy(2 * P);
    TEST_ASSERT(nb != NULL, "a lazy neighbour");
    TEST_EXPECT_EQ(map_at(p, nb, X + P, 2 * P, VMA_PROT_RW), 0, "map the neighbour");
    burrow_unref(nb);
    TEST_EXPECT_EQ((int)fault(p, X + P, true), (int)FAULT_HANDLED, "touch the neighbour");
    u64 leaf = pte_of(p->as->pgtable_root, X + P);
    TEST_ASSERT(leaf != 0, "the neighbour's page is installed");
    u32 pages = pages_of(p->as);
    TEST_EXPECT_EQ(pages, 1u, "one page charged");

    TEST_EXPECT_EQ(detach(p, X, P), (s64)-1, "detaching the CODE alias is refused");
    TEST_EXPECT_EQ(munmap_range(p, X, P), ERR(T_E_ACCES), "munmap: EACCES");
    TEST_EXPECT_EQ(detach(p, X, 3 * P), (s64)-1, "a range that includes it is refused whole");
    TEST_EXPECT_EQ(munmap_range(p, X, 3 * P), ERR(T_E_ACCES), "munmap: EACCES for the span");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, X + P), leaf, "the refusal uninstalled nothing");
    TEST_EXPECT_EQ(pages_of(p->as), pages, "and released nothing");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "and cut nothing");
    struct Vma *cv = vma_lookup(p, X);
    TEST_ASSERT(cv != NULL && cv->burrow == code, "the CODE mapping survives");

    // (b) A shared-in mapping: its exact span is what the sharer's teardown
    // matches, so a cut is refused and only the whole is served.
    struct Burrow *share = burrow_create_anon(3 * P, false);
    TEST_ASSERT(share != NULL, "burrow_create_anon");
    spin_lock(&p->as->lock);
    int src = burrow_share_into(p, share, X + 8 * P, VMA_PROT_RW);
    spin_unlock(&p->as->lock);
    TEST_EXPECT_EQ(src, 0, "burrow_share_into");
    TEST_EXPECT_EQ(shared_of(p->as), 3u, "the share charges shared_map_pages");
    TEST_EXPECT_EQ(pages_of(p->as), pages, "not page_count");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 3u, "three mappings");
    TEST_EXPECT_EQ(detach(p, X + 8 * P, P), (s64)-1, "a cut at the head is refused");
    TEST_EXPECT_EQ(munmap_range(p, X + 9 * P, 2 * P), ERR(T_E_ACCES), "a cut at the tail: EACCES");
    TEST_EXPECT_EQ(munmap_range(p, X + 9 * P, P), ERR(T_E_ACCES), "a cut in the middle: EACCES");
    TEST_EXPECT_EQ(shared_of(p->as), 3u, "refused: shared_map_pages unchanged");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 3u, "refused: geometry unchanged");
    TEST_EXPECT_EQ(detach(p, X + 8 * P, 3 * P), (s64)0, "the whole is served");
    TEST_EXPECT_EQ(shared_of(p->as), 0u, "and shared_map_pages refunded");
    TEST_EXPECT_EQ(pages_of(p->as), pages, "page_count untouched by a shared-in drop");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "the shared-in mapping went");
    burrow_unref(share);

    // (c) The VMA cap: only a middle cut needs a slot.
    struct Burrow *lz = burrow_create_anon_lazy(4 * P);
    TEST_ASSERT(lz != NULL, "a lazy 4P");
    TEST_EXPECT_EQ(map_at(p, lz, X + 16 * P, 4 * P, VMA_PROT_RW), 0, "map it");
    burrow_unref(lz);
    TEST_EXPECT_EQ((int)fault(p, X + 17 * P, true), (int)FAULT_HANDLED, "touch its page 1");
    u64 leaf1 = pte_of(p->as->pgtable_root, X + 17 * P);
    TEST_ASSERT(leaf1 != 0, "page 1 installed");
    pages = pages_of(p->as);
    u32 saved = __atomic_load_n(&p->as->vma_count, __ATOMIC_RELAXED);
    __atomic_store_n(&p->as->vma_count, (u32)PROC_VMA_MAX, __ATOMIC_RELAXED);
    TEST_EXPECT_EQ(detach(p, X + 17 * P, P), (s64)-1, "a middle cut at the cap is refused");
    TEST_EXPECT_EQ(munmap_range(p, X + 17 * P, P), ERR(T_E_NOMEM), "munmap: ENOMEM");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, X + 17 * P), leaf1, "refused: the PTE stays");
    TEST_EXPECT_EQ(pages_of(p->as), pages, "refused: nothing released");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 3u, "refused: nothing cut");
    TEST_EXPECT_EQ(detach(p, X + 16 * P, P), (s64)0, "control: a head trim at the cap needs no slot");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 3u, "still three mappings");
    __atomic_store_n(&p->as->vma_count, saved, __ATOMIC_RELAXED);
    TEST_EXPECT_EQ(detach(p, X + 18 * P, P), (s64)0, "with headroom the middle cut proceeds");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 4u, "one more mapping");
    TEST_EXPECT_EQ(pte_of(p->as->pgtable_root, X + 17 * P), leaf1, "page 1 survives in the head piece");

    // (d) Shapes and the window.
    TEST_EXPECT_EQ(munmap_range(p, X + 1, P), ERR(T_E_INVAL), "unaligned: EINVAL");
    TEST_EXPECT_EQ(munmap_range(p, X, 0), ERR(T_E_INVAL), "zero length: EINVAL");
    TEST_EXPECT_EQ(munmap_range(p, 0x40000000ull, P), ERR(T_E_NOSYS), "below the window: ENOSYS");
    TEST_EXPECT_EQ(detach(p, 0x40000000ull, P), (s64)-1, "native below the window: -1");
    TEST_EXPECT_EQ(pages_of(p->as), pages, "nothing released by any refusal");

    drop(p);                             // the CODE mapping goes with the Proc
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "every charge returned");
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_detach_eager_pages_go_with_the_last_piece(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();

    s64 r = sys_burrow_attach_for_proc(p, 4 * P);
    TEST_ASSERT(r > 0, "eager attach 4 pages");
    u64 va = (u64)r;
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "4 charged at attach");

    TEST_EXPECT_EQ(detach(p, va + P, 2 * P), (s64)0, "cut the middle");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "two pieces");
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "a trim refunds nothing: the block is one allocation");
    TEST_EXPECT_EQ(detach(p, va, P), (s64)0, "the head piece goes whole");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 1u, "one piece");
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "still charged: the tail piece keeps the region alive");
    TEST_EXPECT_EQ(detach(p, va + 3 * P, P), (s64)0, "the last piece goes");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "the region's charge refunded with its last mapping");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");

    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_detach_lazy_over_256mib_detaches(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();

    // 512 MiB = 131072 slots: a depth-2 map (root + leaves).
    s64 r = sys_burrow_attach_lazy_for_proc(p, MIB(512));
    TEST_ASSERT(r > 0, "lazy attach 512 MiB");
    u64 va = (u64)r;
    TEST_EXPECT_EQ((int)fault(p, va, true), (int)FAULT_HANDLED, "touch the first page");
    TEST_EXPECT_EQ((int)fault(p, va + MIB(512) - P, true), (int)FAULT_HANDLED, "touch the last page");
    struct Vma *v = vma_lookup(p, va);
    TEST_ASSERT(v != NULL, "the mapping");
    TEST_EXPECT_EQ(pagemap_node_count(&v->burrow->pm), 3u, "root + two leaves");
    TEST_EXPECT_EQ(pages_of(p->as), 5u, "2 pages + 3 nodes charged");
    TEST_EXPECT_EQ(detach(p, va, MIB(512)), (s64)0, "detached (the old cap refused this)");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");

    // The reserve form, released through the phenotype munmap.
    r = reserve(p, MIB(512), PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 512 MiB RW");
    va = (u64)r;
    TEST_EXPECT_EQ((int)fault(p, va + MIB(256), true), (int)FAULT_HANDLED, "touch the middle");
    TEST_EXPECT_EQ(pages_of(p->as), 3u, "1 page + root + one leaf");
    TEST_EXPECT_EQ(munmap_range(p, va, MIB(512)), (s64)0, "munmap 512 MiB");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");

    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_detach_four_gib_reservation_round_trips(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();

    s64 r = reserve(p, GIB(4), PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 4 GiB RW");
    u64 va = (u64)r;
    // 2^20 slots: depth 3. One page per 512 MiB: k = 0..7 at slot k * 2^17,
    // so pairs (0,1) (2,3) (4,5) (6,7) share a level-1 node each and every
    // page has its own leaf: 1 root + 4 + 8 = 13 nodes.
    for (u64 k = 0; k < 8; k++)
        TEST_EXPECT_EQ((int)fault(p, va + k * MIB(512), true), (int)FAULT_HANDLED, "touch one page per 512 MiB");
    struct Vma *v = vma_lookup(p, va);
    TEST_ASSERT(v != NULL, "the reservation's mapping");
    struct Burrow *b = v->burrow;
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 8u, "8 resident");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 13u, "13 nodes");
    TEST_EXPECT_EQ(pages_of(p->as), 8u + pagemap_node_count(&b->pm), "pages + nodes charged");
    TEST_EXPECT_EQ(pages_of(p->as), 21u, "= 21");

    // The middle GiB to R: three pieces of one Burrow.
    TEST_EXPECT_EQ(protect(p, va + MIB(1536), GIB(1), PR_R, 0), (s64)0, "protect [1.5G, 2.5G) to R");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 3u, "three pieces");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 8u, "a protect releases nothing");

    // A 2 GiB range across the pieces: the first piece's tail, the R piece
    // whole, the last piece's head. Pages k = 2..5 go, and with them their
    // four leaves and the two level-1 nodes that emptied.
    TEST_EXPECT_EQ(detach(p, va + GIB(1), GIB(2)), (s64)0, "detach [1G, 3G)");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 2u, "two survivors");
    struct Vma *lo = vma_lookup(p, va);
    struct Vma *hi = vma_lookup(p, va + GIB(3));
    TEST_ASSERT(lo && hi, "both survivors");
    TEST_EXPECT_EQ(lo->vaddr_end,   va + GIB(1), "the first piece trimmed to [0, 1G)");
    TEST_EXPECT_EQ(hi->vaddr_start, va + GIB(3), "the last piece trimmed to [3G, 4G)");
    TEST_EXPECT_EQ(hi->vaddr_end,   va + GIB(4), "to its end");
    TEST_EXPECT_EQ(ident(hi, va + GIB(3)), GIB(3), "identity preserved");
    TEST_EXPECT_EQ(hi->prot, (u32)PR_RW, "the last piece kept RW");
    TEST_ASSERT(vma_lookup(p, va + GIB(2)) == NULL, "the R piece is gone");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 4u, "4 resident");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 7u, "root + 2 level-1 + 4 leaves");
    TEST_EXPECT_EQ(pages_of(p->as), 11u, "4 pages + 7 nodes charged");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + MIB(512)) != 0, "k=1 stays installed");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + GIB(1)) == 0,   "k=2 is gone");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + MIB(2560)) == 0, "k=5 is gone");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + GIB(3)) != 0,   "k=6 stays installed");

    TEST_EXPECT_EQ(detach(p, va, GIB(4)), (s64)0, "the rest");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");

    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

// =============================================================================
// Capacity: the pagemap's nodes, the replace, the pool.
// =============================================================================

void test_capacity_window_sized_reservation_releases_in_bounded_steps(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();

    // The whole window: 2^34 slots, a depth-4 map. A reservation is free, so a
    // program may hold this; its release must cost what was TOUCHED.
    s64 r = reserve(p, BURROW_RESERVE_MAX, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve the whole window");
    u64 va = (u64)r;
    TEST_EXPECT_EQ((int)fault(p, va, true), (int)FAULT_HANDLED, "touch the first page");
    TEST_EXPECT_EQ((int)fault(p, va + BURROW_RESERVE_MAX - P, true), (int)FAULT_HANDLED, "touch the last page");
    struct Vma *v = vma_lookup(p, va);
    TEST_ASSERT(v != NULL, "the mapping");
    TEST_EXPECT_EQ(v->burrow->pm.depth, 4u, "a depth-4 map");
    TEST_EXPECT_EQ(pagemap_node_count(&v->burrow->pm), 7u, "one root, two distinct 3-node paths");
    TEST_EXPECT_EQ(pages_of(p->as), 9u, "2 pages + 7 nodes charged");

    u64 steps0 = pagemap_walk_steps();
    TEST_EXPECT_EQ(detach(p, va, BURROW_RESERVE_MAX), (s64)0, "detach the whole window");
    u64 steps = pagemap_walk_steps() - steps0;
    // Two takes and the final miss, each at most depth x 512 entries down and
    // as many back up: thousands, against the 2^34 slots a per-slot walk visits.
    TEST_ASSERT(steps >= 8u, "the walk happened (control)");
    TEST_ASSERT(steps <= 3u * 2u * 4u * 512u, "and visited present nodes only, never the range");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");

    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_capacity_replace_window_releases_orphans(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 vmas0 = count_vmas(p->as);
    u32 pool0 = capacity_pool_charged();

    s64 r = reserve(p, 8 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 8 pages RW");
    u64 va = (u64)r;
    for (u64 i = 0; i < 8; i++) TEST_ASSERT(touch_tag(p, va, i), "touch + tag every page");
    struct Vma *ov = vma_lookup(p, va);
    TEST_ASSERT(ov != NULL, "the mapping");
    struct Burrow *old = ov->burrow;
    struct page *keep5 = slot_page(ov, va + 5 * P);
    TEST_EXPECT_EQ(pages_of(p->as), 8u, "8 charged");

    // A fresh lazy Burrow over [2P, 5P): the window's three slots are released
    // BEFORE the swap, so nothing stays charged that no mapping names (F5).
    struct Burrow *nb = burrow_create_anon_lazy(3 * P);
    TEST_ASSERT(nb != NULL, "a fresh lazy Burrow");
    TEST_EXPECT_EQ(map_fixed_locked(p, nb, va + 2 * P, 3 * P, VMA_PROT_RW, 0), 0, "MAP_FIXED over [2P, 5P)");
    burrow_unref(nb);                    // the mapping holds it
    TEST_EXPECT_EQ(pages_of(p->as), 5u, "the window's 3 pages uncharged");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(old), 5u, "and released from the old Burrow");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0 + 3u, "head, window, tail");
    struct Vma *w = vma_lookup(p, va + 2 * P);
    struct Vma *h = vma_lookup(p, va);
    struct Vma *t = vma_lookup(p, va + 5 * P);
    TEST_ASSERT(w && w->burrow == nb, "the window names the new Burrow");
    TEST_ASSERT(h && h->burrow == old && h->vaddr_end == va + 2 * P, "the head is the old Burrow's");
    TEST_ASSERT(t && t->burrow == old && t->vaddr_start == va + 5 * P, "so is the tail");
    TEST_EXPECT_EQ(ident(t, va + 5 * P), 5 * P, "the tail's identity");
    TEST_ASSERT(slot_page(t, va + 5 * P) == keep5, "page 5 is the same page");
    TEST_EXPECT_EQ(slot_words(t, va + 5 * P)[0], 0xC0DE0005u, "with its contents");
    TEST_ASSERT(pte_of(p->as->pgtable_root, va + 3 * P) == 0, "the window's PTEs are gone");
    TEST_EXPECT_EQ((int)fault(p, va + 3 * P, true), (int)FAULT_HANDLED, "the window faults fresh");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(nb), 1u, "into the new Burrow");
    TEST_EXPECT_EQ(pages_of(p->as), 6u, "charged once");

    TEST_EXPECT_EQ(detach(p, va, 8 * P), (s64)0, "the rest");
    TEST_EXPECT_EQ(count_vmas(p->as), vmas0, "no mappings");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");

    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_capacity_pagemap_nodes_charged_and_reclaimed(void) {
    u64 free_before = phys_free_pages();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 pool0 = capacity_pool_charged();

    // 1 GiB = 2^18 slots = 512^2: depth 2. Untouched, it holds no node at all.
    s64 r = reserve(p, GIB(1), PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 1 GiB RW");
    u64 va = (u64)r;
    struct Vma *v = vma_lookup(p, va);
    TEST_ASSERT(v != NULL, "the mapping");
    struct Burrow *b = v->burrow;
    TEST_EXPECT_EQ(b->pm.depth, 2u, "depth 2");
    TEST_ASSERT(b->pm.root == NULL, "untouched: no root");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 0u, "no nodes");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");

    TEST_EXPECT_EQ((int)fault(p, va, true), (int)FAULT_HANDLED, "touch slot 0");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 2u, "root + leaf 0");
    TEST_EXPECT_EQ(pages_of(p->as), 3u, "1 page + 2 nodes");
    TEST_EXPECT_EQ((int)fault(p, va + P, true), (int)FAULT_HANDLED, "touch slot 1");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 2u, "the same leaf");
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "+1 page");
    TEST_EXPECT_EQ((int)fault(p, va + 600 * P, true), (int)FAULT_HANDLED, "touch slot 600");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 3u, "leaf 1");
    TEST_EXPECT_EQ(pages_of(p->as), 6u, "+1 page +1 node");

    TEST_EXPECT_EQ(sys_burrow_decommit_for_proc(p, va + 600 * P, P), (s64)0, "decommit slot 600");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 2u, "leaf 1 reclaimed");
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "-1 page -1 node");
    TEST_EXPECT_EQ(sys_burrow_decommit_for_proc(p, va, 2 * P), (s64)0, "decommit slots 0, 1");
    TEST_EXPECT_EQ(pagemap_node_count(&b->pm), 0u, "leaf 0 and the root reclaimed");
    TEST_ASSERT(b->pm.root == NULL, "no root again");
    TEST_ASSERT(pagemap_live(&b->pm), "the map itself lives on");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(b), 0u, "nothing resident");
    TEST_EXPECT_EQ((int)fault(p, va + P, true), (int)FAULT_HANDLED, "a touch after the decommit re-faults");
    TEST_EXPECT_EQ(pages_of(p->as), 3u, "1 page + root + leaf again");

    TEST_EXPECT_EQ(detach(p, va, GIB(1)), (s64)0, "detach");
    TEST_EXPECT_EQ(pages_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_capacity_default_is_ram_minus_reserve(void) {
    u64 total   = phys_total_pages();
    u64 pool    = capacity_pool_pages();
    u64 reserve = capacity_reserve_pages();
    TEST_ASSERT(total > 0, "the machine has RAM");

    // Facts a wrong formula would break -- none of them the formula itself
    // (the round-1 audit's F7: a test that recomputes the rule compares the
    // rule with itself).
    TEST_EXPECT_EQ(pool + reserve, total, "pool + reserve is the machine");
    TEST_ASSERT(pool > 0, "the pool is never empty");
    TEST_ASSERT(reserve >= total / 8, "the reserve is at least an eighth of RAM");
    TEST_ASSERT(reserve <= total / 2, "and at most half");
    if (total >= 2ull * CAPACITY_RESERVE_MIN_PAGES)
        TEST_ASSERT(reserve >= CAPACITY_RESERVE_MIN_PAGES, "at least 256 MiB when RAM can spare it");
    else
        TEST_EXPECT_EQ(reserve, total / 2, "half of a machine that cannot");
    // The CI guest (2 GiB): the concrete figures, so a changed formula shows.
    if (total == 524288ull) {
        TEST_EXPECT_EQ(reserve, 65536ull, "2 GiB: the reserve is 256 MiB");
        TEST_EXPECT_EQ(pool, 458752ull, "2 GiB: the pool is 1792 MiB");
    }
    TEST_EXPECT_EQ(proc_default_page_budget(), capacity_pool_pages(), "the default budget is the pool");
    TEST_EXPECT_EQ(proc_page_budget_hard_max(), capacity_pool_pages(), "and so is the hard max");

    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(p->page_budget, capacity_pool_pages(), "a fresh Proc carries the pool as its budget");
    TEST_EXPECT_EQ(__atomic_load_n(&p->as->page_budget, __ATOMIC_ACQUIRE), capacity_pool_pages(),
                   "and so does its address space");
    drop(p);
}

void test_capacity_pool_refuses_users_keeps_tcb(void) {
    // The pool's reclaim (audit F8) would otherwise strip the idle images
    // earlier exec tests left cached and move the refusal point.
    (void)image_cache_evict_idle_for_test();
    u64 free_before = phys_free_pages();
    struct Proc *A = mk();
    struct Proc *S = mk();
    TEST_ASSERT(A && S, "two Procs");
    S->principal_id = PRINCIPAL_SYSTEM;  // the TCB; A is a user
    TEST_ASSERT(proc_resource_exempt(S) && !proc_resource_exempt(A), "S exempt, A not");

    const u32 pool  = capacity_pool_pages();
    const u32 pool0 = capacity_pool_charged();
    const u32 K     = 64u;
    TEST_ASSERT(pool0 + K < pool, "the pool has room to park (else the machine is already saturated)");

    // The pool is parked down to K pages of room by accounting alone.
    const u32 park = pool - pool0 - K;
    capacity_pool_park_for_test(park);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool - K, "K pages left machine-wide");

    // A, far below its own budget, is refused at exactly K: the first touch
    // takes its page, the map's one node and the three tables of a fresh path;
    // every later touch one page. The refused touch is refused at its page,
    // so it allocates nothing.
    s64 r = reserve(A, MIB(1), PR_RW, 0);
    TEST_ASSERT(r > 0, "A reserves 1 MiB");
    u64 va = (u64)r;
    u32 n = 0;
    for (u64 i = 0; i < 256; i++) {
        if (fault(A, va + i * P, true) != FAULT_HANDLED) break;
        n++;
    }
    TEST_EXPECT_EQ(n, K - 4u, "A got K-4 pages (the node and the three tables took the rest)");
    TEST_EXPECT_EQ(count_of(A->as), K, "A holds exactly K");
    TEST_EXPECT_EQ(tables_of(A->as), 3u, "three of them tables");
    TEST_ASSERT(count_of(A->as) < __atomic_load_n(&A->as->page_budget, __ATOMIC_ACQUIRE) / 64u,
                "refused far below A's own budget: the bound was the pool");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool, "the pool is full");
    TEST_ASSERT(pte_of(A->as->pgtable_root, va + (u64)n * P) == 0, "the refused touch installed nothing");
    struct Vma *av = vma_lookup(A, va);
    TEST_ASSERT(av != NULL, "A's mapping");
    TEST_EXPECT_EQ(burrow_lazy_resident_count(av->burrow), n, "and committed nothing");

    // The TCB is never refused by the pool.
    s64 rs = reserve(S, P, PR_RW, 0);
    TEST_ASSERT(rs > 0, "S reserves a page");
    TEST_EXPECT_EQ((int)fault(S, (u64)rs, true), (int)FAULT_HANDLED, "S's touch is served past the pool");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool + 4u, "counted, not refused: a page and three tables");

    // Everything returns.
    TEST_EXPECT_EQ(detach(S, (u64)rs, P), (s64)0, "S detaches");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool, "S's four pages back");
    TEST_EXPECT_EQ(detach(A, va, MIB(1)), (s64)0, "A detaches");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool - K, "A's K back");
    TEST_EXPECT_EQ(count_of(A->as), 0u, "A holds nothing");
    capacity_pool_unpark_for_test(park);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the park back");

    // With room again A is served: the bound is the pool, not A's history.
    r = reserve(A, P, PR_RW, 0);
    TEST_ASSERT(r > 0, "A reserves a page");
    TEST_EXPECT_EQ((int)fault(A, (u64)r, true), (int)FAULT_HANDLED, "A's touch is served");
    TEST_EXPECT_EQ(detach(A, (u64)r, P), (s64)0, "A detaches");

    drop(A);
    drop(S);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_capacity_fork_clone_charges_pages_and_nodes(void) {
    u64 free_before = phys_free_pages();
    u32 pool0 = capacity_pool_charged();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // Two touched GiB maps: M1 with slots 0 and 600 resident (2 pages; a root
    // and two leaves), M2 with slot 0 resident (1 page; a root and a leaf).
    s64 r1 = reserve(p, GIB(1), PR_RW, 0);
    TEST_ASSERT(r1 > 0, "M1");
    TEST_EXPECT_EQ((int)fault(p, (u64)r1, true), (int)FAULT_HANDLED, "M1 slot 0");
    TEST_EXPECT_EQ((int)fault(p, (u64)r1 + 600 * P, true), (int)FAULT_HANDLED, "M1 slot 600");
    s64 r2 = reserve(p, GIB(1), PR_RW, 0);
    TEST_ASSERT(r2 > 0, "M2");
    TEST_EXPECT_EQ((int)fault(p, (u64)r2, true), (int)FAULT_HANDLED, "M2 slot 0");
    TEST_EXPECT_EQ(pages_of(p->as), 8u, "the parent: 3 pages + 5 nodes");

    // The clone mirrors every node page as well as every resident page, and
    // the child's own takes refund both -- so both are charged, or the child
    // refunds what it never paid and walks the pool below what is held.
    TEST_EXPECT_EQ(tables_of(p->as), 6u, "the parent's tables: an L1, two L2s, three L3s");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 14u, "the pool: pages, nodes and tables, once each");
    struct AddrSpace *child = addrspace_clone(p->as, false);
    TEST_ASSERT(child != NULL, "the clone");
    TEST_EXPECT_EQ(pages_of(child), 8u, "the child is charged the mirror's pages AND nodes");
    TEST_EXPECT_EQ(tables_of(p->as), 6u, "the parent's tables stay linked across the fork (audit F9), empty until it re-faults");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 19u,
                   "the pool: the parent's 3 pages + 5 nodes + 6 tables, the child's 5 mirrored nodes");

    // Detaching M1 in the child refunds exactly M1's footprint: M2's stays on
    // the child, and the parent's stays on the pool.
    struct Burrow *dead = NULL;
    spin_lock(&child->lock);
    int rc = vma_detach_range_in(child, false, NULL, (u64)r1, GIB(1), 0, &dead);
    spin_unlock(&child->lock);
    burrow_free_deferred(dead);
    TEST_EXPECT_EQ(rc, 0, "the child detaches M1");
    TEST_EXPECT_EQ(pages_of(child), 3u, "M2's page and its two nodes remain on the child");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 16u, "the pool: the parent's 14 + the child's 2 remaining mirror nodes");
    TEST_EXPECT_EQ(pages_of(p->as), 8u, "the parent is untouched");

    addrspace_unref(child);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 14u, "the child's death returns the rest; the parent's tables are still its own");
    drop(p);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_capacity_death_returns_charges_to_pool(void) {
    u64 free_before = phys_free_pages();
    u32 pool0 = capacity_pool_charged();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // Pages, an eager region and a large map's nodes, all charged; then the
    // Proc dies WITHOUT detaching any of it.
    s64 r = reserve(p, 8 * P, PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 8 pages");
    for (u64 i = 0; i < 4; i++)
        TEST_EXPECT_EQ((int)fault(p, (u64)r + i * P, true), (int)FAULT_HANDLED, "touch 4");
    TEST_ASSERT(sys_burrow_attach_for_proc(p, 2 * P) > 0, "eager attach 2 pages");
    s64 big = reserve(p, GIB(1), PR_RW, 0);
    TEST_ASSERT(big > 0, "reserve 1 GiB");
    TEST_EXPECT_EQ((int)fault(p, (u64)big, true), (int)FAULT_HANDLED, "touch it once: a page + 2 nodes");
    TEST_EXPECT_EQ(pages_of(p->as), 9u, "4 + 2 + 3 charged");
    TEST_EXPECT_EQ(tables_of(p->as), 3u, "one path of tables under all of it");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 12u, "all of it on the pool, tables included");

    drop(p);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "a dead address space returns its charges");
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "and its pages");
}

// =============================================================================
// The round-1 close: page tables (audit F1) and the physical pool (F5).
// =============================================================================

void test_capacity_page_tables_charged_and_reclaimed(void) {
    u64 free_before = phys_free_pages();
    u32 pool0 = capacity_pool_charged();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    TEST_EXPECT_EQ(tables_of(p->as), 0u, "a fresh space holds no table");
    // 4 GiB-aligned, so the 2 MiB and 1 GiB steps below cross exactly the
    // table boundaries they name.
    const u64 X = HI_VA;

    // A 4 GiB lazy map: 2^20 slots, depth 3 (root + mid + leaf on every path).
    struct Burrow *lz = burrow_create_anon_lazy(GIB(4));
    TEST_ASSERT(lz != NULL, "a lazy 4 GiB");
    TEST_EXPECT_EQ(map_at(p, lz, X, GIB(4), VMA_PROT_RW), 0, "map it at X");
    burrow_unref(lz);

    // The first touch grows the whole path: L1 + L2 + L3, charged and physical.
    TEST_EXPECT_EQ((int)fault(p, X, true), (int)FAULT_HANDLED, "touch X");
    TEST_EXPECT_EQ(tables_of(p->as), 3u, "L1 + L2 + L3");
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "1 page + 3 nodes");
    TEST_EXPECT_EQ(count_of(p->as), 7u, "page_count carries the tables");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 7u, "the pool counts all seven");

    // The next 2 MiB region shares the L2: one more L3 (and one more leaf node).
    TEST_EXPECT_EQ((int)fault(p, X + MIB(2), true), (int)FAULT_HANDLED, "touch X + 2 MiB");
    TEST_EXPECT_EQ(tables_of(p->as), 4u, "+1 L3");
    TEST_EXPECT_EQ(pages_of(p->as), 6u, "+1 page +1 leaf");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 10u, "the pool follows");

    // The next GiB needs its own L2 and L3 (and a mid node and a leaf).
    TEST_EXPECT_EQ((int)fault(p, X + GIB(1), true), (int)FAULT_HANDLED, "touch X + 1 GiB");
    TEST_EXPECT_EQ(tables_of(p->as), 6u, "+1 L2 +1 L3");
    TEST_EXPECT_EQ(pages_of(p->as), 9u, "+1 page +2 nodes");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 15u, "the pool follows");

    // A decommit that empties an L3 reclaims it; its L2 stays while a sibling
    // L3 lives under it.
    TEST_EXPECT_EQ(sys_burrow_decommit_for_proc(p, X + MIB(2), P), (s64)0, "decommit X + 2 MiB");
    TEST_EXPECT_EQ(tables_of(p->as), 5u, "its L3 reclaimed; the L2 holds X's L3");
    TEST_EXPECT_EQ(pages_of(p->as), 7u, "-1 page -1 leaf");
    TEST_ASSERT(pte_of(p->as->pgtable_root, X) != 0, "X is still mapped");

    // One that empties an L3 AND its L2 reclaims both; the L1 holds X's L2.
    TEST_EXPECT_EQ(sys_burrow_decommit_for_proc(p, X + GIB(1), P), (s64)0, "decommit X + 1 GiB");
    TEST_EXPECT_EQ(tables_of(p->as), 3u, "L3 and L2 reclaimed");
    TEST_EXPECT_EQ(pages_of(p->as), 4u, "-1 page -2 nodes");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 7u, "the pool: back to the first touch");

    // The last page: L3, L2 and L1 all go, and the L0 entry is unlinked.
    TEST_EXPECT_EQ(sys_burrow_decommit_for_proc(p, X, P), (s64)0, "decommit X");
    TEST_EXPECT_EQ(tables_of(p->as), 0u, "every table reclaimed");
    TEST_EXPECT_EQ(count_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    const u64 *l0 = (const u64 *)pa_to_kva(p->as->pgtable_root);
    TEST_EXPECT_EQ(l0[(X >> 39) & 0x1ff], 0ull, "the L1 is unlinked from the L0");

    // Reclaimed is not gone for good: a touch rebuilds the path.
    TEST_EXPECT_EQ((int)fault(p, X, true), (int)FAULT_HANDLED, "touch X again");
    TEST_EXPECT_EQ(tables_of(p->as), 3u, "L1 + L2 + L3 again");
    TEST_EXPECT_EQ(count_of(p->as), 7u, "1 page + 3 nodes + 3 tables");

    // The range form reclaims as the single form does.
    TEST_EXPECT_EQ(detach(p, X, GIB(4)), (s64)0, "detach the map");
    TEST_EXPECT_EQ(tables_of(p->as), 0u, "the range form reclaims too");
    TEST_EXPECT_EQ(count_of(p->as), 0u, "nothing charged");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_capacity_memory_bomb_leaves_the_reserve(void) {
    // The pool's reclaim (audit F8) would otherwise strip the idle images
    // earlier exec tests left cached and move the refusal point.
    (void)image_cache_evict_idle_for_test();
    u64 free_before = phys_free_pages();
    struct Proc *A = mk();
    struct Proc *S = mk();
    TEST_ASSERT(A && S, "two Procs");
    S->principal_id = PRINCIPAL_SYSTEM;
    const u32 pool  = capacity_pool_pages();
    const u32 pool0 = capacity_pool_charged();
    const u32 K     = 64u;
    TEST_ASSERT(pool0 + K < pool, "the pool has room to park");
    const u32 park = pool - pool0 - K;
    const u32 base = pool - K;
    capacity_pool_park_for_test(park);
    TEST_EXPECT_EQ(capacity_pool_charged(), base, "K pages of room");

    // The round-1 attack: one page per 2 MiB of a large reservation. Each
    // touch costs a page, a leaf node and an L3 table (the first an L1, an L2
    // and the map's root as well), and every one of them is a physical page
    // the pool counts, so A is refused within one touch's cost of the pool,
    // tables included, and its physical footprint never exceeds its room. A
    // pool blind to tables would serve half as many touches again and leave
    // that many uncounted table pages standing -- the reserve eaten by what
    // nothing counted.
    s64 r = reserve(A, MIB(512), PR_RW, 0);
    TEST_ASSERT(r > 0, "A reserves 512 MiB");
    u64 va = (u64)r;
    u64 free_loop = phys_free_pages();
    u32 n = 0;
    for (u64 i = 0; i < 256; i++) {
        if (fault(A, va + i * MIB(2), true) != FAULT_HANDLED) break;
        n++;
    }
    TEST_ASSERT(n > 0 && n < 256, "refused before the reservation runs out");
    TEST_EXPECT_EQ(n, (K - 3u) / 3u, "3 per touch, plus the L1, the L2 and the root");
    TEST_EXPECT_EQ(count_of(A->as), 3u * n + 3u, "everything A caused to exist is on A");
    TEST_EXPECT_EQ(tables_of(A->as), n + 2u, "an L3 per touch, plus the L1 and the L2");
    TEST_EXPECT_EQ(pages_of(A->as), 2u * n + 1u, "a page and a leaf per touch, plus the root");
    TEST_ASSERT(pool - capacity_pool_charged() < 3u, "refused within one touch's cost of the pool");
    TEST_ASSERT(free_loop - phys_free_pages() <= (u64)K, "A's physical footprint is bounded by its room");

    // The TCB keeps allocating from the reserve.
    s64 rs = reserve(S, P, PR_RW, 0);
    TEST_ASSERT(rs > 0, "S reserves a page");
    TEST_EXPECT_EQ((int)fault(S, (u64)rs, true), (int)FAULT_HANDLED, "S's touch is served past the pool");
    TEST_EXPECT_EQ(detach(S, (u64)rs, P), (s64)0, "S detaches");

    // Relinquished memory returns, TABLES INCLUDED: after the decommit A holds
    // nothing, the pool is back at the park, and the same attack is refused at
    // the same point -- a pool that kept the tables would let A's footprint
    // ratchet up by one table per round, which is the attack.
    TEST_EXPECT_EQ(sys_burrow_decommit_for_proc(A, va, MIB(512)), (s64)0, "A decommits the lot");
    TEST_EXPECT_EQ(count_of(A->as), 0u, "A holds nothing");
    TEST_EXPECT_EQ(tables_of(A->as), 0u, "every table reclaimed");
    TEST_EXPECT_EQ(capacity_pool_charged(), base, "the pool is back at the park");
    u32 n2 = 0;
    for (u64 i = 0; i < 256; i++) {
        if (fault(A, va + i * MIB(2), true) != FAULT_HANDLED) break;
        n2++;
    }
    TEST_EXPECT_EQ(n2, n, "the second round is refused at the same point: nothing leaked");

    TEST_EXPECT_EQ(detach(A, va, MIB(512)), (s64)0, "A detaches");
    TEST_EXPECT_EQ(count_of(A->as), 0u, "A holds nothing");
    capacity_pool_unpark_for_test(park);
    drop(A);
    drop(S);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

void test_capacity_fork_costs_the_pool_only_its_nodes(void) {
    // The pool's reclaim (audit F8) would otherwise strip the idle images
    // earlier exec tests left cached and move the refusal point.
    (void)image_cache_evict_idle_for_test();
    u64 free_before = phys_free_pages();
    u32 pool0 = capacity_pool_charged();
    struct Proc *p = mk();
    TEST_ASSERT(p != NULL, "proc_alloc");

    // A 1 GiB map with 8 resident pages under one leaf: 8 pages + 2 nodes +
    // 3 tables, each once on the pool.
    s64 r = reserve(p, GIB(1), PR_RW, 0);
    TEST_ASSERT(r > 0, "reserve 1 GiB");
    for (u64 i = 0; i < 8; i++)
        TEST_EXPECT_EQ((int)fault(p, (u64)r + i * P, true), (int)FAULT_HANDLED, "touch 8");
    TEST_EXPECT_EQ(pages_of(p->as), 10u, "8 pages + root + leaf");
    TEST_EXPECT_EQ(tables_of(p->as), 3u, "L1 + L2 + L3");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 13u, "the pool counts each once");

    // Park the pool so the room left is LESS than the parent's resident set.
    // A pool that charged the fork the pages it shares would refuse it here
    // while free memory exists (the round-1 audit's F5); the physical pool
    // pays only the node mirror, which fits.
    const u32 pool = capacity_pool_pages();
    u32 cur = capacity_pool_charged();
    TEST_ASSERT(cur + 4u < pool, "room to park");
    const u32 park = pool - cur - 4u;
    capacity_pool_park_for_test(park);
    struct AddrSpace *child = addrspace_clone(p->as, false);
    TEST_ASSERT(child != NULL, "the fork is served with room for four pages, not eight");
    TEST_EXPECT_EQ(pages_of(child), 10u, "the child COUNTS every page it maps (the holder reading)");
    TEST_EXPECT_EQ(tables_of(child), 0u, "and holds no table yet");
    TEST_EXPECT_EQ(tables_of(p->as), 3u, "the parent's tables stay linked across the fork (audit F9)");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool - 4u + 2u,
                   "the pool: two mirrored nodes in, no page, no table out");
    capacity_pool_unpark_for_test(park);

    // The child's death returns its nodes; the shared pages stay with the parent.
    addrspace_unref(child);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + 13u, "the pool: the parent's pages, nodes and tables alone");
    TEST_EXPECT_EQ(pages_of(p->as), 10u, "the parent is untouched");
    TEST_EXPECT_EQ(detach(p, (u64)r, GIB(1)), (s64)0, "detach");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the pool is back");
    drop(p);
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "phys free back to baseline");
}

// B-1b: the phenotype madvise row's hint answer -- a range is "mapped" only
// when every byte of it is under some mapping (Linux's ENOMEM for a hole).
void test_vma_range_is_mapped(void) {
    struct Proc *p = mk();
    TEST_ASSERT(p, "proc");
    s64 va = sys_burrow_reserve_for_proc(p, 4 * P, PR_RW, 0);
    TEST_ASSERT(va > 0, "reserve 4 pages");
    u64 lo = (u64)va;
    spin_lock(&p->as->lock);
    bool whole = vma_range_is_mapped_in(p->as, lo, lo + 4 * P);
    bool part  = vma_range_is_mapped_in(p->as, lo + P, lo + 3 * P);
    bool past  = vma_range_is_mapped_in(p->as, lo, lo + 5 * P);
    bool below = vma_range_is_mapped_in(p->as, lo - P, lo + P);
    bool empty = vma_range_is_mapped_in(p->as, lo, lo);
    spin_unlock(&p->as->lock);
    TEST_ASSERT(whole, "the whole reservation is mapped");
    TEST_ASSERT(part,  "an interior range is mapped");
    TEST_ASSERT(!past, "a range running past the end is not");
    TEST_ASSERT(!below, "a range starting below it is not");
    TEST_ASSERT(!empty, "an empty range is not");
    // Cut a hole in the middle: the two remaining pieces do not cover it.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, lo + P, 2 * P), (s64)0, "detach the middle");
    spin_lock(&p->as->lock);
    bool across = vma_range_is_mapped_in(p->as, lo, lo + 4 * P);
    bool head   = vma_range_is_mapped_in(p->as, lo, lo + P);
    bool tail   = vma_range_is_mapped_in(p->as, lo + 3 * P, lo + 4 * P);
    spin_unlock(&p->as->lock);
    TEST_ASSERT(!across, "a range across the hole is not mapped");
    TEST_ASSERT(head && tail, "the two pieces are");
    // The release core's errnos over the same shapes (B-1b): a hole is ENOMEM.
    extern s64 sys_burrow_decommit_core(struct Proc *, u64, u64);
    TEST_EXPECT_EQ(sys_burrow_decommit_core(p, lo, 4 * P), ERR(T_E_NOMEM),
                   "decommit across the hole is ENOMEM");
    TEST_EXPECT_EQ(sys_burrow_decommit_core(p, lo, P), (s64)0, "decommit the head piece");
    TEST_EXPECT_EQ(sys_burrow_decommit_core(p, lo + 1, P), ERR(T_E_INVAL),
                   "an unaligned decommit is EINVAL");
    TEST_EXPECT_EQ(sys_burrow_decommit_core(p, EXEC_USER_STACK_BASE, P), ERR(T_E_NOSYS),
                   "below the window the row is not served");
    TEST_EXPECT_EQ(sys_burrow_decommit_for_proc(p, lo, 4 * P), (s64)-1,
                   "the native 84 flattens the refusal to -1");
    drop(p);
}
