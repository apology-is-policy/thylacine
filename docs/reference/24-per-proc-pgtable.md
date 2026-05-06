# Reference: per-Proc page-table allocator (P3-Bcb)

## Purpose

Each non-kernel Proc owns its own L0 translation table for TTBR0 (the user-half of the 48-bit VA space). Per-Proc page tables are the foundation for independent address spaces — every Proc sees its own user-mappings, and a context switch loads the destination's pgtable_root + asid into TTBR0_EL1.

P3-Bcb is the **lifecycle plumbing only**: alloc at `proc_alloc`, free at `proc_free`. The actual TTBR0 swap on context switch lands at P3-Bd; populating L1/L2/L3 sub-tables happens at P3-D (VMA tree + page-fault handler). At P3-Bcb the allocated L0 sits unused — all 512 entries are invalid.

ARCH §16: "Each Proc has a private user-half address space (TTBR0). Kernel-half (TTBR1) is shared. Independent address spaces are critical for process isolation; the per-Proc pgtable_root is what makes them independent."

## Public API

### `<arch/arm64/mmu.h>`

```c
paddr_t proc_pgtable_create(void);
void    proc_pgtable_destroy(paddr_t root);
```

#### `proc_pgtable_create(void) → paddr_t`

Allocates one 4 KiB page from buddy with `KP_ZERO`. The page is the L0 translation table — 512 × 8-byte entries, all invalid (PTE_VALID = 0). Returns the **physical address** of the L0 page; caller writes this into `struct Proc.pgtable_root`. Returns 0 on OOM (caller treats as ENOMEM and rolls back the Proc allocation).

The L0 PA is the value loaded into TTBR0_EL1 at context switch (alongside the Proc's ASID in bits 63:48):

```
TTBR0_EL1 = (asid << 48) | pgtable_root
```

(P3-Bd will wire this load.)

#### `proc_pgtable_destroy(paddr_t root)`

Frees the L0 page back to buddy. Idempotent on `root == 0` (kproc + rolled-back-pre-create paths hit a no-op).

**v1.0 P3-Bcb scope: frees just the L0 page.** No sub-tables exist yet because no fault-handler / vmo_map path populates L1/L2/L3 entries. P3-D adds page-fault → demand-page allocation → PTE installation; `proc_pgtable_destroy` will then need to walk L0 → L1 → L2 → L3, freeing every installed sub-table before the L0. That refactor is **P3-D-blocking** and tracked as trip-hazard #116 in `phase3-status.md`.

## Implementation

### `arch/arm64/mmu.c`

```c
paddr_t proc_pgtable_create(void) {
    struct page *l0_pg = alloc_pages(0, KP_ZERO);
    if (!l0_pg) return 0;
    return page_to_pa(l0_pg);
}

void proc_pgtable_destroy(paddr_t root) {
    if (root == 0) return;
    struct page *l0_pg = pa_to_page(root);
    free_pages(l0_pg, 0);
}
```

The implementation is intentionally trivial because P3-Bcb is just plumbing. The complexity arrives at P3-D when fault-driven sub-table allocation lands.

### `kernel/proc.c` integration

`proc_alloc`:

```c
p->pgtable_root = proc_pgtable_create();
if (p->pgtable_root == 0) {
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);          // releases handles + Proc; pgtable + asid stay 0 (no-op cleanup)
    return NULL;
}
p->asid = asid_alloc();    // extincts on exhaustion (no graceful rollback at v1.0)
```

`proc_free`:

```c
if (p->pgtable_root != 0) {
    proc_pgtable_destroy(p->pgtable_root);
    p->pgtable_root = 0;
}
if (p->asid != 0) {        // ASID 0 is kernel-reserved; don't free
    asid_free(p->asid);
    p->asid = 0;
}
```

Both cleanup branches are idempotent on 0 — kproc (which has both fields = 0 by KP_ZERO via `proc_init`) hits no-ops, as do rolled-back-pre-create paths.

## Data structures

`struct Proc` grows by 16 bytes:

```c
struct Proc {
    // ... pre-P3-Bcb fields (96 bytes) ...
    paddr_t  pgtable_root;        // +8
    u16      asid;                // +2
    u16      _pad_asid[3];        // +6 alignment padding
};
_Static_assert(sizeof(struct Proc) == 112,
    "struct Proc size pinned at 112 bytes");
```

The 6-byte alignment pad is explicit; subsequent field additions (Phase 3+ VMA tree root, credentials, capability bitmask, notes queue per ARCH §16) will replace the pad before growing the struct further.

## State machines

### Proc lifecycle (P3-Bcb additions)

```
INVALID (KP_ZERO; pgtable_root=0, asid=0)
   │
   │ proc_alloc
   ▼
ALIVE (pgtable_root=L0_PA, asid=user_asid)
   │
   │ ... rfork / wait_pid / exits ...
   ▼
ZOMBIE (state set in exits; pgtable + asid still installed)
   │
   │ proc_free (called from wait_pid reap path)
   ▼
REAPED (Proc descriptor freed; pgtable_root + asid released)
```

`proc_init_fields` (called from both `proc_alloc` and `proc_init`) leaves `pgtable_root = 0` and `asid = 0` from KP_ZERO. `proc_alloc` then installs real values; `proc_init` (kproc only) does NOT — kproc retains the zero values forever.

## Spec cross-reference

No new TLA+ spec at P3-Bcb (impl-only refactor). The pgtable lifecycle is structurally trivial: alloc-once-on-Proc-create, free-once-on-Proc-destroy. The interesting concurrency is in the ASID allocator (already specced informally in `asid.c`'s lock comments) and in the eventual TTBR0 swap (P3-Bd will extend `scheduler.tla`).

## Tests

`kernel/test/test_proc_pgtable.c`:

- **`proc.pgtable_alloc_smoke`**: Allocate a Proc; verify `pgtable_root != 0` AND page-aligned; verify `asid` is in `[ASID_USER_FIRST, ASID_USER_LAST]`; verify `asid_inflight()` advances by 1. Force ZOMBIE state, free the Proc; verify `asid_inflight()` returns to baseline.

- **`proc.pgtable_lifecycle_stress`**: 64 alloc/free cycles. Verify `proc_total_created` / `proc_total_destroyed` advance in step (no leak), and `asid_inflight()` returns to baseline after the loop (no ASID leak).

The existing rfork stress tests (`proc.rfork_stress_1000`, `proc.cascading_rfork_stress`) implicitly exercise pgtable_create / pgtable_destroy at high volume; the new tests are focused regression checks for the lifecycle plumbing alone.

## Error paths

- `proc_pgtable_create`: returns `0` on `alloc_pages` OOM. Caller (proc_alloc) treats as ENOMEM and rolls back via proc_free.
- `proc_pgtable_destroy`: no error path — `root == 0` is silently no-op.
- `asid_alloc`: extincts on 8-bit space exhaustion (255 ASIDs at v1.0 — unreachable under test scales).

## Performance characteristics

- `proc_pgtable_create`: single buddy allocation (4 KiB, KP_ZERO). Tens of microseconds at v1.0 with the magazine layer warm.
- `proc_pgtable_destroy`: single buddy free. Microseconds.
- ASID alloc/free: spin-lock-protected counter / free-list operation; sub-microsecond uncontended.

## Status

- **Implemented**: pgtable_create / pgtable_destroy / proc_alloc + proc_free integration / 2 tests.
- **Stubbed**: TTBR0 swap (P3-Bd); sub-table walk in destroy (P3-D).
- **Deferred**: kproc kernel-only TTBR0 (P3-Be) — kproc's pgtable_root stays 0 at P3-Bcb; P3-Be wires a degenerate "kernel-only" mapping for the TTBR0 swap path's safety.

Commit landing point: `d256804`.

## Known caveats / footguns

1. **kproc has `pgtable_root = 0` and `asid = 0`** — proc_free's gates (`!= 0` checks) prevent passing 0 to `proc_pgtable_destroy` / `asid_free`. Future code that adds new cleanup paths MUST include the same gates or use the existing helpers.

2. **`asid_free(0)` extincts** — ASID 0 is kernel-reserved. Bypassing the gate (e.g., a future bulk-cleanup path that doesn't check `asid != 0`) would extinct the kernel.

3. **`proc_pgtable_destroy` at P3-Bcb only frees L0** — when P3-D adds VMA + page-fault → sub-table allocation, this function MUST be extended to walk + free L1/L2/L3. Otherwise sub-tables leak. Trip-hazard #116.

4. **OOM order in proc_alloc**: handle_table_alloc → pgtable_create → asid_alloc. Reordering changes the rollback shape. asid_alloc has no rollback path (extincts on exhaustion); pgtable_create rolls back via proc_free.

5. **L0 alignment**: `proc_pgtable_create` returns a PA from buddy at order=0 (single page, page-aligned). The hardware requires the L0 PA to be page-aligned; this is structurally guaranteed.

## Naming rationale

`proc_pgtable_*` follows the standard `<subsystem>_<verb>_<noun>` form. No thematic name proposed at P3-Bcb (pgtable is the standard term; clarity wins). A future v1.x rename to something like `proc_addrspace_*` could reflect the broader "address space" concept (which would include VMA tree + capability bitmask), but that's a P3-D decision.
