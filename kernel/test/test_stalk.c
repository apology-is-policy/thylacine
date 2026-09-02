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
void test_stalk_notdir(void);                  // #79
void test_stalk_dot_notdir(void);              // #81
void test_stalk_dot_notdir_mount(void);        // #81 (uncrossed-tip choice)
void test_stalk_dot_xsearch(void);             // #84
void test_stalk_trailing_slash(void);          // #82
void test_stalk_trailing_slash_mount(void);    // #82 (crossed-quarry choice)
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
// UM (union mounts): the union walk over the real resolver.
void test_stalk_union_walk(void);
void test_stalk_union_order(void);
void test_stalk_union_xskip(void);
void test_stalk_union_readdir(void);            // UM-5: merge + dedup first-wins
void test_stalk_union_readdir_paginate(void);   // UM-5: ordinal-cursor resume
void test_stalk_union_readdir_nontagged(void);  // UM-5: control (not over-tagged)
void test_stalk_union_create(void);             // UM-5a: MCREATE member selected
void test_stalk_union_create_first_wins(void);  // UM-5a: first MCREATE (declared order)
void test_stalk_union_create_no_target(void);   // UM-5a: no MCREATE -> EACCES
void test_stalk_union_member_holding(void);     // UM-8c/F3: holder, not MCREATE member
void test_stalk_union_remove_uncrossed(void);    // UM-8c/F3: STALK_REMOVE leaves the point
void test_stalk_union_fd_base(void);             // UM-8c/F5: fd-relative union base sees all members
void test_stalk_pheno_symlink_reanchor(void);   // VIVARIUM section 13 (F1)
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

// D-1: symlink expansion (DISTRO.md section 4; the I-28 refinement).
void test_stalk_symlink_follow(void);
void test_stalk_symlink_bounds(void);
void test_stalk_symlink_nofollow(void);
void test_stalk_symlink_stat_vs_lstat(void);
void test_stalk_symlink_pounce_split(void);
void test_stalk_symlink_lifetime(void);

// #50: SYS_OPEN_CREATE over the fixture (the create overlay).
void test_stalk_open_create_cwd_parity(void);
void test_stalk_open_create_open_if_present(void);
void test_stalk_open_create_mkdir_and_nest(void);
void test_stalk_open_create_leaf_rows(void);
void test_stalk_open_create_containment_and_denials(void);

// =============================================================================
// The fixture Dev.
// =============================================================================

struct fixnode {
    u64         path;
    u64         parent;
    const char *name;
    u8          type;   // QTDIR | QTFILE | QTSYMLINK
    u32         mode;   // low 9 rwx bits (the X-search reads owner bits)
    const char *target; // D-1: the symlink target (NULL for non-links)
};

static const struct fixnode g_fix[] = {
    { 0, 0, "/",      QTDIR,  0755u, NULL },
    { 1, 0, "a",      QTDIR,  0755u, NULL },
    { 2, 1, "b",      QTFILE, 0644u, NULL },
    { 3, 1, "deep",   QTDIR,  0755u, NULL },
    { 4, 3, "leaf",   QTFILE, 0640u, NULL },
    { 5, 0, "nox",    QTDIR,  0644u, NULL },
    { 6, 5, "sekret", QTFILE, 0600u, NULL },
    { 7, 0, "loop",   QTDIR,  0755u, NULL },
    { 8, 1, "nor",    QTFILE, 0200u, NULL },   // owner write-only: leaf-R deny
    { 9, 0, "xfile",  QTFILE, 0755u, NULL },   // #79: an EXECUTABLE file. The
                                         // only node whose x bit is set while
                                         // it is not a directory -- without it
                                         // the ENOTDIR gate could not be shown
                                         // to be mode-INDEPENDENT (every other
                                         // file here lacks x, so an x-first
                                         // ordering would answer EACCES and
                                         // look equally correct).

    // ---- D-1 symlinks (DISTRO.md section 4). Mode 0777 throughout: POSIX
    // ignores a symlink's own permission bits for traversal, and giving them
    // the widest bits keeps every leg's failure attributable to the FOLLOW
    // logic rather than to a perm denial.
    { 10, 0, "lnb",    QTSYMLINK, 0777u, "a/b" },      // relative, one hop
    { 11, 0, "lnabs",  QTSYMLINK, 0777u, "/a/b" },     // ABSOLUTE -> re-anchor
    { 12, 0, "lndir",  QTSYMLINK, 0777u, "a/deep" },   // link -> a DIRECTORY
    { 13, 0, "lnchain",QTSYMLINK, 0777u, "lnb" },      // link -> link -> file
    { 14, 0, "lnself", QTSYMLINK, 0777u, "lnself" },   // the cycle (ELOOP)
    { 15, 0, "lndead", QTSYMLINK, 0777u, "nosuch" },   // dangling
    { 16, 3, "lnup",   QTSYMLINK, 0777u, "../b" },     // '..'-bearing target:
                                         // resolves a/deep/lnup -> a/b, and
                                         // takes the RESTART arm (a pop needs
                                         // a 1:1 trail).
    { 17, 0, "lnnox",  QTSYMLINK, 0777u, "nox/sekret" },  // through a no-X dir
    { 18, 3, "lnleaf", QTSYMLINK, 0777u, "leaf" },     // MID-RUN link, so a
                                         // pounced run must split at it

    // ---- VIVARIUM section 13 (F1): a fresh, isolated subtree for the
    // pheno-mount symlink-re-anchor regression. `phx` is mounted MPHENO_LINUX in
    // one test; `lnaway` is an ABSOLUTE symlink pointing OUT of phx (to /xfile at
    // the root), so following it re-anchors the resolution out of the pheno
    // mount; `preal` is the plain-file control reached THROUGH the mount. Nothing
    // else in the fixture references phx, so it cannot perturb any other test.
    { 19,  0, "phx",    QTDIR,     0755u, NULL },
    { 20, 19, "lnaway", QTSYMLINK, 0777u, "/xfile" },
    { 21, 19, "preal",  QTFILE,    0644u, NULL },

    // ---- UM (union mounts): an isolated union subtree. `um1` + `um2` are two
    // mount SOURCES with a COLLIDING child name ("shared", distinct qids) plus
    // one unique child each; `umpt` is the empty union mount POINT. Nothing
    // else references them, so they cannot perturb any other test (the phx
    // pattern). The union walk must return the FIRST member's "shared"
    // (order/first-hit), fall through to member 2 for "only2", and miss
    // cleanly on a name in neither.
    { 22,  0, "um1",    QTDIR,  0755u, NULL },
    { 23, 22, "shared", QTFILE, 0644u, NULL },   // collides with qid 26
    { 24, 22, "only1",  QTFILE, 0644u, NULL },
    { 25,  0, "um2",    QTDIR,  0755u, NULL },
    { 26, 25, "shared", QTFILE, 0644u, NULL },   // same NAME as qid 23
    { 27, 25, "only2",  QTFILE, 0644u, NULL },
    { 28,  0, "umpt",   QTDIR,  0755u, NULL },   // the empty union mount point
    // Per-member X-skip: `uma` is a union member the caller cannot search
    // (0600, no owner-x -> X denied even for the SYSTEM owner, as nox proves);
    // `umb` is searchable (0755). Both hold "tgt" -- a union walk must SKIP the
    // unsearchable member and land on umb's tgt, NOT EACCES (Plan 9 union skip,
    // the divergence from a lone directory's EACCES).
    { 29,  0, "uma",    QTDIR,  0600u, NULL },   // X-denied union member
    { 30, 29, "tgt",    QTFILE, 0644u, NULL },   // shadowed (uma unsearchable)
    { 31,  0, "umb",    QTDIR,  0755u, NULL },   // searchable union member
    { 32, 31, "tgt",    QTFILE, 0644u, NULL },   // the one that wins
    { 33,  0, "umpt2",  QTDIR,  0755u, NULL },   // X-skip union mount point
};
#define FIX_LOOP_PATH 7u
// The first symlink qid -- the boundary the fixture walk uses to answer
// readlink and that the tests reference by name.
#define FIX_LNB_PATH    10u
#define FIX_LNSELF_PATH 14u

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

// #50: the create overlay -- a small MUTABLE extension of the static tree so
// SYS_OPEN_CREATE's core can be driven over the REAL resolver (cwd join,
// containment, X-search, the leaf rows). fix_create records nodes here;
// fix_walk_one / fix_stat_qid consult it after the static table. Reset
// per-test. Overlay qids start at 0x100, clear of the static table's.
#define FIXMADE_MAX  8
#define FIXMADE_BASE 0x100u
struct fixmade {
    u64  parent;
    char name[SYS_WALK_OPEN_NAME_MAX + 1];
    u64  path;
    u8   type;
    u32  mode;
};
static struct fixmade g_fixmade[FIXMADE_MAX];
static int g_fixmade_n;
static u64 g_fix_create_last_parent;   // parent qid of the LAST create -- the
                                       // cwd-parity witness reads this
static int g_fix_create_calls;

static void fixmade_reset(void) {
    g_fixmade_n = 0;
    g_fix_create_last_parent = (u64)-1;
    g_fix_create_calls = 0;
}

static const struct fixmade *fixmade_node(u64 path) {
    for (int i = 0; i < g_fixmade_n; i++)
        if (g_fixmade[i].path == path) return &g_fixmade[i];
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
    // #50: the create overlay resolves like any other child.
    for (int i = 0; i < g_fixmade_n; i++) {
        if (g_fixmade[i].parent == cur_path &&
            fix_streq(name, g_fixmade[i].name)) {
            out->path = g_fixmade[i].path;
            out->type = g_fixmade[i].type;
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
    // R2-F1 regression: a 9P server rejects a Twalk (any nwname) from an OPENED
    // fid (Stratum h_walk: is_open -> EINVAL). The fixture must refuse what
    // production refuses, else the union readdir dedup -- which walks earlier
    // members to drop duplicate names -- passes here while silently failing on
    // dev9p (round-1 F1's inverse: a double LOOSER than production).
    if (c->flag & COPEN) return NULL;
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
    u8  ntype;
    u32 nmode;
    if (fn) {
        ntype = fn->type;
        nmode = fn->mode;
    } else {
        // #50: overlay nodes stat like static ones (the A-2d parent check +
        // the dot gates + X-search must see a created directory as a real dir).
        const struct fixmade *m = fixmade_node(qid_path);
        if (!m) return -1;
        ntype = m->type;
        nmode = m->mode;
    }
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;
    out->mode     = ((ntype & QTDIR)     ? T_S_IFDIR :
                     (ntype & QTSYMLINK) ? T_S_IFLNK : T_S_IFREG) | nmode;
    out->nlink    = 1;
    out->qid_path = qid_path;
    out->qid_type = ntype;
    out->blksize  = 4096;
    out->uid      = PRINCIPAL_SYSTEM;
    out->gid      = GID_SYSTEM;
    return 0;
}

// #84: counted, so the dot-arm X-check can PROVE it consumed the pounce's
// carried leaf record instead of issuing a fresh stat (the cost claim in
// stalk_tip_may_search is measured here, not asserted).
static int g_fix_stat_calls;

static int fix_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out) return -1;
    g_fix_stat_calls++;
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

// #50: the fixture create -- the dev9p create CONTRACT over the overlay:
// exclusive (an existing child answers NULL -- the errno-precision channel is
// dev9p-private, so the fixture's failure surfaces as the generic -1; the
// loopback tests in test_dev9p.c own the EEXIST-exact legs), transitions nc
// onto the new node OPENED and returns nc (the reuse-nc contract
// spoor_create_install enforces).
static struct Spoor *fix_create(struct Spoor *nc, const char *name, int omode,
                                u32 perm, u32 gid) {
    (void)gid;
    g_fix_create_calls++;
    if (!nc) return NULL;
    struct Qid q;
    if (fix_walk_one(nc->qid.path, name, &q)) return NULL;   // exists
    if (g_fixmade_n >= FIXMADE_MAX)           return NULL;
    struct fixmade *m = &g_fixmade[g_fixmade_n];
    m->parent = nc->qid.path;
    size_t l = 0;
    while (name[l] != '\0' && l < SYS_WALK_OPEN_NAME_MAX) {
        m->name[l] = name[l];
        l++;
    }
    m->name[l] = '\0';
    m->path = FIXMADE_BASE + (u64)g_fixmade_n;
    m->type = (perm & SYS_WALK_CREATE_DMDIR) ? QTDIR : QTFILE;
    m->mode = perm & 0777u;
    g_fixmade_n++;
    g_fix_create_last_parent = nc->qid.path;
    nc->qid.path = m->path;
    nc->qid.vers = 0;
    nc->qid.type = m->type;
    nc->flag |= COPEN;
    nc->mode  = omode;
    return nc;
}

static void fix_close(struct Spoor *c) { (void)c; /* qid-based: no heap aux */ }

// D-1: the fixture readlink. Counted so a test can prove the resolver issued
// exactly the expansions it should (a chain costs two, a cached answer would
// cost fewer -- there is no target cache at v1.0, and this is what would
// notice one appearing).
static int g_fix_readlink_calls;

static long fix_readlink(struct Spoor *c, char *buf, long n) {
    if (!c || !buf || n <= 0) return -T_E_INVAL;
    g_fix_readlink_calls++;
    const struct fixnode *fn = fix_node(c->qid.path);
    if (!fn || !fn->target) return -T_E_INVAL;   // not a symlink (server-side)
    long i = 0;
    while (fn->target[i] != '\0') {
        if (i >= n) return -T_E_INVAL;           // target exceeds the caller's cap
        buf[i] = fn->target[i];
        i++;
    }
    return i;
}

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

// UM: enumerate `c`'s children as 9P2000.L dirents for the union readdir merge
// tests. Children = fixnodes whose parent == c->qid.path (self-parent root
// excluded). Cookie = a 1-based ordinal in array order (monotonic, like
// devramfs), so union_readdir_run's per-member cursor advances correctly.
static long fix_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    if (!c || !buf) return -1;
    // UM-8 F1 REGRESSION: mirror the PRODUCTION contract -- dev9p issues
    // Treaddir on the fid and Stratum's h_readdir refuses (EINVAL) a fid that
    // was never opened (is_open). The pre-UM-8 union readdir crossed each member
    // via clone_walk_zero (a Twalk clone, never Dev.open'd) then called readdir,
    // so every dev9p member silently vanished (UM-7 F1, the P0). This fixture
    // used to readdir an UNOPENED Spoor happily -- a test double LOOSER than
    // production, so the P0 passed green. Gating on COPEN makes the union readdir
    // tests fail on the old cross-without-open path and pass ONLY on the
    // opened-member snapshot (union_snap).
    if (!(c->flag & COPEN)) return -1;
    u8 *out = (u8 *)buf;
    long len = 0;
    u64  ord = 0;
    int  nfix = (int)(sizeof(g_fix) / sizeof(g_fix[0]));
    for (int i = 0; i < nfix; i++) {
        if (g_fix[i].parent != c->qid.path) continue;
        if (g_fix[i].path   == c->qid.path) continue;   // exclude a self-parent root
        ord++;
        if ((s64)ord <= off) continue;                  // resume: already delivered
        const char *nm = g_fix[i].name;
        u32 nlen = 0; while (nm && nm[nlen]) nlen++;
        long entry = 24 + (long)nlen;
        if (len + entry > n) break;                     // batch full
        u8 *e = out + len;
        for (long b = 0; b < entry; b++) e[b] = 0;
        e[0] = (g_fix[i].type & QTDIR) ? QTDIR : 0;                 // qid.type
        for (int b = 0; b < 8; b++)                                 // qid.path @5..12
            e[5 + b] = (u8)((u64)g_fix[i].path >> (8 * b));
        for (int b = 0; b < 8; b++)                                 // offset cookie @13..20
            e[13 + b] = (u8)(ord >> (8 * b));
        e[21] = (g_fix[i].type & QTDIR) ? 4 : 8;                    // d_type (cosmetic)
        e[22] = (u8)(nlen & 0xff);
        e[23] = (u8)((nlen >> 8) & 0xff);
        for (u32 b = 0; b < nlen; b++) e[24 + b] = (u8)nm[b];
        len += entry;
    }
    return len;
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
    .readlink      = fix_readlink,   // D-1: the expansion RPC
    .create        = fix_create,     // #50: the SYS_OPEN_CREATE battery
    .readdir       = fix_readdir,    // UM: the union readdir merge tests
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
    .readlink      = fix_readlink,   // D-1: the expansion RPC
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
    .readlink      = fix_readlink,   // D-1: the expansion RPC
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
    .readlink      = fix_readlink,   // D-1: the expansion RPC
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

// #79: resolution THROUGH a non-directory answers T_E_NOTDIR, not T_E_NOENT.
//
// The pre-fix behaviour was worse than imprecise -- it was mode-dependent: a
// 0644 file denied the X-search and reported ACCES, while a 0755 file passed it
// and reported NOENT, so the errno turned on a bit that has nothing to do with
// whether the thing can be searched. Both legs below are asserted for exactly
// that reason; `xfile` (0755) exists in the fixture only to make the second one
// expressible.
//
// Non-vacuity: reverting either gate flips a specific leg -- the base gate
// changes `a/b/./x` and the nowa leg, the POUNCE partial arm changes `a/b/x`.
void test_stalk_notdir(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    int e;

    // The POUNCE partial-walk arm: the batch gathers [a, b, x] and the server
    // walks 2 of 3, so the miss's parent is the fused record sts[1] (`b`, a
    // file) rather than the run base.
    e = -12345;
    struct Spoor *q = stalk_err(&p, root, "a/b/x", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/x -> NULL (b is a file)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "through a 0644 file -> NOTDIR (was ACCES)");

    // Since #81 this leg is caught one step EARLIER -- the '.' token now gates
    // the tip itself, so `b` is rejected before `x` is ever considered. Same
    // verdict, different gate; the base gate's own witness is the nowa leg
    // below (reverting line "parent->qid.type & QTDIR" flips it to ACCES).
    e = -12345;
    q = stalk_err(&p, root, "a/b/./x", 7, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/./x -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "'.' after a file -> NOTDIR (#81 gate)");

    // Mode-independence: 0755 sets the x bit, so the X-search would have PASSED
    // and the old answer was NOENT -- a different errno for the same situation.
    e = -12345;
    q = stalk_err(&p, root, "xfile/y", 7, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "xfile/y -> NULL (xfile is a file)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "through a 0755 file -> NOTDIR (was NOENT)");

    // The per-component loop reaches the same verdict (pounce/loop parity).
    struct Spoor *root_nowa = fix_root_nowa();
    TEST_ASSERT(root_nowa != NULL, "fix_root_nowa");
    e = -12345;
    q = stalk_err(&p, root_nowa, "a/b/x", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/x (no walk_attrs) -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "per-component loop agrees: NOTDIR");
    spoor_unref(root_nowa);

    // The gate must not fire on a file as the FINAL component -- it guards
    // searching THROUGH a node, never naming one. This is the regression that
    // a too-eager gate would trip.
    struct Spoor *ok = stalk_err(&p, root, "a/b", 3, STALK_OPEN, 0, &e);
    TEST_ASSERT(ok != NULL, "a/b still resolves (a file as the leaf is not gated)");
    spoor_clunk(ok);

    // A real X-search denial is still ACCES: `nox` IS a directory (0644), so
    // the type gate passes and the permission check remains the authority.
    // Ordering type-before-permission must not have swallowed this.
    e = -12345;
    struct Spoor *deny = stalk_err(&p, root, "nox/sekret", 10, STALK_OPEN, 0, &e);
    TEST_ASSERT(deny == NULL, "nox/sekret -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "a directory without x is still ACCES, not NOTDIR");

    spoor_unref(root);
}

// #81: "." and ".." are PATH COMPONENTS, so the position they resolve in must
// be a directory. stalk handles both tokens lexically -- they never reach
// Dev.walk -- so #79's gate (which sits on the real-component path) never saw
// them and a file tip silently accepted both: `a/b/..` popped back to `a` and
// `a/b/.` handed back `b`.
//
// Non-vacuity: every ENOTDIR leg below RESOLVED before the fix (returning a
// Spoor, not an error), so reverting either gate turns the assertion from
// "NULL + NOTDIR" into a successful walk.
void test_stalk_dot_notdir(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    int e;
    struct Spoor *q;

    // '..' out of a file. Pre-fix this popped `b` and returned `a` (qid 1) --
    // a successful resolution of a path POSIX rejects.
    e = -12345;
    q = stalk_err(&p, root, "a/b/..", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/.. -> NULL (b is a file)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "'..' out of a file -> NOTDIR");

    // '.' at a file. Pre-fix this returned `b` itself (qid 2).
    e = -12345;
    q = stalk_err(&p, root, "a/b/.", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/. -> NULL (b is a file)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "'.' at a file -> NOTDIR");

    // Mode-independence, the #79 property carried forward: `xfile` is 0755, so
    // an x-bit-first ordering would look equally correct here. The gate reads
    // qid.type, so both files answer the same.
    e = -12345;
    q = stalk_err(&p, root, "xfile/..", 8, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "xfile/.. -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "'..' out of a 0755 file -> NOTDIR");
    e = -12345;
    q = stalk_err(&p, root, "xfile/.", 7, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "xfile/. -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "'.' at a 0755 file -> NOTDIR");

    // The gate must not fire on a DIRECTORY -- these are the regressions a
    // too-eager gate would trip, and they are the overwhelmingly common case.
    q = stalk_err(&p, root, "a/deep/.", 8, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/deep/. still resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)3, "a/deep/. -> qid 3 (deep)");
    spoor_clunk(q);

    q = stalk_err(&p, root, "a/deep/../b", 11, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/deep/../b still resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "a/deep/../b -> qid 2 (b)");
    spoor_clunk(q);

    // I-28 containment is UNCHANGED: the gate can only fail a resolution, never
    // move a pop further up. `start` is a directory, so a leading '..' run is
    // still the no-op that clamps at the base.
    q = stalk_err(&p, root, "../../a", 7, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "../../a still contained + resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)1, "../../a -> qid 1 (a), never escaped");
    spoor_clunk(q);

    // depth 0 with a NON-directory base -- the openat(file_fd, ...) shape, and
    // the case the '..' clamp made invisible (at depth 0 the pop is a no-op, so
    // pre-fix BOTH tokens simply handed the file straight back).
    struct Spoor *bfile = stalk(&p, root, "a/b", 3, STALK_WALK, 0);
    TEST_ASSERT(bfile != NULL, "resolve a/b as a base");
    TEST_EXPECT_EQ((u64)(bfile->qid.type & QTDIR), (u64)0, "a/b is not a directory");
    e = -12345;
    q = stalk_err(&p, bfile, ".", 1, STALK_WALK, 0, &e);
    TEST_ASSERT(q == NULL, "'.' from a file base -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "'.' at depth 0 on a file -> NOTDIR");
    e = -12345;
    q = stalk_err(&p, bfile, "..", 2, STALK_WALK, 0, &e);
    TEST_ASSERT(q == NULL, "'..' from a file base -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "'..' at depth 0 on a file -> NOTDIR");
    spoor_clunk(bfile);

    // The per-component loop (no walk_attrs) reaches the same verdict.
    struct Spoor *root_nowa = fix_root_nowa();
    TEST_ASSERT(root_nowa != NULL, "fix_root_nowa");
    e = -12345;
    q = stalk_err(&p, root_nowa, "a/b/..", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/.. (no walk_attrs) -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "per-component loop agrees: NOTDIR");
    spoor_unref(root_nowa);

    spoor_unref(root);
}

// #82: a trailing '/' asserts the path names a DIRECTORY (POSIX 4.13). The
// tokenizer collapses separator runs, so pre-fix the trailing '/' was simply
// dropped and `a/b/` resolved the file. THREE gate sites, because two success
// exits never reach the quarry -- each leg below drives exactly one of them, so
// removing any single gate fails a distinct assertion.
void test_stalk_trailing_slash(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    int e;
    struct Spoor *q;
    struct t_stat st;
    g_fix_co_enable = false;   // site A/C legs take the normal path

    // -- Site A: the quarry gate (the ordinary resolution path).
    u64 live_before = spoor_total_allocated() - spoor_total_freed();
    e = -12345;
    q = stalk_err(&p, root, "a/b/", 4, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/ -> NULL (b is a file)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "trailing slash on a file -> NOTDIR");
    TEST_EXPECT_EQ(spoor_total_allocated() - spoor_total_freed(), live_before,
                   "the quarry gate's failure path leaks no Spoor");

    // A run of trailing separators is the same assertion.
    e = -12345;
    q = stalk_err(&p, root, "a/b///", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/// -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "a run of trailing slashes -> NOTDIR");

    // Mode-independence (#79's property): xfile is 0755, so an x-bit-first
    // ordering would look correct on 0644 `b` alone. Both files answer NOTDIR.
    e = -12345;
    q = stalk_err(&p, root, "xfile/", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "xfile/ -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "trailing slash on a 0755 file -> NOTDIR");

    // O_PATH (STALK_WALK) is gated too -- Linux answers ENOTDIR there as well.
    e = -12345;
    q = stalk_err(&p, root, "a/b/", 4, STALK_WALK, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/ (O_PATH) -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "O_PATH is gated as well");

    // The DIRECTORY case must keep working -- the regression a too-eager gate
    // trips, and overwhelmingly the common shape (`ls /usr/`, `$(DIR)/`).
    q = stalk_err(&p, root, "a/deep/", 7, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/deep/ still resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)3, "a/deep/ -> qid 3 (deep)");
    spoor_clunk(q);
    q = stalk_err(&p, root, "a/", 2, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/ still resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)1, "a/ -> qid 1");
    spoor_clunk(q);

    // "/" and "//" are EXEMPT: POSIX scopes the rule to a pathname with at
    // least one non-'/' character, and they have no component before the
    // trailing run. (This is why the discriminator scans back instead of
    // testing the last byte.)
    q = stalk_err(&p, root, "/", 1, STALK_WALK, 0, &e);
    TEST_ASSERT(q != NULL, "\"/\" resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)0, "\"/\" -> the root");
    spoor_clunk(q);
    q = stalk_err(&p, root, "//", 2, STALK_WALK, 0, &e);
    TEST_ASSERT(q != NULL, "\"//\" resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)0, "\"//\" -> the root");
    spoor_clunk(q);

    // A trailing slash on a MISSING path is still ENOENT -- the gate must not
    // pre-empt a real walk miss.
    e = -12345;
    q = stalk_err(&p, root, "a/nope/", 7, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/nope/ -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "a missing component still reports NOENT");

    // -- Site C: the STALK_STAT walk-query fast path, which returns from the
    // fused leaf record WITHOUT ever materializing a quarry.
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "a/b/", 4, 0, &st, &e), (u64)-1,
                   "stat a/b/ fails");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "the stat query path gates too");
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "a/deep/", 7, 0, &st, &e), (u64)0,
                   "stat a/deep/ still succeeds");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)3, "and reports the directory");

    // -- Site B: the FID-LIFECYCLE cached-open arm, the other quarry-skipping
    // exit. Assert the arm actually RAN, else this leg silently re-tests site A.
    g_fix_co_enable = true;
    g_fix_co_calls = 0;
    live_before = spoor_total_allocated() - spoor_total_freed();
    e = -12345;
    q = stalk_err(&p, root, "a/b/", 4, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/ via the cached-open arm -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "the cached-open arm gates too");
    TEST_EXPECT_EQ((u64)g_fix_co_calls, 1ull,
                   "the cached-open arm ran (this leg is not site A in disguise)");
    TEST_EXPECT_EQ(spoor_total_allocated() - spoor_total_freed(), live_before,
                   "the cached-open gate clunks its minted Spoor");
    g_fix_co_enable = false;

    // The per-component loop (no walk_attrs) reaches the same verdict.
    struct Spoor *root_nowa = fix_root_nowa();
    TEST_ASSERT(root_nowa != NULL, "fix_root_nowa");
    e = -12345;
    q = stalk_err(&p, root_nowa, "a/b/", 4, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/ (no walk_attrs) -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "per-component loop agrees: NOTDIR");
    spoor_unref(root_nowa);

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

// #81: the gate reads the tip's UNCROSSED qid.type. The discriminating case is
// a '.' on a mount point under STALK_MOUNT, where the mount point and the
// mounted root are DIFFERENT nodes and the amode deliberately suppresses the
// quarry cross: crossing to read the type would leak the mounted root out as
// the result and break MREPL re-keying. `/mnt/.` must mean `/mnt`, exactly.
void test_stalk_dot_notdir_mount(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *src = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(src != NULL && mp != NULL, "resolve src + mount point");
    TEST_EXPECT_EQ(mount(p.territory, src, mp, 0), 0, "mount a onto loop");

    // STALK_MOUNT: "loop" yields the mount POINT (qid 7, no cross). "loop/."
    // must yield the same -- a crossing gate would answer qid 1 (a-root).
    struct Spoor *bare = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    struct Spoor *dot  = stalk(&p, root, "loop/.", 6, STALK_MOUNT, 0);
    TEST_ASSERT(bare != NULL && dot != NULL, "resolve loop and loop/. (MOUNT)");
    TEST_EXPECT_EQ((u64)bare->qid.path, (u64)7, "STALK_MOUNT loop -> mount point");
    TEST_EXPECT_EQ((u64)dot->qid.path, (u64)bare->qid.path,
                   "loop/. == loop under STALK_MOUNT (tip read UNCROSSED)");
    spoor_clunk(bare);
    spoor_clunk(dot);

    // STALK_OPEN still crosses the quarry, so "loop/." tracks "loop" there too.
    struct Spoor *q = stalk(&p, root, "loop/.", 6, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop/. (OPEN)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)1, "loop/. crosses to a-root (qid 1)");
    spoor_clunk(q);

    // '..' off a mount point pops the trail entry, landing above it in the
    // ORIGINAL tree (Plan 9): the fixture root, not anything inside `a`.
    q = stalk(&p, root, "loop/..", 7, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve loop/..");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)0, "loop/.. -> the original root (qid 0)");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(src);
    spoor_clunk(mp);
    spoor_unref(root);
}

// #84: '.' and '..' are lookups performed IN the tip, so they need X there --
// the check the real-component arm has always made and the dot arms skipped.
//
// Measured on a POSIX host (non-root, owner of a `chmod 000` directory d):
// stat("d") SUCCEEDS (that lookup happens in d's PARENT) while stat("d/."),
// stat("d/..") and stat("d/x") are ALL EACCES, and fstatat(dirfd_of_d, ".")
// is EACCES too -- so depth 0 counts, with `start` as the subject. `nox`
// (QTDIR, 0644 -- owner rw-, no x) is the fixture's twin of that directory.
void test_stalk_dot_xsearch(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");
    int e;

    // Pre-#84 these two RESOLVED: '..' popped back to the root (qid 0) and '.'
    // stayed on nox (qid 5) -- while the sibling `nox/sekret` was denied. That
    // asymmetry is the bug.
    struct Spoor *q = stalk_err(&p, root, "nox/..", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "nox/.. -> NULL (no x on nox)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "'..' out of a no-x dir -> ACCES");

    q = stalk_err(&p, root, "nox/.", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "nox/. -> NULL (no x on nox)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "'.' in a no-x dir -> ACCES");

    // A trailing dot is not special: the component still resolves IN nox.
    q = stalk_err(&p, root, "nox/../a", 8, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "nox/../a -> NULL (denied before the pop)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "denied mid-path, never NOENT");

    // DEPTH 0: the subject is `start`. Reaching nox as a base needs x on the
    // ROOT only (0755), which is exactly why stat("d000") succeeds on POSIX --
    // so this base is obtainable, and then '.' / '..' in it must deny.
    struct Spoor *noxdir = stalk(&p, root, "nox", 3, STALK_WALK, 0);
    TEST_ASSERT(noxdir != NULL, "nox itself resolves (x on root suffices)");

    q = stalk_err(&p, noxdir, ".", 1, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "openat(nox, \".\") -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "depth-0 '.' -> ACCES");

    q = stalk_err(&p, noxdir, "..", 2, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "openat(nox, \"..\") -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "depth-0 '..' -> ACCES");
    spoor_clunk(noxdir);

    // ORDER: type BEFORE permission. `b` is a 0644 FILE, so BOTH gates would
    // fire -- POSIX answers ENOTDIR (measured: a 0000 regular file gives
    // ENOTDIR for both tokens, never EACCES). Putting the X-check first would
    // flip this to ACCES.
    q = stalk_err(&p, root, "a/b/..", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/.. -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "type wins over permission ('..')");
    q = stalk_err(&p, root, "a/b/.", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a/b/. -> NULL");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "type wins over permission ('.')");

    // CONTROL: a searchable (0755) directory is unaffected in both arms.
    q = stalk_err(&p, root, "a/deep/.", 8, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/deep/. still resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)3, "a/deep/. -> qid 3");
    spoor_clunk(q);
    q = stalk_err(&p, root, "a/deep/../b", 11, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/deep/../b still resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "a/deep/../b -> qid 2");
    spoor_clunk(q);

    // COST -- two A/Bs, MEASURED. Each compares the same resolution with and
    // without a trailing '.', so the delta is attributable to this gate alone
    // (an absolute count would pin unrelated resolver internals: the STALK_OPEN
    // final-hop R/W check stats too).
    //
    // (1) POUNCE path: a '.' breaks a run but does not disable pouncing, so the
    // run that produced the tip fused its attrs into the walk and `carried`
    // describes exactly the object this gate judges -> the check is FREE.
    g_fix_stat_calls = 0;
    q = stalk_err(&p, root, "a/deep", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/deep resolves (cost baseline)");
    int plain = g_fix_stat_calls;
    spoor_clunk(q);

    g_fix_stat_calls = 0;
    q = stalk_err(&p, root, "a/deep/.", 8, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "a/deep/. resolves (cost probe)");
    TEST_EXPECT_EQ((u64)g_fix_stat_calls, (u64)plain,
                   "'.' adds NO stat -- the gate consumed the carried record");
    spoor_clunk(q);

    // (2) PER-COMPONENT path (the nowa twin: no walk_attrs -- the shape EVERY
    // '..' path takes, since path_has_dotdot disables pouncing). No carried
    // record here, so the gate costs exactly ONE stat: the same call the
    // real-component arm makes at this same position. Not a new cost class --
    // and on dev9p that call is a Larder attr-cache hit (that cache exists for
    // precisely this traffic; see dev9p_stat_native).
    struct Spoor *rn = fix_root_nowa();
    TEST_ASSERT(rn != NULL, "fix_root_nowa");
    g_fix_stat_calls = 0;
    q = stalk_err(&p, rn, "a/deep", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "nowa a/deep resolves");
    int plain_nowa = g_fix_stat_calls;
    spoor_clunk(q);

    g_fix_stat_calls = 0;
    q = stalk_err(&p, rn, "a/deep/.", 8, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "nowa a/deep/. resolves");
    TEST_EXPECT_EQ((u64)g_fix_stat_calls, (u64)(plain_nowa + 1),
                   "no carried record -> exactly one stat, an ordinary hop's cost");
    spoor_clunk(q);
    spoor_unref(rn);

    spoor_unref(root);
}

// #82: the trailing-slash gate reads the CROSSED quarry -- the OPPOSITE of
// #81's dot gate one test up, and deliberately so. The two ask different
// questions of the same field: `.`/`..` are about WHERE RESOLUTION STANDS
// (so `/mnt/.` must equal `/mnt`, uncrossed), a trailing slash about WHAT THE
// PATH NAMES (which is the crossed result). The distinction is observable, not
// academic: territory.c's mount() has no type check, so a mount point and its
// mounted root need not agree -- and then gating the uncrossed point is wrong
// in BOTH directions. Both are built here.
void test_stalk_trailing_slash_mount(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");
    g_fix_co_enable = false;

    int e;
    struct Spoor *q;

    // (1) A FILE mounted over a DIRECTORY mount point. `loop` now RESOLVES to
    //     a file, so `loop/` names a file -> ENOTDIR. A gate on the uncrossed
    //     point would read QTDIR and wrongly ACCEPT.
    struct Spoor *bfile = stalk(&p, root, "a/b", 3, STALK_WALK, 0);
    struct Spoor *mpdir = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    TEST_ASSERT(bfile != NULL && mpdir != NULL, "resolve a/b + loop");
    TEST_EXPECT_EQ((u64)(bfile->qid.type & QTDIR), 0ull, "a/b is a file");
    TEST_EXPECT_EQ((u64)(mpdir->qid.type & QTDIR), (u64)QTDIR, "loop is a directory");
    TEST_EXPECT_EQ(mount(p.territory, bfile, mpdir, 0), 0, "mount the file onto loop");

    e = -12345;
    q = stalk_err(&p, root, "loop/", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "loop/ -> NULL (the mount crosses to a file)");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR,
                   "file-over-dir: loop/ is NOTDIR (the CROSSED type decides)");

    // Without the slash it still resolves -- to the mounted file.
    q = stalk_err(&p, root, "loop", 4, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "loop still resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, 2ull, "loop -> the mounted file (qid 2)");
    spoor_clunk(q);

    // (2) The converse: a DIRECTORY mounted over a FILE mount point. `xfile`
    //     now RESOLVES to a directory, so `xfile/` is LEGAL. A gate on the
    //     uncrossed point would read QTFILE and wrongly REJECT.
    struct Spoor *adir  = stalk(&p, root, "a", 1, STALK_WALK, 0);
    struct Spoor *mpfil = stalk(&p, root, "xfile", 5, STALK_MOUNT, 0);
    TEST_ASSERT(adir != NULL && mpfil != NULL, "resolve a + xfile");
    TEST_EXPECT_EQ((u64)(mpfil->qid.type & QTDIR), 0ull, "xfile is a file");
    TEST_EXPECT_EQ(mount(p.territory, adir, mpfil, 0), 0, "mount a onto xfile");

    q = stalk_err(&p, root, "xfile/", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q != NULL, "xfile/ resolves (the mount crosses to a directory)");
    TEST_EXPECT_EQ((u64)q->qid.path, 1ull,
                   "dir-over-file: xfile/ -> the mounted dir (qid 1)");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(bfile);
    spoor_clunk(mpdir);
    spoor_clunk(adir);
    spoor_clunk(mpfil);
    spoor_unref(root);
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

// VIVARIUM section 13 (F1, the holotype's P2): crossed_pheno is a SET-ONLY
// accumulator, and the resolver's `restart:` (a symlink re-anchor / '..'-rebuild)
// must reset it -- otherwise a resolution that crosses a pheno-mount and THEN
// follows an absolute symlink OUT of it would stamp the NATIVE target
// PHENO_LINUX, falsifying territory.h's "the SAME file reached by another path is
// native." This drives the FULL resolver: phx is mounted MPHENO_LINUX at loop;
// loop/lnaway crosses the pheno-mount then re-anchors to /xfile (a native file
// OUT of the mount) -> crossed_pheno must be false; loop/preal reaches a plain
// file THROUGH the mount with no re-anchor -> crossed_pheno stays true. Without
// the restart: reset, loop/lnaway reports true (fails-without-fix).
void test_stalk_pheno_symlink_reanchor(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");
    if (!root || !p.territory) return;
    // The absolute symlink (lnaway -> /xfile) re-anchors at the caller's OWN
    // Territory root (stalk.c:393, I-28), so the root must be established --
    // cross_setup leaves it NULL, exactly as the lnabs leg of symlink_follow does.
    TEST_EXPECT_EQ(territory_chroot(p.territory, root), 0, "chroot to fixture root");

    struct Spoor *src = stalk(&p, root, "phx", 3, STALK_WALK, 0);
    struct Spoor *mp  = stalk(&p, root, "loop", 4, STALK_MOUNT, 0);
    int mrc = (src && mp) ? mount(p.territory, src, mp, MPHENO_LINUX) : -1;

    // Leg 1 (the fix): loop/lnaway -> cross loop (pheno) -> lnaway -> re-anchor
    // to /xfile (OUT). Caller inits crossed_pheno false, as exec_resolve does.
    int  e1 = 0;
    bool pheno_link = false;
    struct Spoor *q1 = (mrc == 0)
        ? stalk_exec(&p, root, "loop/lnaway", 11, STALK_OPEN, 0, &e1, &pheno_link)
        : NULL;
    // Leg 2 (control): loop/preal -> a plain file UNDER the pheno-mount, no
    // re-anchor -> crossed_pheno true.
    int  e2 = 0;
    bool pheno_plain = false;
    struct Spoor *q2 = (mrc == 0)
        ? stalk_exec(&p, root, "loop/preal", 10, STALK_OPEN, 0, &e2, &pheno_plain)
        : NULL;

    // Design D (VIVARIUM 13.10.5, review F8.2 = self-audit SA-3): the restart:
    // reset is a SEED from the resolving Territory's declaration. Leg 0 is the
    // seed's own control BEFORE any declaration: a resolution that crosses NO
    // mount (xfile) reports false. Then declare the Territory Linux (the
    // container's namespace-level declaration) and re-run: leg 3 re-anchors OUT
    // of the pheno-mount exactly as leg 1 did and must now report TRUE -- the
    // seed survives the re-anchor because the reset IS the seed (a seed hoisted
    // above restart: would drop it here); leg 4 crosses no mount and must report
    // TRUE on the seed alone.
    int  e0 = 0;
    bool pheno_nomount_undecl = false;
    struct Spoor *q0 = stalk_exec(&p, root, "xfile", 5, STALK_OPEN, 0, &e0,
                                  &pheno_nomount_undecl);
    territory_declare_linux(p.territory);
    int  e3 = 0;
    bool pheno_link_decl = false;
    struct Spoor *q3 = (mrc == 0)
        ? stalk_exec(&p, root, "loop/lnaway", 11, STALK_OPEN, 0, &e3, &pheno_link_decl)
        : NULL;
    int  e4 = 0;
    bool pheno_nomount_decl = false;
    struct Spoor *q4 = stalk_exec(&p, root, "xfile", 5, STALK_OPEN, 0, &e4,
                                  &pheno_nomount_decl);

    // Observe, THEN tear down, THEN assert (TEST_ASSERT returns).
    bool q1ok = (q1 != NULL), q2ok = (q2 != NULL);
    bool q0ok = (q0 != NULL), q3ok = (q3 != NULL), q4ok = (q4 != NULL);
    if (q0) spoor_clunk(q0);
    if (q3) spoor_clunk(q3);
    if (q4) spoor_clunk(q4);
    u64  q1qid = q1 ? (u64)q1->qid.path : (u64)-1;
    u64  q2qid = q2 ? (u64)q2->qid.path : (u64)-1;
    if (q1) spoor_clunk(q1);
    if (q2) spoor_clunk(q2);
    territory_unref(p.territory);
    if (src) spoor_clunk(src);
    if (mp)  spoor_clunk(mp);
    spoor_unref(root);

    TEST_EXPECT_EQ(mrc, 0, "mount phx at loop MPHENO_LINUX");
    TEST_ASSERT(q1ok, "loop/lnaway resolves (re-anchored to /xfile)");
    TEST_ASSERT(q2ok, "loop/preal resolves (through the pheno-mount)");
    TEST_EXPECT_EQ(q1qid, (u64)9,
        "loop/lnaway -> /xfile (qid 9, OUT of the pheno-mount)");
    TEST_EXPECT_EQ(q2qid, (u64)21,
        "loop/preal -> phx/preal (qid 21, via the pheno-mount)");
    TEST_ASSERT(pheno_link == false,
        "F1: a symlink re-anchor OUT of a pheno-mount RESETS crossed_pheno "
        "(the native /xfile is not stamped PHENO_LINUX)");
    TEST_ASSERT(pheno_plain == true,
        "CONTROL: a plain file reached THROUGH the pheno-mount keeps "
        "crossed_pheno (final-location, no re-anchor)");
    TEST_ASSERT(q0ok && q3ok && q4ok, "the Design D legs resolve");
    TEST_ASSERT(pheno_nomount_undecl == false,
        "SEED CONTROL: no crossing + no declaration -> false (rule 3)");
    TEST_ASSERT(pheno_link_decl == true,
        "Design D: a declared Territory's seed SURVIVES the re-anchor OUT of the "
        "pheno-mount (the restart: reset is the seed, not a false)");
    TEST_ASSERT(pheno_nomount_decl == true,
        "Design D: a declared Territory decides Linux with NO crossing at all");
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
// UM (union mounts): the union WALK. Grafts multiple sources at one point and
// drives the REAL resolver through the union -- declared-order search,
// first-hit, fallthrough on miss, per-member X-skip, clean miss. The spec
// (specs/territory.tla WalkFirstHit / OrderCorrect + the buggy cfgs) proves the
// model; these prove the impl matches it.
// =============================================================================

void test_stalk_union_walk(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");
    // Seed the root Path (the qid-Dev fixture carries none; the real devramfs/
    // dev9p roots are attach-seeded) so the resolver accumulates names and the
    // union-child Path regression below is observable.
    root->path = path_make_root();
    TEST_ASSERT(root->path != NULL, "seed root path /");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");

    // Union [um1, um2]: um1 MBEFORE (searched first), um2 MAFTER (last).
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MBEFORE), 0, "mount um1 MBEFORE umpt");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MAFTER),  0, "mount um2 MAFTER umpt");

    // Leak accounting spans the four resolves (the union path adds union_snap
    // ref/clunk + the helper's per-member clone/clunk -- balance them).
    u64 live_before = spoor_total_allocated() - spoor_total_freed();

    // First-hit: "shared" is in BOTH members; member 0 (um1) wins -> qid 23.
    struct Spoor *q = stalk(&p, root, "umpt/shared", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve umpt/shared");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)23, "union first-hit -> um1's shared (qid 23)");
    // UM-8 regression (#66/I-33): the union child takes the MOUNT-POINT name
    // (/umpt), not the winning member's internal name (/um1). A stalk_cross_src
    // that omits the transplant yields "/um1/shared" -- which crashes joey's
    // V-4a-0 exe-path assert at boot ("got '/ptyfs' want '/bin/ptyfs'").
    TEST_ASSERT(q->path != NULL && fix_streq(q->path->s, "/umpt/shared"),
        "union child Path = /umpt/shared (mount-point name, not the member's)");
    spoor_clunk(q);

    // Member-0 hit: "only1" exists only in um1 -> qid 24.
    q = stalk(&p, root, "umpt/only1", 10, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve umpt/only1");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)24, "union member-0 hit (qid 24)");
    spoor_clunk(q);

    // Fallthrough: "only2" exists only in um2 -> um1 misses, um2 wins qid 27.
    q = stalk(&p, root, "umpt/only2", 10, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve umpt/only2 (fallthrough)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)27, "union fallthrough -> um2's only2 (qid 27)");
    spoor_clunk(q);

    // Clean miss: neither member has "nosuch".
    int err = 0;
    q = stalk_err(&p, root, "umpt/nosuch", 11, STALK_OPEN, 0, &err);
    TEST_ASSERT(q == NULL, "umpt/nosuch misses");
    TEST_EXPECT_EQ(err, T_E_NOENT, "union all-miss -> ENOENT");

    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before, "no Spoor leak across union resolves");

    territory_unref(p.territory);
    spoor_clunk(um1);
    spoor_clunk(um2);
    spoor_clunk(pt);
    spoor_unref(root);
}

// The ORDER flip: mounting um2 MBEFORE (prepend -> searched first) makes um2's
// "shared" win instead of um1's -- the declared order is load-bearing (the
// OrderCorrect / BUGGY_MOUNT_ORDER counterexample, at runtime).
void test_stalk_union_order(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");

    // um1 MAFTER, then um2 MBEFORE -> union [um2, um1] (MBEFORE prepends).
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MAFTER),  0, "mount um1 MAFTER umpt");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MBEFORE), 0, "mount um2 MBEFORE umpt");

    // um2 is now searched first -> "shared" resolves to um2's (qid 26), not 23.
    struct Spoor *q = stalk(&p, root, "umpt/shared", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve umpt/shared (order flipped)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)26, "MBEFORE order -> um2's shared (qid 26)");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(um1);
    spoor_clunk(um2);
    spoor_clunk(pt);
    spoor_unref(root);
}

// Per-member X-skip: a union member the caller cannot search is SKIPPED, and
// the walk lands on the next searchable member's entry -- NOT EACCES (the union
// divergence from a lone directory, where an X denial is fatal).
void test_stalk_union_xskip(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *uma = stalk(&p, root, "uma",   3, STALK_WALK,  0);  // 0600, no X
    struct Spoor *umb = stalk(&p, root, "umb",   3, STALK_WALK,  0);  // 0755
    struct Spoor *pt  = stalk(&p, root, "umpt2", 5, STALK_MOUNT, 0);
    TEST_ASSERT(uma && umb && pt, "resolve uma + umb + umpt2");

    // union [uma, umb]: uma (unsearchable) first, umb (searchable) second.
    TEST_EXPECT_EQ(mount(p.territory, uma, pt, MBEFORE), 0, "mount uma MBEFORE umpt2");
    TEST_EXPECT_EQ(mount(p.territory, umb, pt, MAFTER),  0, "mount umb MAFTER umpt2");

    // "tgt" is in BOTH members; uma is unsearchable (0600) -> SKIPPED, umb wins.
    struct Spoor *q = stalk(&p, root, "umpt2/tgt", 9, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve umpt2/tgt (uma X-skipped)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)32,
                   "X-denied member skipped -> umb's tgt (qid 32)");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(uma);
    spoor_clunk(umb);
    spoor_clunk(pt);
    spoor_unref(root);
}

// =============================================================================
// UM (union mounts) -- union READDIR merge/dedup/pagination (UM-5).
//
// The merge logic lives in kernel/syscall.c (union_readdir_run, non-static like
// viv_dirent64_encode_run so the byte-level behavior is unit-testable). These
// tests build a real union over the fixture (fix_readdir emits each member's
// children as 9P2000.L dirents), open it through the real resolver (which tags
// union_snap), and assert the merged stream: dedup first-member-wins, member
// declared order, 1-based ordinal cookies, and correct paginated resume.
// =============================================================================

extern s64 union_readdir_run(struct Proc *p, struct Spoor *c, u8 *out, long want,
                             u64 in_ordinal);

// Parse the idx-th 9P2000.L dirent in buf[0..len). Returns its byte span (> 0)
// or 0 if idx is past the last complete entry. Fills qid_path / cookie / name.
static long urd_entry(const u8 *buf, long len, int idx,
                      u64 *qid_path, u64 *cookie, char *name, u32 namecap) {
    long pos = 0;
    for (int i = 0; ; i++) {
        if (pos + 24 > len) return 0;
        u32  nlen  = (u32)buf[pos + 22] | ((u32)buf[pos + 23] << 8);
        long entry = 24 + (long)nlen;
        if (pos + entry > len) return 0;
        if (i == idx) {
            u64 q = 0, ck = 0;
            for (int b = 0; b < 8; b++) q  |= (u64)buf[pos + 5  + b] << (8 * b);
            for (int b = 0; b < 8; b++) ck |= (u64)buf[pos + 13 + b] << (8 * b);
            if (qid_path) *qid_path = q;
            if (cookie)   *cookie   = ck;
            if (name) {
                u32 c = 0;
                for (; c < nlen && c + 1 < namecap; c++) name[c] = (char)buf[pos + 24 + c];
                name[c] = '\0';
            }
            return entry;
        }
        pos += entry;
    }
}

static bool urd_name_eq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

void test_stalk_union_readdir(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MBEFORE), 0, "um1 MBEFORE umpt");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MAFTER),  0, "um2 MAFTER umpt");

    // Open the union directory: the resolver tags union_snap and the fd's own
    // identity is member[0] (um1 root, qid 22).
    struct Spoor *q = stalk(&p, root, "umpt", 4, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "open union dir umpt");
    TEST_ASSERT(q->union_snap != NULL, "STALK_OPEN of a union tags union_snap");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)22, "opened union identity = member[0] (qid 22)");

    u64 live_before = spoor_total_allocated() - spoor_total_freed();

    u8  buf[512];
    s64 got = union_readdir_run(&p, q, buf, (long)sizeof(buf), 0);
    TEST_ASSERT(got > 0, "union readdir returns a run");

    // Merged-dedup sequence [shared(um1,qid23), only1(qid24), only2(um2,qid27)];
    // um2's "shared"(qid26) is DROPPED (first-member-wins). Ordinals 1,2,3.
    u64 qp = 0, ck = 0; char nm[64];
    TEST_ASSERT(urd_entry(buf, (long)got, 0, &qp, &ck, nm, sizeof(nm)) > 0, "entry 0");
    TEST_ASSERT(urd_name_eq(nm, "shared"), "entry 0 name = shared");
    TEST_EXPECT_EQ(qp, (u64)23, "shared dedup first-wins -> um1's qid 23");
    TEST_EXPECT_EQ(ck, (u64)1,  "entry 0 ordinal cookie = 1");

    TEST_ASSERT(urd_entry(buf, (long)got, 1, &qp, &ck, nm, sizeof(nm)) > 0, "entry 1");
    TEST_ASSERT(urd_name_eq(nm, "only1"), "entry 1 name = only1");
    TEST_EXPECT_EQ(qp, (u64)24, "only1 -> qid 24");
    TEST_EXPECT_EQ(ck, (u64)2,  "entry 1 ordinal = 2");

    TEST_ASSERT(urd_entry(buf, (long)got, 2, &qp, &ck, nm, sizeof(nm)) > 0, "entry 2");
    TEST_ASSERT(urd_name_eq(nm, "only2"), "entry 2 name = only2 (um2 fallthrough)");
    TEST_EXPECT_EQ(qp, (u64)27, "only2 -> qid 27");
    TEST_EXPECT_EQ(ck, (u64)3,  "entry 2 ordinal = 3");

    // Exactly three (um2's shared deduped away): a 4th entry is absent.
    TEST_EXPECT_EQ(urd_entry(buf, (long)got, 3, &qp, &ck, nm, sizeof(nm)), (long)0,
                   "exactly 3 merged entries");

    // Resume past the last ordinal -> end-of-directory.
    TEST_EXPECT_EQ((long)union_readdir_run(&p, q, buf, (long)sizeof(buf), 3), (long)0,
                   "resume past last ordinal -> EOD");

    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before, "no Spoor leak across union readdir");

    spoor_clunk(q);
    territory_unref(p.territory);
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);
    spoor_unref(root);
}

// Paginated resume: a `want` that holds only part of the merged stream splits
// it across calls; the ordinal carries the cursor with no gap and no dup.
void test_stalk_union_readdir_paginate(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MBEFORE), 0, "um1 MBEFORE");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MAFTER),  0, "um2 MAFTER");

    struct Spoor *q = stalk(&p, root, "umpt", 4, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL && q->union_snap != NULL, "open + tag union");

    // shared=24+6=30, only1=24+5=29, only2=24+5=29. want=60 holds page 1
    // (shared+only1 = 59), not only2 (would be 88).
    u8  buf[512];
    u64 qp = 0, ck = 0; char nm[64];

    s64 g0 = union_readdir_run(&p, q, buf, 60, 0);
    TEST_ASSERT(g0 > 0, "page 1 returns");
    TEST_ASSERT(urd_entry(buf, (long)g0, 0, &qp, &ck, nm, sizeof(nm)) > 0 &&
                urd_name_eq(nm, "shared"), "page1 e0 = shared");
    TEST_ASSERT(urd_entry(buf, (long)g0, 1, &qp, &ck, nm, sizeof(nm)) > 0 &&
                urd_name_eq(nm, "only1"), "page1 e1 = only1");
    TEST_EXPECT_EQ(ck, (u64)2, "page1 last ordinal = 2");
    TEST_EXPECT_EQ(urd_entry(buf, (long)g0, 2, &qp, &ck, nm, sizeof(nm)), (long)0,
                   "page1 holds exactly 2");

    // Page 2 resumes at ordinal 2 -> only2 (ordinal 3), then EOD.
    s64 g1 = union_readdir_run(&p, q, buf, 60, 2);
    TEST_ASSERT(g1 > 0, "page 2 returns");
    TEST_ASSERT(urd_entry(buf, (long)g1, 0, &qp, &ck, nm, sizeof(nm)) > 0 &&
                urd_name_eq(nm, "only2"), "page2 e0 = only2 (no gap, no dup)");
    TEST_EXPECT_EQ(ck, (u64)3, "page2 ordinal = 3");
    TEST_EXPECT_EQ(urd_entry(buf, (long)g1, 1, &qp, &ck, nm, sizeof(nm)), (long)0,
                   "page2 holds exactly 1");
    TEST_EXPECT_EQ((long)union_readdir_run(&p, q, buf, 60, 3), (long)0, "page 3 EOD");

    spoor_clunk(q);
    territory_unref(p.territory);
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);
    spoor_unref(root);
}

// Control: a plain directory open and a SINGLE-member mount are NOT unions, so
// union_snap stays NULL -- readdir takes the ordinary single-Dev path. Proves
// the tag is set only for a >= 2-member mount (no over-tagging regression).
void test_stalk_union_readdir_nontagged(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    // (a) plain directory (um1 is not a mount point).
    struct Spoor *q1 = stalk(&p, root, "um1", 3, STALK_OPEN, 0);
    TEST_ASSERT(q1 != NULL, "open plain dir um1");
    TEST_ASSERT(q1->union_snap == NULL, "plain dir open is not tagged");
    spoor_clunk(q1);

    // (b) single-member mount (one graft on umpt) is not a union.
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um2 && pt, "resolve um2 + umpt");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MBEFORE), 0, "single mount um2 -> umpt");
    struct Spoor *q2 = stalk(&p, root, "umpt", 4, STALK_OPEN, 0);
    TEST_ASSERT(q2 != NULL, "open single-mount umpt");
    TEST_ASSERT(q2->union_snap == NULL, "single-member mount is not a union");
    spoor_clunk(q2);

    territory_unref(p.territory);
    spoor_clunk(um2); spoor_clunk(pt);
    spoor_unref(root);
}

// =============================================================================
// UM (union mounts) -- union CREATE targets the first MCREATE member (UM-5a).
//
// A create in a union lands in the FIRST member (declared order) whose mount
// entry carries MCREATE (ARCH 9.5 "create in the first writable mount";
// territory.tla::CreateTargetCorrect). The parent resolves via STALK_CREATE,
// which differs from STALK_WALK only at a union final quarry. These drive the
// REAL create path (sys_open_create_kpath_for_proc, asserting the create's
// parent qid via the fixture's g_fix_create_last_parent) plus a stalk-level
// first-wins check.
// =============================================================================

// The #50 create-driver helpers are defined lower in this file (the
// SYS_OPEN_CREATE battery); forward-declare them for the two e2e create tests
// here. A static forward decl matching the later static definition is legal;
// the extern matches the identical decl at the #50 section head.
extern s64 sys_open_create_kpath_for_proc(struct Proc *p, u64 start_fd_raw,
                                          const char *kpath, u64 klen,
                                          u64 omode_raw, u64 perm_raw);
static struct Proc *ocp_proc(const char *dot);
static void ocp_teardown(struct Proc *p);

// The MCREATE flag -- not member position -- selects the create target: member 0
// (um1) is NOT MCREATE, member 1 (um2) is, so the create lands in um2.
void test_stalk_union_create(void) {
    fixmade_reset();
    struct Proc *p = ocp_proc("/");
    TEST_ASSERT(p != NULL, "proc + territory");

    struct Spoor *root = p->territory->root_spoor;
    struct Spoor *um1 = stalk(p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p->territory, um1, pt, MBEFORE),          0, "um1 MBEFORE (no MCREATE)");
    TEST_EXPECT_EQ(mount(p->territory, um2, pt, MAFTER | MCREATE), 0, "um2 MAFTER|MCREATE");
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);

    fixmade_reset();
    s64 fd = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                            "/umpt/newfile", 13, 1 /*OWRITE*/, 0644);
    TEST_ASSERT(fd >= 0, "create /umpt/newfile in the union");
    TEST_EXPECT_EQ(g_fix_create_last_parent, (u64)25,
                   "create landed in the MCREATE member (um2, qid 25), NOT member[0] um1 (22)");

    ocp_teardown(p);
}

// First MCREATE member (declared order) wins when MORE THAN ONE is writable:
// both um1 (MBEFORE) and um2 (MAFTER) carry MCREATE, so um1 -- searched first --
// is the target. Stalk-level: STALK_CREATE returns the chosen member root.
void test_stalk_union_create_first_wins(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MBEFORE | MCREATE), 0, "um1 MBEFORE|MCREATE");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MAFTER  | MCREATE), 0, "um2 MAFTER|MCREATE");

    struct Spoor *q = stalk(&p, root, "umpt", 4, STALK_CREATE, 0);
    TEST_ASSERT(q != NULL, "STALK_CREATE resolves the union create target");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)22,
                   "first MCREATE member wins (um1 root, qid 22), not um2 (25)");
    spoor_clunk(q);

    territory_unref(p.territory);
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);
    spoor_unref(root);
}

// A union with NO MCREATE member has no writable create target -> the create is
// denied -T_E_ACCES and NOTHING is created (not silently placed in a read-only
// member). Real create path.
void test_stalk_union_create_no_target(void) {
    fixmade_reset();
    struct Proc *p = ocp_proc("/");
    TEST_ASSERT(p != NULL, "proc + territory");

    struct Spoor *root = p->territory->root_spoor;
    struct Spoor *um1 = stalk(p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p->territory, um1, pt, MBEFORE), 0, "um1 MBEFORE (no MCREATE)");
    TEST_EXPECT_EQ(mount(p->territory, um2, pt, MAFTER),  0, "um2 MAFTER (no MCREATE)");
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);

    fixmade_reset();
    s64 fd = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                            "/umpt/newfile", 13, 1 /*OWRITE*/, 0644);
    TEST_EXPECT_EQ((long)fd, (long)(-(s64)T_E_ACCES),
                   "no MCREATE member -> create denied EACCES");
    TEST_EXPECT_EQ(g_fix_create_last_parent, (u64)-1,
                   "nothing was created in any member");

    ocp_teardown(p);
}

// =============================================================================
// UM (union mounts) -- REMOVE targets the member HOLDING the leaf (UM-7 F3).
//
// unlink / rmdir / rename-source must act on the member that HOLDS the entry
// (walk first-hit), NOT the MCREATE member a create would pick. Pre-fix these
// routed through STALK_CREATE, so `rm /u/foo` on a union whose foo lived in a
// non-MCREATE member hit the writable member instead (ENOENT, or unlinking a
// shadow). territory.tla::RemoveTargetCorrect + BUGGY_REMOVE_MCREATE_MEMBER.
// =============================================================================

// stalk_union_member_holding returns the member whose walk FINDS the leaf, in
// declared order -- distinct from the MCREATE member. Union [um1 MBEFORE (NOT
// MCREATE), um2 MAFTER|MCREATE]: "only1" lives in um1 (member 0), so the holder
// is um1 (qid 22), NOT the MCREATE member um2 (qid 25) a create would choose.
void test_stalk_union_member_holding(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MBEFORE),          0, "um1 MBEFORE (no MCREATE)");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MAFTER | MCREATE), 0, "um2 MAFTER|MCREATE");

    u64 live_before = spoor_total_allocated() - spoor_total_freed();

    // "only1" is held by um1 (member 0, NOT MCREATE). The holder is um1, NOT the
    // MCREATE member -- the exact F3 mis-selection (STALK_CREATE would give 25).
    int e = 0;
    struct Spoor *m = stalk_union_member_holding(&p, pt, "only1", &e);
    TEST_ASSERT(m != NULL, "only1 has a holder");
    TEST_EXPECT_EQ((u64)m->qid.path, (u64)22,
                   "holder of only1 = um1 (member 0), NOT the MCREATE member um2 (25)");
    spoor_clunk(m);

    // "only2" is held only by um2 (which is also the MCREATE member) -> um2.
    e = 0;
    m = stalk_union_member_holding(&p, pt, "only2", &e);
    TEST_ASSERT(m != NULL, "only2 has a holder");
    TEST_EXPECT_EQ((u64)m->qid.path, (u64)25, "holder of only2 = um2 (25)");
    spoor_clunk(m);

    // "shared" is held by BOTH -> the FIRST member (um1), matching walk first-hit.
    e = 0;
    m = stalk_union_member_holding(&p, pt, "shared", &e);
    TEST_ASSERT(m != NULL, "shared has a holder");
    TEST_EXPECT_EQ((u64)m->qid.path, (u64)22, "holder of shared = first member um1 (22)");
    spoor_clunk(m);

    // No member holds "nosuch" -> NULL, and NOT an error (the caller answers
    // ENOENT; *errp stays 0, distinguishing a miss from a clone OOM).
    e = 0;
    m = stalk_union_member_holding(&p, pt, "nosuch", &e);
    TEST_ASSERT(m == NULL, "nosuch has no holder");
    TEST_EXPECT_EQ(e, 0, "a no-holder miss is not an error");

    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before, "no Spoor leak across member_holding");

    territory_unref(p.territory);
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);
    spoor_unref(root);
}

// STALK_REMOVE resolves a union parent to the UNCROSSED mount point (so the
// caller selects the holder), while STALK_CREATE crosses to the MCREATE member.
// The divergence at the final quarry is the whole of the F3 stalk change.
void test_stalk_union_remove_uncrossed(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MBEFORE),          0, "um1 MBEFORE (no MCREATE)");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MAFTER | MCREATE), 0, "um2 MAFTER|MCREATE");

    // STALK_REMOVE: the union quarry is left UNCROSSED -- still the mount point
    // (qid 28) and still a union (member 1 exists).
    struct Spoor *r = stalk(&p, root, "umpt", 4, STALK_REMOVE, 0);
    TEST_ASSERT(r != NULL, "STALK_REMOVE resolves umpt");
    TEST_EXPECT_EQ((u64)r->qid.path, (u64)28, "STALK_REMOVE yields the mount point (28), uncrossed");
    struct Spoor *rm1 = mount_member_at(p.territory, r, 1, NULL);
    TEST_ASSERT(rm1 != NULL, "STALK_REMOVE left umpt a union point (member 1 present)");
    spoor_clunk(rm1);
    spoor_clunk(r);

    // STALK_CREATE: the union quarry crosses to the MCREATE member (um2, 25),
    // which is NOT itself a mount point.
    struct Spoor *c = stalk(&p, root, "umpt", 4, STALK_CREATE, 0);
    TEST_ASSERT(c != NULL, "STALK_CREATE resolves umpt");
    TEST_EXPECT_EQ((u64)c->qid.path, (u64)25, "STALK_CREATE crosses to the MCREATE member um2 (25)");
    struct Spoor *cm1 = mount_member_at(p.territory, c, 1, NULL);
    TEST_ASSERT(cm1 == NULL, "STALK_CREATE result is a crossed member, not a union point");
    spoor_clunk(c);

    territory_unref(p.territory);
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);
    spoor_unref(root);
}

// =============================================================================
// UM (union mounts) -- an fd-based union resolution base sees ALL members
// (UM-7 F5). A handle to a union directory is member[0] + the union_snap (which
// UM-8c makes retain the mount POINT). Resolving RELATIVE to that fd must search
// every member, not just member[0].
// =============================================================================

void test_stalk_union_fd_base(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");

    struct Spoor *um1 = stalk(&p, root, "um1",  3, STALK_WALK,  0);
    struct Spoor *um2 = stalk(&p, root, "um2",  3, STALK_WALK,  0);
    struct Spoor *pt  = stalk(&p, root, "umpt", 4, STALK_MOUNT, 0);
    TEST_ASSERT(um1 && um2 && pt, "resolve um1 + um2 + umpt");
    TEST_EXPECT_EQ(mount(p.territory, um1, pt, MBEFORE), 0, "um1 MBEFORE");
    TEST_EXPECT_EQ(mount(p.territory, um2, pt, MAFTER),  0, "um2 MAFTER");

    // Open the union dir -> the fd is member[0] (um1, qid 22) + a union_snap that
    // UM-8c makes retain the union POINT (qid 28).
    struct Spoor *ufd = stalk(&p, root, "umpt", 4, STALK_OPEN, 0);
    TEST_ASSERT(ufd != NULL, "open the union dir");
    TEST_ASSERT(ufd->union_snap != NULL, "opened union fd carries a union_snap");
    TEST_ASSERT(ufd->union_snap->point != NULL, "union_snap retains the point (UM-8c F5)");
    TEST_EXPECT_EQ((u64)ufd->union_snap->point->qid.path, (u64)28,
                   "retained point is the mount point umpt (qid 28)");
    struct Spoor *pm1 = mount_member_at(p.territory, ufd->union_snap->point, 1, NULL);
    TEST_ASSERT(pm1 != NULL, "the retained point is still a union point");
    spoor_clunk(pm1);

    u64 live_before = spoor_total_allocated() - spoor_total_freed();

    // F5: "only2" lives only in member 1 (um2). WITHOUT the base-cross fix the
    // resolution walks member[0] (um1) only and misses it; WITH it the union is
    // searched -> um2's only2 (qid 27).
    struct Spoor *q = stalk(&p, ufd, "only2", 5, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "fd-relative resolve only2 (member 1)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)27, "fd-relative walk sees member 1 (only2 qid 27)");
    spoor_clunk(q);

    // "only1" (member 0) still resolves relative to the fd -> qid 24.
    q = stalk(&p, ufd, "only1", 5, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "fd-relative resolve only1 (member 0)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)24, "fd-relative walk member 0 (only1 qid 24)");
    spoor_clunk(q);

    // "shared" (both) -> first member (um1, qid 23) -- first-hit off the fd too.
    q = stalk(&p, ufd, "shared", 6, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "fd-relative resolve shared");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)23, "fd-relative first-hit (shared qid 23)");
    spoor_clunk(q);

    // A clean miss relative to the fd -> ENOENT (no member has it).
    int err = 0;
    q = stalk_err(&p, ufd, "nosuch", 6, STALK_OPEN, 0, &err);
    TEST_ASSERT(q == NULL, "fd-relative miss");
    TEST_EXPECT_EQ(err, T_E_NOENT, "fd-relative all-miss -> ENOENT");

    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before, "no Spoor leak across fd-relative union resolves");

    spoor_clunk(ufd);
    territory_unref(p.territory);
    spoor_clunk(um1); spoor_clunk(um2); spoor_clunk(pt);
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
    int rc = stalk_stat(&p, root, "a/deep/leaf", 11, 0, &st, &e);
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
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "a/nope", 6, 0, &st, &e), (u64)-1,
                   "stat of a missing path fails");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "missing -> NOENT");
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "nox/sekret", 10, 0, &st, &e), (u64)-1,
                   "stat under a no-X dir fails");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES, "denied -> ACCES (fail-ordering)");

    // The stat of "/" (zero real components) takes the fallback quarry path.
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "/", 1, 0, &st, &e), (u64)0, "stat /");
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
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "loop", 4, 0, &st, &e), (u64)0,
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
extern s64 sys_stat_for_proc(struct Proc *p, const char *path, u64 path_len, u32 stalk_flags,
                             struct t_stat *out_k);

void test_sys_stat_for_proc(void) {
    struct Proc p;
    struct Spoor *root = cross_setup(&p);   // synthetic Proc + fresh Territory
    TEST_ASSERT(root != NULL && p.territory != NULL, "cross_setup");
    // The Territory root is the fixture root (SYS_STAT resolves from
    // territory_root_ref, the FROM_ROOT arm).
    TEST_EXPECT_EQ(territory_chroot(p.territory, root), 0, "chroot to fixture");

    struct t_stat st;
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/a/b", 4, 0, &st), (u64)0,
                   "SYS_STAT inner: absolute path");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)2, "/a/b -> qid 2");
    TEST_EXPECT_EQ((u64)st.mode, (u64)(T_S_IFREG | 0644u), "b mode 0644");

    // Relative path joins the cwd (dot unset == "/"); same answer.
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "a/b", 3, 0, &st), (u64)0,
                   "SYS_STAT inner: relative path via the cwd join");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)2, "a/b -> qid 2");

    // Resolution errnos pass through as -errno.
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/a/nope", 7, 0, &st),
                   (u64)(s64)-T_E_NOENT, "missing -> -T_E_NOENT");
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/nox/sekret", 11, 0, &st),
                   (u64)(s64)-T_E_ACCES, "denied -> -T_E_ACCES");

    // Structural rejects -> the bare -1.
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, NULL, 3, 0, &st), (u64)(s64)-1,
                   "NULL path -> -1");
    TEST_EXPECT_EQ((u64)sys_stat_for_proc(&p, "/a/b", 0, 0, &st), (u64)(s64)-1,
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
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "a/b", 3, 0, &st, &e), (u64)0,
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

// =============================================================================
// D-1: symlink expansion (docs/DISTRO.md section 4; the I-28 refinement).
// =============================================================================

// The follow legs: a link in the MIDDLE of a path, at the END, chained, to a
// directory, and one whose target is absolute (the re-anchor + restart arm).
void test_stalk_symlink_follow(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // FINAL component is a link -> the resolution lands on its TARGET.
    g_fix_readlink_calls = 0;
    struct Spoor *q = stalk(&p, root, "lnb", 3, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve lnb");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "lnb -> a/b (qid 2), not the link");
    TEST_EXPECT_EQ((u64)g_fix_readlink_calls, (u64)1, "exactly one readlink");
    spoor_clunk(q);

    // MID-PATH link: lndir -> a/deep, so lndir/leaf is a/deep/leaf. Without
    // expansion this is the #79 ENOTDIR gate (a link is not a directory), so
    // this leg fails LOUDLY on a resolver that only handles the final hop.
    g_fix_readlink_calls = 0;
    q = stalk(&p, root, "lndir/leaf", 10, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve lndir/leaf");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)4, "lndir/leaf -> a/deep/leaf (qid 4)");
    TEST_EXPECT_EQ((u64)g_fix_readlink_calls, (u64)1, "one readlink mid-path");
    spoor_clunk(q);

    // CHAIN: lnchain -> lnb -> a/b. Two expansions, and the count PROVES the
    // second one happened rather than the first having landed by luck.
    g_fix_readlink_calls = 0;
    q = stalk(&p, root, "lnchain", 7, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve lnchain");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "lnchain -> lnb -> a/b (qid 2)");
    TEST_EXPECT_EQ((u64)g_fix_readlink_calls, (u64)2, "two readlinks (the chain)");
    spoor_clunk(q);

    // ABSOLUTE target: the RESTART arm. Requires a Territory (the re-anchor
    // reads its root), so this leg uses the cross_setup Proc rather than the
    // bare fixture one -- and that is the point: an absolute target re-anchors
    // at the CALLER'S OWN root, never a global one (I-28).
    spoor_unref(root);
    struct Proc p2;
    struct Spoor *root2 = cross_setup(&p2);
    TEST_ASSERT(root2 != NULL && p2.territory != NULL, "cross_setup");
    TEST_EXPECT_EQ(territory_chroot(p2.territory, root2), 0, "chroot to fixture");
    g_fix_readlink_calls = 0;
    q = stalk(&p2, root2, "lnabs", 5, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve lnabs (absolute target)");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "/a/b re-anchored at OUR root -> qid 2");
    TEST_EXPECT_EQ((u64)g_fix_readlink_calls, (u64)1, "one readlink");
    spoor_clunk(q);

    // A '..'-BEARING target (a/deep/lnup -> ../b): the other restart arm. The
    // pop must land 1:1, which is why this target cannot take the in-place
    // splice -- and the assertion here is simply that it RESOLVES, which a
    // splice into a pounced trail would not.
    q = stalk(&p2, root2, "a/deep/lnup", 11, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/lnup");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)2, "a/deep/lnup -> a/b (qid 2)");
    spoor_clunk(q);

    territory_unref(p2.territory);
    spoor_unref(root2);
}

// The refusal legs: the follow bound (a cycle), a dangling target, and a link
// whose target crosses a directory the caller cannot search (fail-ordering:
// the DENIAL must survive expansion, not be laundered into a miss).
void test_stalk_symlink_bounds(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // The cycle: lnself -> lnself. Terminates on the follow budget with ELOOP
    // (never spins -- a hang here is the failure mode this bound exists for).
    int e = -12345;
    g_fix_readlink_calls = 0;
    struct Spoor *q = stalk_err(&p, root, "lnself", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a symlink cycle does not resolve");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_LOOP, "cycle -> T_E_LOOP");
    TEST_EXPECT_EQ((u64)g_fix_readlink_calls, (u64)STALK_MAX_FOLLOWS,
                   "exactly STALK_MAX_FOLLOWS expansions, then the bound");

    // Dangling: lndead -> nosuch. The target is expanded and then MISSES --
    // ENOENT, the same answer the spelled-out path gives.
    e = -12345;
    q = stalk_err(&p, root, "lndead", 6, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a dangling symlink does not resolve");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "dangling -> T_E_NOENT");

    // Through a no-X directory: lnnox -> nox/sekret. The expansion re-enters
    // the SAME loop, so the per-component X-search binds the expanded
    // components exactly as it binds spelled-out ones -- EACCES, not ENOENT.
    // This is the I-28 containment claim in its sharpest form: expansion
    // cannot be used to reach past a gate.
    e = -12345;
    q = stalk_err(&p, root, "lnnox", 5, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "a link into a no-X dir does not resolve");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_ACCES,
                   "the expanded path is X-gated -> ACCES (never NOENT)");

    spoor_unref(root);
}

// The no-follow dispositions (POSIX + Linux): open(O_NOFOLLOW) -> ELOOP,
// O_PATH|O_NOFOLLOW -> the link itself, lstat -> the link's own record, and a
// TRAILING SLASH overriding the flag.
void test_stalk_symlink_nofollow(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // A byte-open of a link with the flag: ELOOP (a symlink cannot be opened).
    int e = -12345;
    struct Spoor *q = stalk_err(&p, root, "lnb", 3,
                                STALK_OPEN | STALK_NOFOLLOW, 0, &e);
    TEST_ASSERT(q == NULL, "O_NOFOLLOW open of a link fails");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_LOOP, "-> T_E_LOOP (Linux O_NOFOLLOW)");

    // O_PATH|O_NOFOLLOW: the navigation handle IS the link.
    q = stalk(&p, root, "lnb", 3, STALK_WALK | STALK_NOFOLLOW, 0);
    TEST_ASSERT(q != NULL, "O_PATH|O_NOFOLLOW resolves");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)FIX_LNB_PATH,
                   "the quarry is the LINK (qid 10), not its target");
    TEST_ASSERT((q->qid.type & QTSYMLINK) != 0, "and it is marked QTSYMLINK");
    spoor_clunk(q);

    // Only the FINAL component is held back: an INTERMEDIATE link still
    // follows even under the flag (POSIX -- a mid-path link is a directory
    // position, and no-follow is a statement about what the path NAMES).
    q = stalk(&p, root, "lndir/leaf", 10, STALK_OPEN | STALK_NOFOLLOW, 0);
    TEST_ASSERT(q != NULL, "an intermediate link follows under NOFOLLOW");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)4, "-> a/deep/leaf");
    spoor_clunk(q);

    // TRAILING SLASH overrides the flag (POSIX 4.13): "lndir/" names the
    // directory the link resolves to, so it follows and answers the target.
    q = stalk(&p, root, "lndir/", 6, STALK_OPEN | STALK_NOFOLLOW, 0);
    TEST_ASSERT(q != NULL, "lndir/ follows despite NOFOLLOW");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)3, "-> a/deep (qid 3)");
    spoor_clunk(q);

    // A trailing slash on a link to a FILE is ENOTDIR (the #82 gate reads the
    // followed quarry -- the link resolved, and what it named is not a dir).
    e = -12345;
    q = stalk_err(&p, root, "lnb/", 4, STALK_OPEN, 0, &e);
    TEST_ASSERT(q == NULL, "lnb/ (link to a file) fails");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOTDIR, "-> T_E_NOTDIR");

    // An unknown amode/flag bit is still rejected LOUDLY (the stalk-1 F1
    // guard, which D-1 widened by exactly one bit and no more).
    e = -12345;
    q = stalk_err(&p, root, "a/b", 3, STALK_OPEN | 0x200, 0, &e);
    TEST_ASSERT(q == NULL, "an unknown amode flag is refused");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_INVAL, "-> T_E_INVAL");

    spoor_unref(root);
}

// stalk_stat's two shapes: stat FOLLOWS (reporting the target) and lstat does
// not (reporting the link -- S_IFLNK). The pounce serves both from the fused
// leaf record, so this also pins that the fast path does not silently follow.
void test_stalk_symlink_stat_vs_lstat(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    struct t_stat st;
    int e = -12345;

    // stat("lnb") -> the TARGET's record.
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "lnb", 3, 0, &st, &e), (u64)0,
                   "stat lnb");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)2, "stat follows -> a/b (qid 2)");
    TEST_EXPECT_EQ((u64)(st.mode & T_S_IFMT), (u64)T_S_IFREG, "-> a regular file");

    // lstat("lnb") -> the LINK's own record.
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "lnb", 3, STALK_NOFOLLOW, &st, &e),
                   (u64)0, "lstat lnb");
    TEST_EXPECT_EQ((u64)st.qid_path, (u64)FIX_LNB_PATH,
                   "lstat does NOT follow -> the link (qid 10)");
    TEST_EXPECT_EQ((u64)(st.mode & T_S_IFMT), (u64)T_S_IFLNK, "-> S_IFLNK");

    // lstat of a DANGLING link succeeds (the link exists even though its
    // target does not) -- the divergence a follow-always resolver cannot show.
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "lndead", 6, STALK_NOFOLLOW, &st, &e),
                   (u64)0, "lstat of a dangling link succeeds");
    TEST_EXPECT_EQ((u64)(st.mode & T_S_IFMT), (u64)T_S_IFLNK, "-> S_IFLNK");
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "lndead", 6, 0, &st, &e), (u64)-1,
                   "stat of the same dangling link FAILS");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_NOENT, "-> NOENT");

    // An unknown flag bit is refused (stalk_stat's own guard).
    e = -12345;
    TEST_EXPECT_EQ((u64)stalk_stat(&p, root, "a/b", 3, 0x4u, &st, &e), (u64)-1,
                   "an unknown stat flag is refused");
    TEST_EXPECT_EQ((u64)e, (u64)T_E_INVAL, "-> T_E_INVAL");

    spoor_unref(root);
}

// The POUNCE interaction: a link INSIDE a batched run must split the run at
// its parent and resume per-component. Asserted by ENGAGEMENT counters, since
// a resolver that silently fell back to the per-component loop everywhere
// would answer correctly and prove nothing about the split.
void test_stalk_symlink_pounce_split(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    // a/deep/lnleaf: the run gathers all three, walk_attrs reports component 2
    // as QTSYMLINK, and the resolver splits at its parent (a/deep), resumes at
    // `lnleaf` per-component, expands it, and lands on a/deep/leaf.
    g_fix_walk_calls = 0; g_fix_walkattrs_calls = 0; g_fix_readlink_calls = 0;
    struct Spoor *q = stalk(&p, root, "a/deep/lnleaf", 13, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "resolve a/deep/lnleaf");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)4, "-> a/deep/leaf (qid 4)");
    TEST_EXPECT_EQ((u64)g_fix_readlink_calls, (u64)1, "one expansion");
    TEST_ASSERT(g_fix_walkattrs_calls > 0, "the POUNCE ran (not a silent fallback)");
    TEST_ASSERT(g_fix_walk_calls > 0,
                "and the split resumed PER-COMPONENT for the link hop");
    spoor_clunk(q);

    // The same tree resolved with NO walk_attrs slot must agree exactly -- the
    // A/B parity the pounce battery uses, extended to symlinks.
    struct Spoor *rootn = fix_root_nowa();
    TEST_ASSERT(rootn != NULL, "fix_root_nowa");
    q = stalk(&p, rootn, "a/deep/lnleaf", 13, STALK_OPEN, 0);
    TEST_ASSERT(q != NULL, "the no-walk_attrs twin resolves it too");
    TEST_EXPECT_EQ((u64)q->qid.path, (u64)4, "-> the SAME qid 4");
    spoor_clunk(q);
    spoor_unref(rootn);

    spoor_unref(root);
}

// Lifetime: a resolution that expands must leak no Spoor and free no Spoor
// twice. The allocated-minus-freed identity is the same instrument the
// stalk.lifetime battery uses; a link adds a transient clone per expansion
// (the walked link, clunked after its readlink), so a leak here is a real
// per-symlink leak on the commonest path in a stock rootfs.
void test_stalk_symlink_lifetime(void) {
    struct Proc p; mkproc_system(&p);
    struct Spoor *root = fix_root();
    TEST_ASSERT(root != NULL, "fix_root");

    u64 live_before = spoor_total_allocated() - spoor_total_freed();

    // Every shape: follow, chain, mid-path, no-follow, refusal, cycle.
    struct Spoor *q = stalk(&p, root, "lnb", 3, STALK_OPEN, 0);
    if (q) spoor_clunk(q);
    q = stalk(&p, root, "lnchain", 7, STALK_OPEN, 0);
    if (q) spoor_clunk(q);
    q = stalk(&p, root, "lndir/leaf", 10, STALK_OPEN, 0);
    if (q) spoor_clunk(q);
    q = stalk(&p, root, "lnb", 3, STALK_WALK | STALK_NOFOLLOW, 0);
    if (q) spoor_clunk(q);
    q = stalk(&p, root, "lndead", 6, STALK_OPEN, 0);
    TEST_ASSERT(q == NULL, "dangling still fails");
    q = stalk(&p, root, "lnself", 6, STALK_OPEN, 0);
    TEST_ASSERT(q == NULL, "the cycle still fails");

    u64 live_after = spoor_total_allocated() - spoor_total_freed();
    TEST_EXPECT_EQ(live_after, live_before,
                   "expansion leaks no Spoor and double-frees none");

    spoor_unref(root);
}

// =============================================================================
// #50: SYS_OPEN_CREATE over the fixture (VIVARIUM.md section 6.24; scripture
// b417b307). These drive sys_open_create_kpath_for_proc -- the kernel core
// under the native handler AND the phenotype openat/mkdirat shells -- over the
// REAL resolver: the cwd join, containment, X-search/A-2d, and the lexical
// leaf rows. The EEXIST-exact + bounded-retry legs live in test_dev9p.c (the
// loopback's errno channel is dev9p-private; the fixture's create-fail is the
// generic -1 by design).
// =============================================================================

extern s64 sys_open_create_kpath_for_proc(struct Proc *p, u64 start_fd_raw,
                                          const char *kpath, u64 klen,
                                          u64 omode_raw, u64 perm_raw);

// One heap Proc with a Territory rooted in the fixture + a cwd. proc_free
// tears the whole thing down (handles -> clunks; territory_unref -> root).
static struct Proc *ocp_proc(const char *dot) {
    struct Proc *p = proc_alloc();
    if (!p) return NULL;
    p->principal_id = PRINCIPAL_SYSTEM;   // fixnode owner: A-2d owner bits
    p->primary_gid  = GID_SYSTEM;
    p->caps         = CAP_NONE;
    p->territory    = territory_alloc();
    if (!p->territory) { p->state = PROC_STATE_ZOMBIE; proc_free(p); return NULL; }
    p->territory->root_spoor = fix_root();   // territory owns this ref
    if (dot && territory_setdot(p->territory, dot) != 0) {
        p->state = PROC_STATE_ZOMBIE; proc_free(p); return NULL;
    }
    return p;
}

static void ocp_teardown(struct Proc *p) {
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

// THE blocker-3 regression (VIVARIUM.md 6.20 Correction 1, third blocker): a
// RELATIVE create through FROM_ROOT must land in the CWD, not the Territory
// root. SYS_WALK_CREATE's identical-looking sentinel resolves at the ROOT
// with no join -- the silent wrong-directory hazard the shared join helper
// closes. Non-vacuous: re-pointing the create core at a joinless resolve
// lands the file at qid 0 and this fails.
void test_stalk_open_create_cwd_parity(void) {
    fixmade_reset();
    struct Proc *p = ocp_proc("/a");
    TEST_ASSERT(p != NULL, "proc + territory + cwd=/a");

    s64 fd = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                            "newfile", 7, 1 /*OWRITE*/, 0644);
    TEST_ASSERT(fd >= 0, "relative create succeeds");
    TEST_EXPECT_EQ(g_fix_create_last_parent, (u64)1,
                   "create landed in the CWD (/a, qid 1), NOT the root (qid 0)");

    // The same call with an ABSOLUTE path ignores the cwd (SYS_OPEN parity).
    s64 fd2 = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                             "/rootfile", 9, 1, 0644);
    TEST_ASSERT(fd2 >= 0, "absolute create succeeds");
    TEST_EXPECT_EQ(g_fix_create_last_parent, (u64)0,
                   "absolute create landed at the root, cwd ignored");
    ocp_teardown(p);
}

// The open-if-present half + create-RPC economy: the second O_CREAT (no EXCL)
// of an existing leaf OPENS it -- no second create call, no second overlay
// entry. Then OEXCL on the same leaf fails and creates nothing (exclusivity;
// the -T_E_EXIST-exact assertion is the loopback test's).
void test_stalk_open_create_open_if_present(void) {
    fixmade_reset();
    struct Proc *p = ocp_proc(NULL);
    TEST_ASSERT(p != NULL, "proc");

    s64 fd = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                            "/a/deep/fresh", 13, 1, 0644);
    TEST_ASSERT(fd >= 0, "create the absent leaf");
    TEST_EXPECT_EQ((u64)g_fix_create_calls, (u64)1, "one create call");

    s64 fd2 = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                             "/a/deep/fresh", 13, 1, 0644);
    TEST_ASSERT(fd2 >= 0, "O_CREAT on the existing leaf OPENS it");
    TEST_EXPECT_EQ((u64)g_fix_create_calls, (u64)1,
                   "open-first: the present case pays NO create call");
    TEST_EXPECT_EQ((u64)g_fixmade_n, (u64)1, "no duplicate node");

    s64 fd3 = sys_open_create_kpath_for_proc(
        p, SYS_WALK_OPEN_FROM_ROOT, "/a/deep/fresh", 13,
        1 | SYS_WALK_OPEN_OEXCL, 0644);
    TEST_ASSERT(fd3 < 0, "OEXCL on an existing leaf fails");
    TEST_EXPECT_EQ((u64)g_fixmade_n, (u64)1, "OEXCL created nothing");
    ocp_teardown(p);
}

// mkdir-by-path (DMDIR = the exclusive arm) + the created dir is REAL to the
// resolver: a file then creates INSIDE it (X-search + QTDIR + A-2d all read
// the overlay node's stat).
void test_stalk_open_create_mkdir_and_nest(void) {
    fixmade_reset();
    struct Proc *p = ocp_proc(NULL);
    TEST_ASSERT(p != NULL, "proc");

    s64 dfd = sys_open_create_kpath_for_proc(
        p, SYS_WALK_OPEN_FROM_ROOT, "/a/subdir", 9,
        0 /*OREAD*/, (u64)SYS_WALK_CREATE_DMDIR | 0755);
    TEST_ASSERT(dfd >= 0, "mkdir /a/subdir");
    TEST_EXPECT_EQ(g_fix_create_last_parent, (u64)1, "dir created under /a");

    s64 ffd = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                             "/a/subdir/inner", 15, 1, 0600);
    TEST_ASSERT(ffd >= 0, "create a file INSIDE the created dir");
    TEST_EXPECT_EQ(g_fix_create_last_parent, (u64)FIXMADE_BASE,
                   "the new dir (first overlay qid) is the parent");

    s64 again = sys_open_create_kpath_for_proc(
        p, SYS_WALK_OPEN_FROM_ROOT, "/a/subdir", 9,
        0, (u64)SYS_WALK_CREATE_DMDIR | 0755);
    TEST_ASSERT(again < 0, "mkdir of an existing dir fails (create-only)");
    ocp_teardown(p);
}

// The lexical leaf rows (Linux open_last_lookups / filename_create parity) +
// the omode envelope. All answered BEFORE any resolution -- no overlay entry
// may appear.
void test_stalk_open_create_leaf_rows(void) {
    fixmade_reset();
    struct Proc *p = ocp_proc(NULL);
    TEST_ASSERT(p != NULL, "proc");
    struct { const char *path; u64 len; u64 omode; u64 perm; s64 want; const char *why; } rows[] = {
        { "/a/f/",  5, 1, 0644, -(s64)T_E_ISDIR, "trailing slash on a FILE create" },
        { "/a/.",   4, 1, 0644, -(s64)T_E_ISDIR, "dot leaf on a FILE create" },
        { "/a/..",  5, 1, 0644, -(s64)T_E_ISDIR, "dotdot leaf on a FILE create" },
        { "/",      1, 1, 0644, -(s64)T_E_ISDIR, "root leaf on a FILE create" },
        { "/a/.",   4, 0, (u64)SYS_WALK_CREATE_DMDIR | 0755, -(s64)T_E_EXIST,
          "mkdir(.) answers EEXIST (Linux filename_create)" },
        { "/",      1, 0, (u64)SYS_WALK_CREATE_DMDIR | 0755, -(s64)T_E_EXIST,
          "mkdir(/) answers EEXIST" },
        { "/a/x",   4, 1 | 0x80 /*OPATH*/, 0644, -(s64)T_E_INVAL,
          "OPATH is rejected (a navigation handle cannot want creation)" },
        { "/a/x",   4, 1 | 0x8 /*stray bit*/, 0644, -(s64)T_E_INVAL,
          "an omode bit outside the mask is rejected" },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        s64 rc = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                                rows[i].path, rows[i].len,
                                                rows[i].omode, rows[i].perm);
        TEST_EXPECT_EQ((u64)rc, (u64)rows[i].want, rows[i].why);
    }
    TEST_EXPECT_EQ((u64)g_fixmade_n, (u64)0, "no row created anything");
    TEST_EXPECT_EQ((u64)g_fix_create_calls, (u64)0, "no row reached create");
    ocp_teardown(p);
}

// Containment (I-28, inherited from stalk) + the denial rows: '..' clamps at
// the Territory root; a no-X parent answers ACCES; a missing prefix NOENT;
// mkdir("d/") strips the slash (legal); NOFOLLOW on a final symlink answers
// LOOP and creates nothing; a DANGLING final symlink + O_CREAT fails loudly
// and creates nothing (the documented degradation: Linux would create the
// TARGET; we refuse rather than silently creating the wrong thing).
void test_stalk_open_create_containment_and_denials(void) {
    fixmade_reset();
    struct Proc *p = ocp_proc("/");
    TEST_ASSERT(p != NULL, "proc");

    s64 fd = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                            "../../esc", 9, 1, 0644);
    TEST_ASSERT(fd >= 0, "'..' spam resolves (clamped), create succeeds");
    TEST_EXPECT_EQ(g_fix_create_last_parent, (u64)0,
                   "clamped at the Territory root -- no escape (I-28)");

    s64 rc = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                            "/nox/inside", 11, 1, 0644);
    TEST_EXPECT_EQ((u64)rc, (u64)(s64)-T_E_ACCES,
                   "create into a no-X/no-W dir answers ACCES (A-2d)");

    rc = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                        "/nosuch/f", 9, 1, 0644);
    TEST_EXPECT_EQ((u64)rc, (u64)(s64)-T_E_NOENT,
                   "a missing prefix answers NOENT");

    s64 dfd = sys_open_create_kpath_for_proc(
        p, SYS_WALK_OPEN_FROM_ROOT, "/a/dslash/", 10,
        0, (u64)SYS_WALK_CREATE_DMDIR | 0755);
    TEST_ASSERT(dfd >= 0, "mkdir('d/') strips the trailing slash (legal)");

    int made_before = g_fixmade_n;
    rc = sys_open_create_kpath_for_proc(
        p, SYS_WALK_OPEN_FROM_ROOT, "/lnb", 4,
        1 | SYS_WALK_OPEN_NOFOLLOW, 0644);
    TEST_EXPECT_EQ((u64)rc, (u64)(s64)-T_E_LOOP,
                   "NOFOLLOW + a final symlink answers LOOP, never creates");
    rc = sys_open_create_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                        "/lndead", 7, 1, 0644);
    TEST_ASSERT(rc < 0, "O_CREAT through a DANGLING final symlink fails LOUDLY");
    TEST_EXPECT_EQ((u64)g_fixmade_n, (u64)made_before,
                   "neither symlink row created anything");
    ocp_teardown(p);
}
