// stalk -- the per-Proc multi-component pathname resolver. See
// <thylacine/stalk.h> + docs/STALK-DESIGN.md. Invariant I-28.
//
// stalk-2 adds Plan 9 `domount`: after resolving a component, stalk crosses to
// the mounted tree iff the resolved Spoor's (dc, devno, qid.path) identity
// matches a Territory mount-table entry (territory.c::mount_lookup). The
// resolver still generalizes sys_walk_open_handler's single-hop
// clone->walk->clunk lifetime to N hops via a `trail` of owned clones, with
// `.`/`..` handled in the resolver (never passed to Dev.walk), `..` contained at
// `start`, and a per-component perm_check X-search at each directory hop.
//
// Crossing is "on descent" (Plan 9 namec): a trail Spoor is crossed the moment
// it is used as a directory to walk THROUGH (replaced in place by the mounted
// root, which is then X-checked), and the quarry is crossed at the end (so
// opening a walked mount point opens the mounted root) -- EXCEPT under
// STALK_MOUNT, which returns the mount point's OWN identity so SYS_MOUNT's MREPL
// re-keys the same underlying point. When no mounts exist cross_mounts is a
// table-lookup no-op, so stalk-1 behavior is preserved exactly.
//
// Lifetime discipline (mirrors the audited sys_walk_open_handler):
//   - `start` is BORROWED; never reffed or clunked.
//   - each hop: nc = spoor_clone(parent) -- nc->aux is a SHALLOW copy of the
//     parent's aux (a SHARED dev9p fid) until a successful Dev.walk REPLACES it
//     with nc's own fid. So:
//       * walk returns NULL (or the reuse-nc contract is violated): nc still
//         shares the parent's aux -> detach (nc->aux = NULL) + spoor_unref, NOT
//         spoor_clunk (which would clunk the parent's fid).
//       * walk succeeds: nc owns its fid -> push on the trail; clunk-safe.
//   - a crossed Spoor (clone_walk_zero of a mount source) likewise owns a fresh
//     fid -> clunk-safe.
//   - on return, every trail entry is spoor_clunk'd EXCEPT the quarry.

#include <thylacine/stalk.h>

#include <thylacine/dev.h>
#include <thylacine/errno.h>      // T_E_* (the errno-rollout arc; stalk *errp)
#include <thylacine/perm.h>
#include <thylacine/proc.h>       // struct Proc -> territory
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>    // SYS_WALK_OPEN_NAME_MAX + struct t_stat
#include <thylacine/territory.h>  // mount_lookup + PGRP_MAX_MOUNTS + root_ref

#include "../mm/slub.h"           // kmalloc/kfree (the D-1 expansion state)

// Map a spoor_stat_native / Dev return (0 ok, a negative errno, or the generic
// -1) into a POSITIVE T_E_* code for stalk's *errp. A real -errno in [-4095,-2]
// yields its magnitude; -1 (the generic sentinel -- and == -T_E_PERM, which must
// never surface as errno 1) and anything else collapse to T_E_IO.
static int err_code(int ret) {
    if (ret <= -2 && ret >= -4095) return -ret;
    return T_E_IO;
}

// stalk_tip_is_dir -- is the current resolution position a directory? The
// subject is the trail tip, or `start` at depth 0.
//
// #81: POSIX resolves "." and ".." as PATH COMPONENTS, so the thing they are
// resolved *in* must be a directory -- `/etc/passwd/..` is ENOTDIR, not a
// lexical pop back to /etc. stalk handles both tokens itself (they never reach
// Dev.walk), so the #79 gate -- which sits on the real-component path -- never
// saw them, and a file tip silently accepted both.
//
// The type read is deliberately UNCROSSED, unlike the #79 gate (which runs
// after cross-on-descent). Not an oversight -- crossing would be wrong here:
//   - '..' pops a TRAIL ENTRY, and the pop lands on trail[depth-2] whichever
//     Spoor occupies the tip. Crossing would spend a clone_walk_zero to read a
//     type and change nothing about where we land.
//   - '.' must leave the resolution EXACTLY where it was: `/mnt/.` has to mean
//     `/mnt`, and under STALK_MOUNT `/mnt` deliberately does NOT cross (MREPL
//     re-keys the mount point's own identity). Crossing on '.' would make
//     `/mnt/.` return the mounted root while `/mnt` returns the mount point --
//     a divergence this gate would be INTRODUCING, not fixing.
// The two types can disagree only for a mount whose point and root differ in
// kind (a directory grafted onto a file -- mount() does not gate on type).
// Nothing in-tree builds one; if one existed, `/f/..` would answer ENOTDIR
// while `/f/x` crossed and resolved.
//
// qid.type is the same TOTAL, fetch-free signal #79 established: every Spoor
// carries one, so this gate takes no lock, issues no RPC, and cannot itself
// fail -- there is no "type fetch failed" case to disposition.
static bool stalk_tip_is_dir(struct Spoor **trail, int depth,
                             struct Spoor *start) {
    struct Spoor *tip = (depth > 0) ? trail[depth - 1] : start;
    return (tip->qid.type & QTDIR) != 0;
}

// stalk_tip_may_search -- may the caller SEARCH the current resolution
// position? Same subject as stalk_tip_is_dir: the trail tip, or `start` at
// depth 0. Returns 0, or the errno to fail with.
//
// #84: POSIX resolves "." and ".." INSIDE the current directory, so they are
// lookups like any other and require X (search) on the directory performing
// them. Measured on a POSIX host, non-root: with `chmod 000 d`, `stat("d")`
// SUCCEEDS (the lookup happens in d's parent) while `stat("d/.")`,
// `stat("d/..")` and `stat("d/x")` are all EACCES. The real-component arm has
// always X-checked its parent; the dot arms never reach that arm, so `d000/..`
// resolved where the sibling `d000/x` was denied. Both tokens need it -- the
// task named only '..', but the two measure identically in every row, exactly
// as #81 found for the type gate.
//
// ORDER: the caller runs stalk_tip_is_dir FIRST. That is #79's rule (the x bit
// on a non-directory is meaningless) and it is what POSIX does: a 0000 REGULAR
// FILE answers ENOTDIR for `f/..`, not EACCES -- measured, both tokens. Type
// before permission also keeps this gate free in the common failure.
//
// COST: `carried` when the pounce fused the tip's attrs into the walk that
// produced it (the '.' case -- a '.' breaks a pounce RUN but does not disable
// pouncing, so the previous run's leaf record describes exactly this tip),
// else one spoor_stat_native. That stat is NOT a new cost class: it is the
// same call the real-component arm makes at this same position, and on dev9p
// it is served from the Larder attr cache -- which exists for precisely this
// traffic ("the base X-check (stalk.c) re-stat storm", dev9p_stat_native).
// A '..' path never pounces at all (path_has_dotdot), so nothing is slowed
// that was fast.
//
// The subject is UNCROSSED, matching stalk_tip_is_dir -- required, not merely
// consistent: crossing on '.' would move resolution (`/mnt/.` must still mean
// `/mnt`, the divergence that comment refuses to introduce), so the two halves
// of one gate must read one object. A mount whose point and root differ in X
// is therefore judged by the point. That grants nothing: '.' stays put and
// '..' pops to a Spoor the caller already holds, and reaching any real
// component crosses and X-checks the mounted root as always.
static int stalk_tip_may_search(struct Proc *p, struct Spoor **trail, int depth,
                                struct Spoor *start, const struct t_stat *carried,
                                bool carried_valid) {
    struct Spoor *tip = (depth > 0) ? trail[depth - 1] : start;
    if (!tip->dev || !tip->dev->perm_enforced) return 0;

    struct t_stat st;
    if (carried_valid) {
        st = *carried;
    } else {
        int sr = spoor_stat_native(tip, &st);
        if (sr != 0) return err_code(sr);
    }
    return (perm_check(p, &st, PERM_X) != 0) ? T_E_ACCES : 0;
}

// Clunk every owned trail entry trail[0..depth). Used both on the success path
// (after the quarry is popped, this releases the ancestors) and on every
// failure path.
static void stalk_unwind(struct Spoor **trail, int depth) {
    while (depth > 0) {
        spoor_clunk(trail[--depth]);
    }
}

// clone_walk_zero -- mint an INDEPENDENT, openable clone of `src` (Plan 9
// cclone): spoor_clone + a zero-element Dev.walk, so dev9p allocates a FRESH fid
// (the clone does NOT keep sharing src's fid via the shallow-copied aux).
// Returns an owned Spoor (ref == 1) or NULL. On failure the clone's still-shared
// aux is detached before unref (NEVER clunk -- that would clunk src's fid).
// Used for both the 0-component quarry (clone of `start`) and every mount cross
// (clone of the mount source).
static struct Spoor *clone_walk_zero(struct Spoor *src) {
    if (!src || !src->dev || !src->dev->walk) return NULL;
    struct Spoor *q = spoor_clone(src);
    if (!q) return NULL;
    struct Walkqid *w = src->dev->walk(src, q, (const char **)0, 0);
    // Validate the reply shape with the same rigor as the main resolution loop
    // (which rejects w->nqid != 1): a 0-element walk MUST yield w->nqid == 0. A
    // Dev returning w->spoor == q with a phantom step (nqid != 0) would hand back
    // a crossed root carrying a qid the Dev set during a step that never
    // happened. All real Devs honor nname==0 -> nqid==0; reject otherwise.
    if (!w || w->spoor != q || w->nqid != 0) {
        if (w) walkqid_free(w);
        q->aux = NULL;
        spoor_unref(q);
        return NULL;
    }
    walkqid_free(w);              // a clone-walk has nqid == 0 (no path steps)
    return q;
}

// stalk_cross_mounts -- Plan 9 domount (public; also used by the single-hop
// SYS_WALK_OPEN handler so it crosses identically to stalk). Test `probe`'s
// (dc, devno, qid.path) identity
// against `p`'s Territory mount table; if it is a mount point, mint an
// independent clone-walk of the mounted source and (looping) follow a
// mount-over-a-mount chain to the leaf. `probe` is NOT consumed -- the caller
// decides whether to clunk it.
//   *out == NULL, return 0 : probe is not a mount point (no crossing).
//   *out != NULL, return 0 : crossed; *out is owned (caller clunks it).
//   return -1              : probe IS a mount point but minting the crossed
//                            Spoor failed (OOM / walk error); *out == NULL and
//                            probe is still owned -- the caller fails the walk.
// stalk_cross_from -- the generalized cross. The FIRST hop crosses the
// `first_member`-th UNION MEMBER of `probe` (mount_member_at, declared order)
// rather than always the primary; mount-over-mount hops that follow cross the
// primary (member 0) of each crossed link, as usual. `first_member == 0` is
// exactly stalk_cross_mounts (the common single-source / primary path); the
// union walk calls it with 1,2,... to try the later members after the primary
// missed a component. Returns 0 with *out == NULL when `probe` has no member at
// that index (not a mount point, or index past the last member).
static int stalk_cross_from(struct Proc *p, struct Spoor *probe, int first_member,
                            struct Spoor **out, bool *crossed_pheno) {
    // VIVARIUM section 13: crossed_pheno is a SET-ONLY accumulator (the caller
    // inits it false before the first cross and reads it after the last; NULL
    // for callers that do not resolve for exec). We only ever set it true, so a
    // mount-over-mount chain with a pheno-mount at ANY hop is detected.
    *out = NULL;
    if (!p || !p->territory || !probe) return 0;

    struct Spoor *cur = NULL;     // latest owned crossed clone (NULL until 1st)
    struct Spoor *id  = probe;    // identity to test each round
    // Bound by the table size: each cross consumes a DISTINCT entry, and the
    // mount graph is acyclic -- I-3 is ENFORCED at mount() time
    // (territory.c::would_create_mount_cycle rejects a self-mount or a cross-tree
    // oscillation), NOT merely "by construction" -- so at most PGRP_MAX_MOUNTS
    // crosses are possible. The bound remains as a defensive backstop (it would
    // stop crossing rather than spin even if a cycle somehow existed).
    for (int hops = 0; hops < PGRP_MAX_MOUNTS; hops++) {
        // RW-4 SA-F1: mount_lookup / mount_member_at return a REF-HELD source
        // (the lookup + ref are atomic under the Territory ns_lock, so a
        // concurrent unmount cannot free it mid-cross). clone_walk_zero mints an
        // INDEPENDENT crossed Spoor from it; release the transferred ref after.
        // hops==0 selects the chosen union member; deeper hops (mount-over-mount)
        // always cross the primary (member 0) of the crossed link.
        u32 mflags = 0;
        struct Spoor *src = (hops == 0)
            ? mount_member_at(p->territory, id, first_member, &mflags)
            : mount_lookup(p->territory, id, &mflags);
        if (!src) break;                          // no member at this index
        // VIVARIUM section 13: this cross went through a pheno-mount. Recorded
        // BEFORE the clone can fail, so a pheno-mount is detected even if the
        // subsequent clone_walk_zero errors (the walk then fails closed, but the
        // fact that a pheno-mount was on the path is not silently lost).
        if ((mflags & MPHENO_LINUX) && crossed_pheno) *crossed_pheno = true;
        struct Spoor *crossed = clone_walk_zero(src);
        spoor_clunk(src);                         // done with the looked-up source
        if (!crossed) {
            if (cur) spoor_clunk(cur);
            return -1;
        }
        if (cur) spoor_clunk(cur);                // keep only the latest link
        cur = crossed;
        id  = cur;                                // mount-over-mount: re-test
    }
    // #66: a crossed Spoor takes the MOUNT-POINT's namespace name -- the user
    // named `probe`'s path (e.g. /mnt), and crossing into the mounted tree keeps
    // that name, NOT the mount source's internal name. One transplant on the
    // final link (a mount-over-mount chain keeps the original mount point's
    // name). Non-load-bearing (I-33): cosmetic only.
    if (cur) spoor_path_transplant(cur, probe);
    *out = cur;
    return 0;
}

int stalk_cross_mounts(struct Proc *p, struct Spoor *probe,
                       struct Spoor **out, bool *crossed_pheno) {
    return stalk_cross_from(p, probe, 0, out, crossed_pheno);
}

// stalk_union_member (UM) -- the k-th union member of `base`, crossed + ref-held
// (see stalk.h). Pure re-export of stalk_cross_from for the union readdir merge;
// phenotype is irrelevant to readdir (NULL crossed_pheno).
int stalk_union_member(struct Proc *p, struct Spoor *base, int k,
                       struct Spoor **out) {
    return stalk_cross_from(p, base, k, out, NULL);
}

// stalk_union_has_child (UM) -- raw Dev.walk existence probe for the union
// readdir dedup (see stalk.h). Mirrors the per-component walk's three miss-path
// cleanups exactly (NULL / reuse-violation share the parent aux -> detach+unref;
// nqid mismatch reused nc -> clunk-safe). No perm_check, no symlink follow: this
// asks only whether the NAME is present, so an earlier member's entry shadows a
// later member's same-named entry regardless of access or link-ness.
bool stalk_union_has_child(struct Proc *p, struct Spoor *dir,
                           const char *name, u32 namelen) {
    (void)p;
    if (!dir || !dir->dev || !dir->dev->walk)              return false;
    if (namelen == 0 || namelen > SYS_WALK_OPEN_NAME_MAX)  return false;
    char nb[SYS_WALK_OPEN_NAME_MAX + 1];
    for (u32 i = 0; i < namelen; i++) nb[i] = name[i];
    nb[namelen] = '\0';

    struct Spoor *nc = spoor_clone(dir);
    if (!nc)                                               return false;
    const char *names[1] = { nb };
    struct Walkqid *w = dir->dev->walk(dir, nc, names, 1);
    if (!w)             { nc->aux = NULL; spoor_unref(nc);            return false; }
    if (w->spoor != nc) { walkqid_free(w); nc->aux = NULL; spoor_unref(nc); return false; }
    bool found = (w->nqid == 1);
    walkqid_free(w);
    spoor_clunk(nc);    // nc was reused (w->spoor == nc) -> clunk-safe either way
    return found;
}

// path_has_dotdot -- pre-scan for a ".." component. The POUNCE compresses a
// run of components into ONE trail entry (intermediates never materialize as
// Spoors), which is incompatible with `..`'s pop-one-component semantics --
// a pop into the middle of a pounced run has no Spoor to land on. Any ".."
// in the path therefore disables the pounce wholesale and the resolver runs
// today's per-component loop (the design's stated worst case). Toolchain paths
// in the motivating workloads are absolute and '..'-free, so the pounce
// applies to them.
//
// #83 note: the cwd join no longer collapses '..' (collapsing it popped
// components that were never walked -- see cwd_join), so a cwd-relative path
// SPELLED with '..' now takes this per-component path. That is the same cost
// an absolute path spelled with '..' has always paid, on exactly the paths
// that were previously resolving incorrectly.
static bool path_has_dotdot(const char *path, u64 pathlen) {
    u64 i = 0;
    while (i < pathlen) {
        while (i < pathlen && path[i] == '/') i++;
        u64 s = i;
        while (i < pathlen && path[i] != '/') i++;
        if (i - s == 2 && path[s] == '.' && path[s + 1] == '.') return true;
    }
    return false;
}

// path_has_trailing_slash -- POSIX 4.13: a pathname holding at least one
// non-'/' character and ending in one or more '/' characters names a
// DIRECTORY. The tokenizer collapses separator runs, so without this the
// trailing '/' is simply dropped and `/etc/passwd/` resolves the file (#82).
//
// The "at least one non-'/'" clause is why this scans back rather than testing
// the last byte: "/" and "//" have NO component before the trailing run, so
// the rule does not apply to them at all (they name the base, which the caller
// already vouched for). "" is likewise exempt.
static bool path_has_trailing_slash(const char *path, u64 pathlen) {
    if (pathlen == 0 || path[pathlen - 1] != '/') return false;
    u64 t = pathlen;
    while (t > 0 && path[t - 1] == '/') t--;
    return t > 0;   // a real component precedes the trailing separator run
}

// =============================================================================
// D-1: symlink expansion (docs/DISTRO.md section 4; refines I-28).
// =============================================================================

// Byte copy (freestanding kernel; spans bounded by SYS_OPEN_PATH_MAX).
static void sx_copy(char *dst, const char *src, u64 n) {
    for (u64 b = 0; b < n; b++) dst[b] = src[b];
}

// Lazily-allocated expansion state: a symlink-free resolution (the entire
// native boot surface) never allocates it. ~4 KiB, kmalloc'd at the FIRST
// expansion, freed on every stalk_core exit.
struct stalk_expand {
    char buf[2][SYS_OPEN_PATH_MAX + 1];   // spliced component streams (flip:
                                          // a build reads buf[cur] / the
                                          // caller's original and writes
                                          // buf[cur^1] -- never overlapping)
    char tgt[SYS_OPEN_PATH_MAX + 1];      // readlink scratch
    // The CONSUMED RECORD: the concatenation, across superseded buffers, of
    // every component span consumed before its splice point. Each recorded
    // component was PHYSICALLY WALKED by this resolution when it was
    // consumed, so re-resolving the record reproduces the walk (a restart
    // races concurrent renames exactly as two sequential resolutions would).
    // Appended at each splice; reset on restart (the rebuilt path
    // re-consumes) and on re-anchor (the position is the root -- nothing
    // before it survives).
    char consumed[SYS_OPEN_PATH_MAX + 1];
    u64  consumed_len;
    u64  buflen;                 // length of buf[cur]
    int  cur;                    // which buf[] the resolver's `path` aliases
    int  follows;                // expansions so far (bound: STALK_MAX_FOLLOWS)
    struct Spoor *owned_base;    // absolute re-anchor: ref-held Territory root
};

static void stalk_expand_free(struct stalk_expand *ex) {
    if (!ex) return;
    if (ex->owned_base) spoor_clunk(ex->owned_base);
    kfree(ex);
}

// stalk_expand_link -- read `link`'s target and splice it into the component
// stream. Caller: the per-component arm, on a walked QTSYMLINK Spoor whose
// Dev has a readlink slot and whose disposition says FOLLOW; the caller
// clunks `link` afterward regardless of outcome. `s` / `i` delimit the link
// component in the CURRENT buffer (s = its first byte; i = just past it).
//
// Returns:
//    0 : in-place splice. ex->buf[ex->cur] = target ++ remaining; the caller
//        continues the SAME resolution (trail intact) at i = 0.
//    1 : RESTART. ex->buf[ex->cur] = the full rebuilt path; the caller
//        unwinds the trail and re-enters from *basep. Taken for an ABSOLUTE
//        target (re-anchored at the caller's own Territory root -- I-28:
//        never a global root, so a confined Proc's /bin/sh -> /bin/busybox
//        resolves inside its container BY CONSTRUCTION, not by call-site
//        audit; the restart is FREE of re-walk cost, since the rebuilt path
//        is exactly target ++ remaining resolved from the anchor) and for a
//        '..'-BEARING RELATIVE target (the load-bearing case: a '..' pop
//        must land on a 1:1 trail entry, and only a fresh resolution --
//        pounce disabled by the '..' now in the path -- guarantees the whole
//        trail is 1:1; the soundness argument lives at the '..' arm).
//   -1 : failure; *errp set (T_E_LOOP at the bound, T_E_NOMEM, T_E_INVAL on
//        overflow / a NUL-bearing target, a propagated readlink errno).
static int stalk_expand_link(struct Proc *p, struct stalk_expand **exp,
                             struct Spoor *link,
                             const char *path, u64 pathlen, u64 s, u64 i,
                             struct Spoor **basep, int *errp) {
    struct stalk_expand *ex = *exp;
    if (!ex) {
        ex = kmalloc(sizeof(*ex), 0);
        if (!ex) { *errp = T_E_NOMEM; return -1; }
        ex->consumed_len = 0;
        ex->buflen       = 0;
        ex->cur          = 0;
        ex->follows      = 0;
        ex->owned_base   = NULL;
        *exp = ex;
    }
    if (++ex->follows > STALK_MAX_FOLLOWS) { *errp = T_E_LOOP; return -1; }

    // Bound the return BEFORE it is used as one. `tlen` indexes ex->tgt in the
    // NUL scan and path_has_dotdot below, and both run before the splice-time
    // length checks -- so a Dev returning more than it was given would read
    // past the buffer (and past the kmalloc'd struct) at those two sites, not
    // at the splice. Every other vtable contract in this file is defended the
    // same way (the reuse-nc rule, the walk shape check, the ATTRS_UNSUPPORTED
    // sentinel re-check), all of them "unreachable" for the in-tree Devs;
    // .readlink is a public NULL-permitted slot and gets the same treatment.
    long tlen = link->dev->readlink(link, ex->tgt, SYS_OPEN_PATH_MAX);
    if (tlen <= 0 || tlen > SYS_OPEN_PATH_MAX) {
        *errp = (tlen < 0) ? err_code((int)tlen)
                           : ((tlen == 0) ? T_E_NOENT : T_E_INVAL);
        return -1;
    }
    // A hostile server can put arbitrary bytes in the target string. The
    // resolver is length-delimited, but Dev.walk's namebuf is NUL-terminated
    // -- an embedded NUL would silently truncate a component into a DIFFERENT
    // name. Fail closed (the same class as the handlers' NUL-in-path reject).
    for (long b = 0; b < tlen; b++)
        if (ex->tgt[b] == '\0') { *errp = T_E_INVAL; return -1; }

    bool  tgt_abs = (ex->tgt[0] == '/');
    bool  tgt_dd  = path_has_dotdot(ex->tgt, (u64)tlen);
    char *nb      = ex->buf[ex->cur ^ 1];
    u64   rem     = pathlen - i;         // unconsumed tail, incl. leading '/'

    if (tgt_abs) {
        // Absolute: re-anchor at the caller's own Territory root + restart.
        // No territory (a synthetic test Proc) -> no root to anchor at ->
        // fail closed.
        if (!p || !p->territory) { *errp = T_E_IO; return -1; }
        if (!ex->owned_base) {
            // The RW-4 SA-F1 accessor: read + ref atomic under ns_lock, so a
            // concurrent pivot_root cannot free the root mid-expansion. Held
            // for the rest of the resolution (later absolute targets reuse
            // it), released at stalk_expand_free.
            ex->owned_base = territory_root_ref(p->territory);
            if (!ex->owned_base) { *errp = T_E_IO; return -1; }
        }
        *basep = ex->owned_base;
        ex->consumed_len = 0;
        if ((u64)tlen + rem > SYS_OPEN_PATH_MAX) { *errp = T_E_INVAL; return -1; }
        sx_copy(nb, ex->tgt, (u64)tlen);
        sx_copy(nb + tlen, path + i, rem);
        ex->cur   ^= 1;
        ex->buflen = (u64)tlen + rem;
        return 1;
    }

    if (tgt_dd) {
        // Relative with '..': rebuild the FULL path -- the consumed record +
        // the current buffer's consumed prefix + target + remaining -- and
        // restart. path[0..s) ends at a component boundary (path[s-1] is '/'
        // or s == 0), so the chunks self-separate.
        u64 need = ex->consumed_len + s + (u64)tlen + rem;
        if (need > SYS_OPEN_PATH_MAX) { *errp = T_E_INVAL; return -1; }
        sx_copy(nb, ex->consumed, ex->consumed_len);
        sx_copy(nb + ex->consumed_len, path, s);
        sx_copy(nb + ex->consumed_len + s, ex->tgt, (u64)tlen);
        sx_copy(nb + ex->consumed_len + s + (u64)tlen, path + i, rem);
        ex->consumed_len = 0;
        ex->cur   ^= 1;
        ex->buflen = need;
        return 1;
    }

    // Relative, '..'-free: record the consumed prefix, splice in place.
    if (ex->consumed_len + s > SYS_OPEN_PATH_MAX) { *errp = T_E_INVAL; return -1; }
    sx_copy(ex->consumed + ex->consumed_len, path, s);
    ex->consumed_len += s;
    if ((u64)tlen + rem > SYS_OPEN_PATH_MAX) { *errp = T_E_INVAL; return -1; }
    sx_copy(nb, ex->tgt, (u64)tlen);
    sx_copy(nb + tlen, path + i, rem);
    ex->cur   ^= 1;
    ex->buflen = (u64)tlen + rem;
    return 0;
}

// stalk() -- the errp==NULL convenience wrapper (the common, errno-agnostic
// API the bulk of callers + the test suite use). The errno-rollout consumers
// (SYS_OPEN) call stalk_err() directly.
struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode) {
    return stalk_err(p, start, path, pathlen, amode, omode, NULL);
}

// stalk_union_child (UM) -- resolve component `namebuf` in the UNION at
// `union_base` (a pre-cross mount point with >= 2 members): try each member in
// declared order, crossing it to its leaf (mount-over-mount followed via
// stalk_cross_from), then per-member X-search + directory-type + walk; return
// the FIRST member's resolved child (ref-held, its qid set), or NULL when no
// member has a searchable entry for the name. Plan 9 union skip semantics: a
// member that is not a directory, denies X-search, or lacks the component is
// SKIPPED (not an error) -- unlike a lone directory, where an X denial is
// EACCES; only exhausting all members is a miss (the caller reports ENOENT).
// *errp is set ONLY on a hard error (a member cross failure / OOM); an ordinary
// all-miss leaves *errp 0. The phenotype of the WINNING member (only) is OR'd
// into *crossed_pheno -- a losing member's Linux mount must not stamp a
// resolution that landed elsewhere.
static struct Spoor *stalk_union_child(struct Proc *p, struct Spoor *union_base,
                                       const char *namebuf,
                                       bool *crossed_pheno, int *errp) {
    *errp = 0;
    for (int k = 0; k < PGRP_MAX_MOUNTS; k++) {
        // Per-member phenotype: accumulate locally, propagate only on a WIN.
        bool member_pheno = false;
        struct Spoor *leaf = NULL;
        if (stalk_cross_from(p, union_base, k, &leaf, &member_pheno) < 0) {
            *errp = T_E_IO;              // a member cross failed -> fail closed
            return NULL;
        }
        if (!leaf) break;               // no member at index k -- members exhausted
        // Skip a non-directory member (a union is over directories).
        if (!(leaf->qid.type & QTDIR)) { spoor_clunk(leaf); continue; }
        // Per-member X-search: a member that denies search is SKIPPED (Plan 9
        // union semantics), not an EACCES failure.
        if (leaf->dev && leaf->dev->perm_enforced) {
            struct t_stat st;
            int sr = spoor_stat_native(leaf, &st);
            if (sr != 0 || perm_check(p, &st, PERM_X) != 0) {
                spoor_clunk(leaf); continue;
            }
        }
        if (!leaf->dev || !leaf->dev->walk) { spoor_clunk(leaf); continue; }
        // Walk the component in this member. Cleanup mirrors the non-union arm's
        // three miss cases exactly (NULL -> shares aux; reuse-violation ->
        // shares aux; nqid!=1 -> reused, clunk-safe).
        struct Spoor *nc = spoor_clone(leaf);
        if (!nc) { spoor_clunk(leaf); *errp = T_E_NOMEM; return NULL; }
        const char *names[1] = { namebuf };
        struct Walkqid *w = leaf->dev->walk(leaf, nc, names, 1);
        if (!w) { nc->aux = NULL; spoor_unref(nc); spoor_clunk(leaf); continue; }
        if (w->spoor != nc) {
            walkqid_free(w); nc->aux = NULL; spoor_unref(nc); spoor_clunk(leaf); continue;
        }
        if (w->nqid != 1) {
            walkqid_free(w); spoor_clunk(nc); spoor_clunk(leaf); continue;
        }
        walkqid_free(w);
        spoor_clunk(leaf);              // done with the member root
        if (member_pheno && crossed_pheno) *crossed_pheno = true;  // winner only
        return nc;                     // HIT: first member with the component
    }
    return NULL;                       // all members exhausted -> caller: ENOENT
}

// stalk_core -- the resolver body. stat_out/stat_done are the STALK_STAT
// walk-query sink (stalk_stat only): when the final run resolves through
// Dev.walk_attrs and the leaf is clean, the core fills *stat_out, sets
// *stat_done, unwinds, and returns NULL WITHOUT touching *errp -- no quarry
// Spoor ever exists. Every other caller passes NULL/NULL.
static struct Spoor *stalk_core(struct Proc *p, struct Spoor *start,
                                const char *path, u64 pathlen,
                                int amode, u32 omode, int *errp,
                                struct t_stat *stat_out, bool *stat_done,
                                bool *crossed_pheno) {
    if (!start || !path) { if (errp) *errp = T_E_INVAL; return NULL; }
    // Reject an unknown amode LOUDLY rather than silently degrading to walk-only
    // (stalk-1 audit F1). stalk-2 adds STALK_MOUNT; POUNCE adds STALK_STAT; D-1
    // adds the STALK_NOFOLLOW FLAG (the one admitted flag bit); any future
    // amode/flag must be added here AND given its dispatch arms below -- a
    // missed arm must fail-closed, not skip an open / a cross / a follow.
    bool nofollow_req = (amode & STALK_NOFOLLOW) != 0;
    if ((amode & ~(STALK_AMODE_MASK | STALK_NOFOLLOW)) != 0)
        { if (errp) *errp = T_E_INVAL; return NULL; }
    amode &= STALK_AMODE_MASK;
    if (amode != STALK_WALK && amode != STALK_OPEN && amode != STALK_MOUNT &&
        amode != STALK_STAT)
        { if (errp) *errp = T_E_INVAL; return NULL; }
    // D-1: the mount POINT is never followed (STALK_MOUNT's no-cross-final
    // precedent extends to no-follow-final -- SYS_MOUNT names the link's own
    // identity; DISTRO.md section 4.3).
    if (amode == STALK_MOUNT) nofollow_req = true;

    struct Spoor *trail[STALK_MAX_DEPTH];
    int           depth  = 0;
    struct Spoor *quarry = NULL;
    // Errno-rollout: the cause of a NULL return, written to *errp at `fail`.
    // Set precisely before each `goto fail` (T_E_NOENT walk-miss, T_E_ACCES
    // perm denial, T_E_INVAL structural, propagated for a stat/open failure);
    // T_E_IO is the generic default for an OOM / transport / cross failure.
    int           err    = T_E_IO;

    // D-1: the resolution BASE. `start` (borrowed) until an absolute symlink
    // target re-anchors at the caller's own Territory root (then the expand
    // state's ref-held owned_base). Everything that used to read `start` as
    // the depth-0 position reads `base`.
    struct Spoor        *base = start;
    struct stalk_expand *ex   = NULL;

    // UM (union mounts): when the directory being searched is a union mount
    // point (>= 2 grafted members), `union_base` holds a ref to the pre-cross
    // mount-point Spoor so the per-component walk can iterate its members in
    // declared order (stalk_union_child). Set at the descent cross when a
    // second member is detected; POUNCE is disabled for that component; the
    // per_component union branch consumes (clunks) it. NULL for the common
    // single-source path. Clunked at `fail` if a goto slips past the consume.
    struct Spoor        *union_base = NULL;

    // POUNCE state (docs/POUNCE-DESIGN.md §5). `carried` holds the current
    // trail tip's attrs when they arrived fused with the walk that produced it
    // (the previous run's leaf record) -- the next run's base X-check and the
    // final-hop R/W check consume it instead of a fresh stat_native RPC.
    // Invalidated whenever the tip stops being the Spoor the record describes:
    // a cross (mounted root != mount point), a '..' pop, an old-path hop
    // (plain walk fetches no attrs). `logical_depth` counts REAL components
    // consumed -- runs compress the trail array, so `depth` alone would let a
    // pounced path exceed the STALK_MAX_DEPTH surface the per-component loop
    // enforces; monotone because pounce_ok excludes '..' (the only pop).
    // Declared WITHOUT initializers: the D-1 restart re-enters at the label
    // below and every path-derived field recomputes there.
    bool          pounce_ok;
    struct t_stat carried;
    bool          carried_valid;
    int           logical_depth;
    bool          trailing_slash;
    bool          nofollow_final;
    bool          pounce_skip_one;
    u64           i;

    // D-1 restart: a symlink expansion that re-anchors (absolute target) or
    // rebuilds the path (a '..'-bearing target) unwinds the trail and
    // re-enters HERE, with `path`/`pathlen` aliasing the expand state's
    // rebuilt buffer and `base` possibly re-anchored. All path-derived state
    // recomputes from the new buffer; trail/depth were reset by the jumper.
restart:
    pounce_ok       = !path_has_dotdot(path, pathlen);
    carried_valid   = false;
    logical_depth   = 0;
    // D-1: the pounce resumes ONE component per-component after a symlink
    // split (the split's resumed component must not re-gather -- it would
    // re-walk the same run and split again, forever). Set only by the two
    // split-for-expansion sites; cleared the moment a component reaches the
    // per-component arm.
    pounce_skip_one = false;
    // #82: does this path end in a separator, asserting a directory? Computed
    // from the RAW current buffer -- the tokenizer below drops the trailing
    // run, so by the time resolution finishes nothing remembers it was there.
    trailing_slash  = path_has_trailing_slash(path, pathlen);
    // D-1: a trailing slash FORCES following even for a no-follow caller
    // (POSIX 4.13 -- "link/" names the directory the link resolves to;
    // measured Linux behavior, O_NOFOLLOW included).
    nofollow_final  = nofollow_req && !trailing_slash;
    i               = 0;

    // VIVARIUM section 13: the phenotype is a property of the FINAL resolved
    // binary's location, not of any abandoned prefix. A symlink re-anchor (an
    // absolute target, or a '..'-rebuild) jumps here having unwound the trail;
    // the crossing accumulated by the DISCARDED prefix must be discarded with
    // it, so the restarted resolution re-derives the crossing from scratch (the
    // base-cross below + the per-component walk). Without this, an absolute
    // symlink INSIDE a pheno-mount (`/viv/bin/helper -> /bin/ut`) would run the
    // NATIVE target under the Linux phenotype -- I-43-safe (over-declaration
    // degrades to malfunction, never escalation) but a violation of the stated
    // contract "the SAME file reached by another path is native" (territory.h).
    //
    // Design D (VIVARIUM 13.10.5): the reset is a SEED, not a false. The
    // resolving Territory's own declaration (territory_root_pheno -- the
    // container's, since chroot swaps root_spoor and no crossing can ever fire
    // from inside) is the floor every pass starts from; walk crossings
    // accumulate on top. Seeded HERE, at the shared restart: label, so one line
    // covers the first pass AND every re-anchor -- and it must stay here: a
    // seed hoisted above the label (first pass only) would let an absolute
    // symlink inside a container drop the declaration and revert its target to
    // native. Both outcomes are preserved: in the user namespace (seed false)
    // the /viv/bin/helper -> /bin/ut case above still lands native; in a
    // container (seed true) it stays Linux.
    if (crossed_pheno) *crossed_pheno = territory_root_pheno(p ? p->territory : NULL);

    // Cross the BASE: `base` itself may be a mount point. If it crosses, the
    // owned crossed clone becomes trail[0] (so the first component searches the
    // mounted root, X-checked like any parent). If not, the base stays `base`
    // (borrowed, or the ref-held re-anchor root) and depth stays 0. Either way
    // we cannot cross it in place; the crossed clone goes on the trail instead.
    {
        struct Spoor *crossed = NULL;
        if (stalk_cross_mounts(p, base, &crossed, crossed_pheno) < 0) goto fail;
        if (crossed) trail[depth++] = crossed;
    }

    while (i < pathlen) {
        // Skip the separator run (collapses a leading '/' and any '//').
        while (i < pathlen && path[i] == '/') i++;
        if (i >= pathlen) break;
        u64 s = i;
        while (i < pathlen && path[i] != '/') i++;
        u64 clen = i - s;   // component is path[s .. i)

        // "." -- a no-op (stay at the current directory), but only if there IS
        // a directory to stay in (#81; see stalk_tip_is_dir).
        if (clen == 1 && path[s] == '.') {
            if (!stalk_tip_is_dir(trail, depth, base))
                                                 { err = T_E_NOTDIR; goto fail; }
            // #84: '.' is a lookup performed IN the tip, so it needs X there.
            // Type first (a non-directory is ENOTDIR, never EACCES).
            int se = stalk_tip_may_search(p, trail, depth, base,
                                          &carried, carried_valid);
            if (se != 0)                         { err = se;         goto fail; }
            continue;
        }

        // ".." -- pop the trail. Contained at `base`: at the bottom (depth 0)
        // this is a no-op, so resolution can never escape above the base (the
        // chroot/pivot boundary -- I-28). The popped clone is clunk-safe (it
        // owns its fid: a walked child or a crossed clone).
        //
        // D-1 SOUNDNESS: a pop is 1:1 -- it lands exactly one component up --
        // ONLY when no trail entry compresses a pounced run. That holds here
        // BY CONSTRUCTION: a '..' in the CURRENT buffer implies pounce_ok has
        // been false since this buffer started resolving (the entry
        // recompute at `restart`, or the original-path pre-scan; an in-place
        // symlink splice only ever ADDS a '..'-free target, and its
        // pounce_ok update is sticky-AND). pounce_ok false means every entry
        // pushed from this buffer is per-component (1:1), and a restart
        // unwound whatever compressed entries an earlier buffer pushed. So
        // compressed entries and '..' pops can NEVER coexist -- which is
        // exactly why a '..'-bearing symlink TARGET forces the restart
        // instead of an in-place splice (stalk_expand_link).
        //
        // #81 gates the pop on the tip being a directory. This STRENGTHENS
        // I-28 rather than touching it: the gate can only turn a resolution
        // that used to succeed into a failure, never move a pop further up, so
        // no path that previously stopped at `start` can now pass it. At depth
        // 0 the subject is `start` itself -- an `openat()` on a non-directory
        // fd answers ENOTDIR instead of handing back the file.
        if (clen == 2 && path[s] == '.' && path[s + 1] == '.') {
            if (!stalk_tip_is_dir(trail, depth, base))
                                                 { err = T_E_NOTDIR; goto fail; }
            // #84: the pop is a lookup of ".." performed IN the tip, so it
            // needs X on the directory being popped OUT of -- checked BEFORE
            // the pop, while that directory is still the subject. Containment
            // is untouched: the gate can only turn a success into a failure.
            int se = stalk_tip_may_search(p, trail, depth, base,
                                          &carried, carried_valid);
            if (se != 0)                         { err = se;         goto fail; }
            if (depth > 0) spoor_clunk(trail[--depth]);
            carried_valid = false;   // hygiene; unreachable while pounce_ok
            continue;
        }

        // A real component. Bound its length (the Dev.walk vtable takes a
        // NUL-terminated name; an over-long component is rejected, not
        // truncated).
        if (clen > SYS_WALK_OPEN_NAME_MAX) { err = T_E_INVAL; goto fail; }

        // The directory we are about to search. CROSS IT ON DESCENT: if the
        // trail tip is a mount point, replace it in place with the mounted root
        // so we walk INTO the mounted tree and X-check the mounted root (not the
        // shadowed mount point). The base case (depth==0, parent==start) was
        // already proven not-a-mount by the base cross above.
        struct Spoor *parent;
        if (depth > 0) {
            // UM: detect a UNION (>= 2 members) at this point. mount_member_at(_,1)
            // != NULL means a second member exists. For a union we do NOT cross
            // here -- we leave the mount POINT as the trail tip (so a later '..'
            // lands on the union point and the recorded parent stays accurate)
            // and let the per_component union branch cross each member via
            // stalk_union_child. union_base refs the mount-point Spoor. The mount
            // point is itself a directory, so the #79 QTDIR check below passes.
            bool is_union = false;
            if (p && p->territory) {
                struct Spoor *m1 = mount_member_at(p->territory, trail[depth - 1],
                                                   1, NULL);
                if (m1) {
                    spoor_clunk(m1);
                    is_union    = true;
                    union_base  = trail[depth - 1];
                    spoor_ref(union_base);
                }
            }
            if (!is_union) {
                // Common single-source path: cross the tip in place to the
                // mounted root so we walk INTO the mounted tree.
                struct Spoor *crossed = NULL;
                if (stalk_cross_mounts(p, trail[depth - 1], &crossed, crossed_pheno) < 0) goto fail;
                if (crossed) {
                    spoor_clunk(trail[depth - 1]);
                    trail[depth - 1] = crossed;
                    carried_valid = false;   // the tip changed; the record
                                             // described the mount point, not
                                             // the mounted root
                }
            }
            parent = trail[depth - 1];
        } else {
            parent = base;
        }

        // #79: the thing we are about to search must BE a directory. Without
        // this, resolution walked a real component out of a file and reported
        // the Dev's miss as T_E_NOENT -- so `/bin/ls/foo` answered "no such
        // file" instead of ENOTDIR, and `os.IsNotExist` (the errno a build
        // tool branches on to decide "safe to create") read TRUE for a path
        // that can never exist. Note this is the resolver computing the answer
        // itself, NOT transporting one: a Dev.walk returns `struct Walkqid *`
        // with no errno channel, so a Dev's own ENOTDIR could not have reached
        // EL0 even if it sent one.
        //
        // BEFORE the X-search check below, deliberately: the x bit on a
        // non-directory is meaningless for traversal, so gating on it first
        // would answer EACCES for a 0644 file and ENOTDIR for a 0755 one --
        // the errno would turn on a bit that has no bearing on the question.
        // This ordering discloses nothing new: reaching the gate already
        // required X on every ancestor, and a plain stat() of `parent` (which
        // needs only that same ancestor X) already reveals its type.
        //
        // qid.type is the signal because it is TOTAL -- every Spoor carries
        // one, with no fetch and no RPC -- and because it is what dev9p.c
        // already says it is for ("the kernel-side QT* superset only
        // distinguishes DIR vs FILE for walk-time directory checks"). QTFILE
        // is 0x00, so a Dev that never sets the field reads as "file", which
        // is correct for the leaf Devs (null/zero/full/random/devnotes/pipe)
        // that have no hierarchy to walk. Every Dev that DOES mint directory
        // Spoors sets QTDIR, and spoor_clone copies qid -- so a mount cross
        // and a 0-element clone-walk both preserve directory-ness.
        if (!(parent->qid.type & QTDIR))  { err = T_E_NOTDIR; goto fail; }

        // =====================================================================
        // POUNCE (docs/POUNCE-DESIGN.md §5) -- batch the maximal run of
        // consecutive real components through ONE Dev.walk_attrs call (one wire
        // RPC on dev9p), then enforce the per-component X-search + mount scan
        // in a LEFT-TO-RIGHT post-scan whose outcome is byte-identical to the
        // per-component loop below (the fail-ordering invariant, §6).
        // Disabled when the path contains '..' (pounce_ok) or the Dev lacks
        // the slot -- those take the per-component loop unchanged.
        // =====================================================================
        if (pounce_ok && !pounce_skip_one && !union_base && parent->dev &&
            parent->dev->walk_attrs && logical_depth < STALK_MAX_DEPTH) {
            // -- Gather the run. The already-tokenized component is names[0];
            // keep consuming real components (a '.' / '..' / over-long token
            // ends the run and is LEFT for the outer loop, preserving its
            // existing disposition + ordering). ends[j] = the scan position
            // just past component j -- the split-resume point.
            const char  *names[DEV_WALK_ATTRS_MAX];
            size_t       lens [DEV_WALK_ATTRS_MAX];
            u64          ends [DEV_WALK_ATTRS_MAX];
            int          nrun = 1;
            names[0] = &path[s]; lens[0] = (size_t)clen; ends[0] = i;
            int budget = STALK_MAX_DEPTH - logical_depth;
            if (budget > DEV_WALK_ATTRS_MAX) budget = DEV_WALK_ATTRS_MAX;
            while (nrun < budget) {
                u64 save = i;
                while (i < pathlen && path[i] == '/') i++;
                if (i >= pathlen) { i = save; break; }
                u64 s2 = i;
                while (i < pathlen && path[i] != '/') i++;
                u64 cl2 = i - s2;
                bool tok_dot    = (cl2 == 1 && path[s2] == '.');
                bool tok_dotdot = (cl2 == 2 && path[s2] == '.' &&
                                   path[s2 + 1] == '.');
                if (tok_dot || tok_dotdot || cl2 > SYS_WALK_OPEN_NAME_MAX) {
                    i = save;   // not part of the run; the outer loop owns it
                    break;
                }
                names[nrun] = &path[s2];
                lens [nrun] = (size_t)cl2;
                ends [nrun] = i;
                nrun++;
            }
            bool final_run;
            {
                u64 peek = i;
                while (peek < pathlen && path[peek] == '/') peek++;
                final_run = (peek >= pathlen);
            }

            // -- Base X-check: one stat_native per run, or the previous run's
            // carried leaf record (both server-fresh samples of THIS
            // resolution; the same unsynchronized-snapshot TOCTOU shape as the
            // per-component loop -- §6).
            if (parent->dev->perm_enforced) {
                struct t_stat pst;
                if (carried_valid) {
                    pst = carried;
                } else {
                    int sr = spoor_stat_native(parent, &pst);
                    if (sr != 0)                         { err = err_code(sr); goto fail; }
                }
                if (perm_check(p, &pst, PERM_X) != 0)    { err = T_E_ACCES;    goto fail; }
            }

            struct t_stat sts[DEV_WALK_ATTRS_MAX];

            // ================================================================
            // FID-LIFECYCLE cached-open (docs/FID-LIFECYCLE-DESIGN.md §3.3):
            // the FINAL run of a plain read-only STALK_OPEN (omode == 0
            // exactly -- OTRUNC / write / OEXEC / O_PATH never take it) may be
            // served as a FIDLESS open. The Dev revalidates server-fresh (a
            // forced-wire query walk -- no fid binds on either end; on a B1
            // LOOSE client [the I-38 per-attach opt-in, docs/chase/B1-VOTE.md]
            // the records may instead be cache-served -- see dev9p_open_cached
            // step 2), snapshots the fully-page-cached content, and returns an
            // OPENED Spoor plus the walk's records in sts. The RESOLVER keeps every
            // permission decision: the same left-to-right X + mount post-scan
            // as the batched walk below, then the final-hop R/W gate --
            // identical fail ordering (§6). ANY mount hit (including the leaf
            // -- this path has no quarry-cross) discards the cached open and
            // falls back to the normal walk, whose split/cross machinery owns
            // the crossing. A NULL return reveals nothing: the observable
            // outcome is the normal path's.
            // ================================================================
            if (amode == STALK_OPEN && omode == 0 && final_run &&
                parent->dev->open_cached) {
                struct Spoor *co = parent->dev->open_cached(
                        parent, (const char *const *)names, lens, nrun, sts);
                if (co) {
                    bool co_mount = false;
                    for (int j = 0; j < nrun; j++) {
                        if (j > 0 && parent->dev->perm_enforced &&
                            perm_check(p, &sts[j - 1], PERM_X) != 0) {
                            spoor_clunk(co);   // wire-free destroy
                            err = T_E_ACCES;
                            goto fail;
                        }
                        if (p && p->territory &&
                            mount_is_point_id(p->territory, parent->dc,
                                              parent->devno,
                                              sts[j].qid_path)) {
                            co_mount = true;
                            break;
                        }
                        // D-1: any symlink in the run -- the cached open
                        // resolved through/at a link the resolver must
                        // expand. Discard + fall back to the normal walk
                        // (which owns expansion), exactly like the mount
                        // case. Rarely reachable: a MID-run link makes the
                        // Dev's internal query walk partial (-> NULL), so
                        // this catches the full-walk LEAF-link shape.
                        if (sts[j].qid_type & QTSYMLINK) {
                            co_mount = true;
                            break;
                        }
                    }
                    if (!co_mount) {
                        // (Exit parity: this return + the query return + the
                        // two tail exits all release the D-1 expand state.)
                        // #82: this exit never reaches the quarry gate below,
                        // so re-state it here on the leaf's fused record. That
                        // record IS the quarry: the scan above proved no
                        // component -- the leaf included -- is a mount point,
                        // so no cross is owed and crossed == uncrossed. Before
                        // the R/W check, the same type-before-permission order
                        // the quarry gate uses.
                        if (trailing_slash && !(sts[nrun - 1].qid_type & QTDIR)) {
                            spoor_clunk(co);
                            err = T_E_NOTDIR;
                            goto fail;
                        }
                        if (parent->dev->perm_enforced &&
                            perm_check(p, &sts[nrun - 1],
                                       perm_want_for_omode(omode)) != 0) {
                            spoor_clunk(co);
                            err = T_E_ACCES;
                            goto fail;
                        }
                        stalk_unwind(trail, depth);
                        stalk_expand_free(ex);
                        return co;
                    }
                    spoor_clunk(co);   // a mount in the run: the normal path
                                       // (split + cross) owns the crossing
                }
            }

            // -- The batched walk. STALK_STAT's FINAL run is the walk-QUERY
            // (nc == NULL -> dev9p sends newfid = P9_NOFID): the leaf's attrs
            // return in the reply and no Spoor/fid ever exists. Every other
            // run binds (the leaf becomes the next trail entry / the quarry).
            bool query = (amode == STALK_STAT) && final_run &&
                         (stat_out != NULL) && (stat_done != NULL);
            struct Spoor *nc = NULL;
            if (!query) {
                if (depth >= STALK_MAX_DEPTH)            { err = T_E_INVAL; goto fail; }
                nc = spoor_clone(parent);
                if (!nc) goto fail;
            }
            struct Walkqid *w = parent->dev->walk_attrs(parent, nc, names, lens,
                                                        nrun, sts);
            if (w == DEV_WALK_ATTRS_UNSUPPORTED) {
                // This SESSION's server does not implement the fused op
                // (dev9p latches the first ENOSYS; the sentinel then returns
                // RPC-free). Not a walk failure -- nothing about the path was
                // learned. Release the untouched clone, rewind the gather to
                // just past the first component, and resolve per-component:
                // the outer loop re-tokenizes the rest, and each subsequent
                // iteration's walk_attrs returns the sentinel instantly, so
                // the whole resolution degrades to today's loop.
                if (nc) { nc->aux = NULL; spoor_unref(nc); }
                i = ends[0];
                goto per_component;
            }
            if (!w) {
                // Failed at the FIRST component (dev9p Rlerror / transport /
                // OOM): nothing bound, nc untouched (contract). The base was
                // X-checked above, so NOENT is correctly ordered -- the same
                // disposition as the per-component loop's walk-NULL arm.
                if (nc) { nc->aux = NULL; spoor_unref(nc); }
                err = T_E_NOENT;
                goto fail;
            }
            int  k     = w->nqid;
            bool full  = (k == nrun);
            bool bound = (nc != NULL && full && w->spoor == nc);
            // Valid shapes (the sharpened contract): a full BIND walk MUST
            // have transitioned nc (w->spoor == nc); everything else --
            // partial, or the query form -- MUST carry w->spoor == NULL. A
            // full bind walk with spoor == NULL is a VIOLATION (pushing the
            // untransitioned nc would later clunk the parent's shared fid).
            bool shape_ok = (nc && full) ? bound : (w->spoor == NULL);
            if (k < 0 || k > nrun || !shape_ok) {
                // Contract violation (defense-in-depth; dev9p + devramfs honor
                // the sharpened reuse-nc rule).
                bool was_bound = (nc && w->spoor == nc);
                walkqid_free(w);
                if (nc) {
                    if (was_bound) spoor_clunk(nc);
                    else { nc->aux = NULL; spoor_unref(nc); }
                }
                goto fail;
            }

            // -- Fail-ordering post-scan, LEFT-TO-RIGHT (§6). Component j's
            // parent X-gate: the base was checked pre-batch; thereafter
            // component j-1's own fused record (sts[j-1]) vouches for j. An
            // X-denial MASKS everything deeper -- including a partial walk's
            // miss -- so a caller cannot probe existence under a forbidden
            // directory (T_E_ACCES, never T_E_NOENT). Interleaved with the
            // mount scan in the SAME order as the per-component loop (a
            // component's mount test precedes its own X-check; its X-check
            // precedes the next component's consumption).
            int split_at = -1;
            int link_at  = -1;
            for (int j = 0; j < k; j++) {
                if (j > 0 && parent->dev->perm_enforced &&
                    perm_check(p, &sts[j - 1], PERM_X) != 0) {
                    walkqid_free(w);
                    if (bound) spoor_clunk(nc);
                    else if (nc) { nc->aux = NULL; spoor_unref(nc); }
                    err = T_E_ACCES;
                    goto fail;
                }
                // Mount membership of walked component j (identity = the run
                // parent's (dc, devno) + the reply qid; batch intermediates
                // are never materialized as Spoors). A hit means the batch
                // walked PAST a mount point server-side, so everything past j
                // -- including a partial walk's miss verdict -- is the
                // UNDERLYING tree's answer: split the run at j. EXCEPT the
                // leaf of a full BIND walk, where nc IS the mount point and
                // the existing machinery (cross-on-descent when it becomes a
                // parent; the quarry cross; STALK_MOUNT's no-cross) already
                // handles it exactly as today.
                if (p && p->territory &&
                    mount_is_point_id(p->territory, parent->dc, parent->devno,
                                      w->qid[j].path)) {
                    if (!(bound && j == nrun - 1)) { split_at = j; break; }
                }
                // D-1: a symlink among the walked components (mount tested
                // FIRST for the same j -- a mount over the link's identity
                // shadows it, matching the per-component arm's order). A
                // FOLLOWED link splits the run at its PARENT and resumes at
                // the link component per-component (pounce_skip_one) -- the
                // per-component arm owns expansion, ONE site. A no-followed
                // FINAL leaf does NOT split: the bound leaf / query record
                // IS the quarry (the lstat / O_NOFOLLOW shape), handled by
                // the normal exits below. This test also covers a PARTIAL
                // walk whose deepest step is a link (the server stops AT a
                // link -- Stratum walks by dirent lookup, never following):
                // j == k-1 < nrun-1 there, so it reads as mid-path = follow,
                // and the miss verdict past the link never masquerades as
                // ENOTDIR/ENOENT.
                if (w->qid[j].type & QTSYMLINK) {
                    bool leaf_final = final_run && (j == nrun - 1);
                    if (!(leaf_final && nofollow_final)) { link_at = j; break; }
                }
            }

            if (split_at >= 0) {
                // Split: discard the batch (a bound leaf lives in the WRONG
                // tree -- past the mount point), materialize a Spoor AT the
                // mount point by re-walking the validated prefix [0..split_at]
                // (one extra RPC; mid-path crossings are rare and mount points
                // stable), push it, and resume the outer loop after component
                // split_at. The next iteration's cross-on-descent (or the
                // quarry cross) then crosses it and X-checks the MOUNTED root
                // -- today's exact semantics.
                walkqid_free(w);
                if (bound) spoor_clunk(nc);
                else if (nc) { nc->aux = NULL; spoor_unref(nc); }
                if (depth >= STALK_MAX_DEPTH)            { err = T_E_INVAL; goto fail; }
                struct Spoor *mc = spoor_clone(parent);
                if (!mc) goto fail;
                // Reuse sts -- the consumed prefix's records are spent.
                struct Walkqid *rw = parent->dev->walk_attrs(parent, mc,
                                                             names, lens,
                                                             split_at + 1, sts);
                if (rw == DEV_WALK_ATTRS_UNSUPPORTED) {
                    // Unreachable with the one-way per-session latch (the
                    // batch above just SUCCEEDED on this session), but the
                    // sentinel is a static object -- it must never reach
                    // walkqid_free. Fail closed.
                    mc->aux = NULL;
                    spoor_unref(mc);
                    goto fail;
                }
                if (!rw || rw->nqid != split_at + 1 || rw->spoor != mc) {
                    // The tree changed between the two walks (racing
                    // unlink/rename) -- fail as a sequential resolution racing
                    // the same mutation would.
                    if (rw) {
                        bool rbound = (rw->spoor == mc);
                        walkqid_free(rw);
                        if (rbound) spoor_clunk(mc);
                        else { mc->aux = NULL; spoor_unref(mc); }
                    } else {
                        mc->aux = NULL; spoor_unref(mc);
                    }
                    err = T_E_NOENT;
                    goto fail;
                }
                walkqid_free(rw);
                // #66: the prefix components join mc's namespace name
                // (non-load-bearing, I-33).
                for (int j = 0; j <= split_at; j++)
                    spoor_path_extend(mc, names[j], lens[j]);
                trail[depth++] = mc;
                logical_depth += split_at + 1;
                carried_valid = false;   // the tip is a mount point; the
                                         // resumed loop crosses + re-stats
                i = ends[split_at];
                continue;
            }

            if (link_at >= 0) {
                // D-1: split-for-expansion at a followed symlink (the
                // mount-split's sibling, EXCLUSIVE of the link: the prefix
                // materialized is [0..link_at-1] -- the link's PARENT -- and
                // the outer loop resumes AT the link component, which the
                // per-component arm walks, readlinks, and splices).
                // The discarded batch's bound leaf lives past the link --
                // meaningless until the link expands.
                //
                // pounce_skip_one routes that resumed component to the
                // per-component arm, which is where expansion lives -- ONE
                // site, rather than a second copy inside the batch.
                //
                // IT IS ALSO THE TERMINATION GUARD, and the `link_at == 0`
                // case is the proof. That case pushes nothing, charges no
                // depth, no logical_depth, no follows, and resumes at i = s --
                // the run's own start. Without the flag every input to the
                // gather is then byte-identical to the previous turn, so the
                // resolver spins forever issuing one walk_attrs per turn. It
                // is not an exotic shape: `link_at == 0` is EVERY final-
                // component symlink follow on a walk_attrs-bearing Dev (a
                // 1-component run whose only component is the link) -- the
                // first leg of stalk.symlink_follow reaches it.
                //
                // MEASURED, after an earlier draft of this comment claimed
                // the loop, a revert probe reported no loop, and this comment
                // was narrowed to say the failure mode was undemonstrated.
                // That narrowing was wrong: re-running the probe against the
                // BUILT ARTIFACT (object timestamp checked against the source
                // edit) hangs at `stalk.symlink_follow`, the first symlink
                // test, exactly as the trace above predicts. The earlier probe
                // never reached the kernel it claimed to test.
                walkqid_free(w);
                if (bound) spoor_clunk(nc);
                else if (nc) { nc->aux = NULL; spoor_unref(nc); }
                if (link_at > 0) {
                    if (depth >= STALK_MAX_DEPTH)    { err = T_E_INVAL; goto fail; }
                    struct Spoor *lc = spoor_clone(parent);
                    if (!lc) goto fail;
                    // Reuse sts -- the consumed prefix's records are spent.
                    struct Walkqid *rw = parent->dev->walk_attrs(parent, lc,
                                                                 names, lens,
                                                                 link_at, sts);
                    if (rw == DEV_WALK_ATTRS_UNSUPPORTED) {
                        // Unreachable with the one-way per-session latch
                        // (the batch above just SUCCEEDED on this session);
                        // the sentinel is static -- never walkqid_free it.
                        lc->aux = NULL;
                        spoor_unref(lc);
                        goto fail;
                    }
                    if (!rw || rw->nqid != link_at || rw->spoor != lc) {
                        // The tree changed between the two walks (racing
                        // unlink/rename) -- fail as a sequential resolution
                        // racing the same mutation would.
                        if (rw) {
                            bool rbound = (rw->spoor == lc);
                            walkqid_free(rw);
                            if (rbound) spoor_clunk(lc);
                            else { lc->aux = NULL; spoor_unref(lc); }
                        } else {
                            lc->aux = NULL; spoor_unref(lc);
                        }
                        err = T_E_NOENT;
                        goto fail;
                    }
                    walkqid_free(rw);
                    // #66: the prefix components join lc's namespace name
                    // (non-load-bearing, I-33).
                    for (int j = 0; j < link_at; j++)
                        spoor_path_extend(lc, names[j], lens[j]);
                    trail[depth++] = lc;
                    logical_depth += link_at;
                }
                carried_valid   = false;   // the resumed per-component arm
                                           // stats its parent fresh
                pounce_skip_one = true;
                i = (link_at == 0) ? s : ends[link_at - 1];
                continue;
            }

            if (!full) {
                // Partial walk with no mount among the walked prefix: the miss
                // at component k is a REAL miss in the correct tree. Its
                // parent is component k-1 (the base for k == 0, X-checked
                // pre-batch): the fail-ordering invariant's last obligation --
                // an X-denial on the miss's parent masks the miss.
                walkqid_free(w);
                if (nc) { nc->aux = NULL; spoor_unref(nc); }   // never bound on partial
                // #79: the batch's own not-a-directory case. The miss's parent
                // is component k-1, whose fused record vouches for its type --
                // so a run over `/a/file/c` stops at k=2 with sts[1] naming a
                // non-directory, and the answer is ENOTDIR, not "no such c".
                // Same ordering as the base gate above (type before X, for the
                // same reason). k == 0 means the miss's parent is the run BASE,
                // already gated before the batch was issued.
                if (k > 0 && !(sts[k - 1].qid_type & QTDIR)) {
                    err = T_E_NOTDIR;
                    goto fail;
                }
                if (k > 0 && parent->dev->perm_enforced &&
                    perm_check(p, &sts[k - 1], PERM_X) != 0) {
                    err = T_E_ACCES;
                    goto fail;
                }
                err = T_E_NOENT;
                goto fail;
            }

            // Full walk, no split.
            if (query) {
                // #82: the third gate site -- this exit skips the quarry too.
                // The leaf record IS the would-be quarry: the split post-scan
                // above tests EVERY component for mount membership in the
                // query form (`bound` is false when nc == NULL, so the
                // leaf-of-a-bind-walk carve-out cannot apply), so reaching
                // here proves nothing in the run is a mount point.
                if (trailing_slash && !(sts[nrun - 1].qid_type & QTDIR)) {
                    walkqid_free(w);
                    err = T_E_NOTDIR;
                    goto fail;
                }
                // The 1-RPC stat: the leaf's fused record is the answer; no
                // quarry Spoor / fid ever existed -- nothing to clunk, on
                // either end. Success exit WITHOUT touching *errp.
                walkqid_free(w);
                *stat_out  = sts[nrun - 1];
                stat_out->devno = parent->devno;   // #100: the run's base carries the
                                                   // session identity (Chan.dev); the
                                                   // fused leaf inherits it, matching
                                                   // spoor_stat_native's stamp.
                *stat_done = true;
                stalk_unwind(trail, depth);
                stalk_expand_free(ex);
                return NULL;
            }
            walkqid_free(w);
            // #66: the walked components join nc's namespace name (the leaf
            // Spoor carries the whole run; intermediates never materialize).
            for (int j = 0; j < nrun; j++)
                spoor_path_extend(nc, names[j], lens[j]);
            trail[depth++] = nc;
            logical_depth += nrun;
            carried = sts[nrun - 1];   // the new tip's walk-fused record seeds
            carried_valid = true;      // the next run's base X-check and the
                                       // final-hop R/W check (STALK_OPEN)
            continue;
        }

        // Per-component X-search: a perm_enforced Dev gates traversal on PERM_X
        // for the caller's principal (A-2d/A-3 generalized from the single-hop
        // walk-open to every hop, now including the mounted root we crossed
        // into). Fail-closed if the Dev cannot vouch for the metadata.
        // (`per_component` is the pounce's walk_attrs-unsupported fall-through.)
per_component:
        pounce_skip_one = false;   // D-1: the protected component reached the
                                   // per-component arm; pouncing may resume on
                                   // the components that follow it

        // NUL-terminate the single component for the Dev.walk strlen scan
        // (dev9p_walk discovers each name's length by scanning for '\0'). Both
        // the union + non-union walks pass it to Dev.walk.
        char namebuf[SYS_WALK_OPEN_NAME_MAX + 1];
        for (u64 k = 0; k < clen; k++) namebuf[k] = path[s + k];
        namebuf[clen] = '\0';

        struct Spoor *nc;
        if (union_base) {
            // UM: the parent is a UNION mount point (>= 2 members). Search the
            // members in declared order (each: cross -> per-member X-search ->
            // directory-type -> walk the component, Plan 9 skip-on-miss/deny),
            // first that resolves the component wins. The single-parent
            // perm_check below is intentionally skipped -- it checks member 0
            // only; the helper does the check per member. The trail-full bound
            // still applies to the resulting entry.
            if (depth >= STALK_MAX_DEPTH ||
                (pounce_ok && logical_depth >= STALK_MAX_DEPTH)) {
                spoor_clunk(union_base); union_base = NULL;
                err = T_E_INVAL; goto fail;   // trail full
            }
            int uerr = 0;
            nc = stalk_union_child(p, union_base, namebuf, crossed_pheno, &uerr);
            spoor_clunk(union_base); union_base = NULL;   // consumed
            if (!nc) { err = uerr ? uerr : T_E_NOENT; goto fail; }
        } else {
            if (parent->dev && parent->dev->perm_enforced) {
                struct t_stat st;
                int sr = spoor_stat_native(parent, &st);
                if (sr != 0)                         { err = err_code(sr); goto fail; }
                if (perm_check(p, &st, PERM_X) != 0) { err = T_E_ACCES;    goto fail; }
            }

            // Trail-full reject. When pouncing is possible on this path, ALSO
            // enforce the cap on the LOGICAL component count -- runs compress the
            // trail array, and without this a >STALK_MAX_DEPTH-component path
            // whose tail lands on a walk_attrs-less Dev would resolve where the
            // per-component loop INVALs (a surface divergence). Monotone since
            // pounce_ok excludes '..'.
            if (depth >= STALK_MAX_DEPTH ||
                (pounce_ok && logical_depth >= STALK_MAX_DEPTH))
                                                 { err = T_E_INVAL; goto fail; }  // trail full
            if (!parent->dev || !parent->dev->walk)  { err = T_E_INVAL; goto fail; }

            nc = spoor_clone(parent);
            if (!nc) goto fail;

            const char *names[1] = { namebuf };

            struct Walkqid *w = parent->dev->walk(parent, nc, names, 1);
            if (!w) {
                // Walk failed: nc still shares the parent's aux -> detach + unref.
                // A NULL Walkqid is dev9p's miss (p9_client_walk Rlerror/short) --
                // the dominant case is "no such component" (ENOENT). The rare deep
                // failures (session-dead -> EIO, OOM) also land here; reporting
                // NOENT is the load-bearing-correct answer (os.IsNotExist), and the
                // kernel's own perm_check above is the ACCES authority, so a walk
                // NULL is never a masked permission denial. (ER-2 may propagate the
                // exact dev9p errno via a walk-vtable out-param.)
                nc->aux = NULL;
                spoor_unref(nc);
                err = T_E_NOENT;
                goto fail;
            }
            if (w->spoor != nc) {
                // Dev violated the reuse-nc contract (defense-in-depth; dev9p +
                // devramfs both honor it). nc still shares the parent's aux.
                walkqid_free(w);
                nc->aux = NULL;
                spoor_unref(nc);
                goto fail;
            }
            if (w->nqid != 1) {
                // Walk-miss (devramfs returns nqid=0; dev9p returns NULL above).
                // nc was reused (w->spoor == nc) with a non-heap (devramfs) aux,
                // so clunk is safe -- matches sys_walk_open_handler's nqid!=1 path.
                walkqid_free(w);
                spoor_clunk(nc);
                err = T_E_NOENT;            // walk-miss (devramfs)
                goto fail;
            }
            walkqid_free(w);
        }

        // ====================================================================
        // D-1: symlink expansion (docs/DISTRO.md section 4). The just-walked
        // component is a link iff its qid carries QTSYMLINK (dev9p maps
        // P9_QTSYMLINK; native Devs never mint it). MOUNT MEMBERSHIP WINS
        // FIRST: a mount over the link's identity SHADOWS it -- skip
        // expansion and let the existing lazy cross machinery replace it
        // (the same mount-before-symlink order the pounce post-scan uses).
        // Then the final-component disposition: an INTERMEDIATE link always
        // follows; a FINAL link follows unless nofollow_final. A no-followed
        // link -- or one on a readlink-less Dev (the opaque-leaf fail-closed
        // arm) -- falls through and is pushed like any leaf: mid-path it
        // answers ENOTDIR at the next hop's #79 gate; as the quarry,
        // STALK_OPEN answers T_E_LOOP below and WALK/STAT/MOUNT return the
        // link itself (the lstat / mount-point shape).
        //
        // Expansion produces COMPONENTS, not answers: the spliced target
        // re-enters THIS loop, so the whole gate family (#79/#81/#82/#84
        // + the per-component X-search + mount crossing) binds it BY
        // CONSTRUCTION -- the design's central soundness claim (I-28).
        // ====================================================================
        if ((nc->qid.type & QTSYMLINK) &&
            !(p && p->territory &&
              mount_is_point_id(p->territory, nc->dc, nc->devno,
                                nc->qid.path))) {
            bool is_final;
            {
                u64 peek = i;
                while (peek < pathlen && path[peek] == '/') peek++;
                is_final = (peek >= pathlen);
            }
            if ((!is_final || !nofollow_final) &&
                nc->dev && nc->dev->readlink) {
                int xrc = stalk_expand_link(p, &ex, nc, path, pathlen, s, i,
                                            &base, &err);
                spoor_clunk(nc);   // the link fid is spent either way
                if (xrc < 0) goto fail;
                path    = ex->buf[ex->cur];
                pathlen = ex->buflen;
                if (xrc == 1) {
                    // Restart: absolute re-anchor, or a '..'-bearing target
                    // (the 1:1-pop requirement -- see the '..' arm).
                    stalk_unwind(trail, depth);
                    depth = 0;
                    goto restart;
                }
                // In-place splice: the trail stands; recompute the
                // path-derived state from the new buffer. pounce_ok is
                // STICKY-AND, never re-enabled mid-resolution: the '..'
                // soundness argument requires that a compressed trail entry
                // and a '..' pop never coexist, and only a buffer that was
                // '..'-free END TO END may have compressed entries behind it.
                pounce_ok      = pounce_ok && !path_has_dotdot(path, pathlen);
                trailing_slash = path_has_trailing_slash(path, pathlen);
                nofollow_final = nofollow_req && !trailing_slash;
                i = 0;
                continue;
            }
        }

        // #66: append the walked component to nc's namespace name. nc SHARES
        // parent's Path (from spoor_clone); spoor_path_extend reads that shared
        // Path as the base and replaces it with the extended one. Non-load-bearing
        // (I-33) -- an OOM leaves nc->path NULL ("unknown") and the walk still
        // succeeds. (`.`/`..` never reach here in stalk -- the resolver handles
        // them above -- so this is always a real component.)
        spoor_path_extend(nc, namebuf, clen);

        // Push UN-crossed. The cross happens lazily when nc later becomes a
        // parent (cross-on-descent above) or, for the quarry, at the end below.
        trail[depth++] = nc;   // owned (own fid post-walk for dev9p)
        logical_depth++;       // (only consumed while pounce_ok)
        carried_valid = false; // a plain walk fetches no attrs for the new tip
    }

    // Determine the quarry.
    if (depth > 0) {
        // Pop the deepest resolved Spoor off the trail; trail[0..depth) now
        // holds only the ancestors (released by stalk_unwind below). A valid
        // `carried` record describes exactly this Spoor (it was set when the
        // tip was pushed and invalidated on every event that changed the tip).
        quarry = trail[--depth];
    } else {
        // Zero real components ("/", ".", or a ".." run netted back to the
        // base): the quarry is `base` itself, clone-walked to an owned,
        // openable Spoor (base is borrowed / the ref-held re-anchor root).
        // base is not a mount point (the base cross verified), so no cross is
        // owed before this clone.
        quarry = clone_walk_zero(base);
        if (!quarry) goto fail;
    }

    // Cross the quarry so a walked mount point yields the MOUNTED ROOT (Plan 9
    // domount on the final element), EXCEPT under STALK_MOUNT, which wants the
    // mount point's OWN identity so SYS_MOUNT's MREPL re-keys the same
    // underlying point even when it already hosts a mount. On failure the quarry
    // is still owned -> the fail path clunks it.
    if (amode != STALK_MOUNT) {
        // UM: is the quarry a UNION mount point (>= 2 grafted members)? If so
        // AND this is an OPEN (the only resolution that yields a readdir fd),
        // tag the opened Spoor with the union base so spoor_readdir_run merges
        // every member (dedup first-wins). Capture the base BEFORE the cross:
        // the crossed Spoor carries member[0]'s qid, not the mount point's, so
        // the base must be the pre-cross quarry (the mount-point identity
        // mount_member_at keys on). The +1 ref is transferred onto the crossed
        // Spoor's union_base (a DISTINCT object -- the cross mints a fresh
        // Spoor) and released at spoor_free_internal.
        struct Spoor *union_pt = NULL;
        if (amode == STALK_OPEN && p && p->territory) {
            struct Spoor *m1 = mount_member_at(p->territory, quarry, 1, NULL);
            if (m1) { spoor_clunk(m1); union_pt = quarry; spoor_ref(union_pt); }
        }
        struct Spoor *crossed = NULL;
        if (stalk_cross_mounts(p, quarry, &crossed, crossed_pheno) < 0) {
            if (union_pt) spoor_clunk(union_pt);
            goto fail;
        }
        if (crossed) {
            spoor_clunk(quarry);
            quarry = crossed;
            carried_valid = false;   // the record described the mount point
            if (union_pt) { quarry->union_base = union_pt; union_pt = NULL; }
        }
        if (union_pt) spoor_clunk(union_pt);   // union w/o a cross (defensive)
    }

    // #82: a trailing '/' asserts the path names a DIRECTORY. Gate the QUARRY,
    // and deliberately AFTER the cross above -- unlike #81's dot gate, which
    // reads the tip UNCROSSED. The two ask different questions of the same
    // field:
    //   - `.`/`..` are about WHERE RESOLUTION STANDS. Under STALK_MOUNT a mount
    //     point deliberately does not cross, so `/mnt/.` must equal `/mnt`;
    //     crossing there would INTRODUCE a divergence.
    //   - a trailing slash is about WHAT THE PATH NAMES, which is the crossed
    //     result: `/mnt/` names the MOUNTED ROOT, not the shadowed point.
    // The distinction is observable, not academic: nothing requires a mount
    // point and its mounted root to agree on type (territory.c's mount() has no
    // QTDIR check), so a directory mounted over a file makes `/mnt/` legal and
    // a file mounted over a directory makes it ENOTDIR -- gating the uncrossed
    // point would be wrong in BOTH directions. Placing it on the quarry gets
    // every amode right for free, because the quarry is by construction the
    // thing the resolution names (STALK_MOUNT's uncrossed quarry included: a
    // trailing slash there asserts the MOUNT POINT is a directory, which is
    // exactly what SYS_MOUNT names).
    //
    // BEFORE the final-hop R/W gate, matching #79's type-before-permission
    // ordering: `open("/a/file/", O_RDONLY)` on an unreadable file is ENOTDIR,
    // not EACCES -- the answer must not turn on a mode bit that has no bearing
    // on whether the path is even well-formed.
    if (trailing_slash && !(quarry->qid.type & QTDIR)) { err = T_E_NOTDIR; goto fail; }

    // D-1: an un-followed symlink quarry under STALK_OPEN answers T_E_LOOP --
    // Linux O_NOFOLLOW semantics (the caller asked for not-a-symlink and the
    // final component is one; a symlink cannot be opened), and the same
    // answer covers the readlink-less-Dev fail-closed leftover (a followable
    // link the resolver could not expand). WALK / STAT / MOUNT return the
    // link itself (the lstat / mount-point shape). Type-class before
    // permission (#79's ordering), hence before the R/W gate below. A
    // FOLLOWED open never reaches here as a link: expansion consumed it.
    if (amode == STALK_OPEN && (quarry->qid.type & QTSYMLINK)) {
        err = T_E_LOOP;
        goto fail;
    }

    // Final hop. STALK_WALK (O_PATH) + STALK_MOUNT + STALK_STAT return the
    // resolved quarry unopened (a navigation / create / chroot / mount /
    // metadata base; exempt from the R/W gate, matching the single-hop
    // walk-open's O_PATH carve-out -- POSIX stat authority is the path
    // X-search only, exactly what the O_PATH+fstat emulation granted).
    // STALK_STAT's pure-query fast path returned earlier via stat_done; a
    // quarry reaching here (walk_attrs-less final Dev / leaf mount point /
    // zero-component path) is stat_native'd by the stalk_stat wrapper.
    // STALK_OPEN runs the R/W perm_check then Dev.open.
    if (amode == STALK_OPEN) {
        if (quarry->dev && quarry->dev->perm_enforced) {
            struct t_stat st;
            if (carried_valid) {
                // The quarry's own walk-fused record (the final run's leaf) --
                // the same server sample a fresh Tgetattr would re-fetch,
                // taken by THIS resolution (§6: zero staleness added).
                st = carried;
            } else {
                int sr = spoor_stat_native(quarry, &st);
                if (sr != 0)                                          { err = err_code(sr); goto fail; }
            }
            if (perm_check(p, &st, perm_want_for_omode(omode)) != 0)  { err = T_E_ACCES;    goto fail; }
        }
        if (!quarry->dev || !quarry->dev->open)                       { err = T_E_INVAL; goto fail; }
        // Dev.open returns EITHER the same Spoor opened in place (dev9p /
        // devramfs: a read/write cursor over the walked node, ref unchanged) OR
        // a DIFFERENT owned Spoor that REPLACES the quarry (devsrv open=connect:
        // a /srv/<name> service node is consumed and the connection endpoint --
        // a dev9p root Spoor for a 9p-mode service, a byte-conn Spoor for a
        // byte-mode one -- is returned; STALK-DESIGN.md §5.2). The returned
        // Spoor carries one owned ref; if it differs, the old quarry is spent
        // (open did not consume its ref) -> clunk it and adopt the replacement.
        struct Spoor *opened = quarry->dev->open(quarry, (int)omode);
        if (!opened) goto fail;
        if (opened != quarry) {
            // #66 (audit F2): the replacement (devsrv open=connect's connection
            // endpoint) is born with its OWN name ("/" for a 9P-mode conn root,
            // unknown for a byte conn). It was reached by the user under the
            // quarry's name (e.g. /srv/corvus), so transplant that name onto it --
            // fd2path must report the path the caller opened, not the conn root's
            // "/". `opened` is thread-local (not yet installed), so this stays
            // within the set-before-publish discipline (I-33). Non-load-bearing.
            spoor_path_transplant(opened, quarry);
            spoor_clunk(quarry);
            quarry = opened;
        }
    }

    stalk_unwind(trail, depth);   // release the ancestors; quarry survives
    stalk_expand_free(ex);        // D-1: owned_base ref + the state buffers
    return quarry;

fail:
    if (errp) *errp = err;             // the cause, for the caller's -*errp
    if (union_base) spoor_clunk(union_base);  // UM: a goto slipped past the consume
    if (quarry) spoor_clunk(quarry);   // quarry was popped off the trail
    stalk_unwind(trail, depth);        // release any remaining ancestors
    stalk_expand_free(ex);             // D-1
    return NULL;
}

struct Spoor *stalk_err(struct Proc *p, struct Spoor *start,
                        const char *path, u64 pathlen, int amode, u32 omode,
                        int *errp) {
    return stalk_core(p, start, path, pathlen, amode, omode, errp, NULL, NULL, NULL);
}

// stalk_exec (VIVARIUM section 13) -- the exec-resolution variant: identical to
// stalk_err but reports, via *crossed_pheno, whether the resolution decided
// Linux: SEEDED from the resolving Territory's declaration (territory_root_pheno,
// Design D 13.10.5) at stalk_core's restart: label, then OR-accumulated with
// every MPHENO_LINUX mount crossed. The caller inits *crossed_pheno = false and
// reads it ONLY on success -- and that "only" is load-bearing (audit F5): a
// NULL start/path returns before the seed and leaves the init, but a walk that
// fails AFTER restart: has already written the seed, so on failure the value
// may read true. No caller consults it then: a failed resolve loads no image,
// so nothing is stamped (fail-safe -- the caller keeps its own image). Every
// image-load path -- all spawn variants and execve -- consumes this;
// stalk_err's own callers are untouched.
struct Spoor *stalk_exec(struct Proc *p, struct Spoor *start,
                         const char *path, u64 pathlen, int amode, u32 omode,
                         int *errp, bool *crossed_pheno) {
    return stalk_core(p, start, path, pathlen, amode, omode, errp, NULL, NULL,
                      crossed_pheno);
}

int stalk_stat(struct Proc *p, struct Spoor *start,
               const char *path, u64 pathlen, u32 flags,
               struct t_stat *out, int *errp) {
    if (!out) { if (errp) *errp = T_E_INVAL; return -1; }
    // D-1: STALK_NOFOLLOW is the one admitted flag (the lstat shape); reject
    // anything else loudly (the stalk-1 F1 amode-guard discipline).
    if (flags & ~(u32)STALK_NOFOLLOW) { if (errp) *errp = T_E_INVAL; return -1; }
    bool done = false;
    struct Spoor *q = stalk_core(p, start, path, pathlen,
                                 STALK_STAT | (int)flags, 0,
                                 errp, out, &done, NULL);
    if (done) return 0;   // the walk-query fast path filled *out; no Spoor existed
    if (!q)   return -1;  // *errp carries the cause
    // Fallback quarry (walk_attrs-less final Dev / leaf mount point crossed to
    // the mounted root / zero-component path): stat it and release it --
    // exactly the O_PATH+fstat emulation this syscall replaces, minus the
    // handle-table round trip.
    int sr = spoor_stat_native(q, out);
    spoor_clunk(q);
    if (sr != 0) { if (errp) *errp = err_code(sr); return -1; }
    return 0;
}
