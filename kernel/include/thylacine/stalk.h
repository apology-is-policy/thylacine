// stalk -- the per-Proc multi-component pathname resolver (Plan 9 `namec`,
// renamed for the Thylacine bestiary: the predator stalks its quarry along a
// path through the namespace to the target Spoor).
//
// Binding design: docs/STALK-DESIGN.md (signed off 2026-06-02). Invariant I-28
// (ARCHITECTURE.md section 28): path-resolution containment + per-component
// X-search. This is the stalk-1 sub-chunk: resolution WITHIN one Dev (no
// mount-crossing yet -- stalk-2 adds Plan 9 `domount` keyed by mount-point
// Spoor identity; stalk-3 the namespace-resident /srv consumer).
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

// amode -- what stalk does at the final (quarry) component.
#define STALK_WALK  0   // resolve only; do NOT open (the O_PATH / walkable-base
                        // case -- a navigation / create / chroot target).
#define STALK_OPEN  1   // resolve + Dev.open(quarry, omode) (the byte-I/O case).

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

#endif // THYLACINE_STALK_H
