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
// matching unmount; the test clunks every mp at the end. mount bumps the
// SOURCE, and the mount point only when an MBEFORE / MAFTER mount starts a
// union there: the covered directory's entry (MCOVERED) holds the point itself.
//
// Tests:
//
//   territory_mount.smoke
//     mount one Spoor at a mount point; verify nmounts + source ref bumped.
//     unmount; verify nmounts back to 0 + source ref dropped.
//
//   territory_mount.idempotent_same_source
//     mount (mp, source); mount (mp, source) again with the SAME flags; verify
//     the second call adds no entry and takes no second ref. (Differing flags
//     converge -- that is idempotent_converges_flags below.)
//
//   territory_mount.idempotent_converges_flags                          (#219)
//     mount (mp, source) with flags 0; re-mount the same pair with MNOEXEC;
//     verify the restriction is now APPLIED (the fail-open regression), then
//     re-mount without it and verify it is dropped (the deliberate symmetry).
//     Entry count + source ref must be untouched by both converges.
//
//   territory_mount.mrepl_replaces
//     mount A at mp; mount B at mp with MREPL; verify A's ref dropped,
//     B's ref taken, nmounts stays at 1.
//
//   territory_mount.unmount_missing_returns_error
//     unmount of a non-existent mount-point identity returns -1.
//
//   territory_mount.table_full
//     mount PGRP_MOUNT_CAP distinct entries; next mount returns -2.
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
#include <thylacine/path.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

#include "../../mm/slub.h"       // kcalloc: arrays as long as the cap

extern struct Dev devnone;

void test_territory_mount_smoke(void);
void test_territory_mount_idempotent_same_source(void);
void test_territory_mount_idempotent_converges_flags(void);  // #219
void test_territory_mount_mrepl_replaces(void);
void test_territory_mount_unmount_missing_returns_error(void);
void test_territory_mount_table_full(void);
void test_territory_mount_clone_bumps_refs(void);
void test_territory_mount_destroy_drops_all_refs(void);
void test_territory_mount_devno_disambiguates(void);
void test_territory_mount_devno_full_width(void);
void test_territory_mount_devno_minter_crosses_2_32(void);
void test_territory_mount_noexec_covers(void);       // #217
void test_territory_mount_rejects_cycle(void);
void test_territory_mount_mp_path_lifecycle(void);   // #66
void test_territory_mount_format_ns(void);           // #66
void test_territory_mount_union_keeps_covered(void);           // Plan 9 unions
void test_territory_mount_no_covered_unless_fresh(void);       // Plan 9 unions
void test_territory_mount_covered_file_point_stays_plain(void); // Plan 9 unions
void test_territory_mount_covered_reposition(void);            // Plan 9 unions
void test_territory_mount_covered_leaves_with_last(void);      // Plan 9 unions
void test_territory_mount_covered_needs_two_slots(void);       // Plan 9 unions
void test_territory_mount_covered_self_mount_refused(void);    // Plan 9 unions
void test_territory_mount_covered_clone(void);                 // Plan 9 unions
void test_territory_mount_covered_noexec_scoped(void);         // Plan 9 unions
void test_territory_mount_covered_format_ns(void);             // Plan 9 unions
void test_territory_mount_nodes_follow_every_edit(void);        // the dynamic table
void test_territory_mount_refuses_enomem(void);                // the dynamic table

// Mint a mount-point Spoor with a distinct identity (devnone dc '-', devno 0,
// the given qid.path). The mount table keys on (dc, devno, qid.path), so a
// distinct qid.path = a distinct mount point. A directory, as mount points are:
// only a directory can start a union (covered_file_point_stays_plain mints the
// exception).
static struct Spoor *mkmp(u64 qid_path) {
    struct Spoor *mp = spoor_alloc(&devnone);
    if (mp) { mp->qid.path = qid_path; mp->qid.type = QTDIR; }
    return mp;
}

// #66: build a Path "/<name>" (ref == 1, caller-owned). For the mount-point
// name tests -- mkmp gives an unnamed (NULL-path) Spoor; this stamps a name.
static struct Path *mkname(const char *name) {
    struct Path *root = path_make_root();
    if (!root) return NULL;
    u64 n = 0; while (name[n]) n++;
    struct Path *p = path_addelem(root, name, n);
    path_unref(root);
    return p;   // "/name", ref 1
}

static bool mnt_streq(const char *a, const char *b) {
    u64 i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
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

// #219. The idempotent arm used to return 0 without ever looking at `flags`,
// so a re-mount that ADDED MNOEXEC reported success and left the pair
// unrestricted -- fail-OPEN, which is the dangerous direction for a flag that
// carries an enforcement decision. Both directions are asserted: the tighten is
// the regression, the loosen pins the deliberate symmetry (a mount flag is not
// a lock; see the noexec_covers unmount leg).
//
// Every observation is MEASURED first and ASSERTED after the teardown, because
// TEST_ASSERT returns -- asserting inline would skip the unrefs below and leak
// a Territory + four Spoors on exactly the run where the test is telling you
// something.
void test_territory_mount_idempotent_converges_flags(void) {
    struct Territory *p     = territory_alloc();
    struct Spoor     *s     = p ? spoor_alloc(&devnone) : NULL;
    // Three DISTINCT Spoor objects sharing one mount-point identity: the arm
    // keys on (dc, devno, qid.path) for the point and on POINTER equality for
    // the source, so this is the shape that reaches it.
    struct Spoor     *mp_a  = s ? mkmp(219u) : NULL;
    struct Spoor     *mp_b  = mp_a ? mkmp(219u) : NULL;
    struct Spoor     *mp_c  = mp_b ? mkmp(219u) : NULL;
    if (!p || !s || !mp_a || !mp_b || !mp_c) {
        if (mp_c) spoor_unref(mp_c);
        if (mp_b) spoor_unref(mp_b);
        if (mp_a) spoor_unref(mp_a);
        if (s)    spoor_unref(s);
        if (p)    territory_unref(p);
        test_fail("territory_alloc / spoor_alloc / mkmp returned NULL");
        return;
    }
    s->devno = 21;

    int  rc_first = mount(p, s, mp_a, 0);
    bool cov_none = mount_noexec_covers(p, '-', 21);

    int  rc_tight = mount(p, s, mp_b, MNOEXEC);
    bool cov_on   = mount_noexec_covers(p, '-', 21);
    int  nmounts1 = territory_nmounts(p);
    int  ref1     = s->ref;

    int  rc_loose = mount(p, s, mp_c, 0);
    bool cov_off  = mount_noexec_covers(p, '-', 21);
    int  nmounts2 = territory_nmounts(p);

    int rc_unmount = unmount(p, mp_a);
    territory_unref(p);
    spoor_unref(mp_c);
    spoor_unref(mp_b);
    spoor_unref(mp_a);
    spoor_unref(s);

    TEST_EXPECT_EQ(rc_first, 0, "first mount succeeds");
    TEST_ASSERT(cov_none == false,
        "CONTROL: mounted with flags 0 -> no restriction (an always-true "
        "predicate fails here, so cov_on below means something)");

    TEST_EXPECT_EQ(rc_tight, 0, "re-mount adding MNOEXEC still returns 0");
    TEST_ASSERT(cov_on == true,
        "#219: a re-mount that ADDS MNOEXEC must APPLY it -- returning 0 with "
        "the pair still unrestricted is a success satisfied by the broken state");
    TEST_EXPECT_EQ(nmounts1, 1,
        "converge must not add a second entry (the idempotency contract)");
    TEST_EXPECT_EQ(ref1, 2,
        "converge must not bump the source ref (test + entry, unchanged)");

    TEST_EXPECT_EQ(rc_loose, 0, "re-mount dropping MNOEXEC returns 0");
    TEST_ASSERT(cov_off == false,
        "converge is SYMMETRIC by design: 0 means the table says what you "
        "asked for, in both directions");
    TEST_EXPECT_EQ(nmounts2, 1, "still exactly one entry after both converges");
    TEST_EXPECT_EQ(rc_unmount, 0, "unmount finds the converged entry");
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

// UM-8 F6: MREPL of an EXISTING member of a union must replace the WHOLE group,
// not converge that member's flags in place. Pre-fix the idempotency arm fired
// on the existing pair and returned BEFORE the group-replace, leaving the other
// members mounted -- a caller MREPLing to guarantee exclusivity got a union.
void test_territory_mount_mrepl_existing_replaces_group(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(11u);
    TEST_ASSERT(p && a && b && mp, "alloc");

    TEST_EXPECT_EQ(mount(p, a, mp, MBEFORE), 0, "A MBEFORE -> [A, covered]");
    TEST_EXPECT_EQ(mount(p, b, mp, MAFTER),  0, "B MAFTER  -> [A, covered, B]");
    TEST_EXPECT_EQ(territory_nmounts(p), 3, "union has 3 members (A, the covered dir, B)");
    TEST_EXPECT_EQ(mp->ref, 2, "the covered entry holds the point");

    // MREPL the EXISTING member A: the whole group collapses to [A] -- the
    // covered directory included.
    TEST_EXPECT_EQ(mount(p, a, mp, MREPL), 0, "MREPL existing A");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "group replaced -> 1 member (covered + B removed)");
    struct Spoor *m0 = mount_member_at(p, mp, 0, NULL);
    struct Spoor *m1 = mount_member_at(p, mp, 1, NULL);
    TEST_ASSERT(m0 == a, "sole member is A");
    TEST_ASSERT(m1 == NULL, "no second member after MREPL");
    if (m0) spoor_clunk(m0);
    TEST_EXPECT_EQ(b->ref, 1, "B's per-entry ref dropped by the group replace");
    TEST_EXPECT_EQ(mp->ref, 1, "the covered entry's ref on the point dropped with it");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount");
    spoor_unref(mp); spoor_unref(a); spoor_unref(b);
    territory_unref(p);
}

// UM-8 F6: MBEFORE of an EXISTING member must MOVE it to the front, not
// converge its flags in place (a silent reorder no-op reporting success).
void test_territory_mount_mbefore_moves_existing(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a = spoor_alloc(&devnone);
    struct Spoor *b = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(12u);
    TEST_ASSERT(p && a && b && mp, "alloc");

    // A starts the point with MREPL, so the union B joins holds no covered
    // directory (covered_reposition moves members around one).
    TEST_EXPECT_EQ(mount(p, a, mp, MREPL),  0, "A MREPL  -> [A]");
    TEST_EXPECT_EQ(mount(p, b, mp, MAFTER), 0, "B MAFTER -> [A, B]");
    struct Spoor *m0 = mount_member_at(p, mp, 0, NULL);
    TEST_ASSERT(m0 == a, "member 0 is A before the move");
    if (m0) spoor_clunk(m0);

    // Re-mount the EXISTING member B with MBEFORE: it moves to the front.
    TEST_EXPECT_EQ(mount(p, b, mp, MBEFORE), 0, "B MBEFORE (reposition)");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "still 2 members (a move, not an add)");
    m0 = mount_member_at(p, mp, 0, NULL);
    struct Spoor *m1 = mount_member_at(p, mp, 1, NULL);
    TEST_ASSERT(m0 == b, "member 0 is now B (moved to front)");
    TEST_ASSERT(m1 == a, "member 1 is A");
    if (m0) spoor_clunk(m0);
    if (m1) spoor_clunk(m1);
    TEST_EXPECT_EQ(b->ref, 2, "B ref net-unchanged by the move (self + one entry)");
    TEST_EXPECT_EQ(mp->ref, 1, "no covered entry: the point hosted A when B joined");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount B (first match)");
    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount A");
    spoor_unref(mp); spoor_unref(a); spoor_unref(b);
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

// The table takes PGRP_MOUNT_CAP entries, each at its own point, and refuses
// the next with -2, taking no ref. The arrays live on the heap: at the cap they
// would not fit a kernel stack. Results are read first and asserted after the
// release, since a failing TEST_EXPECT_EQ returns.
void test_territory_mount_table_full(void) {
    struct Territory *p = territory_alloc();
    struct Spoor **sources = kcalloc(PGRP_MOUNT_CAP, sizeof(*sources), 0);
    struct Spoor **mps     = kcalloc(PGRP_MOUNT_CAP, sizeof(*mps), 0);
    TEST_ASSERT(p && sources && mps, "alloc");
    u64 entries0 = territory_mount_entries_live(), points0 = territory_mount_points_live();

    int made = 0, filled = 0;
    for (int i = 0; i < PGRP_MOUNT_CAP; i++) {
        sources[i] = spoor_alloc(&devnone);
        mps[i]     = mkmp((u64)(100u + i));
        if (sources[i] && mps[i]) made++;
    }
    if (made == PGRP_MOUNT_CAP)
        for (int i = 0; i < PGRP_MOUNT_CAP; i++)
            if (mount(p, sources[i], mps[i], 0) == 0) filled++;
    int n_full = territory_nmounts(p), pts_full = territory_npoints(p);
    int check = territory_mount_check(p);

    struct Spoor *extra    = spoor_alloc(&devnone);
    struct Spoor *mp_extra = mkmp(100u + PGRP_MOUNT_CAP);
    int over = (extra && mp_extra) ? mount(p, extra, mp_extra, 0) : 0;
    int extra_ref = extra ? extra->ref : -1;
    int n_after = territory_nmounts(p);
    struct Spoor *last = made == PGRP_MOUNT_CAP
                       ? mount_lookup(p, mps[PGRP_MOUNT_CAP - 1], NULL) : NULL;
    bool last_crosses = last && last == sources[PGRP_MOUNT_CAP - 1];
    if (last) spoor_clunk(last);

    // territory_unref drops every entry's ref in one shot.
    territory_unref(p);
    int refs_back = 0;
    for (int i = 0; i < PGRP_MOUNT_CAP; i++) {
        if (sources[i] && sources[i]->ref == 1) refs_back++;
        if (sources[i]) spoor_unref(sources[i]);
        if (mps[i])     spoor_unref(mps[i]);
    }
    if (extra)    spoor_unref(extra);
    if (mp_extra) spoor_unref(mp_extra);
    kfree(sources);
    kfree(mps);
    u64 entries1 = territory_mount_entries_live(), points1 = territory_mount_points_live();

    TEST_EXPECT_EQ(made, PGRP_MOUNT_CAP, "every Spoor allocated");
    TEST_EXPECT_EQ(filled, PGRP_MOUNT_CAP, "the table takes PGRP_MOUNT_CAP entries");
    TEST_EXPECT_EQ(n_full, PGRP_MOUNT_CAP, "nmounts counts them");
    TEST_EXPECT_EQ(pts_full, PGRP_MOUNT_CAP, "one point per entry");
    TEST_EXPECT_EQ(check, 0, "the full table's lists, hash and counts agree");
    TEST_EXPECT_EQ(over, -2, "one more mount returns -2");
    TEST_EXPECT_EQ(extra_ref, 1, "the refused mount takes no ref on its source");
    TEST_EXPECT_EQ(n_after, PGRP_MOUNT_CAP, "and installs nothing");
    TEST_ASSERT(last_crosses, "the last point still finds its source by key");
    TEST_EXPECT_EQ(refs_back, PGRP_MOUNT_CAP, "territory_unref dropped each source's entry-ref");
    TEST_EXPECT_EQ(entries1, entries0, "every entry freed");
    TEST_EXPECT_EQ(points1, points0, "every point freed");
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
// #217: MNOEXEC. The point of this test is DISCRIMINATION, not detection -- a
// predicate that returned true unconditionally would satisfy any deny-only
// test, so every deny below is paired with a control that must come back false
// on the same call in the same Territory.
void test_territory_mount_noexec_covers(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *noexec_src = spoor_alloc(&devnone);
    struct Spoor *exec_src   = spoor_alloc(&devnone);
    struct Spoor *otherdc    = spoor_alloc(&devnone);
    TEST_ASSERT(noexec_src && exec_src && otherdc, "spoor_alloc");
    noexec_src->devno = 7;
    exec_src->devno   = 8;
    // Same devno as the flagged source, DIFFERENT dc: if the predicate ignored
    // dc and keyed on devno alone, this would come back true and the container
    // case would be over-restricted at best and mis-scoped at worst.
    otherdc->devno = 7;
    otherdc->dc    = 'X';

    struct Spoor *mp1 = mkmp(101u);
    struct Spoor *mp2 = mkmp(102u);
    struct Spoor *mp3 = mkmp(103u);
    TEST_ASSERT(mp1 && mp2 && mp3, "mkmp");

    TEST_EXPECT_EQ(mount(p, noexec_src, mp1, MNOEXEC), 0, "mount noexec source");
    TEST_EXPECT_EQ(mount(p, exec_src, mp2, 0), 0, "mount plain source");
    TEST_EXPECT_EQ(mount(p, otherdc, mp3, 0), 0, "mount other-dc source");

    TEST_ASSERT(mount_noexec_covers(p, '-', 7) == true,
        "DENY: the MNOEXEC-mounted device instance is covered");
    TEST_ASSERT(mount_noexec_covers(p, '-', 8) == false,
        "CONTROL: an unflagged mount in the SAME table is NOT covered "
        "(an always-true predicate fails here)");
    TEST_ASSERT(mount_noexec_covers(p, 'X', 7) == false,
        "CONTROL: same devno, different dc -> NOT covered (the dc axis is live)");
    TEST_ASSERT(mount_noexec_covers(p, '-', 9) == false,
        "CONTROL: a device instance with no mount entry at all is not covered");
    TEST_ASSERT(mount_noexec_covers(NULL, '-', 7) == false,
        "a Proc with no Territory has no mount that could confer the "
        "restriction -- fail-open is the contract, and kernel boot exec needs it");

    // The flag must survive a fork, or a child escapes it for free. This is the
    // hardware-allowance / I-2 shape: a clone inherits an equally-narrow copy.
    struct Territory *child = territory_clone(p);
    TEST_ASSERT(child != NULL, "territory_clone");
    TEST_ASSERT(mount_noexec_covers(child, '-', 7) == true,
        "a cloned Territory INHERITS MNOEXEC (a child cannot escape by forking)");
    TEST_ASSERT(mount_noexec_covers(child, '-', 8) == false,
        "CONTROL: the clone did not gain the flag on the unflagged entry");
    territory_unref(child);

    // And it must not outlive the mount that conferred it: authority conferred
    // by a namespace edit is revoked by the inverse edit, not sticky per-device.
    TEST_EXPECT_EQ(unmount(p, mp1), 0, "unmount the noexec entry");
    TEST_ASSERT(mount_noexec_covers(p, '-', 7) == false,
        "REVOKED: unmount drops the restriction with the entry");

    territory_unref(p);
    spoor_unref(noexec_src);
    spoor_unref(exec_src);
    spoor_unref(otherdc);
    spoor_unref(mp1);
    spoor_unref(mp2);
    spoor_unref(mp3);
}

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
    struct Spoor *r1 = mount_lookup(p, mp1, NULL);
    TEST_ASSERT(r1 == a, "lookup (-,1,0) -> a");
    if (r1) spoor_clunk(r1);
    struct Spoor *r2 = mount_lookup(p, mp2, NULL);
    TEST_ASSERT(r2 == b, "lookup (-,2,0) -> b");
    if (r2) spoor_clunk(r2);
    TEST_ASSERT(mount_lookup(p, mp3, NULL) == NULL, "lookup unmounted devno -> NULL");

    // unmount by mp1 removes ONLY a's entry; b's (same dc+qid, other devno) stays.
    TEST_EXPECT_EQ(unmount(p, mp1), 0, "unmount (-,1,0)");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "one entry left after unmount");
    TEST_ASSERT(mount_lookup(p, mp1, NULL) == NULL, "(-,1,0) gone");
    struct Spoor *r3 = mount_lookup(p, mp2, NULL);
    TEST_ASSERT(r3 == b, "(-,2,0) still -> b");
    if (r3) spoor_clunk(r3);

    territory_unref(p);
    spoor_unref(a);
    spoor_unref(b);
    spoor_unref(mp1);
    spoor_unref(mp2);
    spoor_unref(mp3);
}

// devno-u64: every Env and every multi-instance attach mints a devno, so an
// unprivileged fork loop reaches 2^32 mints, and a 32-bit key read devno
// 1 + 2^32 as 1. The mount key and MNOEXEC coverage must both keep the two
// apart; each refusal is paired with the control one variable away.
void test_territory_mount_devno_full_width(void) {
    const u64 lo = 1u, hi = (1ull << 32) + 1u;
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");
    struct Spoor *src   = spoor_alloc(&devnone);
    struct Spoor *src2  = spoor_alloc(&devnone);
    struct Spoor *mp_lo = spoor_alloc(&devnone);
    struct Spoor *mp_hi = spoor_alloc(&devnone);
    TEST_ASSERT(src && src2 && mp_lo && mp_hi, "spoor_alloc");
    // MNOEXEC coverage keys on the SOURCE's (dc, devno), so src carries lo; its
    // own qid keeps it a different identity from its point (no self-mount).
    // src2 IS (-,1,0): mounted at (-,1+2^32,0) it is a self-mount only to a
    // cycle check whose key (struct mkey) drops the high half.
    src->devno  = lo; src->qid.path  = 5;
    src2->devno = lo; src2->qid.path = 0;
    mp_lo->qid.path = 0; mp_lo->devno = lo;
    mp_hi->qid.path = 0; mp_hi->devno = hi;

    TEST_EXPECT_EQ(mount(p, src, mp_lo, MNOEXEC), 0, "MNOEXEC mount at (-,1,0)");
    TEST_ASSERT(mount_is_point_id(p, '-', lo, 0),
        "CONTROL: the entry is keyed at (-,1,0)");
    TEST_ASSERT(!mount_is_point_id(p, '-', hi, 0),
        "no entry at (-,1+2^32,0) (a 32-bit key reads it as (-,1,0))");
    TEST_ASSERT(mount_lookup(p, mp_hi, NULL) == NULL,
        "lookup (-,1+2^32,0) -> NULL");
    TEST_ASSERT(mount_noexec_covers(p, '-', lo),
        "CONTROL: device instance 1 is MNOEXEC-covered");
    TEST_ASSERT(!mount_noexec_covers(p, '-', hi),
        "device instance 1+2^32 is not covered");

    // The STORED key: a mount at (-,1+2^32,0) is an entry of its own, and each
    // point resolves to its own source. A 32-bit mp_devno stores it as (-,1,0);
    // a 32-bit mkey refuses the mount itself.
    TEST_EXPECT_EQ(mount(p, src2, mp_hi, 0), 0, "mount src2 at (-,1+2^32,0)");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "two entries, not one");
    struct Spoor *r_lo = mount_lookup(p, mp_lo, NULL);
    TEST_ASSERT(r_lo == src, "lookup (-,1,0) -> src");
    if (r_lo) spoor_clunk(r_lo);
    struct Spoor *r_hi = mount_lookup(p, mp_hi, NULL);
    TEST_ASSERT(r_hi == src2, "lookup (-,1+2^32,0) -> src2");
    if (r_hi) spoor_clunk(r_hi);

    territory_unref(p);
    spoor_unref(src);
    spoor_unref(src2);
    spoor_unref(mp_lo);
    spoor_unref(mp_hi);
}

// devno-u64: the minter crosses the old 32-bit wrap instead of returning to 0.
// Forward-only, so every devno minted after this test is above 2^32 and the
// rest of the boot -- the suite and userspace -- runs on wide devnos.
void test_territory_mount_devno_minter_crosses_2_32(void) {
    spoor_devno_advance_for_test(0xFFFFFFFEull);
    u64 a = spoor_next_devno();
    u64 b = spoor_next_devno();
    TEST_ASSERT(a >= 0xFFFFFFFFull, "minted at or past 2^32 - 1");
    TEST_ASSERT(b > a, "monotonic across the old wrap (a 32-bit counter returned 0)");
    TEST_ASSERT(b > 0xFFFFFFFFull, "the next mint is above 2^32 - 1");
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

    struct Spoor *got = mount_lookup(p, mp, NULL);             // SA-F1: returns a REF-HELD source
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

// #66 (I-33): the mount entry retains the mount-POINT's namespace name (mp_path)
// with a Path ref, shared across clones, dropped at unmount / MREPL-displace /
// final release. This exercises the FOUR mp_path lifecycle hooks via the live
// Path counters -- a missed path_ref/unref shows up as a leak or a double-free.
void test_territory_mount_mp_path_lifecycle(void) {
    struct Territory *parent = territory_alloc();
    TEST_ASSERT(parent != NULL, "territory_alloc");
    struct Spoor *s  = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(11u);
    TEST_ASSERT(s && mp, "spoor_alloc/mkmp");
    mp->path = mkname("srv");                       // mountpoint named "/srv"
    TEST_ASSERT(mp->path != NULL, "mkname");

    u64 pa0 = path_total_allocated(), pf0 = path_total_freed();

    // mount: the entry takes ONE ref on mp->path (no new alloc).
    TEST_EXPECT_EQ(mount(parent, s, mp, 0), 0, "mount with a named mount point");
    TEST_EXPECT_EQ(path_total_allocated() - pa0, (u64)0,
                   "mount path_ref's the mount-point name; allocates no new Path");
    TEST_EXPECT_EQ(path_total_freed() - pf0, (u64)0, "mount frees no Path");

    // clone: the child SHARES mp->path -> a second entry ref (still no alloc).
    struct Territory *child = territory_clone(parent);
    TEST_ASSERT(child != NULL, "territory_clone");
    TEST_EXPECT_EQ(path_total_allocated() - pa0, (u64)0, "clone shares; no new Path");
    TEST_EXPECT_EQ(path_total_freed() - pf0, (u64)0, "clone frees no Path");

    // Destroy the child: drops ITS entry ref. The Path is still held by the
    // parent's entry + the test's mp->path, so it must NOT free yet.
    territory_unref(child);
    TEST_EXPECT_EQ(path_total_freed() - pf0, (u64)0,
                   "child destroy drops one mp_path ref but does NOT free (still held)");

    // Destroy the parent: drops the parent's entry ref. The test's mp still
    // holds the last ref via mp->path -> still no free.
    territory_unref(parent);
    TEST_EXPECT_EQ(path_total_freed() - pf0, (u64)0,
                   "parent destroy drops the last ENTRY ref; mp->path still holds it");

    // Drop the test's mp Spoor: spoor_free_internal path_unref's mp->path ->
    // the LAST ref -> the Path frees exactly once.
    spoor_unref(mp);
    TEST_EXPECT_EQ(path_total_freed() - pf0, (u64)1,
                   "the mount-point name frees exactly once when its last ref drops");

    spoor_unref(s);
}

// #66: territory_format_ns renders one "mount <pt> <src>\n" line per entry +
// "binds: N\n". Mount-point name from mp_path; source label = source->path when
// the source has a namespace name, else "#<dc>" (the Plan 9 device spec).
void test_territory_mount_format_ns(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc");

    // Entry 1: /srv <- a device-root source (no path) -> "#-" (devnone dc='-').
    struct Spoor *sa  = spoor_alloc(&devnone);
    struct Spoor *msrv = mkmp(1u);
    TEST_ASSERT(sa && msrv, "alloc 1");
    msrv->path = mkname("srv");
    TEST_ASSERT(msrv->path != NULL, "mkname srv");
    TEST_EXPECT_EQ(mount(p, sa, msrv, 0), 0, "mount /srv");

    // Entry 2: /proc <- a source WITH a namespace name "/realsrc" -> the path.
    struct Spoor *sb   = spoor_alloc(&devnone);
    struct Spoor *mproc = mkmp(2u);
    TEST_ASSERT(sb && mproc, "alloc 2");
    mproc->path = mkname("proc");
    sb->path    = mkname("realsrc");
    TEST_ASSERT(mproc->path && sb->path, "mkname proc/realsrc");
    // #217: mounted MNOEXEC, so this entry must render the suffix and the OTHER
    // one must not -- the rendering is only useful if it discriminates.
    TEST_EXPECT_EQ(mount(p, sb, mproc, MNOEXEC), 0, "mount /proc noexec");

    char buf[256];
    u64 n = territory_format_ns(p, buf, sizeof(buf));
    TEST_ASSERT(n > 0 && n < sizeof(buf), "format_ns produced bounded output");
    buf[n] = '\0';
    TEST_ASSERT(mnt_streq(buf,
                "mount /srv #-\nmount /proc /realsrc noexec\nbinds: 0\n"),
                "ns renders both mounts (name + #dc / source-path), the #217 "
                "noexec suffix on the flagged entry ONLY, + the bind count");

    // F2 (audit): truncation discards a partial line at a whole-line boundary --
    // a cap that fits line 1 ("mount /srv #-\n" = 14 B) but not line 2 yields
    // EXACTLY the first complete line, with NO partial second line AND NO
    // trailing "binds:" (which would falsely imply completeness).
    char mid[20];
    u64 m = territory_format_ns(p, mid, sizeof(mid));
    TEST_ASSERT(m == 14, "format_ns truncated to one whole line (14 B)");
    mid[m] = '\0';
    TEST_ASSERT(mnt_streq(mid, "mount /srv #-\n"),
                "F2: truncation is clean -- first whole line only, no partial, no binds");

    // A cap too small even for line 1 yields a clean empty result (no partial).
    char tiny[8];
    u64 t = territory_format_ns(p, tiny, sizeof(tiny));
    TEST_EXPECT_EQ((int)t, 0, "F2: cap below one line -> empty (no partial 'mount ')");

    territory_unref(p);
    spoor_unref(sa);
    spoor_unref(sb);
    spoor_unref(msrv);
    spoor_unref(mproc);
}

// =============================================================================
// Plan 9 unions (ARCH 9.5; territory.tla, the covered directory): an MBEFORE /
// MAFTER mount at a DIRECTORY hosting no member also makes the directory it
// covers a member, so the union searches it too.
// =============================================================================

// <new, covered> for MBEFORE, <covered, new> for MAFTER. The covered entry's
// source is the point's own Spoor, and it carries MCOVERED ALONE: a member's
// MCREATE / MNOEXEC / MPHENO_LINUX says nothing about the directory under it.
void test_territory_mount_union_keeps_covered(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *b  = spoor_alloc(&devnone);
    struct Spoor *mb = mkmp(21u);
    struct Spoor *ma = mkmp(22u);
    TEST_ASSERT(p && a && b && mb && ma, "alloc");

    const u32 every = MCREATE | MNOEXEC | MPHENO_LINUX;
    TEST_EXPECT_EQ(mount(p, a, mb, MBEFORE | every), 0, "A MBEFORE at a fresh point");
    TEST_EXPECT_EQ(mount(p, b, ma, MAFTER | every),  0, "B MAFTER at a fresh point");
    TEST_EXPECT_EQ(territory_nmounts(p), 4, "each mount installed two entries");
    TEST_EXPECT_EQ(mb->ref, 2, "the covered entry holds its point (test + entry)");
    TEST_EXPECT_EQ(ma->ref, 2, "likewise at the MAFTER point");
    TEST_EXPECT_EQ(a->ref, 2, "the member's own entry ref, as ever");

    u32 f0 = 0, f1 = 0;
    struct Spoor *m0 = mount_member_at(p, mb, 0, &f0);
    struct Spoor *m1 = mount_member_at(p, mb, 1, &f1);
    TEST_ASSERT(m0 == a && f0 == (MBEFORE | every), "MBEFORE: member 0 is A, its flags intact");
    TEST_ASSERT(m1 == mb && f1 == MCOVERED, "MBEFORE: member 1 is the covered directory, MCOVERED alone");
    if (m0) spoor_clunk(m0);
    if (m1) spoor_clunk(m1);
    TEST_ASSERT(mount_member_at(p, mb, 2, NULL) == NULL, "MBEFORE: exactly two members");

    m0 = mount_member_at(p, ma, 0, &f0);
    m1 = mount_member_at(p, ma, 1, &f1);
    TEST_ASSERT(m0 == ma && f0 == MCOVERED, "MAFTER: member 0 is the covered directory, MCOVERED alone");
    TEST_ASSERT(m1 == b && f1 == (MAFTER | every), "MAFTER: member 1 is B");
    if (m0) spoor_clunk(m0);
    if (m1) spoor_clunk(m1);

    // The resolver's base cross takes member 0 -- under MAFTER the covered
    // directory, which stalk_cross_src clones rather than crosses.
    u32 lf = 0;
    struct Spoor *l = mount_lookup(p, ma, &lf);
    TEST_ASSERT(l == ma && lf == MCOVERED, "mount_lookup answers the covered member first under MAFTER");
    if (l) spoor_clunk(l);

    territory_unref(p);
    TEST_EXPECT_EQ(mb->ref, 1, "destroy drops the covered entry's ref on the point");
    TEST_EXPECT_EQ(ma->ref, 1, "at both points");
    spoor_unref(a); spoor_unref(b); spoor_unref(mb); spoor_unref(ma);
}

// No covered entry unless an MBEFORE / MAFTER mount STARTS the union: MREPL
// replaces whatever the point shows, a flagless mount is a plain mount, and a
// member joining a point that already hosts one finds the union formed.
void test_territory_mount_no_covered_unless_fresh(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *b  = spoor_alloc(&devnone);
    struct Spoor *mr = mkmp(23u);
    struct Spoor *mf = mkmp(24u);
    struct Spoor *mu = mkmp(25u);
    TEST_ASSERT(p && a && b && mr && mf && mu, "alloc");

    TEST_EXPECT_EQ(mount(p, a, mr, MREPL), 0, "A MREPL at a fresh point");
    TEST_EXPECT_EQ(mount(p, a, mf, 0),     0, "A flagless at a fresh point");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "one entry each");
    TEST_EXPECT_EQ(mr->ref, 1, "MREPL added no covered entry");
    TEST_EXPECT_EQ(mf->ref, 1, "nor did the flagless mount");

    TEST_EXPECT_EQ(mount(p, b, mr, MBEFORE), 0, "B MBEFORE at the MREPL point");
    TEST_EXPECT_EQ(mount(p, b, mf, MAFTER),  0, "B MAFTER at the flagless point");
    TEST_EXPECT_EQ(territory_nmounts(p), 4, "one more entry each: the points already hosted A");
    TEST_EXPECT_EQ(mr->ref, 1, "still no covered entry at the MREPL point");
    TEST_EXPECT_EQ(mf->ref, 1, "nor at the flagless one");
    struct Spoor *m = mount_member_at(p, mr, 0, NULL);
    TEST_ASSERT(m == b, "MBEFORE still put B ahead of A");
    if (m) spoor_clunk(m);

    // MREPL over a union replaces its covered member with the rest.
    TEST_EXPECT_EQ(mount(p, a, mu, MAFTER), 0, "A MAFTER at a fresh point -> [covered, A]");
    TEST_EXPECT_EQ(mu->ref, 2, "a covered entry");
    TEST_EXPECT_EQ(mount(p, b, mu, MREPL), 0, "B MREPL over the union");
    TEST_EXPECT_EQ(mu->ref, 1, "the covered entry went with the group");
    u32 f = 0;
    m = mount_member_at(p, mu, 0, &f);
    TEST_ASSERT(m == b && f == MREPL, "B alone, as mounted");
    if (m) spoor_clunk(m);
    TEST_ASSERT(mount_member_at(p, mu, 1, NULL) == NULL, "nothing after B");

    territory_unref(p);
    spoor_unref(a); spoor_unref(b); spoor_unref(mr); spoor_unref(mf); spoor_unref(mu);
}

// A point that is not a directory has nothing a union could search: an MBEFORE
// / MAFTER mount there stays a plain mount and never retains the point
// (territory.tla NoCoveredFile). The same mount at a directory, the control,
// adds the covered member.
void test_territory_mount_covered_file_point_stays_plain(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *fp = spoor_alloc(&devnone);   // a FILE point
    struct Spoor *dp = mkmp(27u);               // the control: a directory
    TEST_ASSERT(p && a && fp && dp, "alloc");
    fp->qid.path = 26u;
    fp->qid.type = QTFILE;

    TEST_EXPECT_EQ(mount(p, a, fp, MBEFORE), 0, "A MBEFORE at a file point");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "one entry: no union at a file");
    TEST_EXPECT_EQ(fp->ref, 1, "the file point is not retained");
    TEST_EXPECT_EQ(unmount(p, fp), 0, "unmount");
    TEST_EXPECT_EQ(mount(p, a, fp, MAFTER), 0, "A MAFTER at the file point");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "one entry under MAFTER too");
    TEST_EXPECT_EQ(fp->ref, 1, "still not retained");
    TEST_EXPECT_EQ(unmount(p, fp), 0, "unmount");

    TEST_EXPECT_EQ(mount(p, a, dp, MBEFORE), 0, "control: A MBEFORE at a directory");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "control: the member and the covered directory");
    TEST_EXPECT_EQ(dp->ref, 2, "control: the directory is retained");

    territory_unref(p);
    spoor_unref(a); spoor_unref(fp); spoor_unref(dp);
}

// A reposition (UM-8 F6) moves a member around the covered directory without
// adding a second one; and re-mounting the SOLE member of an MREPL group with
// MBEFORE repositions it at a point that was not fresh, so it grows none
// (territory.tla BUGGY_FRESH_AFTER_REMOVE).
void test_territory_mount_covered_reposition(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *b  = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(28u);
    struct Spoor *m2 = mkmp(29u);
    TEST_ASSERT(p && a && b && mp && m2, "alloc");

    TEST_EXPECT_EQ(mount(p, a, mp, MBEFORE), 0, "A MBEFORE -> [A, covered]");
    TEST_EXPECT_EQ(mount(p, b, mp, MAFTER),  0, "B MAFTER  -> [A, covered, B]");
    TEST_EXPECT_EQ(mount(p, b, mp, MBEFORE), 0, "B MBEFORE (reposition) -> [B, A, covered]");
    struct Spoor *want1[3] = { b, a, mp };
    for (int i = 0; i < 3; i++) {
        struct Spoor *m = mount_member_at(p, mp, i, NULL);
        TEST_ASSERT(m == want1[i], "after the MBEFORE move: [B, A, covered]");
        spoor_clunk(m);
    }
    TEST_EXPECT_EQ(mount(p, a, mp, MAFTER), 0, "A MAFTER (reposition) -> [B, covered, A]");
    struct Spoor *want2[3] = { b, mp, a };
    for (int i = 0; i < 3; i++) {
        struct Spoor *m = mount_member_at(p, mp, i, NULL);
        TEST_ASSERT(m == want2[i], "after the MAFTER move: [B, covered, A]");
        spoor_clunk(m);
    }
    TEST_EXPECT_EQ(territory_nmounts(p), 3, "moves add no entry");
    TEST_EXPECT_EQ(mp->ref, 2, "exactly one covered entry");

    TEST_EXPECT_EQ(mount(p, a, m2, MREPL),   0, "A MREPL at a second point -> [A]");
    TEST_EXPECT_EQ(mount(p, a, m2, MBEFORE), 0, "A MBEFORE: a reposition of the sole member");
    TEST_EXPECT_EQ(territory_nmounts(p), 4, "one entry at the second point");
    TEST_EXPECT_EQ(m2->ref, 1, "no covered entry: the point hosted A");

    territory_unref(p);
    TEST_EXPECT_EQ(mp->ref, 1, "destroy released the covered entry once");
    spoor_unref(a); spoor_unref(b); spoor_unref(mp); spoor_unref(m2);
}

// unmount never removes the covered directory by itself, and removes it with
// the last member actually mounted at the point: a point left holding only its
// own directory would be a union of itself.
void test_territory_mount_covered_leaves_with_last(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *b  = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(30u);
    TEST_ASSERT(p && a && b && mp, "alloc");

    TEST_EXPECT_EQ(mount(p, a, mp, MBEFORE), 0, "A MBEFORE -> [A, covered]");
    TEST_EXPECT_EQ(mount(p, b, mp, MAFTER),  0, "B MAFTER  -> [A, covered, B]");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount: A goes, not the covered directory");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "[covered, B]");
    u32 f = 0;
    struct Spoor *m = mount_member_at(p, mp, 0, &f);
    TEST_ASSERT(m == mp && f == MCOVERED, "member 0 is now the covered directory");
    if (m) spoor_clunk(m);
    TEST_EXPECT_EQ(a->ref, 1, "A's entry ref dropped");
    TEST_EXPECT_EQ(mp->ref, 2, "the covered entry still holds the point");

    TEST_EXPECT_EQ(unmount(p, mp), 0, "unmount: B goes, and the covered directory with it");
    TEST_EXPECT_EQ(territory_nmounts(p), 0, "the point is a plain directory again");
    TEST_EXPECT_EQ(b->ref, 1, "B's entry ref dropped");
    TEST_EXPECT_EQ(mp->ref, 1, "the covered entry's ref on the point dropped");
    TEST_EXPECT_EQ(unmount(p, mp), -1, "nothing is left to unmount");

    territory_unref(p);
    spoor_unref(a); spoor_unref(b); spoor_unref(mp);
}

// A union's first mount installs two entries, so it needs room for two. With
// room for one it fails -2 and installs NOTHING -- no member without its
// covered directory, no reference taken, no point -- while a mount needing one
// entry still fits.
void test_territory_mount_covered_needs_two_slots(void) {
    struct Territory *p = territory_alloc();
    struct Spoor **sources = kcalloc(PGRP_MOUNT_CAP - 1, sizeof(*sources), 0);
    struct Spoor **mps     = kcalloc(PGRP_MOUNT_CAP - 1, sizeof(*mps), 0);
    TEST_ASSERT(p && sources && mps, "alloc");
    int filled = 0;
    for (int i = 0; i < PGRP_MOUNT_CAP - 1; i++) {
        sources[i] = spoor_alloc(&devnone);
        mps[i]     = mkmp((u64)(3000u + i));
        if (sources[i] && mps[i] && mount(p, sources[i], mps[i], 0) == 0) filled++;
    }
    int n_one_free = territory_nmounts(p);

    struct Spoor *s  = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(5000u);
    int before = -9, after = -9, n_refused = -1, s_ref = -1, mp_ref = -1, pts = -1;
    int plain = -9, n_full = -1;
    if (s && mp) {
        before    = mount(p, s, mp, MBEFORE);
        after     = mount(p, s, mp, MAFTER);
        n_refused = territory_nmounts(p);
        pts       = territory_npoints(p);
        s_ref     = s->ref;
        mp_ref    = mp->ref;
        plain     = mount(p, s, mp, 0);
        n_full    = territory_nmounts(p);
    }

    territory_unref(p);
    for (int i = 0; i < PGRP_MOUNT_CAP - 1; i++) {
        if (sources[i]) spoor_unref(sources[i]);
        if (mps[i])     spoor_unref(mps[i]);
    }
    kfree(sources);
    kfree(mps);
    if (s)  spoor_unref(s);
    if (mp) spoor_unref(mp);

    TEST_EXPECT_EQ(filled, PGRP_MOUNT_CAP - 1, "fill");
    TEST_EXPECT_EQ(n_one_free, PGRP_MOUNT_CAP - 1, "room for one");
    TEST_EXPECT_EQ(before, -2, "MBEFORE starting a union needs room for two");
    TEST_EXPECT_EQ(after,  -2, "so does MAFTER");
    TEST_EXPECT_EQ(n_refused, PGRP_MOUNT_CAP - 1, "nothing installed");
    TEST_EXPECT_EQ(pts, PGRP_MOUNT_CAP - 1, "no point added");
    TEST_EXPECT_EQ(s_ref, 1, "no ref on the source");
    TEST_EXPECT_EQ(mp_ref, 1, "no ref on the point");
    TEST_EXPECT_EQ(plain, 0, "a plain mount needs one entry and fits");
    TEST_EXPECT_EQ(n_full, PGRP_MOUNT_CAP, "full");
}

// A self-mount stays an I-3 cycle even though a union's covered entry is keyed
// at its own source: that entry is the kernel's, never crossed, and no caller
// can install one. Nor is the covered self-edge a cycle for a later mount: the
// union's point, mounted elsewhere, installs.
void test_territory_mount_covered_self_mount_refused(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *mp   = mkmp(31u);
    struct Spoor *same = mkmp(31u);   // another Spoor with the point's identity
    struct Spoor *a    = spoor_alloc(&devnone);
    struct Spoor *q    = mkmp(32u);
    TEST_ASSERT(p && mp && same && a && q, "alloc");

    TEST_EXPECT_EQ(mount(p, mp, mp, MBEFORE),  -3, "the point at itself, MBEFORE: refused");
    TEST_EXPECT_EQ(mount(p, same, mp, MAFTER), -3, "the point's identity at it, MAFTER: refused");
    TEST_EXPECT_EQ(territory_nmounts(p), 0, "nothing installed");
    TEST_EXPECT_EQ(mp->ref, 1, "no ref taken");

    TEST_EXPECT_EQ(mount(p, a, mp, MBEFORE), 0, "A MBEFORE -> [A, covered]");
    TEST_EXPECT_EQ(mount(p, same, mp, MBEFORE), -3, "the point's identity into its own union: refused");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "still [A, covered]");

    TEST_EXPECT_EQ(mount(p, mp, q, 0), 0, "the union's point mounted at another point");
    TEST_EXPECT_EQ(territory_nmounts(p), 3, "three entries");

    territory_unref(p);
    TEST_EXPECT_EQ(mp->ref, 1, "both of mp's entry refs released");
    spoor_unref(mp); spoor_unref(same); spoor_unref(a); spoor_unref(q);
}

// A clone copies the covered entry like any other (one ref on the point per
// table), and each Territory's union then lives on its own (I-1): unmounting
// in the child dissolves the child's union, covered entry included, and leaves
// the parent's whole.
void test_territory_mount_covered_clone(void) {
    struct Territory *parent = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(33u);
    TEST_ASSERT(parent && a && mp, "alloc");
    TEST_EXPECT_EQ(mount(parent, a, mp, MAFTER), 0, "A MAFTER -> [covered, A]");

    struct Territory *child = territory_clone(parent);
    TEST_ASSERT(child != NULL, "territory_clone");
    TEST_EXPECT_EQ(territory_nmounts(child), 2, "the child has the covered entry too");
    TEST_EXPECT_EQ(mp->ref, 3, "the point: test + parent's covered entry + child's");

    TEST_EXPECT_EQ(unmount(child, mp), 0, "the child unmounts A");
    TEST_EXPECT_EQ(territory_nmounts(child), 0, "the child's union is gone, covered entry included");
    TEST_EXPECT_EQ(territory_nmounts(parent), 2, "the parent's union is untouched");
    TEST_EXPECT_EQ(mp->ref, 2, "only the child's covered entry released the point");

    territory_unref(child);
    territory_unref(parent);
    TEST_EXPECT_EQ(mp->ref, 1, "every table's ref on the point released");
    TEST_EXPECT_EQ(a->ref, 1, "and on A");
    spoor_unref(a); spoor_unref(mp);
}

// MNOEXEC restricts the device a member's entry names. The covered entry names
// the point's own device and carries no MNOEXEC, so marking the member noexec
// leaves the directory under it executable -- and the table shows why: the
// covered entry is present, on its own device, flagged MCOVERED alone.
void test_territory_mount_covered_noexec_scoped(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(34u);
    TEST_ASSERT(p && a && mp, "alloc");
    a->devno  = 11;   // the member's device instance
    mp->devno = 12;   // the covered directory's
    TEST_EXPECT_EQ(mount(p, a, mp, MBEFORE | MNOEXEC), 0, "A MBEFORE|MNOEXEC -> [A, covered]");

    u32 f = 0;
    struct Spoor *m = mount_member_at(p, mp, 1, &f);
    TEST_ASSERT(m == mp && f == MCOVERED, "the covered entry is there, MCOVERED alone");
    if (m) spoor_clunk(m);
    TEST_ASSERT(mount_noexec_covers(p, '-', 11) == true, "the member's device is restricted");
    TEST_ASSERT(mount_noexec_covers(p, '-', 12) == false,
        "the covered directory's device is not: the flag did not travel to its entry");

    territory_unref(p);
    spoor_unref(a); spoor_unref(mp);
}

// ns shows the covered directory as the member it is: in search order, its
// line names the point twice and carries " covered" alone.
void test_territory_mount_covered_format_ns(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a  = spoor_alloc(&devnone);
    struct Spoor *mp = mkmp(35u);
    TEST_ASSERT(p && a && mp, "alloc");
    mp->path = mkname("u");
    TEST_ASSERT(mp->path != NULL, "mkname u");
    TEST_EXPECT_EQ(mount(p, a, mp, MBEFORE | MNOEXEC), 0, "A MBEFORE|MNOEXEC -> [A, covered]");

    char buf[128];
    u64 n = territory_format_ns(p, buf, sizeof(buf));
    TEST_ASSERT(n > 0 && n < sizeof(buf), "bounded output");
    buf[n] = '\0';
    TEST_ASSERT(mnt_streq(buf, "mount /u #- noexec\nmount /u /u covered\nbinds: 0\n"),
        "the member, then the covered directory named by its point, suffixed covered alone");

    territory_unref(p);
    spoor_unref(a); spoor_unref(mp);
}

// /proc/<pid>/ns as format_ns renders it, against what a step expects.
static bool ns_is(struct Territory *t, const char *want) {
    char buf[512];
    u64 n = territory_format_ns(t, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    return mnt_streq(buf, want);
}

struct edit_seen { int rc, check, points; bool ns; };

static struct edit_seen edit_seen(struct Territory *t, int rc, const char *want) {
    struct edit_seen e = { rc, territory_mount_check(t), territory_npoints(t), ns_is(t, want) };
    return e;
}

// The mount table's nodes through every kind of edit (ARCH 9.1). After each,
// the lists, hash and counts agree (territory_mount_check), there is a point
// per mount point hosting an entry, and /proc/<pid>/ns prints the order the
// fixed array kept: MBEFORE just before the point's first member, every other
// placement at the end. A clone copies both orders; a snapshot too small for
// the union takes nothing; unmounting, replacing, repositioning, shedding and
// releasing give back every node.
void test_territory_mount_nodes_follow_every_edit(void) {
    u64 entries0 = territory_mount_entries_live(), points0 = territory_mount_points_live();
    struct Territory *p = territory_alloc();
    struct Spoor *a = spoor_alloc(&devnone), *b = spoor_alloc(&devnone);
    struct Spoor *c = spoor_alloc(&devnone), *root_x = spoor_alloc(&devnone);
    struct Spoor *mp1 = mkmp(1u), *mp2 = mkmp(2u);
    TEST_ASSERT(p && a && b && c && root_x && mp1 && mp2, "alloc");
    a->path   = mkname("A");   b->path   = mkname("B");   c->path = mkname("C");
    mp1->path = mkname("one"); mp2->path = mkname("two");
    TEST_ASSERT(a->path && b->path && c->path && mp1->path && mp2->path, "names");
    root_x->devno = 77u;                      // an instance no point lies in

    static const char *const want[9] = {
        "mount /one /A\nbinds: 0\n",
        "mount /one /A\nmount /two /two covered\nmount /two /B\nbinds: 0\n",
        "mount /one /C\nmount /one /A\nmount /two /two covered\nmount /two /B\nbinds: 0\n",
        "mount /one /C\nmount /one /A\nmount /two /A\nmount /two /two covered\nmount /two /B\nbinds: 0\n",
        "mount /one /C\nmount /one /A\nmount /two /B\nmount /two /A\nmount /two /two covered\nbinds: 0\n",
        "mount /one /C\nmount /one /A\nmount /two /C\nbinds: 0\n",
        "mount /one /A\nmount /two /C\nbinds: 0\n",
        "mount /two /C\nbinds: 0\n",
        "binds: 0\n",
    };
    static const char *const step[9] = {
        "step 0, a plain mount: ns prints it",
        "step 1, a union started MAFTER: <covered, B>, appended",
        "step 2, MBEFORE at a plain point: C just before A",
        "step 3, MBEFORE at a union: A just before the covered entry, not at the end",
        "step 4, a reposition: B moves to its point's front, the rest keep their places",
        "step 5, MREPL: the whole union at /two replaced by C, at the end",
        "step 6, unmount: the first member at /one goes",
        "step 7, unmount: the last member goes, and its point with it",
        "step 8, chroot into an instance no point is in: the shed drops everything",
    };
    static const int want_points[9] = { 1, 2, 2, 2, 2, 2, 2, 1, 0 };
    struct edit_seen s[9];
    s[0] = edit_seen(p, mount(p, a, mp1, 0),       want[0]);
    s[1] = edit_seen(p, mount(p, b, mp2, MAFTER),  want[1]);
    s[2] = edit_seen(p, mount(p, c, mp1, MBEFORE), want[2]);
    s[3] = edit_seen(p, mount(p, a, mp2, MBEFORE), want[3]);

    // The snapshot takes the whole union or nothing: mp2 holds [A, covered, B].
    struct Spoor *out[3] = { NULL, NULL, NULL };
    u32 fl[3] = { 0, 0, 0 };
    int a_ref = a->ref, b_ref = b->ref, mp2_ref = mp2->ref;
    int short_n = mount_members_snapshot(p, mp2, out, fl, 2);
    bool short_took_nothing = out[0] == NULL && out[1] == NULL && a->ref == a_ref &&
                              b->ref == b_ref && mp2->ref == mp2_ref;
    int full_n = mount_members_snapshot(p, mp2, out, fl, 3);
    bool full_in_order = out[0] == a && out[1] == mp2 && out[2] == b &&
                         fl[0] == MBEFORE && fl[1] == MCOVERED && fl[2] == MAFTER;
    for (int i = 0; i < 3; i++) if (out[i]) spoor_clunk(out[i]);

    // A clone copies the table order and every point's search order.
    struct Territory *child = territory_clone(p);
    bool clone_ns = child && ns_is(child, want[3]);
    int clone_check = child ? territory_mount_check(child) : -1;
    int clone_points = child ? territory_npoints(child) : -1;
    struct Spoor *c0 = child ? mount_member_at(child, mp2, 0, NULL) : NULL;
    struct Spoor *c1 = child ? mount_member_at(child, mp2, 1, NULL) : NULL;
    bool clone_order = c0 == a && c1 == mp2;
    if (c0) spoor_clunk(c0);
    if (c1) spoor_clunk(c1);
    territory_unref(child);

    s[4] = edit_seen(p, mount(p, b, mp2, MBEFORE), want[4]);
    s[5] = edit_seen(p, mount(p, c, mp2, MREPL),   want[5]);
    int mp2_after_repl = mp2->ref;
    s[6] = edit_seen(p, unmount(p, mp1), want[6]);
    s[7] = edit_seen(p, unmount(p, mp1), want[7]);
    int nothing_left = unmount(p, mp1);
    s[8] = edit_seen(p, territory_chroot(p, root_x), want[8]);
    int a_end = a->ref, b_end = b->ref, c_end = c->ref;

    territory_unref(p);
    spoor_unref(a); spoor_unref(b); spoor_unref(c); spoor_unref(root_x);
    spoor_unref(mp1); spoor_unref(mp2);
    u64 entries1 = territory_mount_entries_live(), points1 = territory_mount_points_live();

    for (int i = 0; i < 9; i++) {
        TEST_EXPECT_EQ(s[i].rc, 0, step[i]);
        TEST_EXPECT_EQ(s[i].check, 0, step[i]);
        TEST_EXPECT_EQ(s[i].points, want_points[i], step[i]);
        TEST_ASSERT(s[i].ns, step[i]);
    }
    TEST_EXPECT_EQ(short_n, 3, "a snapshot too small still answers the member count");
    TEST_ASSERT(short_took_nothing, "and writes and refs nothing");
    TEST_EXPECT_EQ(full_n, 3, "a snapshot with room takes the union");
    TEST_ASSERT(full_in_order, "in search order, each member's flags beside it");
    TEST_ASSERT(clone_ns, "a clone prints the parent's table order");
    TEST_EXPECT_EQ(clone_check, 0, "the clone's lists, hash and counts agree");
    TEST_EXPECT_EQ(clone_points, 2, "the clone has the parent's two points");
    TEST_ASSERT(clone_order, "the clone keeps /two's search order: A, then the covered entry");
    TEST_EXPECT_EQ(mp2_after_repl, 1, "MREPL released the covered entry's ref on the point");
    TEST_EXPECT_EQ(nothing_left, -1, "nothing is left at /one");
    TEST_EXPECT_EQ(a_end, 1, "every entry of A released");
    TEST_EXPECT_EQ(b_end, 1, "every entry of B released");
    TEST_EXPECT_EQ(c_end, 1, "the shed released C");
    TEST_EXPECT_EQ(entries1, entries0, "every entry freed");
    TEST_EXPECT_EQ(points1, points0, "every point freed");
}

// ARCH 9.1: mount() allocates before ns_lock, so a failed allocation answers
// -4 and changes nothing -- no entry, no point, no ref -- whichever of its
// three allocations fails, at a point that hosts a member or a new one. A
// clone that cannot allocate its copy fails whole. Each refusal is followed by
// the same call with the knob off, which succeeds: the -4 came from the
// allocation.
void test_territory_mount_refuses_enomem(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *a = spoor_alloc(&devnone), *b = spoor_alloc(&devnone);
    struct Spoor *mp1 = mkmp(1u), *mp2 = mkmp(2u);
    TEST_ASSERT(p && a && b && mp1 && mp2, "alloc");
    int base = mount(p, a, mp1, 0);
    u64 entries0 = territory_mount_entries_live(), points0 = territory_mount_points_live();

    int refused[4], left[4];
    bool unchanged[4];
    for (u32 k = 1; k <= 3; k++) {
        territory_mount_fail_nodes_for_test(k);
        refused[k - 1] = mount(p, b, mp1, MAFTER);
        left[k - 1]    = (int)territory_mount_fail_nodes_for_test(0);
        unchanged[k - 1] = territory_nmounts(p) == 1 && territory_npoints(p) == 1 &&
                           b->ref == 1 && territory_mount_check(p) == 0 &&
                           territory_mount_entries_live() == entries0 &&
                           territory_mount_points_live() == points0;
    }
    territory_mount_fail_nodes_for_test(1);
    refused[3] = mount(p, b, mp2, MBEFORE);              // a new point, a union's start
    left[3]    = (int)territory_mount_fail_nodes_for_test(0);
    unchanged[3] = territory_nmounts(p) == 1 && territory_npoints(p) == 1 &&
                   b->ref == 1 && mp2->ref == 1 &&
                   territory_mount_entries_live() == entries0 &&
                   territory_mount_points_live() == points0;

    u64 made0 = territory_total_created(), gone0 = territory_total_destroyed();
    territory_mount_fail_nodes_for_test(1);
    struct Territory *child = territory_clone(p);
    int clone_left = (int)territory_mount_fail_nodes_for_test(0);
    bool clone_failed_whole = child == NULL && a->ref == 2 &&
                              territory_total_created() - made0 == territory_total_destroyed() - gone0 &&
                              territory_mount_entries_live() == entries0 &&
                              territory_mount_points_live() == points0 &&
                              territory_mount_check(p) == 0;
    territory_unref(child);

    int control_plain = mount(p, b, mp1, MAFTER);
    int control_union = mount(p, b, mp2, MBEFORE);
    struct Territory *control_child = territory_clone(p);
    bool control_clone = control_child && territory_nmounts(control_child) == 4;
    territory_unref(control_child);

    territory_unref(p);
    spoor_unref(a); spoor_unref(b); spoor_unref(mp1); spoor_unref(mp2);

    TEST_EXPECT_EQ(base, 0, "one entry to start from");
    for (int i = 0; i < 4; i++) {
        TEST_EXPECT_EQ(refused[i], -4, "a failed allocation refuses the mount with -4");
        TEST_EXPECT_EQ(left[i], 0, "mount() asked for every node the knob failed");
        TEST_ASSERT(unchanged[i], "and changes nothing: table, points, refs, nodes");
    }
    TEST_EXPECT_EQ(clone_left, 0, "the clone asked for its nodes");
    TEST_ASSERT(clone_failed_whole, "a clone that cannot allocate fails whole, leaving the parent as it was");
    TEST_EXPECT_EQ(control_plain, 0, "control: the same mount with the knob off installs");
    TEST_EXPECT_EQ(control_union, 0, "control: so does the union's start");
    TEST_ASSERT(control_clone, "control: and the clone copies all four entries");
}
