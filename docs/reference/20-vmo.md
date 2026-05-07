# 20 — Virtual Memory Object (VMO) (P2-F / P3-Db)

A Virtual Memory Object — kernel object representing a region of memory, independent of any address space. Per `ARCHITECTURE.md §19`. v1.0 P2-Fd lands the kernel-internal API + the dual-refcount lifecycle. P3-Db extends the public API with `vmo_map(Proc*, Vmo*, vaddr, length, prot)` and `vmo_unmap(Proc*, vaddr, length)` that install / remove VMAs. PTE installation (demand paging) lands at P3-Dc. The syscall surface (`vmo_create`, `vmo_read`, `vmo_write`, `vmo_get_size`) lands at Phase 5+.

---

## Purpose

VMOs are Thylacine's unit of memory sharing. A VMO is created once, referenced by handles, and mapped into address spaces. Pages are eagerly allocated at create time (v1.0); they live until the last handle is closed AND the last mapping is unmapped — the moment both reach 0, the pages are freed.

Key invariant (proven in `specs/vmo.tla`):

- **No use-after-free (ARCH §28 I-7)**: `pages_alive[v] iff (handle_count[v] > 0 OR mapping_count[v] > 0)`. Catches premature free (counts > 0 but pages dead) AND delayed free (counts = 0 but pages alive).

The dual refcount discipline:

- **handle_count**: number of open handles to this VMO. Modified by `vmo_create_anon` (sets to 1), `vmo_ref` (called by `handle_dup` and Phase 4's `handle_transfer_via_9p`), `vmo_unref` (called by `handle_close`).
- **mapping_count**: number of address-space mappings. Incremented by `vmo_acquire_mapping` (called from `vma_alloc`); decremented by `vmo_release_mapping` (called from `vma_free`). The high-level entry points `vmo_map(Proc*, ...)` and `vmo_unmap(Proc*, ...)` route through `vma_alloc + vma_insert` and `vma_remove + vma_free` respectively, so the count tracks the per-Proc VMA tree population.
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

// Refcount-only ops (internal to the VMA layer; tests use them to
// exercise the dual-refcount lifecycle in isolation).
void          vmo_acquire_mapping(struct Vmo *v);
void          vmo_release_mapping(struct Vmo *v);

// High-level public API (P3-Db). Returns 0 on success, -1 on failure.
int           vmo_map(struct Proc *p, struct Vmo *v,
                      u64 vaddr, size_t length, u32 prot);
int           vmo_unmap(struct Proc *p, u64 vaddr, size_t length);

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

### `vmo_unref` / `vmo_release_mapping` — invalidation

After the last unref/release that brings BOTH counts to 0, the v pointer is **invalid**. The SLUB freelist clobbers magic; the memory may be reused. Callers must not dereference v after that point. The high-level `vmo_unmap(Proc*, ...)` entry routes through `vma_free` → `vmo_release_mapping`; the same caveat applies, with the additional note that the VMA itself is also freed.

### `vmo_map(Proc*, Vmo*, vaddr, length, prot)` — return semantics

| Return | Meaning |
|---|---|
| `0` | success: a Vma was allocated, takes a `vmo_acquire_mapping` ref, and was inserted into Proc's sorted list. |
| `-1` | failure: NULL inputs, zero length, unaligned `vaddr` or `length`, W+X prot, vaddr+length overflow, VMA SLUB OOM, or VMA overlap with an existing entry. mapping_count UNCHANGED on failure (overlap rejection's `vma_free` reverses the `vmo_acquire_mapping` taken by `vma_alloc`). |

### `vmo_unmap(Proc*, vaddr, length)` — return semantics

| Return | Meaning |
|---|---|
| `0` | success: the VMA exactly matching `[vaddr, vaddr + length)` was removed and freed. mapping_count--; the VMO may be freed if it was the last mapping AND no handles are open. |
| `-1` | no matching VMA. Partial unmap (a sub-range of an existing VMA) is post-v1.0; v1.0 requires exact-match. |

### Defensive checks (extinct on violation)

- NULL passed where required (vmo_ref / vmo_acquire_mapping / vmo_release_mapping).
- Corrupted magic (vmo_ref / vmo_unref / vmo_acquire_mapping / vmo_release_mapping) — UAF defense.
- Zero-ref unref or zero-mapping release — refcount underflow.
- vmo_ref on a VMO with both counts=0 (already-freed identity).
- vmo_acquire_mapping on a VMO with NULL pages (use-after-free of the backing pages).

---

## Implementation

`kernel/vmo.c` (~190 LOC).

### Lifecycle

- `vmo_init`: SLUB cache + idempotency check. Called from `boot_main` after `handle_init` (the caches are independent; ordering is for grouping).
- `vmo_create_anon`: SLUB-allocate via `kmem_cache_alloc(KP_ZERO)`; allocate backing pages via `alloc_pages(order, KP_ZERO)` where `order = ceil_log2(page_count)`. Sets handle_count=1, mapping_count=0. On any allocation failure, rolls back cleanly.
- `vmo_free_internal` (static): the symmetric internal helper called by both `vmo_unref` and `vmo_release_mapping` when both counts reach 0. Validates state (counts == 0; pages != NULL), calls `free_pages(v->pages, v->order)`, then `kmem_cache_free`. Increments `g_vmo_destroyed`.

### High-level map / unmap (P3-Db)

`vmo_map(Proc*, Vmo*, vaddr, length, prot)`:
1. Validate args (NULL/zero/alignment/overflow). Return -1 on any violation.
2. `vma_alloc(vaddr, vaddr+length, prot, v, /*offset=*/0)` — allocates a Vma + takes `vmo_acquire_mapping`. Returns NULL on constraint violation (W+X reject; included for redundancy with the arg validation) or SLUB OOM.
3. `vma_insert(p, vma)` — sorted-list insert with overlap detection. Returns -1 on overlap; on overlap, `vma_free(vma)` is called (which releases the mapping ref).
4. Return 0 on success.

The arg validation in step 1 is duplicated from `vma_alloc`'s checks for cheap early-return + clearer diagnostics; `vma_alloc` does the same checks again to harden against future direct callers.

`vmo_unmap(Proc*, vaddr, length)`:
1. Validate args.
2. `vma_lookup(p, vaddr)` — fast O(N) sorted walk to find a VMA covering `vaddr`.
3. Verify the VMA exactly matches `[vaddr, vaddr + length)` — otherwise return -1.
4. `vma_remove + vma_free` — symmetric tear-down releasing the mapping ref.

PTE installation / teardown is NOT performed by either entry. PTEs are installed lazily by demand paging (P3-Dc); the per-Proc TTBR0 tree starts empty and grows on faults.

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
| `MapVmo(v)` | `vmo_acquire_mapping(v)` (via `vma_alloc` from `vmo_map(Proc*, ...)`) | mapping_count++. |
| `UnmapVmo(v)` | `vmo_release_mapping(v)` (via `vma_free` from `vmo_unmap(Proc*, ...)`) | mapping_count--; free if both = 0. |
| `BuggyFreeOnUnmap(v)` | (none — bug class statically prevented) | |
| `BuggyNoFreeHandleClose / BuggyNoFreeUnmap` | (none — the impl always evaluates the dual-check at decrement) | |

| Spec invariant | Source enforcement |
|---|---|
| `RefcountConsistent` | A struct Vmo doesn't exist before `vmo_create_anon`; counts and pointers are zero-initialized via SLUB's KP_ZERO; magic check rejects use-before-init. |
| `NoUseAfterFree` | Dual check `(handle_count == 0 && mapping_count == 0)` at the end of both `vmo_unref` and `vmo_release_mapping`. The check is written symmetrically — if either path forgets to check the OTHER count, the impl produces `BuggyFreeOnHandleClose` / `BuggyFreeOnUnmap` semantics, which the spec's TLC counterexamples capture. |

---

## Tests

- `vmo.create_close_round_trip` — basic create + unref → freed (cumulative counter increments).
- `vmo.refcount_lifecycle` — create + ref + unref + unref → freed only at last unref. Verifies handle_count tracking.
- `vmo.map_unmap_lifecycle` — create + acquire_mapping + unref + release_mapping → freed only on the last release. Verifies that handle_count alone reaching 0 does NOT trigger free if mapping_count > 0. Uses the bare refcount ops directly so the lifecycle is exercised in isolation from the VMA layer.
- `vmo.handles_x_mappings_matrix` — six combinations of close-then-release, release-then-close, multiple handles, multiple mappings, interleaved sequences. Each combination asserts the VMO frees at exactly the right moment (no premature, no delayed).
- `vmo.via_handle_table` — end-to-end: vmo_create_anon + handle_alloc(KOBJ_VMO) + handle_dup (ref) + handle_close (unref) → VMO freed on last close.
- `vmo.handle_table_orphan_cleanup` — proc_free with open KOBJ_VMO handles releases VMO references via handle_table_free's per-kind path.

### P3-Db tests (`vmo_map(Proc*, ...)` / `vmo_unmap(Proc*, ...)`)

- `vmo.map_proc_smoke` — `vmo_map(p, v, vaddr, len, prot)` installs a VMA visible via `vma_lookup`; mapping_count tracks; vma_drain releases.
- `vmo.map_proc_constraints` — invalid args (NULL, zero length, unaligned, W+X) return -1 without touching mapping_count.
- `vmo.map_proc_overlap_rejected` — exact + partial overlap rejected with -1; on rejection, mapping_count is rolled back via `vma_free` (the `vmo_acquire_mapping` taken inside `vma_alloc` is symmetrically released).
- `vmo.unmap_proc_smoke` — `vmo_unmap(p, vaddr, len)` removes the VMA + decrements mapping_count.
- `vmo.unmap_proc_no_match` — non-matching range / unaligned start / wrong length all return -1 without disturbing existing VMAs.

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

### `vmo_map(Proc*, ...)` does NOT install PTEs

At v1.0 P3-Db the entry point creates a VMA via `vma_alloc + vma_insert` and stops there. Page-table entry installation is deferred to demand paging via the user-mode page-fault path (P3-Dc): on user access to an unmapped VA, `arch_fault_handle`'s user-mode dispatch case calls `vma_lookup`, allocates a backing page, and installs an L3 PTE in the per-Proc TTBR0 tree. Until P3-Dc lands, a `vmo_map`'d range exists in the VMA tree but causes a fault if accessed (FAULT_UNHANDLED_USER returns from `arch_fault_handle`).

### `vmo_unmap(Proc*, ...)` requires exact-match at v1.0

Partial unmap (a sub-range of an existing VMA) is post-v1.0. The v1.0 entry compares VMA `vaddr_start == vaddr` AND `vaddr_end == vaddr + length` and returns -1 on mismatch. Splitting a VMA into two halves with a hole would require allocating a fresh Vma + reordering the sorted list; that work is deferred until userspace `munmap` is wired (Phase 5+).

### Single-CPU lifecycle

`vmo_ref` / `vmo_unref` / `vmo_acquire_mapping` / `vmo_release_mapping` and the high-level `vmo_map` / `vmo_unmap` paths are not internally synchronized. At v1.0 P3-Db only the boot CPU mutates per-Proc VMA state from outside the running thread (single-thread-Proc invariant from P2-D). Phase 5+ adds atomic operations on `handle_count` / `mapping_count` (the struct field types are already `int`; will become `atomic_int` or use `__atomic_*` builtins) when SMP syscalls go live, plus a per-Proc lock around `vma_insert`/`vma_remove` to handle concurrent multi-thread `mmap`/`munmap` callers.

### `_Static_assert` on `struct Vmo` size is intentionally absent

The struct contains `enum vmo_type` whose underlying integer type is implementation-defined (typically int). To avoid an unnecessarily fragile assert that would fail under different compiler defaults, we don't pin the size. The `_Static_assert(__builtin_offsetof(struct Vmo, magic) == 0)` IS pinned because the SLUB freelist write defense depends on it.

---

## Status

| Component | State |
|---|---|
| `vmo.h` API + `vmo.c` impl | Landed (P2-Fd) |
| SLUB cache + bootstrap | Landed (P2-Fd; vmo_init in main.c after handle_init) |
| `vmo_create_anon` (eager allocation) | Landed (P2-Fd) |
| `vmo_ref` / `vmo_unref` / `vmo_acquire_mapping` / `vmo_release_mapping` | Landed (P2-Fd; renamed at P3-Db) |
| `vmo_map(Proc*, ...)` / `vmo_unmap(Proc*, ...)` (high-level VMA-installing entry points) | Landed (P3-Db) |
| Dual-check NoUseAfterFree enforcement | Landed (P2-Fd) |
| Integration with `handle.c` (KOBJ_VMO release/acquire) | Landed (P2-Fd) |
| In-kernel tests | 6 (P2-Fd) + 5 (P3-Db) = 11 covering refcount lifecycle + new VMA-installing entry points + overlap rollback. |
| Spec `vmo.tla` + 3 buggy configs | Landed (P2-Fb); covers `MapVmo` / `UnmapVmo` actions. P3-Db's high-level entry is a thin orchestrator over the spec-modeled refcount + `vma_insert` overlap; no spec extension needed at this sub-chunk. |
| `vmo_create_physical` (VMO_TYPE_PHYS) | Phase 3+ (after P3-Dc) |
| `VMO_TYPE_FILE` (Stratum page cache) | Post-v1.0 |
| PTE installation (demand paging) | P3-Dc (next sub-chunk) |
| Partial unmap (sub-range of an existing VMA) | Post-v1.0 |
| Atomic refcount ops (SMP) | Phase 5+ |
