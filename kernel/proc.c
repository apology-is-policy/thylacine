// Process descriptor management (P2-A).
//
// Per ARCHITECTURE.md §7.2 + the proc.h commentary on which fields are
// landing when. P2-A only carries pid + threads list + thread_count;
// subsequent sub-chunks grow the struct by appending fields. SLUB
// caches the descriptor.
//
// Bootstrap order (kernel/main.c calls in this order):
//   1. slub_init      — kmem_cache_create can allocate now
//   2. proc_init      — kproc (PID 0) appears
//   3. thread_init    — kthread (TID 0) appears, parented to kproc

#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_proc_cache;
static struct Proc       *g_kproc;
static int                g_next_pid = 0;
static u64                g_proc_created;
static u64                g_proc_destroyed;

// Initialize a freshly-allocated Proc descriptor. Caller has already
// passed KP_ZERO to kmem_cache_alloc, so all fields are zero/NULL on
// entry; this only sets the non-zero-default values (magic, pid).
// New fields added by P2-B onward inherit zero/NULL via KP_ZERO and
// don't need an explicit setter here unless the default isn't right.
static void proc_init_fields(struct Proc *p, int pid) {
    p->magic = PROC_MAGIC;
    p->pid   = pid;
}

void proc_init(void) {
    if (g_proc_cache) extinction("proc_init called twice");

    g_proc_cache = kmem_cache_create("proc",
                                     sizeof(struct Proc),
                                     8,
                                     KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_proc_cache) {
        extinction("kmem_cache_create(proc) returned NULL");
    }

    g_kproc = kmem_cache_alloc(g_proc_cache, KP_ZERO);
    if (!g_kproc) extinction("kmem_cache_alloc(kproc) failed");
    proc_init_fields(g_kproc, 0);
    g_proc_created++;
    g_next_pid = 1;
}

struct Proc *kproc(void) {
    return g_kproc;
}

struct Proc *proc_alloc(void) {
    if (!g_proc_cache) extinction("proc_alloc before proc_init");
    struct Proc *p = kmem_cache_alloc(g_proc_cache, KP_ZERO);
    if (!p) return NULL;
    proc_init_fields(p, g_next_pid++);
    g_proc_created++;
    return p;
}

void proc_free(struct Proc *p) {
    if (!p)                       extinction("proc_free(NULL)");
    // Magic check catches double-free and corrupt-Proc passes. SLUB's
    // freelist write at kmem_cache_free clobbers magic; subsequent free
    // reads the clobbered value and trips here.
    if (p->magic != PROC_MAGIC)   extinction("proc_free of corrupted or already-freed Proc");
    if (p == g_kproc)             extinction("proc_free attempted on kproc");
    if (p->thread_count)          extinction("proc_free with live threads (caller must drain)");
    if (p->threads)               extinction("proc_free with non-NULL threads list");
    kmem_cache_free(g_proc_cache, p);
    g_proc_destroyed++;
}

u64 proc_total_created(void)   { return g_proc_created; }
u64 proc_total_destroyed(void) { return g_proc_destroyed; }
