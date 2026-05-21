// Territory chroot primitive tests (P5-stratumd-stub-bringup-e2).
//
// Maps to specs/territory.tla::Chroot + the impl's
// kernel/territory.c::territory_chroot. The relevant invariant is the
// chunk-extended MountRefcountConsistency:
//
//   refcount[s] = |MountEntriesForSpoor(s)| + |{p : root_spoor[p] = s}|
//
// so each Territory that holds `s` as its root_spoor contributes one
// reference (analogous to a mount-table entry).
//
// Tests:
//
//   territory.chroot_smoke
//     alloc Territory + Spoor; territory_chroot; verify root_spoor +
//     ref bumped from 1 to 2 (test + Territory). territory_unref drops
//     back to 1 (test).
//
//   territory.chroot_idempotent_same_spoor
//     chroot twice with the same Spoor; second call is a no-op success
//     (returns 0 without bumping the ref again).
//
//   territory.chroot_replace_clunks_old
//     chroot s1; chroot s2; verify s1 ref dropped, s2 ref bumped, the
//     Territory's root_spoor points at s2. (MREPL-shape transition for
//     the root pointer.)
//
//   territory.chroot_clone_bumps_ref
//     chroot, clone parent → child, verify ref += 1 (parent + child both
//     hold). Destroy parent → ref -= 1; destroy child → ref -= 1.
//
//   territory.chroot_destroy_drops_ref
//     chroot + territory_unref → root ref dropped (the final-release
//     path's spoor_clunk on root_spoor).
//
//   territory.chroot_null_returns_error
//     territory_chroot with NULL source returns -1; no state change.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

extern struct Dev devnone;

void test_territory_chroot_smoke(void);
void test_territory_chroot_idempotent_same_spoor(void);
void test_territory_chroot_replace_clunks_old(void);
void test_territory_chroot_clone_bumps_ref(void);
void test_territory_chroot_destroy_drops_ref(void);
void test_territory_chroot_null_returns_error(void);

void test_territory_chroot_smoke(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    TEST_EXPECT_EQ(s->ref, 1, "fresh Spoor ref = 1");

    TEST_EXPECT_EQ(territory_chroot(p, s), 0, "chroot should succeed");
    TEST_EXPECT_EQ(s->ref, 2, "chroot bumps ref to 2 (test + Territory)");

    territory_unref(p);
    TEST_EXPECT_EQ(s->ref, 1,
        "territory_unref drops root_spoor ref back to 1 (test)");

    spoor_unref(s);
}

void test_territory_chroot_idempotent_same_spoor(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(territory_chroot(p, s), 0, "first chroot succeeds");
    TEST_EXPECT_EQ(s->ref, 2, "ref = 2 after first chroot");

    // Re-chroot same Spoor — spec precondition `root_spoor[p] # s` keeps
    // the action away; impl returns 0 without bumping the ref again.
    TEST_EXPECT_EQ(territory_chroot(p, s), 0,
        "idempotent re-chroot returns 0");
    TEST_EXPECT_EQ(s->ref, 2,
        "idempotent re-chroot must NOT bump ref again");

    territory_unref(p);
    TEST_EXPECT_EQ(s->ref, 1,
        "territory_unref drops the single Territory ref");

    spoor_unref(s);
}

void test_territory_chroot_replace_clunks_old(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    TEST_ASSERT(a != NULL && b != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(territory_chroot(p, a), 0, "chroot to A");
    TEST_EXPECT_EQ(a->ref, 2, "A ref = 2 (test + Territory)");
    TEST_EXPECT_EQ(b->ref, 1, "B ref = 1 (test only)");

    // Re-chroot to B: A's per-Territory ref dropped (via spoor_clunk),
    // B's ref taken. Matches the spec's Chroot action two-key EXCEPT
    // (refcount[a] -= 1, refcount[b] += 1).
    TEST_EXPECT_EQ(territory_chroot(p, b), 0, "chroot to B replaces A");
    TEST_EXPECT_EQ(a->ref, 1, "replace drops A's per-Territory ref");
    TEST_EXPECT_EQ(b->ref, 2, "replace takes B's per-Territory ref");

    territory_unref(p);
    TEST_EXPECT_EQ(b->ref, 1, "destroy drops B's per-Territory ref");

    spoor_unref(a);
    spoor_unref(b);
}

void test_territory_chroot_clone_bumps_ref(void) {
    struct Territory *parent = territory_alloc();
    TEST_ASSERT(parent != NULL, "parent territory_alloc returned NULL");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(territory_chroot(parent, s), 0, "parent chroot");
    TEST_EXPECT_EQ(s->ref, 2, "ref = 2 (test + parent)");

    // Clone parent → child. Child takes its own ref on root_spoor
    // (spec's ForkClone: refcount[s] += 1 iff root_spoor[parent] = s).
    struct Territory *child = territory_clone(parent);
    TEST_ASSERT(child != NULL, "territory_clone returned NULL");
    TEST_EXPECT_EQ(s->ref, 3,
        "clone bumps root_spoor ref (test + parent + child)");

    territory_unref(parent);
    TEST_EXPECT_EQ(s->ref, 2, "parent destroy drops 1 (test + child)");

    territory_unref(child);
    TEST_EXPECT_EQ(s->ref, 1, "child destroy drops 1 (test only)");

    spoor_unref(s);
}

void test_territory_chroot_destroy_drops_ref(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(territory_chroot(p, s), 0, "chroot");
    TEST_EXPECT_EQ(s->ref, 2, "ref = 2 after chroot");

    // Final-release path: territory_unref's drop-each-mount-entry loop
    // is paired with a drop-root_spoor step. If the impl forgot the
    // root_spoor drop (a BUGGY_DESTROY_LEAK-shape bug for root_spoor),
    // s->ref would stay at 2 + s would leak.
    territory_unref(p);
    TEST_EXPECT_EQ(s->ref, 1,
        "territory_unref must drop root_spoor's per-Territory ref");

    spoor_unref(s);
}

void test_territory_chroot_null_returns_error(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    TEST_EXPECT_EQ(territory_chroot(p, NULL), -1,
        "chroot with NULL source returns -1");
    // The Spoor pointer remains NULL — no state mutation on the error
    // path. Verified indirectly: territory_unref would extinct on a
    // corrupted root_spoor; a clean destroy here means root_spoor is
    // either NULL or a valid Spoor.
    territory_unref(p);
}
