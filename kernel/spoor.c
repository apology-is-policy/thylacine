// Spoor lifecycle — alloc / ref / unref / clone / clunk (P4-A).
//
// Per ARCHITECTURE.md §9 + handoff 024. Mirrors burrow.c's magic +
// counter discipline: SPOOR_MAGIC at offset 0 catches use-after-free;
// cumulative counters let tests verify lifecycle without dereferencing
// freed pointers.

#include <thylacine/dev.h>
#include <thylacine/extinction.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_spoor_cache;
static u64                g_spoor_allocated;
static u64                g_spoor_freed;

void spoor_init(void) {
    if (g_spoor_cache) extinction("spoor_init called twice");

    // Spoors are reachable from userspace via the syscall surface
    // (Phase 5+) and via 9P-mounted dev paths; userspace OOM must NOT
    // extinct the kernel. Cache flag chosen for matching contract:
    // spoor_alloc returns NULL on cache OOM.
    g_spoor_cache = kmem_cache_create("spoor",
                                      sizeof(struct Spoor),
                                      8,
                                      0);
    if (!g_spoor_cache) {
        extinction("kmem_cache_create(spoor) returned NULL");
    }
}

// Internal: SLUB-allocate + initialize fields common to spoor_alloc and
// spoor_clone. dc / dev fields are set by the caller (clone copies from
// the source, alloc takes them from the Dev arg).
static struct Spoor *spoor_alloc_internal(struct Dev *d) {
    if (!g_spoor_cache) extinction("spoor_alloc before spoor_init");
    if (!d) return NULL;

    struct Spoor *c = kmem_cache_alloc(g_spoor_cache, KP_ZERO);
    if (!c) return NULL;

    c->magic = SPOOR_MAGIC;
    c->dc    = d->dc;
    c->dev   = d;
    spin_lock_init(&c->lock);
    c->ref    = 1;
    c->flag   = 0;
    c->mode   = 0;
    c->offset = 0;
    c->aux    = NULL;
    // qid is left zeroed (KP_ZERO already cleared it); the dev's
    // attach/walk hooks populate it as appropriate.

    g_spoor_allocated++;
    return c;
}

struct Spoor *spoor_alloc(struct Dev *d) {
    return spoor_alloc_internal(d);
}

static void spoor_free_internal(struct Spoor *c) {
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_free_internal of corrupted Spoor");
    if (c->ref != 0)
        extinction("spoor_free_internal with ref > 0 (premature free)");

    // Clobber magic explicitly so a stale-pointer dereference between
    // free and SLUB-list-write extincts on the magic check rather than
    // returning plausible-looking stale fields.
    c->magic = 0;

    kmem_cache_free(g_spoor_cache, c);
    g_spoor_freed++;
}

void spoor_ref(struct Spoor *c) {
    if (!c)                       extinction("spoor_ref(NULL)");
    if (c->magic != SPOOR_MAGIC)  extinction("spoor_ref of corrupted Spoor");
    if (c->ref <= 0)
        extinction("spoor_ref of zero-ref Spoor (already freed?)");

    c->ref++;
}

void spoor_unref(struct Spoor *c) {
    if (!c) return;                                  // NULL-safe (mirrors burrow_unref)
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_unref of corrupted Spoor (use-after-free?)");
    if (c->ref <= 0)
        extinction("spoor_unref of zero-ref Spoor");

    c->ref--;
    if (c->ref == 0) {
        spoor_free_internal(c);
    }
}

struct Spoor *spoor_clone(struct Spoor *c) {
    if (!c)                       return NULL;
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_clone of corrupted Spoor");
    if (c->ref <= 0)
        extinction("spoor_clone of zero-ref Spoor (already freed?)");

    struct Spoor *nc = spoor_alloc_internal(c->dev);
    if (!nc) return NULL;

    // Copy walked-state fields. Each field has a deliberate semantic:
    //   - qid: the position the new Spoor inherits (walks update this
    //     in-place on the new Spoor afterwards).
    //   - flag / mode: pre-open flags carry over so a walk of a CMSG
    //     parent inherits message-style semantics.
    //   - offset: 0 by default; cloning a positioned Spoor and the
    //     caller wanting the cursor preserved sets it explicitly. We
    //     copy here to match Plan 9 cclone behavior; tests verify.
    //   - aux: shallow-copied. Devs whose aux owns refcounted state
    //     MUST take their own ref in dev->walk before populating the
    //     new Spoor's aux; spoor_clone does not interpret aux.
    nc->qid    = c->qid;
    nc->flag   = c->flag;
    nc->mode   = c->mode;
    nc->offset = c->offset;
    nc->aux    = c->aux;

    return nc;
}

void spoor_clunk(struct Spoor *c) {
    if (!c) return;
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_clunk of corrupted Spoor (use-after-free?)");
    if (c->ref <= 0)
        extinction("spoor_clunk of zero-ref Spoor");

    // Drive dev->close before dropping the ref. The dev's close hook
    // may release per-Spoor aux state that lives outside the Spoor
    // struct (e.g., a 9P fid under Phase 4's userspace-driver path).
    // close hooks are responsible for being idempotent + safe-on-not-
    // yet-opened (devnone's close is a no-op; the cons / null / proc
    // hooks gate work on (c->flag & COPEN)).
    if (c->dev && c->dev->close) {
        c->dev->close(c);
    }

    spoor_unref(c);     // may invalidate `c`
}

u64 spoor_total_allocated(void) { return g_spoor_allocated; }
u64 spoor_total_freed(void)     { return g_spoor_freed; }
