# Reference: ASID allocator — rolling generation-rollover (RW-1 B-F1)

## Purpose

The ASID (Address Space Identifier) allocator manages the hardware ASID space for per-Proc TTBR0 page-table tagging, so context switches between Procs do not require a full TLB flush: each Proc's user mappings are tagged with a hardware ASID, and TLB lookups match against the current `TTBR0_EL1.ASID`.

Since RW-1 B-F1 (ARCH §6.2.1), ASIDs are a **recycled cache keyed by a global generation counter** — the Linux arm64 rolling-ASID model (`arch/arm64/mm/context.c`) — **not** a per-Proc permanent allocation. The prior per-Proc-permanent design assigned one ASID per Proc for its lifetime and **extincted the kernel on the (ASID-space + 1)th concurrent Proc** (8-bit space, no rollover) — an unprivileged whole-system DoS (RW-1 B-F1, graded P1). The rolling design removes exhaustion entirely: the concurrent-Proc ceiling is gone.

ASID 0 is reserved for the kernel's global (TTBR1) mappings and the kernel TTBR0; user ASID values run `[1, (1<<ASID_BITS)-1]`.

## Public API

### `<arch/arm64/asid.h>`

```c
#define ASID_RESERVED_KERNEL  0u
#define ASID_USER_FIRST       1u
#define ASID_TTBR0_SHIFT      48u   // TTBR0_EL1 ASID field is bits [63:48]

static inline unsigned asid_hw_bits(void);   // 8 or 16 from ID_AA64MMFR0_EL1.ASIDBits

void asid_init(void);
u64  asid_resolve(u64 *context_id, unsigned cpu);

unsigned asid_bits(void);            // 8 or 16
u64      asid_generation_now(void);  // current generation value
u64      asid_rollover_count(void);  // rollovers since boot
```

#### `asid_hw_bits(void) → unsigned`

Reads `ID_AA64MMFR0_EL1.ASIDBits` (bits [7:4]): `0b0010` → 16-bit, else 8-bit. Used by `asid_init` (bitmap + generation sizing) **and** `mmu.c` (`TCR_EL1.AS`) — both read the register directly, so there is no init-ordering dependency on a shared global. ARM requires a system-wide-uniform ASID width, so the boot-CPU width governs all CPUs.

#### `asid_init(void)`

Boot-time init: reads the ASID width, sizes the bitmap + generation, zeroes the per-CPU `active`/`reserved`/`flush_pending` state, and stamps the generation to **#1** (so a `context_id` of 0 — "never assigned" — always misses). Runs after the MMU is up, before any context switch calls `asid_resolve`. Extincts on a second call.

#### `asid_resolve(u64 *context_id, unsigned cpu) → u64`

The context-switch core. Resolves the current-generation ASID for the Proc whose `context_id` is `*context_id`, running on logical CPU `cpu`, updating `*context_id` and the per-CPU/rollover state as needed. Returns the **hardware ASID value** (not the full `context_id`) for the TTBR0 compose. The caller (the scheduler pre-hook `sched_install_asid_ttbr0`) composes `ttbr0 = (asid << ASID_TTBR0_SHIFT) | pgtable_root`.

Two paths:

1. **Fast path (lockless)** — taken iff this CPU's active slot is non-zero AND the Proc's generation matches the global generation. An atomic `cmpxchg` publishes the `context_id` into `g_active_asids[cpu]` and **fails** if a concurrent rollover zeroed the slot (→ slow path). No lock, no flush.
2. **Slow path (under `g_asid_lock`)** — a miss (stale generation or never-assigned). `new_context` claims a free ASID from the bitmap; if none is free, it **rolls over** (below). Then it honors this CPU's pending local flush and publishes.

MUST be called with IRQs masked on the CPU `cpu` names (the pre-hook satisfies this: rq lock held / IRQs masked). NEVER called for kproc / kernel threads (`pgtable_root == 0` → kernel TTBR0, ASID 0, bypassing the allocator). `context_id` is a plain `u64` accessed via `__atomic_*`; the fast path is lockless.

#### Diagnostics

`asid_bits` / `asid_generation_now` / `asid_rollover_count` return atomic snapshots.

## Implementation (`arch/arm64/asid.c`)

### State

```c
static spin_lock_t g_asid_lock;          // a LEAF lock (no nested lock)
static unsigned g_asid_bits;             // 8 or 16
static u64 g_asid_val_mask, g_asid_gen_unit, g_asid_last;
static u64 g_asid_generation;            // high bits; lockless atomic reads, under-lock atomic writes
static u64 g_asid_map[1024];             // claimed-this-generation bitmap (16-bit ceiling, 8 KiB)
static u64 g_asid_cur_idx;               // round-robin search hint
static u64  g_active_asids[DTB_MAX_CPUS];   // per-CPU published context_id (0 = none)
static u64  g_reserved_asids[DTB_MAX_CPUS]; // per-CPU reserved-across-rollover context_id
static bool g_flush_pending[DTB_MAX_CPUS];  // per-CPU local-flush-owed
```

### `context_id` bit layout

```
bits [ASID_BITS-1 : 0]   the hardware ASID value (1 .. (1<<ASID_BITS)-1)
bits [63 : ASID_BITS]    the generation (a multiple of ASID_GEN_UNIT = 1<<ASID_BITS)
```

`context_id == 0` is "never assigned" (generation 0; `gen_match(0)` is false because the global generation starts at #1).

### `asid_resolve` — the fast path and the rollover race

The fast path has **two** guards, both load-bearing:

1. **generation-match** (`gen_match(cid)`): the Proc's ASID is still valid this generation. After a rollover bumps the generation, a stale-generation Proc fails this and is forced to the slow path (re-stamp + flush).
2. **no-pending-flush** (`old_active != 0`): the CPU has no un-flushed stale TLB from before a rollover. `flush_context` zeroes `g_active_asids[all]` on rollover, so a peer's next switch finds active == 0, fails the fast-path test, takes the slow path, and there honors `flush_pending`. This is Linux's `old_active_asid != 0` guard.

The **rollover race** — the famous, subtle rolling-ASID hazard — is between the lockless fast-path `cmpxchg` and `flush_context`'s per-CPU `xchg`, both on `g_active_asids[cpu]`. They are serialized by that single location: a fast publish either lands **before** the rollover's xchg (which then reads and RESERVES it) or **after** (the cmpxchg sees 0 and FAILS → slow path). There is no interleaving where a Proc publishes an ASID the rollover then reassigns. (Modeled exhaustively in `specs/asid.tla`; see Spec cross-reference.)

### `new_context` + `flush_context` (the rollover)

`new_context(old_cid)` (under `g_asid_lock`):
- If `old_cid != 0` and the Proc owns a reservation of its value (`check_update_reserved`, a **full-`context_id`** compare — this is the load-bearing ownership distinction), re-stamp and keep the same ASID.
- Else if the old value is still free this generation, keep it.
- Else claim a free ASID; if none, **roll over**.

`flush_context` (the rollover): bump the generation, reset the bitmap, and for every CPU `xchg` its active slot to 0 — preserving each CPU's active ASID (or, if idle, its existing reserved one) into `g_reserved_asids[cpu]` and re-marking the bitmap — then arm `flush_pending` for every CPU. **NOSTEAL**: a running CPU's ASID is never reassigned (the reservation), so two address spaces never alias one ASID (invariant I-31).

### `check_update_reserved` — the ownership guard

```c
if (g_reserved_asids[i] != 0 && g_reserved_asids[i] == old_cid) { ...reclaim... }
```

The compare is the **full** `context_id` (generation + value), not just the value. Because `(generation, value)` is unique per Proc-instance, this is an **ownership** check: a Proc reclaims **only its own** reservation, never another live Proc's ASID that happens to share the value. (The HOLOTYPE RW-1 audit F1 found the model originally abstracted this as ownerless value-membership — an unsafe algorithm the impl does not have; the spec was corrected to the `rproc` ownership model. The impl was always correct.)

### Teardown TLB-safety (no per-Proc `asid_free`)

There is no per-Proc ASID free. At `proc_free`, the `context_id` is simply dropped; the hardware ASID value stays reserved in the current generation until the next rollover. This is TLB-safe: the leaf user mappings were already invalidated by `vma_drain`'s all-ASID `tlbi vaae1is` (runs before `proc_pgtable_destroy`); no live CPU holds a dead Proc's TTBR0 (every Thread is reaped + `on_cpu`-spun before reap), so no CPU translates under its ASID; and any eventual reuse of the value is gated by the rollover's per-CPU `flush_pending` local flush. (Matches Linux: no TLB flush at mm teardown; reclaim at rollover.)

### Context-switch wiring

`struct Proc` carries `u64 context_id` (replacing the old `u16 asid`; `sizeof(struct Proc)` unchanged at 264). The pre-hook `sched_install_asid_ttbr0(next)` runs at both `cpu_switch_context` call sites (`kernel/sched.c`, `kernel/thread.c`), composing `next->ctx.ttbr0` before the asm switch loads it; a no-op for kproc / kernel threads. `thread_create` bakes the kernel TTBR0 (ASID 0) as the initial value, so a never-reached missing-pre-hook path faults safely rather than aliasing ASID 0 onto a user root. `mmu.c::mmu_program_this_cpu` sets `TCR_EL1.AS = 1` when the CPU reports 16-bit ASIDs.

### Lock order

`asid_resolve` takes `g_asid_lock` while the caller holds the run-queue lock → `rq_lock -> g_asid_lock`. `g_asid_lock` is a true **leaf** (its critical section touches only the bitmap + per-CPU arrays + a local TLBI; no nested lock), so the order is acyclic.

## State machines

### Per-Proc context_id lifecycle

```
[context_id = 0, "never assigned"]
    → asid_resolve slow path (new_context claims a free ASID)
[stamped gen|value; ACTIVE on some CPU]
    → fast path reuse (gen matches) | deschedule (stays claimed)
    → rollover bumps gen: reserved if active (kept), else freed at the bitmap reset
[stale generation]
    → asid_resolve slow path: reclaim own reservation (same value) OR claim fresh
```

## Spec cross-reference

`specs/asid.tla` (model-first; spec-first re-enabled for this surface per the SMP precedent in ARCH §8.4). Invariant **I-31** (ARCH §28). The clean cfg is TLC-GREEN at the F1 exposing bound (≥ 4 Procs); five buggy cfgs are executable documentation, each violating a targeted invariant:

| cfg | violates | the modeled bug |
|---|---|---|
| `asid_buggy_rollover_steals_active` | ActiveClaimed | rollover resets the bitmap without reserving active ASIDs |
| `asid_buggy_fast_no_regen` | NoActiveAlias | fast path drops the generation recheck |
| `asid_buggy_no_flush_pending` | NoStaleTLB | rollover skips `flush_pending` for peers |
| `asid_buggy_fast_no_flush_check` | NoStaleTLB | fast path runs while `flush_pending` set |
| `asid_buggy_reserve_value_only` | NoActiveAlias | ownerless reservation reclaim (the audit-F1 bug) |

Spec actions ↔ source: `FastSwitch`/`SlowSwitch` ↔ `asid_resolve`; the rollover branch ↔ `new_context` + `flush_context`; `OwnsReservedValue` ↔ `check_update_reserved`.

## Tests (`kernel/test/test_asid.c`)

- `asid.width_valid` — `asid_bits()` is 8 or 16; the generation is non-zero.
- `asid.resolve_reuse` — re-resolving a Proc's `context_id` returns the same (valid) ASID (the fast path).
- `asid.distinct_active` — two distinct Procs active on two CPUs hold distinct ASIDs (no alias).
- `asid.rollover_preserves` — fills the full ASID space, forces a real rollover, and asserts a running Proc keeps its ASID across it (NOSTEAL / I-31).

`test_proc_pgtable.c::proc.ttbr0_swap_smoke` additionally verifies the pre-hook installs a valid rolling `(asid << 48) | pgtable_root` with distinct ASIDs for two concurrently-live children.

## Error paths

| Path | Trigger | Message |
|---|---|---|
| `asid_init` re-entry | calling twice | `"asid_init called twice"` |
| `asid_resolve` pre-init | before `asid_init` | `"asid_resolve before asid_init"` |
| `asid_resolve` bad cpu | `cpu >= DTB_MAX_CPUS` | `"asid_resolve: cpu index out of range"` (RW-1 audit F3 fail-fast) |
| rollover with no free ASID | unreachable (`flush_context` reserves ≤ `DTB_MAX_CPUS` < ASID space) | `"asid: no free ASID after rollover"` |

## Performance characteristics

- Fast path: a lockless atomic read + `gen_match` + an atomic `cmpxchg` — the common case, no lock, no flush.
- Slow path: `g_asid_lock` + a bitmap search (amortized O(1) via the `cur_idx` hint) + (rare) a rollover. A rollover is O(`DTB_MAX_CPUS`) reservation + a local TLB flush per CPU at its next switch. 16-bit ASIDs make rollovers rare (65535 vs 255 user ASIDs before one is forced).

## Status

**Landed (RW-1 B-F1):** the rolling allocator (`@d742ffa`), the model-first spec (`@4fe50f7`), the focused audit close (`@d40dbbb`). 806/806 PASS; SMP gate 0 corruption. The fail-soft interim (`@ef29456`) and the scripture (`@83bd74e`) preceded it.

## Known caveats / footguns

- **The fast path needs BOTH guards.** generation-match alone is insufficient — `~flush_pending` (`old_active != 0`) is equally load-bearing (a rollover leaves un-flushed stale TLB on peers). The model surfaced this before the impl.
- **`check_update_reserved` must compare the FULL `context_id`.** A value-only compare reintroduces the audit-F1 alias. The `asid_buggy_reserve_value_only.cfg` is the durable counterexample.
- **Reserved ASID 0.** Never handed out (the bitmap search starts at `ASID_USER_FIRST`); a user Proc with ASID 0 would alias the kernel's global TTBR1 entries.
- **Per-CPU `cpu` index assumption.** `asid_resolve` (like the whole scheduler) assumes `smp_cpu_idx_self()` is a dense `< DTB_MAX_CPUS` index. The fail-fast assert catches an out-of-range value; the durable DTB MPIDR→dense-index map is a tracked whole-scheduler portability item (RW-1 audit F3).
- **No `u64` generation-overflow guard.** Wrap is ~2^48 rollovers (16-bit) — millennia; Linux carries the identical property.
