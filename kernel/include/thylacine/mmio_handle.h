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

// R10 F154 (P1) close: pre-reserve PA ranges owned by kernel-internal
// callers (those that call `mmu_map_mmio` directly, bypassing
// `kobj_mmio_create`'s claim layer). Without this, a userspace caller
// with `CAP_HW_CREATE` (granted at P4-Ic5+ for drivers) could call
// `t_mmio_create(pa=GIC_DISTRIBUTOR_PA, ...)` — succeeds because the
// kernel-owned range isn't in `g_mmio_claims` — and then `t_mmio_map`
// installs user-VA PTEs pointing at the GIC. Driver writes to GICD_*
// registers from userspace → break kernel IRQ delivery (privilege
// escalation; mirror of R9 F142 but on the MMIO side).
//
// Reservation walks the DTB for compatibles known to be kernel-claimed
// (GICv3 distributor + redistributor, PL011 UART, ECAM PCIe root,
// VirtIO MMIO transports) and inserts each into `g_mmio_claims[]` with
// a sentinel owner so subsequent `kobj_mmio_create` for overlapping
// PAs return NULL.
//
// Must be called AFTER `kobj_mmio_init` (it modifies `g_mmio_claims`)
// AND AFTER `dtb_init` (it walks DTB compatibles).
void kobj_mmio_reserve_kernel_ranges(void);

// Diagnostic: how many kernel-reserved slots are currently held.
// Useful for boot banner observability + tests.
int kobj_mmio_kernel_reserved_count(void);

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

// Diagnostic: is any byte of the PA range [pa, pa+size) currently
// claimed by a live KObj_MMIO (or by a kernel-reserved sentinel)?
// Used by driver-crash-recovery tests to verify the release path
// emptied the claim table after a driver process exited.
//
// Returns true if at least one byte overlaps an existing claim;
// false if the range is fully free. Acquires g_mmio_lock internally.
bool kobj_mmio_pa_claimed(u64 pa, size_t size);

#endif  // THYLACINE_MMIO_HANDLE_H
