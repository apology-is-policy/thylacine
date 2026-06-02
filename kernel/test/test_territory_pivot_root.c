// Territory pivot_root primitive tests (P6-pouch-stratumd-boot 16c).
//
// Mirrors test_territory_chroot.c -- pivot's atomicity + refcount
// discipline is identical to chroot's, but with one extra pre-condition:
// REQUIRES root_spoor != NULL. The semantic distinction (pivot vs
// initial chroot) is enforced at this layer.
//
// Tests:
//
//   territory.pivot_root_smoke
//     chroot to A (initial root), then pivot_root to B; verify root_spoor
//     == B + A's ref dropped + B's ref bumped.
//
//   territory.pivot_root_rejects_no_initial_root
//     pivot_root on a fresh Territory (root_spoor == NULL) returns -1
//     without touching the source's ref.
//
//   territory.pivot_root_idempotent_same_spoor
//     pivot to the current root returns 0 with no refcount change
//     (matches chroot's same-pointer no-op + Plan 9 / Linux pivot-to-
//     same).
//
//   territory.pivot_root_null_source_rejected
//     pivot_root with NULL source returns -1; no state change.
//
//   territory.pivot_root_does_not_touch_mounts
//     A mount entry installed BEFORE pivot is still present in the
//     mounts[] table AFTER pivot; pivot only touches root_spoor.
//     (v1.0 contract; v1.x bind-survivor semantics layer on top.)

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

extern struct Dev devnone;

void test_territory_pivot_root_smoke(void);
void test_territory_pivot_root_rejects_no_initial_root(void);
void test_territory_pivot_root_idempotent_same_spoor(void);
void test_territory_pivot_root_null_source_rejected(void);
void test_territory_pivot_root_does_not_touch_mounts(void);

// =============================================================================
// pivot_root_smoke
// =============================================================================

void test_territory_pivot_root_smoke(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    TEST_ASSERT(a != NULL && b != NULL, "spoor_alloc returned NULL");

    // Establish initial root via chroot.
    TEST_EXPECT_EQ(territory_chroot(p, a), 0, "initial chroot to A succeeds");
    TEST_EXPECT_EQ(a->ref, 2, "A ref = 2 (test + Territory)");
    TEST_EXPECT_EQ(b->ref, 1, "B ref = 1 (test only)");

    // Pivot to B. Same semantics as chroot's MREPL-shape transition:
    // bump B, drop A.
    TEST_EXPECT_EQ(territory_pivot_root(p, b), 0, "pivot_root to B succeeds");
    TEST_EXPECT_EQ(a->ref, 1, "A ref dropped to 1 after pivot (test only)");
    TEST_EXPECT_EQ(b->ref, 2, "B ref bumped to 2 (test + Territory)");

    territory_unref(p);
    TEST_EXPECT_EQ(b->ref, 1,
        "territory_unref drops B's per-Territory ref");
    TEST_EXPECT_EQ(a->ref, 1, "A ref unchanged at unref (already dropped)");

    spoor_unref(a);
    spoor_unref(b);
}

// =============================================================================
// pivot_root_rejects_no_initial_root
// =============================================================================

void test_territory_pivot_root_rejects_no_initial_root(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    TEST_EXPECT_EQ(s->ref, 1, "fresh Spoor ref = 1");

    // Pre-condition: pivot requires an existing root. A Territory that
    // has never been chrooted MUST reject pivot.
    TEST_EXPECT_EQ(territory_pivot_root(p, s), -1,
        "pivot_root rejects when root_spoor == NULL");
    TEST_EXPECT_EQ(s->ref, 1,
        "rejected pivot must NOT take a ref on source");

    territory_unref(p);
    spoor_unref(s);
}

// =============================================================================
// pivot_root_idempotent_same_spoor
// =============================================================================

void test_territory_pivot_root_idempotent_same_spoor(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(territory_chroot(p, s), 0, "initial chroot to S succeeds");
    TEST_EXPECT_EQ(s->ref, 2, "S ref = 2 (test + Territory)");

    // Pivot to the same Spoor -> idempotent no-op (returns 0, no
    // refcount change).
    TEST_EXPECT_EQ(territory_pivot_root(p, s), 0,
        "pivot_root to same Spoor returns 0");
    TEST_EXPECT_EQ(s->ref, 2,
        "idempotent pivot must NOT change refcount");

    territory_unref(p);
    spoor_unref(s);
}

// =============================================================================
// pivot_root_null_source_rejected
// =============================================================================

void test_territory_pivot_root_null_source_rejected(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    TEST_EXPECT_EQ(territory_chroot(p, s), 0, "initial chroot succeeds");
    TEST_EXPECT_EQ(s->ref, 2, "S ref = 2");

    // NULL source -> -1, no state change.
    TEST_EXPECT_EQ(territory_pivot_root(p, NULL), -1,
        "pivot_root rejects NULL source");
    TEST_EXPECT_EQ(s->ref, 2,
        "rejected NULL pivot leaves existing root + ref intact");

    territory_unref(p);
    spoor_unref(s);
}

// =============================================================================
// pivot_root_does_not_touch_mounts
// =============================================================================

void test_territory_pivot_root_does_not_touch_mounts(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *root_a = spoor_alloc(&devnone);
    struct Spoor *root_b = spoor_alloc(&devnone);
    struct Spoor *mounted = spoor_alloc(&devnone);
    struct Spoor *mp = spoor_alloc(&devnone);   // stalk-2: mount-point identity
    TEST_ASSERT(root_a && root_b && mounted && mp, "spoor_alloc");
    mp->qid.path = 42u;                         // distinct mount-point identity

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");

    // Install a mount entry while root is A.
    TEST_EXPECT_EQ(mount(p, mounted, mp, MREPL), 0,
        "mount installs at mount point (qid.path 42) with MREPL");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "1 mount entry");
    TEST_EXPECT_EQ(mounted->ref, 2,
        "mounted Spoor ref = 2 (test + mount entry)");

    // Pivot from A to B. Mount entry stays put (the v1.0 contract).
    TEST_EXPECT_EQ(territory_pivot_root(p, root_b), 0, "pivot to B");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "pivot does NOT remove mount entries");
    TEST_EXPECT_EQ(mounted->ref, 2,
        "mounted Spoor ref UNCHANGED by pivot (mount entry still holds)");

    territory_unref(p);
    // Territory destruction drops mount + root_b refs.
    TEST_EXPECT_EQ(mounted->ref, 1, "mount-entry ref dropped at destroy");
    TEST_EXPECT_EQ(root_b->ref, 1, "B root ref dropped at destroy");
    TEST_EXPECT_EQ(root_a->ref, 1, "A ref unchanged (was dropped at pivot)");

    spoor_unref(root_a);
    spoor_unref(root_b);
    spoor_unref(mounted);
    spoor_unref(mp);
}
