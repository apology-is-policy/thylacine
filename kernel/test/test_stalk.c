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

// POUNCE (docs/POUNCE-DESIGN.md): stalkfix implements Dev.walk_attrs, so the
// ENTIRE pre-existing battery below now resolves through the pounce fast path
// -- their unchanged expectations are the pounce==per-component-loop parity
// proof. stalkfix_nowa is the same tree WITHOUT the slot (the per-component
// loop), for explicit A/B parity assertions; the g_fix_*_calls counters prove
// which path engaged (non-vacuity).

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
// POUNCE: the batched fast path + stalk_stat (docs/POUNCE-DESIGN.md).
void test_stalk_pounce_engaged(void);
void test_stalk_pounce_acces_masks_noent(void);
void test_stalk_pounce_parity_nowa(void);
void test_stalk_pounce_full_walk_past_mount(void);
void test_stalk_stat_query(void);
void test_stalk_stat_mount_leaf(void);
void test_sys_stat_for_proc(void);
void test_stalk_pounce_unsupported_fallback(void);

// FID-LIFECYCLE cached-open: the resolver arm (engagement, mode gate,
// fail-ordering post-scan, mount discard-and-fallback).
void test_stalk_cached_open_arm(void);
void test_stalk_cached_open_denials(void);
void test_stalk_cached_open_mount_fallback(void);

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
    { 8, 1, "nor",    QTFILE, 0200u },   // owner write-only: the leaf-R deny
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

// Path-engagement counters: which resolver path actually ran (non-vacuity --
// a pounce test that silently fell back to the per-component loop would
// otherwise pass hollowly).
static int g_fix_walk_calls;
static int g_fix_walkattrs_calls;

static struct Walkqid *fix_walk(struct Spoor *c, struct Spoor *nc,
                                const char **name, int nname) {
    if (!c || nname < 0) return NULL;
    if (nname > 0) g_fix_walk_calls++;   // real steps only (0-walk = clone)
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

static int fix_stat_qid(u64 qid_path, struct t_stat *out) {
    const struct fixnode *fn = fix_node(qid_path);
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

static int fix_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    return fix_stat_qid(c->qid.path, out);
}

// The POUNCE fixture walk_attrs. Honors the sharpened contract in
// <thylacine/dev.h>: transitions nc ONLY on a full walk; partial/query leave
// nc untouched and return w->spoor == NULL.
static struct Walkqid *fix_walk_attrs(struct Spoor *c, struct Spoor *nc,
                                      const char **names,
                                      const size_t *name_lens,
                                      int nname, struct t_stat *sts) {
    if (!c || nname <= 0 || nname > DEV_WALK_ATTRS_MAX) return NULL;
    if (!names || !name_lens || !sts) return NULL;
    g_fix_walkattrs_calls++;

    struct Walkqid *wq = walkqid_alloc(nname);
    if (!wq) return NULL;

    u64 cur = c->qid.path;
    int n = 0;
    for (int i = 0; i < nname; i++) {
        char nb[SYS_WALK_OPEN_NAME_MAX + 1];
        size_t l = name_lens[i];
        if (l == 0 || l > SYS_WALK_OPEN_NAME_MAX) break;
        for (size_t k = 0; k < l; k++) nb[k] = names[i][k];
        nb[l] = '\0';
        struct Qid next;
        if (!fix_walk_one(cur, nb, &next)) break;
        if (fix_stat_qid(next.path, &sts[n]) != 0) break;
        cur = next.path;
        wq->qid[n++] = next;
    }
    wq->nqid = n;
    if (nc && n == nname) {
        nc->qid  = wq->qid[n - 1];
        wq->spoor = nc;
    } else {
        wq->spoor = NULL;
    }
    return wq;
}

static struct Spoor *fix_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    c->flag |= COPEN;
    c->mode  = omode;
    return c;
}

static void fix_close(struct Spoor *c) { (void)c; /* qid-based: no heap aux */ }

// FID-LIFECYCLE cached-open fixture slot. Controllable: g_fix_co_enable false
// declines every attempt (the arm must fall back byte-identically); enabled, it
// resolves the run through the FIXTURE table (the "underlying tree" -- blind to
// mounts, exactly like a real Dev), fills FRESH sts, and mints an OPENED Spoor
// for a plain-file leaf. The counters prove engagement/minting non-vacuously.
static int  g_fix_co_calls;    // slot invocations (the arm consulted us)
static int  g_fix_co_minted;   // successful mints (the arm then post-scans)
static bool g_fix_co_enable;

static struct Spoor *fix_open_cached(struct Spoor *c, const char *const *names,
                                     const size_t *name_lens, int nname,
                                     struct t_stat *sts) {
    g_fix_co_calls++;
    if (!g_fix_co_enable) return NULL;
    if (!c || !names || !name_lens || !sts) return NULL;
    if (nname <= 0 || nname > DEV_WALK_ATTRS_MAX) return NULL;
    u64 cur = c->qid.path;
    for (int i = 0; i < nname; i++) {
        char nb[SYS_WALK_OPEN_NAME_MAX + 1];
        size_t l = name_lens[i];
        if (l == 0 || l > SYS_WALK_OPEN_NAME_MAX) return NULL;
        for (size_t k = 0; k < l; k++) nb[k] = names[i][k];
        nb[l] = '\0';
        struct Qid next;
        if (!fix_walk_one(cur, nb, &next)) return NULL;
        if (fix_stat_qid(next.path, &sts[i]) != 0) return NULL;
        cur = next.path;
    }
    if (sts[nname - 1].qid_type & QTDIR) return NULL;   // plain files only
    struct Spoor *co = spoor_clone(c);
    if (!co) return NULL;
    co->qid.path = sts[nname - 1].qid_path;
    co->qid.vers = 0;
    co->qid.type = sts[nname - 1].qid_type;
    co->flag |= COPEN;
    co->mode  = 0;
    g_fix_co_minted++;
    return co;
}

// The fixture Dev. dc is a test-only sentinel; it is NOT dev_register'd, so the
// dc never collides with a real Dev (stalk reaches it only through the Spoors we
// hand it directly).
static struct Dev stalkfix = {
    .dc            = (int)'Z',
    .name          = "stalkfix",
    .perm_enforced = true,
    .attach        = NULL,   // we mint the root via dev_simple_attach(&stalkfix,...)
    .walk          = fix_walk,
    .walk_attrs    = fix_walk_attrs,   // POUNCE: the whole battery runs the fast path
    .open_cached   = fix_open_cached,  // FID-LIFECYCLE: the resolver-arm tests
    .stat_native   = fix_stat_native,
    .open          = fix_open,
    .close         = fix_close,
};

// The A/B twin: the SAME tree with NO walk_attrs slot -- resolves through the
// per-component loop. The parity test runs identical paths on both and
// asserts identical outcomes.
static struct Dev stalkfix_nowa = {
    .dc            = (int)'X',
    .name          = "stalkfix_nowa",
    .perm_enforced = true,
    .attach        = NULL,
    .walk          = fix_walk,
    .stat_native   = fix_stat_native,
    .open          = fix_open,
    .close         = fix_close,
};

static struct Spoor *fix_root_nowa(void) {
    return dev_simple_attach(&stalkfix_nowa, QTDIR);
}

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
    .walk_attrs    = fix_walk_attrs,
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
    .walk_attrs    = fix_walk_attrs,
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

// =============================================================================
// POUNCE (docs/POUNCE-DESIGN.md §5/§6) -- the batched fast path. The whole
// battery above already runs THROUGH the pounce (stalkfix has walk_attrs);
// these tests pin the properties the batching itself introduces: engagement
// (non-vacuity), the fail-ordering invariant, the mount-mid-run split, the
// A/B parity vs the per-component loop, and the stalk_stat walk-query.
// =============================================================================

void test_stalk_pounce_engaged(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // A 3-component path resolves in ONE walk_attrs batch and ZERO
    // per-component walks (fix_walk counts only real steps; the quarry is
    // popped, not clone-walked). This is the non-vacuity anchor: if the
    // pounce silently fell back to the loop, every "parity" pass above
    // would be hollow.
    g_fix_walk_calls = 0; g_fix_walkattrs_calls = 0;
    struct Spoor *q = stalk(&p, root, "a/deep/leaf", 11, STALK_WALK, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/leaf");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)4, "a/deep/leaf -> qid 4");
    TEST_EXPECT_EQ((u64)g_fix_walkattrs_calls, (u64)1,
                   "ONE batched walk_attrs call for the whole run");
    TEST_EXPECT_EQ((u64)g_fix_walk_calls, (u64)0,
                   "ZERO per-component walks (the run never fell back)");
    spoor_clunk(q);
    spoor_unref(root);
}

void test_stalk_pounce_acces_masks_noent(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // THE fail-ordering invariant (POUNCE-DESIGN §6; the audit's #1 target):
    // nox (0644, no x) followed by a MISSING component. The batch walks nox
    // then misses -- a naive post-scan would report the walk's NOENT, leaking
    // "no such entry under nox" to a caller with no X on nox. The post-scan
    // must consume left-to-right: the X-denial on nox (the miss's parent)
    // MASKS the miss -> T_E_ACCES, never T_E_NOENT.
    int e = -12345;
    g_fix_walkattrs_calls = 0;
    struct Spoor *q = stalk_err(&p, root, "nox/missing", 11, STALK_WALK, 0, &e);
    TEST_ASSERT(q == NULL, "nox/missing denied");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES,
                   "X-denial at nox MASKS the deeper miss (ACCES, not NOENT)");
    TEST_ASSERT(g_fix_walkattrs_calls >= 1, "the pounce path ran (non-vacuous)");

    // The same masking one level deeper: nox/sekret/missing -- sekret EXISTS
    // (a file) but nox denies X; still ACCES.
    e = -12345;
    struct Spoor *q2 = stalk_err(&p, root, "nox/sekret/x", 12, STALK_WALK, 0, &e);
    TEST_ASSERT(q2 == NULL, "nox/sekret/x denied");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "deeper probe still masked (ACCES)");

    spoor_unref(root);
}

void test_stalk_pounce_parity_nowa(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root_wa   = fix_root();
    struct Spoor *root_nowa = fix_root_nowa();
    TEST_ASSERT(root_wa != NULL && root_nowa != NULL, "both roots");

    // A/B parity: identical paths through the pounce (stalkfix) and the
    // per-component loop (stalkfix_nowa) yield identical (qid | errno).
    static const struct { const char *path; u64 len; } cases[] = {
        { "a/b",           3 },   // plain multi-component
        { "a/deep/leaf",  11 },   // 3-deep
        { "/a//b",         5 },   // separator collapsing
        { "a/./b",         5 },   // '.' breaks the run
        { "a/deep/../b",  11 },   // '..' disables the pounce entirely
        { "a/nope",        6 },   // miss under a searchable dir -> NOENT
        { "nox/sekret",   10 },   // X-denial on an existing deeper file
        { "nox/nope",      8 },   // X-denial masking a miss
        { "/",             1 },   // zero real components
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int ea = 0, eb = 0;
        struct Spoor *qa = stalk_err(&p, root_wa, cases[i].path, cases[i].len,
                                     STALK_WALK, 0, &ea);
        struct Spoor *qb = stalk_err(&p, root_nowa, cases[i].path, cases[i].len,
                                     STALK_WALK, 0, &eb);
        TEST_EXPECT_EQ((u64)(qa != NULL), (u64)(qb != NULL),
                       "parity: same success/failure");
        if (qa && qb) {
            TEST_EXPECT_EQ((u64)qa->qid.path, (u64)qb->qid.path,
                           "parity: same resolved qid");
        } else if (!qa && !qb) {
            TEST_EXPECT_EQ((u64)ea, (u64)eb, "parity: same errno");
        }
        if (qa) spoor_clunk(qa);
        if (qb) spoor_clunk(qb);
    }
    spoor_unref(root_wa);
    spoor_unref(root_nowa);
}

void test_stalk_pounce_full_walk_past_mount(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    // Mount deep (qid 3) onto a (qid 1). The underlying `a` HAS a child `b`
    // (qid 2), so the batched walk of [a, b] FULLY succeeds server-side --
    // walking PAST the mount point into the underlying tree. The post-scan's
    // mount test must catch `a` mid-run, SPLIT, cross to the mounted deep,
    // and resolve `b` there -- where it does NOT exist. A broken pounce
    // returns the underlying b (qid 2); the correct answer is NOENT.
    struct Spoor *src = stalk(&p, root, "a/deep", 6, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "a", 1, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve deep + a");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount deep onto a");

    int e = -12345;
    struct Spoor *q = stalk_err(&p, root, "a/b", 3, STALK_WALK, 0, &e);
    TEST_ASSERT(q == NULL,
        "a/b resolves in the MOUNTED tree (deep has no b) -- the batch's "
        "underlying full-walk result was discarded");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "miss in the mounted tree");

    // And the positive twin: a/leaf lives ONLY in the mounted tree (deep's
    // child, qid 4); the underlying a has no `leaf`, so the batch goes
    // partial at it -- the split + cross must still find it.
    struct Spoor *q2 = stalk(&p, root, "a/leaf", 6, STALK_WALK, 0);
    TEST_ASSERT(q2 != NULL, "a/leaf resolves through the crossed mount");
    TEST_EXPECT_EQ((u64)q2->qid.path, (u64)4, "a/leaf -> mounted deep/leaf (qid 4)");
    spoor_clunk(q2);

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

void test_stalk_stat_query(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // The walk-QUERY 1-RPC stat: attrs arrive fused with the walk; NO quarry
    // Spoor is ever materialized. The live-Spoor balance across the call is
    // the no-materialization proof (a fallback-shaped implementation would
    // mint + clunk a quarry -- balanced too -- so ALSO pin the engagement
    // counters: one batch, zero per-component walks, zero plain clones).
    struct t_stat st;
    int e = -12345;
    g_fix_walk_calls = 0; g_fix_walkattrs_calls = 0;
    u64 alloc_before = spoor_total_allocated();
    int rc = stalk_stat(&p, root, "a/deep/leaf", 11, &st, &e);
    u64 alloc_after = spoor_total_allocated();
    TEST_EXPECT_EQ((u64)rc, (u64)0, "stalk_stat a/deep/leaf");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)4, "attrs are the leaf's (qid 4)");
    TEST_EXPECT_EQ((u64)st.mode, (u64)(T_S_IFREG | 0640u), "leaf mode 0640");
    TEST_EXPECT_EQ((u64)st.uid, (u64)PRINCIPAL_SYSTEM, "leaf uid SYSTEM");
    TEST_EXPECT_EQ((u64)g_fix_walkattrs_calls, (u64)1, "ONE batched query walk");
    TEST_EXPECT_EQ((u64)g_fix_walk_calls, (u64)0, "zero per-component walks");
    TEST_EXPECT_EQ(alloc_after, alloc_before,
                   "the query materialized NO Spoor at all");

    // Resolution failures carry the stalk errnos.
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "a/nope", 6, &st, &e), (u64)-1,
                   "stat of a missing path fails");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "missing -> NOENT");
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "nox/sekret", 10, &st, &e), (u64)-1,
                   "stat under a no-X dir fails");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "denied -> ACCES (fail-ordering)");

    // The stat of "/" (zero real components) takes the fallback quarry path.
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "/", 1, &st, &e), (u64)0, "stat /");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)0, "root attrs");
    TEST_EXPECT_EQ((u64)st.mode, (u64)(T_S_IFDIR | 0755u), "root mode");

    spoor_unref(root);
}

void test_stalk_stat_mount_leaf(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    // stat ON a mount point reports the MOUNTED root (POSIX: stat of /mnt
    // shows the mounted fs root). The query walked the UNDERLYING loop, so
    // the leaf-mount split discards the query, materializes the mount point,
    // crosses, and the wrapper stats the crossed a-root.
    struct Spoor *src = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mount point");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount a onto loop");

    struct t_stat st;
    int e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "loop", 4, &st, &e), (u64)0,
                   "stat of a mount point succeeds");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)1,
                   "attrs are the MOUNTED a-root's (qid 1), not loop's (qid 7)");

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

// SYS_STAT's testable inner (the #37 *_for_proc shape; kernel path + kernel
// t_stat -- the handler's uaccess staging wraps it).
extern s64 sys_stat_for_proc(struct Proc *p, const char *path, u64 path_len,
                             struct t_stat *out_k);

void test_sys_stat_for_proc(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);   // synthetic Proc + fresh Territory
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");
    // The Territory root is the fixture root (SYS_STAT resolves from
    // territory_root_ref, the FROM_ROOT arm).
    TEST_EXPECT_EQ(territory_chroot(p.territory, root), 0, "chroot to fixture");

    struct t_stat st;
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/a/b", 4, &st), (u64)0,
                   "SYS_STAT inner: absolute path");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)2, "/a/b -> qid 2");
    TEST_EXPECT_EQ((u64)st.mode, (u64)(T_S_IFREG | 0644u), "b mode 0644");

    // Relative path joins the cwd (dot unset == "/"); same answer.
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "a/b", 3, &st), (u64)0,
                   "SYS_STAT inner: relative path via the cwd join");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)2, "a/b -> qid 2");

    // Resolution errnos pass through as -errno.
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/a/nope", 7, &st),
                   (u64)(s64)-T_E_NOENT, "missing -> -T_E_NOENT");
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/nox/sekret", 11, &st),
                   (u64)(s64)-T_E_ACCES, "denied -> -T_E_ACCES");

    // Structural rejects -> the bare -1.
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, NULL, 3, &st), (u64)(s64)-1,
                   "NULL path -> -1");
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/a/b", 0, &st), (u64)(s64)-1,
                   "zero-length path -> -1");

    territory_unref(p.territory);
    spoor_unref(root);
}

// A Dev whose walk_attrs always reports the backing as incapable (the netd
// case: a 9P server without the Twalkgetattr extension; dev9p latches the
// first ENOSYS and then returns this sentinel RPC-free). The resolver must
// degrade to the per-component loop with identical results.
static struct Walkqid *fix_walk_attrs_unsup(struct Spoor *c, struct Spoor *nc,
                                            const char **names,
                                            const size_t *name_lens,
                                            int nname, struct t_stat *sts) {
    (void)c; (void)nc; (void)names; (void)name_lens; (void)nname; (void)sts;
    g_fix_walkattrs_calls++;
    return DEV_WALK_ATTRS_UNSUPPORTED;
}

static struct Dev stalkfix_unsup = {
    .dc            = (int)'W',
    .name          = "stalkfix_unsup",
    .perm_enforced = true,
    .attach        = NULL,
    .walk          = fix_walk,
    .walk_attrs    = fix_walk_attrs_unsup,
    .stat_native   = fix_stat_native,
    .open          = fix_open,
    .close         = fix_close,
};

void test_stalk_pounce_unsupported_fallback(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = dev_simple_attach(&stalkfix_unsup, QTDIR);
    TEST_ASSERT(root != NULL, "unsup root");

    // The full resolution succeeds through the per-component loop; every
    // component's walk_attrs probe returned the sentinel (no real batch).
    g_fix_walk_calls = 0; g_fix_walkattrs_calls = 0;
    struct Spoor *q = stalk(&p, root, "a/deep/leaf", 11, STALK_WALK, 0);
    TEST_ASSERT(q != NULL, "resolution degrades to the loop and succeeds");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)4, "same answer as the pounce");
    TEST_EXPECT_EQ((u64)g_fix_walk_calls, (u64)3, "three per-component walks");
    TEST_ASSERT(g_fix_walkattrs_calls >= 3, "the sentinel was consulted per hop");
    spoor_clunk(q);

    // stalk_stat degrades too: the fallback quarry path stats + clunks.
    struct t_stat st;
    int e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "a/b", 3, &st, &e), (u64)0,
                   "stalk_stat via the fallback");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)2, "correct attrs via stat_native");

    // The X-search still enforces (the sentinel path is the audited loop).
    e = -12345;
    struct Spoor *deny = stalk_err(&p, root, "nox/sekret", 10, STALK_WALK, 0, &e);
    TEST_ASSERT(deny == NULL, "denial intact on the fallback path");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "ACCES on the fallback path");

    spoor_unref(root);
}

// =============================================================================
// FID-LIFECYCLE cached-open: the resolver arm (docs/FID-LIFECYCLE-DESIGN.md
// section 3.3). The Dev-slot internals (hint / fresh query / snapshot / budget)
// are dev9p's tests; THESE prosecute the stalk side -- engagement, the strict
// mode gate, the mandatory fail-ordering post-scan, and the mount discard.
// =============================================================================

void test_stalk_cached_open_arm(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // Engaged: the arm serves the open FROM the slot -- the batched walk never
    // runs (short-circuit), the result is the opened leaf.
    g_fix_co_enable = true;
    g_fix_co_calls = 0; g_fix_co_minted = 0;
    int wa0 = g_fix_walkattrs_calls;
    struct Spoor *q = stalk(&p, root, "a/b", 3, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "cached-open resolves a/b");
    TEST_EXPECT_EQ((u64)q->qid.path, 2ull, "the leaf qid");
    TEST_ASSERT((q->flag & COPEN) != 0, "opened");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 1ull, "the arm consulted the slot");
    TEST_EXPECT_EQ((u64)g_fix_co_minted, 1ull, "the slot minted");
    TEST_EXPECT_EQ((u64)(g_fix_walkattrs_calls - wa0), 0ull,
                   "the batched walk was short-circuited");
    spoor_clunk(q);

    // The strict mode gate: anything but a plain OREAD never consults the
    // slot (write / OTRUNC / OEXEC / a non-open amode).
    g_fix_co_calls = 0;
    q = stalk(&p, root, "a/b", 3, STALK_OPEN, 1);          // OWRITE
    TEST_ASSERT(q != NULL, "OWRITE resolves via the normal path");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 0ull, "OWRITE never consults the slot");
    spoor_clunk(q);
    g_fix_co_calls = 0;
    q = stalk(&p, root, "a/b", 3, STALK_OPEN, 0x10);       // OTRUNC (R|W want)
    TEST_ASSERT(q != NULL, "OTRUNC resolves via the normal path");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 0ull, "OTRUNC never consults the slot");
    spoor_clunk(q);
    g_fix_co_calls = 0;
    q = stalk(&p, root, "a/b", 3, STALK_OPEN, 3);          // OEXEC
    // (b is 0644 -- no x bit -- so the NORMAL final hop denies OEXEC with
    // PERM_R|PERM_X; the sub-test's point is only that the slot never ran.)
    TEST_ASSERT(q == NULL, "OEXEC denied via the normal path (no x on b)");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 0ull, "OEXEC never consults the slot");
    g_fix_co_calls = 0;
    q = stalk(&p, root, "a/b", 3, STALK_WALK, 0);          // O_PATH shape
    TEST_ASSERT(q != NULL, "STALK_WALK resolves");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 0ull, "STALK_WALK never consults the slot");
    spoor_clunk(q);

    // Declined (the slot returns NULL): byte-identical fallback -- the normal
    // pounce walk resolves + opens the same leaf.
    g_fix_co_enable = false;
    g_fix_co_calls = 0;
    wa0 = g_fix_walkattrs_calls;
    q = stalk(&p, root, "a/b", 3, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "fallback resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, 2ull, "fallback parity: same leaf");
    TEST_ASSERT((q->flag & COPEN) != 0, "fallback parity: opened");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 1ull, "the slot was consulted");
    TEST_ASSERT(g_fix_walkattrs_calls > wa0, "the batched walk ran (fallback)");
    spoor_clunk(q);

    spoor_unref(root);
}

void test_stalk_cached_open_denials(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");
    g_fix_co_enable = true;

    // X-search denial ON THE CACHED PATH: the slot mints (the fixture tree
    // resolves nox/sekret -- it is perm-blind, like a real Dev), and the ARM's
    // post-scan denies on nox (0644, no x): T_E_ACCES, the minted Spoor
    // destroyed. The fail-ordering invariant holds on the fast path.
    int e = -12345;
    g_fix_co_calls = 0; g_fix_co_minted = 0;
    struct Spoor *q = stalk_err(&p, root, "nox/sekret", 10, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "nox/sekret denied on the cached path");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "T_E_ACCES (never NOENT past a denied dir)");
    TEST_EXPECT_EQ((u64)g_fix_co_minted, 1ull,
                   "the slot HAD minted -- the ARM's post-scan denied");

    // Leaf R denial: a/nor is 0200 (owner write-only) -- the final-hop R gate
    // on the FRESH leaf record denies the read-only open.
    e = -12345;
    g_fix_co_minted = 0;
    q = stalk_err(&p, root, "a/nor", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/nor read-open denied (leaf R)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "T_E_ACCES");
    TEST_EXPECT_EQ((u64)g_fix_co_minted, 1ull,
                   "the slot HAD minted -- the ARM's leaf gate denied");

    g_fix_co_enable = false;
    spoor_unref(root);
}

void test_stalk_cached_open_mount_fallback(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    // Graft deep(3) onto a(1). The UNDERLYING tree still resolves a/b -- the
    // wrong tree once a is a mount point.
    struct Spoor *src = stalk(&p, root, "a/deep", 6, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "a", 1, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mp");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount deep onto a");

    g_fix_co_enable = true;

    // The slot mints from the UNDERLYING tree (a/b = qid 2); the arm's mount
    // scan hits a (j == 0) and DISCARDS the mint -- the normal path then
    // crosses into deep, where "b" does not exist. The observable outcome is
    // the MOUNTED tree's NOENT, never the underlying tree's qid-2 Spoor.
    int e = -12345;
    g_fix_co_calls = 0; g_fix_co_minted = 0;
    struct Spoor *q = stalk_err(&p, root, "a/b", 3, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "the underlying a/b is NOT served across the mount");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "the mounted tree's NOENT");
    TEST_EXPECT_EQ((u64)g_fix_co_minted, 1ull,
                   "the slot HAD minted from the underlying tree (discarded)");

    // The mounted tree's real leaf resolves correctly: the slot declines the
    // PRE-cross run (the underlying chain has no a/leaf), the normal path
    // splits + crosses, and the RESUMED run inside the MOUNTED tree
    // legitimately mints a cached-open there -- the fast path composes with
    // the split/cross machinery.
    g_fix_co_calls = 0; g_fix_co_minted = 0;
    q = stalk(&p, root, "a/leaf", 6, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "a/leaf resolves in the MOUNTED tree");
    TEST_EXPECT_EQ((u64)q->qid.path, 4ull, "deep/leaf's qid");
    TEST_ASSERT((q->flag & COPEN) != 0, "opened");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 2ull,
                   "pre-cross run declined + resumed run consulted");
    TEST_EXPECT_EQ((u64)g_fix_co_minted, 1ull,
                   "the RESUMED run minted inside the mounted tree");
    spoor_clunk(q);

    g_fix_co_enable = false;
    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}
