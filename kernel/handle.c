// Kernel handle table impl (P2-Fc).
//
// Per ARCHITECTURE.md §18 + specs/handles.tla. Implements the spec's
// HandleAlloc / HandleClose / HandleDup actions on a per-Proc fixed-size
// SLUB-allocated HandleTable.
//
// State invariants (from specs/handles.tla, TLC-checked):
//   RightsCeiling     — handle.rights subset of origin_rights[handle.kobj].
//                       Enforced by handle_dup's subset check before insert.
//   HwHandlesAtOrigin — hw-typed handles non-transferable. Enforced by
//                       handle_transfer_via_9p's omitted switch cases
//                       (Phase 4); structurally pinned by the
//                       _Static_assert on KOBJ_KIND_COUNT.
//   OnlyTransferVia9P — no direct cross-Proc transfer syscall exists.
//                       Enforced by absence (no impl).
//   CapsCeiling       — proc-level capability mask reduces only
//                       monotonically. Forward-looking; rfork mask is
//                       AND-only at Phase 5+ syscall surface.
//
// At v1.0 P2-Fc:
//   - Per-Proc table is fixed-size (PROC_HANDLE_MAX = 64 slots).
//   - No underlying-kobj refcount integration.
//   - No transfer-via-9P codepath; Phase 4 wires it.
//   - No syscall surface; in-kernel callers only.
//
// Phase 5+ refinement:
//   - Growable table via RB-tree keyed by hidx_t.
//   - Per-Proc handle-table lock for SMP safety.
//   - Refcount integration via vmo_ref/unref (P2-Fd is the first step).
//   - Capability-handle elevation via factotum (per ARCH §15.4).

#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vmo.h>

#include "../mm/slub.h"

static struct kmem_cache *g_handle_table_cache;
static u64                g_handle_allocated;
static u64                g_handle_freed;

// =============================================================================
// Type classifiers — implement the spec's TxKObjs / HwKObjs partition.
// =============================================================================

bool kobj_kind_is_transferable(enum kobj_kind k) {
    if (k <= KOBJ_INVALID || k >= KOBJ_KIND_COUNT) return false;
    return ((1u << (unsigned)k) & KOBJ_KIND_TRANSFERABLE_MASK) != 0;
}

bool kobj_kind_is_hw(enum kobj_kind k) {
    if (k <= KOBJ_INVALID || k >= KOBJ_KIND_COUNT) return false;
    return ((1u << (unsigned)k) & KOBJ_KIND_HW_MASK) != 0;
}

// =============================================================================
// HandleTable lifecycle.
// =============================================================================

void handle_init(void) {
    if (g_handle_table_cache) extinction("handle_init called twice");

    // R5-F F50/F51 close: NO PANIC_ON_FAIL. Per-Proc handle tables
    // allocate at proc_alloc, which is reachable from rfork — userspace
    // OOM (a fork bomb) shouldn't extinct the kernel. Caller-side
    // rollback paths handle the NULL return per the documented
    // contract in handle.h.
    g_handle_table_cache = kmem_cache_create("handle_table",
                                             sizeof(struct HandleTable),
                                             8,
                                             0);
    if (!g_handle_table_cache) {
        extinction("kmem_cache_create(handle_table) returned NULL");
    }
}

struct HandleTable *handle_table_alloc(void) {
    if (!g_handle_table_cache)
        extinction("handle_table_alloc before handle_init");
    // KP_ZERO ensures every slot's magic = 0 (free) at allocation.
    struct HandleTable *t = kmem_cache_alloc(g_handle_table_cache, KP_ZERO);
    return t;   // NULL on OOM
}

// Per-kind release of the underlying kernel object reference. Called
// from handle_close (caller passes a Proc) and handle_table_free
// (orphan path; no Proc available — table outlives it).
//
// At v1.0 P2-Fd: KOBJ_VMO has refcount integration via vmo_unref;
// other kinds are no-op. As subsystems land:
//   KOBJ_CHAN     — Phase 4 (chan_unref via 9P client).
//   KOBJ_MMIO     — Phase 3 (driver-startup CMA release).
//   KOBJ_IRQ      — Phase 3 (gic_unregister).
//   KOBJ_DMA      — Phase 3 (CMA release).
//   KOBJ_PROCESS  — Phase 5+ (proc_unref when struct Proc gets refs).
//   KOBJ_THREAD   — Phase 5+.
//   KOBJ_INTERRUPT — Phase 5+.
//
// R5-F F54 close: explicit `default:` extincts on out-of-enum kind to
// catch memory corruption (the static_assert defends at compile time;
// the default arm defends at runtime).
static void handle_release_obj(enum kobj_kind kind, void *obj) {
    if (!obj) return;
    switch (kind) {
    case KOBJ_VMO:
        vmo_unref((struct Vmo *)obj);
        break;
    case KOBJ_INVALID:
    case KOBJ_PROCESS:
    case KOBJ_THREAD:
    case KOBJ_CHAN:
    case KOBJ_MMIO:
    case KOBJ_IRQ:
    case KOBJ_DMA:
    case KOBJ_INTERRUPT:
    case KOBJ_KIND_COUNT:
        // No refcount integration yet — see comment above. Falls through
        // to the slot-zeroing path; the underlying kobj's lifetime is
        // managed elsewhere at v1.0 P2-Fc/Fd.
        break;
    default:
        extinction("handle_release_obj: out-of-enum kobj_kind (memory corruption?)");
    }
}

// Per-kind acquire of the underlying kernel object reference. Called
// from handle_dup (the dup'd handle is a NEW reference; must be ref'd
// to balance the future close).
//
// R5-F F54 close: explicit `default:` extincts on out-of-enum kind.
static void handle_acquire_obj(enum kobj_kind kind, void *obj) {
    if (!obj) return;
    switch (kind) {
    case KOBJ_VMO:
        vmo_ref((struct Vmo *)obj);
        break;
    case KOBJ_INVALID:
    case KOBJ_PROCESS:
    case KOBJ_THREAD:
    case KOBJ_CHAN:
    case KOBJ_MMIO:
    case KOBJ_IRQ:
    case KOBJ_DMA:
    case KOBJ_INTERRUPT:
    case KOBJ_KIND_COUNT:
        break;
    default:
        extinction("handle_acquire_obj: out-of-enum kobj_kind (memory corruption?)");
    }
}

void handle_table_free(struct HandleTable *t) {
    if (!t) return;

    // Close any in-use handles before releasing the table. Calls the
    // per-kind release path so underlying refcounts (KOBJ_VMO at P2-Fd)
    // are decremented correctly even on orphan-table cleanup.
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == HANDLE_MAGIC) {
            handle_release_obj(t->slots[i].kind, t->slots[i].obj);
            t->slots[i].magic  = 0;
            t->slots[i].kind   = KOBJ_INVALID;
            t->slots[i].rights = RIGHT_NONE;
            t->slots[i].obj    = NULL;
            g_handle_freed++;
        }
    }
    kmem_cache_free(g_handle_table_cache, t);
}

int handle_table_count(const struct HandleTable *t) {
    if (!t) return 0;
    int n = 0;
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == HANDLE_MAGIC) n++;
    }
    return n;
}

// =============================================================================
// Handle ops.
// =============================================================================

static struct HandleTable *proc_handles_or_extinct(struct Proc *p) {
    if (!p)                             extinction("handle op on NULL Proc");
    if (p->magic != PROC_MAGIC)         extinction("handle op on corrupted Proc");
    if (!p->handles)                    extinction("handle op on Proc with NULL handle table");
    return p->handles;
}

// Validate kind + rights for an alloc/dup. Returns true if all checks
// pass; false if the caller should reject with -1.
static bool valid_alloc_args(enum kobj_kind kind, rights_t rights) {
    if (kind <= KOBJ_INVALID || kind >= KOBJ_KIND_COUNT) return false;
    if (rights == RIGHT_NONE) return false;
    if (rights & ~RIGHT_ALL) return false;
    return true;
}

hidx_t handle_alloc(struct Proc *p, enum kobj_kind kind,
                    rights_t rights, void *obj) {
    struct HandleTable *t = proc_handles_or_extinct(p);

    if (!valid_alloc_args(kind, rights)) return -1;

    // R5-F F49 close: defensive runtime check for KOBJ_VMO. The
    // contract is "the caller has already accounted for one ref"
    // (typically: vmo_create_anon's initial handle_count=1, OR a
    // pre-call vmo_ref from a future Phase 4 transfer-via-9p path).
    // Future-buggy callers that pass a Vmo without holding a ref
    // would create a slot with no backing count — the first
    // handle_close would underflow vmo_unref's count check.
    //
    // Catching here makes the convention enforced at runtime, not
    // just documented. The check is a no-op for non-KOBJ_VMO kinds
    // and for NULL obj (test paths).
    if (kind == KOBJ_VMO && obj) {
        struct Vmo *v = (struct Vmo *)obj;
        if (v->magic != VMO_MAGIC)
            extinction("handle_alloc(KOBJ_VMO): obj has bad magic (not a Vmo or UAF)");
        if (v->handle_count <= 0)
            extinction("handle_alloc(KOBJ_VMO): caller has not accounted for "
                       "the slot's reference (call vmo_ref first, or use the "
                       "vmo_create_anon-consumed-reference convention)");
    }

    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == 0) {
            t->slots[i].magic  = HANDLE_MAGIC;
            t->slots[i].kind   = kind;
            t->slots[i].rights = rights;
            t->slots[i].obj    = obj;
            g_handle_allocated++;
            return (hidx_t)i;
        }
    }
    return -1;   // table full
}

int handle_close(struct Proc *p, hidx_t h) {
    struct HandleTable *t = proc_handles_or_extinct(p);

    if (h < 0 || h >= PROC_HANDLE_MAX)  return -1;
    struct Handle *slot = &t->slots[h];
    if (slot->magic != HANDLE_MAGIC)    return -1;

    // Per-kind release. P2-Fd integrates vmo_unref for KOBJ_VMO; future
    // phases add the other kinds.
    handle_release_obj(slot->kind, slot->obj);

    slot->magic  = 0;
    slot->kind   = KOBJ_INVALID;
    slot->rights = RIGHT_NONE;
    slot->obj    = NULL;
    g_handle_freed++;
    return 0;
}

struct Handle *handle_get(struct Proc *p, hidx_t h) {
    if (!p || p->magic != PROC_MAGIC || !p->handles) return NULL;
    if (h < 0 || h >= PROC_HANDLE_MAX) return NULL;
    struct Handle *slot = &p->handles->slots[h];
    if (slot->magic != HANDLE_MAGIC)   return NULL;
    return slot;
}

hidx_t handle_dup(struct Proc *p, hidx_t h, rights_t new_rights) {
    struct Handle *parent = handle_get(p, h);
    if (!parent) return -1;

    // Rights monotonic reduction: new_rights MUST be a subset of
    // parent->rights. Models the spec's HandleDup precondition.
    // Rejects elevation (e.g., parent has {READ}, dup with {READ,
    // WRITE}) — the bug class that specs/handles.tla's BuggyDupElevate
    // models, which produces a RightsCeiling counterexample.
    if ((new_rights & parent->rights) != new_rights) return -1;
    if (!valid_alloc_args(parent->kind, new_rights)) return -1;

    // Capture parent's kind + obj BEFORE handle_alloc — the alloc may
    // reuse parent's slot (it scans for the lowest free slot, but in
    // pathological cases of reuse-after-fragmentation the parent slot
    // could be ours after handle_close earlier; defensive).
    enum kobj_kind kind = parent->kind;
    void          *obj  = parent->obj;

    // Acquire a new reference to the underlying kobj. P2-Fd integrates
    // vmo_ref for KOBJ_VMO. The dup'd handle is a NEW reference; the
    // future handle_close on it will release the corresponding ref.
    handle_acquire_obj(kind, obj);

    hidx_t child = handle_alloc(p, kind, new_rights, obj);
    if (child < 0) {
        // Roll back the acquire on alloc failure.
        handle_release_obj(kind, obj);
    }
    return child;
}

u64 handle_total_allocated(void) { return g_handle_allocated; }
u64 handle_total_freed(void)     { return g_handle_freed; }
