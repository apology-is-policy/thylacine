// P3-Ba: ASID allocator implementation.
//
// State machine:
//
//   * `g_next_asid` is a monotonic counter; advances from ASID_USER_FIRST
//     toward ASID_USER_LAST as fresh ASIDs are needed.
//   * `g_asid_free_list` is a LIFO stack of recently-freed ASIDs.
//     asid_alloc prefers popping from the free-list (cache-locality;
//     a recently-freed ASID has its slot warm in caches and its TLB
//     entries already invalidated). asid_free pushes onto the stack.
//   * `g_asid_lock` serializes both — the critical sections are short
//     (a counter increment + array push/pop) and held with IRQs masked
//     because Phase 5+ may free ASIDs from IRQ context (note delivery
//     causing a Proc kill).
//
// TLB flush ordering: asid_free issues the flush BEFORE pushing to the
// free-list. This ensures any subsequent asid_alloc that pops this ASID
// sees a globally-flushed state. The flush sequence is:
//
//   dsb ishst       — make any pending writes visible to TLB walkers.
//   tlbi aside1is   — broadcast invalidate-by-ASID across all PEs.
//   dsb ish         — wait for the broadcast to complete.
//   isb             — discard prefetched / speculatively-translated
//                     instructions on the issuing PE.
//
// This matches Linux's flush_tlb_mm(mm) discipline and ARM ARM D5.10
// recommendation for TLB maintenance.
//
// SMP: at v1.0 the lock is uncontested (single-threaded boot allocates
// ASIDs sequentially via rfork from kthread). Phase 3+ exec on
// secondaries can introduce contention; the lock is sized for that.

#include "asid.h"

#include <thylacine/extinction.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// =============================================================================
// State
// =============================================================================

static spin_lock_t g_asid_lock = SPIN_LOCK_INIT;

// Monotonic counter for fresh ASIDs. Starts at ASID_USER_FIRST; advances
// each time asid_alloc cannot pop from the free-list. Capped at
// ASID_USER_LAST + 1 to signal "no more fresh ASIDs available."
static u16 g_next_asid;

// LIFO free-list. Sized to ASID_USER_MAX so it can hold every possible
// freed ASID without bounds risk. In practice held to a few entries
// because most allocs come straight from the monotonic path until many
// frees stack up.
static u16 g_asid_free_list[ASID_USER_MAX];
static u32 g_asid_free_count;

// Diagnostics. Atomic loads at the read accessors so observers from
// other CPUs see stable snapshots. Increments under g_asid_lock are
// already serialized.
static u32 g_asid_inflight;
static u64 g_asid_total_allocated;
static u64 g_asid_total_freed;

// Initialization guard — `asid_init` may run only once. extinct if
// reinitialized; that would corrupt the live ASID counts.
static bool g_asid_initialized;

// =============================================================================
// Public API
// =============================================================================

void asid_init(void) {
    if (g_asid_initialized) extinction("asid_init called twice");

    g_next_asid             = ASID_USER_FIRST;
    g_asid_free_count       = 0;
    g_asid_inflight         = 0;
    g_asid_total_allocated  = 0;
    g_asid_total_freed      = 0;
    g_asid_initialized      = true;
}

u16 asid_alloc(void) {
    if (!g_asid_initialized) extinction("asid_alloc before asid_init");

    irq_state_t s = spin_lock_irqsave(&g_asid_lock);

    u16 asid;
    if (g_asid_free_count > 0) {
        // Free-list path: pop most-recently-freed ASID. Its TLB was
        // flushed at free time; safe to hand out as-is.
        asid = g_asid_free_list[--g_asid_free_count];
    } else if (g_next_asid <= ASID_USER_LAST) {
        // Monotonic path: hand out a fresh ASID.
        asid = g_next_asid++;
    } else {
        spin_unlock_irqrestore(&g_asid_lock, s);
        extinction("asid_alloc: 8-bit ASID space exhausted at v1.0 "
                   "(generation rollover deferred to Phase 5+)");
    }

    g_asid_inflight++;
    g_asid_total_allocated++;

    spin_unlock_irqrestore(&g_asid_lock, s);
    return asid;
}

void asid_free(u16 asid) {
    if (!g_asid_initialized) extinction("asid_free before asid_init");
    if (asid < ASID_USER_FIRST || asid > ASID_USER_LAST)
        extinction("asid_free: out-of-range ASID");

    // Flush BEFORE returning to pool. Subsequent asid_alloc that pops
    // this slot sees a globally-flushed TLB for this ASID.
    asid_tlb_flush(asid);

    irq_state_t s = spin_lock_irqsave(&g_asid_lock);

    if (g_asid_inflight == 0) {
        spin_unlock_irqrestore(&g_asid_lock, s);
        extinction("asid_free: inflight count would underflow "
                   "(double-free or free-without-alloc?)");
    }
    if (g_asid_free_count >= ASID_USER_MAX) {
        spin_unlock_irqrestore(&g_asid_lock, s);
        extinction("asid_free: free-list overflow (impossible if alloc/free "
                   "are balanced — corruption?)");
    }

    g_asid_free_list[g_asid_free_count++] = asid;
    g_asid_inflight--;
    g_asid_total_freed++;

    spin_unlock_irqrestore(&g_asid_lock, s);
}

void asid_tlb_flush(u16 asid) {
    // ARM ARM D5.10 / ARM IHI 0069F.b §11.5.4: tlbi aside1is invalidates
    // EL1&0 stage-1 TLB entries by ASID, broadcast inner-shareable. The
    // input register encodes ASID in bits [63:48].
    //
    // Sequencing per ARM ARM:
    //   dsb ishst   — drain pending stores so the TLB walker sees them.
    //   tlbi ...    — issue the broadcast invalidate.
    //   dsb ish     — wait for completion across all PEs in the IS domain.
    //   isb         — discard speculative translations on this PE.
    u64 op = (u64)asid << 48;
    __asm__ __volatile__(
        "dsb ishst\n"
        "tlbi aside1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r" (op)
        : "memory"
    );
}

unsigned asid_inflight(void) {
    return __atomic_load_n(&g_asid_inflight, __ATOMIC_RELAXED);
}

u64 asid_total_allocated(void) {
    return __atomic_load_n(&g_asid_total_allocated, __ATOMIC_RELAXED);
}

u64 asid_total_freed(void) {
    return __atomic_load_n(&g_asid_total_freed, __ATOMIC_RELAXED);
}
