// In-kernel test harness — minimal at v1.0.
//
// Lives at kernel/test/. Each test is a void(void) function registered
// in g_tests[]. test_run_all walks the array, runs each, prints
// PASS/FAIL per test. The harness is freestanding-friendly (no
// constructors, no linker sections), trades a tiny bit of manual
// registration for predictable behavior in the kernel environment.
//
// Tests cover **stable leaf APIs only**: pure functions, DTB-parser
// surfaces, allocator smoke flows. Internal-data-structure invariants
// (buddy free-list shape, SLUB partial-list discipline) are tested
// implicitly via the smoke flows; explicit invariant tests would need
// rewriting as those subsystems evolve, so we defer them. Phase 2's
// process / handle / VMO surfaces will get their own tests when the
// APIs stabilize.
//
// Per CLAUDE.md "test what is stable" guidance + the user's directive
// to avoid premature rewriting.
//
// Future evolution (P1-I and beyond):
//   - Host-side test target (compile leaf modules with -fsanitize).
//   - 10000-iteration alloc/free leak check.
//   - TLA+ specs gate-tied per ARCH §25.2.

#ifndef THYLACINE_KERNEL_TEST_H
#define THYLACINE_KERNEL_TEST_H

#include <thylacine/types.h>

struct test_case {
    const char *name;       // human-readable identifier
    void (*fn)(void);       // test body; calls TEST_ASSERT / TEST_FAIL
    bool failed;            // set by the harness if the test fails
    const char *fail_msg;
};

// Sentinel-terminated array of all tests. Defined in kernel/test/test.c.
// New tests are added by extending the array (no constructors needed).
extern struct test_case g_tests[];

// Run every test in g_tests[]. Reports per-test PASS/FAIL on UART.
// Sets each test_case's failed / fail_msg fields for post-run inspection.
void test_run_all(void);

// True iff every test in g_tests[] passed. Use after test_run_all to
// gate the boot path — boot_main calls this and extinctions if false.
bool test_all_passed(void);

// Number of tests that ran + count of passes / failures.
unsigned test_total(void);
unsigned test_passed(void);
unsigned test_failed(void);

// Called from inside a test_case's fn to report a failure. Sets
// the current test's failed flag and stashes the message; caller
// should `return` immediately afterward (the TEST_ASSERT macro does
// this for you).
void test_fail(const char *msg);

// Convenience macros. TEST_ASSERT short-circuits the current test on
// failure (returns from the test_case's fn). TEST_EXPECT_EQ is a
// thin wrapper that stringifies operands for a more useful message.
#define TEST_ASSERT(cond, msg)                                  \
    do {                                                        \
        if (!(cond)) {                                          \
            test_fail(msg);                                     \
            return;                                             \
        }                                                       \
    } while (0)

#define TEST_EXPECT_EQ(a, b, msg) TEST_ASSERT((a) == (b), msg)
#define TEST_EXPECT_NE(a, b, msg) TEST_ASSERT((a) != (b), msg)

#endif // THYLACINE_KERNEL_TEST_H
