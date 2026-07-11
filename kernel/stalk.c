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
#include <thylacine/territory.h>  // mount_lookup + PGRP_MAX_MOUNTS

// Map a spoor_stat_native / Dev return (0 ok, a negative errno, or the generic
// -1) into a POSITIVE T_E_* code for stalk's *errp. A real -errno in [-4095,-2]
// yields its magnitude; -1 (the generic sentinel -- and == -T_E_PERM, which must
// never surface as errno 1) and anything else collapse to T_E_IO.
static int err_code(int ret) {
    if (ret <= -2 && ret >= -4095) return -ret;
    return T_E_IO;
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
    // #66: a crossed Spoor takes the MOUNT-POINT's namespace name -- the user
    // named `probe`'s path (e.g. /mnt), and crossing into the mounted tree keeps
    // that name, NOT the mount source's internal name. One transplant on the
    // final link (a mount-over-mount chain keeps the original mount point's
    // name). Non-load-bearing (I-33): cosmetic only.
    if (cur) spoor_path_transplant(cur, probe);
    *out = cur;
    return 0;
}

// path_has_dotdot -- pre-scan for a ".." component. The POUNCE compresses a
// run of components into ONE trail entry (intermediates never materialize as
// Spoors), which is incompatible with `..`'s pop-one-component semantics --
// a pop into the middle of a pounced run has no Spoor to land on. Any ".."
// in the path therefore disables the pounce wholesale and the resolver runs
// today's per-component loop (the design's stated worst case). Resolved
// paths in the motivating workloads are lexically cleaned ('..'-free): the
// cwd join cleans (LS-4), and toolchain paths are absolute.
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

// stalk() -- the errp==NULL convenience wrapper (the common, errno-agnostic
// API the bulk of callers + the test suite use). The errno-rollout consumers
// (SYS_OPEN) call stalk_err() directly.
struct Spoor *stalk(struct Proc *p, struct Spoor *start,
                    const char *path, u64 pathlen, int amode, u32 omode) {
    return stalk_err(p, start, path, pathlen, amode, omode, NULL);
}

// stalk_core -- the resolver body. stat_out/stat_done are the STALK_STAT
// walk-query sink (stalk_stat only): when the final run resolves through
// Dev.walk_attrs and the leaf is clean, the core fills *stat_out, sets
// *stat_done, unwinds, and returns NULL WITHOUT touching *errp -- no quarry
// Spoor ever exists. Every other caller passes NULL/NULL.
static struct Spoor *stalk_core(struct Proc *p, struct Spoor *start,
                                const char *path, u64 pathlen,
                                int amode, u32 omode, int *errp,
                                struct t_stat *stat_out, bool *stat_done) {
    if (!start || !path) { if (errp) *errp = T_E_INVAL; return NULL; }
    // Reject an unknown amode LOUDLY rather than silently degrading to walk-only
    // (stalk-1 audit F1). stalk-2 adds STALK_MOUNT; POUNCE adds STALK_STAT; any
    // future amode must be added here AND given its final-hop dispatch arm below
    // -- a missed arm must fail-closed, not skip an open / a cross / a create's
    // parent check.
    if (amode != STALK_WALK && amode != STALK_OPEN && amode != STALK_MOUNT &&
        amode != STALK_STAT)
        { if (errp) *errp = T_E_INVAL; return NULL; }

    struct Spoor *trail[STALK_MAX_DEPTH];
    int           depth  = 0;
    struct Spoor *quarry = NULL;
    // Errno-rollout: the cause of a NULL return, written to *errp at `fail`.
    // Set precisely before each `goto fail` (T_E_NOENT walk-miss, T_E_ACCES
    // perm denial, T_E_INVAL structural, propagated for a stat/open failure);
    // T_E_IO is the generic default for an OOM / transport / cross failure.
    int           err    = T_E_IO;

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
    bool          pounce_ok = !path_has_dotdot(path, pathlen);
    struct t_stat carried;
    bool          carried_valid = false;
    int           logical_depth = 0;

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
            struct Spoor *crossed = NULL;
            if (stalk_cross_mounts(p, trail[depth - 1], &crossed) < 0) goto fail;
            if (crossed) {
                spoor_clunk(trail[depth - 1]);
                trail[depth - 1] = crossed;
                carried_valid = false;   // the tip changed; the record described
                                         // the mount point, not the mounted root
            }
            parent = trail[depth - 1];
        } else {
            parent = start;
        }

        // =====================================================================
        // POUNCE (docs/POUNCE-DESIGN.md §5) -- batch the maximal run of
        // consecutive real components through ONE Dev.walk_attrs call (one wire
        // RPC on dev9p), then enforce the per-component X-search + mount scan
        // in a LEFT-TO-RIGHT post-scan whose outcome is byte-identical to the
        // per-component loop below (the fail-ordering invariant, §6).
        // Disabled when the path contains '..' (pounce_ok) or the Dev lacks
        // the slot -- those take the per-component loop unchanged.
        // =====================================================================
        if (pounce_ok && parent->dev && parent->dev->walk_attrs &&
            logical_depth < STALK_MAX_DEPTH) {
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
                    }
                    if (!co_mount) {
                        if (parent->dev->perm_enforced &&
                            perm_check(p, &sts[nrun - 1],
                                       perm_want_for_omode(omode)) != 0) {
                            spoor_clunk(co);
                            err = T_E_ACCES;
                            goto fail;
                        }
                        stalk_unwind(trail, depth);
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

            if (!full) {
                // Partial walk with no mount among the walked prefix: the miss
                // at component k is a REAL miss in the correct tree. Its
                // parent is component k-1 (the base for k == 0, X-checked
                // pre-batch): the fail-ordering invariant's last obligation --
                // an X-denial on the miss's parent masks the miss.
                walkqid_free(w);
                if (nc) { nc->aux = NULL; spoor_unref(nc); }   // never bound on partial
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
                // The 1-RPC stat: the leaf's fused record is the answer; no
                // quarry Spoor / fid ever existed -- nothing to clunk, on
                // either end. Success exit WITHOUT touching *errp.
                walkqid_free(w);
                *stat_out  = sts[nrun - 1];
                *stat_done = true;
                stalk_unwind(trail, depth);
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
            carried_valid = false;   // the record described the mount point
        }
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
    return quarry;

fail:
    if (errp) *errp = err;             // the cause, for the caller's -*errp
    if (quarry) spoor_clunk(quarry);   // quarry was popped off the trail
    stalk_unwind(trail, depth);        // release any remaining ancestors
    return NULL;
}

struct Spoor *stalk_err(struct Proc *p, struct Spoor *start,
                        const char *path, u64 pathlen, int amode, u32 omode,
                        int *errp) {
    return stalk_core(p, start, path, pathlen, amode, omode, errp, NULL, NULL);
}

int stalk_stat(struct Proc *p, struct Spoor *start,
               const char *path, u64 pathlen,
               struct t_stat *out, int *errp) {
    if (!out) { if (errp) *errp = T_E_INVAL; return -1; }
    bool done = false;
    struct Spoor *q = stalk_core(p, start, path, pathlen, STALK_STAT, 0,
                                 errp, out, &done);
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
