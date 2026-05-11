# 39 — Hardware handles + capability gating (P4-Ib + P4-Ic5b1b)

## Purpose

P4-Ib lifted the kernel from "hw kobj kinds exist in the type enum" (P2-Fc) to "userspace drivers can create + own `KObj_MMIO` and `KObj_IRQ` handles through syscalls, with PA / INTID exclusivity enforced at the kernel layer, gated by a per-Proc capability". P4-Ic5b1b extends the same substrate to `KObj_DMA`: kernel-allocated contiguous pinned page chunks for DMA descriptor tables, ring buffers, and bounce buffers. Together they are the substrate the first Rust userspace driver (P4-Ic5b2 — virtio-blk) builds atop.

Three load-bearing invariants pinned at this layer, all proven in `specs/handles.tla` (extended at P4-Ib):

- **HwResourceExclusive**: at most one alive handle per hw kobj across all procs (no two drivers claim the same MMIO range / INTID).
- **NoHwDup**: `handle_dup` of a hardware-kind handle is rejected (drivers hold exactly one origin handle per resource).
- **HwHandleImpliesCap**: every hw-handle holder has `CAP_HW_CREATE` in `proc->caps` (capability-gated creation).

Plus three pre-existing invariants the chunk preserves:

- I-5 (HwHandlesAtOrigin) — hw handles non-transferable across procs.
- I-6 (RightsCeiling) — rights monotonically reduce.
- I-2 (CapsCeiling) — capabilities monotonically reduce.

---

## Public API

### `<thylacine/caps.h>` — capability bitmask

```c
typedef u64 caps_t;

#define CAP_HW_CREATE   (1ull << 0)
#define CAP_ALL         (CAP_HW_CREATE)        // grows as caps land
#define CAP_NONE        0ull
```

`CAP_ALL` is the kproc-initial mask; `CAP_NONE` is the rfork'd-child default. The `_Static_assert(CAP_ALL == CAP_HW_CREATE)` catches drift when new CAP_* bits land (each addition requires bumping `CAP_ALL` so kproc inherits the new domain).

Reserved for Phase 5+ (one bit per domain): `CAP_NS_MOUNT`, `CAP_NS_BIND`, `CAP_SIGNAL_ANY`, `CAP_NET_RAW`, `CAP_TIME_SET`, `CAP_REBOOT`.

### `struct Proc.caps` — per-Proc capability mask

Added at P4-Ib; struct grew 120 → 128 bytes. `kproc` is initialized to `CAP_ALL` at `proc_init`; `rfork`'d children leave `caps = CAP_NONE` via `KP_ZERO`. Capability inheritance / grant via syscall is a Phase 5+ deliverable.

### `<thylacine/mmio_handle.h>` — KObj_MMIO lifecycle

```c
#define KOBJ_MMIO_MAGIC 0x4D4D494F0BADC0DEULL

struct KObj_MMIO {
    u64    magic;
    u64    pa;          // page-aligned physical address
    size_t size;        // page-aligned, > 0
    int    ref;         // refcount; starts at 1
};

void kobj_mmio_init(void);                          // boot bring-up

struct KObj_MMIO *kobj_mmio_create(u64 pa, size_t size);
void              kobj_mmio_ref(struct KObj_MMIO *);
void              kobj_mmio_unref(struct KObj_MMIO *);
void              kobj_mmio_destroy(struct KObj_MMIO *);

u64 kobj_mmio_total_created(void);   // diagnostic / leak counters
u64 kobj_mmio_live_count(void);
```

`kobj_mmio_create` is the structural enforcement of `HwResourceExclusive`: returns `NULL` if the requested `[pa, pa+size)` overlaps any currently-claimed range. The check is under a single spinlock acquire (`g_mmio_lock`); no TOCTOU between the overlap scan and the slot insert.

### `<thylacine/dma_handle.h>` — KObj_DMA lifecycle (P4-Ic5b1b)

```c
#define KOBJ_DMA_MAGIC 0x444D4100BADC0DEEULL
#define KOBJ_DMA_MAX_SIZE  (1ull * 1024 * 1024)   // 1 MiB

struct KObj_DMA {
    u64           magic;
    u64           pa;          // page-aligned, contiguous
    size_t        size;        // page-aligned, > 0, ≤ KOBJ_DMA_MAX_SIZE
    struct page  *pages;       // alloc_pages chunk (for free_pages)
    unsigned      order;       // buddy order
    int           ref;
};

void kobj_dma_init(void);                          // boot bring-up
struct KObj_DMA *kobj_dma_create(size_t size);     // alloc + pin
void             kobj_dma_ref(struct KObj_DMA *);
void             kobj_dma_unref(struct KObj_DMA *);
void             kobj_dma_destroy(struct KObj_DMA *);
u64              kobj_dma_total_created(void);
u64              kobj_dma_live_count(void);
```

`kobj_dma_create` is the structural enforcement of `HwResourceExclusive` for DMA: the buddy allocator returns a fresh contiguous page chunk per call, so distinct `KObj_DMA` cannot share PA. No global claim table is needed — the page allocator IS the claim layer. Distinct from `kobj_mmio_create` where userspace specifies the PA and overlap rejection runs against `g_mmio_claims`.

PA stability across handle lifetime: once `kobj_dma_create` returns, `k->pa` is never modified. No code path mutates it; the structural property (no migrator exists at v1.0) is the impl-side commitment, documented in `specs/SPEC-TO-CODE.md`.

### Syscall surface — `<thylacine/syscall.h>`

```c
enum {
    SYS_EXITS       = 0,
    SYS_PUTS        = 1,
    SYS_MMIO_CREATE = 2,   // arg: pa (x0), size (x1), rights (x2)
    SYS_IRQ_CREATE  = 3,   // arg: intid (x0), rights (x1)
    SYS_IRQ_WAIT    = 4,   // arg: handle (x0)
    SYS_MMIO_MAP    = 5,   // P4-Ic2: arg: handle (x0), vaddr (x1), prot (x2)
    SYS_DMA_CREATE  = 6,   // P4-Ic5b1b: arg: size (x0), rights (x1)
    SYS_DMA_MAP     = 7,   // P4-Ic5b1b: arg: handle (x0), vaddr (x1), prot (x2)
};
```

**`SYS_MMIO_MAP` (P4-Ic2)** — installs a user-VA mapping for a `KObj_MMIO` handle. Sequence:
1. Cap check (defense; spec invariant `HwHandleImpliesCap` already guarantees the cap is held since the handle exists in p->handles).
2. `handle_get` + verify kind=`KOBJ_MMIO` + `RIGHT_MAP` in rights.
3. Validate `prot` (non-zero; only `READ|WRITE` bits; no `EXEC` — MMIO PTEs aren't architecturally executable in a useful way per ARM ARM B2.7.2); bound by handle rights (`RIGHT_WRITE` required for `VMA_PROT_WRITE`, `RIGHT_READ` for `VMA_PROT_READ`).
4. `burrow_create_mmio(km)` — wraps the KObj_MMIO in a `BURROW_TYPE_MMIO` Burrow (refs the kobj for the Burrow's lifetime).
5. `burrow_map(p, b, vaddr, km->size, prot)` — installs the VMA via `vma_alloc` + `vma_insert`. PTEs install lazily on first access (demand-paging via `userland_demand_page`).
6. `burrow_unref(b)` — transfers ownership to the VMA's mapping ref (matches anon flow). On failure, releases the construction ref → `burrow_free_internal` → `kobj_mmio_unref` → claim released.

**`SYS_DMA_CREATE` (P4-Ic5b1b)** — allocates a contiguous pinned page chunk of `size` bytes (page-aligned, ≤ `KOBJ_DMA_MAX_SIZE`) and returns a `KOBJ_DMA` handle. Sequence:
1. Cap check: `proc->caps & CAP_HW_CREATE` (HwHandleImpliesCap).
2. Validate rights: non-zero, within `RIGHT_ALL`.
3. Validate size: `> 0`.
4. `kobj_dma_create(size)` — `alloc_pages(order, KP_ZERO)` for contiguous chunk + struct alloc.
5. `handle_alloc(p, KOBJ_DMA, rights, k)`.
6. On `handle_alloc` failure: `kobj_dma_unref(k)` releases the chunk back to buddy (rollback discipline mirrors MMIO).

The kernel chooses the PA — userspace cannot specify it. This eliminates the entire bug class of PA-collision with kernel-reserved ranges, IPS-out-of-bound PAs, etc., at the syscall surface (no `kobj_dma_reserve_kernel_ranges` analogue needed because no userspace input ever names a PA).

**`SYS_DMA_MAP` (P4-Ic5b1b)** — installs a user-VA mapping for a `KOBJ_DMA` handle. Returns the buffer's PA on success so the driver can embed it into device-visible descriptors. Sequence:
1. Cap check (defense-in-depth; `HwHandleImpliesCap`).
2. `handle_get` + verify kind=`KOBJ_DMA` + `RIGHT_MAP` in rights.
3. Validate `prot`: non-zero; only `READ|WRITE` bits; no `EXEC` (W^X — DMA pages carry data, not code); bound by handle rights; reject W-without-R (AArch64 has no W-only PTE encoding per R10 F155 close).
4. `burrow_create_dma(kd)` — wraps the `KObj_DMA` in a `BURROW_TYPE_DMA` Burrow (refs the kobj for the Burrow's lifetime).
5. `burrow_map(p, b, vaddr, kd->size, prot)` — installs the VMA via `vma_alloc` + `vma_insert`. PTEs install lazily on first access.
6. `burrow_unref(b)` — transfers ownership to the VMA. On failure, releases construction ref → `burrow_free_internal` → `kobj_dma_unref`.
7. Return `kd->pa` (non-negative; valid PA fits in 40 bits at v1.0).

The actual PTE install happens at first-access via `userland_demand_page` (`arch/arm64/fault.c`):

```c
switch (vma->burrow->type) {
case BURROW_TYPE_ANON:
    page_pa = page_to_pa(vma->burrow->pages) + offset;
    device_memory = false;        // MAIR_IDX_NORMAL_WB
    break;
case BURROW_TYPE_MMIO:
    page_pa = vma->burrow->pa + offset;
    device_memory = true;         // MAIR_IDX_DEVICE (nGnRnE)
    break;
case BURROW_TYPE_DMA:             // P4-Ic5b1b
    page_pa = vma->burrow->pa + offset;
    device_memory = false;        // MAIR_IDX_NORMAL_WB
    break;
}
mmu_install_user_pte(p->pgtable_root, p->asid, page_va, page_pa,
                     vma->prot, device_memory);
```

Device-memory PTE attrs (`MAIR_IDX_DEVICE = 0`, `MAIR_ATTR_DEVICE_nGnRnE = 0x00` per `arch/arm64/mmu.h`) ensure CPU doesn't gather / reorder / coalesce MMIO accesses. DMA buffers use Normal cacheable attrs — CPU + device both access via cacheable PTEs; QEMU virt's VirtIO transports are coherent so no explicit cache maintenance is needed. Phase 5+ real hardware that requires uncached DMA buffers may introduce a flag at create time.

Each cap-gated syscall checks `current_thread()->proc->caps & CAP_HW_CREATE` before any allocation; returns -1 (EPERM) on cap-missing. Rights validation rejects `rights == 0` or `rights & ~RIGHT_ALL`. INTID range bound is enforced by `intid_try_claim` inside `kobj_irq_create`.

### `<thyla/syscall.h>` (libt — userspace) — inline wrappers

```c
long t_mmio_create(unsigned long pa, unsigned long size, unsigned long rights);
long t_irq_create(unsigned long intid, unsigned long rights);
long t_irq_wait(long handle);
long t_mmio_map(long handle, unsigned long vaddr, unsigned long prot);    // P4-Ic2
long t_dma_create(unsigned long size, unsigned long rights);              // P4-Ic5b1b
long t_dma_map(long handle, unsigned long vaddr, unsigned long prot);     // P4-Ic5b1b

#define T_PROT_READ       (1u << 0)
#define T_PROT_WRITE      (1u << 1)
#define T_PROT_EXEC       (1u << 2)
```

`t_mmio_create` / `t_irq_create` / `t_dma_create`: non-negative handle index on success, -1 on error.
`t_irq_wait`: collapsed IRQ count (>=1) on success, -1 on error.
`t_mmio_map`: 0 on success, -1 on error.
`t_dma_map`: non-negative PA on success (always non-negative since PA fits in 40 bits), -1 on error.

---

## Implementation

### `kernel/mmio_handle.c` — `g_mmio_claims` table

```c
#define KOBJ_MMIO_MAX 32

struct mmio_claim {
    struct KObj_MMIO *owner;   // NULL = slot free
    u64               pa;
    size_t            size;
};

static struct mmio_claim g_mmio_claims[KOBJ_MMIO_MAX];
static spin_lock_t       g_mmio_lock = SPIN_LOCK_INIT;
```

Linear scan + insert under one lock acquire. Overlap formula: two ranges `[a, a+s_a)` and `[b, b+s_b)` overlap iff `a < b + s_b && b < a + s_a` (handles all partial / complete overlap; adjacency-but-not-overlap = `b == a + s_a` is permitted).

Adjacent (not overlapping) ranges are explicitly allowed — `test_mmio_handle_create_adjacent_ok` exercises `[A, A+SIZE)` + `[A+SIZE, A+2*SIZE)` both succeeding.

Static table sized at 32 claims at v1.0; Phase 5+ refactors to a growable RB-tree keyed by PA when the system has hundreds of live MMIO regions.

### `kernel/irqfwd.c` — INTID claim tracking

```c
static bool        g_intid_claimed[GIC_NUM_INTIDS];
static spin_lock_t g_intid_lock = SPIN_LOCK_INIT;

static bool intid_try_claim(u32 intid);   // claim if free
static void intid_release(u32 intid);     // unconditional release
```

Added because `gic_attach` silently overwrites any existing handler for the same INTID — without this claim layer, two callers of `kobj_irq_create(intid)` would both succeed (second's attach overwriting first's), violating `HwResourceExclusive`. The claim table is checked before any state mutation; if already claimed, `kobj_irq_create` returns `NULL`.

Released in `kobj_irq_free_internal` AFTER `gic_disable_irq` so no IRQ can fire on the released INTID's slot between release and the next create's attach.

### `kernel/handle.c` — hw rejection in `handle_dup`

```c
hidx_t handle_dup(struct Proc *p, hidx_t h, rights_t new_rights) {
    struct Handle *parent = handle_get(p, h);
    if (!parent) return -1;

    // P4-Ib NoHwDup: dup forbidden for hardware kinds.
    if (kobj_kind_is_hw(parent->kind)) return -1;

    // ... rights check, alloc, etc.
}
```

Covers all four hw kinds via `KOBJ_KIND_HW_MASK`. Extension to a new hw kind requires updating the mask (existing `_Static_assert` catches the mask change at compile time).

### `kernel/handle.c` — `handle_release_obj` cases

```c
case KOBJ_MMIO: kobj_mmio_unref((struct KObj_MMIO *)obj); break;
case KOBJ_IRQ:  kobj_irq_unref ((struct KObj_IRQ  *)obj); break;
```

Closing the last handle to an MMIO releases the PA-range claim; closing the last handle to an IRQ disables the GIC line + releases the INTID claim. `handle_acquire_obj` has the symmetric `kobj_mmio_ref` / `kobj_irq_ref` paths even though `handle_dup` of hw is rejected — defense in depth + forward-compatibility (future Phase 5+ paths might end up with multiple hw handle holders in edge cases like crash recovery).

### `kernel/syscall.c` — capability check + rollback

```c
static s64 sys_mmio_create_handler(u64 pa, u64 size, u64 rights) {
    struct Proc *p = current_thread()->proc;
    if (!p)                                          return -1;
    if ((p->caps & CAP_HW_CREATE) == 0)              return -1;  // HwHandleImpliesCap
    if (rights == 0 || (rights & ~(u64)RIGHT_ALL))   return -1;  // RightsCeiling

    struct KObj_MMIO *k = kobj_mmio_create(pa, (size_t)size);    // HwResourceExclusive
    if (!k)                                          return -1;

    hidx_t h = handle_alloc(p, KOBJ_MMIO, (rights_t)rights, k);
    if (h < 0) {
        kobj_mmio_unref(k);   // rollback: release the claim
        return -1;
    }
    return (s64)h;
}
```

Rollback discipline: if any post-create step fails, `kobj_mmio_unref` releases the PA claim back to `g_mmio_claims` so a retry (or another driver) can re-claim. Same pattern in `sys_irq_create_handler`.

---

## Data structures

### `struct Proc` size bump

```
struct Proc — was 120 bytes (P3-Bcb baseline 112 + vmas 8)
            — now 128 bytes (P3-Bcb baseline 112 + vmas 8 + caps 8)
```

`_Static_assert(sizeof(struct Proc) == 128)` pins the new size. Every byte of bloat costs `SLUB_caches × cached_slabs × 64_slots`, so changes are deliberate.

### `g_mmio_claims[32]` — claim table

32 × 24 bytes = 768 bytes BSS. Linear scan worst-case 32 entries under lock; effectively constant-time at v1.0 scale.

### `g_intid_claimed[GIC_NUM_INTIDS=1020]` — 1020 bytes BSS

A bitmap form would save 1004 bytes; the boolean form keeps the code obvious. Negligible cost.

---

## State machines

### KObj_MMIO refcount lifecycle

```
   kobj_mmio_create
        ↓                                  
   ref=1; claim asserted
   ↓                       ↓ (last handle_close on this obj OR direct unref)
   handle_alloc/_ref       kobj_mmio_unref → ref=0 → kobj_mmio_free_internal
   ↑                                                  ↓
   ref++ (defense-only;                               claim released
   handle_dup rejected at P4-Ib)                      magic clobbered
                                                      kfree(k)
```

Lock discipline: `g_mmio_lock` taken inside `kobj_mmio_create` (overlap scan + slot insert) + `kobj_mmio_free_internal` (slot release). No nested lock takes; no callbacks-under-lock.

### KObj_IRQ INTID claim lifecycle

```
   kobj_irq_create(intid)
        ↓
   intid_try_claim → kmalloc → gic_attach → gic_enable_irq → return k

   kobj_irq_unref → ref=0 → kobj_irq_free_internal:
        ↓
   gic_disable_irq → gic_attach(NULL) [no-op, see below] → intid_release
        ↓
   magic clobber → kfree
```

The `gic_attach(intid, NULL, NULL)` call in `kobj_irq_free_internal` returns `false` because the GIC's `gic_attach` rejects NULL handlers — the slot retains its `kobj_irq_dispatch + arg=k` pointer. Stale-fire safety: the magic clobber before kfree makes `kobj_irq_dispatch`'s `k->magic != KOBJ_IRQ_MAGIC` check return early on any in-flight IRQ that races with the free.

---

## Spec cross-reference

| Spec action | Impl site |
|---|---|
| `HandleAlloc(p, k, granted)` (with `k ∈ HwKObjs` precondition `CapHwCreate ∈ proc_caps[p]`) | `sys_mmio_create_handler` / `sys_irq_create_handler` cap check + `handle_alloc` |
| `HandleDup(p, h, nr)` (with `h.kobj ∈ TxKObjs` precondition) | `handle_dup`'s `kobj_kind_is_hw(parent->kind) → return -1` |
| `HandleClose(p, h)` (with MMIO claim release) | `handle_close` → `handle_release_obj` → `kobj_mmio_unref`'s claim release |
| `ReduceCaps(p, lost)` (with `CapHwCreate ∈ lost ⇒ ¬∃ hw handle in p`) | **Forward-looking**: v1.0 has no cap-drop syscall; when it lands (Phase 5+), it MUST check `p` has no live hw handles before allowing the drop |

Buggy variants pinned by counter-examples (handles_buggy_*.cfg files):
- `BuggyDupElevate` → `RightsCeiling` violated
- `BuggyHwTransfer` → `HwHandlesAtOrigin` violated
- `BuggyDirectTransfer` → `OnlyTransferVia9P` violated
- `BuggyCapsElevate` → `CapsCeiling` violated
- `BuggyHwDup` (P4-Ib) → `NoHwDup` violated
- `BuggyHwOverlap` (P4-Ib) → `HwResourceExclusive` violated
- `BuggyHwCreateNoCap` (P4-Ib) → `HwHandleImpliesCap` violated
- `BuggyRforkElevate` (P4-Ic3) → `CapsCeiling` violated (dynamic ceiling — child grant exceeds parent's caps)

All 8 buggy cfgs produce expected counterexamples under TLC; correct cfg explores **11.7M distinct states** (depth 25, ~9 min on 8 cores) without violation. The pre-P4-Ic3 figure was 1.4M; the state space grew because `proc_ceiling` adds a state dimension and `RforkWithCaps` is now a reachable action.

---

## Tests

| Test | What |
|---|---|
| `caps.kproc_has_all` | `kproc->caps == CAP_ALL` post-`proc_init` |
| `caps.kproc_has_hw_create` | Specifically `kproc->caps & CAP_HW_CREATE != 0` (catches accidental CAP_ALL refactor that drops the bit) |
| `caps.rfork_child_has_none` | `rfork(RFPROC, ...)` child observes `proc->caps == CAP_NONE` |
| `caps.rfork_with_caps_grants_subset` | P4-Ic3: `rfork_with_caps(CAP_HW_CREATE)` from kproc grants exactly `CAP_HW_CREATE` |
| `caps.rfork_with_caps_clamps_to_parent` | P4-Ic3: a zero-cap parent forking with `caps_mask=CAP_HW_CREATE` yields child with `CAP_NONE` (AND-with-parent clamp) |
| `caps.rfork_with_caps_zero_mask` | P4-Ic3: `caps_mask=CAP_NONE` is equivalent to plain `rfork` |
| `userspace.mmio_probe_rfork_with_caps` | P4-Ic5a: end-to-end userspace SVC path. kproc rforks `/mmio-probe` via `rfork_with_caps(CAP_HW_CREATE)`; binary calls `t_mmio_create` + `t_mmio_map` for PL031 RTC at PA 0x09010000; demand-page MMIO dispatch installs device-memory PTE on first access; verifies live `PeriphID0 == 0x31`; exits 0. Closes the bulk of deferred R10 F159 (SVC-path test coverage for SYS_MMIO_CREATE + SYS_MMIO_MAP). |
| `userspace.irq_probe_rfork_with_caps` | P4-Ic5-IRQ-probe: companion to mmio-probe; closes the IRQ side of R10 F159. kproc rforks `/irq-probe` via `rfork_with_caps(CAP_HW_CREATE)`; kernel test pre-pends SPI 96 via `gic_set_pending_spi(96)` before the spawn; child calls `t_irq_create(96, T_RIGHT_SIGNAL)` + `t_irq_wait(handle)`; the pre-pended IRQ delivers on `gic_enable_irq` in `kobj_irq_create`, increments `pending_count`, and the wait returns count >= 1 without blocking. Exits 0. Closes the remaining R10 F159 (SVC-path test coverage for SYS_IRQ_CREATE + SYS_IRQ_WAIT). |
| `mmio_handle.create_basic` | round-trip create + unref |
| `mmio_handle.create_misaligned_rejected` | pa or size not page-aligned → NULL |
| `mmio_handle.create_zero_size_rejected` | size == 0 → NULL |
| `mmio_handle.create_overflow_rejected` | pa + size overflows u64 → NULL |
| `mmio_handle.create_overlap_rejected` | exact + partial overlap both rejected (HwResourceExclusive) |
| `mmio_handle.create_adjacent_ok` | adjacent non-overlapping ranges both succeed |
| `mmio_handle.create_unref_releases_slot` | re-claim same PA after unref succeeds |
| `mmio_handle.live_count_round_trip` | `total_created` + `live_count` instrumentation works |
| `mmio_handle.kernel_reserved_rejected` | R10 F154 regression: GIC dist/redist + PL011 + ECAM ranges in `g_mmio_claims[i].owner == KOBJ_MMIO_KERNEL_RESERVED` reject `kobj_mmio_create`. virtio-mmio is NOT in the list post-P4-Ic5b1a refinement. |
| `mmio_handle.virtio_mmio_claimable` | P4-Ic5b1a refinement: virtio-mmio PA ranges (0x0a000000 first slot, 0x0a003000 last slot on QEMU virt) are claimable from `kobj_mmio_create` — the R10 F154 reservation was over-broad and is relaxed for virtio-mmio (kernel doesn't actively use these post-`virtio_init` probe). |
| `handle_hw.mmio_dup_rejected` | `handle_dup` on KOBJ_MMIO returns -1 (NoHwDup) |
| `handle_hw.irq_dup_rejected` | `handle_dup` on KOBJ_IRQ returns -1 (NoHwDup) |
| `handle_hw.mmio_close_releases_claim` | `handle_close` → claim returns to pool |
| `handle_hw.irq_close_releases_intid` | `handle_close` → INTID returns to pool |
| `dma_handle.create_basic` | P4-Ic5b1b: round-trip create + verify pa/size/ref + page_to_pa(pages)==pa |
| `dma_handle.zero_size_rejected` | P4-Ic5b1b: `kobj_dma_create(0)` → NULL |
| `dma_handle.oversize_rejected` | P4-Ic5b1b: `kobj_dma_create(KOBJ_DMA_MAX_SIZE + PAGE_SIZE)` → NULL |
| `dma_handle.round_up_to_page` | P4-Ic5b1b: sub-page request rounds up to PAGE_SIZE |
| `dma_handle.distinct_pa` | P4-Ic5b1b: two creates yield distinct PAs (HwResourceExclusive structural) |
| `dma_handle.unref_releases_chunk` | P4-Ic5b1b: live_count returns to baseline on final unref |
| `dma_handle.zero_init` | P4-Ic5b1b: KP_ZERO at alloc — buffer reads all-zero via direct map |
| `burrow_dma.create_basic` | P4-Ic5b1b: type=BURROW_TYPE_DMA + pa+size set correctly + handle_count=1 |
| `burrow_dma.create_null_rejected` | P4-Ic5b1b: NULL kobj_dma → NULL Burrow |
| `burrow_dma.holds_kobj_ref` | P4-Ic5b1b: Burrow's ref keeps KObj_DMA alive past caller's unref |
| `burrow_dma.lifecycle_round_trip` | P4-Ic5b1b: symmetric of above — caller-first then Burrow-first both free at boundary |
| `dma_map.install_vma` | P4-Ic5b1b: burrow_create_dma + burrow_map installs VMA reachable via vma_lookup |
| `dma_map.proc_free_releases_kobj` | P4-Ic5b1b: proc_free → VMA → Burrow → KObj_DMA unref cascade |

All 15 P4-Ib tests + 13 new P4-Ic5b1b tests PASS at P4-Ic5b1b tip; 213 → 226 total in-kernel tests. After P4-Ic5a + P4-Ic5-IRQ-probe, two userspace-SVC-path tests (`userspace.mmio_probe_rfork_with_caps` + `userspace.irq_probe_rfork_with_caps`) lock in end-to-end SVC coverage for all four MMIO/IRQ syscalls (`SYS_MMIO_CREATE`, `SYS_MMIO_MAP`, `SYS_IRQ_CREATE`, `SYS_IRQ_WAIT`). DMA-side SVC-path coverage will land naturally at P4-Ic5b2 when the virtio-blk driver crate exercises the path.

---

## Error paths

| Path | Failure | Result |
|---|---|---|
| `sys_mmio_create_handler` | `proc->caps & CAP_HW_CREATE == 0` | -1 (EPERM-equivalent) |
| `sys_mmio_create_handler` | `rights == 0` or `rights & ~RIGHT_ALL` | -1 |
| `sys_mmio_create_handler` | `kobj_mmio_create` returns NULL (overlap / OOM / table-full / misalign) | -1 |
| `sys_mmio_create_handler` | `handle_alloc` returns -1 (table-full) | `kobj_mmio_unref` → -1 (claim released) |
| `sys_irq_create_handler` | as above; INTID > u32 range also -1 | -1 |
| `sys_irq_wait_handler` | bad handle / wrong kind / missing `RIGHT_SIGNAL` | -1 |

---

## Performance characteristics

| Operation | Cost |
|---|---|
| `kobj_mmio_create` | O(KOBJ_MMIO_MAX) under one lock; ~32 entries → 200 ns worst case |
| `kobj_mmio_unref` (last) | O(KOBJ_MMIO_MAX) under one lock for slot lookup |
| `kobj_irq_create` | O(1) under one lock + gic_attach + gic_enable_irq |
| `handle_dup` of hw | O(1) — early `return -1` before any state mutation |
| `SYS_MMIO_CREATE` syscall total | < 1 µs at v1.0 scale (mostly the SVC + context save/restore) |

---

## Status

| Component | State |
|---|---|
| `caps.h` + `struct Proc.caps` | **Landed (P4-Ib)** |
| kproc CAP_ALL init | **Landed (P4-Ib)** in `proc_init` |
| rfork inherits CAP_NONE | **Landed (P4-Ib)** via `KP_ZERO` (no inheritance impl) |
| `rfork_with_caps(flags, entry, arg, mask)` kernel-internal grant | **Landed (P4-Ic3)** — `kernel/proc.c::rfork_internal` ANDs `parent->caps` with `mask`; `rfork` delegates with `CAP_NONE` |
| `mmio_handle.h/c` + `g_mmio_claims` | **Landed (P4-Ib)** |
| `kernel/irqfwd.c` INTID claim | **Landed (P4-Ib)** — `g_intid_claimed` + `intid_try_claim`/`intid_release` |
| `handle.c` hw-dup rejection | **Landed (P4-Ib)** |
| `handle.c` KOBJ_MMIO + KOBJ_IRQ release wiring | **Landed (P4-Ib)** |
| Spec `handles.tla` extension | **Landed (P4-Ib)** — 3 invariants + 3 buggy actions + 3 cfg variants |
| Spec `handles.tla::RforkWithCaps` action + `proc_ceiling` state var | **Landed (P4-Ic3)** — strengthened `CapsCeiling` to dynamic ceiling; +1 buggy action (`BuggyRforkElevate`) + cfg variant |
| Syscall handlers (MMIO_CREATE, IRQ_CREATE, IRQ_WAIT) | **Landed (P4-Ib)** |
| `libt` syscall wrappers | **Landed (P4-Ib + extended P4-Ic2 with `t_mmio_map` + extended P4-Ic5b1b with `t_dma_create` + `t_dma_map`)** |
| `SYS_MMIO_MAP` syscall + `kobj_mmio_map_into_user` semantics | **Landed (P4-Ic2)** — `BURROW_TYPE_MMIO` (P4-Ic1) + `userland_demand_page` dispatch (P4-Ic2) + device-memory PTE attrs (P4-Ic2) |
| `KObj_DMA` lifecycle (`dma_handle.h/c`) | **Landed (P4-Ic5b1b)** — buddy-allocated contiguous chunks; refcount lifecycle mirrors `kobj_mmio`; PA-stability structural via no-migrator |
| `SYS_DMA_CREATE` + `SYS_DMA_MAP` syscalls | **Landed (P4-Ic5b1b)** — kernel-allocated PA; `t_dma_map` returns PA for descriptor-embed |
| `BURROW_TYPE_DMA` + `userland_demand_page` DMA dispatch | **Landed (P4-Ic5b1b)** — Normal cacheable PTE attrs (CPU+device coherent on QEMU virt) |
| `handle.c` KOBJ_DMA release wiring | **Landed (P4-Ic5b1b)** — `handle_release_obj` + `handle_acquire_obj` cases call `kobj_dma_unref` / `kobj_dma_ref` |
| Userspace SVC-path tests (all four hw-handle syscalls) | **Landed (P4-Ic5a + P4-Ic5-IRQ-probe)** — `mmio-probe` (PL031 RTC live read) + `irq-probe` (pre-pended SPI 96 wait). Closes R10 F159 in full. DMA-side SVC-path lands at P4-Ic5b2. |
| virtio-mmio reservation policy | **Refined (P4-Ic5b1a)** — virtio-mmio slots REMOVED from `kobj_mmio_reserve_kernel_ranges` (kernel doesn't actively use them post-`virtio_init` probe). GIC/PL011/ECAM reservations stay (kernel-active). Boot reports 4 reserved ranges (was 8). Unblocks P4-Ic5b2 driver-process MMIO claim. |
| `mmu_install_user_pte(device_memory)` flag | **Landed (P4-Ic2)** |
| Cap-grant syscall (parent → child userspace-callable) | **Phase 5+** — kernel-internal grant path lands at P4-Ic3 via `rfork_with_caps`; userspace surface deferred |
| Cap-drop syscall (with hw-handle interlock) | **Phase 5+** |
| 9P-aware `handle_transfer_via_9p` for KOBJ_SPOOR | **Phase 4** (separate sub-chunk) |

---

## Known caveats / footguns

1. **`kobj_mmio_map_into_user` is NOT in P4-Ib**. The claim ticket exists at P4-Ib (the handle proves PA exclusivity) but installing PTEs into the user address space is a P4-Ic concern. Currently no way to actually access the MMIO from userspace; the spec extension validates the OWNERSHIP semantics independently of the MAPPING.

2. **`gic_attach(intid, NULL, NULL)` is a no-op** because the GIC `gic_attach` rejects NULL handlers. Stale `g_handlers[intid]` retains the pre-free `kobj_irq_dispatch + arg=k` pointer. Stale-fire safety relies on the magic clobber + IRQ-already-disabled discipline; future cleanup would be `gic_detach(intid)` as a proper API.

3. **`ReduceCaps`-of-CAP_HW_CREATE-while-holding-hw-handle is undefined at v1.0**. The spec models this as forbidden; the impl can't check because there's no cap-drop syscall yet. When the syscall lands (Phase 5+), it MUST enumerate `p->handles` and reject if any has `kobj_kind_is_hw(kind)`.

4. **Capability inheritance at `rfork`**. v1.0 children start with `CAP_NONE`. Production drivers (P4-Ic+) receive `CAP_HW_CREATE` via the kernel-internal `rfork_with_caps(flags, entry, arg, caps_mask)` primitive (P4-Ic3) — the child's caps are set to `parent->caps & caps_mask` so a caller cannot grant beyond its own ceiling. Userspace cap-grant syscall surface is Phase 5+.

5. **`KOBJ_INTERRUPT` is in the hw mask but lacks implementation**. `handle_dup` rejects it (correct); `handle_release_obj` has a no-op case for it (no impl to call). Adding the impl is a future sub-chunk (Phase 5+ eventfd-like notification); the spec already covers it as a HwKObj. `KOBJ_DMA` landed at P4-Ic5b1b.

6. **`g_mmio_claims` is global, not per-Proc**. The exclusivity invariant is system-wide. A future enhancement could partition claims per-Proc for faster lookups when many procs each hold few claims; v1.0's 32-entry linear scan is fine.

7. **`SYS_IRQ_WAIT` requires `RIGHT_SIGNAL`** in the handle's rights, but `SYS_IRQ_CREATE` doesn't validate that `rights` includes `RIGHT_SIGNAL`. A caller could create a KObj_IRQ handle without SIGNAL, then call wait, and get -1. Slightly surprising; documented here. The fix is one-line at create-time but adds policy not present in the spec; left as-is per the principle that the spec is authoritative.

---

## Naming rationale

`KObj_MMIO` mirrors `KObj_IRQ` (P4-G) in capitalization and prefix. `kobj_mmio_*` for the API verbs matches `kobj_irq_*`. `g_mmio_claims` is straightforward; could be thematically renamed (e.g., `g_territory_claims`) but the resource-tracking domain is generic enough that the descriptive name wins. `KObj_DMA` (P4-Ic5b1b) extends the same pattern.

Held proposals (none load-bearing):
- `CAP_HW_CREATE` could be `CAP_HW_GRANT` or `CAP_DRIVER`; the verb form `CREATE` matches the syscall it gates. Holding.
