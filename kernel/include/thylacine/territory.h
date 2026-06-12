// Process group (territory) — Plan 9 Territory, Thylacine adaptation
// (P2-Eb + P5-attach-mount).
//
// Per ARCHITECTURE.md §9.1 + §9.6 + specs/territory.tla. A Territory owns
// a process's namespace: two parallel tables —
//
//   binds[]   : path-to-path bind edges (Plan 9 `bind`, the symbolic
//               mapping). Walking `dst` produces `src`. Cycle-checked at
//               every bind() — invariant I-3 (mount points form a DAG).
//
//   mounts[]  : (target_path, Spoor *) grafts (Plan 9 `mount`, the
//               filesystem-as-Spoor primitive of §9.6). Walking
//               target_path dispatches through the source Spoor's Dev
//               vtable. Each entry holds one refcount on its source.
//
// At v1.0 path_id_t is `u32` — abstract path tokens. The fd-syscall
// surface (deferred) populates these with real path identifiers; tests
// pick numeric IDs.
//
// State invariants (proven in specs/territory.tla):
//
//   NoCycle (I-3): for every Territory `p` and every path `x`, x is not
//   reachable from its own binding set via the transitive closure.
//   Enforced by would_create_cycle inside bind().
//
//   MountRefcountConsistency (§9.6.6): for every Spoor s, the kernel's
//   refcount agrees with the cardinality of mount entries referencing s.
//   Maintained structurally by mount() / unmount() / territory_clone() /
//   territory_unref()'s final-release path.
//
//   Isolation (I-1): bind/mount tables are per-Territory. No action
//   updates two Territories simultaneously. RFNAMEG (shared territory)
//   is deferred to Phase 5+ syscall surface.
//
// Naming (P5-attach-mount): the verb `unmount` is reserved for the
// mount-table primitive; `unbind` is the inverse of `bind`. This matches
// Plan 9's syscall surface (bind / unbind / mount / unmount). Prior to
// this chunk the C function was misnamed `unmount`.

#ifndef THYLACINE_PGRP_H
#define THYLACINE_PGRP_H

#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct Spoor;

// PGRP_MAGIC — sentinel set at territory_alloc; checked at territory_unref
// final release. SLUB's freelist write at kmem_cache_free clobbers offset
// 0, catching double-free.
#define PGRP_MAGIC 0x50475250C0DEFADEULL    // 'PGRP' || 0xC0DE'FADE

// Bind-table size. Per the original P2-Eb sizing rationale: small fixed
// cap (8 edges per Territory). Sufficient for v1.0's bind tests + the
// eventual ramfs + /proc + /dev + /net binds.
#define PGRP_MAX_BINDS  8

// Mount-table size. Sufficient for the v1.0 boot path. joey is the high-water
// mark: the kproc boot namespace mounts /srv + /proc + /ctl + /dev (4) onto
// devramfs synth dirs (inherited by every Proc), and the long-running init then
// re-grafts /srv + /bin + /proc + /ctl + /dev (5) onto the pivoted disk root --
// 9 live entries (#57b). The pre-pivot kernel mounts ORPHAN after pivot (their
// devramfs mount points become unreachable from the disk root) but remain in the
// table, so the count is pre+post per re-grafted dir; a pivot-time GC of dead
// mounts is the tracked seam (would halve joey's count). 12 leaves headroom for
// /net (Phase 8) without another bump. The proc.rfork stress test clones
// Territories en masse; each clone deep-copies mounts[] + bumps a spoor_ref per
// entry, so the cap stays modest to hold the per-clone cost in check.
#define PGRP_MAX_MOUNTS  12

// Path identifier. At v1.0 abstract `u32` — bind/mount take whatever
// numeric ID the caller decides on (tests pick small integers). The
// fd-syscall surface (deferred) will translate strings to path IDs
// before reaching this layer.
typedef u32 path_id_t;

// One edge in the bind graph: walking `dst` produces `src` per ARCH §9.1
// ("bind(old, new) — attach old's contents at new"). Spec maps:
//   - PgrpBind.dst   <- spec's `dst` (the mount point)
//   - PgrpBind.src   <- spec's `src` (the bound content)
// An edge `(src, dst)` is `dst -> src` in the walk direction.
struct PgrpBind {
    path_id_t src;
    path_id_t dst;
};

// One entry in the mount table: the Spoor `source` is grafted at the
// MOUNT POINT identified by (mp_dc, mp_devno, mp_qid_path) -- the Plan 9
// (type, dev, qid) identity of the directory mounted onto (stalk-2; was an
// abstract path_id_t target at v1.0). `stalk`, after resolving a component to
// Spoor S, crosses to a clone of `source` iff S's (dc, devno, qid.path) matches
// this key (Plan 9 `domount`). The entry holds one refcount on source; dropped
// at unmount() OR at Territory final release.
//
// Why the full triple: qid.path is unique only WITHIN a (dc, devno) instance.
// Every dev9p session shares dc='9' and every attach root has qid.path 0, so
// (dc, qid.path) alone cannot distinguish two concurrent 9P sessions' mount
// points (the A-5b corvus + per-user-stratum-fs case). devno (Plan 9 Chan.dev,
// minted per attach by spoor_next_devno) closes that.
//
// Field order: pointer first (8B, 8-aligned), u64 qid_path (8B), then the three
// u32s + pad fill the last 16B. sizeof(PgrpMount) = 32.
struct PgrpMount {
    struct Spoor   *source;
    u64             mp_qid_path;
    int             mp_dc;
    u32             mp_devno;
    u32             flags;    // MREPL / MBEFORE / MAFTER / MCREATE
    u32             _pad;     // 8-byte array-stride alignment for source
};

// Mount flags (ARCH §9.6.1 — mirror Plan 9). At v1.0 only MREPL has
// distinguished semantics in the impl (it replaces an existing entry at
// the same target); the others land at v1.x when union semantics get
// their own implementation work.
#define MREPL     0x0001
#define MBEFORE   0x0002
#define MAFTER    0x0004
#define MCREATE   0x0008

struct Territory {
    u64                  magic;      // PGRP_MAGIC
    int                  ref;        // refcount; rfork(RFNAMEG) shares (Phase 5+)
    int                  nbinds;
    int                  nmounts;
    u32                  _pad;       // 8-byte alignment for root_spoor + binds[]
    // P5-stratumd-stub-bringup-e2: the pivoted root Spoor (NULL until the
    // first territory_chroot). When SYS_WALK_OPEN is called with the
    // spoor_fd == -1 sentinel, the handler uses this Spoor as the walk
    // source. Holds one refcount on its target Spoor (taken at chroot,
    // dropped at re-chroot OR at territory_unref final release).
    struct Spoor        *root_spoor;
    struct PgrpBind      binds[PGRP_MAX_BINDS];
    struct PgrpMount     mounts[PGRP_MAX_MOUNTS];
    // LS-4: per-Proc cwd (the Plan 9 "dot"), name-based. A cleaned absolute
    // path string; NULL means "/" (the common case -- heap-allocated lazily
    // only when a Proc chdir's away from root, kfree'd at territory_unref).
    // dot_lock is a LEAF lock guarding ONLY dot_path: held for the short
    // copy/swap, NEVER across stalk (which may block on 9P). Placed AFTER
    // mounts[] so root_spoor / binds[] / mounts[] offsets are unchanged.
    // Threads of a Proc SHARE dot_path (POSIX per-process cwd); a child gets an
    // independent snapshot via territory_clone. A handle-based dot Spoor is the
    // v1.x upgrade (landing with symlinks). See LIFE-SUPPORT.md LS-4 +
    // STALK-DESIGN.md 4.3.
    spin_lock_t          dot_lock;
    char                *dot_path;
    // RW-4 SA-F1: serializes the mount table (mounts[]/nmounts), the bind table
    // (binds[]/nbinds), and root_spoor against concurrent access by peer Threads
    // of a Proc (which share the Territory) and by rfork(RFNAMEG)-sharing Procs.
    // Before SA-F1 these were unlocked -- a concurrent pivot_root/unmount on one
    // thread could free a Spoor a walking thread was mid-read on (root_spoor /
    // mount-source UAF). A near-leaf lock: held ONLY for the table read-modify-
    // write, NEVER across stalk or a spoor_clunk (the Dev close hook may sleep) --
    // the displaced/removed source is captured under the lock + clunked outside it
    // (the dot_lock free-outside-the-lock discipline). Placed last so every pinned
    // offset above is unchanged.
    spin_lock_t          ns_lock;
};

_Static_assert(sizeof(struct PgrpMount) == 32,
               "struct PgrpMount pinned at 32 bytes (8 source + 8 mp_qid_path + "
               "4 mp_dc + 4 mp_devno + 4 flags + 4 pad). stalk-2 re-keyed from "
               "the abstract path_id_t target to the (dc, devno, qid.path) "
               "mount-point identity; the deliberate +16/entry growth.");
_Static_assert(sizeof(struct Territory)
               == 32 + 8 * PGRP_MAX_BINDS + 32 * PGRP_MAX_MOUNTS + 24,
               "struct Territory size pinned (stalk-2: 24 header + 8 root_spoor "
               "+ 8*PGRP_MAX_BINDS + 32*PGRP_MAX_MOUNTS; LS-4 appended "
               "dot_lock[4] + pad[4] + dot_path[8] = 16 after mounts[]; RW-4 SA-F1 "
               "appended ns_lock[4] + pad[4] = 8 more = 24).");
_Static_assert(__builtin_offsetof(struct Territory, magic) == 0,
               "magic must be at offset 0 (SLUB freelist write "
               "clobbers it on free, double-free defense)");
// F5 close (P5-stratumd-stub-bringup audit): pin every load-bearing
// field offset, not just the size. A future field-reorder that shifts
// root_spoor / binds / mounts without changing the total size would
// silently break syscall.c::sys_walk_open_handler's FROM_ROOT path,
// territory_chroot's ref discipline, and the mount-table iteration.
_Static_assert(__builtin_offsetof(struct Territory, root_spoor) == 24,
               "root_spoor pinned at offset 24 (after 8B magic + "
               "4B ref + 4B nbinds + 4B nmounts + 4B _pad)");
_Static_assert(__builtin_offsetof(struct Territory, binds) == 32,
               "binds[] pinned at offset 32 (after the 32B header)");
_Static_assert(__builtin_offsetof(struct Territory, mounts)
               == 32 + 8 * PGRP_MAX_BINDS,
               "mounts[] pinned after binds[]");
// LS-4: the cwd fields trail mounts[] so the offsets above stay fixed.
_Static_assert(__builtin_offsetof(struct Territory, dot_lock)
               == 32 + 8 * PGRP_MAX_BINDS + 32 * PGRP_MAX_MOUNTS,
               "dot_lock pinned after mounts[] (LS-4)");
_Static_assert(__builtin_offsetof(struct Territory, dot_path)
               == 32 + 8 * PGRP_MAX_BINDS + 32 * PGRP_MAX_MOUNTS + 8,
               "dot_path pinned after dot_lock (4B lock + 4B pad)");

// Bring up the territory subsystem. Allocates the Territory SLUB cache
// and kproc's initial Territory (empty bindings, empty mounts; ref=1).
// Must be called after slub_init and BEFORE proc_init.
void territory_init(void);

// Accessor for kproc's initial Territory.
struct Territory *kpgrp(void);

// SLUB-allocate a fresh empty Territory with ref=1. Returns NULL on OOM.
struct Territory *territory_alloc(void);

// Allocate a new Territory and deep-copy parent's bindings + mounts into
// it. For every cloned mount entry, spoor_ref(source) is called — each
// clone contributes one new reference per mount entry. Returns NULL on
// OOM (parent unchanged). Models the spec's ForkClone action.
struct Territory *territory_clone(struct Territory *parent);

// Increment/decrement refcount. territory_unref frees the struct when
// the last reference goes away; the final-release path FIRST calls
// spoor_unref on every mount entry's source (releasing the per-entry
// reference held since mount()), THEN kmem_cache_free's the struct.
// Both are safe to call on NULL.
void territory_ref(struct Territory *p);
void territory_unref(struct Territory *p);

// =============================================================================
// Per-Proc cwd ("dot") -- LS-4. Name-based: a cleaned absolute path string,
// NULL == "/". All three take dot_lock internally where they touch dot_path
// (the leaf lock). See LIFE-SUPPORT.md LS-4 + STALK-DESIGN.md 4.3.
// =============================================================================

// Copy the current cwd (or "/" when dot_path is NULL) into `buf`,
// NUL-terminated, bounded by `cap`. Returns the path length (excluding the
// NUL), or -1 on bad args / buffer too small.
int territory_getdot(struct Territory *p, char *buf, u64 cap);

// Resolve `input` (len `inlen`, NOT NUL-terminated) against `p`'s cwd into a
// cleaned absolute path written to `out` (NUL-terminated, capacity `outcap`).
// An absolute `input` ignores the cwd; a relative one is joined to it. Reads
// dot_path UNDER dot_lock (the lexical resolve is bounded CPU -- no alloc, no
// block -- so it is safe under the leaf lock). Returns the output length
// (excluding NUL) or -1 if it would not fit. This is the SYS_CHDIR + the
// SYS_OPEN-relative-join entry point.
int territory_resolve_cwd(struct Territory *p, const char *input, u64 inlen,
                          char *out, u64 outcap);

// Replace dot_path with a kmalloc'd copy of `cleaned` (which MUST be a cleaned
// absolute path) under dot_lock, freeing the old. "/" is stored as the NULL
// sentinel (no allocation). Returns 0, or -1 on OOM (dot_path unchanged).
int territory_setdot(struct Territory *p, const char *cleaned);

// Pure lexical resolver (no locks, no allocation) -- the testable core of
// territory_resolve_cwd. `dot` is the cwd string (NULL or "/" == root). Exposed
// for unit tests; production callers use territory_resolve_cwd.
int cwd_lexical_resolve(const char *dot, const char *input, u64 inlen,
                        char *out, u64 outcap);

// =============================================================================
// Bind (path-to-path).
// =============================================================================

// bind: add the edge `dst -> src` to `territory`'s bind graph. Walking
// `dst` in this territory will subsequently produce `src`. Cycle-checked:
// rejects if adding the edge would create a cycle.
//
// Return values:
//    0   success.
//   -1   would create a cycle (rejects per `WouldCreateCycle` in
//        territory.tla; corresponds to Plan 9's "namespace cycle:
//        cannot bind X onto Y" errstr).
//   -2   the edge already exists (idempotent rebind is a no-op-error
//        at v1.0; future MREPL/MBEFORE/MAFTER union semantics treat
//        this differently).
//   -3   binds[] full (PGRP_MAX_BINDS reached).
//   -4   trivial self-bind (src == dst); treated as a cycle.
int bind(struct Territory *territory, path_id_t src, path_id_t dst);

// unbind: remove the edge `dst -> src` from `territory`'s bind graph.
// Returns 0 on success, -1 if the edge does not exist.
//
// (Renamed from `unmount` at P5-attach-mount — `unmount` is now the
// mount-table primitive below.)
int unbind(struct Territory *territory, path_id_t src, path_id_t dst);

// =============================================================================
// Mount (graft-a-Spoor-at-a-path).
// =============================================================================

// mount: graft Spoor `source` at the MOUNT POINT `mountpoint` in
// `territory`'s mount table (stalk-2: was an abstract path_id_t target).
// The entry records `mountpoint`'s (dc, devno, qid.path) IDENTITY -- it does
// NOT retain `mountpoint` itself (the caller stalk's it, mount() copies the
// key, the caller clunks it). Bumps source's refcount; the entry holds that
// reference until unmount() or Territory destruction releases it.
//
// `flags` mirror Plan 9 (MREPL / MBEFORE / MAFTER / MCREATE); only MREPL
// has distinguished semantics at v1.0 — when MREPL is set and an entry
// at the same mount-point identity already exists, the existing entry is
// replaced (its source's ref is dropped). The other flags are stored for
// future union-mount work; at v1.0 they're treated as "append a new entry."
//
// Idempotency: mount(t, S, mp, flags) where (key(mp), S) is already in the
// mount table is a no-op success (returns 0; refcount not bumped; the spec's
// `<<path, s>> \notin mounts[p]` precondition under the re-keyed identity).
//
// Return values:
//    0   success (entry added or idempotent no-op).
//   -1   source or mountpoint is NULL / has corrupted magic.
//   -2   mounts[] full (PGRP_MAX_MOUNTS reached).
//   -3   would create a mount cycle (I-3) -- a self-mount (source identity ==
//        mountpoint identity) or a cross-tree oscillation. The SVC layer
//        collapses every failure to -1; the distinct code is for the C-API +
//        tests (mirrors bind()'s -1 cycle code).
int mount(struct Territory *territory, struct Spoor *source,
          struct Spoor *mountpoint, u32 flags);

// unmount: remove ONE mount entry whose mount-point identity matches
// `mountpoint`'s (dc, devno, qid.path) from `territory`'s mount table and
// drop the source's refcount. Models the spec's Unmount action.
//
// If multiple entries exist at the same mount point (union mount), this
// removes the FIRST found; subsequent calls drop the others one by one.
//
// Return values:
//    0   success (entry removed; refcount dropped).
//   -1   no entry exists at `mountpoint`'s identity / mountpoint is NULL.
int unmount(struct Territory *territory, struct Spoor *mountpoint);

// mount_lookup: the `stalk` cross-mount probe (Plan 9 `domount`). Returns a
// REF-HELD source Spoor of the FIRST mount entry whose mount-point identity
// matches `probe`'s (dc, devno, qid.path), or NULL if `probe` is not a mount
// point. RW-4 SA-F1 changed the contract from borrow to OWNED: the lookup +
// spoor_ref happen atomically under ns_lock so a concurrent unmount cannot free
// the source between the lookup and the caller's use. THE CALLER MUST
// spoor_clunk the returned Spoor when done (stalk_cross_mounts clunks it after
// clone_walk_zero mints the independent crossed Spoor).
struct Spoor *mount_lookup(struct Territory *territory, struct Spoor *probe);

// territory_root_ref: atomically read root_spoor + take a ref under ns_lock, so
// the read+ref cannot race a concurrent territory_pivot_root / territory_chroot
// that swaps root_spoor + clunks the displaced one to zero (RW-4 SA-F1). Returns
// a REF-HELD root Spoor (caller spoor_clunks it) or NULL if no root is set. This
// is the ONLY sound way to obtain the FROM_ROOT walk base in a multi-thread Proc.
struct Spoor *territory_root_ref(struct Territory *territory);

// =============================================================================
// Chroot (root-Spoor pivot) — P5-stratumd-stub-bringup-e2.
// =============================================================================

// territory_chroot: stamp `source` as `territory`'s pivoted root Spoor.
// The root is the Spoor at which name resolution starts when a userspace
// SYS_WALK_OPEN is called with the spoor_fd == -1 sentinel — i.e., when
// the caller asks the kernel to walk "from my root" rather than from an
// explicitly-named handle.
//
// Lifecycle: bumps source's refcount (the Territory holds one reference
// until either a subsequent territory_chroot replaces it OR Territory
// destruction releases it). If a previous root was set, its refcount is
// dropped via spoor_clunk (so the Dev's close hook runs if this was the
// last holder — same lifecycle discipline as mount()/MREPL displacement).
//
// Idempotency: territory_chroot(territory, S) where root_spoor == S is a
// no-op success (returns 0; refcount not bumped). Mirrors mount()'s
// idempotent-same-source semantics.
//
// Spec: maps to `specs/territory.tla::Chroot(p, s)`. Refcount discipline
// pinned by MountRefcountConsistency (refcount[s] = mount-table-count +
// |{p : root_spoor[p] = s}|).
//
// Return values:
//    0   success (root stamped or idempotent no-op).
//   -1   source is NULL.
int territory_chroot(struct Territory *territory, struct Spoor *source);

// =============================================================================
// Pivot root (long-running-Proc root swap) — P6-pouch-stratumd-boot 16c.
// =============================================================================

// territory_pivot_root: atomically replace `territory`'s root_spoor with
// `source`, dropping the displaced root's reference. Unlike
// territory_chroot (which is documented for initial chroot use and is
// what kproc calls at boot to stamp devramfs), territory_pivot_root is
// the LONG-RUNNING-Proc primitive: it requires an existing root
// (root_spoor != NULL) and swaps it for a new one.
//
// Joey's v1.0 usage: stamp stratumd's mounted FS root over the
// devramfs root, AFTER all bringup spawns (those resolve through the
// devramfs root). The pivot is the LAST bringup step before
// `Thylacine boot OK`. Closes the v1.x note in usr/joey/joey.c around
// line 293-304 ("v1.x adds SYS_UNCHROOT or a proper pivot_root").
//
// Semantics mirror territory_chroot's atomicity + refcount discipline:
//   - if root_spoor == source: idempotent no-op success (returns 0;
//     no refcount change). Matches Plan 9 / Linux pivot-to-same.
//   - else: spoor_ref(source) BEFORE swap; territory->root_spoor =
//     source; spoor_clunk(old) AFTER swap.
//
// spoor_clunk (NOT spoor_unref) on the displaced root so if this was
// the last holder, the Dev's close hook runs — for joey's pivot the
// old devramfs root is held by:
//   (a) joey's territory root_spoor (the holder this call drops)
//   (b) kproc's territory root_spoor (unaffected; kproc is alive)
//   (c) any child Proc that inherited via territory_clone before
//       pivot (each holds its own ref)
// so joey's pivot does NOT free the devramfs root structurally --
// kproc keeps it alive for the lifetime of the system. The clunk is
// per-Territory discipline, not a system-wide tear-down.
//
// Pre-condition refusal: REQUIRES root_spoor != NULL (this is
// "swap an existing root", not "establish initial root"). Returns -1
// if the caller has no current root_spoor. Use territory_chroot for
// the initial-chroot case (kproc's boot-time setup).
//
// Spec posture: same shape as `specs/territory.tla::Chroot(p, s)` --
// the formal state transition is identical to chroot under the
// renamed action. No new spec module per the 2026-05-23 spec-to-code
// suspension; the no-cycle invariant (I-3) holds trivially because
// pivot does not touch the bind graph, and refcount consistency
// (§9.6.6) holds via the matched bump + drop pattern.
//
// Bind / mount table state across pivot: at v1.0, the per-Territory
// bind[] and mounts[] are NOT modified by territory_pivot_root. Any
// mount entries that pointed at sub-trees of the OLD root remain
// active; walking them post-pivot still routes through the mount-
// table's Spoors (mount-table entries don't depend on root_spoor for
// addressability -- they're keyed by path_id_t in the per-Territory
// namespace). v1.x bind-survivor semantics (e.g., a `/dev` bind that
// crosses the pivot) lift on top of this primitive.
//
// Return values:
//    0   success (root swapped or idempotent no-op).
//   -1   territory has no current root_spoor (pre-condition fail) OR
//        source is NULL.
int territory_pivot_root(struct Territory *territory, struct Spoor *source);

// Diagnostic.
int  territory_nbinds(struct Territory *territory);
int  territory_nmounts(struct Territory *territory);
u64  territory_total_created(void);
u64  territory_total_destroyed(void);

#endif // THYLACINE_PGRP_H
