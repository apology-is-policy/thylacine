# Reference: ASID allocator (P3-Ba)

## Purpose

The ASID (Address Space Identifier) allocator manages the 8-bit hardware ASID space for per-Proc TTBR0 page-table tagging. It exists so that context switches between Procs do not require a full TLB flush: each Proc's user-mappings are tagged with its assigned ASID, and TLB lookups match against the current TTBR0_EL1.ASID at hardware level. The allocator is the foundation for P3-Bb (per-Proc page tables), P3-Bc (cpu_switch_context TTBR0 swap), and Phase 3+ exec / userspace.

At v1.0 P3-Ba the allocator is **forward-looking** — no Proc actually consumes an ASID yet. P3-Bb wires the call sites in `proc_alloc` / `proc_free`; P3-Bc loads the ASID into TTBR0_EL1 on context switch.

ARCH §6.2: "nG bit set in user mappings (no global TLB entry); kernel mappings global with ASID 0." This allocator owns the user-side ASID space (1..255).

## Public API

### `<arch/arm64/asid.h>`

```c
#define ASID_RESERVED_KERNEL  0u
#define ASID_USER_FIRST       1u
#define ASID_USER_LAST        255u
#define ASID_USER_MAX         (ASID_USER_LAST - ASID_USER_FIRST + 1u)

void asid_init(void);
u16  asid_alloc(void);
void asid_free(u16 asid);
void asid_tlb_flush(u16 asid);

u64       asid_total_allocated(void);
u64       asid_total_freed(void);
unsigned  asid_inflight(void);
```

#### `asid_init(void)`

Boot-time initialization. Sets `g_next_asid = ASID_USER_FIRST`; clears all counters and the free-list. Idempotent on first call; extincts on second call to surface bootstrap-order bugs. Must be called once after `slub_init` (the discipline matches main.c bootstrap order; no SLUB dependency at v1.0 since state lives in BSS).

#### `asid_alloc(void) → u16`

Returns a fresh ASID in `[ASID_USER_FIRST, ASID_USER_LAST]`. Two paths:

1. **Free-list pop (LIFO)**: if `g_asid_free_count > 0`, pops the most-recently-freed ASID. Cache-locality benefit: the slot's metadata is warm; its TLB was already flushed at free time.
2. **Monotonic fresh**: otherwise, returns `g_next_asid++` if it's still ≤ `ASID_USER_LAST`.

If both paths are unavailable (free-list empty AND monotonic counter past last), extincts with the message `"asid_alloc: 8-bit ASID space exhausted at v1.0 (generation rollover deferred to Phase 5+)"`. Unreachable under v1.0 test scales.

#### `asid_free(u16 asid)`

Returns `asid` to the free-list. **Issues a TLB-invalidate-by-ASID (inner-shareable) BEFORE pushing to the pool**, ensuring the next caller of `asid_alloc` that pops this slot sees a globally-flushed TLB. Out-of-range (`< ASID_USER_FIRST` or `> ASID_USER_LAST`) extincts. Inflight underflow (free-without-alloc, or double-free) extincts. Free-list overflow extincts (catches alloc/free imbalance — should be unreachable if the API is used correctly).

#### `asid_tlb_flush(u16 asid)`

Issues the broadcast TLB-invalidate sequence:
```
dsb ishst       — make pending stores visible to TLB walkers.
tlbi aside1is   — broadcast invalidate-by-ASID across all PEs.
dsb ish         — wait for completion across all PEs in the IS domain.
isb             — discard speculative translations on this PE.
```
Matches Linux's `flush_tlb_mm(mm)` and the ARM ARM D5.10 recommended sequence. Used internally by `asid_free`; exposed for callers that have torn down a Proc's user mappings while keeping the ASID alive (Phase 3+ may use this for `mprotect`-style flushes).

#### Diagnostics

`asid_total_allocated`, `asid_total_freed`, and `asid_inflight` return atomic snapshots of the running totals. Useful for leak detection in tests; the sum `total_allocated - total_freed - inflight == 0` invariant is maintained.

## Implementation

### State (`arch/arm64/asid.c`)

```c
static spin_lock_t g_asid_lock = SPIN_LOCK_INIT;
static u16  g_next_asid;
static u16  g_asid_free_list[ASID_USER_MAX];
static u32  g_asid_free_count;
static u32  g_asid_inflight;
static u64  g_asid_total_allocated;
static u64  g_asid_total_freed;
static bool g_asid_initialized;
```

All state lives in BSS. The lock is `spin_lock_irqsave`-paired throughout — at v1.0 the lock is uncontested (single-threaded boot allocates ASIDs sequentially via rfork from kthread); Phase 3+ exec on secondaries can introduce contention.

### Lock discipline

- `asid_alloc`: lock → check free-list → pop or monotonic fresh → counter ++ → unlock.
- `asid_free`: TLB flush (no lock) → lock → push free-list → counter -- → unlock.
- TLB flush is OUTSIDE the lock because the `tlbi` + `dsb ish` is multi-cycle and would unnecessarily extend lock-hold time. The flush is ordered before the push (program order on this CPU); the lock ensures the push is atomic; the next allocator who pops the slot sees the post-flush state via the lock's release/acquire synchronization.

### Free-list as LIFO

The free-list is a stack (top-of-stack = most recently pushed). The LIFO discipline is deliberate:

- **Cache locality**: a recently-freed ASID's slot is warm in the cache hierarchy on the freeing CPU; reusing it benefits L1/L2 reuse.
- **TLB warmth**: even though the TLB was invalidated for this ASID, the page-table entries that built up around it remain in caches; if the new owner happens to map similar VAs, walks are faster.
- **Simplicity**: a stack is simpler than a queue.

A FIFO discipline would distribute reuse more uniformly across recently-freed entries (avoiding the LIFO "hot stack top"); not measurably better at v1.0 scales.

### TLB flush sequence

`asid_tlb_flush` issues `tlbi aside1is, x0` where `x0[63:48] = ASID, x0[47:0] = 0`. Per ARM ARM D5.10:

- `aside1is` invalidates **all stage-1 TLB entries (EL1 + EL0) matching the ASID**, broadcast across the **inner-shareable** domain.
- The `dsb ishst` before `tlbi` ensures any pending stores (e.g., to page-table entries that the TLB walker would consult) are committed before the invalidate.
- The `dsb ish` after `tlbi` waits for the broadcast to complete — i.e., all PEs in the IS domain have observed the invalidate.
- The `isb` discards any speculative translations the issuing PE may have prefetched.

The encoding `(u64)asid << 48` puts the ASID in bits [63:48]. Per ARM ARM C5.5.1, `tlbi aside1is` uses bits [63:48] for the ASID input regardless of TCR_EL1.AS (which only affects TTBR0/1's storage width). So the same encoding works for 8-bit and 16-bit ASIDs.

## Data structures

### `g_asid_free_list[ASID_USER_MAX]`

A 255 × 2-byte = 510-byte BSS array. Simple stack: `g_asid_free_list[0..g_asid_free_count - 1]` are the live entries; `g_asid_free_count` is the next free slot.

The size is set to `ASID_USER_MAX` (255) which is the maximum possible inflight count — at this scale, every Proc could free its ASID without overflow. Sizing larger would be defensive but pointless; sizing smaller would risk overflow.

### Counters

- `g_next_asid` (u16): monotonic counter. Increments only on the monotonic-fresh path of `asid_alloc`. Capped at `ASID_USER_LAST + 1` (the post-increment value when we're about to overflow).
- `g_asid_inflight` (u32): running balance of allocated minus freed.
- `g_asid_total_allocated`, `g_asid_total_freed` (u64): cumulative over kernel lifetime.

## State machines

### ASID lifecycle

```
[free / never allocated]
    ↓ asid_alloc (monotonic fresh: g_next_asid++)
[in use by Proc P]
    ↓ Proc P transitions ZOMBIE → reaped → asid_free
[on the free-list, TLB-flushed]
    ↓ asid_alloc (free-list pop)
[in use by Proc Q]
    ↓ ...
```

The TLB-flushed-on-free invariant is the key correctness property: at any moment when an ASID is on the free-list, its TLB entries are guaranteed-flushed across all PEs.

### Spec cross-reference

ASID management is "config parsing" territory per CLAUDE.md spec-first guidance — counter management with no concurrent state machine. No formal spec at P3-Ba.

The TLB-flush-before-reuse invariant is enforced structurally by `asid_free`'s ordering (flush → lock → push); inspected by code review at P3-Ba and the upcoming P3-B closing audit (will likely fold into a Phase 3 closing audit covering per-Proc TTBR0 + page-fault handler + exec).

## Tests

### `kernel/test/test_asid.c`

#### `asid.alloc_unique`

Allocate 8 ASIDs. Verify:
- Each is in `[ASID_USER_FIRST, ASID_USER_LAST]`.
- All are pairwise distinct.
- `asid_total_allocated` advances by 8.
- `asid_inflight` advances by 8.

Free in reverse order (exercises the LIFO free-list). Verify:
- `asid_total_freed` advances by 8.
- `asid_inflight` returns to pre-test baseline.

#### `asid.free_reuses`

Alloc → free → alloc. Verify the second alloc reuses the freed ASID via the LIFO free-list pop. Confirms the free-list discipline.

#### `asid.inflight_count`

Verify `asid_inflight()` reports the running balance accurately across alloc + free.

### Tests deliberately omitted

**Exhaustion**: allocating > 255 ASIDs would extinct the kernel. Could be exercised via the fault matrix (`tools/test-fault.sh`) but not as a smoke test. v1.0 deferred.

## Error paths

| Path | Trigger | Message |
|---|---|---|
| `asid_init` re-entry | calling twice | `"asid_init called twice"` |
| `asid_alloc` pre-init | calling before `asid_init` | `"asid_alloc before asid_init"` |
| `asid_alloc` exhaustion | free-list empty AND `g_next_asid > ASID_USER_LAST` | `"asid_alloc: 8-bit ASID space exhausted at v1.0 (generation rollover deferred to Phase 5+)"` |
| `asid_free` pre-init | calling before `asid_init` | `"asid_free before asid_init"` |
| `asid_free` out-of-range | `asid < ASID_USER_FIRST` or `> ASID_USER_LAST` | `"asid_free: out-of-range ASID"` |
| `asid_free` underflow | `g_asid_inflight == 0` | `"asid_free: inflight count would underflow (double-free or free-without-alloc?)"` |
| `asid_free` overflow | `g_asid_free_count >= ASID_USER_MAX` | `"asid_free: free-list overflow (impossible if alloc/free are balanced — corruption?)"` |

All are extincts (kernel halt) — there is no graceful return path for these errors at v1.0; they all signify either bootstrap-order bugs or memory corruption.

## Performance characteristics

- `asid_alloc`: lock acquire + counter check + (free-list pop OR monotonic increment) + counter ++ + lock release. ~10 instructions hot path; the lock is the dominant cost.
- `asid_free`: TLB flush (multi-cycle; involves cross-CPU broadcast) + lock + push + lock. The TLB flush is the dominant cost — a `tlbi aside1is` + `dsb ish` round-trips through the inner-shareable barrier coordinator (~10-50 cycles depending on platform).
- `asid_tlb_flush`: ~10-50 cycles.
- Read accessors: single atomic load each.

## Status

**Implemented at P3-Ba** (this chunk):
- Allocator + free-list + TLB flush.
- Boot-time init via `asid_init()` in main.c.
- 3 smoke tests (`asid.alloc_unique`, `asid.free_reuses`, `asid.inflight_count`).
- Reference doc (this file).

**Wired by future sub-chunks**:
- P3-Bb: `proc_alloc` calls `asid_alloc`; `proc_free` calls `asid_free`. struct Proc gains `asid` field.
- P3-Bc: `cpu_switch_context` writes `(asid << 48) | pgtable_root_pa` into TTBR0_EL1.

## Known caveats / footguns

- **No rollover at v1.0**: `asid_alloc` extincts on exhaustion (>= 256 lifetime allocations + zero frees outpacing). v1.0 test scales (≤ ~30 alive Procs at once; cumulative bounded by test count) don't approach this. Phase 5+ adds Linux-style generation rollover with full TLB flush + per-Proc generation match.
- **Reserved ASID 0**: must remain reserved; assigning ASID 0 to a user Proc would conflict with the kernel's TTBR1 entries (which use ASID 0 with nG=0). The allocator structurally excludes ASID 0 from `asid_alloc` returns.
- **TLB flush is global, not local**: `tlbi aside1is` is inner-shareable. On a many-core system this could be expensive; v1.0 has 4 vCPUs, fine. Future scaling concern.
- **Free-list ordering is LIFO, not FIFO**: a recently-freed ASID is reused soonest. Could create thrashing patterns in pathological workloads; not a v1.0 concern.
- **No Proc → ASID linkage at P3-Ba**: this chunk only provides the allocator. P3-Bb wires it.
