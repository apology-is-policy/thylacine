# 20 — Virtual Memory Object (VMO) (P2-F)

A Virtual Memory Object — kernel object representing a region of memory, independent of any address space. Per `ARCHITECTURE.md §19`. v1.0 P2-Fd lands the kernel-internal API + the dual-refcount lifecycle; the syscall surface (`vmo_create`, `vmo_read`, `vmo_write`, `vmo_get_size`) lands at Phase 5+. Page-fault-handler integration for actual VMA mapping lands at Phase 3.

---

## Purpose

VMOs are Thylacine's unit of memory sharing. A VMO is created once, referenced by handles, and mapped into address spaces. Pages are eagerly allocated at create time (v1.0); they live until the last handle is closed AND the last mapping is unmapped — the moment both reach 0, the pages are freed.

Key invariant (proven in `specs/vmo.tla`):

- **No use-after-free (ARCH §28 I-7)**: `pages_alive[v] iff (handle_count[v] > 0 OR mapping_count[v] > 0)`. Catches premature free (counts > 0 but pages dead) AND delayed free (counts = 0 but pages alive).

The dual refcount discipline:

- **handle_count**: number of open handles to this VMO. Modified by `vmo_create_anon` (sets to 1), `vmo_ref` (called by `handle_dup` and Phase 4's `handle_transfer_via_9p`), `vmo_unref` (called by `handle_close`).
- **mapping_count**: number of address-space mappings. Modified by `vmo_map` (Phase 3+ from the eventual mmap_handle path) and `vmo_unmap` (from munmap).
- Pages free iff both reach 0. Whichever brings the last count to 0 triggers the free transition.

---

## Public API — `<thylacine/vmo.h>`

```c
#define VMO_MAGIC 0x564D4F00BADC0DE5ULL    // 'VMO\0' || 0xBADC0DE5

enum vmo_type {
    VMO_TYPE_INVALID = 0,
    VMO_TYPE_ANON    = 1,
    // Phase 3+ : VMO_TYPE_PHYS  (DMA / MMIO buffers)
    // Post-v1.0: VMO_TYPE_FILE  (Stratum page cache)
};

struct page;

struct Vmo {
    u64            magic;          // VMO_MAGIC; clobbered by SLUB on free
    enum vmo_type  type;
    size_t         size;           // rounded up to page_count * PAGE_SIZE
    size_t         page_count;
    int            handle_count;   // open handles to this VMO
    int            mapping_count;  // open mappings (vma's)
    struct page   *pages;          // alloc_pages chunk; NULL after free
    unsigned       order;          // for free_pages
};
_Static_assert(__builtin_offsetof(struct Vmo, magic) == 0, ...);

void          vmo_init(void);
struct Vmo   *vmo_create_anon(size_t size);
void          vmo_ref(struct Vmo *v);
void          vmo_unref(struct Vmo *v);
void          vmo_map(struct Vmo *v);
void          vmo_unmap(struct Vmo *v);

size_t        vmo_get_size(const struct Vmo *v);
int           vmo_handle_count(const struct Vmo *v);
int           vmo_mapping_count(const struct Vmo *v);

u64           vmo_total_created(void);
u64           vmo_total_destroyed(void);
```

### `vmo_create_anon(size)` — return semantics

| Return | Meaning |
|---|---|
| `non-NULL` | success; struct Vmo with handle_count=1, mapping_count=0, pages allocated. The caller's "consumed reference" — `handle_alloc` does NOT increment the count. |
| `NULL` | failure: size==0, SLUB OOM, or alloc_pages OOM. |

### `vmo_unref` / `vmo_unmap` — invalidation

After the last unref/unmap that brings BOTH counts to 0, the v pointer is **invalid**. The SLUB freelist clobbers magic; the memory may be reused. Callers must not dereference v after that point.

### Defensive checks (extinct on violation)

- NULL passed where required (vmo_ref / vmo_map / vmo_unmap).
- Corrupted magic (vmo_ref / vmo_unref / vmo_map / vmo_unmap) — UAF defense.
- Zero-ref unref or zero-mapping unmap — refcount underflow.
- vmo_ref on a VMO with both counts=0 (already-freed identity).
- vmo_map on a VMO with NULL pages (use-after-free of the backing pages).

---

## Implementation

`kernel/vmo.c` (~190 LOC).

### Lifecycle

- `vmo_init`: SLUB cache + idempotency check. Called from `boot_main` after `handle_init` (the caches are independent; ordering is for grouping).
- `vmo_create_anon`: SLUB-allocate via `kmem_cache_alloc(KP_ZERO)`; allocate backing pages via `alloc_pages(order, KP_ZERO)` where `order = ceil_log2(page_count)`. Sets handle_count=1, mapping_count=0. On any allocation failure, rolls back cleanly.
- `vmo_free_internal` (static): the symmetric internal helper called by both `vmo_unref` and `vmo_unmap` when both counts reach 0. Validates state (counts == 0; pages != NULL), calls `free_pages(v->pages, v->order)`, then `kmem_cache_free`. Increments `g_vmo_destroyed`.

### Order computation

```c
static unsigned order_for_pages(size_t page_count) {
    unsigned order = 0;
    size_t n = 1;
    while (n < page_count) { n <<= 1; order++; }
    return order;
}
```

For `page_count = 1`: order 0 (1 page allocated). For `page_count = 2`: order 1 (2 pages). For `page_count = 3`: order 2 (4 pages allocated, 1 wasted). Acceptable for v1.0; production-grade per-page allocation deferred.

### Refcount ops

```c
void vmo_unref(struct Vmo *v) {
    if (!v) return;
    if (v->magic != VMO_MAGIC) extinction(...);
    if (v->handle_count <= 0)  extinction(...);
    v->handle_count--;
    if (v->handle_count == 0 && v->mapping_count == 0) {
        vmo_free_internal(v);
    }
}
```

The dual-check `(handle_count == 0 && mapping_count == 0)` is the runtime enforcement of the spec's `NoUseAfterFree` invariant. The spec's three buggy variants each correspond to a way this check could be wrong:

- **`BUGGY_FREE_ON_HANDLE_CLOSE`**: free without checking `mapping_count`. The impl prevents this by writing the dual check explicitly; a future caller that bypasses the check would fail the runtime extinction in `vmo_free_internal`.
- **`BUGGY_FREE_ON_UNMAP`**: free without checking `handle_count`. Same defense.
- **`BUGGY_NEVER_FREE`**: skip the free transition. The impl always evaluates the dual check at decrement time.

`vmo_unmap` is symmetric.

### Integration with handle table (`kernel/handle.c`)

`handle_close` calls `handle_release_obj(slot->kind, slot->obj)` which switches on kind:

```c
static void handle_release_obj(enum kobj_kind kind, void *obj) {
    if (!obj) return;
    switch (kind) {
    case KOBJ_VMO:
        vmo_unref((struct Vmo *)obj);
        break;
    /* Other kinds: refcount integration as their subsystems land. */
    }
}
```

`handle_dup` calls `handle_acquire_obj(parent->kind, parent->obj)` BEFORE allocating the child slot; on alloc failure, rolls back via `handle_release_obj`.

`handle_table_free` iterates open slots, calls `handle_release_obj` for each — the orphan-cleanup path proc_free relies on when a Proc dies with open handles.

### Bootstrap order

```
slub_init → pgrp_init → handle_init → vmo_init → proc_init → thread_init → sched_init
```

`vmo_init` between `handle_init` and `proc_init` (no kproc-VMO ownership at v1.0; the order is for grouping, not dependency).

---

## Spec cross-reference

`specs/vmo.tla` at P2-Fb:
- 9 actions: `Init`, `VmoCreate`, `HandleOpen`, `HandleClose`, `BuggyFreeOnHandleClose`, `MapVmo`, `UnmapVmo`, `BuggyFreeOnUnmap`, `BuggyNoFreeHandleClose` / `BuggyNoFreeUnmap`.
- 2 invariants: `RefcountConsistent`, `NoUseAfterFree`.
- 4 configs: `vmo.cfg` (clean; 100 distinct states / depth 9 / <1s) + 3 buggy variants (each producing a counterexample at depth 6).

Mapping (canonical at `specs/SPEC-TO-CODE.md`):

| Spec action | P2-Fd impl site | Notes |
|---|---|---|
| `Init` | `vmo_init` | SLUB cache init. |
| `VmoCreate(v)` | `vmo_create_anon(size)` | First handle (count=1); pages allocated. |
| `HandleOpen(v)` | `vmo_ref(v)` (via `handle_dup` and Phase 4's `handle_transfer_via_9p`) | handle_count++. |
| `HandleClose(v)` | `vmo_unref(v)` (via `handle_close`) | handle_count--; free if both = 0. |
| `BuggyFreeOnHandleClose(v)` | (none — bug class statically prevented by the dual-check) | |
| `MapVmo(v)` | `vmo_map(v)` (via Phase 3+ mmap_handle) | mapping_count++. |
| `UnmapVmo(v)` | `vmo_unmap(v)` (via Phase 3+ munmap) | mapping_count--; free if both = 0. |
| `BuggyFreeOnUnmap(v)` | (none — bug class statically prevented) | |
| `BuggyNoFreeHandleClose / BuggyNoFreeUnmap` | (none — the impl always evaluates the dual-check at decrement) | |

| Spec invariant | Source enforcement |
|---|---|
| `RefcountConsistent` | A struct Vmo doesn't exist before `vmo_create_anon`; counts and pointers are zero-initialized via SLUB's KP_ZERO; magic check rejects use-before-init. |
| `NoUseAfterFree` | Dual check `(handle_count == 0 && mapping_count == 0)` at the end of both `vmo_unref` and `vmo_unmap`. The check is written symmetrically — if either path forgets to check the OTHER count, the impl produces `BuggyFreeOnHandleClose` / `BuggyFreeOnUnmap` semantics, which the spec's TLC counterexamples capture. |

---

## Tests

- `vmo.create_close_round_trip` — basic create + unref → freed (cumulative counter increments).
- `vmo.refcount_lifecycle` — create + ref + unref + unref → freed only at last unref. Verifies handle_count tracking.
- `vmo.map_unmap_lifecycle` — create + map + unref + unmap → freed only on unmap. Verifies that handle_count alone reaching 0 does NOT trigger free if mapping_count > 0.
- `vmo.handles_x_mappings_matrix` — six combinations of close-then-unmap, unmap-then-close, multiple handles, multiple mappings, interleaved sequences. Each combination asserts the VMO frees at exactly the right moment (no premature, no delayed).
- `vmo.via_handle_table` — end-to-end: vmo_create_anon + handle_alloc(KOBJ_VMO) + handle_dup (ref) + handle_close (unref) → VMO freed on last close.
- `vmo.handle_table_orphan_cleanup` — proc_free with open KOBJ_VMO handles releases VMO references via handle_table_free's per-kind path.

---

## Known caveats / footguns

### `v` becomes invalid after the unref/unmap that frees the pages

After `vmo_unref` or `vmo_unmap` triggers the free transition (both counts == 0), the v pointer is invalid. The SLUB freelist write at `kmem_cache_free` clobbers the magic at offset 0; subsequent dereference may see a zero/garbage magic and extinct, OR (if SLUB recycles the slot for another VMO) may see a valid VMO_MAGIC and return wrong data.

Tests use cumulative counters (`vmo_total_destroyed`) to verify free transitions rather than dereferencing the freed pointer.

### `vmo_create_anon` allocates eagerly with order rounding

Page count is `ceil(size / PAGE_SIZE)`; allocation is a single `alloc_pages(order, KP_ZERO)` chunk where `order = ceil_log2(page_count)`. A VMO of 5 pages allocates 8 (waste of 3 pages). Acceptable for v1.0; per-page allocation can be added later.

### Anonymous VMOs only at v1.0

`VMO_TYPE_PHYS` (DMA/MMIO-backed) requires ARCH §19's CMA allocator + driver-startup integration; lands at Phase 3.

`VMO_TYPE_FILE` (Stratum page cache) requires the 9P client + the page cache; post-v1.0.

### `vmo_map` / `vmo_unmap` are refcount-only at v1.0

The actual VMA wire-up (page-table entries, fault handler) lands at Phase 3 with the demand-paging machinery. At P2-Fd these increment/decrement mapping_count without modifying any address space. Tests verify the refcount lifecycle; integration tests with actual mappings come at Phase 3.

### Single-CPU lifecycle

`vmo_ref` / `vmo_unref` / `vmo_map` / `vmo_unmap` are not internally synchronized. At v1.0 P2-Fd only the boot CPU calls these. Phase 5+ adds atomic operations on `handle_count` / `mapping_count` (the struct field types are already `int`; will become `atomic_int` or use `__atomic_*` builtins) when SMP syscalls go live.

### `_Static_assert` on `struct Vmo` size is intentionally absent

The struct contains `enum vmo_type` whose underlying integer type is implementation-defined (typically int). To avoid an unnecessarily fragile assert that would fail under different compiler defaults, we don't pin the size. The `_Static_assert(__builtin_offsetof(struct Vmo, magic) == 0)` IS pinned because the SLUB freelist write defense depends on it.

---

## Status

| Component | State |
|---|---|
| `vmo.h` API + `vmo.c` impl | Landed (P2-Fd) |
| SLUB cache + bootstrap | Landed (P2-Fd; vmo_init in main.c after handle_init) |
| `vmo_create_anon` (eager allocation) | Landed (P2-Fd) |
| `vmo_ref` / `vmo_unref` / `vmo_map` / `vmo_unmap` | Landed (P2-Fd) |
| Dual-check NoUseAfterFree enforcement | Landed (P2-Fd) |
| Integration with `handle.c` (KOBJ_VMO release/acquire) | Landed (P2-Fd) |
| In-kernel tests | 6 added: create_close_round_trip / refcount_lifecycle / map_unmap_lifecycle / handles_x_mappings_matrix / via_handle_table / handle_table_orphan_cleanup |
| Spec `vmo.tla` + 3 buggy configs | Landed (P2-Fb) |
| `vmo_create_physical` (VMO_TYPE_PHYS) | Phase 3 |
| `VMO_TYPE_FILE` (Stratum page cache) | Post-v1.0 |
| Actual VMA wire-up (page-table entries) | Phase 3 (with demand-paging fault handler) |
| Atomic refcount ops (SMP) | Phase 5+ |
