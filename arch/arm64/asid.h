// P3-Ba: ASID allocator for per-Proc TTBR0 management.
//
// 8-bit ASIDs at v1.0 (ARM64 TCR_EL1.AS=0 default). The hardware ASID
// space partition:
//
//   ASID 0       RESERVED for kernel mappings (nG=0 in TTBR1; the kernel
//                half of the address space is global / non-tagged).
//   ASID 1..255  User-Proc use. Allocated via asid_alloc.
//
// Strategy: free-list with immediate TLB flush on free. asid_alloc pops
// from the free-list (LIFO; cache-friendly) before falling back to a
// monotonic counter for fresh ASIDs. asid_free issues
// `tlbi aside1is, <asid>` to invalidate ALL CPUs' TLB entries for the
// outgoing ASID BEFORE returning the slot to the pool — the next caller
// of asid_alloc that receives this ASID gets a clean TLB unconditionally.
//
// At v1.0 we extinct on hard exhaustion: 255 ASIDs allocated AND no
// frees in the pool. This is unreachable under v1.0 test scales
// (rfork_stress_1000 has at most ~16 alive Procs at a time; cascading
// stress + work-stealing don't exceed ~30). Rollover via Linux-style
// generation tags is deferred to Phase 5+.
//
// Per ARCH §6.2 ("nG bit set in user mappings; kernel mappings global
// with ASID 0") and ARM ARM D5.10 (TLB maintenance + ASIDBits).

#ifndef THYLACINE_ARM64_ASID_H
#define THYLACINE_ARM64_ASID_H

#include <thylacine/types.h>

// ASID space partition.
#define ASID_RESERVED_KERNEL  0u
#define ASID_USER_FIRST       1u
#define ASID_USER_LAST        255u
#define ASID_USER_MAX         (ASID_USER_LAST - ASID_USER_FIRST + 1u)

// Boot-time initializer. Must be called once after slub_init (no SLUB
// dependency at v1.0 — ASID state lives in BSS — but the discipline
// matches main.c bootstrap order). Idempotent on first call; extincts
// if called twice.
void asid_init(void);

// Allocate a fresh ASID in [ASID_USER_FIRST, ASID_USER_LAST]. On
// exhaustion at v1.0, extincts loudly. Caller assigns the returned
// ASID to a per-Proc page-table root (TTBR0_EL1 layout).
u16 asid_alloc(void);

// Release an ASID back to the free-list. Issues a TLB-invalidate-by-ASID
// (inner-shareable) before returning the slot — any cached TLB entries
// keyed by this ASID are flushed across all CPUs. Caller must ensure
// no further reads/writes by the now-defunct Proc occur (e.g., the
// Proc is ZOMBIE; its threads are EXITING/freed). Out-of-range or
// double-free extincts.
void asid_free(u16 asid);

// Issue a TLB-invalidate-by-ASID, inner-shareable. Used by asid_free
// internally; exposed for callers that have torn down a Proc's user
// mappings while keeping the ASID alive (rare; Phase 3+ may use this
// for mprotect-style flushes).
void asid_tlb_flush(u16 asid);

// Diagnostics.
u64       asid_total_allocated(void);
u64       asid_total_freed(void);
unsigned  asid_inflight(void);

#endif // THYLACINE_ARM64_ASID_H
