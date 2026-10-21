// phys.c smoke test — refactored from boot_main's inline version.
//
// Exercises the magazine fast path (orders 0 and 9), the magazine
// refill / drain machinery, AND a non-magazine order (>= 10 bypasses
// magazines and hits buddy direct). Verifies the free-page count
// returns to baseline after `magazines_drain_all`.

#include "test.h"

#include "../../mm/phys.h"
#include "../../mm/magazines.h"
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/smp.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#define SMOKE_N 256

void test_phys_alloc_smoke(void) {
    static struct page *pages[SMOKE_N];   // static so we don't crowd boot stack

    // Drain magazines BEFORE taking the baseline. Phase 1 didn't need
    // this — no SLUB allocations happened pre-test so the magazine was
    // empty. P2-A's proc_init + thread_init each kmem_cache_create +
    // kmem_cache_alloc, which trigger first-time slab_new calls, which
    // refill the order-0 magazine to half-full (8) and consume one or
    // two pages, leaving ~5 pages resident at test entry. Without a
    // pre-drain, the post-drain comparison reads HIGHER than baseline
    // by the resident count and the test reports false drift.
    magazines_drain_all();
    u64 baseline = phys_free_pages();

    // Order-0 (4 KiB) allocations — exercises magazine[0] refill/drain.
    for (int i = 0; i < SMOKE_N; i++) {
        pages[i] = alloc_pages(0, KP_ZERO);
        TEST_ASSERT(pages[i] != NULL, "alloc_pages(0) returned NULL");
    }
    for (int i = 0; i < SMOKE_N; i++) {
        free_pages(pages[i], 0);
    }

    // Order-9 (2 MiB) round-trip — exercises magazine[1].
    struct page *big2 = alloc_pages(9, KP_ZERO);
    TEST_ASSERT(big2 != NULL, "alloc_pages(9) returned NULL");
    free_pages(big2, 9);

    // Order-10 (4 MiB) — bypasses magazines, hits buddy direct.
    struct page *big10 = alloc_pages(10, 0);
    TEST_ASSERT(big10 != NULL, "alloc_pages(10) returned NULL");
    free_pages(big10, 10);

    // Drain magazines so the comparison is exact (otherwise the
    // refill residency holds 7-8 pages per magazine).
    magazines_drain_all();

    u64 after = phys_free_pages();
    TEST_ASSERT(after == baseline,
        "phys_free_pages drift: free count not restored after smoke");
}

// 10 000-iteration alloc/free leak check (ROADMAP §4.2 exit criterion).
// Cycles a single order-0 page through alloc → free 10 000 times,
// drains magazines, verifies the free-page count exactly matches the
// baseline. Catches per-cycle leaks of any size that compound over a
// realistic kernel uptime.
//
// Why a SINGLE pointer (not 10000 simultaneous): the 10K array would
// overflow the boot stack (10000 × 8 = 80 KiB; boot stack is 16 KiB).
// Sequential single-allocation is the right test for "leak per
// alloc/free pair" — a leak would manifest as free-count drift that
// compounds; the magazine refill/drain hysteresis is exercised
// implicitly across iterations.
//
// Test runs in <100 ms on QEMU virt; fits inside tools/test.sh's
// 10-second timeout with margin.
void test_phys_leak_10k(void) {
    // Same robustness rule as alloc_smoke: drain pre-baseline so the
    // post-drain comparison is exact regardless of resident magazine
    // pages at test entry.
    magazines_drain_all();
    u64 baseline = phys_free_pages();

    for (unsigned i = 0; i < 10000; i++) {
        struct page *p = alloc_pages(0, KP_ZERO);
        TEST_ASSERT(p != NULL, "alloc_pages(0) returned NULL mid-10k");
        free_pages(p, 0);
    }

    magazines_drain_all();

    u64 after = phys_free_pages();
    TEST_ASSERT(after == baseline,
        "phys_free_pages drift after 10000-iter alloc/free");
}

// phys.drain_meets_live_owners: magazines_drain_all while three kthreads churn
// their own order-0 sets on the other CPUs. Each owner stamps every page it
// holds and checks the stamps before freeing them: a drain that frees a page
// its owner is popping hands that page to a second owner, whose stamp
// overwrites the first. The suite's drains run with live peers too.
#define DMO_OWNERS 3
#define DMO_HOLD   24       // past MAGAZINE_SIZE: refill and drain-on-full both run
#define DMO_ITERS  4000u
#define DMO_IDLE   0xffffffffu

static volatile bool g_dmo_exited[DMO_OWNERS];
static volatile u32  g_dmo_cpu[DMO_OWNERS];
static volatile u32  g_dmo_running, g_dmo_bad, g_dmo_short;

static u64 dmo_stamp(u64 id, u64 iter, u64 k) { return id << 56 | iter << 8 | k; }
static volatile u64 *dmo_word(struct page *p) { return (volatile u64 *)pa_to_kva(page_to_pa(p)); }

static void dmo_owner(void *arg) {
    u64 id = (u64)(uintptr_t)arg;
    struct page *held[DMO_HOLD];
    for (u64 i = 0; i < DMO_ITERS; i++) {
        __atomic_store_n(&g_dmo_cpu[id], smp_cpu_idx_self(), __ATOMIC_RELAXED);
        unsigned n = 1u + (unsigned)((i * 7u + id) % DMO_HOLD), k = 0;
        for (; k < n; k++) {
            held[k] = alloc_pages(0, 0);
            if (!held[k]) { __atomic_fetch_add(&g_dmo_short, 1u, __ATOMIC_RELAXED); break; }
            *dmo_word(held[k]) = dmo_stamp(id, i, k);
        }
        for (unsigned j = 0; j < k; j++)
            if (*dmo_word(held[j]) != dmo_stamp(id, i, j))
                __atomic_fetch_add(&g_dmo_bad, 1u, __ATOMIC_RELAXED);
        while (k) free_pages(held[--k], 0);
    }
    __atomic_store_n(&g_dmo_cpu[id], DMO_IDLE, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&g_dmo_running, 1u, __ATOMIC_RELEASE);
    test_kthread_park_terminal(&g_dmo_exited[id]);
}

void test_phys_drain_meets_live_owners(void) {
    struct Thread *t[DMO_OWNERS];
    g_dmo_bad = 0;
    g_dmo_short = 0;
    __atomic_store_n(&g_dmo_running, DMO_OWNERS, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < DMO_OWNERS; i++) {
        g_dmo_exited[i] = false;
        g_dmo_cpu[i] = DMO_IDLE;
        t[i] = thread_create_with_arg(kproc(), dmo_owner, (void *)(uintptr_t)i);
        TEST_ASSERT(t[i] != NULL, "thread_create_with_arg(owner) failed");
    }
    for (unsigned i = 0; i < DMO_OWNERS; i++) ready(t[i]);

    // A drain counts as beside an owner when that owner last reported a CPU
    // other than the drainer's: without one, nothing here raced.
    u64 drains = 0, beside = 0;
    for (u64 spin = 0; spin < 100000000ull &&
         __atomic_load_n(&g_dmo_running, __ATOMIC_ACQUIRE) != 0; spin++) {
        u32 me = smp_cpu_idx_self();
        for (unsigned i = 0; i < DMO_OWNERS; i++) {
            u32 c = __atomic_load_n(&g_dmo_cpu[i], __ATOMIC_RELAXED);
            if (c != DMO_IDLE && c != me) beside++;
        }
        magazines_drain_all();
        drains++;
        sched();    // -smp 1: the owners run only when the drainer yields
    }
    TEST_EXPECT_EQ(__atomic_load_n(&g_dmo_running, __ATOMIC_ACQUIRE), 0u,
        "every owner must finish its churn");
    for (unsigned i = 0; i < DMO_OWNERS; i++)
        test_kthread_join_free(t[i], &g_dmo_exited[i]);

    TEST_EXPECT_EQ(g_dmo_bad, 0u, "no page may be handed to two owners");
    TEST_EXPECT_EQ(g_dmo_short, 0u, "every owner allocation must succeed");
    TEST_ASSERT(drains > 0, "the drainer never drained");
    if (smp_cpu_online_count() > 1)
        TEST_ASSERT(beside > 0, "no drain ran beside an owner on another CPU");
}
