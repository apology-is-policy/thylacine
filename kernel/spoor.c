// Spoor lifecycle — alloc / ref / unref / clone / clunk (P4-A).
//
// Per ARCHITECTURE.md §9 + handoff 024. Mirrors burrow.c's magic +
// counter discipline: SPOOR_MAGIC at offset 0 catches use-after-free;
// cumulative counters let tests verify lifecycle without dereferencing
// freed pointers.

#include <thylacine/dev.h>
#include <thylacine/extinction.h>
#include <thylacine/path.h>     // #66: the per-Spoor namespace-name Path (I-33)
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/types.h>
#include <atomic_lse.h>   // t_atomic_fetch_{add,sub}_acqrel_int (W1.5 LSE-patchable refcount)

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
    c->union_snap = NULL;    // UM: set only by a STALK_OPEN of a union point.
    // qid is left zeroed (KP_ZERO already cleared it); the dev's
    // attach/walk hooks populate it as appropriate.

    g_spoor_allocated++;
    return c;
}

struct Spoor *spoor_alloc(struct Dev *d) {
    return spoor_alloc_internal(d);
}

// UM (union mounts): clunk every opened member of a union fd's snapshot + kfree
// the array. NULL-safe. Each m[i] is a distinct opened member Spoor (never this
// fd itself -- the fd is member[0]'s cross, held separately), so this is not a
// self-clunk. spoor_clunk is declared in spoor.h; a member clunk's Dev close
// hook may sleep, so callers must not hold a spinlock (spoor_free_internal and
// the STALK_OPEN union cleanup both satisfy this).
void union_snap_free(struct union_snap *snap) {
    if (!snap) return;
    for (int i = 0; i < snap->n; i++) {
        if (snap->m[i].opened)   spoor_clunk(snap->m[i].opened);    // readdir fid
        if (snap->m[i].walkable) spoor_clunk(snap->m[i].walkable);  // R2-F1 dedup fid
    }
    if (snap->point) spoor_clunk(snap->point);   // UM-8c (F5): the retained point
    kfree(snap);
}

static void spoor_free_internal(struct Spoor *c) {
    if (c->magic != SPOOR_MAGIC)
        extinction("spoor_free_internal of corrupted Spoor");
    // R15 F233 close: atomic load. By contract this is only called on
    // a Spoor whose last drop already brought ref to 0; the load is
    // for diagnostic detection of a premature-free bug.
    if (__atomic_load_n(&c->ref, __ATOMIC_ACQUIRE) != 0)
        extinction("spoor_free_internal with ref > 0 (premature free)");

    // #66: drop this Spoor's hold on its namespace-name Path. A Path frees
    // exactly when its last referencing Spoor frees (lifetime subset of the
    // Spoor's, I-33); path_unref is NULL-safe.
    path_unref(c->path);
    c->path = NULL;

    // UM: free the union member snapshot a STALK_OPEN of a union directory
    // attached here (Chan.umh). Each entry is a DISTINCT opened member Spoor
    // (this Spoor is member[0]'s cross; the snapshot holds every member OREAD),
    // so clunking them is not a self-clunk; the array itself is kfree'd. A member
    // clunk's Dev close hook may sleep -- spoor_free_internal is never called
    // under a spinlock (spoor_clunk is the release seam), so this is safe here.
    union_snap_free(c->union_snap);
    c->union_snap = NULL;

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
    int pre = t_atomic_fetch_add_acqrel_int(&c->ref, 1);
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
    int pre = t_atomic_fetch_sub_acqrel_int(&c->ref, 1);
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
    //     parent inherits message-style semantics. EXCEPT CWALKONLY (#81): it
    //     is a per-final-handle "navigation-only, no byte I/O" marker, set
    //     EXPLICITLY at the two T_OPATH handle-creation sites -- it must NOT be
    //     inherited, or a child CREATED or normally-opened from a T_OPATH parent
    //     (the FS-delta create-from-O_PATH base pattern) would inherit it and
    //     reject its own legitimate read/write.
    //   - offset: 0 by default; cloning a positioned Spoor and the
    //     caller wanting the cursor preserved sets it explicitly. We
    //     copy here to match Plan 9 cclone behavior; tests verify.
    //   - aux: shallow-copied. Devs whose aux owns refcounted state
    //     MUST take their own ref in dev->walk before populating the
    //     new Spoor's aux; spoor_clone does not interpret aux.
    //   - union_snap (UM): deliberately NOT inherited -- spoor_alloc_internal
    //     left it NULL. A clone is a walk position (an intermediate cross, a
    //     dirfd re-clone), never a union open; only the STALK_OPEN final cross
    //     attaches it. Inheriting it would double-free the opened members and
    //     misroute readdir of a plain child through the parent's union.
    nc->qid    = c->qid;
    nc->flag   = c->flag & ~CWALKONLY;   // #81: never inherit the nav-only marker
    nc->mode   = c->mode;
    nc->offset = c->offset;
    nc->aux    = c->aux;
    // devno is the per-instance device identity (Plan 9 Chan.dev). A walk /
    // clone of a Spoor stays within the SAME device instance (a dev9p walk
    // stays in the session; a cross-mount clone stays in the source tree), so
    // the clone inherits it. Together with dc + qid.path this is the mount-key.
    nc->devno  = c->devno;

    // #66: SHARE the parent's namespace-name Path (Plan 9 cclone -- O(1) incref,
    // copies no string; the hot walk path runs this on every hop, including the
    // ones that fail + unwind). A successful resolution step then REPLACES this
    // shared Path with an extended private one via spoor_path_extend; a mount
    // cross replaces it via spoor_path_transplant; a clone that is neither (the
    // "/" quarry, the clone_walk_zero cross source) inherits the parent's name.
    // path_ref is NULL-safe (a NULL parent Path == "unknown" stays unknown).
    nc->path = c->path;
    path_ref(nc->path);

    return nc;
}

// spoor_next_devno -- mint a fresh per-instance device number (Plan 9 Chan.dev)
// for a multi-instance Dev's attach. Monotonic from 1 (0 is the static
// single-instance default set in spoor_alloc_internal).
//
// #217 CONSULTS devno IN A SECURITY DECISION (mount_noexec_covers keys MNOEXEC
// on the (dc, devno) a file shares with its mount source), so the old wording
// here -- "not a security boundary ... identity disambiguation, not a
// capability" -- is retired. It was exactly the kind of stale reassurance that
// tells a future reader not to look.
//
// For the MNOEXEC key the wrap stays benign, but for a DIFFERENT reason than
// before: a collision can only ever ADD coverage (a queried file matches some
// other MNOEXEC entry's source and is refused), never remove it, so it is
// fail-closed over-restriction rather than a bypass. A Proc cannot choose or
// influence its devno either -- the counter is kernel-internal and monotonic.
//
// SCOPED DELIBERATELY to that consumer, because devno has a SECOND one: the
// REVENANT Image cache keys on (dc, devno, qid.path), where a collision is NOT
// over-restriction but an I-1 cache alias (two live Envs sharing a devno could
// serve one Proc the other's bytes on a non-exec file map). That is
// pre-existing, shared by every Dev, and needs 2^32 attaches in one boot to
// reach -- but an unqualified "the wrap is benign" would be the same
// over-broad reassurance this comment was rewritten to retire.
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
    int pre = t_atomic_fetch_sub_acqrel_int(&c->ref, 1);
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
// Namespace name retention (#66 -- the Plan 9 Chan.path; I-33).
// =============================================================================

void spoor_path_extend(struct Spoor *c, const char *name, u64 namelen) {
    if (!c) return;
    // `c->path` is the parent's Path (shared via spoor_clone). "." -- a no-op
    // step (same place + name): keep it. Nothing to do.
    if (namelen == 1 && name && name[0] == '.') return;

    // ".." pops; any other component appends. path_addelem reads c->path (the
    // shared parent Path) as the base and allocates a FRESH owned Path (ref ==
    // 1) -- it NEVER mutates the shared Path (immutable-string property), so
    // reading c->path then replacing it below is safe. NULL on OOM / overflow /
    // unknown-parent -> the name becomes "unknown" and the walk still succeeds.
    struct Path *np = path_addelem(c->path, name, namelen);

    // Replace c's currently-shared Path with the new one. The drop releases the
    // ref spoor_clone took on the parent's Path; the parent keeps its own.
    struct Path *old = c->path;
    c->path = np;
    path_unref(old);
}

void spoor_path_transplant(struct Spoor *dst, struct Spoor *src) {
    if (!dst) return;
    struct Path *np = src ? src->path : NULL;
    path_ref(np);                 // NULL-safe; take dst's own hold on src's Path
    struct Path *old = dst->path;
    dst->path = np;
    path_unref(old);              // drop the Path the clone-walk shared from the source
}

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
