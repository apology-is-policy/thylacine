# 06 — Physical allocator (as-built reference)

The kernel's physical-frame allocator. Buddy allocator (Knuth 1973) for the canonical free-list machinery, with per-CPU magazines (Bonwick & Adams 2001, illumos kmem) layered on top for hot-path lock-freedom on common orders. Single zone at v1.0 (single NUMA node); the API takes a zone explicitly so multi-NUMA at v2.x is mechanical.

P1-D deliverable. Unblocks every Phase 2+ subsystem that needs page memory: per-process VMs, VMOs, kernel object slabs (P1-E SLUB), per-thread stacks, page tables for new ASIDs.

Scope: `kernel/include/thylacine/page.h` (struct page + flags + PFN/page conversion), `kernel/include/thylacine/spinlock.h` (lock stub at P1-D), `mm/buddy.{h,c}` (buddy zone + free lists + split/merge), `mm/magazines.{h,c}` (per-CPU stacks + refill/drain), `mm/phys.{h,c}` (DTB-driven bootstrap + public alloc API + diagnostic accessors). Also see `docs/reference/01-boot.md` (boot integration), `docs/reference/02-dtb.md` (DTB consumer), `docs/reference/03-mmu.md` (TTBR0 identity covers PAs returned by kpage_alloc), `docs/reference/05-kaslr.md` (kernel-image PA range cached during kaslr_init for phys_init's reservation math).

Reference: `ARCHITECTURE.md §6.1`-`§6.3` (memory management goals, ARM64 layout, buddy + magazines design), `§28` (invariants — refcount lifetime is the future I-7 surface).

---

## Purpose

Bring the kernel from "MMU on, but only the fixed BSS-allocated page tables and boot stack are usable as memory" to "any kernel subsystem can request 4 KiB / 2 MiB / N-order page blocks and free them again." The physical allocator owns every byte of RAM that isn't part of the kernel image, the struct-page array itself, the DTB blob, or the low-firmware reservation. Future allocators (SLUB, vmalloc, COW page faults, Phase 2 process page tables) all draw from this pool.

The hot path on the most common orders (0 = 4 KiB and 9 = 2 MiB) is a per-CPU stack pop / push — no buddy-zone lock contention. The buddy lock is acquired only on magazine refill / drain (batched: each acquisition covers 8 pages amortized) plus rare large allocations (orders > 9 bypass magazines).

W^X (I-12) and KASLR (I-16) remain enforced under both translation roots — the allocator returns physical pages, doesn't touch page tables. The future direct-map promotion (Phase 2 retiring TTBR0) will change `kpage_alloc`'s return type from "PA cast to void*" to "high-VA pointer", but the API stays stable.

---

## Public API

`kernel/include/thylacine/page.h`:

```c
#define PAGE_SHIFT  12
#define PAGE_SIZE   (1ull << PAGE_SHIFT)
#define MAX_ORDER   18                  // 1 GiB (2^18 * 4 KiB)

struct page {
    struct page *next, *prev;           // free list (16 bytes)
    u32 order;                          // current order if PG_FREE
    u32 flags;                          // PG_*
    u32 refcount;                       // VMO placeholder (Phase 2-3)
    u32 _pad;
};

#define PG_FREE       (1u << 0)
#define PG_RESERVED   (1u << 1)
#define PG_KERNEL     (1u << 2)

#define KP_ZERO       (1u << 0)         // zero on alloc
#define KP_DMA        (1u << 1)         // < 4 GiB PA (no-op at v1.0; single zone)
#define KP_NOWAIT     (1u << 2)         // implicit at v1.0 (no scheduler)
#define KP_COMPLETE   (1u << 3)         // unused at v1.0

static inline struct page *pfn_to_page(paddr_t pfn);
static inline paddr_t      page_to_pfn(const struct page *p);
static inline paddr_t      page_to_pa(const struct page *p);
static inline struct page *pa_to_page(paddr_t pa);
```

`mm/phys.h`:

```c
bool phys_init(void);                   // DTB-driven bootstrap

u64 phys_total_pages(void);             // banner diagnostic
u64 phys_free_pages(void);
u64 phys_reserved_pages(void);

struct page *alloc_pages(unsigned order, unsigned flags);
void         free_pages(struct page *p, unsigned order);
struct page *alloc_pages_node(int node, unsigned order, unsigned flags);

void *kpage_alloc(unsigned flags);      // single page; PA-as-void* at P1-D
void  kpage_free(void *p);
```

`mm/buddy.h` and `mm/magazines.h` are internal to the allocator; phys.c is the public surface for kernel callers.

---

## Implementation

### Layout discovered at boot

`phys_init` reads `dtb_get_memory(&base, &size)` to find the single RAM range. For QEMU virt with 2 GiB it's `[0x40000000, 0xC0000000)`. Three reservation sub-ranges are then computed:

| Reservation | Where | Why |
|---|---|---|
| Kernel image | `[kaslr_kernel_pa_start(), kaslr_kernel_pa_end())` | Code, rodata, data, BSS, page tables, boot stack — all pinned PAs the kernel itself uses. |
| struct page array | Just past kernel image, page-aligned, size = `num_pages × sizeof(struct page)` | One descriptor per managed frame. For 2 GiB / 4 KiB granule = 524288 entries × 32 bytes = 16 MiB. |
| DTB blob | `round_down(_saved_dtb_ptr, PAGE_SIZE)` to `round_up(_saved_dtb_ptr + dtb_total_size, PAGE_SIZE)` | The DTB is referenced by `dtb_init` for late lookups (e.g., bringing up GIC at P1-F); we keep it pinned through Phase 1. |

Plus an implicit `[zone_base, kern_pa_start)` low-firmware region that's left RESERVED — we don't know what (if anything) the bootloader stashed there, and on QEMU virt it's tiny (512 KiB before the kernel at `0x40080000`).

The three explicit reservations are sorted by start address; phys_init walks the gaps between them and calls `buddy_free_region` for each free range. On QEMU virt with 2 GiB RAM, the typical layout is:

```
0x40000000  zone_base
            [low firmware, 512 KiB — RESERVED implicitly]
0x40080000  kernel image      [~88 KiB — RESERVED]
0x40096000  struct page array [16 MiB — RESERVED]
0x41096000  free                              ┐
                                              │ ~111 MiB free
0x48000000  DTB blob          [~1 MiB — RESERVED]
0x48100000  free                              ┐
                                              │ ~1.94 GiB free
0xC0000000  zone_end
```

The kernel reserves ≈18 MiB; the rest (≈2030 MiB) ends up on the buddy free lists.

### Kernel-image PA range — cached, not recomputed

`phys_init` runs from `boot_main` AFTER the long-branch into TTBR1, so PIC mode's `(uintptr_t)_kernel_start` evaluates to the high VA, not the load PA. To get the load PA we read accessors `kaslr_kernel_pa_start()` / `kaslr_kernel_pa_end()` — these are populated by `kaslr_init` while still running at PA (PC = load PA, PC-relative gives PA).

The cache variables are declared `volatile` because clang at `-O2` with `-fpie -mcmodel=tiny` otherwise folds the assignment as if `_kernel_start` were a link-time constant (rewriting the storage as a 1-byte boolean and gating the accessor on it). `volatile` forces real 8-byte memory traffic and preserves the runtime-computed PA. See `arch/arm64/kaslr.c:45` for the explanatory comment.

### Buddy allocator

`mm/buddy.c`. One zone, sentinel-headed doubly-linked free list per order:

```c
struct buddy_zone {
    paddr_t base_pa, end_pa;
    u64 num_pages;
    struct page free_lists[MAX_ORDER + 1];  // sentinel head per order
    u64 free_pages_per_order[MAX_ORDER + 1];
    u64 total_free_pages;
    spin_lock_t lock;
};
```

**Buddy math.** For a block at PFN `p` of order `k`, its buddy lives at PFN `p ^ (1 << k)`. The "left" buddy (the one whose PFN has bit `k` clear) is the merge anchor.

**Bootstrap (`buddy_free_region`).** Greedy: for each PFN in the range, the largest valid order is `min(alignment_of(pfn), floor_log2(remaining_size), MAX_ORDER)`. Free at that order; advance. The result is at most `O(log range)` chunks per region.

```c
while (pfn < end_pfn) {
    int max_align = pfn_max_align(pfn);          // ctz, capped at MAX_ORDER
    int max_size  = floor_log2_u64(end_pfn - pfn);
    int order     = MIN(max_align, max_size, MAX_ORDER);
    zone_free_chunk(zone, pfn, order);
    pfn += 1ull << order;
}
```

**Allocate (`buddy_alloc`).** Find the smallest non-empty `free_lists[k]` with `k >= order`. Pop one page; while `k > order`, halve and put the right buddy back on `free_lists[k-1]`. The remaining left half is returned with `PG_KERNEL` set and `refcount = 1`.

**Free (`buddy_free`).** Iteratively merge with the buddy at the same order while the buddy is `PG_FREE`. Each merge removes the buddy from its free list, anchors on the lower-PFN buddy, advances order. When merging stops, push the (possibly-merged) page onto the appropriate free list.

### Per-CPU magazines

`mm/magazines.c`. Each CPU has a `struct percpu_data` with two `struct magazine` slots — one for order 0 (4 KiB), one for order 9 (2 MiB). Each magazine is a stack of 16 entries.

**Refill** (called when count == 0): `mag_refill` calls `buddy_alloc` repeatedly until count reaches `MAGAZINE_SIZE / 2` = 8. Partial refill on OOM is OK — the alloc path will see count == 0 still and return NULL.

**Drain** (called when count == MAGAZINE_SIZE): `mag_drain` calls `buddy_free` repeatedly until count reaches `MAGAZINE_SIZE / 2` = 8. The half-full hysteresis avoids thrashing when alloc-free pairs straddle the threshold.

**Drain all** (`magazines_drain_all`): empties every magazine to the buddy. Used by the boot smoke test for clean accounting; future uses include memory-pressure responses (Phase 2+).

**Order routing.** `order_to_mag_idx(order)` returns 0 for order==0, 1 for order==9, -1 otherwise. `alloc_pages(order, flags)` first tries `mag_alloc(order)`; on -1 or NULL, falls through to `buddy_alloc`. `free_pages(p, order)` mirrors.

### Boot smoke test

`boot_main` runs a sanity check post-`phys_init`:

```c
u64 baseline = phys_free_pages();

for (int i = 0; i < 256; i++) pages[i] = alloc_pages(0, KP_ZERO);
for (int i = 0; i < 256; i++) free_pages(pages[i], 0);

struct page *big2  = alloc_pages(9, KP_ZERO);   // 2 MiB
free_pages(big2, 9);

struct page *big10 = alloc_pages(10, 0);        // 4 MiB (bypasses magazines)
free_pages(big10, 10);

magazines_drain_all();                          // clean accounting
ASSERT_OR_DIE(phys_free_pages() == baseline, "phys_init smoke test failed");
```

Exercises the magazine fast path (orders 0 and 9), magazine refill/drain (256 × order 0 forces multiple refills), AND a non-magazine order (order 10 hits `buddy_alloc` directly + tests the split/merge of larger blocks). Pre-merge banner shows `alloc smoke: PASS (256 x 4 KiB + 2 MiB + 4 MiB alloc+free; free count restored)`.

This is a sanity check, not a real test harness — that lands at P1-I with ASan + UBSan.

---

## Data structures

### `struct page` (32 bytes)

| Field | Bytes | Purpose |
|---|---|---|
| `next`, `prev` | 16 | Doubly-linked free list (sentinel-headed). NULL when not on a list. |
| `order` | 4 | Current order if `PG_FREE`; 0 if `PG_KERNEL`. |
| `flags` | 4 | Bitwise OR of `PG_FREE` / `PG_RESERVED` / `PG_KERNEL`. |
| `refcount` | 4 | Placeholder for VMO refcounting (Phase 2-3). 1 when allocated, 0 when free. |
| `_pad` | 4 | Alignment slack to 32 bytes. |

For 2 GiB RAM at 4 KiB granule: 524288 pages × 32 bytes = 16 MiB struct page array.

### `struct buddy_zone`

See [Implementation](#implementation). One per NUMA node; v1.0 has `g_zone0`.

### `struct percpu_data`

Per-CPU magazine slots. `g_percpu[NCPUS]`. NCPUS=1 at P1-D; bumps at P1-F.

---

## Spec cross-reference

No formal spec at P1-D. A future `phys.tla` could prove:

- The buddy invariant: free-list contents + reserved pages + allocated pages partition the zone exactly.
- Merge correctness: the buddy at the same order being PG_FREE is a sufficient condition.
- Magazine consistency under SMP (when P1-F brings real concurrency).

These are post-v1.0 unless a real bug surfaces. The P1-I audit pass at Phase 1 exit will exercise the allocator under sanitizers (ASan + UBSan); a 10000-iteration alloc/free leak check is part of the exit criteria per ROADMAP §4.2.

---

## Tests

P1-D integration test: `tools/test.sh` boots and verifies the boot banner. The smoke test in `boot_main` runs 256 × order-0 plus one order-9 plus one order-10 alloc/free pair and gates on `phys_free_pages() == baseline` after `magazines_drain_all`. Runs every boot.

Future tests (P1-I+):

- 10000-iteration alloc/free leak check (per ROADMAP §4.2 exit criterion).
- ASan-instrumented build catches use-after-free / double-free.
- UBSan-instrumented build catches integer overflow on size paths.
- TSan-instrumented build (Phase 2 SMP) catches races on the buddy free list.

---

## Error paths

| Condition | Behavior |
|---|---|
| DTB missing or `/memory` absent | `phys_init` calls `extinction("phys_init: DTB has no /memory node")`. |
| Reservation outside DTB-discovered RAM | `phys_init` calls `extinction("phys_init: reservation outside DTB-discovered RAM")`. |
| `alloc_pages` for order > MAX_ORDER (=18) | Returns NULL. |
| Out of memory at requested order | `alloc_pages` returns NULL. Caller decides recovery (panic or retry at smaller order). |
| `free_pages(NULL, ...)` | No-op. |
| Smoke test fails | `extinction("phys_init smoke test failed")`. |

The allocator does not call `extinction` on OOM — that's the caller's policy decision (some kernel sites can fall back to smaller orders or wait for memory pressure to abate; others can't).

---

## Performance characteristics

P1-D measurements on QEMU virt under Hypervisor.framework:

| Metric | Measured | Notes |
|---|---|---|
| `phys_init` total cost | ~few ms (informal) | Dominated by struct-page-array zero-fill (16 MiB writes) + one-time `buddy_free_region` chunking. |
| `alloc_pages(0)` magazine-hit | ~10ns (estimate) | Stack pop + 4 stores + return. Measured at P1-I. |
| `alloc_pages(0)` magazine-miss | ~50-100ns (estimate) | One refill of 8 from buddy, one pop. |
| `alloc_pages(10)` buddy-direct | ~100ns (estimate) | One free-list traversal + ~8 splits worst case. |
| Kernel ELF size (debug) | ~140 KB | +23 KB from P1-C-extras (allocator + struct page-array tracking). |
| Kernel flat binary | 16 KB | Unchanged (allocator code is small). |
| Page tables | 40 KiB BSS | Unchanged from P1-C-extras. |
| struct page array | 16 MiB BSS | Sized for 2 GiB RAM. Scales linearly. |

Latency numbers are estimates — rigorous measurement at P1-I.

---

## Status

**Implemented at P1-D**:

- `struct page` (32 bytes) + per-page flag set (`PG_FREE`/`PG_RESERVED`/`PG_KERNEL`).
- Allocation flag set (`KP_ZERO` / `KP_DMA` / `KP_NOWAIT` / `KP_COMPLETE`); `KP_ZERO` actually used; others are no-ops at v1.0.
- Buddy allocator (`mm/buddy.{h,c}`, ~280 LOC) — orders 0..18, doubly-linked free lists, split-on-alloc, merge-on-free.
- Per-CPU magazines (`mm/magazines.{h,c}`, ~110 LOC) — orders 0 and 9, refill/drain to half-full, drain-all.
- Bootstrap + public API (`mm/phys.{h,c}`, ~170 LOC) — DTB-driven layout, three-reservation sort, free-region chunking, banner accessors.
- Spin-lock stub (`kernel/include/thylacine/spinlock.h`) — interface fixed; P1-F installs real LL/SC implementations.
- KASLR PA-cache accessors (`arch/arm64/kaslr.h`/`.c`) — `kaslr_kernel_pa_start` / `kaslr_kernel_pa_end`, declared `volatile` to defeat the clang fold. Documented in `kaslr.c`.
- Boot banner: `ram: X MiB total, Y MiB free, Z KiB reserved` + `alloc smoke: PASS / FAIL`.
- Boot smoke test exercises magazine fast path, refill/drain, and order-10 buddy-direct.

**Not yet implemented**:

- Watermarks / kswapd-style reclaim (no async kernel threads at v1.0; trivial OOM only).
- NUMA multi-zone (single zone at v1.0; designed-not-implemented for v2.x).
- DMA zone separation (single zone at v1.0; KP_DMA is a no-op).
- Direct map (Phase 2; will promote `kpage_alloc` return from PA-as-void* to high-VA pointer).
- SLUB kernel object allocator (P1-E — layered on alloc_pages).
- Real spin lock (P1-F — LL/SC on first SMP entry; LSE at P1-H where supported).
- 10000-iteration leak check + sanitizer matrix (P1-I).

**Landed**: P1-D at commit `198c48c`.

---

## Caveats

### `volatile` on `g_kernel_pa_*`

`arch/arm64/kaslr.c` declares `g_kernel_pa_start` and `g_kernel_pa_end` `volatile`. This is load-bearing under `-O2 -fpie -mcmodel=tiny`: without it, clang folds the assignment-then-read pattern into "store boolean 1, return link-time address gated on it". At runtime under PIE the link-time address ≠ load PA, so `phys_init` would receive bogus values. The volatile forces 8-byte memory traffic and the runtime-computed PA survives the round-trip.

This is a fragile clang-specific behavior and the closest thing P1-D has to a "compiler trip hazard". A future bump to LTO or a different optimizer might surface similar folds elsewhere — keep the diagnostic prints from the debugging round (P1-D commit body) as a recipe for narrowing the next instance.

### Magazine residency

The first allocation of an order-N page (N ∈ {0, 9}) triggers a magazine refill of 8 pages from the buddy. Even if the test allocates and frees a single page, the magazine retains 7 net pages. `phys_free_pages()` therefore drops by 7 even though logically nothing is "leaked".

The smoke test calls `magazines_drain_all` before comparing free counts so the accounting is exact. Production code that needs precise free-count accounting (e.g., a future memory-pressure handler) should similarly drain.

### Single zone, single CPU

P1-D has one zone (single NUMA node) and one CPU. The interfaces (zone parameter, percpu array) are fixed for SMP / NUMA but the implementations are degenerate. P1-F brings real SMP and the lock; v2.x adds multi-NUMA zones. The code paths exist; the data structures are single-element.

### `kpage_alloc` returns a PA-as-void*

At P1-D, `kpage_alloc(flags)` returns `(void *)(uintptr_t)page_to_pa(p)` — a low VA equal to the load PA, dereferenceable through TTBR0's identity map. Phase 2 will retire TTBR0 from kernel use and promote `kpage_alloc` to return a high-VA direct-map pointer. The API stays the same; callers that treat the return as an opaque memory address don't need to change. Callers that compute PAs from the pointer (e.g., for DMA setup) will need to be reviewed when Phase 2 lands.

### Reserved pages have `PG_RESERVED` but aren't on free lists

`buddy_zone_init` marks every struct page `PG_RESERVED` initially; `buddy_free_region` flips the to-be-free pages to `PG_FREE`. After init, reserved pages can be queried via the `flags` field but never appear on free lists or get returned from `alloc_pages`. They stay reserved for the kernel image / struct page array / DTB blob lifetime.

Phase 2's process page-table allocation and Phase 3's userspace VMO management will re-use the same `struct page` pool but track ownership via separate mechanisms (per-VMO refcount, per-process page-table owner, etc.) — the buddy doesn't see those.

---

## See also

- `docs/reference/00-overview.md` — system-wide layer cake.
- `docs/reference/01-boot.md` — phys_init slot in the boot sequence.
- `docs/reference/02-dtb.md` — `dtb_get_memory` / `dtb_get_total_size` consumers.
- `docs/reference/03-mmu.md` — TTBR0 identity covers PAs returned by `kpage_alloc`.
- `docs/reference/05-kaslr.md` — kernel-image PA range cached during `kaslr_init`.
- `docs/ARCHITECTURE.md §6.1`-`§6.3` — design intent.
- Knuth, *The Art of Computer Programming Vol. 1* §2.5 (1973) — buddy method.
- Bonwick & Adams, "Magazines and Vmem" (USENIX 2001) — the magazine layer in illumos kmem.
- Linux `mm/page_alloc.c` — reference buddy implementation.
