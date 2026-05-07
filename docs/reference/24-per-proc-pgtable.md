# Reference: per-Proc page-table allocator (P3-Bcb / P3-Bd / P3-Db)

## Purpose

Each non-kernel Proc owns its own L0 translation table for TTBR0 (the user-half of the 48-bit VA space). Per-Proc page tables are the foundation for independent address spaces — every Proc sees its own user-mappings, and a context switch loads the destination's pgtable_root + asid into TTBR0_EL1.

History:
- **P3-Bcb**: lifecycle plumbing — alloc at `proc_alloc`, free at `proc_free`. Both API entries are trivial: `proc_pgtable_create` allocates a single L0 page; `proc_pgtable_destroy` frees just that page.
- **P3-Bd**: TTBR0 swap on context switch (`cpu_switch_context` saves+loads TTBR0_EL1 atomically with the rest of the register state).
- **P3-Db**: extends `proc_pgtable_destroy` to recursively walk L0 → L1 → L2 → L3 freeing every sub-table reached via a table descriptor (closes trip-hazard #116). Wired in advance of demand-paging populating sub-tables (P3-Dc).

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

#### `proc_pgtable_destroy(paddr_t root)` — P3-Db recursive walk

Walks the L0 → L1 → L2 → L3 tree depth-first, freeing every translation-table page reachable via a table descriptor. Leaf pages (L3 page descriptors / L2 block descriptors) are NOT freed — those user-VA-mapped pages are owned by the VMA layer (VMO `mapping_count` lifecycle); the walker frees only translation-table pages. Idempotent on `root == 0` (kproc + rolled-back-pre-create paths hit a no-op).

**Walk discipline**:
1. For each L0 entry: skip invalid; for table descriptors, recurse into L1; (L0 has no block form on AArch64 stage-1 with 4-KiB granule, so the block-skip branch is dead at v1.0 but kept for symmetry).
2. For each L1 entry: skip invalid; skip block descriptors (1-GiB user blocks are leaf and the user pages they reference belong to VMA layer); for table descriptors, recurse into L2.
3. For each L2 entry: skip invalid; skip block descriptors (2-MiB user blocks); for table descriptors, recurse into L3.
4. For L3: free the L3 table page itself; do NOT touch its 512 page-descriptor entries (those are leaves).
5. After children freed, free the table page at this level.

**Cost**: bounded by populated entries — empty tree (no faults yet) is O(L0 = 1 page free); fully-populated 4-GiB user space is the worst case. v1.0 userspace processes touch a few segments + a stack, so a handful of L3 sub-tables per Proc.

**TLB lifecycle**: the walker does NOT issue per-page TLB ops. ASID-tagged entries for the dying Proc are flushed by `asid_free`'s broadcast `tlbi aside1is`, which fires before the ASID is recycled (regardless of whether `asid_free` runs before or after `proc_pgtable_destroy` in `proc_free`). The Proc is also no longer running on any CPU at `proc_free` time (`wait_pid` spun on `on_cpu == 0`).

Pre-P3-Db, this freed only the L0 page (sub-tables would have leaked once demand paging populated them). Since no path populated sub-tables before P3-Db landed, the v1.0 P3-Bcb shortcut was sound at that point. P3-Db ships the walker in advance of P3-Dc's demand-paging fault handler so the lifecycle is correct from the moment PTE installation arrives.

## Implementation

### `arch/arm64/mmu.c`

```c
paddr_t proc_pgtable_create(void) {
    struct page *l0_pg = alloc_pages(0, KP_ZERO);
    if (!l0_pg) return 0;
    return page_to_pa(l0_pg);
}

static void l3_walk_and_free(paddr_t l3_pa) {
    struct page *l3_pg = pa_to_page(l3_pa);
    free_pages(l3_pg, 0);
}

static void l2_walk_and_free(paddr_t l2_pa) {
    u64 *l2 = (u64 *)pa_to_kva(l2_pa);
    for (u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
        u64 e = l2[i];
        if (!(e & PTE_VALID)) continue;
        if (!(e & PTE_TYPE_TABLE)) continue;     // 2-MiB block leaf
        l3_walk_and_free(e & ~0xFFFull);
    }
    free_pages(pa_to_page(l2_pa), 0);
}

static void l1_walk_and_free(paddr_t l1_pa) { /* same structure as l2_walk_and_free */ }

void proc_pgtable_destroy(paddr_t root) {
    if (root == 0) return;
    u64 *l0 = (u64 *)pa_to_kva(root);
    for (u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
        u64 e = l0[i];
        if (!(e & PTE_VALID)) continue;
        if (!(e & PTE_TYPE_TABLE)) continue;
        l1_walk_and_free(e & ~0xFFFull);
    }
    free_pages(pa_to_page(root), 0);
}
```

The walker is straightforward sorted-list-style recursion. `pa_to_kva` is the direct-map cast; the walker reads PTE bits via the kernel direct-map alias of the table page, never via TTBR0.

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

- **`proc.ttbr0_swap_smoke`**: Two `rfork`'d children record their live `TTBR0_EL1`; verify each equals `(asid << 48) | pgtable_root`. Implicitly exercises the per-Proc swap from `cpu_switch_context`.

- **`proc.pgtable_destroy_walk_releases_subtables`** (P3-Db): Manually install a 3-deep sub-table chain (L1 → L2 → L3) under the Proc's L0; drop the Proc; verify all 4 page-table pages return to buddy (the round-trip `phys_free_pages()` check). Pre-P3-Db's L0-only free would leak L1+L2+L3 → free count short by 3. Closes trip-hazard #116.

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

- **Implemented at P3-Bcb (`d256804`)**: pgtable_create / pgtable_destroy (L0-only) / proc_alloc + proc_free integration / 2 tests.
- **Implemented at P3-Bdb (`fd40047`)**: cpu_switch_context TTBR0 swap, struct Context growth (112→120 bytes adding `u64 ttbr0`), `mmu_kernel_ttbr0_pa()` accessor for kproc threads, ctx.ttbr0 wiring in thread_create_internal / thread_init / thread_init_per_cpu_idle, `proc.ttbr0_swap_smoke` test.
- **Implemented at P3-Db**: recursive `proc_pgtable_destroy` walk (L0 → L1 → L2 → L3 freeing every reachable sub-table). New test `proc.pgtable_destroy_walk_releases_subtables`. Closes trip-hazard #116.
- **Stubbed**: PTE installation. P3-Dc adds the user-mode page-fault dispatcher → `vma_lookup` → page allocate → PTE install (which is what populates the sub-tables the new walker is wired to free).
- **Implicit**: kproc kernel-only TTBR0 (P3-Be) is satisfied by the `mmu_kernel_ttbr0_pa()` defaulting at thread_init — kthread + per-CPU idle threads carry the kernel-only TTBR0 root from their creation. A separate P3-Be chunk that adds a dedicated "degenerate" TTBR0 page table is unnecessary.

Commit landing points: `d256804` (P3-Bcb), `fd40047` (P3-Bdb), `f5585a6` (P3-Db walker).

## TTBR0 swap (P3-Bdb)

`cpu_switch_context` (asm in `arch/arm64/context.S`) saves+loads TTBR0_EL1 alongside the other registers. The save reads the live TTBR0_EL1 into `prev->ctx.ttbr0`; the load writes `next->ctx.ttbr0` into TTBR0_EL1 followed by an ISB (the ISB serializes the TTBR0 change so subsequent translations see the new value).

Encoding: `ctx.ttbr0 = (ASID << 48) | pgtable_root_PA`. The ARM v8 TTBR0_EL1 layout has BADDR in bits 47:1 (bit 0 ignored — L0 tables are page-aligned), and ASID in bits 63:48 (when TCR.A1=0; default). For non-kproc threads, `ctx.ttbr0 = ((u64)proc->asid << 48) | proc->pgtable_root`. For kproc threads (kthread + per-CPU idle), `ctx.ttbr0 = mmu_kernel_ttbr0_pa()` (the PA of the static `l0_ttbr0` array, with implicit ASID 0).

**No TLB flush is needed at the swap.** Per-Proc ASIDs (1..255) tag user-mapping TLB entries; cross-Proc TLB entries don't collide. Kernel mappings via TTBR1 are global (nG=0; ASID-agnostic) and unaffected. ASID reuse (Proc P1 frees ASID X; Proc P2 allocates X) is safe because `asid_free` issues `tlbi aside1is, X` BEFORE returning the slot to the free-list (per P3-Ba; trip-hazard #107).

### Defense-in-depth: kthread + per-CPU idle

`thread_init` (kthread) and `thread_init_per_cpu_idle` (secondary idle threads) pre-populate `ctx.ttbr0 = mmu_kernel_ttbr0_pa()`. Without this, KP_ZERO would leave `ctx.ttbr0 = 0`, and a peer CPU steal (or a future code path that switches INTO kthread before kthread has switched-out once) would load TTBR0_EL1 = 0 → MMU walks PA 0 for any low-VA dereference → fault. Pre-populating ensures any switch into a kproc thread gets a sane (kernel-only) TTBR0.

### Capability cross-reference

The TTBR0 swap mechanism is the runtime enforcement of ARCH §28 I-1 (process isolation: namespace operations in process A don't affect process B). At v1.0 P3-Bdb the page tables are still empty (no user mappings until P3-D adds VMA + page-fault), but the ASID-tagged TLB + per-Proc TTBR0 root is the foundation. Phase 5+ extends to capability-typed kernel addressing per NOVEL §3.9 Contract D — the cpu_switch_context swap surface stays roughly identical; only the encoding becomes capability-derive.

## Known caveats / footguns

1. **kproc has `pgtable_root = 0` and `asid = 0`** — proc_free's gates (`!= 0` checks) prevent passing 0 to `proc_pgtable_destroy` / `asid_free`. Future code that adds new cleanup paths MUST include the same gates or use the existing helpers.

2. **`asid_free(0)` extincts** — ASID 0 is kernel-reserved. Bypassing the gate (e.g., a future bulk-cleanup path that doesn't check `asid != 0`) would extinct the kernel.

3. **`proc_pgtable_destroy` at P3-Bcb only frees L0** — when P3-D adds VMA + page-fault → sub-table allocation, this function MUST be extended to walk + free L1/L2/L3. Otherwise sub-tables leak. Trip-hazard #116.

4. **OOM order in proc_alloc**: handle_table_alloc → pgtable_create → asid_alloc. Reordering changes the rollback shape. asid_alloc has no rollback path (extincts on exhaustion); pgtable_create rolls back via proc_free.

5. **L0 alignment**: `proc_pgtable_create` returns a PA from buddy at order=0 (single page, page-aligned). The hardware requires the L0 PA to be page-aligned; this is structurally guaranteed.

## Naming rationale

`proc_pgtable_*` follows the standard `<subsystem>_<verb>_<noun>` form. No thematic name proposed at P3-Bcb (pgtable is the standard term; clarity wins). A future v1.x rename to something like `proc_addrspace_*` could reflect the broader "address space" concept (which would include VMA tree + capability bitmask), but that's a P3-D decision.
