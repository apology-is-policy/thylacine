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

// Native fstat surface (A-2a; IDENTITY-DESIGN.md §9.5) -> Stratum Tgetattr.
// Fills *out from the server's Rgetattr. uid/gid are the server-reported owner
// + group; for a per-user stratumd they are the connection's principal (A-3
// completes per-user attribution). Unlike the .stat slot (the Plan 9 wire-stat,
// still deferred), this is the metadata source the kernel rwx layer (A-2d) and
// SYS_FSTAT consume. P9_GETATTR_BASIC covers mode/uid/gid/size/times/nlink.
static int dev9p_stat_native(struct Spoor *c, struct t_stat *out) {
    struct dev9p_priv *p = priv_of(c);
    if (!p || !out) return -1;
    struct p9_attr attr;
    if (p9_client_getattr(p->client, p->fid, P9_GETATTR_BASIC, &attr) != 0)
        return -1;
    for (size_t i = 0; i < sizeof(*out); i++) ((u8 *)out)[i] = 0;
    out->size      = attr.size;
    out->qid_path  = attr.qid.path;
    out->atime_sec = attr.atime_sec;
    out->mtime_sec = attr.mtime_sec;
    out->ctime_sec = attr.ctime_sec;
    out->nlink     = (u32)attr.nlink;
    out->qid_vers  = attr.qid.version;
    out->qid_type  = qid_type_p9_to_kernel(attr.qid.type);
    out->blksize   = attr.blksize ? (u32)attr.blksize : 4096u;
    out->blocks    = attr.blocks;
    // A-2a F2 (closed in A-2d): respect the server's `valid` mask for the
    // security-critical trio. A server that did not fill mode/uid/gid must NOT
    // have us report stale wire bytes -- leaving the pre-zeroed field is
    // fail-closed (mode 0 = no rwx bits; uid 0 = PRINCIPAL_INVALID, gid 0 =
    // GID_INVALID -- a real principal matches none, so perm_check denies). v1.0
    // Stratum fills BASIC, so this is dormant for the reference server; it makes
    // the A-3 dev9p enforcement (which reads these) sound against any server.
    if (attr.valid & P9_GETATTR_MODE) out->mode = attr.mode;
    if (attr.valid & P9_GETATTR_UID)  out->uid  = attr.uid;
    if (attr.valid & P9_GETATTR_GID)  out->gid  = attr.gid;
    return 0;
}

static struct Spoor *dev9p_open(struct Spoor *c, int omode) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return NULL;
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
    if (!name) return NULL;

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
    return c;
}

static void dev9p_close(struct Spoor *c) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return;

    // net-6b-2b: free the readiness poll-state (if this was a netd `ready` file).
    // Safe here: dev9p_close runs only at the Spoor's LAST ref, and an outstanding
    // readiness op pins the Spoor (a ref) while a registered poller holds the
    // handle's obj ref -- so at close there is neither a live op nor a poller.
    dev9p_poll_priv_release(p);

    // Weft-6a-2: release the per-flow ring binding (if this data fd went
    // zero-copy). Drops the I-30 registration pin -> the #847 dual count frees
    // the ring Burrow once the guest's mapping also drops (vma_drain at guest
    // exit). LAST-ref runs here, so no concurrent SYS_WEFT_MAP can race the
    // read: a mapper needs a live handle, and the last ref means none remains.
    if (p->weft) {
        weft_binding_release(p->weft);
        p->weft = NULL;
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
        // Walk-derived Spoor: clunk the fid. Ignore the result —
        // close-then-error has no good recovery; the fid is gone
        // from the client's table either way per the wire spec.
        (void)p9_client_clunk(p->client, p->fid);
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
    u32 got = 0;
    int rc = p9_client_read(p->client, p->fid, offset, count, (u8 *)buf, &got);
    if (rc != 0) return -1;
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
    if (n <= 0) return 0;
    u32 count = (n > 0x7fffffffL) ? 0x7fffffffu : (u32)n;
    u64 offset = (off < 0) ? 0 : (u64)off;
    u32 accepted = 0;
    int rc = p9_client_write(p->client, p->fid, offset, count,
                              (const u8 *)buf, &accepted);
    if (rc != 0) return -1;
    return (long)accepted;
}

static long dev9p_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

// Durability barrier -> Stratum Tsync (FS-mutation foundation; section 9.2).
static int dev9p_fsync(struct Spoor *c, u32 datasync) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
    return p9_client_fsync(p->client, p->fid, datasync) == 0 ? 0 : -1;
}

// Directory enumeration -> Stratum Treaddir. Returns the raw 9P2000.L dirent
// byte stream into buf at the Spoor's offset; the caller advances `off` (the
// SYS_READDIR handler passes c->offset and bumps it). Mirrors dev9p_read's
// count/offset clamping.
static long dev9p_readdir(struct Spoor *c, void *buf, long n, s64 off) {
    struct dev9p_priv *p = priv_of(c);
    if (!p) return -1;
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
    if (od->client != nd->client) return -1;     // renameat is within one session
    if (!oldname || !newname) return -1;
    size_t ol = 0; while (oldname[ol] != '\0') ol++;
    size_t nl = 0; while (newname[nl] != '\0') nl++;
    if (ol == 0 || nl == 0) return -1;
    return p9_client_renameat(od->client, od->fid, (const u8 *)oldname, ol,
                               nd->fid, (const u8 *)newname, nl) == 0 ? 0 : -1;
}

// Unlink -> Stratum Tunlinkat (FS-gamma; section 9.3). parent is the caller's
// looked-up directory Spoor. flags 0 = unlink a non-directory;
// P9_UNLINK_AT_REMOVEDIR (== SYS_UNLINK_REMOVEDIR, validated by the handler) =
// rmdir an empty directory. The flags arg is passed straight to the wire.
static int dev9p_unlink(struct Spoor *parent, const char *name, u32 flags) {
    struct dev9p_priv *p = priv_of(parent);
    if (!p) return -1;
    if (!name) return -1;
    size_t nl = 0; while (name[nl] != '\0') nl++;
    if (nl == 0) return -1;
    return p9_client_unlinkat(p->client, p->fid, (const u8 *)name, nl, flags)
               == 0 ? 0 : -1;
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
    struct p9_setattr sa;
    for (size_t i = 0; i < sizeof(sa); i++) ((u8 *)&sa)[i] = 0;
    sa.valid = valid;        // T_WSTAT_* == P9_SETATTR_* (pinned below)
    sa.mode  = mode;
    sa.uid   = uid;
    sa.gid   = gid;
    return p9_client_setattr(p->client, p->fid, &sa) == 0 ? 0 : -1;
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
