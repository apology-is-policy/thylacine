// Mount-table primitive tests (P5-attach-mount; stalk-2 re-key).
//
// Maps to specs/territory.tla actions Mount / Unmount / ForkClone
// (mount-table extension) and the invariant MountRefcountConsistency.
//
// stalk-2: the mount table is keyed by the mount point's (dc, devno, qid.path)
// Spoor identity (was an abstract path_id_t target). Each test mints a
// mount-point Spoor via mkmp() with a distinct qid.path (a devnone Spoor; dc
// '-', devno 0 -- the distinguishing axis here is qid.path). mount() copies the
// identity, not the Spoor, so a single mp can drive both the mount and its
// matching unmount; the test clunks every mp at the end. The SOURCE ref
// assertions are unchanged (mount bumps the SOURCE, never the mount point).
//
// Tests:
//
//   territory_mount.smoke
//     mount one Spoor at a mount point; verify nmounts + source ref bumped.
//     unmount; verify nmounts back to 0 + source ref dropped.
//
//   territory_mount.idempotent_same_source
//     mount (mp, source); mount (mp, source) again; verify second call is
//     no-op success (returns 0, no second ref bump).
//
//   territory_mount.mrepl_replaces
//     mount A at mp; mount B at mp with MREPL; verify A's ref dropped,
//     B's ref taken, nmounts stays at 1.
//
//   territory_mount.unmount_missing_returns_error
//     unmount of a non-existent mount-point identity returns -1.
//
//   territory_mount.table_full
//     mount PGRP_MAX_MOUNTS distinct entries; next mount returns -2.
//
//   territory_mount.clone_bumps_refs
//     mount A at mp; territory_clone parent → child; verify both
//     Territories' mount tables hold the entry AND A's ref is bumped
//     to 2 (parent + child). Destroy parent → A's ref = 1 (child).
//     Destroy child → A's ref = 0 (Spoor freed).
//
//   territory_mount.destroy_drops_all_refs
//     mount A and B at distinct mount points; territory_unref → both refs
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
void test_territory_mount_devno_disambiguates(void);
void test_territory_mount_rejects_cycle(void);

// Mint a mount-point Spoor with a distinct identity (devnone dc '-', devno 0,
// the given qid.path). The mount table keys on (dc, devno, qid.path), so a
// distinct qid.path = a distinct mount point.
static struct Spoor *mkmp(u64 qid_path) {
    struct Spoor *mp = spoor_alloc(&devnone);
    if (mp) mp->qid.path = qid_path;
    return mp;
}

void test_territory_mount_smoke(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    TEST_EXPECT_EQ(territory_nmounts(p), 0,
        "fresh Territory must have 0 mount entries");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    TEST_EXPECT_EQ(s->ref, 1, "fresh Spoor ref = 1");
    struct Spoor *mp = mkmp(1u);
    TEST_ASSERT(mp != NULL, "mkmp returned NULL");

    TEST_EXPECT_EQ(mount(p, s, mp, 0), 0, "mount should succeed");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "nmounts = 1 after mount");
    TEST_EXPECT_EQ(s->ref, 2, "mount bumps source ref to 2 (test + entry)");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount should succeed");
    TEST_EXPECT_EQ(territory_nmounts(p), 0, "nmounts = 0 after unmount");
    TEST_EXPECT_EQ(s->ref, 1, "unmount drops source ref back to 1");

    spoor_unref(mp);
    spoor_unref(s);
    territory_unref(p);
}

void test_territory_mount_idempotent_same_source(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    struct Spoor *mp = mkmp(5u);
    TEST_ASSERT(mp != NULL, "mkmp returned NULL");

    TEST_EXPECT_EQ(mount(p, s, mp, 0), 0, "first mount succeeds");
    TEST_EXPECT_EQ(s->ref, 2, "ref = 2 after first mount");

    // Re-mount the same (mountpoint-identity, source) pair. Spec: precondition
    // <<path, s>> \notin mounts[p] — impl returns 0 (no-op) without bumping the
    // refcount. A SECOND mp Spoor with the SAME qid.path keys identically.
    struct Spoor *mp_same = mkmp(5u);
    TEST_ASSERT(mp_same != NULL, "mkmp returned NULL");
    TEST_EXPECT_EQ(mount(p, s, mp_same, 0), 0,
        "idempotent re-mount returns 0");
    TEST_EXPECT_EQ(s->ref, 2,
        "idempotent re-mount must NOT bump ref again");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "idempotent re-mount must NOT add a second entry");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount cleans up");
    spoor_unref(mp_same);
    spoor_unref(mp);
    spoor_unref(s);
    territory_unref(p);
}

void test_territory_mount_mrepl_replaces(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    TEST_ASSERT(a != NULL && b != NULL, "spoor_alloc returned NULL");
    struct Spoor *mp = mkmp(7u);
    TEST_ASSERT(mp != NULL, "mkmp returned NULL");

    TEST_EXPECT_EQ(mount(p, a, mp, 0), 0, "mount A at mount point 7");
    TEST_EXPECT_EQ(a->ref, 2, "A ref = 2");
    TEST_EXPECT_EQ(b->ref, 1, "B ref = 1");

    // mount(p, B, mp, MREPL): replaces A's entry (same mount-point identity).
    // A's per-entry ref is dropped; B's per-entry ref is taken.
    TEST_EXPECT_EQ(mount(p, b, mp, MREPL), 0, "MREPL mount succeeds");
    TEST_EXPECT_EQ(a->ref, 1, "MREPL drops A's per-entry ref");
    TEST_EXPECT_EQ(b->ref, 2, "MREPL takes B's per-entry ref");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "nmounts stays at 1 across MREPL");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount the replacement entry");
    TEST_EXPECT_EQ(b->ref, 1, "B ref back to 1");

    spoor_unref(mp);
    spoor_unref(a);
    spoor_unref(b);
    territory_unref(p);
}

void test_territory_mount_unmount_missing_returns_error(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *mp1 = mkmp(1u);
    struct Spoor *mp42 = mkmp(42u);
    struct Spoor *mp99 = mkmp(99u);
    TEST_ASSERT(mp1 && mp42 && mp99, "mkmp returned NULL");

    TEST_EXPECT_EQ(unmount(p, mp42), -1,
        "unmount of empty mount table returns -1");

    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    TEST_EXPECT_EQ(mount(p, s, mp1, 0), 0, "mount at mount point 1");

    // unmount of a different mount-point identity returns -1; the mounted
    // entry remains.
    TEST_EXPECT_EQ(unmount(p, mp99), -1,
        "unmount of non-existent mount point returns -1");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "rejected unmount must NOT touch the table");
    TEST_EXPECT_EQ(s->ref, 2, "rejected unmount must NOT drop the ref");

    TEST_EXPECT_EQ(unmount(p, mp1), 0, "unmount cleanup");
    spoor_unref(mp1);
    spoor_unref(mp42);
    spoor_unref(mp99);
    spoor_unref(s);
    territory_unref(p);
}

void test_territory_mount_table_full(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *sources[PGRP_MAX_MOUNTS];
    struct Spoor *mps[PGRP_MAX_MOUNTS];
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) {
        sources[i] = spoor_alloc(&devnone);
        mps[i]     = mkmp((u64)(100u + i));
        TEST_ASSERT(sources[i] != NULL && mps[i] != NULL,
            "spoor_alloc/mkmp returned NULL");
    }

    // Fill the table (each at a distinct mount-point identity).
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) {
        TEST_EXPECT_EQ(mount(p, sources[i], mps[i], 0), 0,
            "fill: mount should succeed");
    }
    TEST_EXPECT_EQ(territory_nmounts(p), PGRP_MAX_MOUNTS,
        "mounts[] full");

    // One more should fail.
    struct Spoor *extra = spoor_alloc(&devnone);
    struct Spoor *mp_extra = mkmp(200u);
    TEST_ASSERT(extra != NULL && mp_extra != NULL, "extra alloc returned NULL");
    TEST_EXPECT_EQ(mount(p, extra, mp_extra, 0), -2,
        "overflow mount returns -2");
    TEST_EXPECT_EQ(extra->ref, 1,
        "overflow mount must NOT bump source ref");

    // Cleanup: territory_unref drops all entries' refs in one shot.
    spoor_unref(mp_extra);
    spoor_unref(extra);
    territory_unref(p);
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) {
        TEST_EXPECT_EQ(sources[i]->ref, 1,
            "territory_unref must drop each source's entry-ref");
        spoor_unref(sources[i]);
        spoor_unref(mps[i]);
    }
}

void test_territory_mount_clone_bumps_refs(void) {
    struct Territory *parent = territory_alloc();
    TEST_ASSERT(parent != NULL, "parent territory_alloc returned NULL");
    struct Spoor *s = spoor_alloc(&devnone);
    TEST_ASSERT(s != NULL, "spoor_alloc returned NULL");
    struct Spoor *mp = mkmp(1u);
    TEST_ASSERT(mp != NULL, "mkmp returned NULL");

    TEST_EXPECT_EQ(mount(parent, s, mp, 0), 0, "mount in parent");
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

    spoor_unref(mp);
    spoor_unref(s);
}

void test_territory_mount_destroy_drops_all_refs(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    TEST_ASSERT(a != NULL && b != NULL, "spoor_alloc returned NULL");
    struct Spoor *mpa = mkmp(1u);
    struct Spoor *mpb = mkmp(2u);
    TEST_ASSERT(mpa != NULL && mpb != NULL, "mkmp returned NULL");

    TEST_EXPECT_EQ(mount(p, a, mpa, 0), 0, "mount A");
    TEST_EXPECT_EQ(mount(p, b, mpb, 0), 0, "mount B");
    TEST_EXPECT_EQ(a->ref, 2, "A ref = 2");
    TEST_EXPECT_EQ(b->ref, 2, "B ref = 2");

    // Final release. The final-release path iterates mounts[] and
    // spoor_unref's each source BEFORE kmem_cache_free. If it skipped
    // this loop (the BUGGY_DESTROY_LEAK spec class), a's ref and b's
    // ref would stay at 2 and the Spoors would leak.
    territory_unref(p);
    TEST_EXPECT_EQ(a->ref, 1, "destroy dropped A's entry-ref");
    TEST_EXPECT_EQ(b->ref, 1, "destroy dropped B's entry-ref");

    spoor_unref(mpa);
    spoor_unref(mpb);
    spoor_unref(a);
    spoor_unref(b);
}

// stalk-2: the (dc, devno, qid.path) key MUST distinguish two mount points with
// the same (dc, qid.path) but different devno -- the dev9p two-session case
// (every 9P session shares dc='9' and every attach root has qid.path 0, so
// without devno their mount points would collide). Also exercises mount_lookup.
void test_territory_mount_devno_disambiguates(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    TEST_ASSERT(a != NULL && b != NULL, "spoor_alloc");

    // Two mount points: SAME dc ('-' from devnone) + SAME qid.path (0), but
    // DISTINCT devno. Without the devno axis these would collide as one key.
    struct Spoor *mp1 = spoor_alloc(&devnone);
    struct Spoor *mp2 = spoor_alloc(&devnone);
    struct Spoor *mp3 = spoor_alloc(&devnone);
    TEST_ASSERT(mp1 && mp2 && mp3, "mp spoor_alloc");
    mp1->qid.path = 0; mp1->devno = 1;
    mp2->qid.path = 0; mp2->devno = 2;
    mp3->qid.path = 0; mp3->devno = 3;   // never mounted

    TEST_EXPECT_EQ(mount(p, a, mp1, 0), 0, "mount a at (-,1,0)");
    TEST_EXPECT_EQ(mount(p, b, mp2, 0), 0, "mount b at (-,2,0)");
    TEST_EXPECT_EQ(territory_nmounts(p), 2,
        "distinct devno -> TWO entries (NOT a collision/idempotent no-op)");

    // mount_lookup resolves each mount point to its OWN source by devno. RW-4
    // SA-F1: mount_lookup now returns a REF-HELD source -- clunk each result.
    struct Spoor *r1 = mount_lookup(p, mp1);
    TEST_ASSERT(r1 == a, "lookup (-,1,0) -> a");
    if (r1) spoor_clunk(r1);
    struct Spoor *r2 = mount_lookup(p, mp2);
    TEST_ASSERT(r2 == b, "lookup (-,2,0) -> b");
    if (r2) spoor_clunk(r2);
    TEST_ASSERT(mount_lookup(p, mp3) == NULL, "lookup unmounted devno -> NULL");

    // unmount by mp1 removes ONLY a's entry; b's (same dc+qid, other devno) stays.
    TEST_EXPECT_EQ(unmount(p, mp1), 0, "unmount (-,1,0)");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "one entry left after unmount");
    TEST_ASSERT(mount_lookup(p, mp1) == NULL, "(-,1,0) gone");
    struct Spoor *r3 = mount_lookup(p, mp2);
    TEST_ASSERT(r3 == b, "(-,2,0) still -> b");
    if (r3) spoor_clunk(r3);

    territory_unref(p);
    spoor_unref(a);
    spoor_unref(b);
    spoor_unref(mp1);
    spoor_unref(mp2);
    spoor_unref(mp3);
}

// RW-4 SA-F1: mount_lookup transfers a ref, so a looked-up source survives a
// concurrent unmount that drops the table's ref. PRE-FIX mount_lookup returned a
// BORROW: the unmount below would free the source (the table was its last holder)
// and the held pointer would dangle -> the subsequent spoor_clunk(got) would
// extinct on the SPOOR_MAGIC check (use-after-free). Deterministic stand-in for
// the SMP pivot/unmount-vs-walk race the ci-smp-gate witnesses.
void test_territory_mount_lookup_ref_survives_unmount(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc");
    struct Spoor *src = spoor_alloc(&devnone);
    struct Spoor *mp  = spoor_alloc(&devnone);
    TEST_ASSERT(src && mp, "spoor_alloc");
    mp->qid.path = 7; mp->devno = 9;

    u64 freed_before = spoor_total_freed();
    TEST_EXPECT_EQ(mount(p, src, mp, 0), 0, "mount");   // src: table holds a ref
    spoor_unref(src);                                    // drop the test's ref -> table is the ONLY holder

    struct Spoor *got = mount_lookup(p, mp);             // SA-F1: returns a REF-HELD source
    TEST_ASSERT(got == src, "mount_lookup -> src");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount drops the table ref");
    TEST_EXPECT_EQ(spoor_total_freed(), freed_before,
        "the lookup ref kept src alive across unmount (pre-fix: freed here -> dangling)");

    spoor_clunk(got);                                    // now the last holder -> frees
    TEST_EXPECT_EQ(spoor_total_freed(), freed_before + 1, "clunk freed src");

    territory_unref(p);
    spoor_unref(mp);
}

// RW-4 SA-F1: territory_root_ref takes a ref atomically, so a FROM_ROOT reader
// holding the result survives a concurrent pivot/chroot that swaps root_spoor +
// clunks the displaced root. Pins the helper the syscall FROM_ROOT paths rely on.
void test_territory_root_ref_survives_pivot(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc");
    struct Spoor *r  = spoor_alloc(&devnone);
    struct Spoor *r2 = spoor_alloc(&devnone);
    TEST_ASSERT(r && r2, "spoor_alloc");

    TEST_EXPECT_EQ(territory_chroot(p, r), 0, "chroot to r");   // r: territory holds a ref
    spoor_unref(r);                                             // territory is the ONLY holder

    u64 freed_before = spoor_total_freed();
    struct Spoor *got = territory_root_ref(p);                  // SA-F1: REF-HELD root
    TEST_ASSERT(got == r, "territory_root_ref -> r");

    TEST_EXPECT_EQ(territory_chroot(p, r2), 0, "pivot away to r2");  // drops the territory's ref on r
    TEST_EXPECT_EQ(spoor_total_freed(), freed_before,
        "the root_ref kept r alive across the pivot (pre-fix borrow: freed -> dangling)");

    spoor_clunk(got);                                          // last holder of r -> frees
    TEST_EXPECT_EQ(spoor_total_freed(), freed_before + 1, "clunk freed r");

    territory_unref(p);                                        // drops r2
    spoor_unref(r2);
}

// stalk-2 audit F1: mount() must reject a mount that would create a cycle in the
// mount-identity graph (I-3). Two reachable triggers: a self-mount (source
// identity == mount-point identity) and a cross-tree oscillation.
void test_territory_mount_rejects_cycle(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    // (1) Self-mount: source and mount point share an identity (-,0,7) -> -3.
    struct Spoor *self_src = spoor_alloc(&devnone);
    struct Spoor *self_mp  = spoor_alloc(&devnone);
    TEST_ASSERT(self_src && self_mp, "self spoor_alloc");
    self_src->qid.path = 7; self_mp->qid.path = 7;   // SAME identity
    TEST_EXPECT_EQ(mount(p, self_src, self_mp, 0), -3,
        "self-mount (source identity == mount-point identity) rejected (I-3)");
    TEST_EXPECT_EQ(territory_nmounts(p), 0, "no entry installed for self-mount");
    TEST_EXPECT_EQ(self_src->ref, 1, "self-mount reject must NOT bump source ref");

    // (2) Cross-tree oscillation. Identities: A=10, B=20.
    //   mount(srcA[10] onto mpB[20]) -> edge 20->10 (installed; not yet a cycle).
    //   mount(srcB[20] onto mpA[10]) -> edge 10->20 closes 20->10->20 -> -3.
    struct Spoor *srcA = spoor_alloc(&devnone);   // identity 10 (the A tree root)
    struct Spoor *mpB  = spoor_alloc(&devnone);   // identity 20 (a dir in B)
    struct Spoor *srcB = spoor_alloc(&devnone);   // identity 20 (the B tree root)
    struct Spoor *mpA  = spoor_alloc(&devnone);   // identity 10 (a dir in A)
    TEST_ASSERT(srcA && mpB && srcB && mpA, "osc spoor_alloc");
    srcA->qid.path = 10; mpA->qid.path = 10;
    srcB->qid.path = 20; mpB->qid.path = 20;

    TEST_EXPECT_EQ(mount(p, srcA, mpB, 0), 0, "A onto B installs (edge 20->10)");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "one entry after first oscillation mount");
    TEST_EXPECT_EQ(mount(p, srcB, mpA, 0), -3,
        "B onto A would close the 20->10->20 cycle -> rejected (I-3)");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "cycle reject left the table at 1");
    TEST_EXPECT_EQ(srcB->ref, 1, "cycle reject must NOT bump srcB ref");

    territory_unref(p);
    spoor_unref(self_src);
    spoor_unref(self_mp);
    spoor_unref(srcA);
    spoor_unref(mpB);
    spoor_unref(srcB);
    spoor_unref(mpA);
}
