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
static u32                g_spoor_devno_ctr;   // monotonic; spoor_next_devno()

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
    c->devno = 0;            // static single-instance Dev default; multi-instance
                             // Devs (dev9p) overwrite with spoor_next_devno() at
                             // attach. The clone path copies it (a walked child
                             // inherits its session's devno).
    c->dev   = d;
    spin_lock_init(&c->lock);
    // R15 F233 close: ref is updated atomically. Init is safe with a
    // relaxed store — the Spoor isn't published to any other CPU until
    // we return + the caller stores the pointer somewhere observable.
    __atomic_store_n(&c->ref, 1, __ATOMIC_RELAXED);
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
    // R15 F233 close: atomic load. By contract this is only called on
    // a Spoor whose last drop already brought ref to 0; the load is
    // for diagnostic detection of a premature-free bug.
    if (__atomic_load_n(&c->ref, __ATOMIC_ACQUIRE) != 0)
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

    // R15 F233 close: atomic increment. fetch_add returns the PRE
    // value; pre == 0 means the Spoor was already last-dropped (a
    // use-after-free in the caller — extinct). The increment has
    // already happened by the time we extinct, but that's fine since
    // extinction halts.
    int pre = __atomic_fetch_add(&c->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)
        extinction("spoor_ref of zero-ref Spoor (already freed?)");
}

// Pure refcount drop. If the ref hits 0 the Spoor's storage is freed
// but `dev->close` is NOT invoked — use this entry point only when the
// caller knows the per-Spoor Dev state has already been torn down, OR
// when the Spoor was never "opened" (e.g., spoor_alloc + early
// rollback before any device-side state was wired up). The general
// "release my hold on this Spoor" entry point is spoor_clunk, which
// runs the Dev close hook on last drop.
void spoor_unref(struct Spoor *c) {
    if (!c) return;                                  // NULL-safe (mirrors burrow_unref)
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_unref of corrupted Spoor (use-after-free?)");

    // R15 F233 close: atomic decrement. fetch_sub returns the PRE
    // value; pre == 1 means we were the last holder (post == 0) and
    // own the free. pre <= 0 is the use-after-free diagnostic case.
    int pre = __atomic_fetch_sub(&c->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)
        extinction("spoor_unref of zero-ref Spoor");
    if (pre == 1) {
        spoor_free_internal(c);
    }
}

struct Spoor *spoor_clone(struct Spoor *c) {
    if (!c)                       return NULL;
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_clone of corrupted Spoor");
    // R15 F233 close: atomic load. This is diagnostic only (clone
    // doesn't mutate the source ref); the caller is responsible for
    // keeping a hold on c across this call.
    if (__atomic_load_n(&c->ref, __ATOMIC_ACQUIRE) <= 0)
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
    // devno is the per-instance device identity (Plan 9 Chan.dev). A walk /
    // clone of a Spoor stays within the SAME device instance (a dev9p walk
    // stays in the session; a cross-mount clone stays in the source tree), so
    // the clone inherits it. Together with dc + qid.path this is the mount-key.
    nc->devno  = c->devno;

    return nc;
}

// spoor_next_devno -- mint a fresh per-instance device number (Plan 9 Chan.dev)
// for a multi-instance Dev's attach. Monotonic from 1 (0 is the static
// single-instance default set in spoor_alloc_internal). The wrap after 2^32
// attaches is benign: the mount key is per-Territory, not a security boundary,
// and a collision would require two LIVE same-devno sessions in one Territory's
// mount table -- astronomically unlikely and non-exploitable (it is identity
// disambiguation, not a capability).
u32 spoor_next_devno(void) {
    return __atomic_add_fetch(&g_spoor_devno_ctr, 1u, __ATOMIC_RELAXED);
}

void spoor_clunk(struct Spoor *c) {
    if (!c) return;
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_clunk of corrupted Spoor (use-after-free?)");

    // Plan 9 cclose semantics: drop a ref; the Dev's close hook runs
    // ONLY on the last drop (ref hits 0). This is what ARCH §9.6.6's
    // "Caller can close the attach_9p fd after mount — the mount
    // table holds the ref" lifecycle requires: extra refs (dup'd
    // handles, mount-table entries) keep the underlying Dev state
    // alive even after some holders clunk. P5-mount-syscall exposed
    // that the prior "run dev->close on every clunk" design tore
    // down Dev state too eagerly — closing one of two handle holders
    // freed the pipe endpoint while the other holder still observed
    // an alive Spoor with a dangling aux. Now: dev->close is invoked
    // strictly when the last holder relinquishes.
    //
    // Dev close hooks need NOT be idempotent under this contract;
    // they may assume one-shot semantics + that no other clunk will
    // call them again on the same Spoor.
    //
    // R15 F233 close: atomic decrement under ACQ_REL ordering. The
    // PRE value determines whether this CPU owns the last-drop close;
    // exactly one CPU sees pre == 1 even under concurrent spoor_clunk
    // from two CPUs. The dev->close runs AFTER the decrement (so
    // post == 0); the storage is still valid (no spoor_free_internal
    // yet), the hook can safely inspect c->aux, c->dev, c->qid.
    int pre = __atomic_fetch_sub(&c->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)
        extinction("spoor_clunk of zero-ref Spoor");
    if (pre == 1) {
        // Last drop. Run Dev close hook (releases per-Spoor aux),
        // then free. The close hook sees ref=0 but storage is intact.
        if (c->dev && c->dev->close) {
            c->dev->close(c);
        }
        spoor_free_internal(c);
    }
}

u64 spoor_total_allocated(void) { return g_spoor_allocated; }
u64 spoor_total_freed(void)     { return g_spoor_freed; }

// =============================================================================
// Walkqid allocation (P4-C).
// =============================================================================
//
// kmalloc-backed; the Walkqid is per-call temporary that the walk caller
// owns until done. walkqid_alloc(0) returns a Walkqid with a 1-slot
// tail to avoid zero-sized-allocation edge cases (the caller decides
// nqid; an unused slot is fine).

struct Walkqid *walkqid_alloc(int max_qids) {
    if (max_qids < 0) return NULL;
    int slots = max_qids > 0 ? max_qids : 1;
    size_t bytes = sizeof(struct Walkqid) + (size_t)slots * sizeof(struct Qid);
    struct Walkqid *w = kmalloc(bytes, KP_ZERO);
    if (!w) return NULL;
    w->spoor = NULL;
    w->nqid  = 0;
    return w;
}

void walkqid_free(struct Walkqid *w) {
    if (!w) return;
    kfree(w);
}
