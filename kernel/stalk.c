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
#include <thylacine/perm.h>
#include <thylacine/proc.h>       // struct Proc -> territory
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>    // SYS_WALK_OPEN_NAME_MAX + struct t_stat
#include <thylacine/territory.h>  // mount_lookup + PGRP_MAX_MOUNTS

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
int stalk_cross_mounts(struct Proc *p, struct Spoor *probe,
                       struct Spoor **out) {
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
        // RW-4 SA-F1: mount_lookup now returns a REF-HELD source (the lookup +
        // ref are atomic under the Territory ns_lock, so a concurrent unmount
        // cannot free it mid-cross). clone_walk_zero mints an INDEPENDENT crossed
        // Spoor from it; release the transferred ref immediately after.
        struct Spoor *src = mount_lookup(p->territory, id);
        if (!src) break;                          // id is not a mount point
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
    *out = cur;
    return 0;
}

struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode) {
    if (!start || !path) return NULL;
    // Reject an unknown amode LOUDLY rather than silently degrading to walk-only
    // (stalk-1 audit F1). stalk-2 adds STALK_MOUNT; any future amode must be
    // added here AND given its final-hop dispatch arm below -- a missed arm must
    // fail-closed, not skip an open / a cross / a create's parent check.
    if (amode != STALK_WALK && amode != STALK_OPEN && amode != STALK_MOUNT)
        return NULL;

    struct Spoor *trail[STALK_MAX_DEPTH];
    int           depth  = 0;
    struct Spoor *quarry = NULL;

    // Cross the BASE: `start` itself may be a mount point. If it crosses, the
    // owned crossed clone becomes trail[0] (so the first component searches the
    // mounted root, X-checked like any parent). If not, the base stays `start`
    // (borrowed) and depth stays 0. `start` is borrowed -> we cannot cross it in
    // place; the crossed clone goes on the trail instead.
    {
        struct Spoor *crossed = NULL;
        if (stalk_cross_mounts(p, start, &crossed) < 0) goto fail;
        if (crossed) trail[depth++] = crossed;
    }

    u64 i = 0;
    while (i < pathlen) {
        // Skip the separator run (collapses a leading '/' and any '//').
        while (i < pathlen && path[i] == '/') i++;
        if (i >= pathlen) break;
        u64 s = i;
        while (i < pathlen && path[i] != '/') i++;
        u64 clen = i - s;   // component is path[s .. i)

        // "." -- a no-op (stay at the current directory).
        if (clen == 1 && path[s] == '.') continue;

        // ".." -- pop the trail. Contained at `start`: at the bottom (depth 0)
        // this is a no-op, so resolution can never escape above the base (the
        // chroot/pivot boundary -- I-28). The popped clone is clunk-safe (it
        // owns its fid: a walked child or a crossed clone).
        if (clen == 2 && path[s] == '.' && path[s + 1] == '.') {
            if (depth > 0) spoor_clunk(trail[--depth]);
            continue;
        }

        // A real component. Bound its length (the Dev.walk vtable takes a
        // NUL-terminated name; an over-long component is rejected, not
        // truncated).
        if (clen > SYS_WALK_OPEN_NAME_MAX) goto fail;

        // The directory we are about to search. CROSS IT ON DESCENT: if the
        // trail tip is a mount point, replace it in place with the mounted root
        // so we walk INTO the mounted tree and X-check the mounted root (not the
        // shadowed mount point). The base case (depth==0, parent==start) was
        // already proven not-a-mount by the base cross above.
        struct Spoor *parent;
        if (depth > 0) {
            struct Spoor *crossed = NULL;
            if (stalk_cross_mounts(p, trail[depth - 1], &crossed) < 0) goto fail;
            if (crossed) {
                spoor_clunk(trail[depth - 1]);
                trail[depth - 1] = crossed;
            }
            parent = trail[depth - 1];
        } else {
            parent = start;
        }

        // Per-component X-search: a perm_enforced Dev gates traversal on PERM_X
        // for the caller's principal (A-2d/A-3 generalized from the single-hop
        // walk-open to every hop, now including the mounted root we crossed
        // into). Fail-closed if the Dev cannot vouch for the metadata.
        if (parent->dev && parent->dev->perm_enforced) {
            struct t_stat st;
            if (spoor_stat_native(parent, &st) != 0) goto fail;
            if (perm_check(p, &st, PERM_X) != 0)      goto fail;
        }

        if (depth >= STALK_MAX_DEPTH)            goto fail;   // trail full
        if (!parent->dev || !parent->dev->walk)  goto fail;

        struct Spoor *nc = spoor_clone(parent);
        if (!nc) goto fail;

        // NUL-terminate the single component for the Dev.walk strlen scan
        // (dev9p_walk discovers each name's length by scanning for '\0').
        char namebuf[SYS_WALK_OPEN_NAME_MAX + 1];
        for (u64 k = 0; k < clen; k++) namebuf[k] = path[s + k];
        namebuf[clen] = '\0';
        const char *names[1] = { namebuf };

        struct Walkqid *w = parent->dev->walk(parent, nc, names, 1);
        if (!w) {
            // Walk failed: nc still shares the parent's aux -> detach + unref.
            nc->aux = NULL;
            spoor_unref(nc);
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
            goto fail;
        }
        walkqid_free(w);

        // Push UN-crossed. The cross happens lazily when nc later becomes a
        // parent (cross-on-descent above) or, for the quarry, at the end below.
        trail[depth++] = nc;   // owned (own fid post-walk for dev9p)
    }

    // Determine the quarry.
    if (depth > 0) {
        // Pop the deepest resolved Spoor off the trail; trail[0..depth) now
        // holds only the ancestors (released by stalk_unwind below).
        quarry = trail[--depth];
    } else {
        // Zero real components ("/", ".", or a ".." run netted back to the
        // base): the quarry is `start` itself, clone-walked to an owned,
        // openable Spoor (start is borrowed). start is not a mount point (the
        // base cross verified), so no cross is owed before this clone.
        quarry = clone_walk_zero(start);
        if (!quarry) goto fail;
    }

    // Cross the quarry so a walked mount point yields the MOUNTED ROOT (Plan 9
    // domount on the final element), EXCEPT under STALK_MOUNT, which wants the
    // mount point's OWN identity so SYS_MOUNT's MREPL re-keys the same
    // underlying point even when it already hosts a mount. On failure the quarry
    // is still owned -> the fail path clunks it.
    if (amode != STALK_MOUNT) {
        struct Spoor *crossed = NULL;
        if (stalk_cross_mounts(p, quarry, &crossed) < 0) goto fail;
        if (crossed) {
            spoor_clunk(quarry);
            quarry = crossed;
        }
    }

    // Final hop. STALK_WALK (O_PATH) + STALK_MOUNT return the resolved quarry
    // unopened (a navigation / create / chroot / mount base; exempt from the R/W
    // gate, matching the single-hop walk-open's O_PATH carve-out). STALK_OPEN
    // runs the R/W perm_check then Dev.open.
    if (amode == STALK_OPEN) {
        if (quarry->dev && quarry->dev->perm_enforced) {
            struct t_stat st;
            if (spoor_stat_native(quarry, &st) != 0)                  goto fail;
            if (perm_check(p, &st, perm_want_for_omode(omode)) != 0)  goto fail;
        }
        if (!quarry->dev || !quarry->dev->open)                       goto fail;
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
            spoor_clunk(quarry);
            quarry = opened;
        }
    }

    stalk_unwind(trail, depth);   // release the ancestors; quarry survives
    return quarry;

fail:
    if (quarry) spoor_clunk(quarry);   // quarry was popped off the trail
    stalk_unwind(trail, depth);        // release any remaining ancestors
    return NULL;
}
