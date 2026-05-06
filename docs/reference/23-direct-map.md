# Reference: Kernel direct map (P3-Bb)

## Purpose

The kernel direct map is a linear PA→KVA mapping that lives in TTBR1's high half at base `KERNEL_DIRECT_MAP_BASE` (0xFFFF_0000_0000_0000). It provides constant-offset arithmetic for converting between physical and kernel-virtual addresses. The kernel allocator (SLUB, buddy, kpage_alloc) returns pointers INTO the direct map; callers dereference those pointers without depending on TTBR0's contents.

P3-Bb is the foundational refactor for per-Proc TTBR0 (P3-Bc/Bd): once kernel data access no longer relies on TTBR0's identity-map of low PAs, replacing TTBR0 with a per-Proc page-table root becomes safe.

Per ARCH §6.2 (memory layout) and §6.10 (capability-addressing v2.x direction). The direct map is the v1.0 pragmatic compromise; v2.x explores capability-based kernel addressing as the principled SOTA (NOVEL.md §3.9 Contract D).

## Public API

### `<thylacine/page.h>`

```c
#define KERNEL_DIRECT_MAP_BASE  0xFFFF000000000000ull

static inline void   *pa_to_kva(paddr_t pa);
static inline paddr_t kva_to_pa(const void *kva);
```

`pa_to_kva(pa)` returns a kernel-virtual address that maps to `pa`. Implemented as `(void *)(pa | KERNEL_DIRECT_MAP_BASE)`. `pa` must fit in bits 47:0 (≤ 1 TiB at v1.0 per ARCH §6.2 IPS=40-bit; the direct map covers up to 8 GiB at v1.0).

`kva_to_pa(kva)` is the inverse: clears bits 63:48 of the KVA. The function name "kva_to_pa" is precise — it requires a direct-map KVA as input. Passing a non-direct-map pointer (e.g., a kernel-image VA) returns an arithmetically-meaningful but semantically-wrong PA; callers must ensure the input is from the direct map.

### `<arch/arm64/mmu.h>`

```c
#define VMALLOC_BASE  0xFFFF800000000000ull

void *mmu_map_mmio(paddr_t pa, size_t size);
```

`mmu_map_mmio(pa, size)` allocates `ceil(size / PAGE_SIZE)` consecutive entries in the vmalloc range starting at `VMALLOC_BASE`. Each entry maps the corresponding 4 KiB chunk of `[pa, pa+size)` with **Device-nGnRnE** attributes (per ARM ARM B2.7.2 — strongly-ordered, no gathering, no reordering, no early write acknowledgement). Returns a kernel VA in the vmalloc range. Caller is responsible for caching the returned KVA (typically in a `g_xxx_base` global at the driver layer).

`pa` must be page-aligned (extincts otherwise). `size` is rounded up to PAGE_SIZE. v1.0 P3-Bb has 512 vmalloc slots (2 MiB total); exhaustion extincts loudly.

Threading at v1.0 P3-Bb: `mmu_map_mmio` is called only from boot_main (single-threaded). The bump cursor is not lock-protected. **Phase 3+ callers from other contexts (driver init, exec) must acquire a vmalloc lock before calling.** Trip-hazard.

## Implementation

### Page-table layout (TTBR1 post-P3-Bb)

```
0xFFFF_0000_0000_0000 - 0xFFFF_007F_FFFF_FFFF   Direct map (l0_ttbr1[0])
                                                covers 8 GiB of physical RAM
                                                via 1 GiB block descriptors
                                                at l1_directmap[1..8]

0xFFFF_8000_0000_0000 - 0xFFFF_8000_001F_FFFF   vmalloc / MMIO (l0_ttbr1[256])
                                                page-grain Device-nGnRnE
                                                via l3_vmalloc[0..511]

0xFFFF_A000_xxxx_xxxx                            Kernel image (l0_ttbr1[KASLR_L0_IDX])
                                                KASLR-randomized
```

### Direct-map PTE attributes

Every direct-map PTE is `PTE_KERN_RW_BLOCK` (1 GiB block at L1) which has `PTE_PXN | PTE_UXN` built in. **R/W + XN unconditionally** — kernel direct map is data, never code. W^X invariant I-12 holds at the alias level: the same physical page mapped R/X via kernel image VA is mapped R/W + XN via direct map; never both R/W and X via the same translation.

### Vmalloc PTE attributes

Every vmalloc-MMIO PTE is `PTE_DEVICE_RW`:
- `PTE_VALID | PTE_TYPE_PAGE | PTE_AF`
- `PTE_SH_NONE` (Device per ARM ARM B2.7.2)
- `PTE_ATTR_IDX(MAIR_IDX_DEVICE)` — Device-nGnRnE
- `PTE_AP_RW_EL1` — kernel R/W only
- `PTE_UXN | PTE_PXN` — never executable

### Vmalloc bump allocator

`g_vmalloc_next_idx` advances per `mmu_map_mmio` call by `ceil(size / PAGE_SIZE)`. v1.0 starts at 0, advances monotonically, extincts at 512. No reclaim at v1.0.

### TLB visibility

`mmu_map_mmio` issues `dsb_ishst + isb` after writing PTEs. No TLB flush is needed because the previous entries were zero (invalid) — there are no stale TLB entries to invalidate. The `dsb_ishst` ensures cross-CPU visibility of the new PTEs through the inner-shareable scope.

### Direct-map alias semantics

The same physical RAM is mapped at TWO virtual addresses post-P3-Bb:
- TTBR0 low identity (PA 0..4 GiB at low VA = PA). v1.0 retains this for legacy access (DTB, kstack guards, MMIO via mmu_map_device).
- TTBR1 direct map (PA 1..8 GiB at high VA = PA | 0xFFFF_0000_*). New at P3-Bb.

Both mappings use Normal-WB cacheable attributes. ARM ARM B2.8 allows multiple cacheable VAs to alias the same PA as long as cacheability attributes match — they do. The kernel uses the high-VA direct map for new code; the low-VA identity remains for the legacy paths. P3-Bd removes TTBR0's identity map entirely.

## Refactored consumers

### `mm/slub.c`

`slab_init_freelist`: object addresses are now direct-map KVAs.
```c
void *base_kva = pa_to_kva(page_to_pa(slab));
for (int i = ...; i >= 0; i--) {
    void *obj = (char *)base_kva + i * c->actual_size;
    *(void **)obj = prev;
    prev = obj;
}
```

`kmem_cache_free`: input pointer is a direct-map KVA; convert to PA for page-frame lookup.
```c
struct page *slab = pa_to_page(kva_to_pa(obj));
```

`kmalloc` (large path): returns direct-map KVA from `alloc_pages`.
```c
return pa_to_kva(page_to_pa(p));
```

`kfree`: input is direct-map KVA; converts to PA for slab metadata + slot validation.

### `mm/phys.c`

`alloc_pages` `KP_ZERO` zeroing loop: uses direct-map KVA instead of PA-as-VA.
```c
u64 *q = pa_to_kva(page_to_pa(p));
for (...) q[i] = 0;
```

`kpage_alloc`: returns direct-map KVA.

`kpage_free`: input is direct-map KVA; `kva_to_pa` for page lookup.

### Unaffected at P3-Bb (deferred to P3-Bc/Bd)

- `lib/dtb.c::dtb_init` — DTB pointer cast PA-as-VA via TTBR0 identity. Will refactor to direct-map KVA when TTBR0 is removed.
- `kernel/thread.c` kstack — still uses PA-as-VA via TTBR0 identity. Refactor needed before TTBR0 removal.
- `mm/phys.c` `struct_pages` array — boot-time PA-as-VA. Refactor needed before TTBR0 removal.
- `arch/arm64/uart.c` PL011 — initial fallback uses PA via TTBR0 identity. mmu_map_mmio is callable but UART hasn't been switched to it yet.
- `arch/arm64/gic.c` GIC dist + redists — uses `mmu_map_device` (TTBR0 identity device blocks). Will switch to `mmu_map_mmio` (vmalloc) when TTBR0 removed.

## Tests

### `kernel/test/test_directmap.c`

Three smoke tests:

#### `directmap.kva_round_trip`

`pa_to_kva` → `kva_to_pa` is the identity over a representative PA range. Checks both arithmetic and the upper-bits invariant (KVA has `KERNEL_DIRECT_MAP_BASE` set).

#### `directmap.alloc_through_directmap`

`kpage_alloc(KP_ZERO)` returns a direct-map KVA. The test writes a sentinel value via the KVA, reads it back, verifies. **Implicitly proves the TTBR1 direct-map L1 block PTE is live** — a faulty mapping would page-fault on the dereference.

#### `directmap.vmalloc_mmio_smoke`

`mmu_map_mmio(pa, PAGE_SIZE)` returns a vmalloc-range KVA. Verifies the KVA is `>= VMALLOC_BASE` and within the first 2 MiB. Plumbing test for the bump allocator + PTE construction + TLB visibility (does NOT exercise Device-nGnRnE attribute behavior — that's meaningful only against actual MMIO; tested implicitly when gic.c / uart.c switch to mmu_map_mmio in P3-Bd).

## Error paths

| Path | Trigger | Message |
|---|---|---|
| `mmu_map_mmio` | `pa` not page-aligned | `"mmu_map_mmio: pa not page-aligned"` |
| `mmu_map_mmio` | `pa + size` overflow | `"mmu_map_mmio: pa + size overflow"` |
| `mmu_map_mmio` | vmalloc l3 exhausted | `"mmu_map_mmio: l3_vmalloc exhausted (>512 4-KiB MMIO pages)"` |

All extincts at v1.0; rare programmer-error or hardware-misconfig conditions.

## Performance characteristics

- `pa_to_kva`: 1 OR instruction (constant-folded by compiler).
- `kva_to_pa`: 1 AND instruction (constant-folded).
- `mmu_map_mmio`: O(n_pages) PTE writes + DSB ISHST + ISB. Boot-time cost; not on hot path.
- Direct-map dereference: same as any TTBR1 access; standard ARM64 TLB walk.

## Status

**Implemented at P3-Bb** (this chunk):
- Direct map L1 covering PA 1 GiB..9 GiB (8 1-GiB blocks).
- Vmalloc L3 covering 2 MiB starting at VMALLOC_BASE.
- `pa_to_kva` / `kva_to_pa` inlines in `<thylacine/page.h>`.
- `mmu_map_mmio` API in `<arch/arm64/mmu.h>`.
- SLUB / kpage_alloc / KP_ZERO refactor.
- 3 smoke tests.

**Deferred to P3-Bc/Bd**:
- DTB pointer cast → direct-map KVA.
- struct_pages array cast → direct-map KVA.
- kstack pointer + guard mechanism → direct-map KVAs (mmu_set_no_access_range refactor).
- UART base → mmu_map_mmio KVA.
- GIC dist + redist bases → mmu_map_mmio KVAs.
- Removal of TTBR0 identity-map (kernel runs entirely on TTBR1 direct map + vmalloc + image).
- Per-Proc TTBR0 + cpu_switch_context swap (P3-Bc/Bd full payoff).

**Deferred to v1.x hardening**:
- KASLR-randomized direct-map base (v1.0 has fixed `0xFFFF_0000_0000_0000`).
- vmalloc reclaim (v1.0 bump allocator is one-way; reuse comes via boot reset only).

**Deferred to v2.x** (NOVEL §3.9 Contract D):
- Capability-based kernel addressing (Rust port + CHERI/Morello).

## Known caveats / footguns

- **`kva_to_pa` requires a direct-map input**: passing a kernel-image VA (high TTBR1 KASLR address) returns an arithmetically-meaningful but semantically-wrong PA. Future kernel code that handles arbitrary kernel pointers needs an `is_directmap_kva()` predicate or a more discriminating conversion.
- **PA must fit in bits 47:0**: v1.0's direct map covers 8 GiB; PA ≥ 8 GiB has no direct-map mapping. Larger physical memory needs the L1 bump extended.
- **Vmalloc bump is single-writer**: lock needed for Phase 3+ multi-thread callers.
- **TTBR0 identity is still alive**: P3-Bb deliberately keeps TTBR0 intact for legacy paths (DTB, kstack, MMIO). The full payoff (TTBR0 free for per-Proc use) lands at P3-Bc/Bd.
- **W^X aliasing is at-the-PTE-level, not at-the-physical-page-level**: a future code path that maps the same physical page as W via direct map AND as X via some new mapping would violate I-12. Audit-trigger: any new mmu_* function that creates an X mapping must verify the page isn't already W-mapped via direct map.
