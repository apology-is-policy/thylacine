// RW-1 B-F1: rolling-ASID allocator tests (ARCH section 6.2.1; spec
// specs/asid.tla).
//
// The allocator under test is the LIVE one -- asid_resolve drives the same
// per-CPU active/reserved slots + global generation the context-switch pre-hook
// uses. These tests run single-threaded on the boot CPU between user Procs, so
// they do not race a real switch (secondaries are idle; kernel-thread switches
// never enter the allocator -- pgtable_root == 0). They DO advance the global
// generation (the rollover test forces one) and leave per-CPU active slots
// dirty, but that is benign: the real switch path re-publishes its CPU's slot
// on the next switch, and the rolling model tolerates generation churn (a
// stale-generation Proc simply re-news). The `cpu` arg is passed explicitly
// (not smp_cpu_idx_self()), so the tests drive chosen logical-CPU slots
// deterministically regardless of which physical CPU the test thread runs on.
//
//   asid.width_valid        asid_bits() is 8 or 16; the generation is non-zero.
//   asid.resolve_reuse      re-resolving a Proc's context_id returns the same
//                           (valid) ASID -- the fast path.
//   asid.distinct_active    two distinct Procs active on two CPUs hold distinct
//                           ASIDs (no alias).
//   asid.rollover_preserves a Proc active across a forced rollover keeps its
//                           ASID -- the NOSTEAL safety obligation (I-31).

#include "test.h"

#include "../../arch/arm64/asid.h"

#include <thylacine/types.h>

void test_asid_width_valid(void);
void test_asid_resolve_reuse(void);
void test_asid_distinct_active(void);
void test_asid_rollover_preserves(void);

void test_asid_width_valid(void) {
    unsigned bits = asid_bits();
    TEST_ASSERT(bits == 8u || bits == 16u, "asid_bits() must be 8 or 16");
    TEST_ASSERT(asid_generation_now() != 0u,
        "the ASID generation must be non-zero (a context_id of 0 must miss)");
}

void test_asid_resolve_reuse(void) {
    u64 asid_max = (1ull << asid_bits()) - 1u;

    u64 cid = 0;                       // a fresh Proc ("never assigned")
    u64 a1 = asid_resolve(&cid, 0);
    TEST_ASSERT(a1 >= ASID_USER_FIRST && a1 <= asid_max,
        "first resolve gave an out-of-range ASID");
    TEST_ASSERT(cid != 0,
        "resolve must stamp the context_id (no longer 'never assigned')");

    // Re-resolving the now-current context_id reuses the same ASID (fast path).
    u64 a2 = asid_resolve(&cid, 0);
    TEST_EXPECT_EQ(a2, a1,
        "re-resolve of a current-generation Proc changed its ASID");
}

void test_asid_distinct_active(void) {
    u64 asid_max = (1ull << asid_bits()) - 1u;

    u64 cidA = 0, cidB = 0;
    u64 aA = asid_resolve(&cidA, 0);   // Proc A active on CPU 0
    u64 aB = asid_resolve(&cidB, 1);   // Proc B active on CPU 1
    TEST_ASSERT(aA >= ASID_USER_FIRST && aA <= asid_max, "A ASID out of range");
    TEST_ASSERT(aB >= ASID_USER_FIRST && aB <= asid_max, "B ASID out of range");
    TEST_ASSERT(aA != aB,
        "two distinct Procs active on two CPUs share an ASID (alias!)");
}

void test_asid_rollover_preserves(void) {
    u64 asid_max = (1ull << asid_bits()) - 1u;

    // Proc A active on CPU 0.
    u64 cidA = 0;
    u64 aA = asid_resolve(&cidA, 0);

    u64 rolls_before = asid_rollover_count();

    // Fill the generation from CPU 1 with a stream of fresh, transient Procs:
    // each c = 0 claims the next free ASID, and overwriting CPU 1's active slot
    // leaves the prior ASID claimed-but-not-active, so the bitmap fills. After
    // at most asid_max claims a rollover fires.
    u64 cap = asid_max + 4u;
    for (u64 i = 0; i < cap && asid_rollover_count() == rolls_before; i++) {
        u64 c = 0;
        (void)asid_resolve(&c, 1);
    }
    TEST_ASSERT(asid_rollover_count() > rolls_before,
        "filling the ASID space did not force a rollover");

    // NOSTEAL (I-31): Proc A was active on CPU 0 across the rollover, so the
    // rollover reserved its ASID. Re-resolving A returns the SAME ASID value --
    // never one reassigned to another address space.
    u64 aA2 = asid_resolve(&cidA, 0);
    TEST_EXPECT_EQ(aA2, aA,
        "a running Proc's ASID was not preserved across a rollover (NOSTEAL)");
}
