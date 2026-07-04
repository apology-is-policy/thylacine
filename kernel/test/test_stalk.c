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
#include <thylacine/errno.h>    // errno-rollout: T_E_NOENT / T_E_ACCES assertions
#include <thylacine/path.h>     // #66: quarry->path assertions
#include <thylacine/perm.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>
#include <thylacine/syscall.h>   // struct t_stat, T_S_IFDIR/IFREG
#include <thylacine/territory.h> // stalk-2: mount / unmount / territory_alloc
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
void test_stalk_open_replace(void);
void test_stalk_depth_cap(void);
void test_stalk_lifetime_no_leak(void);
// stalk-2 cross-mount (Plan 9 domount).
void test_stalk_cross_mount(void);
void test_stalk_cross_mount_final_quarry(void);
void test_stalk_cross_mount_xsearch_deny(void);
void test_stalk_mount_amode_no_cross(void);
void test_stalk_cross_mount_chain(void);
void test_stalk_cross_mount_no_leak(void);
// #66: namespace-name accumulation through the real resolver.
void test_stalk_path_accumulate(void);
void test_stalk_path_dotdot(void);
void test_stalk_path_cross_transplant(void);
void test_stalk_path_adopt_transplant(void);   // #66 F2 (owed from #66a)

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

// fix_open_replace -- an open that returns a DISTINCT owned Spoor, mirroring
// devsrv open=connect (opening a /srv/<name> node yields a different connection-
// endpoint Spoor: a dev9p root for 9p-mode, a byte-conn Spoor for byte-mode).
// Mints a fresh clone stamped with a marker; leaves c's ref untouched (stalk
// clunks the spent quarry). Drives the stalk-3b-β STALK_OPEN open-returns-a-new-
// Spoor branch (stalk.c). dev9p / devramfs / fix_open return c in place instead.
static struct Spoor *fix_open_replace(struct Spoor *c, int omode) {
    if (!c) return NULL;
    struct Spoor *rep = spoor_clone(c);
    if (!rep) return NULL;
    rep->flag    |= COPEN;
    rep->mode     = omode;
    rep->qid.vers = 0xBEEFu;   // marker proving the returned Spoor != the quarry
    return rep;
}

static struct Dev stalkfix_replace = {
    .dc            = (int)'Y',
    .name          = "stalkfix_replace",
    .perm_enforced = true,
    .attach        = NULL,
    .walk          = fix_walk,
    .stat_native   = fix_stat_native,
    .open          = fix_open_replace,
    .close         = fix_close,
};

static struct Spoor *fix_root_replace(void) {
    return dev_simple_attach(&stalkfix_replace, QTDIR);
}

// #66 F2: a replacement-open whose result carries NO namespace name -- the
// FAITHFUL devsrv open=connect shape (devsrv mints a FRESH endpoint Spoor via
// devsrv_attach / p9_attached_root_spoor, which has its own attach-seed path
// "/" or NULL, NEVER the quarry's "/srv/<name>"). fix_open_replace above clones
// the quarry (sharing its path), so it canNOT prove the adoption-arm transplant;
// this one drops the path so the test is NON-VACUOUS: without the F2 transplant
// the adopted Spoor's name would be NULL, not the walked path.
static struct Spoor *fix_open_replace_nopath(struct Spoor *c, int omode) {
    if (!c) return NULL;
    struct Spoor *rep = spoor_clone(c);
    if (!rep) return NULL;
    rep->flag    |= COPEN;
    rep->mode     = omode;
    rep->qid.vers = 0xBEEFu;
    if (rep->path) { path_unref(rep->path); rep->path = NULL; }   // a nameless mint
    return rep;
}

static struct Dev stalkfix_replace_nopath = {
    .dc            = (int)'Z',
    .name          = "stalkfix_replace_nopath",
    .perm_enforced = true,
    .attach        = NULL,
    .walk          = fix_walk,
    .stat_native   = fix_stat_native,
    .open          = fix_open_replace_nopath,
    .close         = fix_close,
};

static struct Spoor *fix_root_replace_nopath(void) {
    return dev_simple_attach(&stalkfix_replace_nopath, QTDIR);
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

// errno-rollout (ER-1): stalk_err writes the cause so SYS_OPEN returns the real
// -errno. The keystone: a missing path -> -T_E_NOENT (Go's os.IsNotExist true ->
// the O_CREATE create-or-open fallback fires) instead of the bare -1 (Go's
// Linux-shaped decode renders that EPERM, "operation not permitted"). A denial
// reports T_E_ACCES, NEVER T_E_PERM (== 1 == the generic -1 sentinel).
void test_stalk_err_codes(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    int e;

    // walk-miss -> T_E_NOENT (the Go os.IsNotExist keystone).
    e = -12345;
    struct Spoor *miss = stalk_err(&p, root, "a/nope", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(miss == NULL, "a/nope -> miss -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "miss reports T_E_NOENT (not the generic -1)");

    // X-search denial -> T_E_ACCES (a permission failure; owner-first denies the
    // 0644 nox even to SYSTEM, and ACCES != T_E_PERM/-1).
    e = -12345;
    struct Spoor *deny = stalk_err(&p, root, "nox/sekret", 10, STALK_OPEN, 0, &e);
    TEST_ASSERT(deny == NULL, "nox/sekret -> X-search denied -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "denial reports T_E_ACCES");

    // success leaves the quarry openable (errp value irrelevant on success).
    struct Spoor *ok = stalk_err(&p, root, "a/b", 3, STALK_OPEN, 0, &e);
    TEST_ASSERT(ok != NULL, "a/b resolves");
    spoor_clunk(ok);

    // the wrapper stalk() == stalk_err(..., NULL): a NULL errp must not fault.
    struct Spoor *wrap = stalk(&p, root, "a/nope", 6, STALK_OPEN, 0);
    TEST_ASSERT(wrap == NULL, "stalk() wrapper (errp==NULL) resolves the miss to NULL, no fault");

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

// stalk-3b-β: Dev.open may RETURN A DIFFERENT Spoor (devsrv open=connect). The
// resolver must adopt the replacement, clunk the spent quarry, and not leak.
void test_stalk_open_replace(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root_replace();
    TEST_ASSERT(root != NULL, "fix_root_replace");

    u64 live_before = spoor_total_allocated() - spoor_total_freed();
    struct Spoor *q = stalk(&p, root, "a/b", 3, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve+open a/b");
    TEST_EXPECT_EQ((u64)q->qid.vers, (u64)0xBEEFu,
                   "open returned the marked replacement Spoor (opened != quarry)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2,
                   "replacement carries the walked node's qid.path (b == 2)");
    TEST_ASSERT((q->flag & COPEN) != 0, "replacement is opened");
    spoor_clunk(q);
    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before,
                   "no leak: the spent quarry was clunked, the replacement adopted");

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

// =============================================================================
// stalk-2: cross-mount (Plan 9 domount). The fixture is one Dev instance
// (devno 0); the (dc, devno) axis is constant, so these prove the QID-keyed
// cross + the on-descent/quarry/STALK_MOUNT behavior + the chain + lifetime.
// The devno DISAMBIGUATION axis (two same-(dc,qid) instances) is proven
// separately in test_territory_mount.c (devno_disambiguates).
// =============================================================================

// Set up a SYSTEM Proc with a fresh Territory + a fixture root. The Territory is
// the mount-table home cross_mounts reads. Returns the root (caller owns); fills
// *p (territory must be territory_unref'd by the caller).
static struct Spoor *cross_setup(struct Proc *p) {
    mkproc_system(p);
    p->territory = territory_alloc();
    if (!p->territory) return NULL;
    return fix_root();
}

void test_stalk_cross_mount(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    // Resolve the source (subtree "a", qid 1) and the mount point ("loop", qid
    // 7, a 0755 dir). Graft a onto loop.
    struct Spoor *src = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mount point");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount a onto loop");

    // "/loop/b": walk to loop, cross loop->a (domount), walk "b" -> a/b (qid 2).
    struct Spoor *q = stalk(&p, root, "loop/b", 6, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop/b (crossed)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "loop/b crosses to a/b (qid 2)");
    spoor_clunk(q);

    territory_unref(p.territory);   // drops the mount entry's ref on src
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

void test_stalk_cross_mount_final_quarry(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *src = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mount point");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount a onto loop");

    // Opening the mount point itself yields the MOUNTED root (Plan 9 domount on
    // the final element): "/loop" (STALK_OPEN) crosses to a-root (qid 1).
    struct Spoor *q = stalk(&p, root, "loop", 4, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop (final-element cross)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)1, "open(loop) -> mounted a-root (qid 1)");
    TEST_ASSERT((q->flag & COPEN) != 0, "mounted root opened");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

void test_stalk_cross_mount_xsearch_deny(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    // Mount "nox" (a 0644 dir -- owner rw-, NO x) onto "loop". After crossing
    // loop->nox-root, the X-search on the MOUNTED root denies traversal: the
    // mounted fs's own perms govern, so loop/sekret is unreachable.
    struct Spoor *src = stalk(&p, root, "nox", 3, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve nox + loop");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount nox onto loop");

    struct Spoor *q = stalk(&p, root, "loop/sekret", 11, STALK_OPEN, 0);
    TEST_ASSERT(q == NULL, "loop/sekret denied at X-search on the mounted nox-root");

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

void test_stalk_mount_amode_no_cross(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *src = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mount point");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount a onto loop");

    // STALK_MOUNT must NOT cross the final element: resolving "loop" yields
    // loop's OWN identity (qid 7), not the mounted a-root -- so a SECOND mount
    // onto "loop" MREPL-replaces the SAME entry (re-keying correctness).
    struct Spoor *mp2 = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(mp2 != NULL, "re-resolve loop (STALK_MOUNT)");
    TEST_EXPECT_EQ((u64)mp2->qid.path, (u64)7,
        "STALK_MOUNT returns loop's own identity (qid 7), not the crossed a-root");

    // Prove MREPL re-keys the same point: mount "deep" (qid 3) onto loop with
    // MREPL; nmounts stays 1; "/loop" now crosses to deep (qid 3).
    struct Spoor *src2 = stalk(&p, root, "a/deep", 6, STALK_WALK, 0);
    TEST_ASSERT(src2 != NULL, "resolve a/deep");
    TEST_EXPECT_EQ(mount(p.territory, src2, mp2, MREPL), 0, "MREPL deep onto loop");
    TEST_EXPECT_EQ(territory_nmounts(p.territory), 1, "MREPL kept ONE entry");

    struct Spoor *q = stalk(&p, root, "loop", 4, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop after MREPL");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)3, "loop now crosses to deep (qid 3)");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(src2);
    spoor_clunk(mp);
    spoor_clunk(mp2);
    spoor_unref(root);
}

void test_stalk_cross_mount_chain(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    // mount-over-a-mount: a onto loop, AND deep onto a. "/loop" then crosses
    // loop->a (qid 1), and a is ITSELF a mount point -> crosses again to deep
    // (qid 3). The bounded domount loop must follow the chain to the leaf.
    struct Spoor *src_a    = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp_loop  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    struct Spoor *src_deep = stalk(&p, root, "a/deep", 6, STALK_WALK, 0);
    struct Spoor *mp_a     = stalk(&p, root, "a", 1, STALK_MOUNT, 0);
    TEST_ASSERT(src_a && mp_loop && src_deep && mp_a, "resolve chain pieces");
    TEST_EXPECT_EQ(mount(p.territory, src_a, mp_loop, 0), 0, "mount a onto loop");
    TEST_EXPECT_EQ(mount(p.territory, src_deep, mp_a, 0), 0, "mount deep onto a");

    struct Spoor *q = stalk(&p, root, "loop", 4, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop (chain)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)3,
        "loop -> a -> deep chain crosses to qid 3 (deep)");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(src_a);
    spoor_clunk(mp_loop);
    spoor_clunk(src_deep);
    spoor_clunk(mp_a);
    spoor_unref(root);
}

void test_stalk_cross_mount_no_leak(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *src = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mount point");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount a onto loop");

    // A crossing resolve mints a transient clone of the source (clone_walk_zero)
    // that must be clunked, not leaked: live count balances across resolve+clunk.
    u64 live_before = spoor_total_allocated() - spoor_total_freed();
    struct Spoor *q = stalk(&p, root, "loop/b", 6, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop/b (crossed)");
    spoor_clunk(q);
    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before, "no Spoor leak across a crossed resolve");

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

// =============================================================================
// #66 -- namespace-name accumulation through the real resolver.
//
// The fixture root is a qid Dev (not devramfs/dev9p), so it carries no seeded
// Path; these tests seed `root->path = "/"` manually (mimicking the attach
// seed) and then assert the quarry's accumulated name.
// =============================================================================

void test_stalk_path_accumulate(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");
    root->path = path_make_root();
    TEST_ASSERT(root->path != NULL, "seed root /");

    u64 pa0 = path_total_allocated(), pf0 = path_total_freed();
    struct Spoor *q = stalk(&p, root, "a/deep/leaf", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/leaf");
    TEST_ASSERT(q->path != NULL && fix_streq(q->path->s, "/a/deep/leaf"),
                "quarry path accumulated to /a/deep/leaf");
    spoor_clunk(q);
    // Every Path allocated during the resolve (one per hop) is freed: the trail
    // unwinds inside stalk; the quarry's path frees with q above. (root's "/"
    // was allocated before this window and frees at unref below.)
    TEST_EXPECT_EQ(path_total_allocated() - pa0, path_total_freed() - pf0,
                   "no Path leak across a multi-hop resolve");
    spoor_unref(root);
}

void test_stalk_path_dotdot(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");
    root->path = path_make_root();
    // 'deep' is walked then popped by '..'; the name must reflect the pop.
    struct Spoor *q = stalk(&p, root, "a/deep/../b", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/../b");
    TEST_ASSERT(q->path != NULL && fix_streq(q->path->s, "/a/b"),
                ".. yields /a/b (deep popped from the name, not /a/deep/b)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_path_cross_transplant(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");
    root->path = path_make_root();

    struct Spoor *src = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mount point");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount a onto loop");

    // Resolving loop/b crosses the /loop mount: the crossed clone (a clone of
    // the 'a' subtree root, whose OWN name is /a) must take the MOUNT-POINT's
    // name /loop, NOT the source's /a -- so the child b reads /loop/b.
    struct Spoor *q = stalk(&p, root, "loop/b", 6, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop/b (crossed)");
    TEST_ASSERT(q->path != NULL && fix_streq(q->path->s, "/loop/b"),
                "crossed quarry takes the mount-point name: /loop/b");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

// #66 F2 (owed from the #66a audit): the STALK_OPEN open=connect adoption arm
// (stalk.c -- Dev.open RETURNS a different Spoor) must TRANSPLANT the walked
// namespace name onto the adopted replacement. With fix_open_replace_nopath
// (the faithful devsrv mint: the replacement carries NO name of its own), the
// only way q->path == "/a/b" is the spoor_path_transplant the F2 fix added; the
// pre-fix code (adopt without transplant) would leave q->path == NULL.
void test_stalk_path_adopt_transplant(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root_replace_nopath();
    TEST_ASSERT(root != NULL, "fix_root_replace_nopath");
    root->path = path_make_root();
    TEST_ASSERT(root->path != NULL, "seed root /");

    struct Spoor *q = stalk(&p, root, "a/b", 3, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve+open a/b (adoption arm)");
    TEST_EXPECT_EQ((u64)q->qid.vers, (u64)0xBEEFu,
                   "open returned the nameless replacement (opened != quarry)");
    TEST_ASSERT(q->path != NULL && fix_streq(q->path->s, "/a/b"),
                "adopted Spoor takes the WALKED name /a/b (F2 transplant; "
                "pre-fix this was NULL)");
    spoor_clunk(q);
    spoor_unref(root);
}

// #36: content-addressed names must pass the per-component bound. The Go build
// cache names every entry <64-hex>-a / <64-hex>-d (66 chars); the pre-#36 cap
// of 64 EINVAL'd every such open/create at stalk's component check, so the
// on-device GOCACHE could neither read the host-baked entries nor persist its
// own -- a TOTAL cache miss that cmd/go absorbs silently (every cache error is
// a best-effort miss). Pin: a 66-char and a 255-char component PASS the bound
// (reach the Dev and report a clean walk-miss, T_E_NOENT -- proving the
// component validator no longer rejects them); a 256-char component still
// fails CLOSED with T_E_INVAL (the bound itself stays enforced).
void test_stalk_long_component_bound(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    char name[258];
    int e;

    // 66 chars -- the exact Go-cache entry shape.
    for (int i = 0; i < 66; i++) name[i] = 'x';
    e = -12345;
    struct Spoor *q66 = stalk_err(&p, root, name, 66, STALK_OPEN, 0, &e);
    TEST_ASSERT(q66 == NULL, "66-char unknown name -> walk-miss");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT,
                   "66-char component PASSES the bound (miss, not EINVAL)");

    // 255 chars -- the new bound, inclusive.
    for (int i = 0; i < 255; i++) name[i] = 'y';
    e = -12345;
    struct Spoor *q255 = stalk_err(&p, root, name, 255, STALK_OPEN, 0, &e);
    TEST_ASSERT(q255 == NULL, "255-char unknown name -> walk-miss");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT,
                   "255-char component PASSES the bound (miss, not EINVAL)");

    // 256 chars -- over the bound; fail-closed rejection, never truncation.
    for (int i = 0; i < 256; i++) name[i] = 'z';
    e = -12345;
    struct Spoor *q256 = stalk_err(&p, root, name, 256, STALK_OPEN, 0, &e);
    TEST_ASSERT(q256 == NULL, "256-char name -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_INVAL,
                   "over-bound component rejected with T_E_INVAL");

    spoor_unref(root);
}
