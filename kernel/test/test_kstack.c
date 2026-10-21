// The kernel stack cache and the direct-map guard toggles (thread.c, mmu.c).
//
// Every thread here is created and freed without ever being dispatched (the
// test-only pattern thread_free documents), so its stack is touched only by
// the allocator and by the test itself.
//
// mmu_kva_readable_for_test asks AT S1E1R. Hardware may answer that from the
// TLB, and that is what lets the warm-entry test below see a stale entry;
// QEMU's TCG answers AT by walking the tables, so there every probe here
// reads the PTE alone.
//
//   thread.kstack_cache_reuses_without_guard_toggle
//     A free into the cache and the create that takes the block back toggle
//     no guard page, so they issue no TLB invalidate at all.
//
//   thread.kstack_cache_keeps_the_guard
//     The guard pages of a cached block, and of the block a create takes
//     back, still do not translate; the usable pages do.
//
//   thread.kstack_recycled_stack_is_fully_poisoned
//     A reused block carries nothing of the thread that last ran on it: every
//     usable word is poison again, so the watermark starts at 0.
//
//   thread.kstack_cache_is_bounded
//     Past THREAD_KSTACK_CACHE_MAX a freed stack goes back to the buddy with
//     its guard restored; the drain releases every cached block the same way.
//
//   directmap.tlbi_operand_masks_the_ttl_hint
//     The by-VA invalidate operand for a direct-map page carries VA[55:12] in
//     bits [43:0] and nothing above, against a value worked out by hand.
//
//   directmap.guard_toggle_invalidates_a_warm_entry
//     A guard page whose translation this CPU has just used stops translating
//     as soon as the toggle returns, and translates again after the restore:
//     the invalidate reached the TLB, not only the PTE.
//
//   thread.kstack_create_free_cost
//     Reports the cost of a create+free cycle from the cache and from the
//     buddy. Asserts only that the cached cycles toggled no guard.

#include "test.h"

#include "../../arch/arm64/mmu.h"
#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uart.h"

#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../mm/phys.h"

void test_thread_kstack_cache_reuses_without_guard_toggle(void);
void test_thread_kstack_cache_keeps_the_guard(void);
void test_thread_kstack_recycled_stack_is_fully_poisoned(void);
void test_thread_kstack_cache_is_bounded(void);
void test_directmap_tlbi_operand_masks_the_ttl_hint(void);
void test_directmap_guard_toggle_invalidates_a_warm_entry(void);
void test_thread_kstack_create_free_cost(void);

// Test hooks in mmu.c (the *_for_test convention).
u64  mmu_guard_toggles_for_test(void);
bool mmu_kva_readable_for_test(const void *kva);
u64  mmu_directmap_tlbi_operand_for_test(paddr_t pa);

static void kstack_never_runs(void) { }

// The guard pages do not translate and the usable pages do.
static bool kstack_guarded(const void *base) {
    for (u32 i = 0; i < THREAD_KSTACK_TOTAL_SIZE / PAGE_SIZE; i++) {
        bool readable = mmu_kva_readable_for_test((const char *)base + i * PAGE_SIZE);
        if (readable != (i >= THREAD_KSTACK_GUARD_PAGES))
            return false;
    }
    return true;
}

// Every page of the block translates: what the buddy's next user is owed.
static bool kstack_open(const void *base) {
    for (u32 i = 0; i < THREAD_KSTACK_TOTAL_SIZE / PAGE_SIZE; i++) {
        if (!mmu_kva_readable_for_test((const char *)base + i * PAGE_SIZE))
            return false;
    }
    return true;
}

void test_thread_kstack_cache_reuses_without_guard_toggle(void) {
    // From an empty cache, so the block A's free caches is the only one there.
    (void)thread_kstack_cache_drain();
    struct Thread *a = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(a != NULL, "thread_create failed");
    void *base = a->kstack_base;

    u64 t0 = mmu_guard_toggles_for_test();
    thread_free(a);
    u32 held = thread_kstack_cache_count();
    struct Thread *b = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(b != NULL, "thread_create failed");
    u64 toggles = mmu_guard_toggles_for_test() - t0;
    void *reused = b->kstack_base;
    thread_free(b);

    TEST_EXPECT_EQ(held, 1u, "the free cached the stack");
    TEST_EXPECT_EQ(toggles, 0u,
        "a free into the cache and a create from it toggle no guard page");
    TEST_ASSERT(reused == base,
        "the create took back the block the free cached");
}

void test_thread_kstack_cache_keeps_the_guard(void) {
    // From an empty cache, so A's block comes from the buddy and `fresh`
    // checks the guard a miss sets.
    (void)thread_kstack_cache_drain();
    struct Thread *a = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(a != NULL, "thread_create failed");
    void *base = a->kstack_base;
    bool fresh = kstack_guarded(base);

    thread_free(a);
    bool cached = kstack_guarded(base);

    struct Thread *b = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(b != NULL, "thread_create failed");
    bool same  = (b->kstack_base == base);
    bool taken = kstack_guarded(b->kstack_base);
    thread_free(b);

    // The control: the probe tells a guarded block from an open one.
    TEST_ASSERT(fresh, "a new thread's guard pages do not translate and its usable pages do");
    TEST_ASSERT(cached, "a block waiting in the cache keeps its guard no-access");
    TEST_ASSERT(same, "the create took back the block the free cached");
    TEST_ASSERT(taken, "the block a create takes from the cache still has its guard");
}

void test_thread_kstack_recycled_stack_is_fully_poisoned(void) {
    (void)thread_kstack_cache_drain();
    struct Thread *a = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(a != NULL, "thread_create failed");
    void *base = a->kstack_base;

    // What a thread that used its whole stack leaves behind.
    u64 *w = (u64 *)((char *)base + THREAD_KSTACK_GUARD_SIZE);
    for (u32 i = 0; i < THREAD_KSTACK_SIZE / 8u; i++)
        w[i] = 0xD1EDD1EDD1EDD1EDull ^ i;
    thread_free(a);

    struct Thread *b = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(b != NULL, "thread_create failed");
    bool same = (b->kstack_base == base);
    const u64 *r = (const u64 *)((const char *)b->kstack_base + THREAD_KSTACK_GUARD_SIZE);
    u32 stale = 0;
    for (u32 i = 0; i < THREAD_KSTACK_SIZE / 8u; i++)
        if (r[i] != THREAD_KSTACK_POISON)
            stale++;
    u32 used = thread_kstack_used(b, NULL);
    thread_free(b);

    TEST_ASSERT(same, "the create took back the block the free cached");
    TEST_EXPECT_EQ(stale, 0u,
        "no word of the last thread's stack survives into the next thread's");
    TEST_EXPECT_EQ(used, 0u,
        "a reused stack's watermark starts at 0");
}

#define KSB_N (THREAD_KSTACK_CACHE_MAX + 4)

void test_thread_kstack_cache_is_bounded(void) {
    (void)thread_kstack_cache_drain();
    TEST_EXPECT_EQ(thread_kstack_cache_count(), 0u,
        "the drain releases every cached stack");

    struct Thread *t[KSB_N];
    void *base[KSB_N];
    u32 made = 0;
    while (made < KSB_N) {
        t[made] = thread_create(kproc(), kstack_never_runs);
        if (!t[made]) break;
        base[made] = t[made]->kstack_base;
        made++;
    }
    if (made < KSB_N) {
        while (made > 0) thread_free(t[--made]);
        TEST_ASSERT(false, "thread_create failed");
    }

    // The first MAX frees fill the cache; the last four find it full.
    u64 t0 = mmu_guard_toggles_for_test();
    for (u32 i = 0; i < KSB_N; i++)
        thread_free(t[i]);
    u64 freed = mmu_guard_toggles_for_test() - t0;
    u32 held  = thread_kstack_cache_count();
    bool cached_guarded = true, released_open = true;
    for (u32 i = 0; i < THREAD_KSTACK_CACHE_MAX; i++)
        cached_guarded = cached_guarded && kstack_guarded(base[i]);
    for (u32 i = THREAD_KSTACK_CACHE_MAX; i < KSB_N; i++)
        released_open = released_open && kstack_open(base[i]);

    u64 t1 = mmu_guard_toggles_for_test();
    u32 drained = thread_kstack_cache_drain();
    u64 drain_toggles = mmu_guard_toggles_for_test() - t1;
    bool drained_open = true;
    for (u32 i = 0; i < THREAD_KSTACK_CACHE_MAX; i++)
        drained_open = drained_open && kstack_open(base[i]);

    TEST_EXPECT_EQ(held, (u32)THREAD_KSTACK_CACHE_MAX,
        "the cache holds THREAD_KSTACK_CACHE_MAX stacks and no more");
    TEST_EXPECT_EQ(freed, 4u,
        "each free past the bound releases its stack, one guard restore each");
    TEST_ASSERT(cached_guarded, "the cached stacks keep their guard no-access");
    TEST_ASSERT(released_open, "a stack released past the bound has its guard restored");
    TEST_EXPECT_EQ(drained, (u32)THREAD_KSTACK_CACHE_MAX,
        "the drain releases every cached stack");
    TEST_EXPECT_EQ(drain_toggles, (u64)THREAD_KSTACK_CACHE_MAX,
        "the drain restores each released stack's guard");
    TEST_ASSERT(drained_open, "a drained stack has its guard restored");
    TEST_EXPECT_EQ(thread_kstack_cache_count(), 0u, "the drain leaves the cache empty");
}

void test_directmap_tlbi_operand_masks_the_ttl_hint(void) {
    // PA 1 GiB sits at VA 0xFFFF_0000_4000_0000 in the direct map. VA[55:12]
    // is 0xFF in [43:36] (VA[55:48], all ones in TTBR1) over 0x0_0004_0000 in
    // [35:0] (VA[47:12]). An unmasked shift would also set bits [51:44], of
    // which [47:44] are the TTL hint.
    TEST_EXPECT_EQ(mmu_directmap_tlbi_operand_for_test(0x40000000ull), 0xFF000040000ull,
        "the invalidate operand for PA 1 GiB is VA[55:12] and nothing above bit 43");
    // And the top of the 8 GiB direct map: VA 0xFFFF_0002_3FFF_F000.
    TEST_EXPECT_EQ(mmu_directmap_tlbi_operand_for_test(0x23FFFF000ull), 0xFF00023FFFFull,
        "the invalidate operand for the last direct-map page is VA[55:12] and nothing above bit 43");
}

void test_directmap_guard_toggle_invalidates_a_warm_entry(void) {
    struct page *pg = alloc_pages(THREAD_KSTACK_TOTAL_ORDER, 0);
    TEST_ASSERT(pg != NULL, "alloc_pages failed");
    paddr_t pa = page_to_pa(pg);
    const volatile u64 *w = (const volatile u64 *)pa_to_kva(pa);
    const u32 stride = PAGE_SIZE / 8u;

    // IRQs masked from the loads to the last probe: one CPU, and nothing
    // scheduled to evict the entries the loads put in its TLB.
    bool warm = true, closed = true, reopened = true;
    irq_state_t s = spin_lock_irqsave(NULL);
    for (u32 i = 0; i < THREAD_KSTACK_GUARD_PAGES; i++) {
        (void)w[i * stride];
        warm = warm && mmu_kva_readable_for_test((const void *)&w[i * stride]);
    }
    bool set = mmu_set_no_access_range(pa, THREAD_KSTACK_GUARD_PAGES);
    for (u32 i = 0; i < THREAD_KSTACK_GUARD_PAGES; i++)
        closed = closed && !mmu_kva_readable_for_test((const void *)&w[i * stride]);
    bool restored = mmu_restore_normal_range(pa, THREAD_KSTACK_GUARD_PAGES);
    for (u32 i = 0; i < THREAD_KSTACK_GUARD_PAGES; i++)
        reopened = reopened && mmu_kva_readable_for_test((const void *)&w[i * stride]);
    spin_unlock_irqrestore(NULL, s);
    free_pages(pg, THREAD_KSTACK_TOTAL_ORDER);

    TEST_ASSERT(warm, "the block's pages translate before the toggle");
    TEST_ASSERT(set && restored, "both guard toggles succeed");
    TEST_ASSERT(closed, "a guard page this CPU has just read stops translating when the toggle returns");
    TEST_ASSERT(reopened, "and translates again when the restore returns");
}

#define KSC_WARM 256u
#define KSC_COLD 64u

void test_thread_kstack_create_free_cost(void) {
    // One cycle first, so the cache holds a block for the warm loop.
    struct Thread *w = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(w != NULL, "thread_create failed");
    thread_free(w);

    u64 t0 = mmu_guard_toggles_for_test();
    u64 n0 = timer_now_ns();
    for (u32 i = 0; i < KSC_WARM; i++) {
        struct Thread *t = thread_create(kproc(), kstack_never_runs);
        TEST_ASSERT(t != NULL, "thread_create failed");
        thread_free(t);
    }
    u64 warm_ns = timer_now_ns() - n0;
    u64 warm_toggles = mmu_guard_toggles_for_test() - t0;

    // From the buddy and back to it: the cache emptied before each create,
    // the freed block released after each free.
    (void)thread_kstack_cache_drain();
    n0 = timer_now_ns();
    for (u32 i = 0; i < KSC_COLD; i++) {
        struct Thread *t = thread_create(kproc(), kstack_never_runs);
        TEST_ASSERT(t != NULL, "thread_create failed");
        thread_free(t);
        (void)thread_kstack_cache_drain();
    }
    u64 cold_ns = timer_now_ns() - n0;

    uart_puts("    kstack-cost: cached ");
    uart_putdec(warm_ns / KSC_WARM);
    uart_puts(" ns/cycle (n=");
    uart_putdec(KSC_WARM);
    uart_puts("), buddy ");
    uart_putdec(cold_ns / KSC_COLD);
    uart_puts(" ns/cycle (n=");
    uart_putdec(KSC_COLD);
    uart_puts(")\n");

    TEST_EXPECT_EQ(warm_toggles, 0u, "the cached create+free cycles toggled no guard page");
}
