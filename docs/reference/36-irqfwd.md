# 36 — IRQ forwarding (KObj_IRQ blocker) (P4-G)

The hardware-IRQ → driver rendezvous. Per ARCH §9.3 + ROADMAP §6.1. v1.0 P4-G lands the kernel-internal `KObj_IRQ` lifecycle + a GIC dispatch hook that wakes a Rendez attached to the KObj_IRQ on every IRQ fire. Userspace driver bindings (handle table → driver process) come at P4-I+.

---

## Purpose

Userspace drivers (P4-I/J/K/L: virtio-blk / net / input / gpu) consume hardware completion notifications via Plan 9 `KObj_IRQ` handles. The kernel-side machinery — what P4-G implements — is:

1. Driver creates a KObj_IRQ for a specific GIC INTID via a syscall (Phase 5+ wrapper) or kernel-internal call (v1.0 P4-G).
2. GIC delivery routes through the kernel's IRQ vector → `gic_dispatch` → the registered KObj_IRQ's `kobj_irq_dispatch`.
3. Dispatch increments a pending-IRQ counter under the Rendez lock + wakes any blocked waiter.
4. The driver's wait thread (kernel-internal at v1.0; userspace at P4-I+) returns from `kobj_irq_wait` with the collapsed IRQ count and zeroes the counter.

The IRQ → wake atomicity is the same wait/wake protocol pinned by `scheduler.tla`'s `NoMissedWakeup` invariant (I-9): cond is checked under `r->lock`, and the producer's update + the wakeup straddle the same lock, so a fire that happens between cond check and sleep transition is captured by either the cond check (if cond saw it first) or the wakeup (which transitions the sleeping thread back to RUNNABLE).

VISION §4.5 budget: IRQ → userspace handler latency p99 < 5µs. v1.0 P4-G doesn't yet measure this — the latency benchmark requires userspace drivers (P4-I+); Phase 4 closing audit (P4-Z) verifies the budget.

---

## Public API — `<thylacine/irqfwd.h>`

```c
#define KOBJ_IRQ_MAGIC 0x4952510DBADC0DECULL   // 'IRQ\r' || 0xBADC0DEC
#define IPI_IRQFWD_TEST 1u                     // SGI 1 reserved for tests

struct KObj_IRQ {
    u64           magic;
    u32           intid;
    int           ref;
    struct Rendez rendez;       // single-waiter; lock guards pending_count
    u32           pending_count;
};

struct KObj_IRQ *kobj_irq_create(u32 intid);
void             kobj_irq_ref(struct KObj_IRQ *k);
void             kobj_irq_unref(struct KObj_IRQ *k);
void             kobj_irq_destroy(struct KObj_IRQ *k);
u32              kobj_irq_wait(struct KObj_IRQ *k);
u64              kobj_irq_total_fires(void);
u64              kobj_irq_live_count(void);
```

### `kobj_irq_create(intid)` — return semantics

| Return | Meaning |
|---|---|
| non-NULL | success: `magic` set, `ref=1`, `intid` recorded, `rendez` initialized, `pending_count=0`. GIC handler registered + IRQ enabled. |
| NULL | failure: SLUB OOM, gic_attach failed (handler slot already in use), or gic_enable_irq failed. |

### `kobj_irq_wait(k)` — return semantics

Blocks until `pending_count > 0`, then atomically reads + zeroes the counter under `r->lock`. Returns the count of IRQs collapsed into this single wait. Always >= 1 on a successful return. NULL k returns 0.

Spurious wake from a non-IRQ source is impossible — sleep's cond loop guarantees we only return when cond is true, and cond is `pending_count > 0` (only kobj_irq_dispatch increments it).

### `kobj_irq_destroy / unref` — invalidation

After the unref that drops ref to 0:
1. `gic_disable_irq(intid)` — stop the IRQ from firing.
2. `gic_attach(intid, NULL, NULL)` — clear the handler slot.
3. `magic = 0` — explicit clobber so a stale-pointer dispatch sees magic mismatch + bails before touching freed memory.
4. `kfree(k)` — release the SLUB slot.

The order matters: disable + unregister before free, so a fire-in-flight at destroy time can't dispatch into a freed KObj_IRQ.

---

## Implementation

`kernel/include/thylacine/irqfwd.h` (~95 LOC) + `kernel/irqfwd.c` (~140 LOC).

### Dispatch path

```c
static void kobj_irq_dispatch(u32 intid, void *arg) {
    struct KObj_IRQ *k = (struct KObj_IRQ *)arg;
    if (!k || k->magic != KOBJ_IRQ_MAGIC) return;

    irq_state_t s = spin_lock_irqsave(&k->rendez.lock);
    k->pending_count++;
    spin_unlock_irqrestore(&k->rendez.lock, s);

    __atomic_fetch_add(&g_irq_total_fires, 1u, __ATOMIC_RELAXED);

    wakeup(&k->rendez);
}
```

The lock is dropped BEFORE `wakeup` because `wakeup` re-takes `r->lock` to transition the waiter to RUNNABLE. Holding through wakeup would self-deadlock (same-CPU re-acquire of a non-reentrant lock).

### Wait path

```c
u32 kobj_irq_wait(struct KObj_IRQ *k) {
    if (!k) return 0;
    if (k->magic != KOBJ_IRQ_MAGIC) extinction(...);

    sleep(&k->rendez, kobj_irq_pending_cond, k);

    irq_state_t s = spin_lock_irqsave(&k->rendez.lock);
    u32 count = k->pending_count;
    k->pending_count = 0;
    spin_unlock_irqrestore(&k->rendez.lock, s);
    return count;
}
```

`sleep` calls cond under `r->lock`; if cond is already true (an IRQ fired before sleep entered), sleep returns immediately on the fast path without state transition. The post-sleep re-lock is what makes the read + zero atomic against a concurrent dispatch.

### Race walkthrough

Three orderings of "send IPI" vs "kobj_irq_wait":

**A. IRQ fires BEFORE wait enters sleep.**
1. dispatch: lock, `pending_count = 1`, unlock, wakeup (no waiter; no-op).
2. wait: sleep → cond (locked) sees `pending_count == 1`, fast-path returns.
3. wait: re-lock, read `count = 1`, zero, unlock, return 1.

**B. IRQ fires AFTER wait enters sleep.**
1. wait: sleep → cond sees 0 → marks self SLEEPING under lock → drops lock → schedules out.
2. dispatch: lock, `pending_count = 1`, unlock, wakeup → wakeup takes lock, transitions waiter to RUNNABLE, drops lock.
3. wait resumes: cond re-checks under lock, sees 1, returns.
4. wait: re-lock, read `count = 1`, zero, unlock, return 1.

**C. IRQ fires DURING wait's sleep transition (concurrent).**
- wait holds `r->lock` during cond + state transition.
- dispatch spins on `r->lock`.
- wait drops lock + schedules out (now SLEEPING, lock free).
- dispatch acquires lock, increments, drops, calls wakeup.
- wakeup transitions waiter to RUNNABLE.
- wait resumes; same as path B from step 3.

In all three cases, `count = 1` returned. NoMissedWakeup proved by the lock interlock.

### IRQ collapsing

If multiple IRQs fire before `wait` consumes the counter, the count is the sum:

```
dispatch → pending_count = 1
dispatch → pending_count = 2
dispatch → pending_count = 3
wait     → returns 3
```

Drivers handle this by checking the device's used-ring (virtqueue) state — the used-ring index reflects "how many entries actually completed", which is the authoritative count. The IRQ count is informational; the protocol requires "wait returns ≥ 1 ⇒ check the device for new completions until none remain".

### GIC integration

`kobj_irq_create` calls:
- `gic_attach(intid, kobj_irq_dispatch, k)` — binds the dispatch handler with `k` as the cookie.
- `gic_enable_irq(intid)` — unmasks the IRQ on this CPU's redistributor (for SGIs/PPIs) or distributor (for SPIs).

If `gic_attach` fails (handler slot already in use), create returns NULL after kfree. If `gic_enable_irq` fails, create rolls back the attach + frees.

Cleanup on destroy: `gic_disable_irq(intid)` then `gic_attach(intid, NULL, NULL)` then `kfree`.

---

## Spec cross-reference

P4-G is impl-only — no new TLA+ module. The wait/wake atomicity is covered by `scheduler.tla::NoMissedWakeup` (I-9). The IRQ-counter pattern is a direct application of the wait-on-cond protocol.

ARCH §28 I-19 (note delivery causal order) is the related-but-distinct invariant for cross-process notes (Phase 5+); the IRQ → wait path is the same wait/wake protocol but at a single-Rendez granularity.

---

## Tests

`kernel/test/test_irqfwd.c` — 4 tests:

| Test | Covers |
|---|---|
| `irqfwd.create_destroy` | Lifecycle: create → destroy; live count balances; magic/intid/ref correct on fresh KObj_IRQ. |
| `irqfwd.refcount_lifecycle` | ref/unref balance; live count stable while ref > 0; drops on last unref. |
| `irqfwd.wait_wakes_on_sgi` | Self-IPI on SGI 1 → kobj_irq_wait returns ≥ 1 + global fire counter increments. |
| `irqfwd.collapses_concurrent_fires` | 3 self-IPIs sent before wait → wait returns ≥ 1 (GIC SGI coalescing may reduce count, but at least 1 must be observed). |

All tests use SGI 1 (`IPI_IRQFWD_TEST`) as the fire source, software-triggered via `gic_send_ipi(self, ...)` so we don't need a real hardware device.

---

## Status

| Component | State |
|---|---|
| `kernel/include/thylacine/irqfwd.h` | Landed (P4-G) |
| `kernel/irqfwd.c` (lifecycle + dispatch + wait) | Landed (P4-G) |
| GIC integration (gic_attach + gic_enable_irq + gic_disable_irq) | Landed (P4-G) |
| In-kernel tests | 4 covering lifecycle, refcount, wait/wake, IRQ collapsing |
| Handle-table integration (KOBJ_IRQ release calls kobj_irq_unref) | Held to P4-I+ alongside the first userspace driver |
| Userspace syscall wrapper (allocate KOBJ_IRQ via syscall) | Held to Phase 5+ syscall surface |
| Multi-waiter wait queues | Held to Phase 5+ (poll/futex) |
| Multi-subscriber per IRQ (fan-out wakeup) | Held — v1.0 each IRQ has exactly one KObj_IRQ |
| IRQ → userspace latency benchmark (VISION §4.5 p99 < 5µs) | Held to P4-Z (Phase 4 closing audit; needs userspace drivers to measure) |

---

## Known caveats / footguns

### Single-waiter per KObj_IRQ

The Rendez allows at most one sleeping thread. Two concurrent `kobj_irq_wait` calls on the same KObj_IRQ extincts on the second. v1.0 acceptable: a driver process has one IRQ-handling thread per IRQ. Phase 5+ adds multi-waiter via poll/futex when userspace event multiplexing lands.

### Edge-triggered counter, not a queue

The `pending_count` is a count, not a queue of (timestamp, payload) tuples. Drivers can't recover "what time did each IRQ fire" — they get an aggregate count. For VirtIO this is sufficient (the device's used-ring carries per-completion info); for arbitrary IRQ sources it may matter. Phase 5+ might add a per-IRQ ring buffer if a driver demands it.

### gic_disable_irq + gic_attach(NULL, NULL) order at destroy

The destroy path disables BEFORE clearing the handler slot. A subtle race: if a fire is pending in the GIC at the moment of disable, and the GIC delivers it after disable but before attach-NULL, dispatch runs with the still-valid k. Currently safe because:
1. SGI/PPI/SPI go through the "IRQ pending" → "delivered" transition while the CPU still has IRQs unmasked at the GIC.
2. gic_disable_irq writes to ICENABLER which suppresses *future* deliveries; in-flight deliveries continue.
3. The dispatch's magic check catches a UAF if k is freed between the GIC's "delivery start" and dispatch entry.

A stronger guarantee would be a synchronous "drain pending" step in destroy. Held to Phase 5+ when concurrent destroy + IRQ becomes a real scenario (currently single-threaded boot test pattern).

### `__atomic_fetch_add` on g_irq_total_fires uses RELAXED

Counter is diagnostic; ordering doesn't matter. Tests use this for "did any IRQ fire?" not for synchronization. RELAXED is the cheapest correct choice.

### gic_send_ipi(self, SGI 1) coalescing

ARM GIC SGI delivery may coalesce multiple sends if the IRQ is still pending at the redistributor. The `irqfwd.collapses_concurrent_fires` test checks "at least 1" rather than exactly 3 because the GIC's coalescing behavior is implementation-defined.

### Test reuses IPI_IRQFWD_TEST = SGI 1

Tests that run sequentially each create + destroy a KObj_IRQ on SGI 1. The destroy path clears the handler slot, so subsequent tests can re-attach. If a test fails mid-flight without destroying, SGI 1's handler remains attached + the next test fails on `gic_attach`. Tests are written to balance create/destroy explicitly; future stress tests should use a per-test SGI to avoid the dependency.

---

## References

- `docs/ARCHITECTURE.md` §9.3 — driver model + KObj_IRQ semantics.
- `docs/ROADMAP.md` §6.1 — Phase 4 deliverables (irqfwd among them).
- `docs/reference/16-rendez.md` — Plan 9 wait/wake (the substrate).
- `docs/reference/15-scheduler.md` — sched/ready/preempt (the substrate's substrate).
- `arch/arm64/gic.h::gic_attach + gic_enable_irq` — handler binding.
- `kernel/sched.c::wakeup` — Rendez wake transition.
- `specs/scheduler.tla::NoMissedWakeup` (I-9) — wait/wake atomicity invariant.
