// Kernel pipe — connected Spoor pair over a shared ring buffer (P5-pipe).
//
// Per ARCH §10.3 + docs/reference/51-pipe.md. The ring is a fixed-size
// PIPE_BUF_SIZE buffer (4 KiB; POSIX PIPE_BUF guarantee) with separate
// head / tail / count fields. Two Spoors point at the same ring via
// per-endpoint `struct pipe_endpoint` aux records; the Dev vtable
// dispatches based on each endpoint's `is_read_end` flag.
//
// v1.0 is single-CPU, non-blocking, and synchronous. No locking, no
// wait/wake. Concurrent multi-CPU access lands at P5-pipe-blocking;
// the rendez integration there will close ARCH §28 I-9 (missed-wakeup
// freedom) by composing with `specs/scheduler.tla`'s wait/wake state
// machine.

#include <thylacine/dev.h>
#include <thylacine/extinction.h>
#include <thylacine/pipe.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

// =============================================================================
// Internal types.
// =============================================================================

struct pipe_ring {
    u32     magic;          // PIPE_RING_MAGIC
    int     ref;            // 2 at creation; per-endpoint close drops by 1
    size_t  count;          // bytes in buffer; 0..PIPE_BUF_SIZE
    size_t  head;           // next write position; mod PIPE_BUF_SIZE
    size_t  tail;           // next read position; mod PIPE_BUF_SIZE
    u8      buf[PIPE_BUF_SIZE];
};

struct pipe_endpoint {
    u32                magic;       // PIPE_ENDPOINT_MAGIC
    struct pipe_ring  *ring;
    bool               is_read_end;
};

_Static_assert(sizeof(struct pipe_ring) == 32 + PIPE_BUF_SIZE,
               "pipe_ring size pinned (32-byte header + 4 KiB buf)");

// The ring is ~4 KiB — above the SLUB max-object threshold, so kmalloc
// routes it through alloc_pages (same path the p9_client uses). The
// endpoint is tiny (16 bytes); we use a SLUB cache for it to keep the
// per-pipe-create allocation footprint tight.
static struct kmem_cache *g_endpoint_cache;
static u64                g_pipe_allocated;
static u64                g_pipe_freed;
static bool               g_pipe_initialized;

// =============================================================================
// Ring buffer ops.
// =============================================================================

static long ring_write(struct pipe_ring *r, const u8 *buf, long len) {
    if (!r || r->magic != PIPE_RING_MAGIC) return -1;
    if (len <= 0) return 0;

    size_t avail = PIPE_BUF_SIZE - r->count;
    size_t to_write = ((size_t)len < avail) ? (size_t)len : avail;
    if (to_write == 0) return 0;

    // Two-segment copy: from head to end-of-buf, then wrap.
    size_t first = PIPE_BUF_SIZE - r->head;
    if (first > to_write) first = to_write;
    for (size_t i = 0; i < first; i++) {
        r->buf[r->head + i] = buf[i];
    }
    size_t second = to_write - first;
    for (size_t i = 0; i < second; i++) {
        r->buf[i] = buf[first + i];
    }

    r->head = (r->head + to_write) % PIPE_BUF_SIZE;
    r->count += to_write;
    return (long)to_write;
}

static long ring_read(struct pipe_ring *r, u8 *buf, long len) {
    if (!r || r->magic != PIPE_RING_MAGIC) return -1;
    if (len <= 0) return 0;

    size_t avail = r->count;
    size_t to_read = ((size_t)len < avail) ? (size_t)len : avail;
    if (to_read == 0) return 0;

    size_t first = PIPE_BUF_SIZE - r->tail;
    if (first > to_read) first = to_read;
    for (size_t i = 0; i < first; i++) {
        buf[i] = r->buf[r->tail + i];
    }
    size_t second = to_read - first;
    for (size_t i = 0; i < second; i++) {
        buf[first + i] = r->buf[i];
    }

    r->tail = (r->tail + to_read) % PIPE_BUF_SIZE;
    r->count -= to_read;
    return (long)to_read;
}

// =============================================================================
// Dev vtable.
// =============================================================================

static struct pipe_endpoint *priv_of(struct Spoor *c) {
    if (!c) return NULL;
    if (!c->aux) return NULL;
    struct pipe_endpoint *p = (struct pipe_endpoint *)c->aux;
    if (p->magic != PIPE_ENDPOINT_MAGIC) {
        extinction("pipe: corrupted endpoint magic (use-after-free?)");
    }
    return p;
}

static void devpipe_reset(void)    { /* no-op */ }
static void devpipe_shutdown(void) { /* no-op */ }
static void devpipe_init_noop(void) { /* registration via pipe_init */ }

static struct Spoor *devpipe_attach(const char *spec) {
    (void)spec;
    // attach via spec is not how pipes are constructed; tests + kernel
    // callers use pipe_create() directly. Plan 9's `/srv` posting story
    // (where a server creates a named pipe and clients walk to it) is
    // Phase 5+ once the syscall surface lands.
    return NULL;
}

static struct Walkqid *devpipe_walk(struct Spoor *c, struct Spoor *nc,
                                    const char **name, int nname) {
    (void)c; (void)nc; (void)name; (void)nname;
    return NULL;
}

static int devpipe_stat(struct Spoor *c, u8 *dp, int n) {
    (void)c; (void)dp; (void)n;
    return -1;
}

static struct Spoor *devpipe_open(struct Spoor *c, int omode) {
    if (!c) return NULL;
    c->flag |= 0x01;            // COPEN
    c->mode  = omode;
    return c;
}

static void devpipe_create(struct Spoor *c, const char *name, int omode, u32 perm) {
    (void)c; (void)name; (void)omode; (void)perm;
}

static void devpipe_close(struct Spoor *c) {
    struct pipe_endpoint *p = priv_of(c);
    if (!p) return;
    struct pipe_ring *r = p->ring;
    if (!r || r->magic != PIPE_RING_MAGIC) {
        extinction("pipe: close on endpoint with corrupted ring");
    }
    // Drop this endpoint's ring ref. When both endpoints have been
    // closed, the ring is freed. ref is signed int — defensive
    // underflow check.
    r->ref--;
    if (r->ref < 0) {
        extinction("pipe: ring refcount underflow");
    }
    if (r->ref == 0) {
        r->magic = 0;       // UAF defense — readers see magic clobber
        kfree(r);
        __atomic_fetch_add(&g_pipe_freed, 1u, __ATOMIC_RELAXED);
    }
    // Free the endpoint priv. The Spoor's aux is now dangling — caller
    // must not dereference c after spoor_clunk returns, which spoor.c
    // documents as the contract.
    p->magic = 0;
    kmem_cache_free(g_endpoint_cache, p);
    c->aux = NULL;
}

static long devpipe_read(struct Spoor *c, void *buf, long n, s64 off) {
    (void)off;
    struct pipe_endpoint *p = priv_of(c);
    if (!p)                      return -1;
    if (!p->is_read_end)         return -1;     // wrong end
    if (n < 0 || !buf)           return -1;
    if (!p->ring)                return -1;
    return ring_read(p->ring, (u8 *)buf, n);
}

static long devpipe_write(struct Spoor *c, const void *buf, long n, s64 off) {
    (void)off;
    struct pipe_endpoint *p = priv_of(c);
    if (!p)                      return -1;
    if (p->is_read_end)          return -1;     // wrong end
    if (n < 0 || !buf)           return -1;
    if (!p->ring)                return -1;
    return ring_write(p->ring, (const u8 *)buf, n);
}

static struct Block *devpipe_bread(struct Spoor *c, long n, s64 off) {
    (void)c; (void)n; (void)off;
    return NULL;
}

static long devpipe_bwrite(struct Spoor *c, struct Block *bp, s64 off) {
    (void)c; (void)bp; (void)off;
    return -1;
}

static void devpipe_remove(struct Spoor *c) { (void)c; }
static int  devpipe_wstat(struct Spoor *c, u8 *dp, int n)
                                            { (void)c; (void)dp; (void)n; return -1; }
static struct Spoor *devpipe_power(struct Spoor *c, int on)
                                            { (void)c; (void)on; return NULL; }

struct Dev devpipe = {
    .dc       = DEVPIPE_DC,
    .name     = "pipe",
    .reset    = devpipe_reset,
    .init     = devpipe_init_noop,
    .shutdown = devpipe_shutdown,
    .attach   = devpipe_attach,
    .walk     = devpipe_walk,
    .stat     = devpipe_stat,
    .open     = devpipe_open,
    .create   = devpipe_create,
    .close    = devpipe_close,
    .read     = devpipe_read,
    .bread    = devpipe_bread,
    .write    = devpipe_write,
    .bwrite   = devpipe_bwrite,
    .remove   = devpipe_remove,
    .wstat    = devpipe_wstat,
    .power    = devpipe_power,
};

// =============================================================================
// Bring-up.
// =============================================================================

void pipe_init(void) {
    if (g_pipe_initialized) return;

    // Ring is allocated via kmalloc (routes through alloc_pages for the
    // ~4 KiB size). Only the endpoint gets a SLUB cache.
    g_endpoint_cache = kmem_cache_create("pipe_endpoint",
                                         sizeof(struct pipe_endpoint),
                                         8,
                                         0);
    if (!g_endpoint_cache) {
        extinction("kmem_cache_create(pipe_endpoint) returned NULL");
    }
    dev_register(&devpipe);
    g_pipe_initialized = true;
}

// =============================================================================
// pipe_create — the one constructor.
// =============================================================================

int pipe_create(struct Spoor **out_read_end, struct Spoor **out_write_end) {
    if (!g_pipe_initialized) extinction("pipe_create before pipe_init");
    if (!out_read_end || !out_write_end) return -1;
    *out_read_end  = NULL;
    *out_write_end = NULL;

    struct pipe_ring *r = kmalloc(sizeof(*r), KP_ZERO);
    if (!r) return -1;
    r->magic = PIPE_RING_MAGIC;
    r->ref   = 2;
    r->count = 0;
    r->head  = 0;
    r->tail  = 0;
    // buf[] already zero from KP_ZERO.

    struct pipe_endpoint *rd_priv = kmem_cache_alloc(g_endpoint_cache, KP_ZERO);
    if (!rd_priv) {
        r->magic = 0;
        kfree(r);
        return -1;
    }
    rd_priv->magic       = PIPE_ENDPOINT_MAGIC;
    rd_priv->ring        = r;
    rd_priv->is_read_end = true;

    struct pipe_endpoint *wr_priv = kmem_cache_alloc(g_endpoint_cache, KP_ZERO);
    if (!wr_priv) {
        rd_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, rd_priv);
        r->magic = 0;
        kfree(r);
        return -1;
    }
    wr_priv->magic       = PIPE_ENDPOINT_MAGIC;
    wr_priv->ring        = r;
    wr_priv->is_read_end = false;

    struct Spoor *rd = spoor_alloc(&devpipe);
    if (!rd) {
        wr_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, wr_priv);
        rd_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, rd_priv);
        r->magic = 0;
        kfree(r);
        return -1;
    }
    struct Spoor *wr = spoor_alloc(&devpipe);
    if (!wr) {
        // Rolling back the read-end Spoor: spoor_clunk would call
        // devpipe_close → drop ring ref → potentially free the ring.
        // But we still own the write-end priv that points at the ring.
        // Take the path that frees both pieces of state manually then
        // unrefs the Spoor without calling its close hook.
        rd->aux = NULL;                      // detach priv before close fires
        spoor_clunk(rd);                     // close sees NULL aux → no-op via priv_of
        wr_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, wr_priv);
        rd_priv->magic = 0;
        kmem_cache_free(g_endpoint_cache, rd_priv);
        r->magic = 0;
        kfree(r);
        return -1;
    }

    rd->aux = rd_priv;
    wr->aux = wr_priv;
    rd->qid.type = 0;                        // QTFILE
    wr->qid.type = 0;

    __atomic_fetch_add(&g_pipe_allocated, 1u, __ATOMIC_RELAXED);
    *out_read_end  = rd;
    *out_write_end = wr;
    return 0;
}

u64 pipe_total_allocated(void) {
    return __atomic_load_n(&g_pipe_allocated, __ATOMIC_RELAXED);
}

u64 pipe_total_freed(void) {
    return __atomic_load_n(&g_pipe_freed, __ATOMIC_RELAXED);
}
