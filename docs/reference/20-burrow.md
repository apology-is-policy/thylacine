# 20 — Virtual Memory Object (BURROW) (P2-F / P3-Db)

A Virtual Memory Object — kernel object representing a region of memory, independent of any address space. Per `ARCHITECTURE.md §19`. v1.0 P2-Fd lands the kernel-internal API + the dual-refcount lifecycle. P3-Db extends the public API with `burrow_map(Proc*, Burrow*, vaddr, length, prot)` and `burrow_unmap(Proc*, vaddr, length)` that install / remove VMAs. PTE installation (demand paging) lands at P3-Dc. The syscall surface (`burrow_create`, `burrow_read`, `burrow_write`, `burrow_get_size`) lands at Phase 5+.

---

## Purpose

VMOs are Thylacine's unit of memory sharing. A BURROW is created once, referenced by handles, and mapped into address spaces. Pages are eagerly allocated at create time (v1.0); they live until the last handle is closed AND the last mapping is unmapped — the moment both reach 0, the pages are freed.

Key invariant (proven in `specs/burrow.tla`):

- **No use-after-free (ARCH §28 I-7)**: `pages_alive[v] iff (handle_count[v] > 0 OR mapping_count[v] > 0)`. Catches premature free (counts > 0 but pages dead) AND delayed free (counts = 0 but pages alive).

The dual refcount discipline:

- **handle_count**: number of open handles to this BURROW. Modified by `burrow_create_anon` (sets to 1), `burrow_ref` (called by `handle_dup` and Phase 4's `handle_transfer_via_9p`), `burrow_unref` (called by `handle_close`).
- **mapping_count**: number of address-space mappings. Incremented by `burrow_acquire_mapping` (called from `vma_alloc`); decremented by `burrow_release_mapping` (called from `vma_free`). The high-level entry points `burrow_map(Proc*, ...)` and `burrow_unmap(Proc*, ...)` route through `vma_alloc + vma_insert` and `vma_remove + vma_free` respectively, so the count tracks the per-Proc VMA tree population.
- Pages free iff both reach 0. Whichever brings the last count to 0 triggers the free transition.

---

## Public API — `<thylacine/burrow.h>`

```c
#define VMO_MAGIC 0x564D4F00BADC0DE5ULL    // 'BURROW\0' || 0xBADC0DE5

enum burrow_type {
    BURROW_TYPE_INVALID = 0,
    BURROW_TYPE_ANON    = 1,
    // Phase 3+ : BURROW_TYPE_PHYS  (DMA / MMIO buffers)
    // Post-v1.0: BURROW_TYPE_FILE  (Stratum page cache)
};

struct page;

struct Burrow {
    u64            magic;          // VMO_MAGIC; clobbered to 0 in burrow_free_internal before kmem_cache_free (R9 F148; R13 F213)
    enum burrow_type  type;
    size_t         size;           // rounded up to page_count * PAGE_SIZE
    size_t         page_count;
    int            handle_count;   // open handles to this BURROW
    int            mapping_count;  // open mappings (vma's)
    struct page   *pages;          // alloc_pages chunk; NULL after free
    unsigned       order;          // for free_pages
};
_Static_assert(__builtin_offsetof(struct Burrow, magic) == 0, ...);

void          burrow_init(void);
struct Burrow   *burrow_create_anon(size_t size);
void          burrow_ref(struct Burrow *v);
void          burrow_unref(struct Burrow *v);

// Refcount-only ops (internal to the VMA layer; tests use them to
// exercise the dual-refcount lifecycle in isolation).
void          burrow_acquire_mapping(struct Burrow *v);
void          burrow_release_mapping(struct Burrow *v);

// High-level public API (P3-Db). Returns 0 on success, -1 on failure.
int           burrow_map(struct Proc *p, struct Burrow *v,
                      u64 vaddr, size_t length, u32 prot);
int           burrow_unmap(struct Proc *p, u64 vaddr, size_t length);

// Weft-2 / I-37: map the WHOLE of an ANON Burrow into a SECOND Proc (the
// per-flow dataplane share). length is implicit (v->size). 0 / -1.
int           burrow_share_into(struct Proc *dst, struct Burrow *v,
                      u64 vaddr, u32 prot);

size_t        burrow_get_size(const struct Burrow *v);
int           burrow_handle_count(const struct Burrow *v);
int           burrow_mapping_count(const struct Burrow *v);

u64           burrow_total_created(void);
u64           burrow_total_destroyed(void);
```

### `burrow_create_anon(size)` — return semantics

| Return | Meaning |
|---|---|
| `non-NULL` | success; struct Burrow with handle_count=1, mapping_count=0, pages allocated. The caller's "consumed reference" — `handle_alloc` does NOT increment the count. |
| `NULL` | failure: size==0, SLUB OOM, or alloc_pages OOM. |

### `burrow_unref` / `burrow_release_mapping` — invalidation

After the last unref/release that brings BOTH counts to 0, the v pointer is **invalid**. `burrow_free_internal` clobbers `v->magic = 0` immediately before `kmem_cache_free` (R9 F148 discipline; closed at R13 F213 / P4-N); SLUB's freelist write may then overwrite the slot's first 8 bytes with its next-pointer. Either way, the slot's first qword is no longer `VMO_MAGIC`. A subsequent stale-pointer `burrow_ref` / `burrow_acquire_mapping` extincts via the magic check rather than masking the UAF. Callers must not dereference v after the free transition. The high-level `burrow_unmap(Proc*, ...)` entry routes through `vma_free` → `burrow_release_mapping`; the same caveat applies, with the additional note that the VMA itself is also freed.

### `burrow_map(Proc*, Burrow*, vaddr, length, prot)` — return semantics

| Return | Meaning |
|---|---|
| `0` | success: a Vma was allocated, takes a `burrow_acquire_mapping` ref, and was inserted into Proc's sorted list. |
| `-1` | failure: NULL inputs, zero length, unaligned `vaddr` or `length`, W+X prot, vaddr+length overflow, vaddr+length > `USER_VA_TOP` (= 1 << 47; R12-vaddr close of F180), VMA SLUB OOM, or VMA overlap with an existing entry. mapping_count UNCHANGED on failure (overlap rejection's `vma_free` reverses the `burrow_acquire_mapping` taken by `vma_alloc`). |

The upper-bound check `vaddr + length > USER_VA_TOP` (added at R12-vaddr) enforces the TTBR0 user-half bound at the VMA layer. The same bit-47 reject in `mmu_install_user_pte` (R10 F158) remains as per-page defense-in-depth on the demand-page path. A `_Static_assert` in `burrow_map` pins `USER_VA_TOP == (1ull << 47)` to the mmu.c bound — if either bound shifts (e.g., a port to 52-bit VAs), both must move together.

### `burrow_unmap(Proc*, vaddr, length)` — return semantics

| Return | Meaning |
|---|---|
| `0` | success: the VMA exactly matching `[vaddr, vaddr + length)` was removed and freed. mapping_count--; the BURROW may be freed if it was the last mapping AND no handles are open. |
| `-1` | no matching VMA. Partial unmap (a sub-range of an existing VMA) is post-v1.0; v1.0 requires exact-match. |

### `burrow_share_into(Proc*, Burrow*, vaddr, prot)` — cross-Proc share (Weft-2 / I-37)

The Weft capability dataplane (`docs/NET-THROUGHPUT.md §6`; `ARCHITECTURE.md §28 I-37`) needs one Burrow — a per-flow ring — reachable from **two** Procs: the guest and netd. `burrow_share_into` is that path, the tree's **first** cross-Proc-shared Burrow. It maps the **whole** Burrow (`length = v->size`; a share is always whole-ring, so the caller passes no length) into `dst`'s address space at `vaddr`, taking the `mapping_count` ref that keeps `v` alive for `dst` **independent** of the other Proc's refs. NO Burrow *handle* crosses Procs — `dst` holds only a mapping (grant-is-the-share; the capability is holding the namespace-gated flow fid, I-1/I-28).

It is a thin composition of `burrow_map`, which is already Proc-agnostic (it takes any `p`) and #847-SMP-safe. The only new property is that the two refcount holders now sit in **different** Procs — sound because the dual-refcount lock (`v->lock`) is **per-Burrow, not per-Proc**, so it serializes `dst`'s `burrow_acquire_mapping` against the other Proc's `burrow_unref` / `burrow_release_mapping` identically to two threads of one Proc; the Burrow frees only when ALL refs (both Procs' mappings + any handle) drop, in any interleaving (`weft.tla::ShareBoundedByFlow`). Lock order is acyclic across Procs: A holds `A->vma_lock -> v->lock`, B holds `B->vma_lock -> v->lock` — distinct per-Proc leaf-ward `vma_lock`s, one shared inner `v->lock`.

| Return | Meaning |
|---|---|
| `0` | success: `v` mapped whole into `dst`; `mapping_count++`. |
| `-1` | NULL `dst`/`v`; corrupted, non-ANON, or zero-size `v`; W+X `prot` (rejected by `vma_alloc`, I-12); VMA overlap; or VMA SLUB OOM. No mapping installed; `v`'s refcount unchanged. |

**Scope**: ANON only — a dataplane ring is anonymous memory; cross-Proc sharing of an MMIO/DMA Burrow is a distinct (unaudited) hardware-mapping surface (its own I-5 analysis owed) and fails closed here.

**Preconditions** (the same shape as `burrow_map`): the caller holds `dst->vma_lock` (a `dst->vmas` mutator), AND guarantees `v` stays live across the call (a held ref, or a higher-level lock excluding a concurrent teardown to `{h:0,m:0}` — the Weft-6 caller serializes the data-fid open against flow teardown). The substrate cannot manufacture liveness for the instant before `vma_alloc` bumps the count; if both counts were already 0, `burrow_acquire_mapping`'s resurrection guard extincts.

### Defensive checks (extinct on violation)

- NULL passed where required (burrow_ref / burrow_acquire_mapping / burrow_release_mapping).
- Corrupted magic (burrow_ref / burrow_unref / burrow_acquire_mapping / burrow_release_mapping) — UAF defense.
- Zero-ref unref or zero-mapping release — refcount underflow.
- burrow_ref on a BURROW with both counts=0 (already-freed identity).
- burrow_acquire_mapping on a BURROW with NULL pages (use-after-free of the backing pages).

---

## Implementation

`kernel/burrow.c` (~190 LOC).

### Lifecycle

- `burrow_init`: SLUB cache + idempotency check. Called from `boot_main` after `handle_init` (the caches are independent; ordering is for grouping).
- `burrow_create_anon`: SLUB-allocate via `kmem_cache_alloc(KP_ZERO)`; allocate backing pages via `alloc_pages(order, KP_ZERO)` where `order = ceil_log2(page_count)`. Sets handle_count=1, mapping_count=0. On any allocation failure, rolls back cleanly.
- `burrow_free_internal` (static): the symmetric internal helper called by both `burrow_unref` and `burrow_release_mapping` when both counts reach 0. Validates state (counts == 0; pages != NULL), calls `free_pages(v->pages, v->order)`, then `kmem_cache_free`. Increments `g_vmo_destroyed`.

### High-level map / unmap (P3-Db)

`burrow_map(Proc*, Burrow*, vaddr, length, prot)`:
1. Validate args (NULL/zero/alignment/overflow). Return -1 on any violation.
2. `vma_alloc(vaddr, vaddr+length, prot, v, /*offset=*/0)` — allocates a Vma + takes `burrow_acquire_mapping`. Returns NULL on constraint violation (W+X reject; included for redundancy with the arg validation) or SLUB OOM.
3. `vma_insert(p, vma)` — sorted-list insert with overlap detection. Returns -1 on overlap; on overlap, `vma_free(vma)` is called (which releases the mapping ref).
4. Return 0 on success.

The arg validation in step 1 is duplicated from `vma_alloc`'s checks for cheap early-return + clearer diagnostics; `vma_alloc` does the same checks again to harden against future direct callers.

`burrow_unmap(Proc*, vaddr, length)`:
1. Validate args.
2. `vma_lookup(p, vaddr)` — fast O(N) sorted walk to find a VMA covering `vaddr`.
3. Verify the VMA exactly matches `[vaddr, vaddr + length)` — otherwise return -1.
4. `vma_remove + vma_free` — symmetric tear-down releasing the mapping ref.

PTE *installation* is NOT performed by `burrow_map` -- PTEs are installed lazily by demand paging (P3-Dc); the per-Proc TTBR0 tree starts empty and grows on faults. PTE *teardown* IS performed by `burrow_unmap` (RW-1 C-F6 doc-fold; the p6 hardening #2 / F1 fix): `vma_free`'s teardown calls `mmu_uninstall_user_range` over the VMA's range, clearing the leaf PTEs + broadcasting `tlbi vaae1is` BEFORE the backing pages are freed -- without it, stale PTEs/TLB entries would persist after detach (the suspected AEGIS-256/mallocng corruption class). The clear is idempotent on never-faulted-in pages.

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
void burrow_unref(struct Burrow *v) {
    if (!v) return;
    if (v->magic != VMO_MAGIC) extinction(...);
    if (v->handle_count <= 0)  extinction(...);
    v->handle_count--;
    if (v->handle_count == 0 && v->mapping_count == 0) {
        burrow_free_internal(v);
    }
}
```

The dual-check `(handle_count == 0 && mapping_count == 0)` is the runtime enforcement of the spec's `NoUseAfterFree` invariant. The spec's three buggy variants each correspond to a way this check could be wrong:

- **`BUGGY_FREE_ON_HANDLE_CLOSE`**: free without checking `mapping_count`. The impl prevents this by writing the dual check explicitly; a future caller that bypasses the check would fail the runtime extinction in `burrow_free_internal`.
- **`BUGGY_FREE_ON_UNMAP`**: free without checking `handle_count`. Same defense.
- **`BUGGY_NEVER_FREE`**: skip the free transition. The impl always evaluates the dual check at decrement time.

`burrow_unmap` is symmetric.

### Integration with handle table (`kernel/handle.c`)

`handle_close` calls `handle_release_obj(slot->kind, slot->obj)` which switches on kind:

```c
static void handle_release_obj(enum kobj_kind kind, void *obj) {
    if (!obj) return;
    switch (kind) {
    case KOBJ_BURROW:
        burrow_unref((struct Burrow *)obj);
        break;
    /* Other kinds: refcount integration as their subsystems land. */
    }
}
```

`handle_dup` calls `handle_acquire_obj(parent->kind, parent->obj)` BEFORE allocating the child slot; on alloc failure, rolls back via `handle_release_obj`.

`handle_table_free` iterates open slots, calls `handle_release_obj` for each — the orphan-cleanup path proc_free relies on when a Proc dies with open handles.

### Bootstrap order

```
slub_init → territory_init → handle_init → burrow_init → proc_init → thread_init → sched_init
```

`burrow_init` between `handle_init` and `proc_init` (no kproc-BURROW ownership at v1.0; the order is for grouping, not dependency).

---

## Spec cross-reference

`specs/burrow.tla` at P2-Fb:
- 9 actions: `Init`, `VmoCreate`, `HandleOpen`, `HandleClose`, `BuggyFreeOnHandleClose`, `MapVmo`, `UnmapVmo`, `BuggyFreeOnUnmap`, `BuggyNoFreeHandleClose` / `BuggyNoFreeUnmap`.
- 2 invariants: `RefcountConsistent`, `NoUseAfterFree`.
- 4 configs: `burrow.cfg` (clean; 100 distinct states / depth 9 / <1s) + 3 buggy variants (each producing a counterexample at depth 6).

Mapping (canonical at `specs/SPEC-TO-CODE.md`):

| Spec action | P2-Fd impl site | Notes |
|---|---|---|
| `Init` | `burrow_init` | SLUB cache init. |
| `VmoCreate(v)` | `burrow_create_anon(size)` | First handle (count=1); pages allocated. |
| `HandleOpen(v)` | `burrow_ref(v)` (via `handle_dup` and Phase 4's `handle_transfer_via_9p`) | handle_count++. |
| `HandleClose(v)` | `burrow_unref(v)` (via `handle_close`) | handle_count--; free if both = 0. |
| `BuggyFreeOnHandleClose(v)` | (none — bug class statically prevented by the dual-check) | |
| `MapVmo(v)` | `burrow_acquire_mapping(v)` (via `vma_alloc` from `burrow_map(Proc*, ...)`) | mapping_count++. |
| `UnmapVmo(v)` | `burrow_release_mapping(v)` (via `vma_free` from `burrow_unmap(Proc*, ...)`) | mapping_count--; free if both = 0. |
| `BuggyFreeOnUnmap(v)` | (none — bug class statically prevented) | |
| `BuggyNoFreeHandleClose / BuggyNoFreeUnmap` | (none — the impl always evaluates the dual-check at decrement) | |

| Spec invariant | Source enforcement |
|---|---|
| `RefcountConsistent` | A struct Burrow doesn't exist before `burrow_create_anon`; counts and pointers are zero-initialized via SLUB's KP_ZERO; magic check rejects use-before-init. |
| `NoUseAfterFree` | Dual check `(handle_count == 0 && mapping_count == 0)` at the end of both `burrow_unref` and `burrow_release_mapping`. The check is written symmetrically — if either path forgets to check the OTHER count, the impl produces `BuggyFreeOnHandleClose` / `BuggyFreeOnUnmap` semantics, which the spec's TLC counterexamples capture. |

---

## Tests

- `burrow.create_close_round_trip` — basic create + unref → freed (cumulative counter increments).
- `burrow.refcount_lifecycle` — create + ref + unref + unref → freed only at last unref. Verifies handle_count tracking.
- `burrow.map_unmap_lifecycle` — create + acquire_mapping + unref + release_mapping → freed only on the last release. Verifies that handle_count alone reaching 0 does NOT trigger free if mapping_count > 0. Uses the bare refcount ops directly so the lifecycle is exercised in isolation from the VMA layer.
- `burrow.handles_x_mappings_matrix` — six combinations of close-then-release, release-then-close, multiple handles, multiple mappings, interleaved sequences. Each combination asserts the BURROW frees at exactly the right moment (no premature, no delayed).
- `burrow.via_handle_table` — end-to-end: burrow_create_anon + handle_alloc(KOBJ_BURROW) + handle_dup (ref) + handle_close (unref) → BURROW freed on last close.
- `burrow.handle_table_orphan_cleanup` — proc_free with open KOBJ_BURROW handles releases BURROW references via handle_table_free's per-kind path.

### P3-Db tests (`burrow_map(Proc*, ...)` / `burrow_unmap(Proc*, ...)`)

- `burrow.map_proc_smoke` — `burrow_map(p, v, vaddr, len, prot)` installs a VMA visible via `vma_lookup`; mapping_count tracks; vma_drain releases.
- `burrow.map_proc_constraints` — invalid args (NULL, zero length, unaligned, W+X) return -1 without touching mapping_count.
- `burrow.map_proc_overlap_rejected` — exact + partial overlap rejected with -1; on rejection, mapping_count is rolled back via `vma_free` (the `burrow_acquire_mapping` taken inside `vma_alloc` is symmetrically released).
- `burrow.unmap_proc_smoke` — `burrow_unmap(p, vaddr, len)` removes the VMA + decrements mapping_count.
- `burrow.unmap_proc_no_match` — non-matching range / unaligned start / wrong length all return -1 without disturbing existing VMAs.

### Weft-2 tests (`burrow_share_into(Proc*, ...)` — cross-Proc share)

- `burrow.share_into_cross_proc` — one ANON Burrow mapped into TWO Procs (netd via `burrow_map`, the guest via `burrow_share_into`); both Procs' VMAs reference the IDENTICAL Burrow → the identical backing page (a byte on the shared page is visible to both mappings); `mapping_count == 2`; teardown frees on the last ref drop.
- `burrow.share_into_alive_while_either_maps` — drop the construction HANDLE while two mappings remain → `v` stays alive (mappings alone keep it, h==0 — the grant-is-the-share guest-holds-only-a-mapping case); frees only when the LAST mapping drops.
- `burrow.share_into_frees_on_last_drop` — the reverse teardown order (both mappings, then the handle last); with the test above, witnesses order-independence (free iff ALL refs gone — the cross-Proc #847 proof). Also exercises the multi-page whole-ring share.
- `burrow.share_into_constraints` — NULL inputs, W+X prot, and a same-VA overlap within one Proc each return -1 without disturbing `mapping_count`.

---

## Known caveats / footguns

### `v` becomes invalid after the unref/unmap that frees the pages

After `burrow_unref` or `burrow_unmap` triggers the free transition (both counts == 0), the v pointer is invalid. `burrow_free_internal` clobbers `v->magic = 0` immediately before `kmem_cache_free` (R9 F148 discipline; R13 F213 / P4-N close). Subsequent stale-pointer access either sees `0` (if SLUB hasn't repurposed the slot) or SLUB's freelist next-pointer — neither matches `VMO_MAGIC`, so `burrow_ref` / `burrow_acquire_mapping` / `burrow_get_size` / `burrow_handle_count` / `burrow_mapping_count` all reject. The narrow remaining UAF window (SLUB recycles the same Burrow slot for *another* Burrow before the stale dereference) is bounded by the magic check's mismatch on `VMO_MAGIC` once the new constructor has overwritten the qword — at which point the stale pointer reads valid-but-different Burrow state. The handle-table layer's own magic check on the kobj guards against this composing into wrong semantics (see `kernel/handle.c::handle_acquire_obj`).

Tests use cumulative counters (`burrow_total_destroyed`) to verify free transitions rather than dereferencing the freed pointer.

### `burrow_create_anon` allocates eagerly with order rounding

Page count is `ceil(size / PAGE_SIZE)`; allocation is a single `alloc_pages(order, KP_ZERO)` chunk where `order = ceil_log2(page_count)`. A BURROW of 5 pages allocates 8 (waste of 3 pages). Acceptable for v1.0; per-page allocation can be added later.

### Burrow types

`BURROW_TYPE_ANON` (anonymous, demand-paged) is the P3-Db type. `BURROW_TYPE_MMIO` (`burrow_create_mmio`, P4-Ic1) and `BURROW_TYPE_DMA` (`burrow_create_dma`, P4-Ic5b1b) back device-MMIO and DMA buffers respectively -- both landed in Phase 4 (the `BURROW_TYPE_PHYS` placeholder this section once named was superseded by the MMIO/DMA split).

`BURROW_TYPE_FILE` (Stratum page cache) requires the 9P client + the page cache; post-v1.0.

### `burrow_map(Proc*, ...)` does NOT install PTEs

At v1.0 P3-Db the entry point creates a VMA via `vma_alloc + vma_insert` and stops there. Page-table entry installation is deferred to demand paging via the user-mode page-fault path (P3-Dc): on user access to an unmapped VA, `arch_fault_handle`'s user-mode dispatch case calls `vma_lookup`, allocates a backing page, and installs an L3 PTE in the per-Proc TTBR0 tree. Until P3-Dc lands, a `burrow_map`'d range exists in the VMA tree but causes a fault if accessed (FAULT_UNHANDLED_USER returns from `arch_fault_handle`).

### `burrow_unmap(Proc*, ...)` requires exact-match at v1.0

Partial unmap (a sub-range of an existing VMA) is post-v1.0. The v1.0 entry compares VMA `vaddr_start == vaddr` AND `vaddr_end == vaddr + length` and returns -1 on mismatch. Splitting a VMA into two halves with a hole would require allocating a fresh Vma + reordering the sorted list; that work is deferred until userspace `munmap` is wired (Phase 5+).

### SMP-safe refcount lifecycle (#847)

`burrow_ref` / `burrow_unref` / `burrow_acquire_mapping` / `burrow_release_mapping` run the `handle_count` + `mapping_count` mutations AND the `both-counts-zero` free decision under a **per-Burrow `spin_lock` (`v->lock`)** -- the #847 fix. A multi-thread Proc reaches these from concurrent CPUs (e.g. a sibling `handle_close` racing a mapping teardown), so the dual refcount had to become intrinsically SMP-safe: a torn count or two paths racing the free decision was a latent UAF/double-free. `burrow_free_internal` runs OUTSIDE `v->lock` (leaf discipline -- it takes the buddy / mmio / dma locks). `burrow_acquire_mapping` additionally carries the both-counts-zero resurrection guard + the per-type liveness read inside the lock (RW-1 C-F4). The per-Proc VMA-list mutators (`burrow_map` / `burrow_unmap` via `vma_insert`/`vma_remove`) serialize on the caller-held `p->vma_lock` (#713; lock order `vma_lock -> v->lock -> buddy zone->lock`).

Weft-2 (`burrow_share_into`) exercises this SMP-safety **across Procs** for the first time: because `v->lock` is per-Burrow (not per-Proc), the guest's `burrow_acquire_mapping` and netd's `burrow_unref` / `burrow_release_mapping` on the one shared ring serialize identically to two threads of one Proc. The cross-Proc lock order stays acyclic — each Proc holds its own `vma_lock` then the single shared `v->lock`. The runtime witness is the SMP gate (the unit tests exercise the refcount transitions; the multi-boot gate witnesses the concurrent race).

### `_Static_assert` on `struct Burrow` size is intentionally absent

The struct contains `enum burrow_type` whose underlying integer type is implementation-defined (typically int). To avoid an unnecessarily fragile assert that would fail under different compiler defaults, we don't pin the size. The `_Static_assert(__builtin_offsetof(struct Burrow, magic) == 0)` IS pinned because `burrow_free_internal`'s clobber (`v->magic = 0`) targets offset 0, and any post-free stale-pointer read of offset 0 needs to land on `magic` (not some other field) for the magic-check defense to detect the UAF. SLUB's own freelist next-pointer write (which on most allocators also lands at offset 0 of a freed slot) is a defense-in-depth backup, not the primary mechanism.

---

## Status

| Component | State |
|---|---|
| `burrow.h` API + `burrow.c` impl | Landed (P2-Fd) |
| SLUB cache + bootstrap | Landed (P2-Fd; burrow_init in main.c after handle_init) |
| `burrow_create_anon` (eager allocation) | Landed (P2-Fd) |
| `burrow_ref` / `burrow_unref` / `burrow_acquire_mapping` / `burrow_release_mapping` | Landed (P2-Fd; renamed at P3-Db) |
| `burrow_map(Proc*, ...)` / `burrow_unmap(Proc*, ...)` (high-level VMA-installing entry points) | Landed (P3-Db) |
| `burrow_share_into(Proc*, ...)` (cross-Proc Burrow share; the Weft per-flow dataplane ring) | **Landed (Weft-2; I-37 substrate, no EL0 ABI — the flow-keyed delivery is Weft-6)** |
| Dual-check NoUseAfterFree enforcement | Landed (P2-Fd) |
| Integration with `handle.c` (KOBJ_BURROW release/acquire) | Landed (P2-Fd) |
| In-kernel tests | 6 (P2-Fd) + 5 (P3-Db) = 11 covering refcount lifecycle + new VMA-installing entry points + overlap rollback. |
| Spec `burrow.tla` + 3 buggy configs | Landed (P2-Fb); covers `MapVmo` / `UnmapVmo` actions. P3-Db's high-level entry is a thin orchestrator over the spec-modeled refcount + `vma_insert` overlap; no spec extension needed at this sub-chunk. |
| `burrow_create_mmio(struct KObj_MMIO *)` (BURROW_TYPE_MMIO) | **Landed (P4-Ic1)** — wraps KObj_MMIO; holds a ref on the underlying kobj for the Burrow's lifetime; pages=NULL; burrow_free_internal type-dispatches between free_pages (ANON) and kobj_mmio_unref (MMIO). |
| `burrow_create_dma(struct KObj_DMA *)` (BURROW_TYPE_DMA) | **Landed (P4-Ic5b1b)** — wraps KObj_DMA; holds a ref on the underlying kobj for the Burrow's lifetime; pages=NULL (the contiguous page chunk lives on the KObj_DMA itself); burrow_free_internal type-switch adds DMA → `kobj_dma_unref`. Used by `SYS_DMA_MAP` + the IRQ-latency-bench's shared-memory mechanism. |
| Magic clobber on `burrow_free_internal` (R9 F148 discipline) | **Landed (P4-N / R13 F213)** — `v->magic = 0` immediately before `kmem_cache_free`. Sibling kobjs (kobj_mmio, kobj_dma) had this discipline since R9; burrow.c was the outlier until P4-N. |
| `BURROW_TYPE_FILE` (Stratum page cache) | Post-v1.0 |
| PTE installation (demand paging) for ANON | **Landed (P3-Dc)** — `userland_demand_page` in arch/arm64/fault.c |
| PTE installation for MMIO (device-memory PTE attrs) | **Landed (P4-Ic2)** — `userland_demand_page` dispatches on `vma->burrow->type`; `mmu_install_user_pte` accepts `bool device_memory` flag |
| `SYS_MMIO_MAP` syscall | **Landed (P4-Ic2)** — calls `burrow_create_mmio` + `burrow_map`; PTEs install lazily via demand-page |
| Partial unmap (sub-range of an existing VMA) | Post-v1.0 |
| Atomic refcount ops (SMP) | Phase 5+ |

### P4-Ic1 MMIO Burrow details

A MMIO Burrow has the same dual-count NoUseAfterFree lifecycle as ANON — that's why `burrow.tla` didn't need behavioral extension. The differences are all impl-level:

- **No `pages` field**: `pages = NULL`; backing is a fixed PA owned by the underlying `KObj_MMIO`.
- **`kobj_mmio` cross-reference**: the Burrow holds a `kobj_mmio_ref(km)` for its lifetime. Released in `burrow_free_internal` when both counts reach 0.
- **`pa` field**: device PA (page-aligned, matches `kobj_mmio->pa`). Read by the demand-page handler (lands at P4-Ic2) to construct device-memory PTEs.
- **`burrow_free_internal` type switch**: ANON → `free_pages`; MMIO → `kobj_mmio_unref` + struct kfree (no page-allocator interaction).
- **`burrow_acquire_mapping` liveness check**: ANON checks `pages != NULL`; MMIO checks `kobj_mmio != NULL`. Both indicate "backing resource alive."

Tests at P4-Ic1:
- `burrow_mmio.create_basic` — round trip
- `burrow_mmio.create_null_rejected` — NULL kobj_mmio rejected
- `burrow_mmio.create_holds_kobj_ref` — Burrow's ref keeps kobj alive after caller's unref
- `burrow_mmio.unref_releases_kobj_ref` — symmetric path
- `burrow_mmio.acquire_mapping_works` — dual-count mechanics work for MMIO
- `burrow_mmio.lifecycle_round_trip` — full create → map → unmap → unref → kobj_unref

### P4-Ic5b1b DMA Burrow details

A DMA Burrow has the same dual-count NoUseAfterFree lifecycle as ANON + MMIO — the spec's type-agnostic refcount model covers all three. Differences from MMIO:

- **`kobj_dma` cross-reference instead of `kobj_mmio`**: the Burrow holds a `kobj_dma_ref(kd)` for its lifetime; released in `burrow_free_internal`.
- **`pa` field**: buddy-chosen PA (page-aligned, matches `kobj_dma->pa`). Read by the demand-page handler to construct Normal-cacheable PTEs (vs MMIO's device-nGnRnE).
- **`burrow_free_internal` type switch**: DMA → `kobj_dma_unref` + struct kfree. The `kobj_dma_unref` cascades into `free_pages` on the buddy chunk when the last KObj_DMA ref drops.

Tests at P4-Ic5b1b (in `kernel/test/test_burrow.c`):
- `burrow_dma.create_basic` — round trip
- `burrow_dma.create_null_rejected` — NULL kobj_dma rejected
- `burrow_dma.holds_kobj_ref` — Burrow's ref keeps kobj alive after caller's unref
- `burrow_dma.lifecycle_round_trip` — full create → map → unmap → unref → kobj_unref

### P4-N — R13 audit close (BURROW spec finalize)

This chunk closes the BURROW audit-trigger surface in preparation for Phase 4 close. Adversarial self-audit against ARCH §28 I-7 found **0 P0 + 0 P1 + 1 P2 + 3 P3**, all closed in this chunk:

- **F213 (P2)**: `burrow_free_internal` lacked the `v->magic = 0` clobber that sibling kobjs (kobj_mmio, kobj_dma) established at R9 F148. Without it, a stale-pointer dereference between `kmem_cache_free` and SLUB's next-pointer write would read a valid `VMO_MAGIC` and pass `burrow_ref` / `burrow_acquire_mapping`'s magic check, masking the UAF. **Fix**: added `v->magic = 0;` immediately before `kmem_cache_free(g_vmo_cache, v);` with a comment block explaining the discipline. Inspection-verified; no new test (the regression is structural — any post-free magic-checked access on the slot extincts before SLUB has had a chance to reuse the slot).

- **F214 (P3)**: SPEC-TO-CODE.md's burrow.tla section pointed at outdated symbols (`burrow_map` / `burrow_unmap` for MapVmo/UnmapVmo when the actual increment lives in `burrow_acquire_mapping` / `burrow_release_mapping`) and didn't mention `burrow_create_mmio` (P4-Ic1) + `burrow_create_dma` (P4-Ic5b1b) as VmoCreate variants. **Fix**: refreshed the table to enumerate all three constructors + correct MapVmo/UnmapVmo callsites; refreshed buggy-config state counts (TLC2 v1.8.0 produces 66 / 54 / 43 distinct states; prior recorded 56 / 58 / 43).

- **F215 (P3)**: `specs/burrow.tla`'s commentary at lines 18-28 enumerated `BURROW_TYPE_ANON` + `BURROW_TYPE_MMIO` but not `BURROW_TYPE_DMA` (added at P4-Ic5b1b). **Fix**: extended the type list with DMA + adjusted "Both share..." to "All three share..."; cross-reference to handles.tla's HwResourceExclusive updated.

- **F216 (P3)**: `kernel/include/thylacine/burrow.h` + `docs/reference/20-burrow.md` had four "SLUB clobbers magic on free" claims that were factually wrong (SLUB's `kmem_cache_free` doesn't zero the slot's data — it writes a freelist next-pointer at some offset, which on most allocators is offset 0 but is not architecturally a magic clobber). **Fix**: corrected all four sites to attribute the magic clobber to `burrow_free_internal`'s explicit `v->magic = 0;` per F213.

**TLC verification**: all 4 burrow configs re-run at P4-N. burrow.cfg clean (100 distinct states / depth 9, matching the P2-Fb baseline). The three buggy configs each produce expected `NoUseAfterFree` violations.
