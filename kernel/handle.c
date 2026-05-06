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

    g_handle_table_cache = kmem_cache_create("handle_table",
                                             sizeof(struct HandleTable),
                                             8,
                                             KMEM_CACHE_PANIC_ON_FAIL);
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

void handle_table_free(struct HandleTable *t) {
    if (!t) return;

    // Close any in-use handles before releasing the table. At v1.0 P2-Fc,
    // close does NOT decrement underlying kobj refcounts (no kobj has
    // integrated refcount yet); it only zeros the slot. P2-Fd integrates
    // vmo_unref for KOBJ_VMO; future phases add refs for other kinds.
    for (int i = 0; i < PROC_HANDLE_MAX; i++) {
        if (t->slots[i].magic == HANDLE_MAGIC) {
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

    return handle_alloc(p, parent->kind, new_rights, parent->obj);
}

u64 handle_total_allocated(void) { return g_handle_allocated; }
u64 handle_total_freed(void)     { return g_handle_freed; }
