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
//   territory.pivot_root_keeps_reachable_mount
//     A mount whose point lies in a device tree reachable from the NEW root
//     survives the pivot with its ref intact. (Until ARCH 9.6.10 this test was
//     "pivot does not touch mounts" -- true then, and the cause of #80.)
//
// The mount-table SHED (ARCH 9.6.10; specs/territory_shed.tla). A device
// instance is modelled by a Spoor's devno: test Spoors are all devnone, and
// the shed keys on (dc, devno).
//
//   territory.shed_pivot_drops_unreachable
//     An entry keyed in the OLD root's tree is dropped at the pivot, its
//     source ref released exactly once. territory.shed_chroot_drops_unreachable
//     is the chroot twin.
//   territory.shed_keeps_transitive
//     root -> X (mounted in root's tree) -> Y (mounted inside X's tree): both
//     survive, in order, while an orphan between them goes. The non-transitive
//     rule (keep only the new root's own tree) would drop Y -- the spec's
//     buggy_nontransitive cfg, here against the real code.
//   territory.shed_per_walker_dev_matched_on_dc
//     A mount point INSIDE a devno_per_walker Dev (devenv) carries the
//     walker's devno, not its mount source's; it must be kept on dc alone.
//     The control one variable away -- the same shape on a Dev without the
//     flag -- must be shed, or the test would pass on a shed that keeps
//     everything.
//   territory.shed_same_root_is_a_noop
//     Pivot / chroot to the CURRENT root swaps nothing and sheds nothing.
//   territory.shed_preserves_union_order
//     Two members of one kept point, with an orphan between them in the
//     array: after the shed the search order is still member 0, member 1.
//   territory.shed_clone_before_pivot_unaffected
//     I-1: a child cloned before the parent pivots keeps its whole table.
//   territory.shed_union_root_keeps_point_entries
//     A root that is a UNION handle (member[0]'s identity + the mount point in
//     union_snap) keeps the entries keyed at the point, which lives in ANOTHER
//     tree. The control one variable away -- the same identity with no snap --
//     sheds them. (Audit r1 F1; the spec's buggy_no_union_seed.)
//   territory.shed_drops_nested_orphan
//     An orphan keyed in a tree that hangs off the OLD tree (not in the old
//     root's own instance) goes too: the rule is "unreachable from the new
//     root", not "keyed in the old root".
//   territory.shed_full_table_boundary
//     32 chained entries under a union root: reach[] lands exactly on its
//     capacity (2 seeds + 32 sources), every entry survives, nothing overflows.
//   territory.shed_releases_mp_path_once
//     Mount points with REAL Paths: a shed entry's mp_path ref is released
//     exactly once (a NULL mp_path makes path_unref vacuous), witnessed by the
//     ref count and by the allocator balance.
//   territory.shed_initial_chroot_and_root_as_source
//     The first chroot (old root NULL) sheds like any other, and a new root
//     that is itself the SOURCE of a shed entry keeps exactly its root ref.

#include "test.h"

#include <thylacine/dev.h>
#include <thylacine/path.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

#include "../../mm/slub.h"       // kmalloc: a point-only union_snap

extern struct Dev devnone;
extern struct Dev devenv;

void test_territory_pivot_root_smoke(void);
void test_territory_pivot_root_rejects_no_initial_root(void);
void test_territory_pivot_root_idempotent_same_spoor(void);
void test_territory_pivot_root_null_source_rejected(void);
void test_territory_pivot_root_keeps_reachable_mount(void);
void test_territory_shed_pivot_drops_unreachable(void);
void test_territory_shed_chroot_drops_unreachable(void);
void test_territory_shed_keeps_transitive(void);
void test_territory_shed_per_walker_dev_matched_on_dc(void);
void test_territory_shed_same_root_is_a_noop(void);
void test_territory_shed_preserves_union_order(void);
void test_territory_shed_clone_before_pivot_unaffected(void);
void test_territory_shed_union_root_keeps_point_entries(void);
void test_territory_shed_drops_nested_orphan(void);
void test_territory_shed_full_table_boundary(void);
void test_territory_shed_releases_mp_path_once(void);
void test_territory_shed_initial_chroot_and_root_as_source(void);
void test_territory_shed_covered_shares_fate(void);       // Plan 9 unions

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
// pivot_root_keeps_reachable_mount
// =============================================================================

void test_territory_pivot_root_keeps_reachable_mount(void) {
    struct Territory *p = territory_alloc();
    TEST_ASSERT(p != NULL, "territory_alloc returned NULL");

    struct Spoor *root_a = spoor_alloc(&devnone);
    struct Spoor *root_b = spoor_alloc(&devnone);
    struct Spoor *mounted = spoor_alloc(&devnone);
    struct Spoor *mp = spoor_alloc(&devnone);   // stalk-2: mount-point identity
    TEST_ASSERT(root_a && root_b && mounted && mp, "spoor_alloc");
    mp->qid.path = 42u;                         // distinct mount-point identity
    // Every Spoor here shares devnone's (dc, devno 0): ONE device instance, so
    // the mount point is in the new root's own tree and must survive.

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(p, mounted, mp, MREPL), 0,
        "mount installs at mount point (qid.path 42) with MREPL");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "1 mount entry");
    TEST_EXPECT_EQ(mounted->ref, 2, "mounted ref = 2 (test + mount entry)");

    TEST_EXPECT_EQ(territory_pivot_root(p, root_b), 0, "pivot to B");
    TEST_EXPECT_EQ(territory_nmounts(p), 1,
        "a mount point in a tree reachable from the new root survives the pivot");
    TEST_EXPECT_EQ(mounted->ref, 2, "its source ref is untouched");

    territory_unref(p);
    TEST_EXPECT_EQ(mounted->ref, 1, "mount-entry ref dropped at destroy");
    TEST_EXPECT_EQ(root_b->ref, 1, "B root ref dropped at destroy");
    TEST_EXPECT_EQ(root_a->ref, 1, "A ref unchanged (was dropped at pivot)");

    spoor_unref(root_a);
    spoor_unref(root_b);
    spoor_unref(mounted);
    spoor_unref(mp);
}

// =============================================================================
// The shed (ARCH 9.6.10)
// =============================================================================

// A devnone Spoor standing for a directory in device instance `devno`.
static struct Spoor *shed_spoor(u32 devno, u64 qid_path) {
    struct Spoor *s = spoor_alloc(&devnone);
    if (s) { s->devno = devno; s->qid.path = qid_path; }
    return s;
}

static void shed_drops_unreachable(bool via_chroot) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_a   = shed_spoor(1, 10);   // a directory in A's tree
    struct Spoor *src    = shed_spoor(5, 0);
    TEST_ASSERT(p && root_a && root_b && mp_a && src, "alloc");

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(p, src, mp_a, 0), 0, "mount in A's tree");
    TEST_EXPECT_EQ(src->ref, 2, "src ref = 2 (test + entry)");

    int rc = via_chroot ? territory_chroot(p, root_b) : territory_pivot_root(p, root_b);
    TEST_EXPECT_EQ(rc, 0, "root swap to B (a different device instance)");
    TEST_EXPECT_EQ(territory_nmounts(p), 0,
        "the entry keyed in the old tree is unreachable from B and is shed");
    TEST_EXPECT_EQ(src->ref, 1, "its source ref was released exactly once");

    // The slot is genuinely back: the table accepts a full complement again.
    struct Spoor *mp_b = shed_spoor(2, 11);
    TEST_ASSERT(mp_b != NULL, "alloc");
    TEST_EXPECT_EQ(mount(p, src, mp_b, 0), 0, "a mount in B's tree still installs");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "1 entry");

    territory_unref(p);
    TEST_EXPECT_EQ(src->ref, 1, "destroy drops the surviving entry's ref");
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_a);
    spoor_unref(mp_b);   spoor_unref(src);
}

void test_territory_shed_pivot_drops_unreachable(void)  { shed_drops_unreachable(false); }
void test_territory_shed_chroot_drops_unreachable(void) { shed_drops_unreachable(true); }

void test_territory_shed_keeps_transitive(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_b = shed_spoor(2, 20);     // a dir in B's tree      (/dev)
    struct Spoor *x    = shed_spoor(3, 0);      // tree X mounted there   (devdev)
    struct Spoor *mp_x = shed_spoor(3, 30);     // a dir INSIDE X         (/dev/pts)
    struct Spoor *y    = shed_spoor(4, 0);      // tree Y mounted in X    (ptyfs)
    struct Spoor *mp_a = shed_spoor(1, 10);     // a dir in the OLD tree
    struct Spoor *z    = shed_spoor(6, 0);      // the orphan-to-be
    TEST_ASSERT(p && root_a && root_b && mp_b && x && mp_x && y && mp_a && z, "alloc");

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    // Array order on purpose: X, then the orphan, then Y -- so the compaction
    // has to close a gap BETWEEN two survivors.
    TEST_EXPECT_EQ(mount(p, x, mp_b, 0), 0, "mount X in B's tree");
    TEST_EXPECT_EQ(mount(p, z, mp_a, 0), 0, "mount Z in A's tree");
    TEST_EXPECT_EQ(mount(p, y, mp_x, 0), 0, "mount Y inside X's tree");
    TEST_EXPECT_EQ(territory_nmounts(p), 3, "3 entries");

    TEST_EXPECT_EQ(territory_pivot_root(p, root_b), 0, "pivot to B");
    TEST_EXPECT_EQ(territory_nmounts(p), 2,
        "X and the mount NESTED inside X survive; the old-tree orphan goes");
    TEST_EXPECT_EQ(x->ref, 2, "X kept");
    TEST_EXPECT_EQ(y->ref, 2,
        "Y kept: its point is in X's tree, reachable only THROUGH X (transitive)");
    TEST_EXPECT_EQ(z->ref, 1, "Z shed");

    // Both survivors still resolve (the compaction moved Y's entry down a slot).
    struct Spoor *gx = mount_lookup(p, mp_b, NULL);
    struct Spoor *gy = mount_lookup(p, mp_x, NULL);
    TEST_EXPECT_EQ(gx == x, true, "lookup at B's dir still crosses to X");
    TEST_EXPECT_EQ(gy == y, true, "lookup inside X still crosses to Y");
    if (gx) spoor_clunk(gx);
    if (gy) spoor_clunk(gy);

    territory_unref(p);
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_b); spoor_unref(x);
    spoor_unref(mp_x);   spoor_unref(y);      spoor_unref(mp_a); spoor_unref(z);
}

// One nested mount whose point carries a devno DIFFERENT from its parent
// source's. `pt_dev` is the Dev the parent tree belongs to.
static int shed_nested_mismatch_survivors(struct Dev *pt_dev) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_b = shed_spoor(2, 20);
    struct Spoor *tree = spoor_alloc(pt_dev);      // the tree mounted at mp_b
    struct Spoor *mp_t = spoor_alloc(pt_dev);      // a point inside it...
    struct Spoor *leaf = shed_spoor(7, 0);
    if (!p || !root_a || !root_b || !mp_b || !tree || !mp_t || !leaf) return -1;
    tree->devno = 40;                              // the mount source's devno
    mp_t->devno = 41;                              // ...stamped with ANOTHER one
    mp_t->qid.path = 5;

    int n = -1;
    if (territory_chroot(p, root_a) == 0 &&
        mount(p, tree, mp_b, 0) == 0 &&
        mount(p, leaf, mp_t, 0) == 0 &&
        territory_pivot_root(p, root_b) == 0)
        n = territory_nmounts(p);

    territory_unref(p);
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_b);
    spoor_unref(tree);   spoor_unref(mp_t);   spoor_unref(leaf);
    return n;
}

void test_territory_shed_per_walker_dev_matched_on_dc(void) {
    TEST_EXPECT_EQ(devenv.devno_per_walker, true,
        "devenv declares that its walk re-stamps devno");
    TEST_EXPECT_EQ(shed_nested_mismatch_survivors(&devenv), 2,
        "a point inside a devno_per_walker Dev is kept on dc alone");
    // The control, one variable away: same shape, a Dev WITHOUT the flag. The
    // inner point's instance (dc, 41) is not the mounted tree's (dc, 40), so it
    // is unreachable and must go -- otherwise the assertion above would also be
    // satisfied by a shed that keeps everything.
    TEST_EXPECT_EQ(shed_nested_mismatch_survivors(&devnone), 1,
        "without the flag the same mismatch is unreachable and is shed");
}

void test_territory_shed_same_root_is_a_noop(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0);
    struct Spoor *mp_far = shed_spoor(9, 10);   // unreachable from A already
    struct Spoor *src    = shed_spoor(5, 0);
    TEST_ASSERT(p && root_a && mp_far && src, "alloc");

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(p, src, mp_far, 0), 0,
        "mount() itself never sheds: an unreachable point still installs");
    TEST_EXPECT_EQ(territory_pivot_root(p, root_a), 0, "pivot to the CURRENT root");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "no swap happened, so nothing is shed");
    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to the CURRENT root");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "likewise");
    TEST_EXPECT_EQ(src->ref, 2, "entry ref intact");

    territory_unref(p);
    spoor_unref(root_a); spoor_unref(mp_far); spoor_unref(src);
}

void test_territory_shed_preserves_union_order(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_b = shed_spoor(2, 20);
    struct Spoor *mp_a = shed_spoor(1, 10);
    struct Spoor *u0 = shed_spoor(3, 0), *u1 = shed_spoor(4, 0), *z = shed_spoor(6, 0);
    TEST_ASSERT(p && root_a && root_b && mp_b && mp_a && u0 && u1 && z, "alloc");

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(p, u0, mp_b, 0), 0,      "union member 0");
    TEST_EXPECT_EQ(mount(p, z,  mp_a, 0), 0,      "an orphan BETWEEN the members");
    TEST_EXPECT_EQ(mount(p, u1, mp_b, MAFTER), 0, "union member 1");

    TEST_EXPECT_EQ(territory_pivot_root(p, root_b), 0, "pivot to B");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "the group survives whole");
    struct Spoor *m0 = mount_member_at(p, mp_b, 0, NULL);
    struct Spoor *m1 = mount_member_at(p, mp_b, 1, NULL);
    TEST_EXPECT_EQ(m0 == u0, true, "search order: member 0 first");
    TEST_EXPECT_EQ(m1 == u1, true, "search order: member 1 second");
    if (m0) spoor_clunk(m0);
    if (m1) spoor_clunk(m1);

    territory_unref(p);
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_b); spoor_unref(mp_a);
    spoor_unref(u0);     spoor_unref(u1);     spoor_unref(z);
}

void test_territory_shed_clone_before_pivot_unaffected(void) {
    struct Territory *parent = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_a = shed_spoor(1, 10), *src = shed_spoor(5, 0);
    TEST_ASSERT(parent && root_a && root_b && mp_a && src, "alloc");

    TEST_EXPECT_EQ(territory_chroot(parent, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(parent, src, mp_a, 0), 0, "mount in A's tree");
    struct Territory *child = territory_clone(parent);
    TEST_ASSERT(child != NULL, "territory_clone");
    TEST_EXPECT_EQ(src->ref, 3, "src ref = 3 (test + parent entry + child entry)");

    TEST_EXPECT_EQ(territory_pivot_root(parent, root_b), 0, "the PARENT pivots");
    TEST_EXPECT_EQ(territory_nmounts(parent), 0, "parent: shed");
    TEST_EXPECT_EQ(territory_nmounts(child), 1,
        "I-1: the child's table is its own; its root is still A, its mount still live");
    TEST_EXPECT_EQ(src->ref, 2, "only the parent's entry ref was released");

    territory_unref(child);
    territory_unref(parent);
    TEST_EXPECT_EQ(src->ref, 1, "all entry refs gone");
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_a); spoor_unref(src);
}

// A Spoor with `identity`'s instance that carries `point` as a POINT-ONLY union
// snapshot -- what an O_PATH open of a union directory yields (stalk.c
// union_snap_point_only). spoor_free_internal releases the snap and its point
// ref. `point` NULL = the control: same identity, no snap.
static struct Spoor *shed_union_handle(u32 devno, struct Spoor *point) {
    struct Spoor *s = shed_spoor(devno, 0);
    if (!s || !point) return s;
    struct union_snap *snap = kmalloc(sizeof(*snap), 0);
    if (!snap) { spoor_unref(s); return NULL; }
    snap->point = point;
    spoor_ref(point);
    snap->n = 0;
    s->union_snap = snap;
    return s;
}

// The default image's /bin in miniature: a point in A's tree with two members,
// then a chroot INTO the union. Returns the surviving entry count.
static int shed_union_root_survivors(bool with_snap) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0);
    struct Spoor *point  = shed_spoor(1, 50);    // /bin, a dir in A's tree
    struct Spoor *m0 = shed_spoor(3, 0), *m1 = shed_spoor(4, 0);
    struct Spoor *mp_w = shed_spoor(9, 10), *w = shed_spoor(8, 0);   // an orphan
    struct Spoor *uroot = shed_union_handle(3, with_snap ? point : NULL);
    if (!p || !root_a || !point || !m0 || !m1 || !mp_w || !w || !uroot) return -1;

    int n = -1;
    if (territory_chroot(p, root_a) == 0 &&
        mount(p, m0, point, 0) == 0 &&
        mount(p, w,  mp_w,  0) == 0 &&
        mount(p, m1, point, MAFTER) == 0 &&
        territory_chroot(p, uroot) == 0) {
        n = territory_nmounts(p);
        if (with_snap) {
            // Not just counted: the members are still found AT THE POINT, in
            // order -- which is what stalk_union_child asks for.
            struct Spoor *g0 = mount_member_at(p, point, 0, NULL);
            struct Spoor *g1 = mount_member_at(p, point, 1, NULL);
            if (g0 != m0 || g1 != m1) n = -2;
            if (g0) spoor_clunk(g0);
            if (g1) spoor_clunk(g1);
        }
    }

    territory_unref(p);
    spoor_unref(uroot);
    spoor_unref(root_a); spoor_unref(point); spoor_unref(m0); spoor_unref(m1);
    spoor_unref(mp_w);   spoor_unref(w);
    return n;
}

void test_territory_shed_union_root_keeps_point_entries(void) {
    TEST_EXPECT_EQ(shed_union_root_survivors(true), 2,
        "a union root keeps both entries keyed at its point; the orphan goes");
    // The control: the same root identity WITHOUT the snap reaches nothing, so
    // everything is shed -- otherwise the arm above would also be satisfied by
    // a shed that keeps everything.
    TEST_EXPECT_EQ(shed_union_root_survivors(false), 0,
        "without the point the same identity reaches no entry");
}

void test_territory_shed_drops_nested_orphan(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_a = shed_spoor(1, 10), *z = shed_spoor(6, 0);   // Z in A
    struct Spoor *mp_z = shed_spoor(6, 60), *q = shed_spoor(7, 0);   // Q in Z
    TEST_ASSERT(p && root_a && root_b && mp_a && z && mp_z && q, "alloc");

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(p, z, mp_a, 0), 0, "mount Z in A's tree");
    TEST_EXPECT_EQ(mount(p, q, mp_z, 0), 0, "mount Q inside Z's tree");
    TEST_EXPECT_EQ(territory_pivot_root(p, root_b), 0, "pivot to B");
    TEST_EXPECT_EQ(territory_nmounts(p), 0,
        "Q is keyed in Z's instance, not the old root's, and is unreachable all the same");
    TEST_EXPECT_EQ(z->ref, 1, "Z released once");
    TEST_EXPECT_EQ(q->ref, 1, "Q released once");

    territory_unref(p);
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_a); spoor_unref(z);
    spoor_unref(mp_z);   spoor_unref(q);
}

void test_territory_shed_full_table_boundary(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0);
    struct Spoor *stale_pt = shed_spoor(90, 1);   // a point in its own tree: seed 2
    struct Spoor *uroot = shed_union_handle(100, stale_pt);
    struct Spoor *mp[PGRP_MAX_MOUNTS], *src[PGRP_MAX_MOUNTS];
    TEST_ASSERT(p && root_a && stale_pt && uroot, "alloc");

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    // Entry i grafts tree 101+i at a directory of tree 100+i: a chain hanging
    // off the new root, every source a NEW instance.
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) {
        mp[i]  = shed_spoor(100 + (u32)i, 7);
        src[i] = shed_spoor(101 + (u32)i, 0);
        TEST_ASSERT(mp[i] && src[i], "alloc chain link");
        TEST_EXPECT_EQ(mount(p, src[i], mp[i], 0), 0, "chain link installs");
    }
    TEST_EXPECT_EQ(territory_nmounts(p), PGRP_MAX_MOUNTS, "the table is full");

    TEST_EXPECT_EQ(territory_pivot_root(p, uroot), 0,
        "pivot: the closure holds 2 seeds + 32 sources, exactly its capacity");
    TEST_EXPECT_EQ(territory_nmounts(p), PGRP_MAX_MOUNTS, "every link is reachable and kept");
    struct Spoor *last = mount_lookup(p, mp[PGRP_MAX_MOUNTS - 1], NULL);
    TEST_EXPECT_EQ(last == src[PGRP_MAX_MOUNTS - 1], true, "the deepest link still crosses");
    if (last) spoor_clunk(last);

    territory_unref(p);
    for (int i = 0; i < PGRP_MAX_MOUNTS; i++) { spoor_unref(mp[i]); spoor_unref(src[i]); }
    spoor_unref(uroot); spoor_unref(stale_pt); spoor_unref(root_a);
}

void test_territory_shed_releases_mp_path_once(void) {
    u64 pa0 = path_total_allocated(), pf0 = path_total_freed();
    struct Path *rootp = path_make_root();
    TEST_ASSERT(rootp != NULL, "path_make_root");

    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_a = shed_spoor(1, 10), *mp_b = shed_spoor(2, 20);
    struct Spoor *gone = shed_spoor(5, 0),  *kept = shed_spoor(6, 0);
    TEST_ASSERT(p && root_a && root_b && mp_a && mp_b && gone && kept, "alloc");
    mp_a->path = path_addelem(rootp, "old", 3);     // owned by the Spoor
    mp_b->path = path_addelem(rootp, "new", 3);
    TEST_ASSERT(mp_a->path && mp_b->path, "path_addelem");

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(p, gone, mp_a, 0), 0, "mount in A's tree");
    TEST_EXPECT_EQ(mount(p, kept, mp_b, 0), 0, "mount in B's tree");
    TEST_EXPECT_EQ(mp_a->path->ref, 2, "mp_path ref = Spoor + entry");
    TEST_EXPECT_EQ(mp_b->path->ref, 2, "likewise");

    TEST_EXPECT_EQ(territory_pivot_root(p, root_b), 0, "pivot to B");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "one shed, one kept");
    TEST_EXPECT_EQ(mp_a->path->ref, 1, "the shed entry released its mp_path ref exactly once");
    TEST_EXPECT_EQ(mp_b->path->ref, 2, "the survivor's moved with it, untouched");

    territory_unref(p);
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_a); spoor_unref(mp_b);
    spoor_unref(gone);   spoor_unref(kept);
    path_unref(rootp);
    TEST_EXPECT_EQ(path_total_allocated() - pa0, path_total_freed() - pf0,
        "every Path allocated here was freed: no leaked mp_path ref");
}

void test_territory_shed_initial_chroot_and_root_as_source(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_b = shed_spoor(2, 0);
    struct Spoor *mp_far = shed_spoor(9, 10);     // a tree B cannot reach
    struct Spoor *mp_b   = shed_spoor(2, 20), *src = shed_spoor(5, 0);
    TEST_ASSERT(p && root_b && mp_far && mp_b && src, "alloc");

    // Mounts exist BEFORE any root does; B itself is the source of one of them.
    TEST_EXPECT_EQ(mount(p, root_b, mp_far, 0), 0, "B grafted somewhere unreachable");
    TEST_EXPECT_EQ(mount(p, src,    mp_b,   0), 0, "a mount in B's own tree");
    TEST_EXPECT_EQ(root_b->ref, 2, "B: test + entry");

    TEST_EXPECT_EQ(territory_chroot(p, root_b), 0, "the FIRST chroot (old root NULL)");
    TEST_EXPECT_EQ(territory_nmounts(p), 1, "it sheds like any other swap");
    TEST_EXPECT_EQ(root_b->ref, 2, "B: test + root (the shed entry's ref went, the root's came)");
    TEST_EXPECT_EQ(src->ref, 2, "the reachable entry is intact");

    territory_unref(p);
    TEST_EXPECT_EQ(root_b->ref, 1, "destroy drops the root ref");
    spoor_unref(root_b); spoor_unref(mp_far); spoor_unref(mp_b); spoor_unref(src);
}

// Plan 9 unions: the covered directory's entry is keyed at its point and its
// source IS the point, a self-edge, so it shares the fate of the point's other
// entries. A union in A's tree goes whole at a pivot to B, and the point's own
// reference is released once; a union in B's tree survives whole, still in
// search order.
void test_territory_shed_covered_shares_fate(void) {
    struct Territory *p = territory_alloc();
    struct Spoor *root_a = shed_spoor(1, 0), *root_b = shed_spoor(2, 0);
    struct Spoor *mp_a = shed_spoor(1, 10), *mp_b = shed_spoor(2, 20);
    struct Spoor *u = shed_spoor(3, 0), *v = shed_spoor(4, 0);
    TEST_ASSERT(p && root_a && root_b && mp_a && mp_b && u && v, "alloc");
    mp_a->qid.type = QTDIR;   // a union starts only at a directory
    mp_b->qid.type = QTDIR;

    TEST_EXPECT_EQ(territory_chroot(p, root_a), 0, "chroot to A");
    TEST_EXPECT_EQ(mount(p, u, mp_a, MBEFORE), 0, "a union in A's tree: [u, covered]");
    TEST_EXPECT_EQ(mount(p, v, mp_b, MAFTER),  0, "a union in B's tree: [covered, v]");
    TEST_EXPECT_EQ(territory_nmounts(p), 4, "two members + two covered entries");
    TEST_EXPECT_EQ(mp_a->ref, 2, "A's covered entry holds its point");
    TEST_EXPECT_EQ(mp_b->ref, 2, "B's covered entry holds its point");

    TEST_EXPECT_EQ(territory_pivot_root(p, root_b), 0, "pivot to B");
    TEST_EXPECT_EQ(territory_nmounts(p), 2, "B's union survives whole; A's goes whole");
    TEST_EXPECT_EQ(mp_a->ref, 1, "A's covered entry released its point once");
    TEST_EXPECT_EQ(u->ref, 1, "A's member released once");
    TEST_EXPECT_EQ(mp_b->ref, 2, "B's covered entry kept");
    u32 f = 0;
    struct Spoor *g0 = mount_member_at(p, mp_b, 0, &f);
    TEST_EXPECT_EQ(g0 == mp_b && f == MCOVERED, true, "B's member 0 is still its covered directory");
    if (g0) spoor_clunk(g0);
    struct Spoor *g1 = mount_member_at(p, mp_b, 1, NULL);
    TEST_EXPECT_EQ(g1 == v, true, "then v");
    if (g1) spoor_clunk(g1);

    territory_unref(p);
    TEST_EXPECT_EQ(mp_b->ref, 1, "destroy drops the surviving covered entry's ref");
    spoor_unref(root_a); spoor_unref(root_b); spoor_unref(mp_a); spoor_unref(mp_b);
    spoor_unref(u);      spoor_unref(v);
}
