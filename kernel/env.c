// Per-Proc environment group (ARCH section 9.7 / G15). The kernel side of the
// /env device (devenv.c): the ref-counted Env table, its lock discipline, and
// the per-name primitives devenv drives. See env.h for the model.
//
// Lifetime: the Env is owned 1:1 by its Proc (RFENVG sharing is deferred, so
// `ref` is always 1 at v1.0 -- the field is the forward hook + the territory_-
// unref-shaped double-free guard). It is allocated lazily on the first set
// (env_lazy's CAS closes the peer-thread alloc race), deep-copied on spawn
// (env_clone_into), and freed at proc_free (env_free). Names are inline; each
// value is kmalloc'd individually, so the struct stays modest and a long value
// never needs a large contiguous allocation.

#include <thylacine/env.h>
#include <thylacine/extinction.h>  // extinction (the double-free guard)
#include <thylacine/page.h>     // KP_ZERO
#include <thylacine/proc.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#include "../mm/slub.h"          // kmalloc / kfree

// =============================================================================
// Allocation + teardown of the struct itself.
// =============================================================================

static struct Env *env_alloc(void) {
    struct Env *e = (struct Env *)kmalloc(sizeof(struct Env), KP_ZERO);
    if (!e) return NULL;
    spin_lock_init(&e->lock);
    __atomic_store_n(&e->ref, 1, __ATOMIC_RELAXED);
    e->next_id = 1;                 // 0 is the free-slot sentinel; ids start at 1
    e->count   = 0;
    return e;
}

// Free every live entry's value, then the struct. Used by env_free's last drop,
// env_clone_into's OOM rollback, and env_lazy's CAS loser. Not lock-protected:
// every caller owns the Env exclusively (no peer can reach it -- it is being
// destroyed or was never published).
static void env_destroy(struct Env *e) {
    if (!e) return;
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) {
        if (e->entries[i].value) {
            kfree(e->entries[i].value);
            e->entries[i].value = NULL;
        }
    }
    kfree(e);
}

// Lazily install p->env on the first mutation. The CAS closes the race where two
// peer threads of one Proc both observe env == NULL and both allocate: the loser
// frees its allocation and adopts the winner's. Returns the live Env or NULL on
// OOM.
static struct Env *env_lazy(struct Proc *p) {
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (e) return e;
    struct Env *ne = env_alloc();
    if (!ne) return NULL;
    struct Env *expected = NULL;
    if (__atomic_compare_exchange_n(&p->env, &expected, ne, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return ne;                  // we installed it
    }
    env_destroy(ne);                // lost the race -- the winner is `expected`
    return expected;
}

// =============================================================================
// Small helpers (all callers hold e->lock).
// =============================================================================

static bool name_matches(const char *a, const char *b, u32 b_len) {
    for (u32 i = 0; i < b_len; i++) {
        if (a[i] == '\0' || a[i] != b[i]) return false;
    }
    return a[b_len] == '\0';        // a must end exactly at b_len
}

static int find_by_name_locked(struct Env *e, const char *name, u32 name_len) {
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) {
        if (e->entries[i].id != 0 && name_matches(e->entries[i].name, name, name_len))
            return i;
    }
    return -1;
}

static int find_by_id_locked(struct Env *e, u64 id) {
    if (id == 0) return -1;
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) {
        if (e->entries[i].id == id) return i;
    }
    return -1;
}

static int find_free_locked(struct Env *e) {
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) {
        if (e->entries[i].id == 0) return i;
    }
    return -1;
}

// A valid env var name is a single non-empty component that fits with a NUL and
// carries no '/' or embedded NUL (the resolver passes a single component, but
// devenv re-validates: a hostile direct caller must not smuggle a separator).
static bool name_valid(const char *name, u32 name_len) {
    if (name_len == 0 || name_len >= ENV_NAME_MAX) return false;
    for (u32 i = 0; i < name_len; i++) {
        if (name[i] == '\0' || name[i] == '/') return false;
    }
    return true;
}

// =============================================================================
// Per-name primitives (devenv.c). Each takes e->lock; reads load p->env
// directly (NULL == empty); only env_create lazily allocates.
// =============================================================================

u64 env_lookup(struct Proc *p, const char *name, u32 name_len) {
    if (!p || !name) return 0;
    if (!name_valid(name, name_len)) return 0;
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (!e) return 0;
    spin_lock(&e->lock);
    int i = find_by_name_locked(e, name, name_len);
    u64 id = (i >= 0) ? e->entries[i].id : 0;
    spin_unlock(&e->lock);
    return id;
}

u64 env_create(struct Proc *p, const char *name, u32 name_len) {
    if (!p || !name) return 0;
    if (!name_valid(name, name_len)) return 0;
    struct Env *e = env_lazy(p);
    if (!e) return 0;

    spin_lock(&e->lock);
    int i = find_by_name_locked(e, name, name_len);
    if (i >= 0) {                   // create-or-get: name already exists
        u64 id = e->entries[i].id;
        spin_unlock(&e->lock);
        return id;
    }
    int slot = find_free_locked(e);
    if (slot < 0) {                 // ENV_MAX_ENTRIES reached (DoS floor)
        spin_unlock(&e->lock);
        return 0;
    }
    struct EnvEntry *en = &e->entries[slot];
    en->id    = e->next_id++;
    en->value = NULL;
    en->len   = 0;
    for (u32 k = 0; k < name_len; k++) en->name[k] = name[k];
    en->name[name_len] = '\0';
    e->count++;
    u64 id = en->id;
    spin_unlock(&e->lock);
    return id;
}

void env_truncate(struct Proc *p, u64 id) {
    if (!p) return;
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (!e) return;
    spin_lock(&e->lock);
    int i = find_by_id_locked(e, id);
    if (i >= 0 && e->entries[i].value) {
        kfree(e->entries[i].value);
        e->entries[i].value = NULL;
        e->entries[i].len   = 0;
    }
    spin_unlock(&e->lock);
}

long env_read(struct Proc *p, u64 id, s64 off, void *buf, long n) {
    if (!p || !buf) return -1;
    if (n < 0 || off < 0) return -1;
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (!e) return -1;
    spin_lock(&e->lock);
    int i = find_by_id_locked(e, id);
    if (i < 0) { spin_unlock(&e->lock); return -1; }   // entry gone (monotonic-id)
    struct EnvEntry *en = &e->entries[i];
    if ((u64)off >= en->len) { spin_unlock(&e->lock); return 0; }   // EOF
    long avail = (long)(en->len - (u32)off);
    long cnt   = (n < avail) ? n : avail;
    for (long k = 0; k < cnt; k++) ((u8 *)buf)[k] = (u8)en->value[off + k];
    spin_unlock(&e->lock);
    return cnt;
}

long env_write(struct Proc *p, u64 id, s64 off, const void *buf, long n) {
    if (!p || !buf) return -1;
    if (n < 0 || off < 0) return -1;
    if (n == 0) return 0;
    if ((u64)off + (u64)n > ENV_VALUE_MAX) return -1;   // bound (DoS floor)
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (!e) return -1;

    spin_lock(&e->lock);
    int i = find_by_id_locked(e, id);
    if (i < 0) { spin_unlock(&e->lock); return -1; }    // entry gone
    struct EnvEntry *en = &e->entries[i];

    u32 new_len = (u32)(off + n);
    if (new_len < en->len) new_len = en->len;           // a short write keeps the tail
    // Grow into a fresh buffer (values are exactly `len` bytes -- no spare
    // capacity, so any write that extends past `len` reallocates). kmalloc
    // under the leaf e->lock is the territory_clone dot_path precedent.
    char *nv = (char *)kmalloc(new_len, KP_ZERO);
    if (!nv) { spin_unlock(&e->lock); return -1; }      // OOM
    for (u32 k = 0; k < en->len; k++) nv[k] = en->value[k];   // keep the old prefix/tail
    for (long k = 0; k < n; k++) nv[off + k] = ((const u8 *)buf)[k];
    if (en->value) kfree(en->value);
    en->value = nv;
    en->len   = new_len;
    spin_unlock(&e->lock);
    return n;
}

bool env_unset(struct Proc *p, const char *name, u32 name_len) {
    if (!p || !name) return false;
    if (!name_valid(name, name_len)) return false;
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (!e) return false;
    spin_lock(&e->lock);
    int i = find_by_name_locked(e, name, name_len);
    if (i < 0) { spin_unlock(&e->lock); return false; }
    if (e->entries[i].value) {
        kfree(e->entries[i].value);
        e->entries[i].value = NULL;
    }
    e->entries[i].len = 0;
    e->entries[i].id  = 0;          // free the slot (id never reused -> stable qids)
    e->count--;
    spin_unlock(&e->lock);
    return true;
}

bool env_iter(struct Proc *p, u64 after_id, u64 *out_id, char *name_out,
              u32 name_cap, u32 *name_len_out) {
    if (!p || !out_id || !name_out || name_cap == 0) return false;
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (!e) return false;
    spin_lock(&e->lock);
    // The live entry with the smallest id strictly greater than after_id -- so a
    // readdir resuming from cookie `after_id` advances monotonically and emits
    // strictly-increasing cookies. O(n) per step; n <= ENV_MAX_ENTRIES.
    int best = -1;
    u64 best_id = 0;
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) {
        u64 id = e->entries[i].id;
        if (id != 0 && id > after_id && (best < 0 || id < best_id)) {
            best = i; best_id = id;
        }
    }
    if (best < 0) { spin_unlock(&e->lock); return false; }
    struct EnvEntry *en = &e->entries[best];
    u32 nlen = 0;
    while (nlen < ENV_NAME_MAX && en->name[nlen] != '\0') nlen++;
    if (nlen >= name_cap) nlen = name_cap - 1;          // defensive clamp
    for (u32 k = 0; k < nlen; k++) name_out[k] = en->name[k];
    *out_id = best_id;
    *name_len_out = nlen;
    spin_unlock(&e->lock);
    return true;
}

// =============================================================================
// Lifecycle (proc.c).
// =============================================================================

int env_clone_into(struct Proc *child, struct Proc *parent) {
    if (!parent) return 0;
    struct Env *src = __atomic_load_n(&parent->env, __ATOMIC_ACQUIRE);
    if (!src) return 0;             // parent has no env -> child stays NULL (empty)

    struct Env *ne = env_alloc();
    if (!ne) return -1;             // OOM -> child->env stays NULL; env_free no-ops

    // Deep-copy under the parent's lock (a peer thread of the parent may mutate
    // concurrently). The child is not yet running, so writing ne is unraced; the
    // per-value kmalloc-under-lock mirrors territory_clone's dot_path dup.
    spin_lock(&src->lock);
    ne->next_id = src->next_id;
    for (int i = 0; i < ENV_MAX_ENTRIES; i++) {
        if (src->entries[i].id == 0) continue;          // free slot (ne is KP_ZERO)
        struct EnvEntry *s = &src->entries[i];
        struct EnvEntry *d = &ne->entries[i];           // same slot index (keep layout)
        d->id  = s->id;
        d->len = s->len;
        for (u32 k = 0; k < ENV_NAME_MAX; k++) d->name[k] = s->name[k];
        if (s->len > 0) {
            char *v = (char *)kmalloc(s->len, 0);
            if (!v) {                                   // OOM mid-copy -> roll back
                spin_unlock(&src->lock);
                env_destroy(ne);                        // frees the values copied so far
                return -1;
            }
            for (u32 k = 0; k < s->len; k++) v[k] = s->value[k];
            d->value = v;
        }
        ne->count++;
    }
    spin_unlock(&src->lock);

    __atomic_store_n(&child->env, ne, __ATOMIC_RELEASE);
    return 0;
}

void env_free(struct Proc *p) {
    if (!p) return;
    struct Env *e = __atomic_load_n(&p->env, __ATOMIC_ACQUIRE);
    if (!e) return;
    __atomic_store_n(&p->env, NULL, __ATOMIC_RELEASE);  // a second call no-ops
    int pre = __atomic_fetch_sub(&e->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0) extinction("env_free of zero-ref Env");
    if (pre == 1) env_destroy(e);                       // last owner drops -> free
}
