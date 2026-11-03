// What retention accounting does today (docs/SHARED-MEMORY-ACCOUNTING.md 12).
//
// Each test pins a behaviour the accounts must change on purpose, so a later red
// here means the behaviour moved: change the test in the commit that moves it, and
// say so. None of these is a claim that the behaviour is right.
//   mm0.today_alias_charges_each_mapping
//     two shared-in mappings of one object charge the budget twice, the pool once.
//   mm0.today_partial_map_retains_the_whole
//     one surviving page of a cut DMA mapping keeps every page of the object, with
//     no budget charged, and a second wrapper over the object is a second reference.
//     A shared-in cut is refused: detach.range_refusals_change_nothing pins that.
//   mm0.today_orphan_pin_charges_no_proc
//     after the sharer walks away, a client's mapping keeps the backing charged to
//     the pool and to nobody's page_count.
//   mm0.today_dma_bypasses_the_pool
//     all three DMA kinds take raw pages; anonymous backing one variable away does
//     not, so a pool that stopped moving cannot pass this.

#include "test.h"

#include "../../mm/phys.h"

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/dma_handle.h>
#include <thylacine/exec.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#define P     PAGE_SIZE
#define HI_VA (EXEC_USER_BURROW_BASE + (1ull << 40))

// Whole blocks, so the buddy's rounding is not part of any figure below.
#define OBJECT_PAGES 4u

static void drop(struct Proc *p) {
    if (!p) return;
    p->state = 2;                        // PROC_STATE_ZOMBIE; proc_free drains VMAs
    proc_free(p);
}

static u32 shared_of(struct AddrSpace *as) {
    return __atomic_load_n(&as->shared_map_pages, __ATOMIC_ACQUIRE);
}

static u32 pages_of(struct AddrSpace *as) {
    return __atomic_load_n(&as->page_count, __ATOMIC_ACQUIRE);
}

void test_mm0_today_alias_charges_each_mapping(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u32 pool0 = capacity_pool_charged();
    struct Burrow *b = burrow_create_anon(OBJECT_PAGES * P, false);
    TEST_ASSERT(b != NULL, "burrow_create_anon");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + OBJECT_PAGES, "the backing is in the pool once");

    u64 second = HI_VA + 2 * OBJECT_PAGES * P;
    spin_lock(&p->as->lock);
    int a = burrow_share_into(p, b, HI_VA, VMA_PROT_RW);
    int c = burrow_share_into(p, b, second, VMA_PROT_RW);
    spin_unlock(&p->as->lock);
    TEST_EXPECT_EQ(a, 0, "the first alias");
    TEST_EXPECT_EQ(c, 0, "the second alias");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 2, "two mappings of one object");
    TEST_EXPECT_EQ(shared_of(p->as), 2 * OBJECT_PAGES, "the budget counts each alias");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + OBJECT_PAGES, "the pool does not");

    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, HI_VA, OBJECT_PAGES * P), (s64)0, "drop one alias");
    TEST_EXPECT_EQ(shared_of(p->as), OBJECT_PAGES, "its charge returns");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + OBJECT_PAGES, "the backing stays");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, second, OBJECT_PAGES * P), (s64)0, "drop the other");
    TEST_EXPECT_EQ(shared_of(p->as), 0u, "the budget is back");

    burrow_unref(b);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the construction reference held the last page");
    drop(p);
}

void test_mm0_today_partial_map_retains_the_whole(void) {
    struct Proc *p = proc_alloc();
    TEST_ASSERT(p != NULL, "proc_alloc");
    struct KObj_DMA *k = kobj_dma_create_weave(OBJECT_PAGES * P);
    TEST_ASSERT(k != NULL, "kobj_dma_create_weave");
    TEST_EXPECT_EQ(k->ref, 1, "the construction reference");

    // The shape SYS_DMA_MAP builds: a wrapper per map, its reference moved to the VMA.
    struct Burrow *b = burrow_create_dma(k);
    TEST_ASSERT(b != NULL, "burrow_create_dma");
    TEST_EXPECT_EQ(k->ref, 2, "the wrapper holds the object");
    spin_lock(&p->as->lock);
    int rc = burrow_map(p, b, HI_VA, OBJECT_PAGES * P, VMA_PROT_RW);
    spin_unlock(&p->as->lock);
    TEST_EXPECT_EQ(rc, 0, "map the whole object");
    burrow_unref(b);
    TEST_EXPECT_EQ(shared_of(p->as), 0u, "an owner's own map charges no shared-in budget");

    // Only inside the burrow window: outside it a hardware map detaches whole.
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, HI_VA, (OBJECT_PAGES - 1) * P), (s64)0, "cut all but one page");
    struct Vma *v = vma_lookup(p, HI_VA + (OBJECT_PAGES - 1) * P);
    TEST_ASSERT(v != NULL, "the last page's mapping survives");
    TEST_EXPECT_EQ(v->vaddr_end - v->vaddr_start, (u64)P, "and it is one page");
    TEST_EXPECT_EQ(k->ref, 2, "one page still retains the whole object");
    TEST_EXPECT_EQ(k->size, (size_t)(OBJECT_PAGES * P), "the object did not shrink");

    // Section 6's wrapper alias: identity is the object's, not the wrapper's.
    struct Burrow *w = burrow_create_dma(k);
    TEST_ASSERT(w != NULL, "a second wrapper");
    TEST_EXPECT_EQ(k->ref, 3, "a second wrapper is a second reference to one allocation");
    burrow_unref(w);
    TEST_EXPECT_EQ(k->ref, 2, "and goes with its own reference");

    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(p, HI_VA + (OBJECT_PAGES - 1) * P, P), (s64)0, "drop the last page");
    TEST_EXPECT_EQ(k->ref, 1, "only then is the mapping's hold gone");
    kobj_dma_unref(k);
    drop(p);
}

void test_mm0_today_orphan_pin_charges_no_proc(void) {
    struct Proc *sharer = proc_alloc();
    struct Proc *client = proc_alloc();
    TEST_ASSERT(sharer != NULL && client != NULL, "proc_alloc");
    // The charge record is keyed by pid and proc_alloc stamps 0.
    sharer->pid = 90021;
    client->pid = 90022;
    u32 base = pages_of(sharer->as);
    u32 cbase = pages_of(client->as);
    u32 pool0 = capacity_pool_charged();

    s64 va = sys_burrow_attach_for_proc(sharer, OBJECT_PAGES * P);
    TEST_ASSERT(va > 0, "the sharer attaches");
    TEST_EXPECT_EQ(pages_of(sharer->as), base + OBJECT_PAGES, "the sharer pays");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + OBJECT_PAGES, "the pool holds the backing");
    spin_lock(&sharer->as->lock);
    struct Vma *sv = vma_lookup(sharer, (u64)va);
    struct Burrow *b = sv ? sv->burrow : NULL;
    spin_unlock(&sharer->as->lock);
    TEST_ASSERT(b != NULL, "the sharer's Burrow");

    spin_lock(&client->as->lock);
    int rc = burrow_share_into(client, b, HI_VA, VMA_PROT_RW);
    spin_unlock(&client->as->lock);
    TEST_EXPECT_EQ(rc, 0, "share into the client");
    TEST_EXPECT_EQ(sys_burrow_detach_for_proc(sharer, (u64)va, OBJECT_PAGES * P), (s64)0, "the sharer walks away");

    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "the client's mapping is the last holder");
    TEST_EXPECT_EQ(pages_of(sharer->as), base, "the sharer no longer pays");
    TEST_EXPECT_EQ(pages_of(client->as), cbase, "the client never did");
    TEST_EXPECT_EQ(shared_of(client->as), OBJECT_PAGES, "the client's budget says what it maps");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + OBJECT_PAGES, "the backing is live and in the pool");

    vma_drain(client);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the holder's end returns it");
    TEST_EXPECT_EQ(pages_of(sharer->as), base, "with no refund to anybody");
    drop(client);
    drop(sharer);
}

void test_mm0_today_dma_bypasses_the_pool(void) {
    u32 pool0 = capacity_pool_charged();
    struct Burrow *anon = burrow_create_anon(OBJECT_PAGES * P, false);
    TEST_ASSERT(anon != NULL, "burrow_create_anon");
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0 + OBJECT_PAGES, "the control: anonymous backing moves the pool");
    burrow_unref(anon);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "the control returns");

    struct KObj_DMA *k[3] = {
        kobj_dma_create(OBJECT_PAGES * P),
        kobj_dma_create_weave(OBJECT_PAGES * P),
        kobj_dma_create_gpu_bo(OBJECT_PAGES * P),
    };
    for (u32 i = 0; i < 3; i++) {
        TEST_ASSERT(k[i] != NULL, "a DMA object");
        TEST_ASSERT(k[i]->blk[0].pages != NULL, "with backing");
        TEST_EXPECT_EQ(k[i]->blk[0].pages->flags & PG_USER, 0u, "its head page is not a user page");
    }
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "and the pool did not see any of them");
    for (u32 i = 0; i < 3; i++)
        kobj_dma_unref(k[i]);
    TEST_EXPECT_EQ(capacity_pool_charged(), pool0, "nor their end");
}
