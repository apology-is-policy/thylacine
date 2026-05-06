# 19 — Handle table (P2-F)

The kernel handle table — typed unforgeable tokens that name kernel objects a process is allowed to access. Per `ARCHITECTURE.md §18`. v1.0 P2-Fc lands the kernel-internal API; the syscall surface lands at Phase 5+ (the v1.0 kernel only has internal callers — tests + the eventual driver-startup flow at Phase 3 + the 9P client at Phase 4).

---

## Purpose

Eight typed kobj kinds (Process / Thread / VMO / Chan / MMIO / IRQ / DMA / Interrupt) form Thylacine's kernel-object universe. Each is named by a `struct Handle` (kobj_kind + rights + obj pointer); each Proc owns a fixed-size `struct HandleTable` of these. Handles cannot be forged — a Proc receives them only via kernel grant or 9P transfer (Phase 4).

Key invariants (proven in `specs/handles.tla`):

- **Rights monotonic reduction (ARCH §28 I-6)**: `dup` and (Phase 4) `transfer-via-9P` reject elevation. `RightsCeiling` invariant: every handle's rights are a subset of `origin_rights[its kobj]`.
- **Hardware non-transferability (ARCH §28 I-5)**: `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA`, `KObj_Interrupt` cannot transfer. The 9P transfer codepath has no case for these kinds; `_Static_assert(KOBJ_KIND_COUNT == 9)` pins the enum so the type partition can't drift silently.
- **Transfer only via 9P (ARCH §28 I-4)**: no syscall exists for direct cross-Proc transfer. The only public path is `handle_transfer_via_9p` (Phase 4).
- **Capability monotonic reduction (ARCH §28 I-2)**: per-Proc coarse capabilities only reduce. Forward-looking; `rfork`'s capability mask uses bitwise AND (Phase 5+ syscall surface).

---

## Public API — `<thylacine/handle.h>`

```c
#define HANDLE_MAGIC 0x48414e444c45BAD2ULL   // 'HANDLE' | 0xBAD2

enum kobj_kind {
    KOBJ_INVALID    = 0,
    KOBJ_PROCESS    = 1,
    KOBJ_THREAD     = 2,
    KOBJ_VMO        = 3,
    KOBJ_CHAN       = 4,
    KOBJ_MMIO       = 5,
    KOBJ_IRQ        = 6,
    KOBJ_DMA        = 7,
    KOBJ_INTERRUPT  = 8,
    KOBJ_KIND_COUNT = 9,
};
_Static_assert(KOBJ_KIND_COUNT == 9, ...);

#define KOBJ_KIND_TRANSFERABLE_MASK \
    ((1u << KOBJ_PROCESS) | (1u << KOBJ_THREAD) |
     (1u << KOBJ_VMO)     | (1u << KOBJ_CHAN))
#define KOBJ_KIND_HW_MASK \
    ((1u << KOBJ_MMIO) | (1u << KOBJ_IRQ) |
     (1u << KOBJ_DMA)  | (1u << KOBJ_INTERRUPT))
_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_HW_MASK) == 0, ...);

typedef u32 rights_t;
#define RIGHT_NONE      0u
#define RIGHT_READ      (1u << 0)
#define RIGHT_WRITE     (1u << 1)
#define RIGHT_MAP       (1u << 2)
#define RIGHT_TRANSFER  (1u << 3)
#define RIGHT_DMA       (1u << 4)
#define RIGHT_SIGNAL    (1u << 5)
#define RIGHT_ALL       0x3fu

struct Handle {
    u64               magic;       // HANDLE_MAGIC; 0 means free
    enum kobj_kind    kind;
    rights_t          rights;
    void             *obj;         // pointer to underlying kernel object
};
_Static_assert(sizeof(struct Handle) == 24, ...);

#define PROC_HANDLE_MAX 64
struct HandleTable { struct Handle slots[PROC_HANDLE_MAX]; };
_Static_assert(sizeof(struct HandleTable) == 24 * PROC_HANDLE_MAX, ...);

typedef int hidx_t;                              // -1 means invalid

void                handle_init(void);            // bootstraps SLUB cache
struct HandleTable *handle_table_alloc(void);
void                handle_table_free(struct HandleTable *);
int                 handle_table_count(const struct HandleTable *);

hidx_t handle_alloc(struct Proc *, enum kobj_kind, rights_t, void *obj);
int    handle_close(struct Proc *, hidx_t);
struct Handle *handle_get(struct Proc *, hidx_t);
hidx_t handle_dup(struct Proc *, hidx_t, rights_t new_rights);

bool kobj_kind_is_transferable(enum kobj_kind);
bool kobj_kind_is_hw(enum kobj_kind);

u64 handle_total_allocated(void);
u64 handle_total_freed(void);
```

### `handle_alloc(p, kind, rights, obj)` — return semantics

| Return | Meaning |
|---|---|
| `>= 0` | success; slot index in p's table |
| `-1`   | rejection: invalid kind / empty rights / out-of-range rights / table full |

### `handle_dup(p, h, new_rights)` — return semantics

| Return | Meaning |
|---|---|
| `>= 0` | success; new slot index |
| `-1`   | rejection: h out-of-range or empty / rights elevation (`new_rights ⊄ parent.rights`) / new_rights == 0 / table full |

`handle_dup`'s subset check is the runtime enforcement of the spec's `RightsCeiling` invariant. The check is `(new_rights & parent->rights) == new_rights` — a bit elevation produces `≠`, returning -1. This is exactly the bug class the spec's `BuggyDupElevate` action models.

### `handle_close(p, h)` — return semantics

| Return | Meaning |
|---|---|
| `0`  | slot freed |
| `-1` | h out-of-range or already-empty (double-close defense) |

---

## Implementation

`kernel/handle.c` (~140 LOC).

### HandleTable lifecycle

- `handle_init`: SLUB cache + idempotency check. Called from `boot_main` AFTER `pgrp_init` and BEFORE `proc_init` (proc_init allocates a HandleTable for kproc).
- `handle_table_alloc`: SLUB-allocate via `kmem_cache_alloc(KP_ZERO)`. Every slot's magic = 0 (free) at allocation. Returns NULL on OOM.
- `handle_table_free`: closes any in-use slots (zeros magic, kind, rights, obj) before `kmem_cache_free`. NULL-safe.

### Slot layout

Each `struct Handle` is 24 bytes:
- `u64 magic` at offset 0 (free iff magic == 0)
- `enum kobj_kind kind` at offset 8 (4 bytes)
- `rights_t rights` at offset 12 (4 bytes)
- `void *obj` at offset 16 (8 bytes)

`magic` at offset 0 means: SLUB's `*(void **)obj = freelist` write at `kmem_cache_free` clobbers the first 8 bytes of the table; reusing the table later would naturally reset most slots' magic to non-HANDLE_MAGIC. The defense isn't quite as load-bearing as the per-Proc magic (since the table is a separate slab), but the offset-0 placement preserves the pattern.

### Type classifiers

```c
bool kobj_kind_is_transferable(enum kobj_kind k) {
    if (k <= KOBJ_INVALID || k >= KOBJ_KIND_COUNT) return false;
    return ((1u << (unsigned)k) & KOBJ_KIND_TRANSFERABLE_MASK) != 0;
}

bool kobj_kind_is_hw(enum kobj_kind k) {
    if (k <= KOBJ_INVALID || k >= KOBJ_KIND_COUNT) return false;
    return ((1u << (unsigned)k) & KOBJ_KIND_HW_MASK) != 0;
}
```

The disjoint `_Static_assert` on the masks is the compile-time guarantee that no kind is in both partitions — adding a new kind requires bumping `KOBJ_KIND_COUNT` AND extending one of the masks; if both, the static assert fires.

### Rights subset check

```c
if ((new_rights & parent->rights) != new_rights) return -1;
```

Standard subset test: `A ⊆ B` iff `A & B == A`. Maps to the spec's `new_rights \subseteq h.rights` precondition for HandleDup.

### Integration with Proc

`kernel/proc.c` (P2-Fc updates):
- `proc_init`: kproc gets `kproc->handles = handle_table_alloc()` (panics on OOM — boot can't continue without kproc).
- `proc_alloc`: each new Proc gets its own table; on OOM, rolls back the proc allocation (transitions to ZOMBIE so `proc_free`'s lifecycle gate passes).
- `proc_free`: calls `handle_table_free(p->handles)` before SLUB-free.

`struct Proc` grew **88 → 96 bytes** (added `struct HandleTable *handles`).

### Bootstrap order

```
slub_init → pgrp_init → handle_init → proc_init → thread_init → sched_init
```

`handle_init` between `pgrp_init` and `proc_init`: the slab cache must exist before kproc's table is allocated.

---

## Spec cross-reference

`specs/handles.tla` at P2-Fa:
- 11 actions: `Init`, `HandleAlloc`, `HandleClose`, `HandleDup`, `BuggyDupElevate`, `OpenSession`, `CloseSession`, `HandleTransferVia9P`, `BuggyHwTransfer`, `BuggyDirectTransfer`, `ReduceCaps`, `BuggyCapsElevate`.
- 5 invariants: `TypeOk`, `RightsCeiling`, `HwHandlesAtOrigin`, `OnlyTransferVia9P`, `CapsCeiling`.
- 5 configs: `handles.cfg` (clean; 6,055,072 distinct states / depth 26 / ~4 min) + 4 buggy variants (each producing a counterexample at depth 4-5).

Mapping (canonical at `specs/SPEC-TO-CODE.md`):

| Spec action | P2-Fc impl site | Notes |
|---|---|---|
| `Init` | `handle_init` | SLUB cache init. |
| `HandleAlloc(p, k, granted)` | `handle_alloc(p, kind, rights, obj)` | Returns slot index or -1. |
| `HandleClose(p, h)` | `handle_close(p, h)` | Returns 0 or -1. |
| `HandleDup(p, h, nr)` | `handle_dup(p, h, new_rights)` | Subset check enforced. |
| `BuggyDupElevate` | (none — bug class statically prevented by subset check) | |
| `OpenSession / CloseSession` | (Phase 4: `9p_attach / 9p_clunk`) | |
| `HandleTransferVia9P` | (Phase 4: `handle_transfer_via_9p`) | |
| `BuggyHwTransfer` | (none — `_Static_assert(KOBJ_KIND_COUNT == 9)` + omitted switch cases) | |
| `BuggyDirectTransfer` | (none — no direct-transfer syscall exists) | |
| `ReduceCaps` | (Phase 5+: `rfork` capability mask) | |
| `BuggyCapsElevate` | (none — `rfork` mask is bitwise AND) | |

| Spec invariant | Source enforcement |
|---|---|
| `RightsCeiling` | `handle_dup`'s `(new_rights & parent->rights) == new_rights` subset check before insert. |
| `HwHandlesAtOrigin` | `_Static_assert(KOBJ_KIND_COUNT == 9)` + the disjoint `_Static_assert((KOBJ_KIND_TRANSFERABLE_MASK & KOBJ_KIND_HW_MASK) == 0)`. Phase 4's `handle_transfer_via_9p` switch has no case for hw kinds. |
| `OnlyTransferVia9P` | No direct-transfer syscall exists; absence is the enforcement. |
| `CapsCeiling` | `rfork`'s mask is `parent->caps & mask` (Phase 5+); no codepath ORs in caps. |

---

## Tests

- `handles.alloc_close_smoke` — alloc / count / get / close round-trip + cumulative counters + invalid-arg rejection (KOBJ_INVALID, RIGHT_NONE, out-of-range rights, double-close, out-of-range close).
- `handles.rights_monotonic` — dup with subset succeeds; dup with elevated rights or disjoint rights fails (-1); dup with empty rights fails.
- `handles.dup_lifecycle` — dup; close parent; dup remains. Dup again; close child; original dup remains. Independent close ordering.
- `handles.full_table_oom` — alloc to PROC_HANDLE_MAX; verify all distinct slots + (-1) on overflow + slot reuse after partial close.
- `handles.kind_classifiers` — truth table: PROCESS/THREAD/VMO/CHAN are transferable; MMIO/IRQ/DMA/INTERRUPT are hw; KOBJ_INVALID is neither; out-of-range rejected by both classifiers (defensive).

---

## Known caveats / footguns

### `obj` may be NULL at v1.0

For kobj kinds whose underlying impl isn't yet integrated (KOBJ_VMO at P2-Fc — comes at P2-Fd; KOBJ_CHAN — Phase 4; KOBJ_MMIO/IRQ/DMA — Phase 3; KOBJ_INTERRUPT — Phase 5+), test code may pass `obj = NULL`. Production callers always pass a valid pointer. The handle table itself doesn't deref `obj` — that's the responsibility of the syscall handler / driver code that gets the handle and uses it.

### No underlying-kobj refcount integration at P2-Fc

`handle_close` just zeros the slot. P2-Fd integrates `vmo_unref` for KOBJ_VMO (which has its own dual-refcount lifecycle). Other kinds get refcount integration as their syscalls land:
- KOBJ_CHAN: Phase 4 with the 9P client.
- KOBJ_PROCESS / THREAD: when they become syscall-visible (Phase 5+).
- KOBJ_MMIO / IRQ / DMA / INTERRUPT: when the driver-startup flow lands at Phase 3.

A handle close that should decrement an underlying kobj refcount must be added to `handle_close`'s switch over kind (none today — explicit "no refcount yet" comment in the code).

### PROC_HANDLE_MAX = 64

Sufficient for v1.0's test scenarios + the eventual ramfs/proc/dev/net handle plumbing at boot. Container init flows that hold > 64 handles hit `-1` (table full). Phase 5+ replaces with a growable RB-tree keyed by hidx_t.

### Hardware kinds are statically prevented from transfer; the policy is in the type system, not at runtime

The transfer-via-9P codepath (Phase 4) will switch on `h->kind` and have cases ONLY for transferable kinds:

```c
switch (h->kind) {
case KOBJ_PROCESS:
case KOBJ_THREAD:
case KOBJ_VMO:
case KOBJ_CHAN:
    if (!(h->rights & RIGHT_TRANSFER)) return -EPERM;
    return do_transfer_via_9p(...);
/* No code path for hardware kinds. */
default:
    extinction("transfer of non-transferable handle kind %d", h->kind);
}
```

Stronger than "policy that says don't transfer hardware handles" — at the syscall site, the invariant is unfalsifiable. A bug that would have transferred a hardware handle instead extincts immediately.

### Single-CPU lifecycle at v1.0

`handle_alloc` / `handle_close` / `handle_dup` are not internally synchronized; concurrent callers on different CPUs would race. At v1.0 P2-Fc only the boot CPU calls these (no userspace yet → no syscalls). Phase 5+ adds a per-Proc handle-table lock when the syscall surface goes live and multi-thread Procs become possible.

### Capability handles (KObj_Capability) deferred to v2.0

Per ARCH §15.4, factotum-mediated capability elevation grants short-lived `KObj_Capability` handles. Out of scope for v1.0. The kobj_kind enum has space (KOBJ_KIND_COUNT may grow).

---

## Status

| Component | State |
|---|---|
| `handle.h` API + `handle.c` impl | Landed (P2-Fc) |
| `struct Proc.handles` field | Landed (P2-Fc) |
| `proc_alloc`/`proc_init`/`proc_free` integration | Landed (P2-Fc) |
| `handle_init` bootstrap | Landed (P2-Fc; between `pgrp_init` and `proc_init`) |
| Type classifiers (`is_transferable` / `is_hw`) | Landed (P2-Fc) |
| In-kernel tests | 5 added: alloc_close_smoke / rights_monotonic / dup_lifecycle / full_table_oom / kind_classifiers |
| Spec `handles.tla` + 4 buggy configs | Landed (P2-Fa) |
| KOBJ_VMO underlying-obj integration | P2-Fd |
| KOBJ_CHAN underlying-obj integration | Phase 4 |
| KOBJ_MMIO / IRQ / DMA underlying-obj integration | Phase 3 |
| `handle_transfer_via_9p` impl | Phase 4 (with 9P client) |
| Per-Proc handle-table lock | Phase 5+ |
| Growable RB-tree (replacing fixed slots) | Phase 5+ when bind count justifies |
| `KObj_Capability` (factotum elevation) | v2.0 |
