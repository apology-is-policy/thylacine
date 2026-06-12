// Territory primitives — Plan 9 bind/unbind/mount/unmount + Territory
// lifecycle (P2-Eb + P5-attach-mount).
//
// Per ARCHITECTURE.md §9.1 + §9.6 + specs/territory.tla. Implements the
// spec's Bind / Unbind / Mount / Unmount / ForkClone actions on a
// SLUB-allocated Territory struct holding fixed-size bind + mount
// tables.
//
// State invariants:
//
//   NoCycle (ARCH §28 I-3): for every Territory `p` and every path `x`,
//   `x` is not reachable from its own binding set via the transitive
//   closure. Enforced by `would_create_cycle` (DFS over existing edges)
//   called from `bind()` BEFORE inserting; if a cycle would form, bind
//   returns -1.
//
//   MountRefcountConsistency (ARCH §9.6.6): every mount entry holds one
//   refcount on its source Spoor. Maintained by:
//     - mount(): spoor_ref(source) before insert.
//     - unmount(): spoor_unref(source) after remove.
//     - territory_clone(): spoor_ref(source) per cloned entry.
//     - territory_unref() final release: spoor_unref(source) for each
//       entry BEFORE kmem_cache_free.
//
//   Isolation (ARCH §28 I-1): all operations take a single Territory *.
//   Two Territories' tables are never modified in the same call.
//
// Bootstrap order (kernel/main.c calls):
//   1. slub_init
//   2. territory_init       — Territory SLUB cache + kproc's empty Territory
//   3. proc_init            — kproc; assigns kproc->territory = kpgrp()
//   4. thread_init          — kthread
//   5. dev_init             — which internally calls spoor_init (Spoor cache)
//
// territory_init runs BEFORE spoor_init. Safe because territory_init
// only allocates EMPTY Territories (nmounts = 0); the territory_unref
// final-release path only calls spoor_unref when nmounts > 0, which
// requires mount() to have been called first, which requires a Spoor,
// which requires spoor_init. The dependency is therefore satisfied
// automatically by call ordering, not by init ordering.

#include <thylacine/extinction.h>
#include <thylacine/path.h>
#include <thylacine/spoor.h>
#include <thylacine/territory.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_pgrp_cache;
static struct Territory  *g_kpgrp;
static u64                g_pgrp_created;
static u64                g_pgrp_destroyed;

// =============================================================================
// Territory lifecycle.
// =============================================================================

static void territory_init_fields(struct Territory *p) {
    p->magic      = PGRP_MAGIC;
    p->ref        = 1;
    p->nbinds     = 0;
    p->nmounts    = 0;
    p->root_spoor = NULL;
    spin_lock_init(&p->dot_lock);
    spin_lock_init(&p->ns_lock);   // RW-4 SA-F1: mounts[]/binds[]/root_spoor guard
    p->dot_path   = NULL;   // NULL == cwd "/" (allocated lazily on first chdir)
    // KP_ZERO already cleared binds[] and mounts[]; nothing more to do.
}

void territory_init(void) {
    if (g_pgrp_cache) extinction("territory_init called twice");

    g_pgrp_cache = kmem_cache_create("territory",
                                     sizeof(struct Territory),
                                     8,
                                     KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_pgrp_cache) {
        extinction("kmem_cache_create(territory) returned NULL");
    }

    g_kpgrp = kmem_cache_alloc(g_pgrp_cache, KP_ZERO);
    if (!g_kpgrp) extinction("kmem_cache_alloc(kpgrp) failed");
    territory_init_fields(g_kpgrp);
    __atomic_fetch_add(&g_pgrp_created, 1u, __ATOMIC_RELAXED);
}

struct Territory *kpgrp(void) {
    return g_kpgrp;
}

struct Territory *territory_alloc(void) {
    if (!g_pgrp_cache) extinction("territory_alloc before territory_init");
    struct Territory *p = kmem_cache_alloc(g_pgrp_cache, KP_ZERO);
    if (!p) return NULL;
    territory_init_fields(p);
    __atomic_fetch_add(&g_pgrp_created, 1u, __ATOMIC_RELAXED);
    return p;
}

struct Territory *territory_clone(struct Territory *parent) {
    if (!parent)                  extinction("territory_clone(NULL)");
    if (parent->magic != PGRP_MAGIC)
        extinction("territory_clone of corrupted Territory");
    // R5-H F92: defense-in-depth against single-bit-flip / partial-
    // corruption that leaves magic intact but corrupts the count
    // fields. The bound is fixed at compile time; validating here
    // catches a torn nbinds/nmounts before the deep-copy loop reads
    // past the array.
    if ((unsigned)parent->nbinds > PGRP_MAX_BINDS)
        extinction("territory_clone: parent->nbinds out of range (corrupted Territory)");
    if ((unsigned)parent->nmounts > PGRP_MAX_MOUNTS)
        extinction("territory_clone: parent->nmounts out of range (corrupted Territory)");

    struct Territory *child = territory_alloc();
    if (!child) return NULL;

    // RW-4 SA-F1: the bind/mount/root deep-copy reads parent's tables -- take
    // parent->ns_lock so a concurrent mount/unmount/pivot on a peer thread (or an
    // rfork(RFNAMEG)-sharing Proc) cannot tear the copy or free a source mid-ref.
    // spoor_ref under the lock is safe (atomic, non-sleeping). Released after the
    // root copy, before the dot_path copy (which takes the separate dot_lock).
    spin_lock(&parent->ns_lock);

    // Deep-copy the bind table. Models the spec's ForkClone action's
    // bindings update: the child gets an independent function value.
    child->nbinds = parent->nbinds;
    for (int i = 0; i < parent->nbinds; i++) {
        child->binds[i] = parent->binds[i];
    }

    // Deep-copy the mount table. For each cloned entry, spoor_ref(source)
    // — each Territory holding an entry contributes one reference per
    // entry. Models the spec's ForkClone refcount update:
    //   refcount' = refcount + Cardinality(entries-in-parent-pointing-at-s)
    //
    // spoor_ref extincts on a NULL or corrupted source — that's a kernel
    // invariant violation (we put a corrupted Spoor into the mount table)
    // and the extinct is the correct response.
    child->nmounts = parent->nmounts;
    for (int i = 0; i < parent->nmounts; i++) {
        child->mounts[i] = parent->mounts[i];
        spoor_ref(child->mounts[i].source);
        // #66 (I-33): the cloned entry SHARES the parent's mount-point name --
        // one more Path ref per clone, dropped at this child's unmount /
        // final-release. path_ref is NULL-safe (a nameless mount stays nameless).
        path_ref(child->mounts[i].mp_path);
    }

    // Deep-copy the root_spoor pivot (P5-stratumd-stub-bringup-e2). Each
    // clone is its own holder; spoor_ref(parent->root_spoor) here is the
    // analog of the per-mount-entry ref bump above. Models the spec's
    // ForkClone refcount update: + (IF root_spoor[parent] = s THEN 1
    // ELSE 0).
    child->root_spoor = parent->root_spoor;
    if (child->root_spoor) {
        spoor_ref(child->root_spoor);
    }
    spin_unlock(&parent->ns_lock);

    // LS-4: copy the cwd snapshot (POSIX fork semantics -- the child inherits
    // the parent's cwd, independent thereafter). Take the PARENT's dot_lock so
    // a concurrent chdir on a parent thread cannot free dot_path under us; a
    // race is benign (either pre- or post-chdir cwd is a valid snapshot).
    // kmalloc UNDER the leaf lock is OK -- SLUB alloc is bounded + non-sleeping.
    // On OOM, territory_unref the child (drops the mount/root refs just taken)
    // and fail; the parent is unchanged.
    spin_lock(&parent->dot_lock);
    if (parent->dot_path) {
        u64 len = 0;
        while (parent->dot_path[len] != '\0') len++;
        char *dup = kmalloc(len + 1, 0);
        if (!dup) {
            spin_unlock(&parent->dot_lock);
            territory_unref(child);
            return NULL;
        }
        for (u64 i = 0; i <= len; i++) dup[i] = parent->dot_path[i];
        child->dot_path = dup;
    }
    spin_unlock(&parent->dot_lock);
    return child;
}

void territory_ref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_ref of corrupted Territory");
    __atomic_fetch_add(&p->ref, 1, __ATOMIC_RELAXED);
}

void territory_unref(struct Territory *p) {
    if (!p) return;
    if (p->magic != PGRP_MAGIC)   extinction("territory_unref of corrupted Territory");
    if (p == g_kpgrp)             extinction("territory_unref attempted on kpgrp");
    int pre = __atomic_fetch_sub(&p->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0)                 extinction("territory_unref of zero-ref Territory");
    if (pre == 1) {
        // Final release. Drop each mount entry's source refcount BEFORE
        // freeing the Territory's storage. Models the spec's
        // "DestroyTerritory requires mounts[p] = {}" precondition: the
        // impl satisfies it by iterating + spoor_unref'ing each entry
        // here, which is equivalent to a sequence of Unmount actions
        // immediately preceding the destroy.
        //
        // If this loop is skipped (the BUGGY_DESTROY_LEAK class), each
        // source's refcount stays bumped while the entries vanish from
        // mounts[] — the Spoor's storage is leaked. The spec's
        // MountRefcountConsistency invariant catches that desync.
        //
        // Use spoor_clunk (Plan 9 cclose) not spoor_unref: if this is
        // the Spoor's LAST holder (e.g., the user already closed any
        // attach_9p fd; this Territory was the last mount-table entry
        // for it), the Dev's close hook needs to run to release
        // per-Spoor Dev state (pipe endpoints, 9P sessions, etc.).
        // P5-mount-syscall closed this gap; pre-fix, spoor_unref's
        // close-less last-drop would have leaked Dev state on
        // Territory destruction.
        //
        // Walk in reverse so the array indexing stays valid even if we
        // ever add side-effects that clear the slot during the close
        // path; current Dev close hooks don't touch the Territory, so
        // the direction is cosmetic.
        for (int i = p->nmounts - 1; i >= 0; i--) {
            // #66 (I-33): drop the entry's mount-point name ref (the matched
            // counterpart to the path_ref at mount()/MREPL/clone). path_unref is
            // NULL-safe + non-sleeping (refcount + kfree, no Dev close hook), so
            // it needs no defer-outside-lock; here at final release there is no
            // concurrent accessor anyway.
            path_unref(p->mounts[i].mp_path);
            p->mounts[i].mp_path = NULL;
            spoor_clunk(p->mounts[i].source);
        }
        // Clear nmounts so a post-free read (UAF) sees consistent state
        // (zero entries) — SLUB's freelist write will clobber magic but
        // not the count fields immediately; defensive.
        p->nmounts = 0;

        // Drop the root_spoor pivot's per-Territory ref. Mirrors the
        // mount-entry discipline above: spoor_clunk (not spoor_unref) so
        // the Dev's close hook runs if this was the last holder
        // (e.g., the user already closed any attach_9p fd; this Territory
        // was the only remaining holder of the pivoted root). Same
        // rationale as P5-mount-syscall's mount-table-drop fix.
        // P5-stratumd-stub-bringup-e2.
        if (p->root_spoor) {
            struct Spoor *r = p->root_spoor;
            p->root_spoor = NULL;
            spoor_clunk(r);
        }

        // LS-4: free the cwd string (NULL == "/", no-op). Final release:
        // ref == 0, no concurrent accessor, so no dot_lock is needed.
        if (p->dot_path) {
            kfree(p->dot_path);
            p->dot_path = NULL;
        }

        kmem_cache_free(g_pgrp_cache, p);
        __atomic_fetch_add(&g_pgrp_destroyed, 1u, __ATOMIC_RELAXED);
    }
}

// =============================================================================
// Per-Proc cwd ("dot") -- LS-4. See <thylacine/territory.h> + LIFE-SUPPORT.md
// LS-4 + STALK-DESIGN.md 4.3. Name-based: dot_path is a cleaned absolute path
// string, NULL == "/". The lexical resolver keeps stalk's I-28 containment
// untouched -- it always hands stalk an absolute-from-root path; ".." is
// resolved here lexically and re-clamped at root_spoor by stalk regardless.
// =============================================================================

int cwd_lexical_resolve(const char *dot, const char *input, u64 inlen,
                        char *out, u64 outcap) {
    if (!input || !out || outcap < 2) return -1;

    u64 olen = 0;       // length of the absolute path built so far; 0 == "/"
    out[0] = '\0';

    // A relative input is seeded with the cwd's components; an absolute input
    // ignores the cwd. `dot` is already a cleaned absolute path (or NULL/"/").
    int absolute = (inlen > 0 && input[0] == '/');
    if (!absolute && dot && dot[0] == '/') {
        u64 i = 0;
        while (dot[i] != '\0') {
            while (dot[i] == '/') i++;
            if (dot[i] == '\0') break;
            u64 s = i;
            while (dot[i] != '\0' && dot[i] != '/') i++;
            u64 clen = i - s;
            // dot is pre-cleaned: every component is real. Append "/comp".
            if (olen + 1 + clen + 1 > outcap) return -1;
            out[olen++] = '/';
            for (u64 k = 0; k < clen; k++) out[olen++] = dot[s + k];
            out[olen] = '\0';
        }
    }

    // Process the input's components, resolving "." / ".." lexically.
    u64 i = 0;
    while (i < inlen) {
        while (i < inlen && input[i] == '/') i++;
        if (i >= inlen) break;
        u64 s = i;
        while (i < inlen && input[i] != '/') i++;
        u64 clen = i - s;

        if (clen == 1 && input[s] == '.') continue;                  // "."
        if (clen == 2 && input[s] == '.' && input[s + 1] == '.') {   // ".."
            // Pop the last component; clamped at root (olen never goes < 0).
            while (olen > 0 && out[olen - 1] != '/') olen--;
            if (olen > 0) olen--;            // drop the separating '/'
            out[olen] = '\0';
            continue;
        }
        if (olen + 1 + clen + 1 > outcap) return -1;                 // "/comp\0"
        out[olen++] = '/';
        for (u64 k = 0; k < clen; k++) out[olen++] = input[s + k];
        out[olen] = '\0';
    }

    if (olen == 0) { out[0] = '/'; out[1] = '\0'; olen = 1; }        // "/" root
    return (int)olen;
}

int territory_resolve_cwd(struct Territory *p, const char *input, u64 inlen,
                          char *out, u64 outcap) {
    if (!p)                       return -1;
    if (p->magic != PGRP_MAGIC)   extinction("territory_resolve_cwd of corrupted Territory");
    // Read dot_path UNDER the leaf lock; the lexical resolve is bounded CPU
    // (no alloc, no block), so holding dot_lock across it is safe.
    spin_lock(&p->dot_lock);
    int r = cwd_lexical_resolve(p->dot_path, input, inlen, out, outcap);
    spin_unlock(&p->dot_lock);
    return r;
}

int territory_getdot(struct Territory *p, char *buf, u64 cap) {
    if (!p || !buf || cap == 0)   return -1;
    if (p->magic != PGRP_MAGIC)   extinction("territory_getdot of corrupted Territory");
    spin_lock(&p->dot_lock);
    const char *src = (p->dot_path && p->dot_path[0]) ? p->dot_path : "/";
    u64 len = 0;
    while (src[len] != '\0') len++;
    if (len + 1 > cap) { spin_unlock(&p->dot_lock); return -1; }
    for (u64 i = 0; i < len; i++) buf[i] = src[i];
    buf[len] = '\0';
    spin_unlock(&p->dot_lock);
    return (int)len;
}

int territory_setdot(struct Territory *p, const char *cleaned) {
    if (!p || !cleaned)           return -1;
    if (p->magic != PGRP_MAGIC)   extinction("territory_setdot of corrupted Territory");

    // "/" is stored as the NULL sentinel -- no allocation. kmalloc the new copy
    // BEFORE taking dot_lock (the dup is private until installed).
    char *dup = NULL;
    if (!(cleaned[0] == '/' && cleaned[1] == '\0')) {
        u64 len = 0;
        while (cleaned[len] != '\0') len++;
        dup = kmalloc(len + 1, 0);
        if (!dup) return -1;
        for (u64 i = 0; i <= len; i++) dup[i] = cleaned[i];
    }

    spin_lock(&p->dot_lock);
    char *old = p->dot_path;
    p->dot_path = dup;
    spin_unlock(&p->dot_lock);

    // Free the old string OUTSIDE the lock. Safe: readers copy dot_path under
    // dot_lock and never retain the pointer past their critical section.
    if (old) kfree(old);
    return 0;
}

int territory_nbinds(struct Territory *p) {
    if (!p) return 0;
    return p->nbinds;
}

int territory_nmounts(struct Territory *p) {
    if (!p) return 0;
    return p->nmounts;
}

u64 territory_total_created(void)   { return __atomic_load_n(&g_pgrp_created, __ATOMIC_RELAXED); }
u64 territory_total_destroyed(void) { return __atomic_load_n(&g_pgrp_destroyed, __ATOMIC_RELAXED); }

// =============================================================================
// /proc/<pid>/ns rendering (#66). Tiny append helpers -- each returns false on
// overflow (the renderer then stops; the list is best-effort introspection per
// I-33). *off advances only on a fit.
// =============================================================================

static bool ns_put_str(char *buf, u64 cap, u64 *off, const char *s) {
    u64 o = *off;
    for (u64 i = 0; s[i] != '\0'; i++) {
        if (o >= cap) return false;
        buf[o++] = s[i];
    }
    *off = o;
    return true;
}

// Append a bounded byte run (a Path string `s` is NOT necessarily reread as
// NUL-terminated here -- we copy exactly `n` bytes, the cached path->len).
static bool ns_put_bytes(char *buf, u64 cap, u64 *off, const char *s, u64 n) {
    if (*off + n > cap) return false;
    for (u64 i = 0; i < n; i++) buf[*off + i] = s[i];
    *off += n;
    return true;
}

static bool ns_put_udec(char *buf, u64 cap, u64 *off, u64 v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (*off + (u64)n > cap) return false;
    for (int i = 0; i < n; i++) buf[(*off)++] = tmp[n - 1 - i];
    return true;
}

u64 territory_format_ns(struct Territory *p, char *buf, u64 cap) {
    if (!p || !buf || cap == 0)   return 0;
    if (p->magic != PGRP_MAGIC)   extinction("territory_format_ns of corrupted Territory");

    u64 off = 0;
    // Read the mount table UNDER ns_lock: while held, each entry's source +
    // mp_path are ref-pinned (and source->path is ref-pinned by source + is an
    // immutable string), so every byte copied below is stable. ns_lock is a
    // near-leaf; the bounded buffer writes take no further lock + cannot sleep.
    // The g_proc_table_lock -> ns_lock edge the devproc caller introduces is
    // acyclic (no ns_lock holder takes g_proc_table_lock).
    spin_lock(&p->ns_lock);
    bool truncated = false;
    for (int i = 0; i < p->nmounts; i++) {
        struct PgrpMount *m = &p->mounts[i];
        // Render a COMPLETE "mount <pt> <src>\n" line or NONE: snapshot off, and
        // on any overflow rewind to discard the partial line (#66b audit F2 --
        // so a truncated list never leaves a half "mount " prefix that the
        // trailing "binds:" then concatenates into a malformed record). The
        // output is therefore always a sequence of whole lines.
        u64 line_start = off;
        bool ok = ns_put_str(buf, cap, &off, "mount ");

        // Mount-point name (the Plan 9 Chan.path of the dir mounted onto). NULL
        // mp_path -> "?" (a kernel-internal mountpoint with no retained name).
        if (ok) {
            if (m->mp_path && m->mp_path->len)
                ok = ns_put_bytes(buf, cap, &off, m->mp_path->s, m->mp_path->len);
            else
                ok = ns_put_str(buf, cap, &off, "?");
        }
        if (ok) ok = ns_put_str(buf, cap, &off, " ");

        // Source label: the source Spoor's namespace name when it has one (a
        // mounted sub-tree); else "#<dc>" -- the Plan 9 device spec (a device
        // root has no namespace path; '#9'=9P, '#s'=srv, '#p'=proc, ...).
        if (ok) {
            struct Spoor *src = m->source;
            if (src && src->path && src->path->len) {
                ok = ns_put_bytes(buf, cap, &off, src->path->s, src->path->len);
            } else {
                char dev[3];
                dev[0] = '#';
                dev[1] = src ? (char)src->dc : '?';
                dev[2] = '\0';
                ok = ns_put_str(buf, cap, &off, dev);
            }
        }
        if (ok) ok = ns_put_str(buf, cap, &off, "\n");

        if (!ok) { off = line_start; truncated = true; break; }   // discard partial
    }
    // Bind count (binds[] are abstract path_id_t at v1.0 -- no string names to
    // render; the count keeps the file's pre-#66 contract). Emit it ONLY when the
    // mount list rendered in full -- a "binds:" line after a truncated list would
    // falsely imply completeness. Rewind a partial binds line too (F2).
    int nb = p->nbinds;
    spin_unlock(&p->ns_lock);

    if (!truncated) {
        u64 bstart = off;
        if (!(ns_put_str(buf, cap, &off, "binds: ") &&
              ns_put_udec(buf, cap, &off, (u64)(nb < 0 ? 0 : nb)) &&
              ns_put_str(buf, cap, &off, "\n")))
            off = bstart;
    }
    return off;
}

// =============================================================================
// bind / unbind.
// =============================================================================

// Path-in-set helper. Linear scan; PGRP_MAX_BINDS+1 is the worst-case
// `reachable[]` size, so this is at most ~33 comparisons.
static bool path_in(const path_id_t *arr, int n, path_id_t p) {
    for (int i = 0; i < n; i++) {
        if (arr[i] == p) return true;
    }
    return false;
}

// would_create_cycle(territory, src, dst): would adding the edge `dst -> src`
// produce a cycle in territory's bind graph?
//
// Algorithm: starting from {src}, iteratively follow existing edges in
// the walk direction (for each existing edge `e_dst -> e_src`, if
// e_dst is in the reachable set, add e_src). After fixed point, check
// whether `dst` is reachable from `src`. If yes, the new edge `dst ->
// src` would close a cycle: src -> ... -> dst -> (new) -> src.
//
// Maps directly to the spec's WouldCreateCycle.
static bool would_create_cycle(const struct Territory *territory,
                               path_id_t src, path_id_t dst) {
    if (src == dst) return true;

    path_id_t reachable[PGRP_MAX_BINDS + 1];
    int n = 0;
    reachable[n++] = src;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < territory->nbinds; i++) {
            path_id_t e_src = territory->binds[i].src;
            path_id_t e_dst = territory->binds[i].dst;
            if (path_in(reachable, n, e_dst) &&
                !path_in(reachable, n, e_src)) {
                if (n >= PGRP_MAX_BINDS + 1) break;
                reachable[n++] = e_src;
                changed = true;
            }
        }
    }

    return path_in(reachable, n, dst);
}

int bind(struct Territory *territory, path_id_t src, path_id_t dst) {
    if (!territory)                    extinction("bind(NULL territory)");
    if (territory->magic != PGRP_MAGIC)
        extinction("bind on corrupted Territory");

    if (src == dst)                       return -4;   // no table access; pre-lock

    // RW-4 SA-F1: the cycle check + dup scan READ binds[]/nbinds and the append
    // WRITES them -- serialize against a concurrent bind/unbind/clone under ns_lock.
    // No sleeping call inside (pure table RMW), so holding the lock is leaf-safe.
    spin_lock(&territory->ns_lock);
    if (would_create_cycle(territory, src, dst)) { spin_unlock(&territory->ns_lock); return -1; }

    for (int i = 0; i < territory->nbinds; i++) {
        if (territory->binds[i].src == src && territory->binds[i].dst == dst) {
            spin_unlock(&territory->ns_lock);
            return -2;
        }
    }

    if (territory->nbinds >= PGRP_MAX_BINDS) { spin_unlock(&territory->ns_lock); return -3; }

    territory->binds[territory->nbinds].src = src;
    territory->binds[territory->nbinds].dst = dst;
    territory->nbinds++;
    spin_unlock(&territory->ns_lock);
    return 0;
}

int unbind(struct Territory *territory, path_id_t src, path_id_t dst) {
    if (!territory)                    extinction("unbind(NULL territory)");
    if (territory->magic != PGRP_MAGIC)
        extinction("unbind on corrupted Territory");

    spin_lock(&territory->ns_lock);   // RW-4 SA-F1: serialize the binds[] RMW
    for (int i = 0; i < territory->nbinds; i++) {
        if (territory->binds[i].src == src && territory->binds[i].dst == dst) {
            territory->binds[i] = territory->binds[territory->nbinds - 1];
            territory->nbinds--;
            spin_unlock(&territory->ns_lock);
            return 0;
        }
    }
    spin_unlock(&territory->ns_lock);
    return -1;
}

// =============================================================================
// mount / unmount.
// =============================================================================

// Spoor magic check helper. The Spoor's magic is at offset 0; we read it
// without dereferencing through the type (would require <spoor.h>'s full
// struct knowledge — already included). spoor_ref does its own check
// and extincts on mismatch, so we just need to reject NULL early.
//
// Defined inline as a function — caller does:
//   if (!source_is_valid(source)) return -1;
// before any ref-affecting op.
static bool source_is_valid(struct Spoor *s) {
    if (!s) return false;
    // SPOOR_MAGIC check is encapsulated by spoor_ref's own invariant;
    // testing it here would duplicate. Rely on spoor_ref's extinct
    // discipline — if the Spoor is corrupted, the spoor_ref call below
    // will extinct cleanly with a precise message.
    return true;
}

// Mount-point identity match (stalk-2): the Plan 9 (type, dev, qid) triple ==
// (dc, devno, qid.path). All three compare: qid.path is unique only within a
// (dc, devno) instance, so two dev9p sessions (same dc, both root qid.path 0)
// are distinguished by devno.
static inline bool mount_key_eq(const struct PgrpMount *m,
                                const struct Spoor *s) {
    return m->mp_dc == s->dc &&
           m->mp_devno == s->devno &&
           m->mp_qid_path == s->qid.path;
}

// A mount-point / source identity (Plan 9 type+dev+qid). Used by the mount
// cycle check below.
struct mkey { int dc; u32 devno; u64 path; };

static inline bool mkey_eq(struct mkey a, struct mkey b) {
    return a.dc == b.dc && a.devno == b.devno && a.path == b.path;
}
static inline struct mkey spoor_mkey(const struct Spoor *s) {
    struct mkey k = { s->dc, s->devno, s->qid.path };
    return k;
}
static bool mkey_in(const struct mkey *arr, int n, struct mkey k) {
    for (int i = 0; i < n; i++) if (mkey_eq(arr[i], k)) return true;
    return false;
}

// would_create_mount_cycle(t, source, mountpoint): would adding the mount edge
// `key(mountpoint) -> key(source)` (walking onto the mount point crosses to the
// source's tree) create a cycle in `t`'s mount-identity graph? I-3 (mount points
// form a DAG, never a cycle) -- the mount-table analog of would_create_cycle for
// binds (stalk-2 audit F1: I-3 was claimed "by construction" but a self-mount or
// a two-tree oscillation could form a cycle that `cross_mounts` then resolved to
// a silently-wrong endpoint; enforce it here so the invariant actually holds).
//
// Algorithm mirrors would_create_cycle: from `key(source)`, follow existing
// mount edges to a fixed point (an entry whose mount-point key is reachable adds
// its source key); if `key(mountpoint)` is then reachable, the new edge closes a
// cycle (mountpoint -> source -> ... -> mountpoint). The trivial self-mount
// (source identity == mountpoint identity) is the degenerate case.
static bool would_create_mount_cycle(const struct Territory *t,
                                     const struct Spoor *source,
                                     const struct Spoor *mountpoint) {
    struct mkey src = spoor_mkey(source);
    struct mkey dst = spoor_mkey(mountpoint);
    if (mkey_eq(src, dst)) return true;            // self-mount

    struct mkey reach[PGRP_MAX_MOUNTS + 1];
    int n = 0;
    reach[n++] = src;

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < t->nmounts; i++) {
            struct mkey e_mp  = { t->mounts[i].mp_dc, t->mounts[i].mp_devno,
                                  t->mounts[i].mp_qid_path };
            struct mkey e_src = spoor_mkey(t->mounts[i].source);
            if (mkey_in(reach, n, e_mp) && !mkey_in(reach, n, e_src)) {
                if (n >= PGRP_MAX_MOUNTS + 1) break;
                reach[n++] = e_src;
                changed = true;
            }
        }
    }
    return mkey_in(reach, n, dst);
}

int mount(struct Territory *territory, struct Spoor *source,
          struct Spoor *mountpoint, u32 flags) {
    if (!territory)                    extinction("mount(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("mount on corrupted Territory");

    if (!source_is_valid(source))       return -1;
    // The mount point is a transient resolved Spoor; we copy its IDENTITY
    // (dc, devno, qid.path), never retain the Spoor. Reject NULL / corrupted.
    if (!mountpoint)                    return -1;
    if (mountpoint->magic != SPOOR_MAGIC) return -1;

    // RW-4 SA-F1: the cycle check + idempotency scan + MREPL/append all read or
    // write mounts[]/nmounts -- serialize under ns_lock against a concurrent
    // mount/unmount/clone on a peer thread. spoor_ref is leaf-safe inside the
    // lock; the MREPL spoor_clunk(old) is captured under the lock + deferred to
    // OUTSIDE it (the displaced source's Dev close hook may sleep -- never hold a
    // spinlock across it, the dot_lock free-outside-the-lock discipline).
    struct Spoor *to_clunk = NULL;
    int rc;
    spin_lock(&territory->ns_lock);

    // I-3: reject a mount that would create a cycle in the mount-identity graph
    // (a self-mount, or a cross-tree oscillation). stalk-2 audit F1 -- enforce
    // the DAG here rather than relying on cross_mounts' loop bound, so a
    // cyclic mount cannot be installed + then resolve to a wrong endpoint.
    if (would_create_mount_cycle(territory, source, mountpoint)) { rc = -3; goto out; }

    // Idempotency: (key(mountpoint), source) pair already in the table → no-op.
    // Spec: <<path, s>> \notin mounts[p] precondition under the re-keyed
    // identity. Caller sees no refcount bump.
    for (int i = 0; i < territory->nmounts; i++) {
        if (mount_key_eq(&territory->mounts[i], mountpoint) &&
            territory->mounts[i].source == source) {
            rc = 0;
            goto out;
        }
    }

    // MREPL semantics at v1.0: if `flags & MREPL` and an entry at the
    // same mount-point identity exists (with a different source — idempotent
    // same-source handled above), replace the FIRST matching entry. Drop the
    // old source's refcount; install the new source with a fresh ref.
    //
    // MBEFORE/MAFTER/MCREATE union semantics are recorded but treated
    // as "append a new entry" at v1.0; union walking is Phase 5+ once
    // the walk algorithm grows union support.
    if (flags & MREPL) {
        for (int i = 0; i < territory->nmounts; i++) {
            if (mount_key_eq(&territory->mounts[i], mountpoint)) {
                // spoor_clunk (not spoor_unref): MREPL displaces a holder; if this
                // was the last ref on `old`, the Dev's close hook must run to
                // release per-Spoor state (P5-mount-syscall fix). Deferred to `out`.
                to_clunk = territory->mounts[i].source;
                // #66 (I-33): re-capture the mount-point name from the new
                // mountpoint Spoor (same (dc,devno,qid.path) identity, fresh
                // resolve -> latest retained name). ref-NEW-before-unref-OLD so a
                // (degenerate) shared Path object survives the swap.
                struct Path *old_mp = territory->mounts[i].mp_path;
                territory->mounts[i].mp_path = mountpoint->path;
                path_ref(territory->mounts[i].mp_path);
                path_unref(old_mp);
                territory->mounts[i].source = source;
                territory->mounts[i].flags  = flags;
                spoor_ref(source);
                rc = 0;
                goto out;
            }
        }
        // MREPL with no existing entry: fall through to append.
    }

    if (territory->nmounts >= PGRP_MAX_MOUNTS) { rc = -2; goto out; }

    // Take the per-entry reference BEFORE installing the entry. The path
    // below is infallible after the ref, so no rollback is needed.
    spoor_ref(source);

    struct PgrpMount *e = &territory->mounts[territory->nmounts];
    e->source      = source;
    // #66 (I-33): retain the mount-POINT's namespace name for /proc/<pid>/ns.
    // path_ref is NULL-safe (a kernel-internal mountpoint with no retained name
    // -> NULL mp_path -> rendered "?"). Introspection-only: the entry keys on
    // the (dc, devno, qid.path) identity below, never on mp_path.
    e->mp_path     = mountpoint->path;
    path_ref(e->mp_path);
    e->mp_qid_path = mountpoint->qid.path;
    e->mp_dc       = mountpoint->dc;
    e->mp_devno    = mountpoint->devno;
    e->flags       = flags;
    e->_pad        = 0;
    territory->nmounts++;
    rc = 0;

out:
    spin_unlock(&territory->ns_lock);
    if (to_clunk) spoor_clunk(to_clunk);
    return rc;
}

int unmount(struct Territory *territory, struct Spoor *mountpoint) {
    if (!territory)                    extinction("unmount(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("unmount on corrupted Territory");
    if (!mountpoint)                    return -1;
    if (mountpoint->magic != SPOOR_MAGIC) return -1;

    // RW-4 SA-F1: scan + swap-remove under ns_lock against a concurrent
    // mount/unmount/clone; capture the removed source + clunk it OUTSIDE the lock
    // (its Dev close hook may sleep).
    struct Spoor *to_clunk = NULL;
    int rc = -1;
    spin_lock(&territory->ns_lock);
    for (int i = 0; i < territory->nmounts; i++) {
        if (mount_key_eq(&territory->mounts[i], mountpoint)) {
            // spoor_clunk (not spoor_unref) so the Dev's close hook runs if this
            // is the last holder -- the ARCH 9.6.6 lifecycle where the user
            // already closed the attach_9p fd and the mount-table was the only
            // holder keeping the 9P session alive (P5-mount-syscall fix). Deferred
            // to outside the lock.
            to_clunk = territory->mounts[i].source;
            // #66 (I-33): drop the removed entry's mount-point name BEFORE the
            // swap-remove overwrites the slot (else the Path ref leaks). The
            // last entry's own mp_path ref TRANSFERS into slot i via the struct
            // copy -- no ref change for it. path_unref is non-sleeping (no defer).
            path_unref(territory->mounts[i].mp_path);
            // Remove by swapping with the last element. Order within mounts[] is
            // not load-bearing at v1.0; MBEFORE/MAFTER union walking (Phase 5+)
            // introduces an ordering invariant + switches to shift-down then.
            territory->mounts[i] = territory->mounts[territory->nmounts - 1];
            territory->nmounts--;
            rc = 0;
            break;
        }
    }
    spin_unlock(&territory->ns_lock);
    if (to_clunk) spoor_clunk(to_clunk);
    return rc;
}

// mount_lookup -- the `stalk` cross-mount probe (Plan 9 domount). Returns a
// REF-HELD source of the FIRST entry whose mount-point identity matches `probe`'s
// (dc, devno, qid.path), or NULL. RW-4 SA-F1: the lookup + spoor_ref happen
// atomically under ns_lock so a concurrent unmount cannot free the source between
// the lookup and the caller's use. THE CALLER MUST spoor_clunk the result
// (stalk_cross_mounts clunks it after clone_walk_zero mints the crossed Spoor).
struct Spoor *mount_lookup(struct Territory *territory, struct Spoor *probe) {
    if (!territory)                    return NULL;
    if (territory->magic != PGRP_MAGIC) extinction("mount_lookup on corrupted Territory");
    if (!probe)                        return NULL;
    if (probe->magic != SPOOR_MAGIC)   extinction("mount_lookup probe corrupted Spoor");

    struct Spoor *src = NULL;
    spin_lock(&territory->ns_lock);
    for (int i = 0; i < territory->nmounts; i++) {
        if (mount_key_eq(&territory->mounts[i], probe)) {
            src = territory->mounts[i].source;
            spoor_ref(src);   // transfer a ref to the caller (atomic vs unmount)
            break;
        }
    }
    spin_unlock(&territory->ns_lock);
    return src;
}

// =============================================================================
// chroot (root-Spoor pivot) — P5-stratumd-stub-bringup-e2.
// =============================================================================

// Maps to specs/territory.tla::Chroot(p, s). Refcount discipline:
//
//   - if old == NULL: spoor_ref(new); root_spoor = new. (single bump)
//   - if old == new: idempotent no-op; no ref change.
//   - if old != new, old != NULL: spoor_ref(new); root_spoor = new;
//     spoor_clunk(old). (one bump, one drop — matches the spec's
//     two-key EXCEPT update in Chroot.)
//
// spoor_ref BEFORE the pointer swap so a failure (spoor_ref extincts on
// corruption; doesn't fail-soft) leaves root_spoor unchanged. After the
// swap, the old pointer is still locally referenced for the spoor_clunk.
// spoor_clunk (not spoor_unref) for the displaced root: if the previous
// root held its last ref via this Territory's root_spoor, the Dev's
// close hook needs to run to release per-Spoor Dev state — same
// discipline as MREPL displacement in mount().
int territory_chroot(struct Territory *territory, struct Spoor *source) {
    if (!territory)                    extinction("territory_chroot(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("territory_chroot on corrupted Territory");

    if (!source_is_valid(source))       return -1;

    // RW-4 SA-F1: the idempotency check + read-old + ref + swap run under ns_lock
    // so a concurrent FROM_ROOT reader (territory_root_ref) and a peer chroot/pivot
    // cannot observe a torn swap or free `old` mid-read. spoor_ref is leaf-safe
    // inside; the displaced root's spoor_clunk is deferred to OUTSIDE the lock (its
    // Dev close hook may sleep).
    struct Spoor *old = NULL;
    spin_lock(&territory->ns_lock);
    // Idempotent same-pointer: same Spoor reasserted as root -> no-op success;
    // refcount unchanged. Matches spec precondition `root_spoor[p] # s`.
    if (territory->root_spoor == source) {
        spin_unlock(&territory->ns_lock);
        return 0;
    }
    old = territory->root_spoor;
    // Bump BEFORE swap. spoor_ref extincts on a corrupted source; under a clean
    // Spoor the swap is then infallible.
    spoor_ref(source);
    territory->root_spoor = source;
    spin_unlock(&territory->ns_lock);

    if (old) {
        // spoor_clunk (not spoor_unref) -- see header comment + mount() MREPL
        // precedent. If this Territory was the last holder of `old`, the Dev's
        // close hook runs here.
        spoor_clunk(old);
    }
    return 0;
}

// =============================================================================
// Pivot root (long-running-Proc root swap) — P6-pouch-stratumd-boot 16c.
// =============================================================================
//
// Semantics identical to territory_chroot's atomic swap, with one
// added pre-condition: REQUIRES root_spoor != NULL. The semantic
// distinction (pivot vs initial chroot) is enforced at this layer so
// SYS_PIVOT_ROOT cannot be used to establish an initial root on a
// fresh Territory; that's territory_chroot's job. A pivot on a no-root
// Territory is a contract violation and returns -1.
//
// Implementation footprint deliberately mirrors territory_chroot's
// post-precondition path; refactoring to a shared helper is deferred
// to a v1.x cleanup. At v1.0 the two semantics are explicit and
// independent so a future divergence (e.g., v1.x bind-survivor
// semantics in pivot but NOT chroot) lands as a localized change.
int territory_pivot_root(struct Territory *territory, struct Spoor *source) {
    if (!territory)                    extinction("territory_pivot_root(NULL territory)");
    if (territory->magic != PGRP_MAGIC) extinction("territory_pivot_root on corrupted Territory");

    if (!source_is_valid(source))       return -1;

    // Pre-condition: pivot requires an existing root. A Territory with
    // NULL root_spoor has never been chrooted (only kproc + Procs
    // whose ancestor never chrooted are in this state at v1.0; userspace
    // Procs that reach SYS_PIVOT_ROOT will always have a root inherited
    // from kproc's territory_chroot at boot). Returning -1 here makes
    // the syscall fail-closed -- the caller MUST have established a
    // root via SYS_CHROOT first (which all v1.0 Procs do, via kproc).
    // RW-4 SA-F1: the precondition + idempotency + read-old + ref + swap run
    // under ns_lock (so a concurrent FROM_ROOT reader / peer pivot cannot race
    // the check or free `old` mid-read); the displaced root's spoor_clunk is
    // deferred to OUTSIDE the lock (its Dev close hook may sleep).
    struct Spoor *old = NULL;
    spin_lock(&territory->ns_lock);
    if (!territory->root_spoor) {
        spin_unlock(&territory->ns_lock);
        return -1;
    }
    // Idempotent same-pointer: same Spoor reasserted as root -> no-op success;
    // refcount unchanged. Matches territory_chroot + Plan 9 / Linux pivot-to-same.
    if (territory->root_spoor == source) {
        spin_unlock(&territory->ns_lock);
        return 0;
    }
    old = territory->root_spoor;
    // Bump BEFORE swap. spoor_ref extincts on a corrupted source; the swap is
    // then infallible.
    spoor_ref(source);
    territory->root_spoor = source;
    spin_unlock(&territory->ns_lock);

    // spoor_clunk (NOT spoor_unref) on the displaced root: if THIS Territory was
    // the last holder, the Dev's close hook runs. Same discipline as
    // territory_chroot + mount()'s MREPL displacement. For joey's pivot the old
    // devramfs root is held by kproc as well (system-wide; ARCH-invariant for the
    // v1.0 boot path), so this clunk drops joey's per-Territory ref but does NOT
    // free the underlying tree structurally. `old` is non-NULL (precondition).
    spoor_clunk(old);
    return 0;
}

// territory_root_ref -- atomically read root_spoor + take a ref under ns_lock, so
// the read+ref cannot race a concurrent territory_pivot_root / territory_chroot
// that swaps root_spoor + clunks the displaced one to zero (RW-4 SA-F1). Returns
// a REF-HELD root Spoor (caller spoor_clunks it) or NULL if no root is set.
struct Spoor *territory_root_ref(struct Territory *territory) {
    if (!territory)                    return NULL;
    if (territory->magic != PGRP_MAGIC) extinction("territory_root_ref on corrupted Territory");
    spin_lock(&territory->ns_lock);
    struct Spoor *r = territory->root_spoor;
    if (r) spoor_ref(r);
    spin_unlock(&territory->ns_lock);
    return r;
}
