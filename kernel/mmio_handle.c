// KObj_MMIO impl (P4-Ib) — hardware-MMIO range claims with overlap
// rejection.
//
// Per <thylacine/mmio_handle.h> + specs/handles.tla. The g_mmio_claims
// table tracks every currently-alive (pa, size, owner) tuple; create
// scans for overlap before allocating; unref releases the slot.
//
// **Lock discipline**: g_mmio_lock guards g_mmio_claims for all
// reads + writes. Acquired in IRQ-safe spinlock mode — kobj_mmio_create
// can be called from kernel-context test code which may be preempted,
// and kobj_mmio_unref is called from handle_close which runs in process
// context. No nested-lock paths (no other locks taken under
// g_mmio_lock); held only for the constant-time scan + insert/release.

#include <thylacine/extinction.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/page.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#include "../arch/arm64/uart.h"
#include "../mm/slub.h"

// Claim table. At v1.0 P4-Ib a static array bounds the number of
// alive KObj_MMIO at KOBJ_MMIO_MAX. Real drivers (P4-Ic+) need a
// handful per driver process; 32 is enough headroom for the immediate
// future. Phase 5+ refactors to a growable RB-tree keyed by PA when
// the system has hundreds of MMIO regions live.
#define KOBJ_MMIO_MAX 32

struct mmio_claim {
    struct KObj_MMIO *owner;   // NULL = slot free; non-NULL = claimed
    u64               pa;       // first byte of the claimed range
    size_t            size;     // byte size; pa + size doesn't overflow
};

static struct mmio_claim g_mmio_claims[KOBJ_MMIO_MAX];
static spin_lock_t       g_mmio_lock = SPIN_LOCK_INIT;
static u64               g_mmio_created;
static u64               g_mmio_live;
static bool              g_mmio_initialized;

u64 kobj_mmio_total_created(void) {
    return __atomic_load_n(&g_mmio_created, __ATOMIC_RELAXED);
}

u64 kobj_mmio_live_count(void) {
    return __atomic_load_n(&g_mmio_live, __ATOMIC_RELAXED);
}

// =============================================================================
// Init.
// =============================================================================

void kobj_mmio_init(void) {
    // R9 F151 (P3) close: atomic init guard. Plain bool check +
    // assignment would race if two CPUs reached kobj_mmio_init
    // simultaneously (hypothetical future per-CPU subsystem_init).
    // __atomic_exchange returns the PREVIOUS value: if it was true,
    // someone got here first → extinct. If false, this caller is the
    // first → proceed.
    if (__atomic_exchange_n(&g_mmio_initialized, true, __ATOMIC_ACQ_REL)) {
        extinction("kobj_mmio_init called twice");
    }

    // KP_ZERO at BSS already left g_mmio_claims all-NULL-owners; no
    // explicit zeroing needed.

    uart_puts("kobj_mmio: claims=");
    uart_putdec((u64)KOBJ_MMIO_MAX);
    uart_puts(" slots\n");
}

// =============================================================================
// Claim helpers (caller holds g_mmio_lock).
// =============================================================================

// Check if [pa, pa+size) overlaps any existing claim. Returns true on
// overlap. Caller MUST hold g_mmio_lock.
//
// Overlap formula: two ranges [a, a+s_a) and [b, b+s_b) overlap iff
// a < b + s_b AND b < a + s_a. Symmetric; handles partial + complete
// overlap + adjacency-not-overlap (b == a + s_a is contiguous-but-distinct).
static bool ranges_overlap(u64 pa, size_t size) {
    u64 end = pa + size;        // caller has overflow-checked
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (!g_mmio_claims[i].owner) continue;
        u64 c_pa = g_mmio_claims[i].pa;
        u64 c_end = c_pa + g_mmio_claims[i].size;
        if (pa < c_end && c_pa < end) return true;
    }
    return false;
}

// Find a free slot. Returns slot index, or -1 if all slots are in use.
// Caller MUST hold g_mmio_lock.
static int find_free_slot(void) {
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (!g_mmio_claims[i].owner) return i;
    }
    return -1;
}

// Find the slot whose owner == k. Returns slot index, or -1 if no
// match. Caller MUST hold g_mmio_lock.
static int find_slot_by_owner(struct KObj_MMIO *k) {
    for (int i = 0; i < KOBJ_MMIO_MAX; i++) {
        if (g_mmio_claims[i].owner == k) return i;
    }
    return -1;
}

// =============================================================================
// Lifecycle.
// =============================================================================

struct KObj_MMIO *kobj_mmio_create(u64 pa, size_t size) {
    if (!g_mmio_initialized)              return NULL;
    if (size == 0)                        return NULL;
    if ((pa & (PAGE_SIZE - 1)) != 0)      return NULL;
    if ((size & (PAGE_SIZE - 1)) != 0)    return NULL;
    // Overflow check: pa + size must not wrap.
    if (size > (u64)-1 - pa)              return NULL;

    struct KObj_MMIO *k = kmalloc(sizeof(*k), KP_ZERO);
    if (!k) return NULL;

    k->magic = KOBJ_MMIO_MAGIC;
    k->pa    = pa;
    k->size  = size;
    k->ref   = 1;

    irq_state_t s = spin_lock_irqsave(&g_mmio_lock);

    // R9 F147 (P2) — rollback-asymmetry note:
    // Both failure paths below (overlap, slot-full) call kfree(k)
    // directly instead of going through kobj_mmio_unref. This is
    // INTENTIONAL because:
    //   (a) the claim slot was never allocated for k (we failed
    //       BEFORE g_mmio_claims[slot].owner = k), so there's nothing
    //       to release;
    //   (b) the refcount is 1 by construction (just set above) and
    //       no other holder exists yet, so atomic-dec-to-0 would be
    //       a no-op aside from the free.
    // The magic clobber + kfree pair mirrors kobj_mmio_free_internal's
    // tail. Future state added to KObj_MMIO that needs cleanup BEFORE
    // the claim is wired (e.g., a Burrow attachment installed in
    // kobj_mmio_create's body) MUST be torn down here too — adding a
    // single unref call won't work because unref's free_internal
    // expects find_slot_by_owner to succeed. The "unref" discipline
    // applies post-slot-installation only.
    //
    // P4-Ib spec invariant HwResourceExclusive: no two alive KObj_MMIO
    // overlap. Reject before allocating any slot resources.
    if (ranges_overlap(pa, size)) {
        spin_unlock_irqrestore(&g_mmio_lock, s);
        k->magic = 0;          // clobber to mark dead before kfree (defense)
        kfree(k);
        return NULL;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&g_mmio_lock, s);
        k->magic = 0;
        kfree(k);
        return NULL;
    }

    g_mmio_claims[slot].owner = k;
    g_mmio_claims[slot].pa    = pa;
    g_mmio_claims[slot].size  = size;

    spin_unlock_irqrestore(&g_mmio_lock, s);

    __atomic_fetch_add(&g_mmio_created, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_mmio_live,    1u, __ATOMIC_RELAXED);
    return k;
}

void kobj_mmio_ref(struct KObj_MMIO *k) {
    if (!k)                              extinction("kobj_mmio_ref(NULL)");
    if (k->magic != KOBJ_MMIO_MAGIC)     extinction("kobj_mmio_ref of corrupted KObj_MMIO");

    // R9 F148 (P2) close: atomic ref bump. Without this, two CPUs
    // concurrently calling ref+unref could torn-update the count. The
    // returned old value drives the zero-from-positive check —
    // catching the "ref was already 0" case after the fact (the
    // increment has already happened; we extinct on observation).
    int old = __atomic_fetch_add(&k->ref, 1, __ATOMIC_RELAXED);
    if (old <= 0) {
        extinction("kobj_mmio_ref of zero-ref KObj_MMIO (already freed?)");
    }
}

static void kobj_mmio_free_internal(struct KObj_MMIO *k) {
    if (k->magic != KOBJ_MMIO_MAGIC)
        extinction("kobj_mmio_free_internal of corrupted KObj_MMIO");
    if (k->ref != 0)
        extinction("kobj_mmio_free_internal with ref > 0");

    // Release the claim slot before the kfree so a racing
    // kobj_mmio_create can immediately re-use the PA range.
    irq_state_t s = spin_lock_irqsave(&g_mmio_lock);
    int slot = find_slot_by_owner(k);
    if (slot < 0) {
        spin_unlock_irqrestore(&g_mmio_lock, s);
        extinction("kobj_mmio_free_internal: no claim slot for owner (UAF or double-free?)");
    }
    g_mmio_claims[slot].owner = NULL;
    g_mmio_claims[slot].pa    = 0;
    g_mmio_claims[slot].size  = 0;
    spin_unlock_irqrestore(&g_mmio_lock, s);

    // Defensive: clobber magic before kfree so a stale-pointer
    // dereference between free and SLUB-list-write extincts on the
    // magic check.
    k->magic = 0;

    kfree(k);
    __atomic_fetch_sub(&g_mmio_live, 1u, __ATOMIC_RELAXED);
}

void kobj_mmio_unref(struct KObj_MMIO *k) {
    if (!k) return;
    if (k->magic != KOBJ_MMIO_MAGIC)
        extinction("kobj_mmio_unref of corrupted KObj_MMIO");

    // R9 F148 (P2) close: atomic ref decrement. ACQ_REL ordering on
    // the dec ensures (a) all prior accesses to *k by ANY CPU happen
    // before the dec is observed (release on the decrement), and (b)
    // the post-dec free_internal sees the final state of *k coherently
    // (acquire on the returned value's interpretation).
    //
    // The returned OLD value drives the decision to free: only the
    // caller that observed old==1 (i.e., dec was 1→0) calls
    // free_internal. Concurrent decrements that produced old==2 or
    // higher don't see the zero edge — guaranteed by atomic semantics.
    int old = __atomic_fetch_sub(&k->ref, 1, __ATOMIC_ACQ_REL);
    if (old <= 0) {
        extinction("kobj_mmio_unref of zero-ref KObj_MMIO (double-free?)");
    }
    if (old == 1) {
        kobj_mmio_free_internal(k);
    }
}

void kobj_mmio_destroy(struct KObj_MMIO *k) {
    kobj_mmio_unref(k);
}
