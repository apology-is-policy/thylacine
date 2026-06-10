// ASID management: the rolling-ASID allocator (RW-1 B-F1; ARCH section 6.2.1).
//
// The Linux arm64 rolling-ASID model (arch/arm64/mm/context.c) as Thylacine
// adopts it. See asid.h for the design narrative + the safety obligation
// (I-31). This file is validated against specs/asid.tla (model-first); each
// action below names the spec action it realizes.
//
// Bit layout of a context_id (a u64):
//   bits [ASID_BITS-1 : 0]   the hardware ASID value (1 .. (1<<ASID_BITS)-1)
//   bits [63 : ASID_BITS]    the generation (a multiple of ASID_GEN_UNIT)
// generation 0 (context_id == 0) is "never assigned" and always mismatches.
//
// Locking. The fast path (asid_resolve's first half) is lockless: atomics on
// the per-CPU active slot + the global generation, with a cmpxchg that fails
// against a concurrent rollover. The slow path (new_context + flush_context)
// runs under g_asid_lock, a LEAF lock (it takes no other lock; it only touches
// the bitmap, the per-CPU arrays, and issues a local TLB flush). The context-
// switch pre-hook calls asid_resolve with the run-queue lock held, so the lock
// order is rq_lock -> g_asid_lock; nothing acquires rq_lock under g_asid_lock,
// so the order is acyclic.

#include "asid.h"

#include <thylacine/extinction.h>
#include <thylacine/smp.h>          // smp_cpu_idx_self; DTB_MAX_CPUS (via dtb.h)
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

// =============================================================================
// State
// =============================================================================

static spin_lock_t g_asid_lock = SPIN_LOCK_INIT;

// Width-derived constants, set at asid_init from asid_hw_bits().
static unsigned g_asid_bits;        // 8 or 16
static u64      g_asid_val_mask;    // (1 << bits) - 1   -- the hardware ASID field
static u64      g_asid_gen_unit;    // 1 << bits         -- one generation step
static u64      g_asid_last;        // (1 << bits) - 1   -- max user ASID value

// The current generation. Lives in the high bits (low ASID_BITS are zero);
// starts at one ASID_GEN_UNIT (generation #1) so a context_id of 0 misses.
// Read locklessly by the fast path (via __atomic_load_n); mutated only under
// g_asid_lock.
static u64 g_asid_generation;

// Claimed-this-generation bitmap. Sized for the 16-bit ceiling (8 KiB BSS);
// only the low (1<<g_asid_bits) bits are used. Value 0 (kernel ASID) is never
// claimed nor handed out (the find starts at ASID_USER_FIRST).
#define ASID_NUM_MAX    (1u << 16)
#define ASID_MAP_WORDS  (ASID_NUM_MAX / 64u)
static u64 g_asid_map[ASID_MAP_WORDS];
static u64 g_asid_cur_idx;          // round-robin search hint

// Per-CPU rolling state. Sized to DTB_MAX_CPUS; offline CPUs hold zero and
// contribute nothing to flush_context. active_asids publishes "this CPU is
// running this context_id" (0 == none); reserved_asids preserves a CPU's ASID
// across a rollover; flush_pending owes a local TLB flush after a rollover.
static u64  g_active_asids[DTB_MAX_CPUS];
static u64  g_reserved_asids[DTB_MAX_CPUS];
static bool g_flush_pending[DTB_MAX_CPUS];

static u64  g_asid_rollovers;       // diagnostic
static bool g_asid_initialized;

// =============================================================================
// Bitmap helpers (all callers hold g_asid_lock)
// =============================================================================

static inline bool map_test(u64 v) {
    return (g_asid_map[v >> 6] >> (v & 63u)) & 1u;
}
static inline void map_set(u64 v) {
    g_asid_map[v >> 6] |= (1ull << (v & 63u));
}
// test-and-set: returns the PRIOR bit (true if already claimed).
static inline bool map_test_and_set(u64 v) {
    if (map_test(v)) return true;
    map_set(v);
    return false;
}
static void map_clear_all(void) {
    for (u64 w = 0; w <= (g_asid_last >> 6); w++) g_asid_map[w] = 0;
}
// First free ASID value in [max(start, ASID_USER_FIRST), g_asid_last], or
// g_asid_last + 1 if the generation is full.
static u64 map_find_free(u64 start) {
    if (start < ASID_USER_FIRST) start = ASID_USER_FIRST;
    for (u64 v = start; v <= g_asid_last; v++)
        if (!map_test(v)) return v;
    return g_asid_last + 1u;
}

// =============================================================================
// Internals
// =============================================================================

// Generation match: the high (generation) bits of cid equal the global
// generation. Reads the generation locklessly (the fast path).
static inline bool gen_match(u64 cid) {
    return ((cid ^ __atomic_load_n(&g_asid_generation, __ATOMIC_RELAXED))
            >> g_asid_bits) == 0;
}

// Local (this-CPU-only) TLB invalidate of all EL1&0 stage-1 entries. Issued at
// the slow path when this CPU's flush_pending is set: a rollover's reservation
// keeps every still-active ASID, but a CPU may have speculated stale entries in
// the rollover window that the broadcast (inner-shareable) flush did not cover.
// Non-shareable barriers -- this is a CPU-local operation.
static inline void asid_local_tlb_flush(void) {
    __asm__ __volatile__(
        "dsb nshst\n"
        "tlbi vmalle1\n"
        "dsb nsh\n"
        "isb\n"
        ::: "memory");
}

// spec: the rollover's reservation re-stamp (check_update_reserved_asid). If
// the Proc's old context_id is reserved on some CPU, re-stamp that reservation
// to the new generation and report a hit (so new_context keeps the same ASID).
static bool check_update_reserved(u64 old_cid, u64 new_cid) {
    bool hit = false;
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        if (g_reserved_asids[i] != 0 && g_reserved_asids[i] == old_cid) {
            hit = true;
            g_reserved_asids[i] = new_cid;
        }
    }
    return hit;
}

// spec: Rollover (the SlowSwitch rollover branch). Bump already done by the
// caller; here reset the bitmap, preserve every CPU's active (or, if idle, its
// existing reserved) ASID -- the NOSTEAL obligation: a running CPU's ASID is
// never reassigned -- and arm a per-CPU local flush. The xchg of each active
// slot to 0 is the fast-path interlock: a peer whose active slot reads 0 fails
// its fast-path cmpxchg and is forced to the slow path (where flush_pending is
// honored), which is the spec's ~fpend guard realized in hardware.
static void flush_context(void) {
    map_clear_all();
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        u64 a = __atomic_exchange_n(&g_active_asids[i], 0, __ATOMIC_RELAXED);
        if (a == 0) a = g_reserved_asids[i];   // preserve across back-to-back rollovers
        g_reserved_asids[i] = a;
        u64 v = a & g_asid_val_mask;
        if (v != 0) map_set(v);                // reserve it (NOSTEAL)
        g_flush_pending[i] = true;
    }
    g_asid_cur_idx = ASID_USER_FIRST;
    g_asid_rollovers++;
}

// spec: new_context (the SlowSwitch non-fast resolution). Assign a current-
// generation context_id for a Proc whose stored context_id is stale (old_cid;
// 0 == never assigned). Under g_asid_lock.
static u64 new_context(u64 old_cid) {
    if (old_cid != 0) {
        u64 new_cid = g_asid_generation | (old_cid & g_asid_val_mask);
        // Old ASID reserved for us across a rollover -> keep it (re-stamp).
        if (check_update_reserved(old_cid, new_cid))
            return new_cid;
        // Old ASID value still free this generation -> keep it.
        u64 v = old_cid & g_asid_val_mask;
        if (v >= ASID_USER_FIRST && !map_test_and_set(v))
            return new_cid;
    }

    u64 v = map_find_free(g_asid_cur_idx);
    if (v > g_asid_last) {
        // No free ASID this generation -> roll over.
        g_asid_generation += g_asid_gen_unit;
        flush_context();
        v = map_find_free(ASID_USER_FIRST);
        // Guaranteed by construction: flush_context reserves at most one ASID
        // per online CPU, and the ASID space (>= 255) exceeds DTB_MAX_CPUS (8).
        if (v > g_asid_last)
            extinction("asid: no free ASID after rollover");
    }
    map_set(v);
    g_asid_cur_idx = v;
    return g_asid_generation | v;
}

// =============================================================================
// Public API
// =============================================================================

void asid_init(void) {
    if (g_asid_initialized) extinction("asid_init called twice");

    g_asid_bits     = asid_hw_bits();              // 8 or 16
    g_asid_val_mask = (1ull << g_asid_bits) - 1u;
    g_asid_gen_unit = (1ull << g_asid_bits);
    g_asid_last     = g_asid_val_mask;             // max user ASID value
    g_asid_generation = g_asid_gen_unit;           // generation #1 (never 0)

    map_clear_all();
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        g_active_asids[i]   = 0;
        g_reserved_asids[i] = 0;
        g_flush_pending[i]  = false;
    }
    g_asid_cur_idx     = ASID_USER_FIRST;
    g_asid_rollovers   = 0;
    g_asid_initialized = true;
}

u64 asid_resolve(u64 *context_id, unsigned cpu) {
    if (!g_asid_initialized) extinction("asid_resolve before asid_init");

    u64 cid        = __atomic_load_n(context_id, __ATOMIC_RELAXED);
    u64 old_active = __atomic_load_n(&g_active_asids[cpu], __ATOMIC_RELAXED);

    // spec: FastSwitch. Guard (1) generation-match; guard (2) old_active != 0
    // (no pending flush -- a rollover xchg'd this slot to 0). The cmpxchg
    // publishes cid and FAILS if a concurrent rollover zeroed the slot between
    // the read and here -> fall through to the slow path.
    if (old_active != 0 && gen_match(cid) &&
        __atomic_compare_exchange_n(&g_active_asids[cpu], &old_active, cid,
                                    /*weak=*/false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        return cid & g_asid_val_mask;
    }

    // spec: SlowSwitch. Under g_asid_lock.
    irq_state_t s = spin_lock_irqsave(&g_asid_lock);

    cid = __atomic_load_n(context_id, __ATOMIC_RELAXED);
    if (!gen_match(cid)) {
        cid = new_context(cid);
        __atomic_store_n(context_id, cid, __ATOMIC_RELAXED);
    }
    if (g_flush_pending[cpu]) {
        g_flush_pending[cpu] = false;
        asid_local_tlb_flush();
    }
    __atomic_store_n(&g_active_asids[cpu], cid, __ATOMIC_RELAXED);

    spin_unlock_irqrestore(&g_asid_lock, s);
    return cid & g_asid_val_mask;
}

unsigned asid_bits(void) { return g_asid_bits; }

u64 asid_generation_now(void) {
    return __atomic_load_n(&g_asid_generation, __ATOMIC_RELAXED);
}

u64 asid_rollover_count(void) {
    return __atomic_load_n(&g_asid_rollovers, __ATOMIC_RELAXED);
}
