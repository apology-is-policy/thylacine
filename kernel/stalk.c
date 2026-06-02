// stalk -- the per-Proc multi-component pathname resolver. See
// <thylacine/stalk.h> + docs/STALK-DESIGN.md. Invariant I-28.
//
// This is stalk-1: resolution WITHIN one Dev (no mount-crossing -- stalk-2 adds
// Plan 9 `domount` keyed by mount-point Spoor identity). The resolver
// generalizes sys_walk_open_handler's single-hop clone->walk->clunk lifetime to
// N hops via a `trail` of owned clones, with `.`/`..` handled in the resolver
// (never passed to Dev.walk), `..` contained at `start`, and a per-component
// perm_check X-search at each directory hop.
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
//   - on return, every trail entry is spoor_clunk'd EXCEPT the quarry.

#include <thylacine/stalk.h>

#include <thylacine/dev.h>
#include <thylacine/perm.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>   // SYS_WALK_OPEN_NAME_MAX + struct t_stat

// Clunk every owned trail entry trail[0..depth). Used both on the success path
// (after the quarry is popped, this releases the ancestors) and on every
// failure path.
static void stalk_unwind(struct Spoor **trail, int depth) {
    while (depth > 0) {
        spoor_clunk(trail[--depth]);
    }
}

struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode) {
    if (!start || !path) return NULL;
    // Reject an unknown amode LOUDLY rather than silently degrading to walk-only
    // (stalk-1 audit F1): stalk-2/3 add STALK_CREATE / STALK_MOUNT, and a missed
    // final-hop dispatch arm must fail-closed, not skip the open / a create's
    // W+X parent check. Cheap insurance against sub-chunk-boundary rot.
    if (amode != STALK_WALK && amode != STALK_OPEN) return NULL;

    struct Spoor *trail[STALK_MAX_DEPTH];
    int           depth  = 0;
    struct Spoor *quarry = NULL;

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
        // this is a no-op, so resolution can never escape above the base
        // (the chroot/pivot boundary -- I-28). The popped clone is clunk-safe
        // (it owned its fid post-walk).
        if (clen == 2 && path[s] == '.' && path[s + 1] == '.') {
            if (depth > 0) spoor_clunk(trail[--depth]);
            continue;
        }

        // A real component. Bound its length (the Dev.walk vtable takes a
        // NUL-terminated name; an over-long component is rejected, not
        // truncated).
        if (clen > SYS_WALK_OPEN_NAME_MAX) goto fail;

        // The directory we are about to search is the current trail tip (or
        // `start` for the first hop). Per-component X-search: a perm_enforced
        // Dev gates traversal on PERM_X for the caller's principal (A-2d/A-3
        // generalized from the single-hop walk-open to every hop). Fail-closed
        // if the Dev cannot vouch for the metadata.
        struct Spoor *parent = (depth > 0) ? trail[depth - 1] : start;
        if (parent->dev && parent->dev->perm_enforced) {
            struct t_stat st;
            if (spoor_stat_native(parent, &st) != 0) goto fail;
            if (perm_check(p, &st, PERM_X) != 0)      goto fail;
        }

        if (depth >= STALK_MAX_DEPTH)        goto fail;   // trail full
        if (!parent->dev || !parent->dev->walk) goto fail;

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

        trail[depth++] = nc;   // owned (own fid post-walk for dev9p)
    }

    // Determine the quarry.
    if (depth > 0) {
        // Pop the deepest resolved Spoor off the trail; trail[0..depth) now
        // holds only the ancestors (released by stalk_unwind below).
        quarry = trail[--depth];
    } else {
        // Zero real components ("/", ".", or a ".." run netted back to the
        // base): the quarry is `start` itself. `start` is borrowed, so
        // clone-walk it (Dev.walk with nname=0) to mint an owned, openable
        // Spoor with its own fid.
        if (!start->dev || !start->dev->walk) goto fail;
        struct Spoor *q = spoor_clone(start);
        if (!q) goto fail;
        struct Walkqid *w = start->dev->walk(start, q, (const char **)0, 0);
        if (!w || w->spoor != q) {
            if (w) walkqid_free(w);
            q->aux = NULL;          // clone-walk failed -> q still shares start's aux
            spoor_unref(q);
            goto fail;
        }
        walkqid_free(w);            // nqid == 0 expected for a clone-walk
        quarry = q;
    }

    // Final hop. STALK_WALK (O_PATH) returns the walkable quarry unopened (a
    // navigation / create / chroot base; exempt from the R/W gate, matching the
    // single-hop walk-open's O_PATH carve-out). STALK_OPEN runs the R/W
    // perm_check then Dev.open.
    if (amode == STALK_OPEN) {
        if (quarry->dev && quarry->dev->perm_enforced) {
            struct t_stat st;
            if (spoor_stat_native(quarry, &st) != 0)                  goto fail;
            if (perm_check(p, &st, perm_want_for_omode(omode)) != 0)  goto fail;
        }
        if (!quarry->dev || !quarry->dev->open ||
            !quarry->dev->open(quarry, (int)omode)) {
            goto fail;
        }
    }

    stalk_unwind(trail, depth);   // release the ancestors; quarry survives
    return quarry;

fail:
    if (quarry) spoor_clunk(quarry);   // quarry was popped off the trail
    stalk_unwind(trail, depth);        // release any remaining ancestors
    return NULL;
}
