// dev9p — Dev vtable proxying to a kernel 9P client (P5-attach-dev).
//
// Per `kernel/include/thylacine/dev9p.h` + ARCHITECTURE.md §9.6. Each
// dev9p-backed Spoor carries a (`p9_client *`, `fid`) pair in its aux;
// Dev vtable ops route through the high-level p9_client API.

#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_wire.h>
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/page.h>
#include <thylacine/path.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/weft.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

_Static_assert(DEV9P_PRIV_MAGIC == 0x44395050u, "dev9p priv magic drift");

// =============================================================================
// Internal: priv allocation + lookup.
// =============================================================================

// F2: priv_alloc takes an `attached_owner` so the new priv contributes
// one p9_attached_ref. Pre-fix the walked dev9p_priv had no link to the
// attached, so the root's close could destroy the attached while walked
// privs were still alive — R15 F236 UAF. The bump-on-alloc / drop-on-close
// discipline is the F236 close.
//
// `attached_owner` may be NULL for the test-path (dev9p_attach_client called
// with an externally-owned p9_client and no p9_attached wrapper). In that
// case the priv carries no ref and dev9p_close skips the unref.
static struct dev9p_priv *priv_alloc(struct p9_client *client, u32 fid,
                                       bool fid_owned,
                                       struct p9_attached *attached_owner) {
    struct dev9p_priv *p = kmalloc(sizeof(*p), KP_ZERO);
    if (!p) return NULL;
    p->magic          = DEV9P_PRIV_MAGIC;
    p->client         = client;
    p->fid            = fid;
    p->fid_owned      = fid_owned;
    p->attached_owner = attached_owner;
    if (attached_owner) {
        p9_attached_ref(attached_owner);
    }
    return p;
}

static struct dev9p_priv *priv_of(struct Spoor *c) {
    if (!c) return NULL;
    if (c->dc != DEV9P_DC) return NULL;
    struct dev9p_priv *p = (struct dev9p_priv *)c->aux;
    if (!p) return NULL;
    if (p->magic != DEV9P_PRIV_MAGIC) return NULL;
    return p;
}

// Exposed for kernel/dev9p_poll.c (the `.poll` bridge needs p->poll + p->client +
// p->fid). Same dc + magic gate as priv_of.
struct dev9p_priv *dev9p_priv_of(struct Spoor *c) {
    return priv_of(c);
}

// =============================================================================
// FID-LIFECYCLE cached-open support (docs/FID-LIFECYCLE-DESIGN.md section 3.3).
// =============================================================================

// The global outstanding-snapshot-bytes budget (the CF-3 bounce-budget class:
// the snapshot is user-drivable kernel heap). GLOBAL, not per-Proc -- a
// cached-open fd crosses Proc boundaries (rfork inheritance, handle transfer),
// so a per-Proc charge would unbalance at close-by-inheritor. Charge at mint,
// uncharge at dev9p_close's cached_open arm; exhaustion degrades the fast path
// only (the caller falls back to the normal fid open).
static u64 g_co_budget_used;

static bool co_budget_charge(u64 n) {
    u64 cur = __atomic_load_n(&g_co_budget_used, __ATOMIC_RELAXED);
    for (;;) {
        if (cur + n > (u64)DEV9P_CO_BUDGET) return false;
        if (__atomic_compare_exchange_n(&g_co_budget_used, &cur, cur + n,
                                        false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return true;
    }
}

static void co_budget_uncharge(u64 n) {
    __atomic_fetch_sub(&g_co_budget_used, n, __ATOMIC_RELAXED);
}

// Diagnostics accessor (tests + a future /ctl exposure): the bytes currently
// held by live cached-open snapshots.
u64 dev9p_co_budget_used(void) {
    return __atomic_load_n(&g_co_budget_used, __ATOMIC_RELAXED);
}

// Word-wise copy for the snapshot serve (the larder_pagecopy shape; the kernel
// links no memcpy).
static void co_copy(u8 *dst, const u8 *src, u64 n) {
    if ((((u64)dst | (u64)src) & 7u) == 0) {
        u64 i = 0;
        for (; i + 8 <= n; i += 8) *(u64 *)(dst + i) = *(const u64 *)(src + i);
        for (; i < n; i++) dst[i] = src[i];
    } else {
        for (u64 i = 0; i < n; i++) dst[i] = src[i];
    }
}

// Map a 9P qid type to a Plan 9 QT* bit. The wire constants
// (P9_QT*) and the in-kernel constants (QT*) happen to share the
// same numeric values for the bits we care about (DIR=0x80, FILE=0x00,
// SYMLINK=0x02, AUTH=0x08, TMP=0x04). This is the 9P-spec mapping;
// pinned by inspection. We copy explicitly rather than rely on the
// numeric coincidence.
static u8 qid_type_p9_to_kernel(u8 p9) {
    // The 9P qid type bits we model at v1.0: DIR / SYMLINK / AUTH /
    // TMP. APPEND-mode + EXCL-mode aren't surfaced in the 9P2000.L
    // qid type byte directly (Linux v9fs carries those through mode
    // bits separately). At v1.0 the kernel-side QT* superset only
    // distinguishes DIR vs FILE for walk-time directory checks; we
    // can refine if more callers need finer-grained types.
    u8 out = 0;
    if (p9 & P9_QTDIR)     out |= QTDIR;
    if (p9 & P9_QTAUTH)    out |= QTAUTH;
    if (p9 & P9_QTTMP)     out |= QTTMP;
    // net-6b-2b: carry the readiness marker through so dev9p_poll's QTPOLL gate
    // (on the cached qid) sees it. A server that never sets it -> dev9p_poll is
    // POSIX always-ready (fail-safe). P9_QTPOLL == QTPOLL == 0x01.
    if (p9 & P9_QTPOLL)    out |= QTPOLL;
    return out;
}

// =============================================================================
// Public constructor.
// =============================================================================

struct Spoor *dev9p_attach_client(struct p9_client *client, u32 root_fid) {
    if (!client) return NULL;
    if (!p9_client_is_open(client)) return NULL;
    struct Spoor *c = spoor_alloc(&dev9p);
    if (!c) return NULL;
    // F2: test-path constructor — external p9_client lifecycle, no
    // p9_attached wrapper; attached_owner stays NULL. SYS_ATTACH_9P's
    // root Spoor goes through this constructor too, then gets its
    // attached_owner stamped in by sys_attach_9p_handler.
    struct dev9p_priv *p = priv_alloc(client, root_fid, /*fid_owned=*/false,
                                       /*attached_owner=*/NULL);
    if (!p) {
        // F238: uniform clunk-on-error (dev9p_close handles NULL c->aux
        // safely via priv_of's magic check). Pre-fix used spoor_unref;
        // both paths drop ref=1 → free in this case, but clunk is the
        // canonical Plan-9-shape teardown.
        spoor_clunk(c);
        return NULL;
    }
    c->aux = p;
    // Per-instance device number (Plan 9 Chan.dev). EVERY dev9p Spoor shares
    // dc='9' and every attach root has qid.path 0, so without a per-session
    // devno the mount table's (dc, qid.path) key cannot tell two concurrent
    // sessions apart (corvus + a per-user stratum-fs -- the A-5b case). One
    // fresh devno per attach session; walked + cloned descendants inherit it
    // via spoor_clone (the session is the instance). dev9p_attach_client is
    // called EXACTLY ONCE per session (SYS_ATTACH_9P / SYS_ATTACH_9P_SRV ->
    // p9_attached_root_spoor, both single-mint), so one devno == one session.
    c->devno = spoor_next_devno();
    // Root is always a directory; the qid path/vers come from the
    // server but at v1.0 we don't carry them at the Spoor layer (the
    // qid is meaningful per-walk via the Dev vtable). The cached qid
    // mostly matters when consumers stat the root; left as a placeholder.
    c->qid.type = QTDIR;
    c->qid.path = 0;
    c->qid.vers = 0;
    // #66: a 9P attach root is a filesystem root, named "/" (the namespace name
    // it carries when pivoted to OR before a cross overwrites it). Seeded HERE,
    // at birth, before the Spoor is published -> immutable thereafter (the I-33
    // set-before-publish discipline; no lock). This "/" is overwritten by a
    // transplant wherever the root is reached under another name: as a mount
    // SOURCE (stalk_cross_mounts stamps the mount-point name onto the crossed
    // clone) and as a devsrv open=connect endpoint (the stalk / walk_open
    // adoption arms stamp the opened path -- audit F2). So the raw "/" surfaces
    // only when the root IS the namespace root (joey's pivot target).
    // path_make_root NULL (OOM) -> "unknown", never fatal.
    c->path = path_make_root();
    return c;
}

int dev9p_client_fid(struct Spoor *c, struct p9_client **out_client, u32 *out_fid) {
    // Loom submit-time pin (I-30): resolve a registered dev9p Spoor to the
    // (client, fid) the engine dispatches an async op against. priv_of gates on
    // dc == DEV9P_DC + the priv magic, so a non-dev9p Spoor (devsrv conn,
    // devramfs, ...) is rejected -- Loom rejects such a registered handle at
    // submit. The returned client is valid only while the caller holds a ref on
    // `c`: a live dev9p Spoor implies a live client (dev9p's lifecycle invariant
    // -- the Spoor's own ops dereference the same pointer), and Loom holds an
    // independent spoor_ref pin across the op's lifetime.
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
    if (out_client) *out_client = p->client;
    if (out_fid)    *out_fid    = p->fid;
    return 0;
}

// =============================================================================
// Dev vtable ops.
// =============================================================================

static void dev9p_reset(void)    { /* no-op */ }
static void dev9p_init_noop(void) { /* registration happens in dev9p_init */ }
static void dev9p_shutdown(void) { /* no-op */ }

static struct Spoor *dev9p_attach_spec(const char *spec) {
    (void)spec;
    // dev9p Spoors are constructed via dev9p_attach_client, not the
    // standard Dev attach() path — the attach takes (client, root_fid)
    // which can't be encoded in a spec string. Caller bug if reached.
    return NULL;
}

static struct Walkqid *dev9p_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    struct dev9p_priv *src_priv = priv_of(c);
    if (!src_priv) return NULL;
    if (src_priv->fid == P9_NOFID) return NULL;   // fidless (cached-open) Spoor
    if (nname < 0 || nname > P9_MAX_WALK) return NULL;

    // F3 close (P5-stratumd-stub-bringup audit): allocate the Walkqid
    // carrier FIRST so a SLUB OOM here doesn't consume a fid number
    // from the client's monotonic allocator. (The fid pool wrap-around
    // is benign at v1.0 — the counter is monotonic over a u32 range —
    // but the discipline of "consume resources in the order they can
    // be released" is the right shape.)
    int max_qids = (nname == 0) ? 1 : nname;
    struct Walkqid *w = walkqid_alloc(max_qids);
    if (!w) return NULL;

    // Allocate a fresh fid for the destination.
    u32 new_fid = p9_client_alloc_fid(src_priv->client);
    if (new_fid == P9_NOFID) {
        walkqid_free(w);
        return NULL;
    }

    // Issue the 9P walk. nname=0 → clone; nname>0 → walk path.
    int rc;
    if (nname == 0) {
        // Clone: walk with empty name list.
        u16 nwqid = 0;
        struct p9_qid qids[P9_MAX_WALK];
        rc = p9_client_walk(src_priv->client, src_priv->fid, new_fid,
                              0, NULL, NULL, &nwqid, qids);
        if (rc != 0) {
            walkqid_free(w);
            return NULL;
        }
        // Clone returns 0 qids in 9P; we inherit src's cached qid for nc.
        w->nqid = 0;
    } else {
        // Path walk: pack names + lens.
        size_t lens[P9_MAX_WALK];
        for (int i = 0; i < nname; i++) {
            const char *s = name[i];
            size_t l = 0;
            while (s[l] != '\0') l++;
            lens[i] = l;
        }
        u16 nwqid = 0;
        struct p9_qid qids[P9_MAX_WALK];
        // Cast: const char ** → const u8 *const *. The codec
        // treats names as opaque bytes (it doesn't require NUL
        // termination — the explicit length arg defines extent).
        rc = p9_client_walk(src_priv->client, src_priv->fid, new_fid,
                              (u16)nname, (const u8 *const *)name,
                              lens, &nwqid, qids);
        if (rc != 0) {
            walkqid_free(w);
            return NULL;
        }
        // Caller asked for `nname` components; the server may have
        // walked fewer (partial walk). At v1.0 we treat that as a
        // failure — partial-walk handling is a Phase 5+ extension.
        if ((int)nwqid != nname) {
            walkqid_free(w);
            return NULL;
        }
        w->nqid = (int)nwqid;
        for (int i = 0; i < w->nqid; i++) {
            w->qid[i].path = qids[i].path;
            w->qid[i].vers = qids[i].version;
            w->qid[i].type = qid_type_p9_to_kernel(qids[i].type);
        }
    }

    // Install the new fid into nc (clone or walked target). nc was
    // pre-allocated by the caller (via spoor_clone-or-equivalent);
    // its aux is currently a shallow copy of src's aux (which we
    // must NOT free — src still uses it). Replace nc's aux with a
    // freshly allocated priv.
    // F2: walked priv inherits the source's attached_owner so the
    // walked Spoor's lifetime contributes a p9_attached_ref. dev9p_close
    // drops it; the last unref runs the attached's full teardown.
    // Pre-fix the walked priv had NO link to the attached → the root's
    // dev9p_close ran p9_attached_destroy immediately, leaving walked
    // privs dangling (R15 F236 UAF on subsequent walked Spoor close).
    struct dev9p_priv *new_priv = priv_alloc(src_priv->client, new_fid,
                                                /*fid_owned=*/true,
                                                src_priv->attached_owner);
    if (!new_priv) {
        // Best-effort: clunk the fid we just allocated. Ignore the
        // result — if the clunk fails, we still need to fail the walk.
        (void)p9_client_clunk(src_priv->client, new_fid);
        walkqid_free(w);
        return NULL;
    }
    nc->aux = new_priv;
    nc->qid.path = (w->nqid > 0) ? w->qid[w->nqid - 1].path : c->qid.path;
    nc->qid.vers = (w->nqid > 0) ? w->qid[w->nqid - 1].vers : c->qid.vers;
    nc->qid.type = (w->nqid > 0) ? w->qid[w->nqid - 1].type : c->qid.type;
    w->spoor = nc;
    return w;
}

static int dev9p_stat(struct Spoor *c, u8 *dp, int n) {
    // Stat surface is non-trivial — it composes Tgetattr + the Plan 9
    // stat wire format. Deferred to a follow-up chunk (the syscall +
    // mount integration chunks will exercise stat through the actual
    // syscall layer).
    (void)c; (void)dp; (void)n;
    return -1;
}

// p9_attr -> struct t_stat conversion, shared by dev9p_stat_native (Tgetattr)
// and dev9p_walk_attrs (the POUNCE Twalkgetattr per-component records — the
// two paths MUST report identical shapes for the same server attrs, or the
// pounce's X-search would diverge from the per-component loop's).
static void t_stat_from_p9_attr(struct t_stat *out, const struct p9_attr *attr) {
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;
    out->size      = attr->size;
    out->qid_path  = attr->qid.path;
    out->atime_sec = attr->atime_sec;
    out->mtime_sec = attr->mtime_sec;
    out->ctime_sec = attr->ctime_sec;
    out->nlink     = (u32)attr->nlink;
    out->qid_vers  = attr->qid.version;
    out->qid_type  = qid_type_p9_to_kernel(attr->qid.type);
    out->blksize   = attr->blksize ? (u32)attr->blksize : 4096u;
    out->blocks    = attr->blocks;
    // A-2a F2 (closed in A-2d): respect the server's `valid` mask for the
    // security-critical trio. A server that did not fill mode/uid/gid must NOT
    // have us report stale wire bytes -- leaving the pre-zeroed field is
    // fail-closed (mode 0 = no rwx bits; uid 0 = PRINCIPAL_INVALID, gid 0 =
    // GID_INVALID -- a real principal matches none, so perm_check denies). v1.0
    // Stratum fills BASIC, so this is dormant for the reference server; it makes
    // the A-3 dev9p enforcement (which reads these) sound against any server.
    if (attr->valid & P9_GETATTR_MODE) out->mode = attr->mode;
    if (attr->valid & P9_GETATTR_UID)  out->uid  = attr->uid;
    if (attr->valid & P9_GETATTR_GID)  out->gid  = attr->gid;
}

// Native fstat surface (A-2a; IDENTITY-DESIGN.md §9.5) -> Stratum Tgetattr.
// Fills *out from the server's Rgetattr. uid/gid are the server-reported owner
// + group; for a per-user stratumd they are the connection's principal (A-3
// completes per-user attribution). Unlike the .stat slot (the Plan 9 wire-stat,
// still deferred), this is the metadata source the kernel rwx layer (A-2d) and
// SYS_FSTAT consume. P9_GETATTR_BASIC covers mode/uid/gid/size/times/nlink.
static int dev9p_stat_native(struct Spoor *c, struct t_stat *out) {
    struct dev9p_priv *p = priv_of(c);
    if (!p || !out) return -1;
    // FID-LIFECYCLE cached-open: fstat serves the open-time snapshot stat --
    // the same close-to-open discipline as the attr cache (no fid to Tgetattr).
    if (p->cached_open) {
        *out = p->co_stat;
        return 0;
    }
    struct larder *l = &p->client->larder;
    u64 key = c->qid.path;
    // L1e gate: the attr cache engages ONLY for a cacheable client (a proven
    // content-versioned FS). A stream/control server (netd /net) is never latched
    // cacheable, so its attrs -- which the network mutates out of band (own-write
    // invalidation cannot cover an external writer) -- are never served stale from
    // the Larder. This closes the latent L1c gap (attr caching had no server gate).
    bool cacheable = __atomic_load_n(&p->client->cacheable, __ATOMIC_RELAXED);
    // L1c serve (fs_cache.tla Read): a cached attr for this qid.path is served
    // with NO RPC -- the base X-check (stalk.c) re-stat storm (root ~96.8% warm)
    // + fstat redundancy, the Larder's biggest cheap win. Close-to-open coherent
    // via the own-write invalidation on the mutation paths below.
    u64 seq0 = 0;
    if (cacheable && larder_attr_serve(l, key, out, &seq0))
        return 0;
    struct p9_attr attr;
    // errno-rollout: propagate the server's POSIX errno (p9_client_getattr
    // returns -ecode on Rlerror, e.g. -T_E_NOENT for a vanished file; I-14
    // bounds it to [-4095,-2], so it is never the generic -1/-T_E_PERM). The
    // caller (spoor_stat_native -> stalk/SYS_FSTAT) propagates it as -errno.
    int gr = p9_client_getattr(p->client, p->fid, P9_GETATTR_BASIC, &attr);
    if (gr != 0)
        return gr;
    t_stat_from_p9_attr(out, &attr);
    // L1c populate (fs_cache.tla Refetch): install {attr, cvers} keyed by
    // qid.path; the seq0 gen guard skips it if an invalidate raced this getattr
    // (the populate-after-invalidate resurrection close -- larder.h note (2)).
    // Gated on `cacheable` (a non-content-versioned server is never cached).
    if (cacheable)
        larder_attr_install(l, seq0, key, attr.qid.version, out);
    return 0;
}

// The POUNCE walk-fused getattr (docs/POUNCE-DESIGN.md §4) -> Twalkgetattr.
// Contract per <thylacine/dev.h> walk_attrs: bind form transitions nc ONLY on
// a full walk; query form (nc == NULL -> newfid = P9_NOFID) binds nothing on
// either end. The per-element p9_attr array is heap scratch (16 * ~152 B —
// too big to stack alongside stalk's own run arrays on the 16 KiB kstack).
_Static_assert(DEV_WALK_ATTRS_MAX == P9_MAX_WALK,
               "the vtable walk_attrs cap must equal the Twalkgetattr wire "
               "bound -- a resolver run must fit one wire op");
static struct Walkqid *dev9p_walk_attrs(struct Spoor *c, struct Spoor *nc,
                                        const char **name,
                                        const size_t *name_lens,
                                        int nname, struct t_stat *sts) {
    struct dev9p_priv *src_priv = priv_of(c);
    if (!src_priv) return NULL;
    if (src_priv->fid == P9_NOFID) return NULL;   // fidless (cached-open) Spoor
    if (nname <= 0 || nname > (int)P9_MAX_WALK) return NULL;
    if (!name || !name_lens || !sts) return NULL;

    // Per-session capability latch: Twalkgetattr is a Stratum extension.
    // netd's /net (and any plain 9P2000.L server) answers it Rlerror ENOSYS
    // -- latched below on the first probe, after which every walk_attrs on
    // this session degrades the resolver to the per-component loop with no
    // further RPC. Racy read is benign (one extra probe at worst).
    if (src_priv->client->wga_unsupported)
        return DEV_WALK_ATTRS_UNSUPPORTED;

    // L1d dentry serve (fs_cache.tla Read): resolve the whole run from the
    // dentry + attr sub-caches, skipping the Twalkgetattr. A FULL positive run
    // serves ONLY in the query form (nc == NULL -- there is no server fid to
    // bind, so no RPC is needed); a cached MISS (negative dentry) serves in
    // either form (a miss binds nothing). A bind-form full walk still RPCs (the
    // server must bind the fid) but re-populates below. Gated on `cacheable` (a
    // non-content-versioned server is never served from the Larder -- its caches
    // are provably empty, but the gate makes the invariant explicit + grep-able).
    if (__atomic_load_n(&src_priv->client->cacheable, __ATOMIC_RELAXED)) {
        int  nres    = 0;
        bool is_miss = false;
        if (larder_walk_serve(&src_priv->client->larder, c->qid.path,
                              (const char *const *)name, name_lens, nname, sts,
                              &nres, &is_miss)) {
            bool full = (!is_miss && nres == nname);
            if (is_miss || (full && nc == NULL)) {
                struct Walkqid *sw = walkqid_alloc(nname);
                if (sw) {
                    sw->nqid = nres;
                    for (int i = 0; i < nres; i++) {
                        sw->qid[i].path   = sts[i].qid_path;
                        sw->qid[i].vers   = sts[i].qid_vers;
                        sw->qid[i].type   = sts[i].qid_type;
                        sw->qid[i].pad[0] = sw->qid[i].pad[1] = sw->qid[i].pad[2] = 0;
                    }
                    sw->spoor = NULL;   // query form / miss: nothing bound
                    return sw;
                }
                // walkqid_alloc OOM -> fall through to the RPC (fail-safe).
            }
            // full positive in BIND form -> must RPC to bind the fid.
        }
    }

    // Carrier first (the F3 resource-order discipline shared with dev9p_walk).
    struct Walkqid *w = walkqid_alloc(nname);
    if (!w) return NULL;

    struct p9_attr *attrs = kmalloc(sizeof(struct p9_attr) * (size_t)nname, 0);
    if (!attrs) {
        walkqid_free(w);
        return NULL;
    }

    // Bind form allocates the destination fid; query form sends P9_NOFID.
    // A fid NUMBER consumed by a walk that then goes partial (never bound
    // server-side) is abandoned, exactly like dev9p_walk's failure paths —
    // benign under the monotonic-u32 allocator (numbers are never reused).
    u32 new_fid = P9_NOFID;
    if (nc) {
        new_fid = p9_client_alloc_fid(src_priv->client);
        if (new_fid == P9_NOFID) {
            kfree(attrs);
            walkqid_free(w);
            return NULL;
        }
    }

    // L1c populate guard: capture the Larder gen BEFORE the RPC so a concurrent
    // own-write that invalidates during the walk skips our (now-stale) install.
    u64 wga_seq0 = larder_gen_snapshot(&src_priv->client->larder);
    u16 nwqid = 0;
    struct p9_qid qids[P9_MAX_WALK];
    int rc = p9_client_walkgetattr(src_priv->client, src_priv->fid, new_fid,
                                   P9_GETATTR_BASIC, (u16)nname,
                                   (const u8 *const *)name, name_lens,
                                   &nwqid, qids, attrs);
    if (rc == -T_E_NOSYS) {
        // The server does not speak the extension (netd's unknown-op arm:
        // Rlerror E_NOSYS -- the one non-supporting server in the v1.0 set;
        // Stratum implements the op). Latch it for the session and hand the
        // resolver the fallback sentinel -- this is NOT a walk failure
        // (nothing about the path was learned). The abandoned new_fid number
        // is benign (monotonic allocator). A future foreign server replying
        // EOPNOTSUPP would need that code appended to the errno registry
        // (ERRORS.md signoff) and classified here.
        src_priv->client->wga_unsupported = true;
        kfree(attrs);
        walkqid_free(w);
        return DEV_WALK_ATTRS_UNSUPPORTED;
    }
    if (rc != 0 || nwqid > (u16)nname) {
        // L1d negative populate: a definitive first-component miss (Rlerror
        // ENOENT -- name[0] does not exist in c) is cacheable so a repeated
        // failed lookup serves RPC-free. Only for -T_E_NOENT (a clean "no such
        // name"); a transport/other error caches nothing. Gen-guarded.
        if (rc == -T_E_NOENT)
            larder_dentry_install(&src_priv->client->larder, wga_seq0,
                                  c->qid.path, name[0], name_lens[0], 0,
                                  /*negative=*/true);
        // Rlerror (miss at the first component / server error) or a
        // malformed over-count. Nothing was bound (the session layer binds
        // new_fid only on a FULL walk); nc untouched.
        kfree(attrs);
        walkqid_free(w);
        return NULL;
    }

    // L1e: a successful Twalkgetattr proves this mount's server speaks the POUNCE
    // extension -- the v1.0 proxy for "a content-versioned, offset-stable FS"
    // (Stratum). Latch cacheable so the Larder attr + page caches engage. A stream
    // / control server (netd /net -- Rlerror ENOSYS above, never reaching here)
    // stays non-cacheable, so its consuming reads are never page-cached. This runs
    // BEFORE any read of a walked file (a file is resolved via walk_attrs first),
    // so the gate is settled before the read path consults it. Monotonic false ->
    // true; a benign one-word race (concurrent walks both latch true).
    __atomic_store_n(&src_priv->client->cacheable, true, __ATOMIC_RELAXED);

    w->nqid = (int)nwqid;
    // The parent of component i: c for i==0, else the previous walked component.
    u64 prev_path = c->qid.path;
    for (int i = 0; i < w->nqid; i++) {
        w->qid[i].path   = qids[i].path;
        w->qid[i].vers   = qids[i].version;
        w->qid[i].type   = qid_type_p9_to_kernel(qids[i].type);
        w->qid[i].pad[0] = w->qid[i].pad[1] = w->qid[i].pad[2] = 0;
        t_stat_from_p9_attr(&sts[i], &attrs[i]);
        // L1c populate (free -- attrs already fetched): install each walked
        // component's attr keyed by its qid.path, with its content-version. A
        // getattr/walk_attrs qid carries the true si_cvers (never a readdir qid
        // -- the L1a-2 audit-F1 rule; LARDER-DESIGN section 3.2).
        larder_attr_install(&src_priv->client->larder, wga_seq0,
                            w->qid[i].path, w->qid[i].vers, &sts[i]);
        // L1d populate: install the POSITIVE dentry (prev_path, name[i]) ->
        // qids[i].path. Same gen guard as the attr install (skipped if a
        // concurrent create/rename/unlink bumped gen during the RPC).
        larder_dentry_install(&src_priv->client->larder, wga_seq0, prev_path,
                              name[i], name_lens[i], w->qid[i].path,
                              /*negative=*/false);
        prev_path = w->qid[i].path;
    }
    kfree(attrs);

    // L1d negative populate: a PARTIAL walk (nwqid < nname, rc == 0) missed at
    // component nwqid -- name[nwqid] does not exist in prev_path (the last walked
    // component, or c for nwqid == 0). Cache the negative so the failed-lookup
    // storm is served RPC-free. (A first-component Rlerror ENOENT is cached at
    // the rc handling above.)
    if ((int)nwqid < nname)
        larder_dentry_install(&src_priv->client->larder, wga_seq0, prev_path,
                              name[nwqid], name_lens[nwqid], 0, /*negative=*/true);

    if (nc && w->nqid == nname) {
        // FULL walk: new_fid is bound server-side; install it into nc (the
        // dev9p_walk tail — same priv shape, same attached_owner inheritance).
        struct dev9p_priv *new_priv = priv_alloc(src_priv->client, new_fid,
                                                 /*fid_owned=*/true,
                                                 src_priv->attached_owner);
        if (!new_priv) {
            (void)p9_client_clunk(src_priv->client, new_fid);
            walkqid_free(w);
            return NULL;
        }
        nc->aux = new_priv;
        nc->qid = w->qid[w->nqid - 1];
        w->spoor = nc;
    } else {
        // Partial walk (bind form: new_fid was never bound; nc still
        // shallow-shares c's aux — the caller detaches + unrefs it) or the
        // query form. Nothing to clunk.
        w->spoor = NULL;
    }
    return w;
}

// FID-LIFECYCLE cached-open (docs/FID-LIFECYCLE-DESIGN.md section 3.3; refines
// I-38): the fidless close-to-open open. stalk calls this on the FINAL run of a
// plain read-only STALK_OPEN resolution, BEFORE the normal bind walk. Contract
// per <thylacine/dev.h> open_cached: on success the returned Spoor is OPENED +
// fidless and sts[0..nname) holds the walk's FRESH per-component records for
// the RESOLVER's mandatory fail-ordering post-scan (permission policy stays in
// stalk -- I-28/I-22); on NULL nothing was bound or revealed and the caller's
// observable outcome comes from the normal path.
static struct Spoor *dev9p_open_cached(struct Spoor *c, const char *const *names,
                                       const size_t *name_lens, int nname,
                                       struct t_stat *sts) {
    struct dev9p_priv *src_priv = priv_of(c);
    if (!src_priv || !names || !name_lens || !sts) return NULL;
    if (nname <= 0 || nname > (int)P9_MAX_WALK) return NULL;
    if (src_priv->fid == P9_NOFID) return NULL;   // never walk FROM a fidless Spoor
    struct p9_client *client = src_priv->client;
    if (client->wga_unsupported) return NULL;
    if (!__atomic_load_n(&client->cacheable, __ATOMIC_RELAXED)) return NULL;
    struct larder *l = &client->larder;

    // The gen witness (B1-audit F1): captured BEFORE the hint's coverage
    // decision; larder_pages_snapshot fails closed if ANY invalidate moves
    // the gen between here and the step-4 copy (the third-actor stale-fid
    // repopulate hole). Also serves as the strict path's install guard.
    u64 seq0 = larder_gen_snapshot(l);

    // 1. The RPC-free HINT: dentry-chain the run + the leaf attr + full page
    //    coverage, all from the caches under the Larder lock. A non-eligible
    //    open (chain not cached, negative, dir/special, oversize, not covered)
    //    costs the normal path nothing beyond this consult. On a STRICT
    //    client the hint is never authoritative -- step 3 re-checks against
    //    the FRESH records the step-2 wire query refills; on a LOOSE client
    //    (B1) the hint records ARE the post-scan input (no refill). It fills
    //    the CALLER's sts as scratch (the dev.h contract: sts may be scribbled
    //    on a NULL return) -- a second t_stat[16] here would stack ~1.3 KiB on top of
    //    stalk_core's own run arrays above the deep wire call chain.
    {
        int  nres    = 0;
        bool is_miss = false;
        if (!larder_walk_serve(l, c->qid.path, names, name_lens, nname, sts,
                               &nres, &is_miss))
            { return NULL; }
        if (is_miss || nres != nname)
            { return NULL; }
        const struct t_stat *hleaf = &sts[nname - 1];
        if (hleaf->qid_type != 0)
            { return NULL; }
        if (hleaf->size > (u64)DEV9P_CO_MAX_SIZE)
            { return NULL; }
        if (!larder_pages_cover(l, hleaf->qid_path, hleaf->qid_vers,
                                hleaf->size))
            { return NULL; }
    }

    // 2. The FORCED-WIRE revalidation: the query-form Twalkgetattr issued
    //    DIRECTLY at the client -- deliberately BYPASSING dev9p_walk_attrs and
    //    its L1d dentry serve (which would answer a query-form full-positive
    //    run from the caches). On a STRICT client the B2 revalidation MUST be
    //    server-fresh, or cached-open silently degrades to B1/loose (the
    //    FID-LIFECYCLE prosecution item). newfid = P9_NOFID: no fid binds on
    //    either end, exactly the walk-query shape.
    //
    //    B1 per-attach loose mode (I-38 opt-in; user-voted option B
    //    2026-07-11, docs/chase/B1-VOTE.md + the ARCH I-38 row): a LOOSE
    //    client (client->loose, set once at attach before the root
    //    published) skips the wire query on a FULL hint hit -- the step-1
    //    records already in sts ARE the post-scan input (the same cached
    //    attrs the L1c base X-check serves; the permission axis is not
    //    weakened beyond L1c's accepted discipline), and step 4 snapshots
    //    at the CACHED cvers (hleaf->qid_vers) with the identical
    //    Larder-lock atomicity. Any hint miss returned NULL above -- a
    //    loose first touch takes the normal path and populates via its
    //    walk_attrs, so the wire is never skipped on unproven state.
    if (!client->loose) {
        struct p9_attr *attrs = kmalloc(sizeof(struct p9_attr) * (size_t)nname, 0);
        if (!attrs) return NULL;
        u16 nwqid = 0;
        struct p9_qid qids[P9_MAX_WALK];
        int rc = p9_client_walkgetattr(client, src_priv->fid, P9_NOFID,
                                       P9_GETATTR_BASIC, (u16)nname,
                                       (const u8 *const *)names, name_lens,
                                       &nwqid, qids, attrs);
        if (rc == -T_E_NOSYS) {
            // The same per-session latch dev9p_walk_attrs maintains (a server
            // that does not speak the extension never reaches the hint anyway
            // -- it is never latched cacheable -- but keep the latch coherent).
            client->wga_unsupported = true;
            kfree(attrs);
            return NULL;
        }
        if (rc != 0 || nwqid != (u16)nname) {
            // The fresh tree disagrees with the cached hint (a concurrent
            // unlink/rename). Nothing bound; the normal path's own walk
            // produces the observable outcome with its own fail ordering.
            kfree(attrs);

            return NULL;
        }

        // Fill the caller's sts (the post-scan input) and re-populate the
        // caches from the fresh records -- the dev9p_walk_attrs populate
        // discipline (gen-guarded; a concurrent own-write that invalidated
        // during the RPC skips the install).
        u64 prev_path = c->qid.path;
        for (int i = 0; i < nname; i++) {
            t_stat_from_p9_attr(&sts[i], &attrs[i]);
            larder_attr_install(l, seq0, attrs[i].qid.path, attrs[i].qid.version,
                                &sts[i]);
            larder_dentry_install(l, seq0, prev_path, names[i], name_lens[i],
                                  attrs[i].qid.path, /*negative=*/false);
            prev_path = attrs[i].qid.path;
        }
        kfree(attrs);
    }

    // 3. Fresh-leaf gates (authoritative on the strict path -- the hint may
    //    be stale vs the wire refill; on the loose path they re-run over the
    //    hint records, redundant with step 1 but kept as one shared gate).
    const struct t_stat *leaf = &sts[nname - 1];
    if (leaf->qid_type != 0) { return NULL; }
    if (leaf->size > (u64)DEV9P_CO_MAX_SIZE)
        { return NULL; }

    // 4. Budget + the snapshot at the leaf cvers (wire-fresh on strict;
    //    cached on loose -- the B1 contract), under ONE Larder lock hold
    //    (atomic vs a concurrent own-write invalidate -- it precedes, failing
    //    the coverage, or follows the whole copy). Any failure falls back with
    //    everything released; size == 0 (the empty file) snapshots trivially.
    u64 size = leaf->size;
    if (!co_budget_charge(size)) { return NULL; }
    u8 *buf = NULL;
    if (size > 0) {
        buf = kmalloc((size_t)size, 0);
        if (!buf) { co_budget_uncharge(size); return NULL; }
    }
    if (!larder_pages_snapshot(l, leaf->qid_path, leaf->qid_vers, size, buf,
                               seq0)) {
        if (buf) kfree(buf);
        co_budget_uncharge(size);

        return NULL;
    }

    // 5. Mint the opened fidless Spoor. The clone inherits dc/devno/path from
    //    the run's base (the walked-child pattern); a fresh priv replaces the
    //    clone's shared aux. fid = P9_NOFID + fid_owned = false: dev9p_close
    //    does no wire op -- its cached_open arm frees the snapshot + uncharges.
    struct Spoor *co = spoor_clone(c);
    if (!co) {
        if (buf) kfree(buf);
        co_budget_uncharge(size);
        return NULL;
    }
    struct dev9p_priv *np = priv_alloc(client, P9_NOFID, /*fid_owned=*/false,
                                       src_priv->attached_owner);
    if (!np) {
        co->aux = NULL;                 // still shares c's aux; detach first
        spoor_unref(co);
        if (buf) kfree(buf);
        co_budget_uncharge(size);
        return NULL;
    }
    np->cached_open = true;
    np->co_buf      = buf;
    np->co_size     = size;
    np->co_stat     = *leaf;
    co->aux      = np;
    co->qid.path = leaf->qid_path;
    co->qid.vers = leaf->qid_vers;
    co->qid.type = leaf->qid_type;
    co->flag    |= COPEN;
    co->mode     = 0;                   // OREAD (the stalk gate admits omode 0 only)
    co->offset   = 0;
    // #66: the walked components join the namespace name (non-load-bearing,
    // I-33 -- an OOM leaves the name short; the open still succeeds).
    for (int i = 0; i < nname; i++)
        spoor_path_extend(co, names[i], name_lens[i]);

    return co;
}

static struct Spoor *dev9p_open(struct Spoor *c, int omode) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return NULL;
    if (p->fid == P9_NOFID) return NULL;   // a fidless (cached-open) Spoor is
                                           // already open; no fid to Tlopen
    // Map Plan 9 omode → Linux O_* flags. Plan 9: OREAD=0, OWRITE=1,
    // ORDWR=2, OEXEC=3, OTRUNC=0x10, OCEXEC=0x20, ORCLOSE=0x40,
    // OEXCL=0x1000. Linux: O_RDONLY=0, O_WRONLY=1, O_RDWR=2,
    // O_TRUNC=01000, O_CLOEXEC=02000000, O_EXCL=0200.
    // At v1.0 we forward the low 2 bits (rdonly/wronly/rdwr) directly
    // since they match; richer flag translation lands when Plan 9 +
    // Linux callers diverge.
    u32 flags = (u32)(omode & 0x3);
    // OEXEC (Plan 9 access mode 3) -> O_RDONLY (#58 exec-from-namespace). 9P2000.L
    // has no exec-open, and `flags & O_ACCMODE == 3` is the INVALID Linux access
    // mode (a conformant server rejects it -EINVAL; Stratum only works today by a
    // permissive read-gate). The kernel just READS the file to exec it -- the
    // X-permission was enforced identity-side at stalk's perm_check(PERM_R|PERM_X)
    // BEFORE this open -- so a dev9p OEXEC-open reads the bytes as O_RDONLY.
    if (flags == 3) flags = 0;
    if (omode & 0x10) flags |= 01000;       // OTRUNC → O_TRUNC
    struct p9_qid qid;
    u32 iounit;
    int rc = p9_client_lopen(p->client, p->fid, flags, &qid, &iounit);
    if (rc != 0) return NULL;
    // Update the cached qid with the server's response.
    c->qid.path = qid.path;
    c->qid.vers = qid.version;
    c->qid.type = qid_type_p9_to_kernel(qid.type);
    c->flag |= COPEN;
    c->mode  = omode;
    c->offset = 0;
    return c;
}

// Create `name` in the directory c (c's fid is a private clone the caller
// already walked to the parent dir) and OPEN it; on success c refers to the
// new opened object. perm's low 9 bits = POSIX mode; the DMDIR bit selects a
// directory (Tmkdir) over a file (Tlcreate). gid is carried into the 9P gid
// field. Returns c on success, NULL on failure (the caller spoor_clunks c,
// whose dev9p_close clunks the walked fid).
static struct Spoor *dev9p_create(struct Spoor *c, const char *name,
                                    int omode, u32 perm, u32 gid) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return NULL;
    if (p->fid == P9_NOFID) return NULL;   // fidless (cached-open) Spoor
    if (!name) return NULL;
    // Capture the PARENT's qid.path before Tlcreate/Tmkdir transitions c->qid to
    // the new child -- the L1c invalidate below drops the parent's cached attr.
    u64 parent_path = c->qid.path;

    // Name length: the handler NUL-terminates within SYS_WALK_OPEN_NAME_MAX,
    // so this scan is bounded. p9_client_* take an explicit length.
    size_t name_len = 0;
    while (name[name_len] != '\0') name_len++;
    if (name_len == 0) return NULL;

    u32 mode = perm & 0777u;
    struct p9_qid qid;
    u32 iounit = 0;

    if (perm & SYS_WALK_CREATE_DMDIR) {
        // Directory: Tmkdir leaves p->fid at the PARENT, so after creating
        // the dir we walk parent->name into a fresh fid, swap it in, and
        // lopen it OREAD (you readdir a directory, never write it).
        int rc = p9_client_mkdir(p->client, p->fid, (const u8 *)name, name_len,
                                  mode, gid, &qid);
        if (rc != 0) return NULL;                 // p->fid still parent; caller clunks

        u32 dir_fid = p9_client_alloc_fid(p->client);
        if (dir_fid == P9_NOFID) return NULL;     // dir created; can't open it

        const u8 *names[1] = { (const u8 *)name };
        size_t   lens[1]  = { name_len };
        u16 nwqid = 0;
        struct p9_qid wq[1];
        rc = p9_client_walk(p->client, p->fid, dir_fid, 1,
                             (const u8 *const *)names, lens, &nwqid, wq);
        if (rc != 0 || nwqid != 1) {
            (void)p9_client_clunk(p->client, dir_fid);
            return NULL;                          // p->fid still parent; caller clunks
        }
        // Swap: clunk the parent clone, adopt the new-dir fid. From here a
        // failure leaves p->fid == dir_fid so dev9p_close clunks the right one.
        (void)p9_client_clunk(p->client, p->fid);
        p->fid = dir_fid;

        rc = p9_client_lopen(p->client, dir_fid, 0u /* OREAD */, &qid, &iounit);
        if (rc != 0) return NULL;
        c->mode = 0;                              // OREAD
    } else {
        // File: Tlcreate creates AND opens; afterward p->fid refers to the
        // new file. Map Plan 9 omode -> Linux O_* (same shape as dev9p_open).
        u32 flags = (u32)(omode & 0x3);
        if (omode & 0x10) flags |= 01000u;        // OTRUNC -> O_TRUNC
        int rc = p9_client_lcreate(p->client, p->fid, (const u8 *)name, name_len,
                                    flags, mode, gid, &qid, &iounit);
        if (rc != 0) return NULL;                 // p->fid still parent; caller clunks
        c->mode = omode;
    }

    c->qid.path = qid.path;
    c->qid.vers = qid.version;
    c->qid.type = qid_type_p9_to_kernel(qid.type);
    c->flag    |= COPEN;
    c->offset   = 0;
    // L1c invalidate (fs_cache.tla OwnWrite): a create changes TWO qid.paths.
    //  - the PARENT dir (nlink/mtime/cvers) -- so its base X-check + fstat see
    //    the new metadata.
    //  - the CHILD itself: Stratum may reuse a just-freed ino, so the new file's
    //    qid.path can carry a STALE prior-occupant attr in the Larder (a deleted
    //    dir/file's mode). Unlike a walk, the create path never runs walk_attrs
    //    (no revalidate-by-overwrite), so the stale entry would be served by the
    //    next stat -- the stalk-2-e2e delete+recreate+create-in-it failure. Drop
    //    it; the next stat refetches fresh (create returns only the qid, not a
    //    full attr to populate).
    struct larder *l = &p->client->larder;
    larder_attr_invalidate(l, parent_path);
    larder_attr_invalidate(l, c->qid.path);
    // L1e invalidate (L1f audit F1): the reused-ino hazard applies to the
    // child's PAGES exactly as to its attr. A create at a freed+reused qid.path
    // can carry a STALE prior-occupant page whose cvers collides with the fresh
    // file's qid.version -- a collision the Thylacine tree cannot rule out (it
    // depends on Stratum's fresh-inode si_cvers assignment). Drop the child's
    // pages too, mirroring the attr defense above: data integrity must NOT rest
    // on an unstated cross-project version-uniqueness guarantee. (Usually a
    // no-op -- a genuinely-new qid.path has no cached pages.)
    larder_page_invalidate(l, c->qid.path);
    // L1d invalidate (fs_cache.tla OwnWrite): the create added a name to the
    // parent's dirent set -- drop the parent's cached dentries so a stale
    // NEGATIVE entry (parent_path, name) cannot serve ENOENT for the new file.
    larder_dentry_invalidate_parent(l, parent_path);
    return c;
}

static void dev9p_close(struct Spoor *c) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return;

    // net-6b-2b + #294: release the readiness poll-state (if this was a netd
    // `ready` file). A registered poller holds the Spoor obj-ref, so poll_list is
    // empty here, but an outstanding readiness op may be live (it pins the
    // refcounted poll-state + the session, NOT the Spoor). priv_release cancels
    // that op at the client (Tflush) BEFORE the fid_owned Tclunk below delivers the
    // `ready`-fd clunk -- so the clunk frees the netd slot deterministically at
    // fd-close (the cancel-at-close leak fix) without orphaning the held Tread.
    dev9p_poll_priv_release(p);

    // Weft-6a-2: release the per-flow ring binding (if this data fd went
    // zero-copy). Drops the I-30 registration pin -> the #847 dual count frees
    // the ring Burrow once the guest's mapping also drops (vma_drain at guest
    // exit). LAST-ref runs here, so no concurrent reader can race the clear:
    // EVERY reader of p->weft holds a Spoor ref while it reads -- the sync
    // paths (dev9p_weft_try_{write,read}) via sys_lookup_rw_handle's handle ref,
    // AND the Loom async path (loom_submit_payload) via the reg-handle table's
    // spoor_ref (op->pinned, held submit..reap) -- and a held ref excludes this
    // last-ref close. Weft-7 F3: the clear is a RELEASE store paired with those
    // readers' ACQUIRE loads (symmetry + defense-in-depth: the ref-exclusion
    // makes it non-racy today; the pairing keeps it sound if a future reader
    // ever reads priv->weft without first taking the Spoor ref the invariant
    // above requires). NULL the slot BEFORE freeing so it never names a freed
    // binding.
    struct weft_binding *wb = __atomic_load_n(&p->weft, __ATOMIC_ACQUIRE);
    if (wb) {
        __atomic_store_n(&p->weft, NULL, __ATOMIC_RELEASE);
        weft_binding_release(wb);
    }

    // FID-LIFECYCLE cached-open: free the snapshot + release the global
    // budget. fid_owned is false on a fidless priv, so the clunk branch below
    // is naturally skipped -- the whole close is wire-free.
    if (p->cached_open) {
        if (p->co_buf) kfree(p->co_buf);
        co_budget_uncharge(p->co_size);
        p->co_buf = NULL;
    }

    // F2 (F236 close) discipline — order matters:
    //
    //   1. fid_owned: clunk the walked-fid via the client BEFORE the
    //      attached_owner unref. The unref might be the last drop and
    //      trigger p9_client_destroy; we need the client alive for the
    //      wire round-trip. (Test paths with attached_owner==NULL still
    //      hit this branch; their client lifecycle is externally
    //      managed and stays valid.)
    //
    //   2. attached_owner unref: drops this priv's hold on the
    //      session-resource holder. On the LAST drop (when the root +
    //      every walked Spoor have closed) attached_destroy_inner
    //      runs the full teardown (clunk root_fid + p9_client_close +
    //      p9_client_destroy + free buffers + spoor_clunk transports +
    //      kfree adapter + kfree(attached)).
    //
    //   3. magic clobber + kfree priv as today.
    //
    // Pre-fix the root branch ran p9_attached_destroy IMMEDIATELY and
    // tore down the adapter — walked privs closing afterward UAF'd via
    // their stale client pointer (R15 F236).
    if (p->fid_owned) {
        // Walk-derived Spoor: clunk the fid. FID-LIFECYCLE async-clunk -- the
        // normal close path fires the Tclunk fire-and-forget (the submitter is
        // not parked for the clunk RTT; the fid unbinds at send + its number is
        // never reused, and the ownerless Rclunk drains via a later op's reader,
        // the #845 discipline). Ignore the result -- close-then-error has no good
        // recovery; the fid is gone from the client's table either way per the
        // wire spec. (Error/rollback clunks in walk/walk_attrs/create stay
        // synchronous -- off the hot path, correctness over latency.)
        // p9_client_clunk_async now composes internally with the #841 tag pool
        // (it drains ownerless Rclunks on a full pool -- the #926 proc-exit
        // close-burst -- before send, so the fid never leaks bound) and the #349
        // c2s back-pressure (EAGAIN is retried, never a session death). A
        // non-zero return is a can't/shouldn't-clunk (session dead / fid unbound
        // / root / a live op targets it) OR the narrow burst-during-a-kill race
        // (pool full AND the Proc dying -- the fid then leaks bound exactly as
        // the old sync clunk did on the same race, session-teardown-bounded;
        // round-2 F1). Ignored exactly as a sync-clunk error was, no fallback.
        (void)p9_client_clunk_async(p->client, p->fid);
    }

    if (p->attached_owner) {
        p9_attached_unref(p->attached_owner);
        p->attached_owner = NULL;
    }

    // Release the priv allocation. SLUB's freelist write clobbers
    // offset 0 (magic) on free; subsequent priv_of will see the
    // wrong magic and return NULL (UAF defense).
    p->magic = 0;
    kfree(p);
    c->aux = NULL;
}

static long dev9p_read(struct Spoor *c, void *buf, long n, s64 off) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
    if (n <= 0) return 0;
    // u32 cap on the read count + s64-to-u64 cast on offset.
    u32 count = (n > 0x7fffffffL) ? 0x7fffffffu : (u32)n;
    u64 offset = (off < 0) ? 0 : (u64)off;
    // FID-LIFECYCLE cached-open serve: the immutable open-time snapshot
    // (section 3.3) -- every read of this open sees the open-time content
    // (close-to-open). No lock, no RPC: the buffer never mutates post-mint.
    if (p->cached_open) {
        if (offset >= p->co_size) return 0;             // EOF
        u64 rem  = p->co_size - offset;
        u64 take = (count > rem) ? rem : (u64)count;
        co_copy((u8 *)buf, p->co_buf + offset, take);
        return (long)take;
    }
    struct larder *l = &p->client->larder;
    bool cacheable = __atomic_load_n(&p->client->cacheable, __ATOMIC_RELAXED);
    // L1e page serve (fs_cache.tla Read): the ONE cached page containing `offset`,
    // fresh (cvers == this fid's qid.vers) + in range, is served RPC-free. One page
    // per call bounds the <= 4 KiB copy under the Larder lock; a short serve is a
    // legal short read the caller loops on. Gated on `cacheable` -- a stream server
    // (netd /net: consuming reads, qid.version 0) is never page-cached, so an offset
    // re-read can never serve stale stream bytes. `buf` is a kernel bounce buffer
    // (SYS_READ scratch), so the copy is kernel-to-kernel (no uaccess).
    u64 seq0 = 0;
    if (cacheable) {
        // Task-#44 attr-served EOF: a FRESH cached attr (cvers == this fid's
        // open-time qid.vers -- the page-serve freshness rule) answers the
        // sequential reader's final read-returns-0 probe RPC-free. Sound under
        // I-38 close-to-open + own-write invalidation: the fresh size IS the
        // open-time size, so the wire would answer 0 identically. PLAIN FILES
        // only (qid.type 0): a dir's cached size must not convert the server's
        // read-on-directory ERROR into a silent 0.
        u64 fsz;
        if (c->qid.type == 0 &&
            larder_attr_fresh_size(l, c->qid.path, c->qid.vers, &fsz) &&
            offset >= fsz)
            return 0;
        u64 page_index = offset / LARDER_PAGE_SIZE;
        u32 page_off   = (u32)(offset % LARDER_PAGE_SIZE);
        u32 served = larder_page_serve(l, c->qid.path, page_index, page_off,
                                       count, c->qid.vers, (u8 *)buf, &seq0);
        if (served > 0)
            return (long)served;
    }
    // Task-#44 aligned wire read: a big unaligned read on a cacheable client is
    // issued at the containing page's ALIGNED start instead (a legal short read
    // -- the caller loops). Why: the msize payload (131049 B) is not a page
    // multiple, so a sequential stream's chunks each end in a PARTIAL page; the
    // populate below cannot fill a partial-front page, so the hole persisted
    // forever and every re-read of the stream pv-missed at the hole and re-paid
    // the wire for the whole tail (the measured 82%-of-misses class: the go
    // tools' multi-MB rodata segments eagerly re-read per exec). Fetching from
    // the aligned start fully REWRITES the prior chunk's partial tail page, so
    // holes heal and the second pass serves from pages. Cost: <= 4095 duplicate
    // bytes per chunk (~3%). Small reads (<= one page) keep the exact path --
    // they never populate, so they cannot create holes.
    u64 wire_off = offset;
    u32 lead = 0;
    if (cacheable && count > LARDER_PAGE_SIZE) {
        u32 mis = (u32)(offset % LARDER_PAGE_SIZE);
        if (mis) { wire_off = offset - mis; lead = mis; }
    }
    u32 got = 0;
    int rc = p9_client_read(p->client, p->fid, wire_off, count, (u8 *)buf, &got);
    // #3 (Area F errno-rollout): propagate the real ecode, not -1. See dev9p_write.
    if (rc != 0) return (long)rc;
    // L1e page populate (fs_cache.tla Refetch): cache each page the read covered
    // FROM ITS ALIGNED START -- a page whose start is within [wire_off, end)
    // holds bytes [0, valid_len) with no hole. On the aligned path wire_off IS
    // page-aligned, so the front page installs FULL (the hole heal); on the
    // exact path an unaligned front page is skipped as before. cvers = this
    // fid's qid.vers; the seq0 gen guard (captured at the serve miss above)
    // skips a fill that raced an own-write.
    if (cacheable && got > 0) {
        u64 end = wire_off + (u64)got;
        // First page whose aligned start is >= wire_off.
        u64 ps = (wire_off + (LARDER_PAGE_SIZE - 1)) / LARDER_PAGE_SIZE * LARDER_PAGE_SIZE;
        for (; ps < end; ps += LARDER_PAGE_SIZE) {
            u64 rem  = end - ps;
            u32 plen = (rem >= LARDER_PAGE_SIZE) ? LARDER_PAGE_SIZE : (u32)rem;
            larder_page_install(l, seq0, c->qid.path, ps / LARDER_PAGE_SIZE,
                                c->qid.vers, (const u8 *)buf + (ps - wire_off), plen);
        }
    }
    if (lead) {
        // The caller's bytes start `lead` into the fetched window. got <= lead
        // means the file ends at/before the caller's offset -> EOF. The shift is
        // a forward copy with dst < src (overlap-safe).
        if (got <= lead) return 0;
        u32 cgot = got - lead;
        co_copy((u8 *)buf, (const u8 *)buf + lead, cgot);
        return (long)cgot;
    }
    return (long)got;
}

static struct Block *dev9p_bread(struct Spoor *c, long n, s64 off) {
    // Block I/O is a Plan 9-ism that isn't strictly necessary on top
    // of byte-stream read/write. We don't implement it at v1.0 —
    // callers use the byte-stream path.
    (void)c; (void)n; (void)off;
    return NULL;
}

static long dev9p_write(struct Spoor *c, const void *buf, long n, s64 off) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
    if (p->cached_open) return -1;   // read-only fidless open (defense-in-depth;
                                     // the omode-derived handle rights already deny)
    if (n <= 0) return 0;
    u32 count = (n > 0x7fffffffL) ? 0x7fffffffu : (u32)n;
    u64 offset = (off < 0) ? 0 : (u64)off;
    u32 accepted = 0;
    int rc = p9_client_write(p->client, p->fid, offset, count,
                              (const u8 *)buf, &accepted);
    // #3 (Area F errno-rollout): propagate the real Stratum ecode instead of
    // collapsing to -1. p9_client_write returns -(T_E_*) on an Rlerror (e.g.
    // -T_E_NOSPC, -T_E_IO) or -P9_E_IO on transport death, with P9_E_* ==
    // T_E_* == POSIX, so rc is already a valid -errno in [-4095,-1] --
    // sys_write_for_proc propagates it and userspace sees the precise errno
    // (ENOSPC/EACCES/...) instead of the pre-#3 generic -1, which the
    // pouch/native boundary decodes to EIO (NOT EPERM -- errno.h forbids a
    // handler returning -T_E_PERM=1). One residual: a server EPERM (Rlerror
    // ecode==1) collides with the -1 generic sentinel -> EIO; conveying it
    // needs a wider channel (the ER-rollout's job).
    if (rc != 0) return (long)rc;
    // L1c/L1e invalidate (fs_cache.tla OwnWrite): the file's attrs (size/mtime/
    // cvers) AND its content changed. Drop its cached attr entry AND every cached
    // page so a subsequent stat/fstat/read cannot serve the pre-write metadata or
    // bytes -- the guest sees its own write strongly (unconditional: harmless
    // no-ops for a non-cacheable client, whose caches are empty).
    larder_attr_invalidate(&p->client->larder, c->qid.path);
    larder_page_invalidate(&p->client->larder, c->qid.path);
    return (long)accepted;
}

// Weft-6b-2 data drive: a large SYS_WRITE whose buffer points INTO a weft-bound
// /net data fd's shared ring moves zero-copy -- the kernel validates the
// descriptor against the flow's private ring view (the I-30 validator-once) and
// issues Tweftio(WRITE); netd reads the ring IN PLACE + replies the moved-byte
// count. Returns 1 if handled (the weft path was taken; *accepted set), 0 if NOT
// a weft write (the caller falls back to the byte-copy path -- small payload,
// not weft-bound, or a buffer outside the ring), -1 on a weft transport error.
// NOT the byte-copy path: the payload is already in the shared ring (the native
// client wrote it there), so nothing is copied through the 9P body.
int dev9p_weft_try_write(struct Spoor *spoor, u64 ubuf_va, u32 len, u32 *accepted) {
    struct dev9p_priv *p = priv_of(spoor);
    if (!p) return 0;                                   // not dev9p -> byte-copy
    struct weft_binding *b = __atomic_load_n(&p->weft, __ATOMIC_ACQUIRE);
    if (!b) return 0;                                   // not weft-bound -> byte-copy
    if (!weft_should_ring(len)) return 0;               // below the hybrid threshold
    u32 off = 0;
    if (weft_binding_validate_rw(b, ubuf_va, len, &off) != 0)
        return 0;                                       // buffer not in the ring -> byte-copy
    u32 got = 0;
    int e = p9_client_weftio(p->client, p->fid, off, len, WEFT_DIR_WRITE, &got);
    if (e != 0) return -1;                              // weft attempted + failed (dead flow)
    if (accepted) *accepted = got;
    return 1;                                           // handled zero-copy
}

// Weft-6b-3 data drive (RX): a large SYS_READ whose buffer points INTO a
// weft-bound /net data fd's shared ring recvs zero-copy -- the kernel validates
// the destination descriptor against the flow's private ring view (the I-30
// validator-once) and issues Tweftio(READ); netd recvs from the socket IN PLACE
// into the ring + replies the recv'd byte count. Returns 1 if handled (*got set),
// 0 if NOT a weft read (the caller falls back to the byte-copy path -- small
// payload, not weft-bound, or a buffer outside the ring), -1 on a weft transport
// error. NOT the byte-copy path: netd writes the bytes directly into the guest's
// shared ring, so nothing is copied back through the 9P body OR the SYS_READ
// scratch -- the SYS_READ handler does NO uaccess_store on this path.
int dev9p_weft_try_read(struct Spoor *spoor, u64 ubuf_va, u32 len, u32 *got) {
    struct dev9p_priv *p = priv_of(spoor);
    if (!p) return 0;                                   // not dev9p -> byte-copy
    struct weft_binding *b = __atomic_load_n(&p->weft, __ATOMIC_ACQUIRE);
    if (!b) return 0;                                   // not weft-bound -> byte-copy
    if (!weft_should_ring(len)) return 0;               // below the hybrid threshold
    u32 off = 0;
    if (weft_binding_validate_rw(b, ubuf_va, len, &off) != 0)
        return 0;                                       // buffer not in the ring -> byte-copy
    u32 n = 0;
    int e = p9_client_weftio(p->client, p->fid, off, len, WEFT_DIR_READ, &n);
    if (e != 0) return -1;                              // weft attempted + failed (dead flow)
    if (got) *got = n;
    return 1;                                           // handled zero-copy
}

static long dev9p_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// Durability barrier -> Stratum Tsync (FS-mutation foundation; section 9.2).
static int dev9p_fsync(struct Spoor *c, u32 datasync) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
    // FID-LIFECYCLE cached-open: fsync on a read-only fd is a POSIX-legal
    // no-op success (nothing to flush; no fid to Tsync).
    if (p->cached_open) return 0;
    // Area-F errno rollout (the slot the #3 pass missed): propagate the real
    // ecode -- p9_client_fsync returns 0 or -(T_E_*)/-P9_E_IO, already a valid
    // -errno in [-4095,-1]. Collapsing to -1 masked the underlying failure
    // (the post-go-build fsync cascade read as a meaningless EPERM).
    return p9_client_fsync(p->client, p->fid, datasync);
}

// Directory enumeration -> Stratum Treaddir. Returns the raw 9P2000.L dirent
// byte stream into buf at the Spoor's offset; the caller advances `off` (the
// SYS_READDIR handler passes c->offset and bumps it). Mirrors dev9p_read's
// count/offset clamping.
static long dev9p_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
    if (p->cached_open) return -1;   // plain files only (type-gated at mint)
    if (n <= 0) return 0;
    u32 count = (n > 0x7fffffffL) ? 0x7fffffffu : (u32)n;
    // The Treaddir `offset` is an OPAQUE resume cookie, not a byte position:
    // Stratum derives it from an entry hash, so real dirents routinely exceed
    // INT64_MAX (bit 63 set). The Spoor's s64 `offset` carries the cookie
    // through the sign bit; reinterpret the bits straight back to u64. Do NOT
    // clamp a "negative" cookie to 0 -- that restarts enumeration and a
    // paginating reader (ls) re-fetches the first batch forever (#955). Byte
    // Devs (dev9p_read/write) keep their non-negative clamp; a dir cursor is
    // not a byte offset.
    u64 offset = (u64)off;
    u32 got = 0;
    int rc = p9_client_readdir(p->client, p->fid, offset, count, (u8 *)buf, &got);
    if (rc != 0) return -1;
    return (long)got;
}

// Rename -> Stratum Trenameat (FS-mutation foundation FS-gamma; section 9.3).
// olddir / newdir are the caller's looked-up directory Spoors (NOT clone-walked
// -- Trenameat operates on the dirfids by name without transitioning them, like
// Tsync / Treaddir). The SYS_RENAME handler already required the same Dev; this
// adds the same-SESSION guard (two dev9p mounts are distinct p9_clients, and a
// 9P renameat is within one session). Names are NUL-terminated by the handler.
static int dev9p_rename(struct Spoor *olddir, const char *oldname,
                        struct Spoor *newdir, const char *newname) {
    struct dev9p_priv *od = priv_of(olddir);
    struct dev9p_priv *nd = priv_of(newdir);
    if (!od || !nd) return -1;
    if (od->fid == P9_NOFID || nd->fid == P9_NOFID) return -1;   // fidless Spoor
    if (od->client != nd->client) return -1;     // renameat is within one session
    if (!oldname || !newname) return -1;
    size_t ol = 0; while (oldname[ol] != '\0') ol++;
    size_t nl = 0; while (newname[nl] != '\0') nl++;
    if (ol == 0 || nl == 0) return -1;
    int rc = p9_client_renameat(od->client, od->fid, (const u8 *)oldname, ol,
                                nd->fid, (const u8 *)newname, nl);
    if (rc != 0) return -1;
    // L1c invalidate (fs_cache.tla OwnWrite): both dirs' attrs (nlink/mtime/cvers)
    // changed. od->client == nd->client (checked above) -- one Larder.
    struct larder *l = &od->client->larder;
    larder_attr_invalidate(l, olddir->qid.path);
    larder_attr_invalidate(l, newdir->qid.path);
    // L1d invalidate: the rename removed oldname from olddir + added newname to
    // newdir -- drop BOTH dirs' cached dentries (a same-dir rename drops it once
    // effectively; the second scan is a benign no-op).
    larder_dentry_invalidate_parent(l, olddir->qid.path);
    larder_dentry_invalidate_parent(l, newdir->qid.path);
    return 0;
}

// Unlink -> Stratum Tunlinkat (FS-gamma; section 9.3). parent is the caller's
// looked-up directory Spoor. flags 0 = unlink a non-directory;
// P9_UNLINK_AT_REMOVEDIR (== SYS_UNLINK_REMOVEDIR, validated by the handler) =
// rmdir an empty directory. The flags arg is passed straight to the wire.
static int dev9p_unlink(struct Spoor *parent, const char *name, u32 flags) {
    struct dev9p_priv *p = priv_of(parent);
    if (!p) return -1;
    if (p->fid == P9_NOFID) return -1;   // fidless (cached-open) Spoor
    if (!name) return -1;
    size_t nl = 0; while (name[nl] != '\0') nl++;
    if (nl == 0) return -1;
    int rc = p9_client_unlinkat(p->client, p->fid, (const u8 *)name, nl, flags);
    if (rc != 0) return -1;
    // L1c invalidate (fs_cache.tla OwnWrite): unlink changed the parent dir's
    // attrs (nlink/mtime/cvers). The unlinked child's own cached entry (if any)
    // is left; an ino-reuse serve is caught by the gen guard + walk-overwrite,
    // and mode is unchanged by unlink (LARDER-DESIGN section 11).
    larder_attr_invalidate(&p->client->larder, parent->qid.path);
    // L1d invalidate: unlink removed a name from the parent's dirent set -- drop
    // the parent's cached dentries so a stale POSITIVE entry (parent, name) ->
    // the now-removed child cannot be served.
    larder_dentry_invalidate_parent(&p->client->larder, parent->qid.path);
    return 0;
}

// SYS_UNLINK passes its flags arg straight through dev9p_unlink to the wire, so
// the syscall ABI's REMOVEDIR bit MUST equal the wire's. Pinned here (the only
// TU that sees both).
_Static_assert(SYS_UNLINK_REMOVEDIR == P9_UNLINK_AT_REMOVEDIR,
               "SYS_UNLINK_REMOVEDIR must equal the 9P Tunlinkat flag");

static void dev9p_remove(struct Spoor *c) {
    (void)c;
    // The Plan 9 .remove slot (target-by-Spoor, void return) is the wrong shape
    // for SYS_UNLINK (parent + name, error-returning) -- SYS_UNLINK uses the new
    // .unlink slot (dev9p_unlink) instead. Left as a no-op stub.
}

static int dev9p_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    // The Plan 9 wire-stat .wstat slot is deferred; SYS_WSTAT (chmod/chown)
    // uses the native .wstat_native slot below (dev9p_wstat_native -> Tsetattr).
    return -1;
}

// Native chmod/chown surface (A-2a; IDENTITY-DESIGN.md §9.5) -> Stratum
// Tsetattr. The SYS_WSTAT handler has already validated the mask (>=1 T_WSTAT_*
// bit, no reserved bit) + value bounds (mode in 0777, uid/gid != INVALID); this
// maps the T_WSTAT_* mask onto P9_SETATTR_* (identical bit values, pinned below)
// and forwards. Like dev9p_rename / dev9p_unlink it borrows the caller's fid and
// allocates no transient fid, so the create-path fid-leak class cannot arise.
static int dev9p_wstat_native(struct Spoor *c, u32 valid, u32 mode,
                              u32 uid, u32 gid) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
    // FID-LIFECYCLE cached-open seam (section 3.3, documented + tested):
    // Tsetattr is fid-addressed and a fidless Spoor cannot late-bind (no
    // fid-from-qid op; a retained-path re-walk is rename-unsound), so
    // fchmod/fchown ON a cached-open fd fails LOUD. No v1.0 consumer does
    // this (cmd/go's cache mtime updates are path-based Chtimes); path-based
    // chmod/chown are untouched. The v1.x fix, if a consumer appears, is the
    // retain-the-walk-fid-unopened variant.
    if (p->cached_open) return -1;
    struct p9_setattr sa;
    for (size_t i = 0; i < sizeof(sa); i++) ((u8 *)&sa)[i] = 0;
    sa.valid = valid;        // T_WSTAT_* == P9_SETATTR_* (pinned below)
    sa.mode  = mode;
    sa.uid   = uid;
    sa.gid   = gid;
    int rc = p9_client_setattr(p->client, p->fid, &sa);
    if (rc != 0) return -1;
    // L1c invalidate (fs_cache.tla OwnWrite): chmod/chown/truncate changed
    // mode/uid/gid/size. CRITICAL -- the base X-check perm_checks the cached
    // mode, so a stale mode after a tighten would be a bounded I-28 window; the
    // invalidate keeps the window at zero for the guest's own chmod.
    larder_attr_invalidate(&p->client->larder, c->qid.path);
    return 0;
}

// SYS_WSTAT passes its valid mask straight through dev9p_wstat_native to the
// Tsetattr wire, so the syscall ABI's T_WSTAT_* bits MUST equal the wire's
// P9_SETATTR_* bits. Pinned here (the only TU that sees both).
_Static_assert(T_WSTAT_MODE == P9_SETATTR_MODE,
               "T_WSTAT_MODE must equal the 9P Tsetattr MODE bit");
_Static_assert(T_WSTAT_UID == P9_SETATTR_UID,
               "T_WSTAT_UID must equal the 9P Tsetattr UID bit");
_Static_assert(T_WSTAT_GID == P9_SETATTR_GID,
               "T_WSTAT_GID must equal the 9P Tsetattr GID bit");

static struct Spoor *dev9p_power(struct Spoor *c, int on) {
    (void)c; (void)on;
    return NULL;
}

// =============================================================================
// Initialization.
// =============================================================================

static bool g_dev9p_initialized = false;

void dev9p_init(void) {
    if (g_dev9p_initialized) return;
    dev_register(&dev9p);
    g_dev9p_initialized = true;
}

// =============================================================================
// Dev struct.
// =============================================================================

struct Dev dev9p = {
    .dc       = DEV9P_DC,
    .name     = "9p",
    // A-3b: rwx enforcement ACTIVE. The reconciliation A-2d deferred is in place
    // (IDENTITY-DESIGN.md 3.7.1 + 9.7): the host-bake stamps the pool
    // PRINCIPAL_SYSTEM-owned (Stratum --bake-owner-uid) and SO_PEERCRED carries
    // the connecting principal (the pouch shim), so dev9p_stat_native's
    // server-reported uid/gid is a Thylacine principal and perm_check is coherent
    // -- the boot chain (PRINCIPAL_SYSTEM = owner) is not denied. The A-2d audit
    // F1 (sys_walk_open_handler now derives handle rights from omode via
    // rights_for_omode) + F2 (sys_rename_handler / sys_unlink_handler now
    // perm_check(parent, PERM_W|PERM_X)) were closed in lockstep with this flip.
    .perm_enforced = true,

    .reset    = dev9p_reset,
    .init     = dev9p_init_noop,             // registration is via dev9p_init (outside the bestiary walk)
    .shutdown = dev9p_shutdown,

    .attach   = dev9p_attach_spec,
    .walk     = dev9p_walk,
    .walk_attrs = dev9p_walk_attrs,   // POUNCE: Twalkgetattr (the stalk fast path)
    .open_cached = dev9p_open_cached, // FID-LIFECYCLE: the fidless cached open
    .stat     = dev9p_stat,
    .stat_native = dev9p_stat_native,
    .seekable = true,   // file content: read/write honor the byte offset (RW-4 R2-F2)

    .open     = dev9p_open,
    .create   = dev9p_create,
    .close    = dev9p_close,

    .read     = dev9p_read,
    .bread    = dev9p_bread,
    .write    = dev9p_write,
    .bwrite   = dev9p_bwrite,
    .poll     = dev9p_poll,    // net-6b-2b: readiness bridge (QTPOLL files only)
    .fsync    = dev9p_fsync,
    .readdir  = dev9p_readdir,
    .rename   = dev9p_rename,
    .unlink   = dev9p_unlink,

    .remove   = dev9p_remove,
    .wstat    = dev9p_wstat,
    .wstat_native = dev9p_wstat_native,
    .power    = dev9p_power,
};
