// Namespace primitives tests (P2-Eb).
//
// Three tests:
//
//   namespace.bind_smoke
//     Allocate a fresh Pgrp; bind a few non-cyclic edges; verify
//     pgrp_nbinds tracks the count and unmount removes correctly.
//
//   namespace.cycle_rejected
//     Bind two non-cyclic edges; attempt a third bind that would close
//     a cycle; verify the third bind returns -1 (cycle rejected).
//     Models the spec's BuggyBind being statically prevented at the
//     impl level (bind() always calls would_create_cycle before insert).
//
//   namespace.fork_isolated
//     Allocate parent Pgrp; bind some edges. pgrp_clone child. Bind
//     more on parent. Verify child's bindings reflect only the
//     pre-clone state (independent function values per the spec's
//     ForkClone action).
//
// Maps to specs/namespace.tla state invariant `NoCycle` (cycle_rejected)
// and the structural Isolation property (fork_isolated).

#include "test.h"

#include <thylacine/pgrp.h>
#include <thylacine/types.h>

void test_namespace_bind_smoke(void);
void test_namespace_cycle_rejected(void);
void test_namespace_fork_isolated(void);

void test_namespace_bind_smoke(void) {
    struct Pgrp *p = pgrp_alloc();
    TEST_ASSERT(p != NULL, "pgrp_alloc returned NULL");
    TEST_EXPECT_EQ(pgrp_nbinds(p), 0,
        "fresh Pgrp must have 0 bindings");

    // bind 1: a -> b (walking b yields a). Acyclic.
    TEST_EXPECT_EQ(bind(p, 1u, 2u), 0,
        "first bind should succeed");
    TEST_EXPECT_EQ(pgrp_nbinds(p), 1,
        "nbinds should be 1 after one bind");

    // bind 2: b -> c (walking c yields b). Still acyclic — c can reach
    // b can reach a, but none can reach themselves.
    TEST_EXPECT_EQ(bind(p, 2u, 3u), 0,
        "second bind should succeed");
    TEST_EXPECT_EQ(pgrp_nbinds(p), 2,
        "nbinds should be 2 after two binds");

    // Idempotent re-bind returns -2.
    TEST_EXPECT_EQ(bind(p, 1u, 2u), -2,
        "duplicate bind must return -2 (already bound)");

    // unmount: remove the second edge.
    TEST_EXPECT_EQ(unmount(p, 2u, 3u), 0,
        "unmount of existing edge should succeed");
    TEST_EXPECT_EQ(pgrp_nbinds(p), 1,
        "nbinds back to 1 after unmount");

    // unmount of non-existent edge returns -1.
    TEST_EXPECT_EQ(unmount(p, 99u, 100u), -1,
        "unmount of non-existent edge must return -1");

    pgrp_unref(p);
}

void test_namespace_cycle_rejected(void) {
    struct Pgrp *p = pgrp_alloc();
    TEST_ASSERT(p != NULL, "pgrp_alloc returned NULL");

    // Build chain: walking c yields b; walking b yields a. So from c
    // we can reach a (c -> b -> a in walk direction).
    //
    // bind args (src, dst): walking dst yields src.
    //   bind(b, a) -> walking a yields b. Edge a -> b.
    //   bind(c, b) -> walking b yields c. Edge b -> c.
    // After both: starting at a, walking yields b, then walking b
    // yields c. So a reaches {b, c}.
    TEST_EXPECT_EQ(bind(p, 2u, 1u), 0, "bind 2->1 should succeed");
    TEST_EXPECT_EQ(bind(p, 3u, 2u), 0, "bind 3->2 should succeed");

    // Now attempt bind(1, 3): walking 3 would yield 1. Edge 3 -> 1.
    // From 3 we already reach 1 (chain 3 -> 2 -> 1 yields ... wait,
    // let me re-check the direction).
    //
    // Actually: edge a -> b in our model means walking a yields b
    // (since bind(b, a) means a is the mount point that yields b).
    // Wait that's getting confusing. Let me re-derive.
    //
    // bind(src=2, dst=1): adds the binding "1 has 2 bound at it." So
    // walking 1 yields 2. Edge 1 -> 2 (walking 1 -> reaches 2).
    // bind(src=3, dst=2): walking 2 yields 3. Edge 2 -> 3.
    //
    // From 1, walking yields 2, then walking 2 yields 3. So Reachable(1)
    // = {1, 2, 3}.
    //
    // Now bind(src=1, dst=3): would add "walking 3 yields 1." Edge 3 -> 1.
    // Cycle check: would adding edge 3 -> 1 create a cycle? Cycle iff
    // 3 is reachable from 1 (then 1 -> ... -> 3 -> (new) -> 1 closes).
    // Yes: Reachable(1) = {1, 2, 3} contains 3. So bind must reject.
    TEST_EXPECT_EQ(bind(p, 1u, 3u), -1,
        "bind that would close a cycle must return -1");
    TEST_EXPECT_EQ(pgrp_nbinds(p), 2,
        "rejected bind must NOT modify the bind table");

    // Self-bind is also rejected as a degenerate cycle (length-1 cycle).
    TEST_EXPECT_EQ(bind(p, 5u, 5u), -4,
        "self-bind (src == dst) must return -4");
    TEST_EXPECT_EQ(pgrp_nbinds(p), 2,
        "rejected self-bind must NOT modify the bind table");

    pgrp_unref(p);
}

void test_namespace_fork_isolated(void) {
    struct Pgrp *parent = pgrp_alloc();
    TEST_ASSERT(parent != NULL, "parent pgrp_alloc returned NULL");

    TEST_EXPECT_EQ(bind(parent, 1u, 2u), 0,
        "parent bind 1->2 should succeed");
    TEST_EXPECT_EQ(bind(parent, 3u, 4u), 0,
        "parent bind 3->4 should succeed");
    TEST_EXPECT_EQ(pgrp_nbinds(parent), 2,
        "parent has 2 bindings before clone");

    struct Pgrp *child = pgrp_clone(parent);
    TEST_ASSERT(child != NULL, "pgrp_clone returned NULL");
    TEST_EXPECT_EQ(pgrp_nbinds(child), 2,
        "child should have 2 bindings (cloned from parent)");

    // Add a binding to parent only. Child should be unaffected.
    TEST_EXPECT_EQ(bind(parent, 5u, 6u), 0,
        "parent bind 5->6 (post-clone) should succeed");
    TEST_EXPECT_EQ(pgrp_nbinds(parent), 3,
        "parent has 3 bindings");
    TEST_EXPECT_EQ(pgrp_nbinds(child), 2,
        "child must remain at 2 bindings after parent's post-clone bind");

    // Add a binding to child only. Parent should be unaffected.
    TEST_EXPECT_EQ(bind(child, 7u, 8u), 0,
        "child bind 7->8 should succeed");
    TEST_EXPECT_EQ(pgrp_nbinds(child), 3,
        "child has 3 bindings");
    TEST_EXPECT_EQ(pgrp_nbinds(parent), 3,
        "parent must remain at 3 bindings after child's bind");

    pgrp_unref(parent);
    pgrp_unref(child);
}
