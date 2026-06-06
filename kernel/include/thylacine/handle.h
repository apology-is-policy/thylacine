// Kernel handle table — typed unforgeable tokens to kernel objects (P2-Fc).
//
// Per ARCHITECTURE.md §18 + specs/handles.tla. A Handle is a per-Proc
// integer index naming a kernel object the process is allowed to access.
// Handles cannot be forged; they can only be received from the kernel
// (e.g., as the return of `burrow_create`) or transferred via 9P (Phase 4).
//
// At v1.0 P2-Fc:
//   - Nine kobj kinds (per §18.2): Process / Thread / BURROW / Spoor
//     (transferable) + MMIO / IRQ / DMA / Interrupt (hardware, non-
//     transferable) + Srv (a /srv service or connection object, non-
//     transferable; P5-corvus-srv).
//   - Six rights (per §18.2): READ / WRITE / MAP / TRANSFER / DMA / SIGNAL.
//   - Per-Proc HandleTable is a fixed-size array (PROC_HANDLE_MAX = 64).
//     Phase 5+ refactors to growable RB-tree when the syscall surface
//     lands.
//   - Underlying-kobj refcount integration: not yet. handle_close just
//     zeros the slot. P2-Fd integrates `burrow_unref` for KOBJ_BURROW; future
//     phases add refs for other kinds (struct Proc / Spoor / etc.).
//   - Cross-Proc transfer (handle_transfer_via_9p): not yet. Phase 4
//     wires the 9P out-of-band metadata path per §18.6.
//   - Type partitioning (transferable vs hw) is enforced at compile time
//     via _Static_assert on KOBJ_KIND_COUNT — adding a new kind requires
//     bumping the count + extending the masks + reviewing every switch
//     over kobj_kind in the kernel.
//
// State invariants pinned by specs/handles.tla (TLC-checked at P2-Fa):
//   I-2 (CapsCeiling)        — proc capabilities only reduce.
//   I-4 (OnlyTransferVia9P)  — no direct cross-Proc transfer syscall.
//   I-5 (HwHandlesAtOrigin)  — hw handles never transfer.
//   I-6 (RightsCeiling)      — handle rights only reduce on dup/transfer.
//   SrvHandlesAtOrigin       — KObj_Srv handles never transfer — the
//                              I-5-style structural rule for /srv
//                              connection Spoors (P5-corvus-srv).

#ifndef THYLACINE_HANDLE_H
#define THYLACINE_HANDLE_H

#include <thylacine/spinlock.h>     // #844: per-Proc handle-table lock
#include <thylacine/types.h>

struct Proc;

// HANDLE_MAGIC — sentinel set at handle_alloc; checked at handle_get /
// handle_close. Slot is FREE iff magic == 0 (KP_ZERO clears table to
// all-free at alloc; handle_close zeros the slot on release).
#define HANDLE_MAGIC 0x48414e444c45BAD2ULL    // 'HANDLE' | 0xBAD2

// Per ARCH §18.2. Eight kinds. Order matters — _Static_asserts pin the
// enum values to specific bits in the transferable + hw masks below.
enum kobj_kind {
    KOBJ_INVALID    = 0,    // zero-initialized; not usable
    KOBJ_PROCESS    = 1,    // a struct Proc *
    KOBJ_THREAD     = 2,    // a struct Thread *
    KOBJ_BURROW        = 3,    // a struct Burrow * (P2-Fd)
    KOBJ_SPOOR       = 4,    // an open 9P channel (Phase 4)
    KOBJ_MMIO       = 5,    // an MMIO range, non-transferable
    KOBJ_IRQ        = 6,    // an IRQ subscription, non-transferable
    KOBJ_DMA        = 7,    // a DMA buffer, non-transferable
    KOBJ_INTERRUPT  = 8,    // an eventfd-like interrupt, non-transferable
    KOBJ_SRV        = 9,    // a /srv service or connection object, non-transferable (P5-corvus-srv)
    KOBJ_LOOM       = 10,   // a Loom ring (KObj_Loom), non-transferable (Loom-2a)
    KOBJ_KIND_COUNT = 11,
};

// _Static_assert pins KIND_COUNT — adding a new kind requires bumping
// this constant + extending the transferable/hw/srv/loom masks below +
// reviewing every switch over kobj_kind in the kernel (per ARCH §18.3 typed
// transferability).
_Static_assert(KOBJ_KIND_COUNT == 11,
               "kobj_kind drift: when adding a new kind, update "
               "KOBJ_KIND_TRANSFERABLE_MASK / KOBJ_KIND_HW_MASK / "
               "KOBJ_KIND_SRV_MASK / KOBJ_KIND_LOOM_MASK + every switch over "
               "kobj_kind (handle_release_obj, handle_acquire_obj).");

// Per ARCH §28 I-4 + I-5 + §18.2: handles are partitioned into three
// disjoint sets — transferable (Process / Thread / BURROW / Spoor —
// pass-able via 9P), hardware (MMIO / IRQ / DMA / Interrupt — pinned to
// the origin Proc), and srv (Srv — a /srv connection Spoor, likewise
// pinned to its origin Proc). KOBJ_INVALID is in none.
//
// Implementing specs/handles.tla's TxKObjs / HwKObjs / SrvKObjs
// partition (its three pairwise-disjoint ASSUMEs). The _Static_asserts
// below are the runtime guarantee that no kind ever appears in two
// sets — a violation would silently let a non-transferable handle
// transfer.
#define KOBJ_KIND_TRANSFERABLE_MASK \
    ((1u << KOBJ_PROCESS) | (1u << KOBJ_THREAD) | \
     (1u << KOBJ_BURROW)     | (1u << KOBJ_SPOOR))

#define KOBJ_KIND_HW_MASK \
    ((1u << KOBJ_MMIO) | (1u << KOBJ_IRQ) | \
     (1u << KOBJ_DMA)  | (1u << KOBJ_INTERRUPT))

// P5-corvus-srv: KObj_Srv is non-transferable but NOT hardware — a
// distinct third partition. A /srv connection Spoor is pinned to the
// Proc that opened it, so the kernel-stamped peer identity behind it
// (CORVUS-DESIGN.md §6.3) is unforgeable across a 9P walk.
#define KOBJ_KIND_SRV_MASK \
    (1u << KOBJ_SRV)

// Loom-2a: KObj_Loom is non-transferable + non-hardware — a fourth
// partition. A Loom ring is pinned to the Proc whose address space holds
// the ring Burrow + whose handle table the registered handles name, so it
// is meaningless to pass to another Proc (and is never dup-able).
#define KOBJ_KIND_LOOM_MASK \
    (1u << KOBJ_LOOM)

_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_HW_MASK) == 0,
               "transferable + hw kind masks must be disjoint");
_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_SRV_MASK) == 0,
               "transferable + srv kind masks must be disjoint");
_Static_assert((KOBJ_KIND_HW_MASK & KOBJ_KIND_SRV_MASK) == 0,
               "hw + srv kind masks must be disjoint");
_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_LOOM_MASK) == 0,
               "transferable + loom kind masks must be disjoint");
_Static_assert((KOBJ_KIND_HW_MASK & KOBJ_KIND_LOOM_MASK) == 0,
               "hw + loom kind masks must be disjoint");
_Static_assert((KOBJ_KIND_SRV_MASK & KOBJ_KIND_LOOM_MASK) == 0,
               "srv + loom kind masks must be disjoint");
_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK | KOBJ_KIND_HW_MASK |
                KOBJ_KIND_SRV_MASK | KOBJ_KIND_LOOM_MASK)
                   == (((1u << KOBJ_KIND_COUNT) - 1u) & ~(1u << KOBJ_INVALID)),
               "every kobj_kind except KOBJ_INVALID must be classified "
               "into exactly one of the four partitions");

// Per ARCH §18.2. Handle rights — bitmask of what the holder can do.
typedef u32 rights_t;
#define RIGHT_NONE      0u
#define RIGHT_READ      (1u << 0)
#define RIGHT_WRITE     (1u << 1)
#define RIGHT_MAP       (1u << 2)
#define RIGHT_TRANSFER  (1u << 3)
#define RIGHT_DMA       (1u << 4)
#define RIGHT_SIGNAL    (1u << 5)
#define RIGHT_ALL       0x3fu

struct Handle {
    u64               magic;       // HANDLE_MAGIC; 0 means free slot
    enum kobj_kind    kind;
    rights_t          rights;
    void             *obj;         // pointer to underlying kernel object
};

_Static_assert(sizeof(struct Handle) == 24,
               "struct Handle pinned at 24 bytes (8 magic + 4 kind + 4 "
               "rights + 8 obj). Adding a field grows the per-Proc table "
               "by PROC_HANDLE_MAX * delta bytes.");
_Static_assert(__builtin_offsetof(struct Handle, magic) == 0,
               "magic at offset 0 — KP_ZERO clearing the table at alloc "
               "makes every slot's magic == 0, naturally signaling free");

// Per-Proc handle table size at v1.0. Phase 5+ refactors to growable
// (e.g., RB-tree keyed by hidx_t) when the syscall surface lands and
// processes need >> 64 handles. 64 * 24 = 1536 bytes per Proc — kept
// in its own SLUB cache.
#define PROC_HANDLE_MAX 64

struct HandleTable {
    // #844: serializes all slot ops (alloc / close / get / dup) -- the Plan 9
    // Fgrp lock. Peer threads of a multi-threaded Proc share one HandleTable,
    // so a sibling's handle_close must not free a slot's obj or zero the slot
    // under a concurrent handle_get. Plain spin_lock (process-context only --
    // handle ops never run from IRQ; matches p->vma_lock). KP_ZERO at
    // handle_table_alloc inits it unlocked. The obj refcount (bumped under
    // this lock in handle_get/dup, dropped OUTSIDE it in handle_put/close)
    // carries the obj's lifetime past the lock release.
    spin_lock_t   lock;
    u32           _pad_lock;
    struct Handle slots[PROC_HANDLE_MAX];
};

_Static_assert(sizeof(struct HandleTable) == 8 + 24 * PROC_HANDLE_MAX,
               "HandleTable size pinned at 8 (lock + pad) + "
               "PROC_HANDLE_MAX * sizeof(Handle)");

// Handle index — signed; -1 indicates invalid / not-found / table-full.
typedef int hidx_t;

// Bring up the handle subsystem. Allocates the SLUB cache for
// HandleTable. Must be called before proc_init (since proc_alloc
// allocates a HandleTable for each new Proc).
void handle_init(void);

// SLUB-allocate a fresh HandleTable for a new Proc. All slots free
// (magic == 0). Returns NULL on OOM.
struct HandleTable *handle_table_alloc(void);

// Release a HandleTable. Closes any open handles first (zeros their
// slots; at v1.0 P2-Fc no underlying kobj refcount is decremented —
// P2-Fd integrates burrow_unref for KOBJ_BURROW; future phases wire the
// other kinds).
void handle_table_free(struct HandleTable *t);

// Allocate a handle in p's table.
//
// kind: must be in [KOBJ_PROCESS .. KOBJ_KIND_COUNT-1]. KOBJ_INVALID
//   and out-of-range values are rejected.
// rights: non-empty bitmask drawn from RIGHT_ALL. Empty rights or
//   bits outside RIGHT_ALL are rejected.
// obj: pointer to the underlying kernel object. May be NULL at v1.0
//   for kinds whose underlying impl isn't yet integrated (test paths);
//   production callers always pass a valid obj.
//
// Returns the slot index on success, -1 on validation failure or
// table-full.
//
// Maps to specs/handles.tla::HandleAlloc(p, k, granted).
hidx_t handle_alloc(struct Proc *p, enum kobj_kind kind,
                    rights_t rights, void *obj);

// Release a handle. Returns 0 on success, -1 if the slot is empty
// (already-closed or never-allocated) or out-of-range. At v1.0 P2-Fc
// the underlying kobj is NOT reference-counted; close just zeros the
// slot.
//
// Maps to specs/handles.tla::HandleClose(p, h).
int handle_close(struct Proc *p, hidx_t h);

// Look up a handle into a caller-owned snapshot, with a reference HELD on
// the underlying kobj. Returns 0 on success (*out filled: kind / rights /
// obj, magic == HANDLE_MAGIC, and the obj's refcount bumped so it stays alive
// until the matching handle_put), -1 on failure (out-of-range h, empty slot,
// NULL/corrupted p, or NULL out -- *out zeroed, no ref held).
//
// #844: the lookup + the snapshot copy + the ref bump run under the per-Proc
// handle-table lock, so a sibling thread's concurrent handle_close cannot
// invalidate the read or free the obj under the caller. The OLD contract
// ("returns a live pointer into the table, valid until the slot is closed")
// was a TOCTOU in a multi-threaded Proc -- the slot pointer dangled and the
// obj could be freed across any use (esp. blocking dev I/O / accept / 9P
// handshake). Callers use out->obj (ref-held, safe across blocking ops) and
// MUST handle_put(out) on EVERY exit path once a get succeeds.
int handle_get(struct Proc *p, hidx_t h, struct Handle *out);

// Release the reference a successful handle_get acquired on h->obj. Runs the
// per-kind release OUTSIDE any handle-table lock (it may sleep -- spoor_clunk's
// Dev close hook, SrvConn teardown). NULL-safe + idempotent: a no-op on a
// NULL/zeroed snapshot (a failed handle_get), and it zeroes *h after release so
// a double handle_put is a no-op. NOT slot deletion -- that is handle_close
// (which drops the TABLE's ref); handle_put drops the CALLER's borrowed ref.
void handle_put(struct Handle *h);

// Duplicate a handle within p's table with possibly reduced rights.
//
// new_rights MUST be a subset of the parent's rights — elevation is
// rejected (-1 returned). This is the impl-side enforcement of the
// spec's RightsCeiling invariant: BuggyDupElevate produces a counter-
// example by adding bits not in parent's rights, which this check
// rejects at runtime.
//
// Returns the new slot index on success, -1 on:
//   - h out-of-range / empty slot
//   - new_rights NOT a subset of parent's rights (rights elevation)
//   - new_rights == 0 or has bits outside RIGHT_ALL
//   - table full
//
// Maps to specs/handles.tla::HandleDup(p, h, new_rights).
hidx_t handle_dup(struct Proc *p, hidx_t h, rights_t new_rights);

// Type classifiers. Map to specs/handles.tla's TxKObjs / HwKObjs /
// SrvKObjs partitions. kobj_kind_is_transferable returns true for
// Process / Thread / BURROW / Spoor; kobj_kind_is_hw for MMIO / IRQ /
// DMA / Interrupt; kobj_kind_is_srv for Srv. Exactly one is true for
// every kind except KOBJ_INVALID (and any out-of-range value), for
// which all three return false.
bool kobj_kind_is_transferable(enum kobj_kind k);
bool kobj_kind_is_hw(enum kobj_kind k);
bool kobj_kind_is_srv(enum kobj_kind k);

// Per-Proc count of in-use handle slots (linear scan; for tests +
// diagnostics, not perf-critical).
int handle_table_count(const struct HandleTable *t);

// Diagnostics — cumulative cache statistics.
u64 handle_total_allocated(void);
u64 handle_total_freed(void);

#endif // THYLACINE_HANDLE_H
