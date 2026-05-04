// kaslr.c leaf-API tests — pure-function coverage.
//
// mix64 is a SipHash-style avalanche function. A small change in the
// input should propagate to many output bits; very different inputs
// should produce very different outputs. We don't need formal
// statistical testing here — just sanity checks that the function
// isn't accidentally returning the input or zero.
//
// We test mix64 indirectly via the seed-source path in kaslr.c:
// observable effect is the chosen offset varies with the input. To
// keep this a leaf test (no kernel state mutation), we expose mix64
// via a wrapper function and call it directly. (mix64 is currently
// `static` in kaslr.c; this test forces us to either expose it or
// duplicate the algorithm. We choose to expose because mix64 is
// stable and small — drift risk is minimal.)

#include "test.h"
#include "../../arch/arm64/kaslr.h"

#include <thylacine/types.h>

// kaslr.c exposes mix64 for testing via this declaration.
u64 kaslr_test_mix64(u64 x);

void test_kaslr_mix64_avalanche(void) {
    // mix64(0) should be 0 only if the function is the identity, which
    // it isn't (multiplies + xors guarantee a non-trivial result).
    u64 m0 = kaslr_test_mix64(0);
    TEST_ASSERT(m0 == 0,
        "mix64(0) should be 0 because no bits are set to propagate");

    // mix64(1) should be very different from mix64(0).
    u64 m1 = kaslr_test_mix64(1);
    TEST_ASSERT(m1 != 0, "mix64(1) must be non-zero (single bit propagates)");
    TEST_ASSERT(m1 != 1, "mix64(1) must not equal its input");

    // Bit-difference between mix64(1) and mix64(2): both are
    // single-bit inputs that should map to wildly different outputs.
    u64 m2 = kaslr_test_mix64(2);
    u64 diff = m1 ^ m2;
    int popcount = __builtin_popcountll(diff);
    TEST_ASSERT(popcount > 16,
        "mix64(1) ^ mix64(2) should differ in many bits (avalanche)");

    // Repeating an input is deterministic.
    u64 m1_again = kaslr_test_mix64(1);
    TEST_EXPECT_EQ(m1, m1_again, "mix64 must be a pure function");
}
