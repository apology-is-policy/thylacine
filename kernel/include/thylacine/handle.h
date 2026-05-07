// Kernel handle table — typed unforgeable tokens to kernel objects (P2-Fc).
//
// Per ARCHITECTURE.md §18 + specs/handles.tla. A Handle is a per-Proc
// integer index naming a kernel object the process is allowed to access.
// Handles cannot be forged; they can only be received from the kernel
// (e.g., as the return of `burrow_create`) or transferred via 9P (Phase 4).
//
// At v1.0 P2-Fc:
//   - Eight kobj kinds (per §18.2): Process / Thread / BURROW / Spoor
//     (transferable) + MMIO / IRQ / DMA / Interrupt (non-transferable).
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

#ifndef THYLACINE_HANDLE_H
#define THYLACINE_HANDLE_H

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
    KOBJ_KIND_COUNT = 9,
};

// _Static_assert pins KIND_COUNT — adding a new kind requires bumping
// this constant + extending the transferable/hw masks below + reviewing
// every switch over kobj_kind in the kernel (per ARCH §18.3 typed
// transferability).
_Static_assert(KOBJ_KIND_COUNT == 9,
               "kobj_kind drift: when adding a new kind, update "
               "KOBJ_KIND_TRANSFERABLE_MASK / KOBJ_KIND_HW_MASK + every "
               "switch over kobj_kind (handle_transfer_via_9p, etc.)");

// Per ARCH §28 I-4 + I-5: handles are partitioned into transferable
// (Process / Thread / BURROW / Spoor — pass-able via 9P) and hardware
// (MMIO / IRQ / DMA / Interrupt — non-transferable). KOBJ_INVALID is
// neither.
//
// Implementing the spec's TxKObjs / HwKObjs partition. The disjoint
// _Static_assert is the runtime guarantee that no kind ever appears
// in both sets — a violation would silently let a hw handle transfer.
#define KOBJ_KIND_TRANSFERABLE_MASK \
    ((1u << KOBJ_PROCESS) | (1u << KOBJ_THREAD) | \
     (1u << KOBJ_BURROW)     | (1u << KOBJ_SPOOR))

#define KOBJ_KIND_HW_MASK \
    ((1u << KOBJ_MMIO) | (1u << KOBJ_IRQ) | \
     (1u << KOBJ_DMA)  | (1u << KOBJ_INTERRUPT))

_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_HW_MASK) == 0,
               "transferable + hw kind masks must be disjoint — every "
               "kind is either passable via 9P or pinned to its origin "
               "Proc, never both");

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
    struct Handle slots[PROC_HANDLE_MAX];
};

_Static_assert(sizeof(struct HandleTable) == 24 * PROC_HANDLE_MAX,
               "HandleTable size pinned at PROC_HANDLE_MAX * sizeof(Handle)");

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

// Look up a handle. Returns NULL if h is out of range, or the slot is
// empty (magic != HANDLE_MAGIC), or p is NULL/corrupted. The returned
// pointer is into p's table; valid until the slot is closed.
struct Handle *handle_get(struct Proc *p, hidx_t h);

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

// Type classifiers. Map to the spec's TxKObjs / HwKObjs partitions.
// kobj_kind_is_transferable returns true for Process / Thread / BURROW /
// Spoor; kobj_kind_is_hw returns true for MMIO / IRQ / DMA / Interrupt.
// KOBJ_INVALID returns false from both.
bool kobj_kind_is_transferable(enum kobj_kind k);
bool kobj_kind_is_hw(enum kobj_kind k);

// Per-Proc count of in-use handle slots (linear scan; for tests +
// diagnostics, not perf-critical).
int handle_table_count(const struct HandleTable *t);

// Diagnostics — cumulative cache statistics.
u64 handle_total_allocated(void);
u64 handle_total_freed(void);

#endif // THYLACINE_HANDLE_H
