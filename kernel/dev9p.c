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
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
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
    // Root is always a directory; the qid path/vers come from the
    // server but at v1.0 we don't carry them at the Spoor layer (the
    // qid is meaningful per-walk via the Dev vtable). The cached qid
    // mostly matters when consumers stat the root; left as a placeholder.
    c->qid.type = QTDIR;
    c->qid.path = 0;
    c->qid.vers = 0;
    return c;
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

static void dev9p_remove(struct Spoor *c) {
    (void)c;
    // Remove maps to Tunlinkat on the parent; deferred to syscall chunk.
}

static int dev9p_wstat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    // wstat maps to Tsetattr; deferred.
    return -1;
}

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

    .reset    = dev9p_reset,
    .init     = dev9p_init_noop,             // registration is via dev9p_init (outside the bestiary walk)
    .shutdown = dev9p_shutdown,

    .attach   = dev9p_attach_spec,
    .walk     = dev9p_walk,
    .stat     = dev9p_stat,

    .open     = dev9p_open,
    .create   = dev9p_create,
    .close    = dev9p_close,

    .read     = dev9p_read,
    .bread    = dev9p_bread,
    .write    = dev9p_write,
    .bwrite   = dev9p_bwrite,

    .remove   = dev9p_remove,
    .wstat    = dev9p_wstat,
    .power    = dev9p_power,
};
