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
#include <thylacine/proc.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

static struct kmem_cache *g_proc_cache;
static struct Proc       *g_kproc;
static int                g_next_pid = 0;
static u64                g_proc_created;
static u64                g_proc_destroyed;

static void proc_zero_init(struct Proc *p, int pid) {
    p->pid           = pid;
    p->thread_count  = 0;
    p->threads       = NULL;
}

void proc_init(void) {
    if (g_proc_cache) extinction("proc_init called twice");

    g_proc_cache = kmem_cache_create("proc",
                                     sizeof(struct Proc),
                                     8,
                                     KMEM_CACHE_PANIC_ON_FAIL);
    if (!g_proc_cache) {
        // KMEM_CACHE_PANIC_ON_FAIL extincts internally; this is
        // belt-and-braces in case someone changes that flag's
        // semantics later.
        extinction("kmem_cache_create(proc) returned NULL");
    }

    g_kproc = kmem_cache_alloc(g_proc_cache, 0);
    if (!g_kproc) extinction("kmem_cache_alloc(kproc) failed");
    proc_zero_init(g_kproc, 0);
    g_proc_created++;
    g_next_pid = 1;
}

struct Proc *kproc(void) {
    return g_kproc;
}

struct Proc *proc_alloc(void) {
    if (!g_proc_cache) extinction("proc_alloc before proc_init");
    struct Proc *p = kmem_cache_alloc(g_proc_cache, 0);
    if (!p) return NULL;
    proc_zero_init(p, g_next_pid++);
    g_proc_created++;
    return p;
}

void proc_free(struct Proc *p) {
    if (!p)              extinction("proc_free(NULL)");
    if (p == g_kproc)    extinction("proc_free attempted on kproc");
    if (p->thread_count) extinction("proc_free with live threads (caller must drain)");
    if (p->threads)      extinction("proc_free with non-NULL threads list");
    kmem_cache_free(g_proc_cache, p);
    g_proc_destroyed++;
}

u64 proc_total_created(void)   { return g_proc_created; }
u64 proc_total_destroyed(void) { return g_proc_destroyed; }
