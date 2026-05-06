// arch/arm64/fault.h — page-fault dispatcher (P3-C).
//
// Decodes ESR_EL1 + FAR_EL1 + ELR_EL1 into a structured `fault_info`
// and dispatches to handlers that either resolve the fault (return
// FAULT_HANDLED — the ERET resumes the interrupted instruction) or
// extinct with a specific diagnostic.
//
// At v1.0 P3-C the dispatcher's job is classification + plumbing; the
// only paths that resolve are kernel-image patterns already known
// (kstack guard → "kernel stack overflow"; W^X violation → "PTE
// violates W^X"). User-mode demand paging arrives at P3-D when the
// VMA tree + VMO mapping land. The hook is in place: the dispatcher
// has a documented FAULT_UNHANDLED_USER return value that P3-D will
// expand into VMA lookup + page allocation.
//
// Per ARCHITECTURE.md §12 (exception model).

#ifndef THYLACINE_ARCH_ARM64_FAULT_H
#define THYLACINE_ARCH_ARM64_FAULT_H

#include <thylacine/types.h>

// Decoded fault classification. Pure data; no kernel state references.
struct fault_info {
    u64 vaddr;            // FAR_EL1 — faulting VA.
    u64 elr;              // ELR_EL1 — faulting PC (instruction that took the fault).
    u64 esr;              // ESR_EL1 — raw register (for diagnostic).
    u32 ec;               // ESR.EC[31:26]: exception class.
    u32 fsc;              // ESR.ISS[5:0]: data/instruction fault status code.
    u8  fault_level;      // FSC[1:0]: 0/1/2/3 — translation table level the fault happened at.
    bool from_user;       // Source EL was 0 (user-mode fault).
    bool is_instruction;  // Instruction abort (vs. data abort).
    bool is_write;        // ESR.WnR — true=write, false=read. Always false for instruction aborts.
    bool is_translation;  // FSC ∈ FSC_TRANS_FAULT_L{0..3}.
    bool is_permission;   // FSC ∈ FSC_PERM_FAULT_L{1..3}.
    bool is_access_flag;  // FSC ∈ FSC_ACCESS_FAULT_L{1..3} — FEAT_HAFDBS hardware
                          // access-flag fault. v1.0 sets PTE_AF eagerly so this
                          // shouldn't fire at P3-C; we still classify defensively.
};

// Dispatcher result. The caller (exception_sync_curr_el / equivalent for
// EL0) inspects this value and decides whether to ERET (resumes the
// interrupted instruction) or extinct.
enum fault_result {
    // Fault resolved — the dispatcher installed/updated PTEs. ERET safely
    // resumes the interrupted instruction. At v1.0 P3-C this only fires
    // for known-recoverable kernel paths (none yet — placeholder for
    // P3-D's COW + demand paging).
    FAULT_HANDLED         = 0,

    // Fault is unrecoverable. The dispatcher extincted internally; this
    // return value is reserved for paths that can't extinct from inside
    // (currently none — every fatal path inside arch_fault_handle calls
    // extinction directly). Listed for API completeness.
    FAULT_FATAL           = 1,

    // User-mode fault that the kernel can't recover (e.g., dereferencing
    // an unmapped user VA). At Phase 5+ this becomes a SIGSEGV-like note
    // delivered to the offending Proc. At v1.0 P3-C the caller extincts
    // (no userspace yet). At P3-D once VMA-tree dispatch lands, this
    // path will mostly disappear — most user-mode faults will resolve
    // via demand paging — and the remaining "truly bad VA" cases stay
    // as the P5+ note path.
    FAULT_UNHANDLED_USER  = 2,
};

// Decode raw exception state into a fault_info. Pure function; no
// kernel state reads. Used by exception.c when EC indicates a data or
// instruction abort.
//
// Caller passes the saved ESR / FAR / ELR from the exception_context
// AND the EL of the faulting access (derived from EC: 0x20/0x24 = lower
// EL → from_user=true; 0x21/0x25 = current EL → from_user=false).
void fault_info_decode(u64 esr, u64 far, u64 elr, struct fault_info *out);

// Top-level fault dispatcher. Inspects fi and either resolves the fault
// (returns FAULT_HANDLED — the caller's ERET safely resumes) or
// extincts with a specific diagnostic.
//
// At v1.0 P3-C the resolve path is empty (no in-tree handler resolves
// faults); every call extincts. P3-D adds VMA-tree dispatch for the
// FAULT_UNHANDLED_USER path; P3-D's COW + demand-paging integration
// arrives there.
//
// Returns FAULT_HANDLED only when the fault was resolved AND the ERET
// is safe. Other return values are advisory; the dispatcher itself
// handles the FATAL path internally.
enum fault_result arch_fault_handle(const struct fault_info *fi);

#endif // THYLACINE_ARCH_ARM64_FAULT_H
