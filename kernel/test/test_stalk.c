// stalk resolver tests (stalk-1; A-5b-0; docs/STALK-DESIGN.md; invariant I-28).
//
// devramfs is a FLAT namespace (every file is a direct child of root), so it
// cannot exercise multi-component resolution, '..', or a mid-path X-search
// denial. These tests run against a small in-file fixture Dev (`stalkfix`) with
// a nested tree + varied perms + a self-referential node for the depth cap. The
// fixture is qid-based (no heap aux, like devramfs) so trail clunks are safe.
//
// Tree (uid/gid = PRINCIPAL_SYSTEM; the synthetic Proc is the SYSTEM owner, so
// the owner rwx bits decide -- no CAP_HOSTOWNER, so perm_check enforces):
//
//   / (0, 0755)
//   |-- a (1, 0755)
//   |   |-- b (2, 0644, file)
//   |   `-- deep (3, 0755)
//   |       `-- leaf (4, 0640, file)
//   |-- nox (5, 0644)            <- owner rw- (no x) -> X-search denies traversal
//   |   `-- sekret (6, 0600, file, unreachable through nox)
//   `-- loop (7, 0755)           <- "loop/loop/loop/..." is self-referential
//                                   (walk "loop" from node 7 -> node 7) to drive
//                                   the STALK_MAX_DEPTH cap.
//
// Coverage note (stalk-1 audit F2): the resolver's reuse-nc-contract-violation
// cleanup branch (a Dev.walk returning w->spoor != nc) is defense-in-depth
// inherited verbatim from the audited sys_walk_open_handler (F4) -- no real Dev
// violates the contract, and this fixture honors it, so that branch is not
// independently exercised. The dev9p-specific `nqid != 1` branch is likewise
// unreachable (dev9p returns NULL on a partial walk); the devramfs-shaped miss
// (nqid == 0) is covered by stalk.missing_component.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/perm.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>
#include <thylacine/syscall.h>   // struct t_stat, T_S_IFDIR/IFREG
#include <thylacine/types.h>

// Forward declarations (registered in kernel/test/test.c).
void test_stalk_resolve_multi(void);
void test_stalk_resolve_deep(void);
void test_stalk_leading_and_double_slash(void);
void test_stalk_dot_noop(void);
void test_stalk_dotdot_pop(void);
void test_stalk_dotdot_containment(void);
void test_stalk_xsearch_deny(void);
void test_stalk_missing_component(void);
void test_stalk_opath_no_open(void);
void test_stalk_open_root(void);
void test_stalk_depth_cap(void);
void test_stalk_lifetime_no_leak(void);

// =============================================================================
// The fixture Dev.
// =============================================================================

struct fixnode {
    u64         path;
    u64         parent;
    const char *name;
    u8          type;   // QTDIR | QTFILE
    u32         mode;   // low 9 rwx bits (the X-search reads owner bits)
};

static const struct fixnode g_fix[] = {
    { 0, 0, "/",      QTDIR,  0755u },
    { 1, 0, "a",      QTDIR,  0755u },
    { 2, 1, "b",      QTFILE, 0644u },
    { 3, 1, "deep",   QTDIR,  0755u },
    { 4, 3, "leaf",   QTFILE, 0640u },
    { 5, 0, "nox",    QTDIR,  0644u },
    { 6, 5, "sekret", QTFILE, 0600u },
    { 7, 0, "loop",   QTDIR,  0755u },
};
#define FIX_LOOP_PATH 7u

static bool fix_streq(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

static const struct fixnode *fix_node(u64 path) {
    for (unsigned i = 0; i < sizeof(g_fix) / sizeof(g_fix[0]); i++) {
        if (g_fix[i].path == path) return &g_fix[i];
    }
    return NULL;
}

static bool fix_walk_one(u64 cur_path, const char *name, struct Qid *out) {
    out->path = 0; out->vers = 0; out->type = 0;
    out->pad[0] = out->pad[1] = out->pad[2] = 0;

    // Self-referential loop node: walking "loop" from node 7 returns node 7.
    if (cur_path == FIX_LOOP_PATH && fix_streq(name, "loop")) {
        out->path = FIX_LOOP_PATH;
        out->type = QTDIR;
        return true;
    }
    for (unsigned i = 0; i < sizeof(g_fix) / sizeof(g_fix[0]); i++) {
        if (g_fix[i].path == 0) continue;   // root has no name to walk to
        if (g_fix[i].parent == cur_path && fix_streq(name, g_fix[i].name)) {
            out->path = g_fix[i].path;
            out->type = g_fix[i].type;
            return true;
        }
    }
    return false;
}

static struct Walkqid *fix_walk(struct Spoor *c, struct Spoor *nc,
                                const char **name, int nname) {
    if (!c || nname < 0) return NULL;
    struct Walkqid *wq = walkqid_alloc(nname > 0 ? nname : 1);
    if (!wq) return NULL;

    struct Spoor *cur;
    if (nc) { cur = nc; cur->qid = c->qid; }
    else    { cur = spoor_clone(c); if (!cur) { walkqid_free(wq); return NULL; } }

    int n = 0;
    for (int i = 0; i < nname; i++) {
        struct Qid next;
        if (!fix_walk_one(cur->qid.path, name[i], &next)) break;
        cur->qid = next;
        wq->qid[n++] = next;
    }
    wq->spoor = cur;
    wq->nqid  = n;
    return wq;
}

static int fix_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    const struct fixnode *fn = fix_node(c->qid.path);
    if (!fn) return -1;
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;
    out->mode     = ((fn->type & QTDIR) ? T_S_IFDIR : T_S_IFREG) | fn->mode;
    out->nlink    = 1;
    out->qid_path = fn->path;
    out->qid_type = fn->type;
    out->blksize  = 4096;
    out->uid      = PRINCIPAL_SYSTEM;
    out->gid      = GID_SYSTEM;
    return 0;
}

static struct Spoor *fix_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    c->flag |= COPEN;
    c->mode  = omode;
    return c;
}

static void fix_close(struct Spoor *c) { (void)c; /* qid-based: no heap aux */ }

// The fixture Dev. dc is a test-only sentinel; it is NOT dev_register'd, so the
// dc never collides with a real Dev (stalk reaches it only through the Spoors we
// hand it directly).
static struct Dev stalkfix = {
    .dc            = (int)'Z',
    .name          = "stalkfix",
    .perm_enforced = true,
    .attach        = NULL,   // we mint the root via dev_simple_attach(&stalkfix,...)
    .walk          = fix_walk,
    .stat_native   = fix_stat_native,
    .open          = fix_open,
    .close         = fix_close,
};

// Mint the fixture root Spoor (qid.path 0, QTDIR). Caller owns the ref.
static struct Spoor *fix_root(void) {
    return dev_simple_attach(&stalkfix, QTDIR);
}

// A synthetic SYSTEM Proc with no caps -- the owner of every fixture node, so
// perm_check decides on the owner rwx bits (no CAP_HOSTOWNER bypass; I-22).
static void mkproc_system(struct Proc *p) {
    for (size_t i = 0; i < sizeof(*p); i++) ((u8 *)p)[i] = 0;
    p->principal_id   = PRINCIPAL_SYSTEM;
    p->primary_gid    = GID_SYSTEM;
    p->supp_gid_count = 0;
    p->caps           = CAP_NONE;
}

// =============================================================================
// Tests.
// =============================================================================

void test_stalk_resolve_multi(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    struct Spoor *q = stalk(&p, root, "a/b", 3, STALK_OPEN, 0 /*OREAD*/);
    TEST_ASSERT(q != NULL, "resolve a/b");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "a/b -> qid 2 (b)");
    TEST_ASSERT((q->flag & COPEN) != 0, "b is opened (STALK_OPEN)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_resolve_deep(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    struct Spoor *q = stalk(&p, root, "a/deep/leaf", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/leaf");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)4, "a/deep/leaf -> qid 4 (leaf)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_leading_and_double_slash(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // Leading '/' + a '//' both collapse to empty components.
    struct Spoor *q = stalk(&p, root, "/a//b", 5, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve /a//b");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "/a//b -> qid 2 (b)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_dot_noop(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    struct Spoor *q = stalk(&p, root, "a/./b", 5, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/./b");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "a/./b -> qid 2 (b)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_dotdot_pop(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // a/deep/../b : 'deep' is popped (back to a), then b resolves.
    struct Spoor *q = stalk(&p, root, "a/deep/../b", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/../b");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "a/deep/../b -> qid 2 (b)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_dotdot_containment(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // '..' at the base is a no-op (cannot escape above root, I-28): the leading
    // ".." run nets back to root, then "a" resolves from root.
    struct Spoor *q = stalk(&p, root, "../../a", 7, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve ../../a (contained)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)1, "../../a -> qid 1 (a), never escaped");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_xsearch_deny(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // 'nox' is 0644 (owner rw-, no x) -> the per-component X-search denies
    // traversal INTO it, so sekret is unreachable even though it exists.
    struct Spoor *q = stalk(&p, root, "nox/sekret", 10, STALK_OPEN, 0);
    TEST_ASSERT(q == NULL, "nox/sekret denied at the X-search on nox");
    spoor_unref(root);
}

void test_stalk_missing_component(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    struct Spoor *q = stalk(&p, root, "a/nope", 6, STALK_OPEN, 0);
    TEST_ASSERT(q == NULL, "a/nope -> miss -> NULL");
    spoor_unref(root);
}

void test_stalk_opath_no_open(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // STALK_WALK resolves but does NOT open (the O_PATH / walkable-base case).
    struct Spoor *q = stalk(&p, root, "a/deep", 6, STALK_WALK, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep (walk-only)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)3, "a/deep -> qid 3 (deep)");
    TEST_ASSERT((q->flag & COPEN) == 0, "deep is NOT opened (STALK_WALK)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_open_root(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // "/" has zero real components -> the quarry is the base, minted via a
    // clone-walk so it is independently openable (the 0-component path).
    struct Spoor *q = stalk(&p, root, "/", 1, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve / (root)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)0, "/ -> qid 0 (root)");
    TEST_ASSERT((q->flag & COPEN) != 0, "root opened");
    TEST_ASSERT(q != root, "quarry is a distinct Spoor, not the borrowed base");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_depth_cap(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // STALK_MAX_DEPTH + 1 "loop" components (self-referential) overflow the
    // trail cap -> clean NULL, no overrun.
    char path[(STALK_MAX_DEPTH + 1) * 5 + 1];
    u64 n = 0;
    for (int i = 0; i < STALK_MAX_DEPTH + 1; i++) {
        if (i) path[n++] = '/';
        path[n++] = 'l'; path[n++] = 'o'; path[n++] = 'o'; path[n++] = 'p';
    }
    struct Spoor *q = stalk(&p, root, path, n, STALK_WALK, 0);
    TEST_ASSERT(q == NULL, "over-deep loop path -> NULL (depth cap)");
    spoor_unref(root);
}

void test_stalk_lifetime_no_leak(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // Resolve a 3-hop path; every trail ancestor must be clunked and only the
    // quarry survives. After clunking the quarry, the live Spoor count returns
    // to the pre-resolve baseline (root excluded -- it is unref'd after).
    u64 live_before = spoor_total_allocated() - spoor_total_freed();
    struct Spoor *q = stalk(&p, root, "a/deep/leaf", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/leaf");
    spoor_clunk(q);
    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before, "no Spoor leak across resolve+clunk");

    // The denial path must also balance (the trail unwinds on failure).
    live_before = spoor_total_allocated() - spoor_total_freed();
    struct Spoor *qd = stalk(&p, root, "nox/sekret", 10, STALK_OPEN, 0);
    TEST_ASSERT(qd == NULL, "denied resolve -> NULL");
    live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before, "no Spoor leak across a denied resolve");

    spoor_unref(root);
}
