// Loom -- the KObj_Loom ring substrate (Loom-2a).
//
// Per kernel/include/thylacine/loom.h + docs/LOOM.md. This file owns the ring
// memory + geometry + the registered-handle table + the kobj refcount. The
// syscall inners that map the ring into a Proc + install the handle live in
// kernel/syscall.c (the sys_burrow_attach_for_proc factoring); the engine
// pluggable-completion seam + the SQE dispatch + the CQE post are Loom-2b /
// Loom-3.
//
// The ring lives in one anonymous Burrow: a 64-byte loom_ring_hdr at offset 0,
// then the SQ index ring (u32[sq_entries]), the SQE array, and the CQE array,
// each 64-aligned, the whole page-rounded. The kernel reaches the bytes via the
// Burrow's direct-map alias (exec.c precedent); userspace sees the same pages
// through its mapping. The dual-refcount discipline (#847) keeps the pages
// alive while EITHER the Loom (handle_count) OR the user mapping (mapping_count)
// holds a reference.

#include <thylacine/loom.h>
#include <thylacine/burrow.h>
#include <thylacine/page.h>
#include <thylacine/spoor.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#include "../mm/slub.h"

_Static_assert(LOOM_MAGIC == 0x4C4F4F4D52494E47ULL, "loom magic drift");
_Static_assert((LOOM_MAX_ENTRIES & (LOOM_MAX_ENTRIES - 1u)) == 0,
               "LOOM_MAX_ENTRIES must be a power of two");

// Cumulative diagnostics (tests assert on the deltas). Atomic so the SMP
// matrix's concurrent create/destroy don't tear the counts.
static u64 g_loom_created;
static u64 g_loom_destroyed;

u64 loom_total_created(void)   { return __atomic_load_n(&g_loom_created, __ATOMIC_RELAXED); }
u64 loom_total_destroyed(void) { return __atomic_load_n(&g_loom_destroyed, __ATOMIC_RELAXED); }

static u32 align_up_u32(u32 x, u32 a) { return (x + (a - 1u)) & ~(a - 1u); }

static bool is_pow2_u32(u32 x) { return x != 0u && (x & (x - 1u)) == 0u; }

struct Loom *loom_create(u32 sq_entries, u32 cq_entries) {
    if (!is_pow2_u32(sq_entries) || sq_entries > LOOM_MAX_ENTRIES)  return NULL;
    if (!is_pow2_u32(cq_entries))                                   return NULL;
    if (cq_entries < sq_entries || cq_entries > 2u * LOOM_MAX_ENTRIES) return NULL;

    // Geometry. Each region 64-aligned (cache line); the whole ring
    // page-rounded. All sizes are bounded (sq/cq <= 2*LOOM_MAX_ENTRIES, each
    // entry <= 64 B) so the u32 arithmetic cannot overflow: the largest ring is
    // ~ 16 KiB (sq index) + 256 KiB (sqes) + 256 KiB (cqes) < 1 MiB.
    u32 hdr_off       = 0;
    u32 sq_array_off  = align_up_u32(hdr_off + (u32)sizeof(struct loom_ring_hdr), 64u);
    u32 sq_array_size = sq_entries * (u32)sizeof(u32);
    u32 sqe_off       = align_up_u32(sq_array_off + sq_array_size, 64u);
    u32 sqe_size      = sq_entries * (u32)sizeof(struct loom_sqe);
    u32 cqe_off       = align_up_u32(sqe_off + sqe_size, 64u);
    u32 cqe_size      = cq_entries * (u32)sizeof(struct loom_cqe);
    u32 ring_end      = cqe_off + cqe_size;
    u32 ring_size     = (ring_end + (PAGE_SIZE - 1u)) & ~((u32)PAGE_SIZE - 1u);

    struct Loom *l = kmalloc(sizeof(struct Loom), KP_ZERO);
    if (!l) return NULL;
    struct Burrow *r = burrow_create_anon((size_t)ring_size);
    if (!r) { kfree(l); return NULL; }

    l->magic    = LOOM_MAGIC;
    l->refcount = 1;
    spin_lock_init(&l->lock);
    l->ring     = r;
    // The ring Burrow is anonymous + physically contiguous (alloc_pages chunk);
    // its direct-map base is stable for the Burrow's lifetime (the Loom holds a
    // handle_count ref so the pages are never freed under us). exec.c sets the
    // precedent for kernel writes through a Burrow's direct-map alias.
    l->ring_kva = (u8 *)pa_to_kva(page_to_pa(r->pages));
    l->sq_entries    = sq_entries;
    l->cq_entries    = cq_entries;
    l->hdr_off       = hdr_off;
    l->sq_array_off  = sq_array_off;
    l->sqe_off       = sqe_off;
    l->cqe_off       = cqe_off;
    l->sq_array_size = sq_array_size;
    l->sqe_size      = sqe_size;
    l->cqe_size      = cqe_size;
    l->ring_size     = ring_size;

    // Stamp the immutable geometry into the shared ring header. The Burrow pages
    // are KP_ZERO, so the head/tail/flags/diagnostics start at 0; only the masks
    // + entry counts are written here. A dsb ish publishes the stores to the
    // inner-shareable domain so a secondary CPU that maps + reads the ring sees
    // them (burrow_create_anon already dsb'd its own zeroing).
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + hdr_off);
    h->sq_mask    = sq_entries - 1u;
    h->sq_entries = sq_entries;
    h->cq_mask    = cq_entries - 1u;
    h->cq_entries = cq_entries;
    __asm__ __volatile__("dsb ish" ::: "memory");

    __atomic_fetch_add(&g_loom_created, 1, __ATOMIC_RELAXED);
    return l;
}

// Last-ref teardown. No concurrent access (refcount hit 0), so no lock is
// taken. Clunking a registered Spoor may sleep (its Dev close hook) -- safe
// here because handle_release_obj runs OUTSIDE the handle-table lock (#844) and
// we hold none. burrow_unref drops the ring's handle_count; if the user mapping
// is already gone (mapping_count == 0) the pages free here, else the VMA
// teardown frees them later (dual-refcount, #847).
static void loom_free(struct Loom *l) {
    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++) {
        if (l->reg[i].spoor) {
            spoor_clunk(l->reg[i].spoor);
            l->reg[i].spoor = NULL;
        }
    }
    if (l->ring) {
        burrow_unref(l->ring);
        l->ring = NULL;
    }
    l->magic = 0;   // clobber before free (UAF defense, mirrors burrow_free_internal)
    kfree(l);
    __atomic_fetch_add(&g_loom_destroyed, 1, __ATOMIC_RELAXED);
}

void loom_ref(struct Loom *l) {
    if (!l || l->magic != LOOM_MAGIC) return;
    __atomic_fetch_add(&l->refcount, 1, __ATOMIC_ACQUIRE);
}

void loom_unref(struct Loom *l) {
    if (!l || l->magic != LOOM_MAGIC) return;
    if (__atomic_fetch_sub(&l->refcount, 1, __ATOMIC_RELEASE) == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        loom_free(l);
    }
}

int loom_register_handles(struct Loom *l, struct Spoor **spoors,
                          const rights_t *rights, u32 n) {
    if (!l || l->magic != LOOM_MAGIC)  return -1;
    if (n > LOOM_MAX_REG_HANDLES)      return -1;
    if (n > 0 && (!spoors || !rights)) return -1;

    // Replace the whole table (IORING_REGISTER_FILES semantics). Snapshot the
    // old Spoors + install the new under the lock, then clunk the old OUTSIDE
    // the lock (spoor_clunk may sleep -- it cannot run under the spin_lock).
    struct Spoor *old[LOOM_MAX_REG_HANDLES];
    spin_lock(&l->lock);
    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++) {
        old[i] = l->reg[i].spoor;
        l->reg[i].spoor  = NULL;
        l->reg[i].rights = 0;
    }
    for (u32 i = 0; i < n; i++) {
        l->reg[i].spoor  = spoors[i];   // adopt the caller's ref
        l->reg[i].rights = rights[i];
    }
    spin_unlock(&l->lock);

    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++) {
        if (old[i]) spoor_clunk(old[i]);
    }
    return 0;
}

int loom_post_cqe(struct Loom *l, u64 user_data, s32 result, u32 flags) {
    if (!l || l->magic != LOOM_MAGIC) return -1;

    spin_lock(&l->lock);
    struct loom_ring_hdr *h = (struct loom_ring_hdr *)(l->ring_kva + l->hdr_off);
    u32 tail = h->cq_tail;                                       // kernel-owned
    u32 head = __atomic_load_n(&h->cq_head, __ATOMIC_ACQUIRE);   // user-owned

    // (tail - head) is the count of posted-unreaped CQEs (free-running u32 --
    // the subtraction is correct across wrap). cq_entries is the capacity.
    if ((u32)(tail - head) >= l->cq_entries) {
        // CQ full: never overwrite an unreaped CQE (CqNeverOverfull, I-29).
        // Count the dropped post; Loom-3's submit-time admission makes this
        // unreachable in production (then `overflow` is a pure diagnostic).
        __atomic_store_n(&h->overflow, h->overflow + 1u, __ATOMIC_RELEASE);
        spin_unlock(&l->lock);
        return -1;
    }

    struct loom_cqe *cqe = (struct loom_cqe *)(l->ring_kva + l->cqe_off);
    u32 idx = tail & h->cq_mask;
    cqe[idx].user_data = user_data;
    cqe[idx].result    = result;
    cqe[idx].flags     = flags;
    // Publish the CQE bytes BEFORE the tail bump (release): a user-side
    // load-acquire of cq_tail then observes a fully-written slot.
    __atomic_store_n(&h->cq_tail, tail + 1u, __ATOMIC_RELEASE);
    spin_unlock(&l->lock);
    return 0;
}
