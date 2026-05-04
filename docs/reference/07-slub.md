# 07 — SLUB kernel object allocator (as-built reference)

The kernel's variable-size object allocator. SLUB-style (Linux's modern default since 2008): per-object-class slab pages with embedded freelist, per-cache partial-slab list, zero-overhead-per-free-object accounting. Layered on top of `mm/phys`'s `alloc_pages`. Provides `kmalloc(N)` / `kfree(p)` / `kmem_cache_*` for kernel objects — the foundation Phase 2's process / thread / handle / VMO / chan structures will build on.

P1-E deliverable. Standard kmalloc-{8..2048} caches plus a meta cache for `struct kmem_cache` itself; sizes above 2048 bypass slab and go directly through `alloc_pages`.

Scope: `mm/slub.{h,c}`, the extension of `struct page` with two SLUB-only fields (`slab_freelist`, `slab_cache`) plus the new `PG_SLAB` flag, the `slub_init` call in `boot_main`, and the kmem smoke test. Also see `docs/reference/06-allocator.md` (the `alloc_pages` substrate).

Reference: `ARCHITECTURE.md §6.4` (kernel object allocator design intent).

---

## Purpose

`mm/phys.c` gives us 4 KiB / 2 MiB / etc. page chunks. Phase 2+ will allocate hundreds of variable-size structures (`struct Proc`, `struct Thread`, `struct Chan`, `struct VMO`, `struct Handle`, plus dynamically-sized buffers); doing each as a 4 KiB `kpage_alloc` would waste >99% of every allocation. SLUB groups same-size allocations into shared "slab" pages so the unused capacity in each page is amortized across many objects.

Each `struct kmem_cache` owns:
- A target object size + alignment.
- A list of "partial" slab pages (slabs with at least one free object).
- Per-slab freelist of free objects, embedded in the object memory itself (zero metadata).

Allocations pop one object from a partial slab's freelist — O(1) without lock contention on the buddy zone. Frees push back. When a slab fills, it leaves the partial list (we don't track full slabs separately — they re-add to partial on first free). When a slab fully empties, its page is returned to the buddy.

For requests above 2 KiB (`SLUB_MAX_OBJECT_SIZE`), `kmalloc` bypasses slab and calls `alloc_pages` directly. The page's struct page records the order; `kfree` reads it back. This covers the ARCH §6.4-listed `kmalloc-{4096..262144}` caches without needing multi-page slabs at v1.0 — same API, different backend.

---

## Public API

`mm/slub.h`:

```c
void slub_init(void);                       // call after phys_init

struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                     size_t align, unsigned flags);
void kmem_cache_destroy(struct kmem_cache *c);
void *kmem_cache_alloc(struct kmem_cache *c, unsigned flags);
void  kmem_cache_free(struct kmem_cache *c, void *obj);

void *kmalloc(size_t n, unsigned flags);
void *kzalloc(size_t n, unsigned flags);    // == kmalloc | KP_ZERO
void *kcalloc(size_t n, size_t size, unsigned flags);   // overflow-checked
void  kfree(void *p);

u64 slub_total_alloc(void);                 // diagnostics
u64 slub_total_free(void);
u64 slub_active_slabs(void);
```

Cache flags (`flags` argument to `kmem_cache_create`):

```c
#define KMEM_CACHE_PANIC_ON_FAIL  (1u << 0)   // extinction on alloc failure
```

Allocation flags (passed in `flags` to alloc functions, shared with `mm/phys.h`):

```c
#define KP_ZERO      (1u << 0)   // zero-initialize
#define KP_DMA       (1u << 1)   // < 4 GiB PA (no-op at v1.0; single zone)
#define KP_NOWAIT    (1u << 2)   // implicit at v1.0
#define KP_COMPLETE  (1u << 3)   // unused
```

---

## Implementation

### Cache descriptor

```c
struct kmem_cache {
    const char *name;
    size_t object_size;     // requested
    size_t actual_size;     // padded for alignment + freelist link
    size_t align;
    unsigned flags;

    unsigned slab_order;            // log2(pages per slab); 0 at v1.0
    unsigned objects_per_slab;

    struct page partial_list;       // sentinel head
    u64 nr_partial;

    u64 alloc_count, free_count, slabs_active, slabs_drained;

    spin_lock_t lock;

    struct kmem_cache *next_cache;  // global iteration list
};
```

`actual_size` is the per-object stride within a slab. It's `max(requested_size, SLUB_MIN_OBJECT_SIZE = 8)` rounded up to `align`. The 8-byte minimum exists because each free object's first 8 bytes hold the freelist `next` pointer (we embed the linked list in the unused object memory).

`objects_per_slab` is `(PAGE_SIZE << slab_order) / actual_size`. At v1.0 with `slab_order = 0` this is `4096 / actual_size`:

| Cache | actual_size | objects_per_slab |
|---|---|---|
| kmalloc-8 | 8 | 512 |
| kmalloc-16 | 16 | 256 |
| kmalloc-32 | 32 | 128 |
| kmalloc-64 | 64 | 64 |
| kmalloc-128 | 128 | 32 |
| kmalloc-256 | 256 | 16 |
| kmalloc-512 | 512 | 8 |
| kmalloc-1024 | 1024 | 4 |
| kmalloc-2048 | 2048 | 2 |

### Slab page layout

A slab is a single 4 KiB page from the buddy (compound pages with `slab_order > 0` are post-v1.0). The page's struct page carries:

- `flags |= PG_SLAB`.
- `slab_cache` — pointer back to the owning `struct kmem_cache`.
- `slab_freelist` — pointer to the first free object in the page.
- `refcount` — repurposed as `inuse` (count of allocated objects).

Within the page, objects occupy slots `0..objects_per_slab-1`, each `actual_size` bytes. **Free objects** carry a `void *` to the next free object in their first 8 bytes (the freelist threads through them); **allocated objects** are caller-owned data. There's no per-slot bitmap or header — the freelist is the only state, and it's stored inside the free objects themselves.

Initial layout (fresh slab from `slab_init_freelist`):

```
slab page (4 KiB):
+--------+--------+--------+ ... +--------+
| obj 0  | obj 1  | obj 2  |     | obj N-1|
| -> 1   | -> 2   | -> 3   |     | NULL   |
+--------+--------+--------+ ... +--------+
   ^
   slab->slab_freelist
```

Each `obj k`'s first 8 bytes hold `&obj (k+1)`; the last object's first 8 bytes are NULL (end of list). `slab->slab_freelist` points at `obj 0`.

After `kmem_cache_alloc`: `slab_freelist` advances to obj 1 (the popped obj 0 is returned to caller); `inuse` increments.

After `kmem_cache_free(obj 5)`: obj 5's first 8 bytes get the current `slab_freelist`, then `slab_freelist` becomes obj 5; `inuse` decrements.

### Allocation paths

**`kmem_cache_alloc(c, flags)`**:

1. If `c->partial_list` is non-empty: take the head slab.
2. Else: allocate a new slab page via `alloc_pages(c->slab_order, 0)`, run `slab_init_freelist`, push to partial.
3. Pop one object from `slab->slab_freelist`. Increment `inuse`. Increment `c->alloc_count`.
4. If `inuse == objects_per_slab` (slab now full): `list_remove(slab)` from partial. (We don't track a separate "full" list — `kmem_cache_free` re-inserts when transitioning back to has-free.)
5. If `flags & KP_ZERO`: zero-fill the object.
6. Return the object pointer.

**`kmem_cache_free(c, obj)`**:

1. `slab = pa_to_page((paddr_t)obj)` — for single-page slabs, the page-aligned PA of `obj` IS the slab page.
2. Sanity: assert `slab->flags & PG_SLAB` and `slab->slab_cache == c`. Mismatch → `extinction`.
3. Note `was_full = (slab->refcount == c->objects_per_slab)`.
4. Push `obj` onto `slab->slab_freelist` (write `*(void**)obj = old_head; new_head = obj`). Decrement `inuse`. Increment `c->free_count`.
5. If `was_full`: re-insert into partial list (slab transitioned full → has-free).
6. Else if `inuse == 0` (slab now empty): remove from partial, drain via `slab_drain` → `free_pages`.

**`kmalloc(n, flags)`**:

- `n == 0` → NULL.
- `n <= SLUB_MAX_OBJECT_SIZE`: `idx = max(ceil_log2(n), 3)`; allocate from `g_kmalloc_caches[idx]`.
- `n > SLUB_MAX_OBJECT_SIZE`: compute `order = ceil_log2(ceil(n / PAGE_SIZE))`; `alloc_pages(order, flags)`. Return `(void*)page_to_pa(p)`.

**`kfree(p)`**:

- `p == NULL` → no-op.
- `page = pa_to_page((paddr_t)p)`.
- If `page->flags & PG_SLAB`: `kmem_cache_free(page->slab_cache, p)`.
- Else: `free_pages(page, page->order)` — large allocation path.

### Bootstrap

`slub_init` runs after `phys_init`. The chicken-and-egg problem (creating a cache for `struct kmem_cache` itself) is resolved by a static `g_meta_cache` initialized in place:

```c
static struct kmem_cache g_meta_cache;
static struct kmem_cache g_kmalloc_caches[KMALLOC_NUM_CACHES];

void slub_init(void) {
    g_cache_list_head = NULL;
    init_cache(&g_meta_cache, "kmem_cache",
               sizeof(struct kmem_cache),
               _Alignof(struct kmem_cache), 0);
    for (int i = KMALLOC_MIN_IDX; i <= KMALLOC_MAX_IDX; i++) {
        size_t size = (size_t)1 << i;
        init_cache(&g_kmalloc_caches[i], KMALLOC_NAMES[i],
                   size, SLUB_MIN_ALIGN, 0);
    }
}
```

The static descriptors live in BSS; no allocation is needed for them. `kmem_cache_create` for dynamic caches calls `kmem_cache_alloc(&g_meta_cache, KP_ZERO)` to allocate a fresh `struct kmem_cache`, then runs `init_cache` on it.

---

## Data structures

### Updated `struct page`

P1-E grows `struct page` from 32 to 48 bytes:

```c
struct page {
    struct page *next, *prev;       // free list / partial list / full list
    u32 order;                      // buddy order or slab order
    u32 flags;                      // PG_FREE / PG_RESERVED / PG_KERNEL / PG_SLAB
    u32 refcount;                   // VMO refcount; slab: inuse count
    u32 _pad;
    void *slab_freelist;            // SLUB: head of free objects (NULL if not slab)
    struct kmem_cache *slab_cache;  // SLUB: cache backref (NULL if not slab)
};
```

For 2 GiB RAM (524288 pages): struct page array = 24 MiB (was 16 MiB at P1-D). Banner shows the bump as ~26 MiB total reserved (kernel ~88 KiB + struct pages 24 MiB + DTB blob ~1 MiB + low firmware 512 KiB).

The slab fields are valid only when `flags & PG_SLAB` is set; for free / generic-kernel pages they're zero. We accept the 50% array-size growth from a strict-union approach because the union route comes with "is this field meaningful?" footguns at every read site. 8 MiB of BSS at v1.0 is a fair price for clarity.

### `g_meta_cache` and `g_kmalloc_caches[]`

Static BSS. `g_meta_cache` carries `struct kmem_cache` itself (so `kmem_cache_create` can allocate descriptors). `g_kmalloc_caches[3..11]` carry the standard power-of-two caches.

### Global cache list

`g_cache_list_head` plus `kmem_cache.next_cache` thread every cache (static + dynamic) onto a linked list for diagnostic iteration. `slub_total_alloc` etc. walk this list. Phase 2's `/ctl/mem` will surface per-cache stats via the same list.

---

## Spec cross-reference

No formal spec at P1-E. A future `slub.tla` could prove:

- The freelist invariant: the linked chain through free-object first-bytes terminates and visits exactly `objects_per_slab - inuse` objects.
- Partial-list invariant: a slab is on `partial_list` iff `0 < inuse < objects_per_slab`.
- No double-free: `kmem_cache_free` on an already-free object corrupts the freelist (currently undetected — debug mode would add a poison sentinel).

These are post-v1.0 unless a real bug surfaces. The P1-I sanitizer matrix (ASan + UBSan) plus the formal 10000-iteration leak check exercise the allocator under stress.

---

## Tests

P1-E integration test: `tools/test.sh` boots and verifies the boot banner. The kmem smoke test in `boot_main` does:

- 1500 `kmalloc(8)` / `kfree` round-trips (forces 3 slab pages to be allocated and drained — `1500 / 512 = 2.93` slabs).
- Mixed-size kmalloc: 16, 64, 128, 512, 2048 bytes (each cache exercised once).
- 8 KiB `kzalloc` / `kfree` (bypasses slab, direct `alloc_pages` path).
- Custom typed cache: `kmem_cache_create("smoke-typed", 100, 16, 0)`, two allocs, two frees, destroy.

After draining magazines, asserts `phys_free_pages() == baseline`. Five consecutive boots produce five PASS results.

Future tests (P1-I+):

- 10000-iteration alloc/free leak check (ROADMAP §4.2 exit criterion).
- ASan-instrumented build catches use-after-free / double-free / out-of-bounds.
- UBSan-instrumented build catches integer overflow + alignment violations.
- TSan-instrumented build (Phase 2 SMP) catches partial-list races.

---

## Error paths

| Condition | Behavior |
|---|---|
| `kmalloc(0)` / `kfree(NULL)` / `kcalloc(0, ...)` | Returns NULL / no-op. |
| `kmalloc(n)` for `n > 4 GiB` | `ceil_log2` overflows; alloc_pages returns NULL. |
| `kmem_cache_create(size > 2048)` | Returns NULL (multi-page slabs are post-v1.0). |
| `kmem_cache_alloc` OOM | Returns NULL (or `extinction` if `KMEM_CACHE_PANIC_ON_FAIL`). |
| `kmem_cache_free` on object not from this cache | `extinction("kmem_cache_free: object not from this cache")`. |
| `kfree` on object whose page doesn't have `PG_SLAB` AND `page->order` is invalid | UB (no validation). Phase 2 audit will tighten this. |
| `kmem_cache_destroy` with live objects | Caller's fault; current implementation drains the partial list and silently leaks anything not on it. |

---

## Performance characteristics

P1-E measurements on QEMU virt under Hypervisor.framework (estimates; rigorous at P1-I):

| Metric | Estimated | Notes |
|---|---|---|
| `kmalloc(8)` cache-hit | ~10ns | `pa_to_page` via PFN math + freelist pop + counter inc. |
| `kmem_cache_alloc` cache-hit | ~10ns | Same path; named caches just skip the kmalloc dispatch. |
| `kmem_cache_alloc` slab-empty | ~50-100ns | Drops to `alloc_pages` for a fresh slab; one buddy-magazine refill if order 0 ∈ {magazine orders} (it is). |
| `kfree` slab-resident | ~10ns | Freelist push + counter dec + (optional) drain. |
| `kfree` slab-drain | ~100-200ns | Returns slab to buddy via `free_pages` → magazine push (most cases). |
| Kernel ELF size (debug) | ~155 KB | +18 KB from P1-D. |
| Kernel flat binary | ~16 KB | Unchanged (slab code is small). |
| Page tables | 40 KiB BSS | Unchanged from P1-C-extras. |
| struct page array | 24 MiB BSS | +8 MiB from P1-D (struct page grew 32→48 bytes). |

Latency numbers are estimates — formal benchmarks at P1-I.

---

## Status

**Implemented at P1-E**:

- `struct page` extended with `slab_freelist` + `slab_cache` (16 bytes added; struct now 48 bytes).
- `PG_SLAB` flag.
- `mm/slub.{h,c}` (~370 LOC). Per-cache partial list + per-slab embedded freelist.
- `kmem_cache_create` / `kmem_cache_destroy` / `kmem_cache_alloc` / `kmem_cache_free`.
- `kmalloc` / `kzalloc` / `kcalloc` / `kfree`. Sizes ≤ 2048 bytes go through slab; larger requests bypass to `alloc_pages` directly.
- Bootstrap: static `g_meta_cache` for `struct kmem_cache` itself; static `g_kmalloc_caches[3..11]` for kmalloc-{8..2048}; dynamic caches via `kmem_cache_create`.
- Boot smoke test exercising small-cache (1500 × kmalloc-8), mixed sizes, large (8 KiB direct path), and dynamic cache create/use/destroy.
- Banner adds `kmem smoke: PASS / FAIL` line.
- Diagnostic accessors `slub_total_alloc` / `slub_total_free` / `slub_active_slabs` for future `/ctl/mem`.

**Not yet implemented**:

- Multi-page slabs (`slab_order > 0`) for objects > 2048 bytes within `kmem_cache_create`. Currently rejected with NULL return; large kmalloc still works via `alloc_pages` direct path.
- Per-CPU active-slab fast path (NCPUS=1 at P1-E; meaningful only when SMP arrives at P1-F).
- Debug mode (red zones, poison patterns, allocation/free trace). Boot-cmdline opt-in lands at P1-I.
- `slab_caches` enumeration via `/ctl/mem` (Phase 2 — needs the namespace + Dev infrastructure).
- 10000-iteration leak check + sanitizer matrix (P1-I).
- Free-object cookie / double-free detection (P1-I or post-v1.0).

**Landed**: P1-E at commit `(pending hash-fixup)`.

---

## Caveats

### Single-page slabs only at v1.0

`slab_order = 0` is hardcoded. For object sizes ≤ 2048 bytes, single-page slabs hold ≥ 2 objects per page (kmalloc-2048: 2 objects/page). For sizes ≤ 1024 bytes, ≥ 4 objects/page. For typical kernel objects (Phase 2's `struct Proc`, `struct Thread`, `struct Chan`, `struct VMO`, `struct Handle` are all < 512 bytes), we get ≥ 8 objects/page — comfortable.

If a Phase 2 caller creates a cache for objects > 2 KiB, `kmem_cache_create` returns NULL. The future bump to multi-page slabs (compound pages) is mechanical — primarily updating `slab_init_freelist` to thread across multiple pages and adjusting `pa_to_page` to derive the slab head from any page in the compound. Linux's slub_setup uses `__GFP_COMP` and the head/tail page distinction; we'd do the same.

### Object alignment defaults to 8 bytes

`SLUB_MIN_ALIGN = 8`. Caches with stronger alignment requirements (e.g., DMA buffers needing cache-line alignment) pass `align` to `kmem_cache_create`. The kmalloc-* caches don't care — they're general-purpose.

### `kmem_cache_destroy` doesn't audit live objects

If a caller destroys a cache while objects are still allocated, the partial-list drain releases empty slabs but full / partial slabs containing live objects get **leaked silently**. Debug mode (P1-I) will add an assertion. For now: caller is responsible for freeing every allocation before destroying the cache.

### No double-free detection

`kmem_cache_free` checks `page->flags & PG_SLAB` and `page->slab_cache == c` but not whether the specific object is already on the freelist. A double-free corrupts the freelist (the object's first 8 bytes get rewritten to a stale freelist head, splitting the freelist or creating cycles).

Phase 1-I will add a per-object cookie or a debug-mode bitmap. Until then, the audit policy is "match every kmalloc with exactly one kfree." `extinction` is the safety net for everything else.

### Static `boot_main.smalls` BSS array

The SLUB smoke test in `kernel/main.c` declares its 1500-entry pointer array as `static` so the 12 KiB doesn't crowd the 16 KiB boot stack. Future test infrastructure (P1-I) will move smoke tests out of `boot_main`; in the meantime, the static is the simplest fix and `llvm-readelf` shows `boot_main.smalls` as a 12000-byte BSS object.

### Stack guard page interaction

The kmem smoke test executes with the boot stack guard page (P1-C-extras Part A) live. A bug that overflowed the boot stack would now fault into the guard page rather than corrupt prior BSS — a real win compared to the silent-corruption mode of pre-P1-C-extras kernels. P1-F's exception handler will route the fault to `extinction("kernel stack overflow", FAR_EL1)`.

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — slub_init slot in the boot sequence.
- `docs/reference/06-allocator.md` — the `alloc_pages` substrate that SLUB sits on.
- `docs/ARCHITECTURE.md §6.4` — SLUB design intent.
- Bonwick, "The Slab Allocator: An Object-Caching Kernel Memory Allocator" (USENIX 1994) — the original slab paper.
- Lameter, "SLUB: The unqueued slab allocator" (LWN 2007) — the SLUB simplification of slab.
- Linux `mm/slub.c` — reference SLUB implementation.
