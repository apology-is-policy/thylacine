// Path -- refcounted, copy-on-walk namespace-name strings (#66). See
// <thylacine/path.h> + ARCHITECTURE.md section 9.6.9 + invariant I-33.
//
// Mirrors spoor.c's refcount discipline: the atomic `ref` extincts on a <= 0
// pre-value (use-after-free / double-free diagnostic); cumulative counters let
// tests verify the alloc/free balance without dereferencing freed storage.
//
// The string is IMMUTABLE once built -- path_addelem / path_parent ALWAYS
// allocate a fresh Path, never mutate a shared one (copy-on-walk). So the only
// concurrently-mutated field is `ref`; the string is safe to read locklessly.

#include <thylacine/path.h>

#include <thylacine/extinction.h>
#include <thylacine/syscall.h>    // SYS_OPEN_PATH_MAX
#include <thylacine/types.h>
#include <atomic_lse.h>           // t_atomic_fetch_{add,sub}_acqrel_int

#include "../mm/slub.h"

static u64 g_path_allocated;
static u64 g_path_freed;

// path_alloc_raw -- kmalloc a Path with room for `len` chars + NUL, ref == 1,
// len set, s[] left zeroed (KP_ZERO -> the trailing NUL is already in place).
// The caller fills s[0..len). NULL on OOM or len > SYS_OPEN_PATH_MAX.
static struct Path *path_alloc_raw(u32 len) {
    if (len > SYS_OPEN_PATH_MAX) return NULL;
    struct Path *p = kmalloc(sizeof(struct Path) + (size_t)len + 1, KP_ZERO);
    if (!p) return NULL;
    // The Path isn't published to any other CPU until the caller stores it into
    // a Spoor's ->path (an observable location); a relaxed init store is safe.
    __atomic_store_n(&p->ref, 1, __ATOMIC_RELAXED);
    p->len = len;
    g_path_allocated++;
    return p;
}

struct Path *path_make_root(void) {
    struct Path *p = path_alloc_raw(1);
    if (!p) return NULL;
    p->s[0] = '/';
    return p;
}

struct Path *path_parent(const struct Path *p) {
    if (!p) return NULL;
    // Paths always begin with '/', so there is at least the leading separator.
    // The parent is everything up to the LAST '/', or the root "/" if that last
    // '/' is the leading one ("/a/b" -> "/a"; "/a" -> "/"; "/" -> "/").
    u32 last = 0;
    for (u32 i = 0; i < p->len; i++)
        if (p->s[i] == '/') last = i;
    u32 plen = (last == 0) ? 1u : last;
    struct Path *np = path_alloc_raw(plen);
    if (!np) return NULL;
    if (last == 0) {
        np->s[0] = '/';
    } else {
        for (u32 i = 0; i < plen; i++) np->s[i] = p->s[i];
    }
    return np;
}

struct Path *path_addelem(const struct Path *parent, const char *name, u64 namelen) {
    if (!parent || !name) return NULL;
    // An empty component would build a trailing-slash path ("/a" + "" -> "/a/").
    // The kernel never reaches here with namelen == 0 (stalk guarantees clen >= 1;
    // the walk-open / walk-create handlers reject name_len == 0), but reject it so
    // the helper is total. A single component also cannot exceed the whole-path
    // bound; reject early so the newlen arithmetic below cannot overflow.
    if (namelen == 0 || namelen > SYS_OPEN_PATH_MAX) return NULL;

    // "." -- a no-op step: a fresh copy of parent (the child sits at the same
    // place + name). stalk never reaches here with "." (the resolver handles
    // it); the single-hop walk-open can, so handle it.
    if (namelen == 1 && name[0] == '.') {
        struct Path *p = path_alloc_raw(parent->len);
        if (!p) return NULL;
        for (u32 i = 0; i < parent->len; i++) p->s[i] = parent->s[i];
        return p;
    }
    // ".." -- pop the last element.
    if (namelen == 2 && name[0] == '.' && name[1] == '.') {
        return path_parent(parent);
    }

    // A real component. Result = parent + "/" + name, EXCEPT at the root, where
    // parent is "/" and the separator is not doubled ("/" + "x" -> "/x").
    bool parent_is_root = (parent->len == 1 && parent->s[0] == '/');
    u64 newlen64 = parent_is_root ? (1 + namelen)
                                  : ((u64)parent->len + 1 + namelen);
    if (newlen64 > SYS_OPEN_PATH_MAX) return NULL;   // over-long -> "unknown"; walk still succeeds
    u32 newlen = (u32)newlen64;

    struct Path *p = path_alloc_raw(newlen);
    if (!p) return NULL;
    u32 off = 0;
    if (parent_is_root) {
        p->s[off++] = '/';
    } else {
        for (u32 i = 0; i < parent->len; i++) p->s[off++] = parent->s[i];
        p->s[off++] = '/';
    }
    for (u64 i = 0; i < namelen; i++) p->s[off++] = name[i];
    // off == newlen; s[newlen] is already NUL (KP_ZERO).
    return p;
}

void path_ref(struct Path *p) {
    if (!p) return;                                  // NULL == "unknown name"; valid no-op
    int pre = t_atomic_fetch_add_acqrel_int(&p->ref, 1);
    if (pre <= 0)
        extinction("path_ref of zero-ref Path (already freed?)");
}

void path_unref(struct Path *p) {
    if (!p) return;                                  // NULL-safe
    int pre = t_atomic_fetch_sub_acqrel_int(&p->ref, 1);
    if (pre <= 0)
        extinction("path_unref of zero-ref Path");
    if (pre == 1) {
        kfree(p);
        g_path_freed++;
    }
}

u64 path_total_allocated(void) { return g_path_allocated; }
u64 path_total_freed(void)     { return g_path_freed; }
