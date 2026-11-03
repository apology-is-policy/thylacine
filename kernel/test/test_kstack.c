// The kernel stack range and the stack cache (thread.c, mmu.c).
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
//   thread.kstack_cache_reuses_without_a_remap
//     A free into the cache and the create that takes the stack back touch no
//     page table, so they issue no TLB invalidate at all.
//
//   thread.kstack_cache_keeps_the_guard
//     The guard of a cached stack, and of the stack a create takes back, does
//     not translate; the usable pages do.
//
//   thread.kstack_recycled_stack_is_fully_poisoned
//     A reused stack carries nothing of the thread that last ran on it: every
//     usable word is poison again, so the watermark starts at 0.
//
//   thread.kstack_cache_is_bounded
//     Past THREAD_KSTACK_CACHE_MAX a freed stack is unmapped whole, one change
//     each; the drain releases every cached stack the same way.
//
//   thread.kstack_lives_in_its_range
//     Every page of a stack's span has the two bits vectors.S reads an SP with,
//     as that test needs them: the tag set throughout, the usable bit set in the
//     usable half and clear in the guard.
//
//   thread.kstack_costs_its_four_pages
//     A stack holds four pages and a guard holds none, by the map and by the
//     buddy. Reports the memory a thread now costs.
//
//   thread.kstack_release_invalidates_a_warm_entry
//     A stack whose translation this CPU has just used stops translating as
//     soon as the release returns: the invalidate reached the TLB, not only
//     the PTE.
//
//   thread.kstack_tables_grow_and_stay
//     Enough stacks to need a second L3 table are all mapped and guarded, across
//     the table boundary; freed, they give back every slot and page and keep
//     the tables.
//
//   thread.kstack_refused_step_leaks_nothing
//     Each step of making a stack, refused in turn: the create fails clean, with
//     no slot and no page left behind, and the next one works.
//
//   thread.kstack_tlbi_operand_masks_the_ttl_hint
//     The by-VA invalidate operand for a stack page carries VA[55:12] in bits
//     [43:0] and nothing above, against a value worked out by hand.
//
//   thread.kstack_create_free_cost
//     Reports the cost of a create+free cycle from the cache and from the
//     allocators. Asserts only that the cached cycles changed no mapping.

#include "test.h"

#include "../../arch/arm64/exception.h"
#include "../../arch/arm64/mmu.h"
#include "../../arch/arm64/timer.h"
#include "../../arch/arm64/uart.h"

#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../../mm/magazines.h"
#include "../../mm/phys.h"

static void kstack_never_runs(void) { }

// The guard does not translate and the usable pages do.
static bool kstack_guarded(const void *base) {
    for (u32 i = 0; i < THREAD_KSTACK_TOTAL_SIZE / PAGE_SIZE; i++) {
        bool readable = mmu_kva_readable_for_test((const char *)base + i * PAGE_SIZE);
        if (readable != (i >= THREAD_KSTACK_GUARD_PAGES))
            return false;
    }
    return true;
}

// No page of the span translates: a released stack.
static bool kstack_gone(const void *base) {
    for (u32 i = 0; i < THREAD_KSTACK_TOTAL_SIZE / PAGE_SIZE; i++) {
        if (mmu_kva_readable_for_test((const char *)base + i * PAGE_SIZE))
            return false;
    }
    return true;
}

void test_thread_kstack_cache_reuses_without_a_remap(void) {
    // From an empty cache, so the stack A's free caches is the only one there.
    (void)thread_kstack_cache_drain();
    struct Thread *a = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(a != NULL, "thread_create failed");
    void *base = a->kstack_base;

    u64 c0 = mmu_kstack_changes_for_test();
    thread_free(a);
    u32 held = thread_kstack_cache_count();
    struct Thread *b = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(b != NULL, "thread_create failed");
    u64 changes = mmu_kstack_changes_for_test() - c0;
    void *reused = b->kstack_base;
    thread_free(b);

    TEST_EXPECT_EQ(held, 1u, "the free cached the stack");
    TEST_EXPECT_EQ(changes, 0ull,
        "a free into the cache and a create from it change no mapping");
    TEST_ASSERT(reused == base,
        "the create took back the stack the free cached");
}

void test_thread_kstack_cache_keeps_the_guard(void) {
    // From an empty cache, so A's stack is new and `fresh` checks what a new
    // one has.
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

    TEST_ASSERT(fresh, "a new thread's guard does not translate and its usable pages do");
    TEST_ASSERT(cached, "a stack waiting in the cache keeps its guard");
    TEST_ASSERT(same, "the create took back the stack the free cached");
    TEST_ASSERT(taken, "the stack a create takes from the cache still has its guard");
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

    TEST_ASSERT(same, "the create took back the stack the free cached");
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
    u64 c0 = mmu_kstack_changes_for_test();
    for (u32 i = 0; i < KSB_N; i++)
        thread_free(t[i]);
    u64 freed = mmu_kstack_changes_for_test() - c0;
    u32 held  = thread_kstack_cache_count();
    bool cached_guarded = true, released_gone = true;
    for (u32 i = 0; i < THREAD_KSTACK_CACHE_MAX; i++)
        cached_guarded = cached_guarded && kstack_guarded(base[i]);
    for (u32 i = THREAD_KSTACK_CACHE_MAX; i < KSB_N; i++)
        released_gone = released_gone && kstack_gone(base[i]);

    u64 c1 = mmu_kstack_changes_for_test();
    u32 drained = thread_kstack_cache_drain();
    u64 drain_changes = mmu_kstack_changes_for_test() - c1;
    bool drained_gone = true;
    for (u32 i = 0; i < THREAD_KSTACK_CACHE_MAX; i++)
        drained_gone = drained_gone && kstack_gone(base[i]);

    TEST_EXPECT_EQ(held, (u32)THREAD_KSTACK_CACHE_MAX,
        "the cache holds THREAD_KSTACK_CACHE_MAX stacks and no more");
    TEST_EXPECT_EQ(freed, 4ull,
        "each free past the bound unmaps its stack");
    TEST_ASSERT(cached_guarded, "the cached stacks keep their guard");
    TEST_ASSERT(released_gone, "a stack released past the bound translates nowhere");
    TEST_EXPECT_EQ(drained, (u32)THREAD_KSTACK_CACHE_MAX,
        "the drain releases every cached stack");
    TEST_EXPECT_EQ(drain_changes, (u64)THREAD_KSTACK_CACHE_MAX,
        "the drain unmaps each stack it releases");
    TEST_ASSERT(drained_gone, "a drained stack translates nowhere");
    TEST_EXPECT_EQ(thread_kstack_cache_count(), 0u, "the drain leaves the cache empty");
}

void test_thread_kstack_lives_in_its_range(void) {
    struct Thread *t = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(t != NULL, "thread_create failed");
    u64 base = (u64)(uintptr_t)t->kstack_base;
    u64 top  = t->ctx.sp;
    size_t span = t->kstack_size;

    bool bits = true;
    for (u64 va = base; va < base + THREAD_KSTACK_TOTAL_SIZE; va += 8u) {
        bool usable = va >= base + THREAD_KSTACK_GUARD_SIZE;
        if (!((va >> KSTACK_RANGE_TAG_BIT) & 1u)) bits = false;
        if (((va >> KSTACK_USABLE_BIT) & 1u) != (u64)usable) bits = false;
    }
    thread_free(t);

    TEST_EXPECT_EQ((u64)span, (u64)THREAD_KSTACK_TOTAL_SIZE, "a stack's span");
    TEST_ASSERT(base >= KSTACK_RANGE_BASE &&
                base + THREAD_KSTACK_TOTAL_SIZE <= KSTACK_RANGE_BASE + KSTACK_RANGE_SIZE,
        "the span lies in the kernel stack range");
    TEST_EXPECT_EQ(base & (THREAD_KSTACK_TOTAL_SIZE - 1u), 0ull, "and is aligned to itself");
    TEST_ASSERT(top <= base + THREAD_KSTACK_TOTAL_SIZE && top > base + THREAD_KSTACK_GUARD_SIZE,
        "a new thread's SP starts in the usable half");
    TEST_ASSERT(bits, "every word of the span has the bits vectors.S reads SP with");
}

#define KSP_N 128u

void test_thread_kstack_costs_its_four_pages(void) {
    (void)thread_kstack_cache_drain();
    struct Thread *t[KSP_N];
    u32 slots0  = thread_kstack_slots_in_use();
    u32 pages0  = thread_kstack_pages_in_use();
    u32 tables0 = mmu_kstack_tables_for_test();
    u64 free0   = phys_free_pages();

    u32 made = 0;
    while (made < KSP_N) {
        t[made] = thread_create(kproc(), kstack_never_runs);
        if (!t[made]) break;
        made++;
    }
    u32 slots  = thread_kstack_slots_in_use() - slots0;
    u32 pages  = thread_kstack_pages_in_use() - pages0;
    u32 tables = mmu_kstack_tables_for_test() - tables0;
    u64 free1  = phys_free_pages();
    u64 taken  = free0 > free1 ? free0 - free1 : 0;

    bool backed = true;
    for (u32 i = 0; i < made; i++)
        backed = backed && kstack_guarded(t[i]->kstack_base);

    while (made > 0) thread_free(t[--made]);
    (void)thread_kstack_cache_drain();

    uart_puts("    kstack-memory: ");
    uart_putdec(KSP_N);
    uart_puts(" threads hold ");
    uart_putdec(pages);
    uart_puts(" stack pages, ");
    uart_putdec(tables);
    uart_puts(" new tables, buddy down ");
    uart_putdec(taken);
    uart_puts(" pages\n");

    TEST_EXPECT_EQ(slots, KSP_N, "every thread took a slot");
    TEST_EXPECT_EQ(pages, KSP_N * THREAD_KSTACK_PAGES, "and exactly its stack's pages");
    TEST_ASSERT(backed, "with nothing behind any guard");
    // Stack pages, a page for each slab's worth of Thread slots, a table per 64
    // stacks, and whatever a refill left in this CPU's magazine. Eight pages a
    // stack, what one cost before the range, is far above this.
    TEST_ASSERT(taken <= (u64)KSP_N * THREAD_POOL_PAGES + KSP_N / 64u + 2u + MAGAZINE_SIZE,
        "the buddy gave up no more than five pages a thread, the tables and a magazine");
}

void test_thread_kstack_release_invalidates_a_warm_entry(void) {
    (void)thread_kstack_cache_drain();
    struct Thread *a = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(a != NULL, "thread_create failed");
    const char *base = (const char *)a->kstack_base;
    thread_free(a);
    TEST_EXPECT_EQ(thread_kstack_cache_count(), 1u, "the free cached the stack");

    // IRQs masked from the loads to the last probe: one CPU, and nothing
    // scheduled to evict the entries the loads put in its TLB.
    bool warm = true, closed = true;
    irq_state_t s = spin_lock_irqsave(NULL);
    for (u32 i = THREAD_KSTACK_GUARD_PAGES; i < THREAD_KSTACK_TOTAL_SIZE / PAGE_SIZE; i++) {
        const volatile u64 *w = (const volatile u64 *)(base + i * PAGE_SIZE);
        (void)*w;
        warm = warm && mmu_kva_readable_for_test((const void *)w);
    }
    u32 drained = thread_kstack_cache_drain();
    for (u32 i = THREAD_KSTACK_GUARD_PAGES; i < THREAD_KSTACK_TOTAL_SIZE / PAGE_SIZE; i++)
        closed = closed && !mmu_kva_readable_for_test(base + i * PAGE_SIZE);
    spin_unlock_irqrestore(NULL, s);

    TEST_EXPECT_EQ(drained, 1u, "the drain released the stack");
    TEST_ASSERT(warm, "the usable pages translate before the release");
    TEST_ASSERT(closed, "and a page this CPU has just read stops translating when it returns");
}

// More than two tables' worth, so whatever the range's state the run crosses at
// least one table boundary and needs at least one table it did not have, unless
// an earlier peak left them: the count must then simply not shrink.
#define KST_N 130u

void test_thread_kstack_tables_grow_and_stay(void) {
    (void)thread_kstack_cache_drain();
    struct Thread *t[KST_N];
    void *base[KST_N];
    u32 slots0 = thread_kstack_slots_in_use();
    u32 pages0 = thread_kstack_pages_in_use();

    u32 made = 0;
    while (made < KST_N) {
        t[made] = thread_create(kproc(), kstack_never_runs);
        if (!t[made]) break;
        base[made] = t[made]->kstack_base;
        made++;
    }
    u32 peak = mmu_kstack_tables_for_test();
    bool guarded = true, crossed = false;
    for (u32 i = 0; i < made; i++) {
        guarded = guarded && kstack_guarded(base[i]);
        if (i != 0 && ((u64)(uintptr_t)base[i] >> 21) != ((u64)(uintptr_t)base[0] >> 21))
            crossed = true;
    }
    u32 n = made;
    while (made > 0) thread_free(t[--made]);
    (void)thread_kstack_cache_drain();

    u32 slots = thread_kstack_slots_in_use();
    u32 pages = thread_kstack_pages_in_use();
    bool gone = true;
    void *lowest = n ? base[0] : NULL;
    for (u32 i = 0; i < n; i++) {
        gone = gone && kstack_gone(base[i]);
        if ((uintptr_t)base[i] < (uintptr_t)lowest) lowest = base[i];
    }

    // Lowest first, or the tables would follow the history and not the peak.
    struct Thread *next = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(next != NULL, "thread_create failed");
    bool low = (uintptr_t)next->kstack_base <= (uintptr_t)lowest;
    thread_free(next);
    (void)thread_kstack_cache_drain();

    TEST_EXPECT_EQ(n, KST_N, "every thread was created");
    TEST_ASSERT(crossed, "their stacks span more than one L3 table");
    TEST_ASSERT(peak >= 2u, "so there are at least two tables");
    TEST_ASSERT(guarded, "and every stack, across the boundary, is mapped and guarded");
    TEST_ASSERT(gone, "freed, none of them translates");
    TEST_ASSERT(slots <= slots0, "and every slot came back");
    TEST_ASSERT(pages <= pages0, "and every page");
    TEST_ASSERT(low, "and the next stack takes the lowest slot free");
    TEST_EXPECT_EQ(mmu_kstack_tables_for_test(), peak, "while the tables stay");
}

void test_thread_kstack_refused_step_leaks_nothing(void) {
    const u32 steps = THREAD_KSTACK_PAGES + 2u;
    bool fired = true, refused = true, slots_back = true, pages_back = true;

    for (u32 step = 1; step <= steps; step++) {
        (void)thread_kstack_cache_drain();
        u32 slots0 = thread_kstack_slots_in_use();
        u32 pages0 = thread_kstack_pages_in_use();
        thread_kstack_refuse_step(step);
        struct Thread *t = thread_create(kproc(), kstack_never_runs);
        fired = fired && thread_kstack_refuse_pending() == 0u;
        thread_kstack_refuse_step(0);
        if (t) {
            refused = false;
            thread_free(t);
        }
        (void)thread_kstack_cache_drain();
        slots_back = slots_back && thread_kstack_slots_in_use() <= slots0;
        pages_back = pages_back && thread_kstack_pages_in_use() <= pages0;
    }

    struct Thread *after = thread_create(kproc(), kstack_never_runs);
    bool works = after != NULL;
    if (after) thread_free(after);

    TEST_ASSERT(fired, "the create reached every step");
    TEST_ASSERT(refused, "and each refusal fails the create");
    TEST_ASSERT(slots_back, "no refusal keeps a slot");
    TEST_ASSERT(pages_back, "no refusal keeps a page");
    TEST_ASSERT(works, "and the next create works");
}

void test_thread_kstack_tlbi_operand_masks_the_ttl_hint(void) {
    // The first stack page at VA 0xFFFF_9000_0000_4000. VA[55:12] is 0xFF in
    // [43:36] (VA[55:48], all ones in TTBR1) over 0x9_0000_0004 in [35:0]
    // (VA[47:12]). An unmasked shift would also set bits [51:44], of which
    // [47:44] are the TTL hint.
    TEST_EXPECT_EQ(mmu_kernel_tlbi_operand_for_test(0xFFFF900000004000ull), 0xFF900000004ull,
        "the invalidate operand for a stack page is VA[55:12] and nothing above bit 43");
    // And the last page of the range: VA 0xFFFF_9003_FFFF_F000.
    TEST_EXPECT_EQ(mmu_kernel_tlbi_operand_for_test(0xFFFF9003FFFFF000ull), 0xFF9003FFFFFull,
        "the invalidate operand for the range's last page is VA[55:12] and nothing above bit 43");
}

#define KSC_WARM 256u
#define KSC_COLD 64u

void test_thread_kstack_create_free_cost(void) {
    // One cycle first, so the cache holds a stack for the warm loop.
    struct Thread *w = thread_create(kproc(), kstack_never_runs);
    TEST_ASSERT(w != NULL, "thread_create failed");
    thread_free(w);

    u64 c0 = mmu_kstack_changes_for_test();
    u64 n0 = timer_now_ns();
    for (u32 i = 0; i < KSC_WARM; i++) {
        struct Thread *t = thread_create(kproc(), kstack_never_runs);
        TEST_ASSERT(t != NULL, "thread_create failed");
        thread_free(t);
    }
    u64 warm_ns = timer_now_ns() - n0;
    u64 warm_changes = mmu_kstack_changes_for_test() - c0;

    // From the allocators and back to them: the cache emptied before each
    // create, the freed stack released after each free.
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
    uart_puts("), new ");
    uart_putdec(cold_ns / KSC_COLD);
    uart_puts(" ns/cycle (n=");
    uart_putdec(KSC_COLD);
    uart_puts(")\n");

    TEST_EXPECT_EQ(warm_changes, 0ull, "the cached create+free cycles changed no mapping");
}
