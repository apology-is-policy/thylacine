# Reference: per-Proc VMA list (P3-Da)

## Purpose

A VMA (Virtual Memory Area) describes a contiguous range of user virtual addresses with associated permissions and a backing VMO. The sorted list of VMAs anchored at `struct Proc.vmas` forms the per-Proc address-space description against which page faults are dispatched. P3-Da establishes the data structure + lifecycle hooks; P3-Db wires `vmo_map` to create VMA entries; P3-Dc connects `arch_fault_handle`'s user-mode path to `vma_lookup` → demand paging.

ARCH §16 (process address space) names this surface explicitly as the "VMA tree" — at v1.0 P3-Da it's a sorted doubly-linked list (O(N) operations, room for ~hundreds of entries before perceptible cost). Phase 5+ adds an interval tree (RB-tree with per-node interval-max) for O(log N) lookup once N grows.

## Public API

### `<thylacine/vma.h>`

```c
#define VMA_PROT_READ   (1u << 0)
#define VMA_PROT_WRITE  (1u << 1)
#define VMA_PROT_EXEC   (1u << 2)
#define VMA_PROT_RW     (VMA_PROT_READ | VMA_PROT_WRITE)
#define VMA_PROT_RX     (VMA_PROT_READ | VMA_PROT_EXEC)

struct Vma {
    u64 magic;            // VMA_MAGIC
    u64 vaddr_start, vaddr_end;
    u32 prot;
    u32 _pad;
    struct Vmo *vmo;
    u64 vmo_offset;
    struct Vma *next, *prev;
};

void vma_init(void);
struct Vma *vma_alloc(u64 vaddr_start, u64 vaddr_end, u32 prot,
                     struct Vmo *vmo, u64 vmo_offset);
void vma_free(struct Vma *v);
int  vma_insert(struct Proc *p, struct Vma *v);
void vma_remove(struct Proc *p, struct Vma *v);
struct Vma *vma_lookup(struct Proc *p, u64 vaddr);
void vma_drain(struct Proc *p);

u64 vma_total_allocated(void);
u64 vma_total_freed(void);
```

#### Constraints validated by `vma_alloc`

- `vaddr_start < vaddr_end` (zero-length rejected).
- Both page-aligned (4 KiB).
- `vmo != NULL`.
- `prot` ∈ `{0, R, RW, RX}` — W+X rejected at the VMA layer (mirrors ARCH §28 I-12).

Returns NULL on any constraint violation without partial allocation. Calls `vmo_map` (mapping_count++) on success; `vma_free` calls `vmo_unmap` symmetrically.

#### Insertion + lookup

`vma_insert` is a sorted-list walk:
1. Walk to find the last node with `start < v->start` AND check overlap with every node visited.
2. On overlap (any node where `[v->start, v->end)` intersects `[node->start, node->end)`), return -1.
3. Otherwise link `v` between `prev` and the next-larger-start node.

Adjacent ranges (touching at a single boundary) are NOT overlap — `[a, b)` and `[b, c)` are accepted (half-open intervals).

`vma_lookup` is a sorted-list early-exit:
1. Walk; on each node, check `vaddr_start <= addr < vaddr_end`.
2. Early exit when `node->vaddr_start > addr` (lookup miss; sorted list).

`vma_remove` unlinks; the caller owns the Vma and typically calls `vma_free` next.

`vma_drain` walks the whole list, removing + freeing each — the lifecycle hook called from `proc_free`.

## Implementation

### `kernel/vma.c`

- SLUB cache `g_vma_cache` (sized for `struct Vma` = 64 bytes).
- Atomic counters `g_vma_allocated` / `g_vma_freed` for diagnostic + leak detection.
- `ranges_overlap(a, b, c, d) = (a < d && c < b)` — half-open form.
- `vma_alloc` calls `vmo_map(vmo)` (mapping_count++) on success; `vma_free` calls `vmo_unmap(vmo)` (mapping_count--).

### `kernel/proc.c` integration

`proc_free` calls `vma_drain(p)` BEFORE `handle_table_free`. The order matters for VMO lifecycle:
- `vma_drain` releases `mapping_count` via repeated `vmo_unmap`.
- `handle_table_free` closes any open VMO handles, releasing `handle_count` via `vmo_unref`.
- `vmo_free` runs only when both counts reach 0 — see `specs/vmo.tla::NoUseAfterFree`.

For kproc: `proc_init` doesn't allocate VMAs; `g_kproc->vmas` stays NULL via KP_ZERO. `proc_free` extincts on kproc anyway.

## Data structures

```c
struct Vma { ... };  // 64 bytes, magic at offset 0
struct Proc {
    // ... (existing 112 bytes) ...
    struct Vma *vmas;   // +8 bytes; total 120 bytes
};
```

`struct Proc` size assert bumped 112 → 120.

## State machines

### VMA lifecycle (P3-Da)

```
ALLOC                          (vma_alloc)
   │
   │ kmem_cache_alloc + vmo_map
   ▼
DETACHED (next/prev = NULL)    (initial state post-alloc)
   │
   │ vma_insert(p, v)
   ▼
LINKED (in p->vmas list)        (sorted by vaddr_start)
   │
   │ vma_remove(p, v)           (or vma_drain(p))
   ▼
DETACHED                        (returnable to alloc state if reused)
   │
   │ vma_free(v)                (vmo_unmap + kmem_cache_free)
   ▼
FREED (magic clobbered by SLUB freelist write)
```

`vma_insert` rejects DETACHED → LINKED transitions on overlap (returns -1; caller still owns the DETACHED Vma and must `vma_free` it).

## Spec cross-reference

No new TLA+ spec at P3-Da — sorted-list operations are pure data-structure code; CLAUDE.md spec-first guidance lists this in the "config parsing / pure computation" tier.

The VMA layer interacts with the existing `specs/vmo.tla::NoUseAfterFree` invariant: every `vma_alloc` is a `vmo_map` and every `vma_free` is a `vmo_unmap`. The P3-Da test `vma.drain_releases_all` verifies the symmetry — `vmo_mapping_count` returns to baseline after drain.

P3-Dc's demand-paging dispatcher will need a spec — likely an extension to `vmo.tla` covering "fault → VMA lookup → page allocate → PTE install" atomicity. Documented as a P3-Dc work item.

## Tests

`kernel/test/test_vma.c` — six unit tests:

1. **`vma.alloc_free_smoke`**: basic alloc/free; verifies `vma_total_allocated` / `vma_total_freed` advance.

2. **`vma.alloc_constraints`**: zero-length / reversed range / unaligned start/end / W+X / NULL VMO all return NULL.

3. **`vma.insert_lookup_smoke`**: 3 non-overlapping VMAs (gap pattern); lookup hits at every covered address; misses on uncovered addresses + before-any-VMA address.

4. **`vma.insert_overlap_rejected`**: exact / partial-left / partial-right overlaps return -1; adjacent (touching boundary) accepted as non-overlapping (half-open ranges).

5. **`vma.insert_sorted_invariant`**: insert in mixed order [4,2,6,1,3]; walk the resulting list; verify ascending order.

6. **`vma.drain_releases_all`**: 4-VMA insert + drain; verify `vmo_mapping_count` returns to baseline (proves the vmo_map ↔ vma_alloc symmetry).

## Error paths

- `vma_alloc` returns NULL on any constraint violation OR on `kmem_cache_alloc` OOM. No partial allocation: VMO ref is taken AFTER kmem_cache_alloc succeeds, so an OOM doesn't leave a stale ref.
- `vma_insert` returns -1 on overlap. Caller owns the rejected Vma.
- `vma_free` extincts on `magic != VMA_MAGIC` (SLUB freelist clobber detection) or on `next/prev != NULL` (still-linked Vma).
- `vma_remove` extincts on `magic != VMA_MAGIC`.

## Performance characteristics

At v1.0 P3-Da (sorted list, O(N) per operation):
- `vma_alloc`: kmem_cache_alloc + vmo_map → microseconds.
- `vma_free`: kmem_cache_free + vmo_unmap → microseconds.
- `vma_insert`: O(N) walk for overlap check + insert. With v1.0 typical N (<10 per Proc) this is sub-microsecond.
- `vma_lookup`: O(N) walk with early exit. Typical hit is O(M) where M is the rank of the matching VMA.
- `vma_drain`: O(N).

Phase 5+ RB-tree converts insert/lookup to O(log N).

## Status

- **Implemented at P3-Da (`a7ff570`)**: data structure + alloc/free/insert/remove/lookup/drain + 6 unit tests + struct Proc integration + bootstrap order.
- **Stubbed**: `vmo_map` extension to take (Proc, vaddr, length, prot) → vma_alloc + vma_insert (P3-Db).
- **Stubbed**: arch_fault_handle's user-mode dispatch path → vma_lookup → demand paging (P3-Dc).
- **Stubbed**: proc_pgtable_destroy sub-table walk (P3-Db / P3-D depending on when sub-tables get populated).

## Known caveats / footguns

1. **vma_drain is the ONLY safe path to free all VMAs of a Proc** at v1.0. Manual iteration requires `vma_remove` first then `vma_free` — the gates on `next/prev != NULL` in `vma_free` enforce this.

2. **Adjacent ranges are NOT overlap**. `[a, b)` and `[b, c)` are accepted by `vma_insert`. The half-open convention is standard but sometimes counter-intuitive; tests cover the boundary.

3. **W+X rejection is layered**. The VMA layer rejects W+X; the PTE constructors reject W+X; the ELF loader rejects W+X. Three layers of defense for ARCH §28 I-12.

4. **Phase 5+ multi-thread Procs need a per-Proc lock** around vma_insert/remove. v1.0 single-thread Procs sidestep the issue. Trip-hazard documented.

5. **VMA list is sorted**. `vma_insert` relies on this for overlap detection AND lookup early-exit. Direct manipulation of `p->vmas` outside the API risks breaking the invariant; only `vma_insert/remove/drain` should mutate.

6. **`vma_alloc` always takes `vmo_map`** (mapping_count++). Callers that pre-incremented mapping_count externally would over-count. The vma layer is the only place that takes mapping_count at v1.0; future direct mapping_count manipulation must be coordinated.

## Naming rationale

`vma` — standard term in mm parlance (Linux uses `vm_area_struct` etc.). No thematic name proposed; clarity wins.

`VMA_PROT_*` mirrors POSIX `PROT_READ/WRITE/EXEC` for syscall-surface alignment at Phase 5+ (mmap/mprotect).

`vma_drain` rather than `vma_free_all` because "drain" reflects the streaming-iteration semantic (walk-and-free, not bulk-free) — same word as `gic.c::queue_drain`, `phys.c::magazine_drain`.
