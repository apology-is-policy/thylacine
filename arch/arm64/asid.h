// ASID management: the rolling-ASID allocator (RW-1 B-F1; ARCH section 6.2.1).
//
// User ASIDs are a recycled CACHE keyed by a global generation counter --
// the Linux arm64 rolling-ASID model (arch/arm64/mm/context.c) -- NOT a
// per-Proc permanent allocation. The prior per-Proc-permanent design
// extincted the kernel on the (ASID-space+1)th concurrent Proc; this design
// removes exhaustion entirely (rollover recycles the space).
//
// THE MODEL. A global generation (high bits) + the hardware ASID value (low
// ASID_BITS) form a per-Proc `context_id` (a u64; generation 0 == "never
// assigned", which always misses). At each context switch the kernel resolves
// the running Proc's current ASID via asid_resolve:
//
//   * FAST PATH (lockless): the Proc's stored generation == the global
//     generation AND this CPU's active slot is non-zero (no pending flush).
//     A cmpxchg publishes the ASID into active_asids[cpu] and FAILS if a
//     concurrent rollover zeroed the slot (-> slow path). No lock, no flush.
//   * SLOW PATH (new_context, under g_asid_lock): generation stale -> claim a
//     free ASID from the bitmap. If none is free, ROLL OVER: bump the
//     generation, reset the bitmap, preserve every CPU's active ASID into its
//     reserved slot (so a running CPU is never yanked -- the central safety
//     obligation), and set flush_pending for every CPU. Then honor this CPU's
//     pending local flush and publish.
//
// SAFETY (invariant I-31, ARCH section 28). No two CPUs concurrently run
// distinct user address spaces sharing an ASID -- else the TLB returns a wrong
// translation and one Proc reads/writes another's memory. The rollover race
// (a generation rollover concurrent with another CPU's context switch
// reassigning a live ASID) is the classic, subtle rolling-ASID hazard, so the
// surface is MODEL-FIRST: specs/asid.tla (clean + the rollover_steals_active /
// fast_no_regen / no_flush_pending / fast_no_flush_check buggy cfgs) is
// TLC-green BEFORE this code. The fast path rests on TWO guards, both modeled:
// generation-match AND the active-slot-nonzero (== no-pending-flush) check;
// omitting the second runs a CPU over stale TLB entries across a rollover.
//
// ASID WIDTH. 8 or 16 bits, detected at boot from ID_AA64MMFR0_EL1.ASIDBits
// (asid_hw_bits below). TCR_EL1.AS is programmed to match in mmu.c. 16-bit
// where supported makes rollovers rare (65535 vs 255 user ASIDs).
//
// Per ARCH section 6.2.1 + ARM ARM D5.10 (TLB maintenance + ASIDBits).

#ifndef THYLACINE_ARM64_ASID_H
#define THYLACINE_ARM64_ASID_H

#include <thylacine/types.h>

// ASID space partition. Value 0 is reserved for the kernel's global (TTBR1)
// mappings and the kernel TTBR0; user ASID values run [ASID_USER_FIRST,
// (1<<ASID_BITS)-1]. The upper bound is runtime (8- vs 16-bit) -- see
// asid_bits().
#define ASID_RESERVED_KERNEL  0u
#define ASID_USER_FIRST       1u

// TTBR0_EL1 ASID field occupies bits [63:48] regardless of 8- vs 16-bit width
// (TCR_EL1.AS selects how many of those bits the hardware uses). The pre-hook
// composes ttbr0 = (asid << ASID_TTBR0_SHIFT) | pgtable_root.
#define ASID_TTBR0_SHIFT      48u

// Read the hardware-supported ASID width from ID_AA64MMFR0_EL1.ASIDBits
// (bits [7:4]): 0b0010 -> 16-bit, otherwise 8-bit. Used by asid_init (bitmap +
// generation sizing) AND mmu.c (TCR_EL1.AS) -- both read the register directly
// so there is no init-ordering dependency on a shared global.
static inline unsigned asid_hw_bits(void) {
    u64 mmfr0;
    __asm__ __volatile__("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    return (((mmfr0 >> 4) & 0xfu) == 0x2u) ? 16u : 8u;
}

// Boot-time initializer. Reads the ASID width, sizes the bitmap + generation,
// zeroes the per-CPU active/reserved/flush_pending state, and stamps the
// generation to #1 (so a context_id of 0 always misses). Call once after the
// MMU is up; extincts if called twice.
void asid_init(void);

// Resolve the current-generation ASID for the Proc whose context_id is
// *context_id, running on logical CPU `cpu`, updating *context_id and the
// per-CPU/rollover state as needed (spec: asid.tla FastSwitch / SlowSwitch).
// Returns the hardware ASID value (bits, not the full context_id) for the
// TTBR0 compose. MUST be called with IRQs masked on a stable CPU (the context-
// switch pre-hook satisfies this: rq lock held / IRQs masked). NEVER call for
// kproc (pgtable_root == 0) -- it uses the kernel TTBR0 (ASID 0), bypassing the
// allocator. context_id is a plain u64 accessed via __atomic_* (the fast path
// is lockless); the caller need not hold any lock.
u64 asid_resolve(u64 *context_id, unsigned cpu);

// Diagnostics (atomic/seqlock-free snapshots; exact under g_asid_lock writers).
unsigned asid_bits(void);              // 8 or 16
u64      asid_generation_now(void);    // current generation value
u64      asid_rollover_count(void);    // number of rollovers since boot

#endif // THYLACINE_ARM64_ASID_H
