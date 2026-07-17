// stalk -- the per-Proc multi-component pathname resolver (Plan 9 `namec`,
// renamed for the Thylacine bestiary: the predator stalks its quarry along a
// path through the namespace to the target Spoor).
//
// Binding design: docs/STALK-DESIGN.md (signed off 2026-06-02). Invariant I-28
// (ARCHITECTURE.md section 28): path-resolution containment + per-component
// X-search + mount-cross permission. stalk-2 adds Plan 9 `domount`: after
// resolving a component, stalk crosses to the mounted tree iff the resolved
// Spoor's (dc, devno, qid.path) identity matches a mount-table entry
// (territory.c::mount_lookup). Crossing is "on descent" -- a Spoor is crossed
// the moment it is used as a directory to walk through, and the quarry is
// crossed at the end (so opening a mount point opens the mounted root), EXCEPT
// under STALK_MOUNT. stalk-3 adds the namespace-resident /srv consumer.
//
// Vocabulary (user-approved thematic names):
//   - stalk  : the resolver.
//   - trail  : the in-call stack of resolved Spoors the resolver follows and
//              that `..` pops back along (bounded at `start` -- the chroot/pivot
//              boundary -- so `..` can never escape; I-28).
//   - quarry : the target Spoor the resolver returns.
//
// Lifetime: every resolved Spoor on the trail is an owned clone (its own fid
// for dev9p; qid-only for devramfs); on return the resolver clunks every trail
// entry EXCEPT the quarry, which carries the caller's ref. `start` is BORROWED
// (the caller owns it -- a handle's Spoor or the Territory root_spoor) and is
// never reffed or clunked.

#ifndef THYLACINE_STALK_H
#define THYLACINE_STALK_H

#include <thylacine/types.h>

struct Proc;
struct Spoor;
struct t_stat;   // <thylacine/syscall.h>; the stalk_stat metadata sink

// amode -- what stalk does at the final (quarry) component.
#define STALK_WALK  0   // resolve only; do NOT open (the O_PATH / walkable-base
                        // case -- a navigation / create / chroot target). The
                        // quarry IS crossed (a walked mount point yields the
                        // mounted root).
#define STALK_OPEN  1   // resolve + Dev.open(quarry, omode) (the byte-I/O case).
                        // The quarry is crossed before opening.
#define STALK_MOUNT 2   // resolve to the mount point's OWN identity (the final
                        // component is NOT crossed) + do NOT open. SYS_MOUNT /
                        // SYS_UNMOUNT use this so MREPL re-keys the same
                        // underlying mount point even when it already hosts a
                        // mount (Plan 9 Amount). Intermediate components still
                        // cross normally (you can mount onto /a/b where /a is
                        // itself a mount).
#define STALK_STAT  3   // resolve for METADATA only (POUNCE; SYS_STAT): like
                        // STALK_WALK (quarry crossed, never opened), but when
                        // the final run resolves via Dev.walk_attrs the leaf's
                        // attrs return in the fused reply and NO quarry Spoor /
                        // fid is ever materialized (the walk-QUERY form -- the
                        // 1-RPC stat). Callers use stalk_stat(); passing
                        // STALK_STAT to stalk()/stalk_err() (no stat sink)
                        // degrades to STALK_WALK behavior.

// Trail depth cap: the maximum number of path components stalk resolves. An
// over-deep path (including a '..'-heavy path that pushes past the cap before
// popping) fails cleanly rather than overflowing the fixed trail array.
#define STALK_MAX_DEPTH 40

// stalk -- resolve `path` (`pathlen` bytes, NUL-free; the caller has already
// copied it from user space and rejected embedded NUL) from `start` to a target
// Spoor.
//
//   p        : the calling Proc (for the per-component perm_check; the handler
//              passes current_thread()->proc, a test passes a synthetic Proc).
//   start    : the base Spoor -- BORROWED. The handler selects it: the
//              Territory root_spoor for an absolute walk, or a dirfd's Spoor for
//              a relative one. stalk never refs or clunks it.
//   path     : the path, '/'-separated. Empty components (leading '/', '//')
//              collapse; "." is a no-op; ".." pops the trail (contained at
//              `start`). Each real component is <= SYS_WALK_OPEN_NAME_MAX bytes.
//   amode    : STALK_WALK or STALK_OPEN.
//   omode    : the Plan 9 open mode (OREAD/OWRITE/ORDWR/OEXEC + OTRUNC); used
//              for the final-hop perm_check and Dev.open under STALK_OPEN.
//
// Returns the resolved Spoor (the quarry; ref == 1, opened iff STALK_OPEN) or
// NULL on any failure (missing component, permission denied, depth overflow,
// OOM, open failure). The caller installs the handle and derives its rights.
struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode);

// stalk_err -- the errno-aware core (the errno-rollout arc; ERRORS.md). Identical
// to stalk(), but on a NULL return writes the cause to *errp (OPTIONAL -- may be
// NULL) as a POSITIVE T_E_<NAME> code: T_E_NOENT (missing component), T_E_ACCES
// (perm_check denial), T_E_INVAL (structural reject), or a propagated / T_E_IO
// otherwise. NEVER T_E_PERM (== 1, which collides with the generic -1 sentinel).
// On a non-NULL return *errp is unspecified. The caller returns -*errp so a
// missing path surfaces as -T_E_NOENT (Go os.IsNotExist) instead of the bare -1
// (which Go's Linux-shaped decode renders EPERM). stalk() == stalk_err(...,NULL).
struct Spoor *stalk_err(struct Proc *p, struct Spoor *start,
                        const char *path, u64 pathlen, int amode, u32 omode,
                        int *errp);

// stalk_stat -- resolve `path` and fill *out with the LEAF's metadata without
// installing anything (POUNCE; the SYS_STAT core). The X-search is identical
// to a STALK_WALK resolution (POSIX stat authority = the path X-search only;
// the leaf's own R/W are irrelevant). On the fast path (the final run's Dev
// implements walk_attrs and the leaf is not a mount point) NO quarry Spoor or
// fid is ever created -- the attrs arrive fused with the walk. Fallback paths
// (Dev without walk_attrs / leaf mount point / zero-component path) resolve a
// quarry, stat_native it, and clunk it -- today's exact O_PATH+fstat shape.
// Returns 0 (out filled) or -1 with the cause in *errp (OPTIONAL; same codes
// as stalk_err).
int stalk_stat(struct Proc *p, struct Spoor *start,
               const char *path, u64 pathlen,
               struct t_stat *out, int *errp);

// stalk_cross_mounts -- Plan 9 `domount`, exposed for the single-hop walk
// syscalls (SYS_WALK_OPEN) so they cross mounts identically to stalk()/SYS_OPEN.
// Tests `probe`'s (dc, devno, qid.path) identity against `p`'s mount table; if
// it is a mount point, mints an INDEPENDENT clone-walk of the mounted source and
// follows a mount-over-mount chain to the leaf. `probe` is NOT consumed -- the
// caller decides whether to clunk it.
//
//   *out == NULL, return 0 : probe is not a mount point (no crossing).
//   *out != NULL, return 0 : crossed; *out is OWNED (caller clunks it).
//   return -1              : probe IS a mount point but minting the crossed
//                            Spoor failed; *out == NULL; probe still owned.
int stalk_cross_mounts(struct Proc *p, struct Spoor *probe, struct Spoor **out);

#endif // THYLACINE_STALK_H
