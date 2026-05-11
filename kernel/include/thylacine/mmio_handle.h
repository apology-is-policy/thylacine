// KObj_MMIO — hardware-MMIO range as a handle-table-managed kernel object
// (P4-Ib).
//
// Per ARCHITECTURE.md §13 (handles) + §28 I-5 (KObj_MMIO non-transferable)
// + specs/handles.tla. A userspace driver process holds a KObj_MMIO
// handle naming a physical-memory range exclusively reserved for that
// driver. The kernel guarantees:
//
//   1. Exclusivity (specs/handles.tla::HwResourceExclusive): no two
//      drivers can simultaneously hold KObj_MMIO handles for overlapping
//      PA ranges. Pinned by g_mmio_claims overlap rejection in
//      kobj_mmio_create.
//
//   2. Non-transferability (specs/handles.tla::HwHandlesAtOrigin): the
//      handle stays with its origin Proc; the 9P transfer path
//      structurally has no KOBJ_MMIO case (see kernel/handle.h
//      KOBJ_KIND_HW_MASK + handle_transfer_via_9p switch).
//
//   3. No in-proc duplication (specs/handles.tla::NoHwDup): handle_dup
//      rejects KOBJ_MMIO at runtime.
//
//   4. Capability-gated creation (specs/handles.tla::HwHandleImpliesCap):
//      SYS_MMIO_CREATE checks `current_proc->caps & CAP_HW_CREATE`
//      before allocating.
//
// At v1.0 P4-Ib the KObj_MMIO carries the PA range identity only —
// installing PTEs into a user address space (kobj_mmio_map_into_user)
// is deferred to P4-Ic (needs Burrow-type extension for MMIO pages so
// the existing VMA + page-fault dispatch path can handle device
// memory uniformly). At P4-Ib the handle is a CLAIM TICKET; the actual
// MMIO mapping happens via a future syscall.

#ifndef THYLACINE_MMIO_HANDLE_H
#define THYLACINE_MMIO_HANDLE_H

#include <thylacine/types.h>

// KOBJ_MMIO_MAGIC — sentinel set at kobj_mmio_create; checked at every
// public API entry. Sits at offset 0; SLUB freelist write on free
// clobbers it (use-after-free defense, mirrors burrow.c / irqfwd.c).
#define KOBJ_MMIO_MAGIC 0x4D4D494F0BADC0DEULL

struct KObj_MMIO {
    u64    magic;       // KOBJ_MMIO_MAGIC
    u64    pa;          // physical address (page-aligned)
    size_t size;        // byte size (page-aligned, > 0)
    int    ref;         // refcount; starts at 1 from kobj_mmio_create
};

// Bring up the MMIO-handle subsystem. Allocates the g_mmio_claims
// tracking storage. Must be called after slub_init; idempotent guard
// extincts on re-call.
void kobj_mmio_init(void);

// Allocate a fresh KObj_MMIO claiming the PA range [pa, pa+size).
// Returns NULL on:
//   - pa or size not page-aligned (pa % PAGE_SIZE != 0; size % PAGE_SIZE != 0).
//   - size == 0.
//   - Integer overflow on pa + size.
//   - Range overlaps any already-claimed range (specs/handles.tla
//     HwResourceExclusive: at most one alive KObj_MMIO per PA range).
//   - g_mmio_claims storage full (KOBJ_MMIO_MAX claims at v1.0).
//   - SLUB OOM.
//
// On success, the caller owns the returned reference (refcount=1);
// balance with kobj_mmio_unref / kobj_mmio_destroy. The PA range stays
// claimed until the last unref drops the refcount to zero.
struct KObj_MMIO *kobj_mmio_create(u64 pa, size_t size);

// Refcount ops. Mirror burrow_ref / kobj_irq_ref.
void kobj_mmio_ref(struct KObj_MMIO *k);

// Decrement ref. If zero: release the PA-range claim + kfree. After
// the unref that drops ref to 0, `k` is INVALID — the SLUB freelist
// clobbers magic and the memory may be reused.
//
// NULL-safe.
void kobj_mmio_unref(struct KObj_MMIO *k);

// Convenience: drop the caller's reference + ensure the PA range is
// released. Equivalent to kobj_mmio_unref when the caller holds the
// only reference.
void kobj_mmio_destroy(struct KObj_MMIO *k);

// Diagnostic: cumulative create counter + currently-live count.
u64 kobj_mmio_total_created(void);
u64 kobj_mmio_live_count(void);

#endif  // THYLACINE_MMIO_HANDLE_H
