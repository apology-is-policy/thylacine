// Mount-table primitive tests (P5-attach-mount).
//
// Maps to specs/territory.tla actions Mount / Unmount / ForkClone
// (mount-table extension) and the invariant MountRefcountConsistency.
//
// Tests:
//
//   territory_mount.smoke
//     mount one Spoor at a target; verify nmounts + source ref bumped.
//     unmount; verify nmounts back to 0 + source ref dropped.
//
//   territory_mount.idempotent_same_source
//     mount (target, source); mount (target, source) again; verify
//     second call is no-op success (returns 0, no second ref bump).
//
//   territory_mount.mrepl_replaces
//     mount A at target; mount B at target with MREPL; verify A's ref
//     dropped, B's ref taken, nmounts stays at 1.
//
//   territory_mount.unmount_missing_returns_error
//     unmount of a non-existent target_path returns -1.
//
//   territory_mount.table_full
//     mount PGRP_MAX_MOUNTS distinct entries; next mount returns -2.
//
//   territory_mount.clone_bumps_refs
//     mount A at target; territory_clone parent → child; verify both
//     Territories' mount tables hold the entry AND A's ref is bumped
//     to 2 (parent + child). Destroy parent → A's ref = 1 (child).
//     Destroy child → A's ref = 0 (Spoor freed).
//
//   territory_mount.destroy_drops_all_refs
//     mount A and B at distinct targets; territory_unref → both refs
//     dropped.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

extern struct Dev devnone;

void test_territory_mount_smoke(void);
void test_territory_mount_idempotent_same_source(void);
void test_territory_mount_mrepl_replaces(void);
void test_territory_mount_unmount_missing_returns_error(void);
void test_territory_mount_table_full(void);
void test_territory_mount_clone_bumps_refs(void);
void test_territory_mount_destroy_drops_all_refs(void);

void test_territory_mount_smoke(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    TEST_EXPECT_EQ(territory_nmounts(p), 0,
        "fresh Territory must have 0 mount entries");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    TEST_EXPECT_EQ(s->ref, 1, "fresh Spoor ref = 1");

    TEST_EXPECT_EQ(mount(p, s, 1u, 0), 0, "mount should succeed");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "nmounts = 1 after mount");
    TEST_EXPECT_EQ(s->ref, 2, "mount bumps source ref to 2 (test + entry)");

    TEST_EXPECT_EQ(unmount(p, 1u), 0, "unmount should succeed");
    TEST_EXPECT_EQ(territory_nmounts(p), 0, "nmounts = 0 after unmount");
    TEST_EXPECT_EQ(s->ref, 1, "unmount drops source ref back to 1");

    spoor_unref(s);
    territory_unref(p);
}

void test_territory_mount_idempotent_same_source(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(mount(p, s, 5u, 0), 0, "first mount succeeds");
    TEST_EXPECT_EQ(s->ref, 2, "ref = 2 after first mount");

    // Re-mount the same (target, source) pair. Spec: precondition
    // <<path, s>> \notin mounts[p] — impl returns 0 (no-op) without
    // bumping the refcount.
    TEST_EXPECT_EQ(mount(p, s, 5u, 0), 0,
        "idempotent re-mount returns 0");
    TEST_EXPECT_EQ(s->ref, 2,
        "idempotent re-mount must NOT bump ref again");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "idempotent re-mount must NOT add a second entry");

    TEST_EXPECT_EQ(unmount(p, 5u), 0, "unmount cleans up");
    spoor_unref(s);
    territory_unref(p);
}

void test_territory_mount_mrepl_replaces(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    TEST_ASSERT(a != NULL && b != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(mount(p, a, 7u, 0), 0, "mount A at target 7");
    TEST_EXPECT_EQ(a->ref, 2, "A ref = 2");
    TEST_EXPECT_EQ(b->ref, 1, "B ref = 1");

    // mount(p, B, 7, MREPL): replaces A's entry. A's per-entry ref is
    // dropped; B's per-entry ref is taken.
    TEST_EXPECT_EQ(mount(p, b, 7u, MREPL), 0, "MREPL mount succeeds");
    TEST_EXPECT_EQ(a->ref, 1, "MREPL drops A's per-entry ref");
    TEST_EXPECT_EQ(b->ref, 2, "MREPL takes B's per-entry ref");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "nmounts stays at 1 across MREPL");

    TEST_EXPECT_EQ(unmount(p, 7u), 0, "unmount the replacement entry");
    TEST_EXPECT_EQ(b->ref, 1, "B ref back to 1");

    spoor_unref(a);
    spoor_unref(b);
    territory_unref(p);
}

void test_territory_mount_unmount_missing_returns_error(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    TEST_EXPECT_EQ(unmount(p, 42u), -1,
        "unmount of empty mount table returns -1");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    TEST_EXPECT_EQ(mount(p, s, 1u, 0), 0, "mount at target 1");

    // unmount of a different target returns -1; the mounted entry
    // remains.
    TEST_EXPECT_EQ(unmount(p, 99u), -1,
        "unmount of non-existent target returns -1");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "rejected unmount must NOT touch the table");
    TEST_EXPECT_EQ(s->ref, 2, "rejected unmount must NOT drop the ref");

    TEST_EXPECT_EQ(unmount(p, 1u), 0, "unmount cleanup");
    spoor_unref(s);
    territory_unref(p);
}

void test_territory_mount_table_full(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *sources[PGRP_MAX_MOUNTS];
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) {
        sources[i] = spoor_alloc(&devnone);
        TEST_ASSERT(sources[i] != NULL, "spoor_alloc returned NULL");
    }

    // Fill the table.
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) {
        TEST_EXPECT_EQ(mount(p, sources[i], (path_id_t)(100u + i), 0), 0,
            "fill: mount should succeed");
    }
    TEST_EXPECT_EQ(territory_nmounts(p), PGRP_MAX_MOUNTS,
        "mounts[] full");

    // One more should fail.
    struct Spoor *extra = spoor_alloc(&devnone);
    TEST_ASSERT(extra != NULL, "extra spoor_alloc returned NULL");
    TEST_EXPECT_EQ(mount(p, extra, 200u, 0), -2,
        "overflow mount returns -2");
    TEST_EXPECT_EQ(extra->ref, 1,
        "overflow mount must NOT bump source ref");

    // Cleanup: territory_unref drops all entries' refs in one shot.
    spoor_unref(extra);
    territory_unref(p);
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) {
        TEST_EXPECT_EQ(sources[i]->ref, 1,
            "territory_unref must drop each source's entry-ref");
        spoor_unref(sources[i]);
    }
}

void test_territory_mount_clone_bumps_refs(void) {
    struct Territory *parent = territory_alloc();
    TEST_ASSERT(parent != NULL, "parent territory_alloc returned NULL");
    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(mount(parent, s, 1u, 0), 0, "mount in parent");
    TEST_EXPECT_EQ(s->ref, 2, "ref = 2: test + parent-entry");

    // Clone parent into child. Child gets a deep copy of mounts[]; each
    // entry takes a fresh spoor_ref(source). Spec ForkClone bumps
    // refcount[s] by the cardinality of entries-in-parent-pointing-at-s.
    struct Territory *child = territory_clone(parent);
    TEST_ASSERT(child != NULL, "territory_clone returned NULL");
    TEST_EXPECT_EQ(territory_nmounts(child), 1,
        "child inherits parent's mount entries");
    TEST_EXPECT_EQ(s->ref, 3, "clone bumps ref: test + parent + child");

    // Destroy parent → drops parent's entry ref.
    territory_unref(parent);
    TEST_EXPECT_EQ(s->ref, 2,
        "parent destroy drops 1 ref (test + child remain)");

    // Destroy child → drops child's entry ref.
    territory_unref(child);
    TEST_EXPECT_EQ(s->ref, 1,
        "child destroy drops 1 ref (test ref remains)");

    spoor_unref(s);
}

void test_territory_mount_destroy_drops_all_refs(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    TEST_ASSERT(a != NULL && b != NULL, "spoor_alloc returned NULL");

    TEST_EXPECT_EQ(mount(p, a, 1u, 0), 0, "mount A");
    TEST_EXPECT_EQ(mount(p, b, 2u, 0), 0, "mount B");
    TEST_EXPECT_EQ(a->ref, 2, "A ref = 2");
    TEST_EXPECT_EQ(b->ref, 2, "B ref = 2");

    // Final release. The final-release path iterates mounts[] and
    // spoor_unref's each source BEFORE kmem_cache_free. If it skipped
    // this loop (the BUGGY_DESTROY_LEAK spec class), a's ref and b's
    // ref would stay at 2 and the Spoors would leak.
    territory_unref(p);
    TEST_EXPECT_EQ(a->ref, 1, "destroy dropped A's entry-ref");
    TEST_EXPECT_EQ(b->ref, 1, "destroy dropped B's entry-ref");

    spoor_unref(a);
    spoor_unref(b);
}
