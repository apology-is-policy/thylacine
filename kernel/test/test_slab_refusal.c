// I-32: a slab shortfall under a user-reachable allocation refuses, and leaves
// nothing behind. The shortfall is injected, one allocation at a time, so each
// unwind runs with everything around it intact.

#include "test.h"
#include "../../mm/slub.h"

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/vma.h>

#define REFUSAL_VA 0x40000000ull

static void refusal_child(void *arg) {
    (void)arg;
    exits("ok");
}

static void rfork_still_works(void) {
    int pid = rfork(RFPROC, refusal_child, NULL);
    TEST_ASSERT(pid > 0, "rfork failed after the refusal passed");
    int status = -1;
    TEST_EXPECT_EQ(wait_pid(&status), pid, "wait_pid reaps the child");
    TEST_EXPECT_EQ(status, 0, "the child exited ok");
}

static void rfork_refused(const char *cache) {
    u64 created = proc_total_created();
    u64 destroyed = proc_total_destroyed();
    kmem_cache_test_refuse(cache, 0);
    int pid = rfork(RFPROC, refusal_child, NULL);
    TEST_ASSERT(kmem_cache_test_refused(), "rfork never asked this cache");
    TEST_ASSERT(pid < 0, "rfork succeeded with its allocation refused");
    TEST_EXPECT_EQ(proc_total_created() - created, proc_total_destroyed() - destroyed,
                   "a refused rfork leaves no Proc behind");
    rfork_still_works();
}

void test_slab_refusal_proc(void) { rfork_refused("proc"); }
void test_slab_refusal_territory(void) { rfork_refused("territory"); }
void test_slab_refusal_thread(void) { rfork_refused("thread"); }

static bool spans(struct AddrSpace *as, u64 va, u64 start, u64 end) {
    spin_lock(&as->lock);
    struct Vma *v = vma_lookup_in(as, va);
    bool yes = v && v->vaddr_start == start && v->vaddr_end == end;
    spin_unlock(&as->lock);
    return yes;
}

// A map, a cut in a detach, and each cut of a reprotect: refused, and the space
// reads as before.
void test_slab_refusal_vma(void) {
    const u64 len = 3 * PAGE_SIZE, end = REFUSAL_VA + len;
    struct AddrSpace *as = addrspace_alloc(proc_default_page_budget());
    TEST_ASSERT(as != NULL, "addrspace_alloc");
    struct Burrow *b = burrow_create_anon(len, false);
    TEST_ASSERT(b != NULL, "burrow_create_anon");
    u64 allocated = vma_total_allocated();
    u64 freed = vma_total_freed();

    kmem_cache_test_refuse("vma", 0);
    int rc = burrow_map_in(as, true, b, REFUSAL_VA, len, VMA_PROT_RW);
    TEST_ASSERT(kmem_cache_test_refused(), "the map never asked for a Vma");
    TEST_ASSERT(rc != 0, "the map succeeded with its Vma refused");
    TEST_EXPECT_EQ(burrow_mapping_count(b), 0, "a refused map holds no mapping");

    rc = burrow_map_in(as, true, b, REFUSAL_VA, len, VMA_PROT_RW);
    TEST_EXPECT_EQ(rc, 0, "the map works once the refusal passed");

    struct Burrow *dead = NULL;
    kmem_cache_test_refuse("vma", 0);
    spin_lock(&as->lock);
    rc = vma_detach_range_in(as, true, NULL, REFUSAL_VA + PAGE_SIZE, PAGE_SIZE, 0, &dead);
    spin_unlock(&as->lock);
    burrow_free_deferred(dead);
    TEST_ASSERT(kmem_cache_test_refused(), "the detach never asked for a piece");
    TEST_EXPECT_EQ(rc, -(int)T_E_NOMEM, "a refused cut says ENOMEM");
    TEST_ASSERT(spans(as, REFUSAL_VA + PAGE_SIZE, REFUSAL_VA, end), "a refused detach cut the mapping");

    for (u32 skip = 0; skip < 2; skip++) {
        kmem_cache_test_refuse("vma", skip);
        spin_lock(&as->lock);
        rc = vma_reprotect_range_in(as, true, REFUSAL_VA + PAGE_SIZE, PAGE_SIZE, VMA_PROT_READ, false);
        spin_unlock(&as->lock);
        TEST_ASSERT(kmem_cache_test_refused(), "the reprotect never asked for this piece");
        TEST_EXPECT_EQ(rc, -(int)T_E_NOMEM, "a refused reprotect says ENOMEM");
        TEST_ASSERT(spans(as, REFUSAL_VA + PAGE_SIZE, REFUSAL_VA, end), "a refused reprotect cut the mapping");
    }
    TEST_EXPECT_EQ(burrow_mapping_count(b), 1, "the refusals left the one mapping");
    TEST_EXPECT_EQ(vma_total_allocated() - allocated, vma_total_freed() - freed + 1,
                   "the refusals left only the one Vma");

    addrspace_unref(as);
    burrow_unref(b);
}

// A fork that runs short on any mapping of the space takes back what it copied.
void test_slab_refusal_clone(void) {
    struct AddrSpace *as = addrspace_alloc(proc_default_page_budget());
    TEST_ASSERT(as != NULL, "addrspace_alloc");
    struct Burrow *one = burrow_create_anon(PAGE_SIZE, false);
    struct Burrow *two = burrow_create_anon(PAGE_SIZE, false);
    TEST_ASSERT(one && two, "burrow_create_anon");
    TEST_EXPECT_EQ(burrow_map_in(as, true, one, REFUSAL_VA, PAGE_SIZE, VMA_PROT_RW), 0, "first mapping");
    TEST_EXPECT_EQ(burrow_map_in(as, true, two, REFUSAL_VA + 4 * PAGE_SIZE, PAGE_SIZE, VMA_PROT_RW), 0, "second mapping");

    for (u32 skip = 0; skip < 2; skip++) {
        u64 allocated = vma_total_allocated();
        u64 freed = vma_total_freed();
        kmem_cache_test_refuse("vma", skip);
        struct AddrSpace *child = addrspace_clone(as, true);
        TEST_ASSERT(kmem_cache_test_refused(), "the clone never asked for this Vma");
        TEST_ASSERT(child == NULL, "the clone succeeded with a Vma refused");
        TEST_EXPECT_EQ(vma_total_allocated() - allocated, vma_total_freed() - freed,
                       "a refused clone leaves no Vma behind");
        TEST_EXPECT_EQ(burrow_mapping_count(one), 1, "the first mapping stands alone");
        TEST_EXPECT_EQ(burrow_mapping_count(two), 1, "the second mapping stands alone");
    }

    struct AddrSpace *child = addrspace_clone(as, true);
    TEST_ASSERT(child != NULL, "the clone works once the refusal passed");
    addrspace_unref(child);
    addrspace_unref(as);
    burrow_unref(one);
    burrow_unref(two);
}
