# 15 — Scheduler dispatch (as-built reference)

P2-Ba's deliverable: the kernel can now dispatch between multiple threads via `sched()` (yield) + `ready(t)` (mark runnable). Three priority bands (INTERACTIVE / NORMAL / IDLE) with a per-band sorted-by-`vd_t` run tree; pick-min-`vd_t` from the highest-priority non-empty band. P2-Bb adds wait/wake (`thread_block` / `thread_wake` / `Rendez`); P2-Bc adds scheduler-tick preemption + IRQ-mask discipline + the full EEVDF math (weight, slice_size, latency bound).

Scope: `kernel/include/thylacine/sched.h`, `kernel/sched.c`, `kernel/test/test_sched.c`, `kernel/include/thylacine/thread.h` (struct Thread extended with EEVDF fields).

Reference: `ARCHITECTURE.md §8` (scheduler design), `§28` invariants I-8, I-17, I-18 (latency bound, IPI ordering — pinned at P2-Bc / P2-C).

---

## Purpose

Phase 2 entry (P2-A) brought up the context-switch primitive (`cpu_switch_context`) + the C wrapper (`thread_switch`) — both direct: caller specifies the target, no scheduler logic. P2-Ba layers the scheduler on top: `sched()` is "yield to next-best runnable thread"; `ready(t)` is "mark thread schedulable."

The simplification at P2-Ba (vs the full EEVDF design in ARCH §8): each thread's `vd_t` is monotonically advanced past every other runnable thread on yield, giving FIFO-equivalent rotation within a band. The full EEVDF math (weighted virtual time advance + slice-bounded `vd_t = ve_t + slice × W_total / w_self`) lands at P2-Bc, when scheduler-tick preemption introduces meaningful "elapsed virtual time."

---

## Public API

### `<thylacine/sched.h>`

```c
#define SCHED_BAND_INTERACTIVE  0u
#define SCHED_BAND_NORMAL       1u
#define SCHED_BAND_IDLE         2u
#define SCHED_BAND_COUNT        3u

void     sched_init(void);             // call after thread_init
void     sched(void);                  // yield
void     ready(struct Thread *t);      // mark t runnable + insert into run tree
unsigned sched_runnable_count(void);
unsigned sched_runnable_count_band(unsigned band);
void     sched_remove_if_runnable(struct Thread *t);  // internal — for thread_free
```

`sched()` preconditions:
- `sched_init` has run.
- `current_thread()` returns a valid Thread (post-`thread_init`).

`sched()` semantics:
- Picks the highest-priority band with at least one runnable thread; within that band, the head of the sorted list (min `vd_t`).
- If no other runnable thread: returns immediately, caller continues.
- Otherwise: caller (`prev`) transitions RUNNING → RUNNABLE; `prev->vd_t` advances past the run tree's max via `g_vd_counter++`; `prev` inserts into its band's tree (back of rotation). Picked thread (`next`) transitions RUNNABLE → RUNNING; removed from tree; `set_current_thread(next)`; `cpu_switch_context(&prev->ctx, &next->ctx)`.

`ready(t)` preconditions:
- `t != NULL`; `t->magic == THREAD_MAGIC`.
- `t->state == THREAD_RUNNABLE` (caller's responsibility — typically set by `thread_create` or, future, `thread_wake`).
- `t->band < SCHED_BAND_COUNT`.
- `t` is NOT already in the run tree.

`ready(t)` semantics:
- Insert `t` into `t->band`'s tree, sorted ascending by `vd_t`. Ties (equal `vd_t`) place `t` after existing equal-keyed nodes (FIFO within ties).

`sched_remove_if_runnable(t)` semantics:
- Idempotent. If `t->state != RUNNABLE` or `t` is not in the tree: no-op.
- Otherwise: unlink. Used by `thread_free` so a freed RUNNABLE thread doesn't leave a dangling pointer in the run tree.

---

## Implementation

### `kernel/sched.c` (~150 LOC)

State (all globals at v1.0 single-CPU; per-CPU at P2-C):
- `g_run_tree[SCHED_BAND_COUNT]`: head pointer per band (NULL if band empty).
- `g_vd_counter`: monotonic counter; advanced on each `sched()` yield. Starts at 1 (kthread reserves `vd_t=0`).
- `g_sched_initialized`: one-shot init flag.

Run-tree representation: per-band doubly-linked list, sorted ascending by `vd_t`. Head = min. Insert is O(N) (linear scan). Remove is O(1) (link manipulation). Pick-next is O(1) per band, O(SCHED_BAND_COUNT) overall.

Why linear-scan list at v1.0: ARCH §8.4 specifies red-black tree, but with realistic v1.0 thread counts (tens, not thousands), the tree's O(log N) advantage is invisible. Refactoring to RB at P2-Bc or Phase 7 is a contained change once the API is stable.

Key functions:

- `insert_sorted(t)`: walks `g_run_tree[t->band]` to find first node with `vd_t > t->vd_t`; inserts `t` before. Handles head-prepend (empty band or `t < head`).

- `unlink(t)`: standard doubly-linked-list removal. Handles `t == head` (updates `g_run_tree[t->band]`).

- `pick_next()`: scans bands from highest priority (INTERACTIVE) to lowest (IDLE); returns + unlinks the first non-empty band's head.

- `sched()`: `pick_next()`; if NULL, return. Otherwise: `prev->vd_t = g_vd_counter++`; `insert_sorted(prev)`; `prev->state = RUNNABLE`; `next->state = RUNNING`; `set_current_thread(next)`; `cpu_switch_context(&prev->ctx, &next->ctx)`.

- `ready(t)`: validate; `insert_sorted(t)`.

### thread.c integration

`struct Thread` extended at P2-Ba (per `thread.h` contract; `_Static_assert` updated 168 → 200 bytes):
- `s64 vd_t` — virtual deadline. 0 for kthread initial; `g_vd_counter++` on yield.
- `u32 weight` — EEVDF weight. Default 1; full semantics at P2-Bc.
- `u32 band` — `SCHED_BAND_*`. Default `NORMAL`.
- `runnable_next`, `runnable_prev` — doubly-linked list links into band's run tree. Both `NULL` when not in tree.

Defaults:
- `thread_init`: kthread `vd_t = 0` (via KP_ZERO), `weight = 1`, `band = NORMAL`.
- `thread_create`: new thread `vd_t = 0` (via KP_ZERO), `weight = 1`, `band = NORMAL`.

`thread_free` calls `sched_remove_if_runnable(t)` before `thread_unlink_from_proc(t)`. A freed RUNNABLE thread's run-tree links are NULLed; the buddy/SLUB free of the descriptor + kstack proceeds as before.

### Integration with main.c

`boot_main` calls `sched_init()` immediately after `thread_init()`. Banner adds:

```
sched:   bands=3 (INTERACTIVE/NORMAL/IDLE) runnable=N (0 expected pre-test; ready() inserts)
```

---

## Data structures

### `struct Thread` (P2-Ba, 200 bytes total — was 168 at P2-A R4)

| Field | Offset | P2-Ba? | Purpose |
|---|---|---|---|
| `magic` | 0 | (P2-A R4) | THREAD_MAGIC sentinel |
| `tid` | 8 | (P2-A) | Thread ID |
| `state` | 12 | (P2-A) | thread_state enum |
| `proc` | 16 | (P2-A) | owning Proc |
| `ctx` | 24 | (P2-A) | saved register context (112 B) |
| `kstack_base` | 136 | (P2-A) | kernel stack low address |
| `kstack_size` | 144 | (P2-A) | kernel stack size |
| `next_in_proc` | 152 | (P2-A) | proc's threads list link |
| `prev_in_proc` | 160 | (P2-A) | proc's threads list link |
| **`vd_t`** | **168** | **NEW** | virtual deadline (smaller → run sooner) |
| **`weight`** | **176** | **NEW** | EEVDF weight (default 1) |
| **`band`** | **180** | **NEW** | SCHED_BAND_* (INTERACTIVE / NORMAL / IDLE) |
| **`runnable_next`** | **184** | **NEW** | run-tree forward link (NULL if not in tree) |
| **`runnable_prev`** | **192** | **NEW** | run-tree backward link |

`_Static_assert(sizeof(struct Thread) == 200)` — pinned at P2-Ba.

### per-band run tree

Three doubly-linked lists (one per band), sorted ascending by `vd_t`. `g_run_tree[band]` is the head pointer (NULL if band empty). Threads are linked via `runnable_next` / `runnable_prev`.

```
band 0 (INTERACTIVE):  NULL
band 1 (NORMAL):       [t_a, vd=2] ⇄ [t_b, vd=5] ⇄ [t_c, vd=7]
band 2 (IDLE):         NULL
                       ↑ head      ↑ tail
```

Pick from band 1 → unlink `t_a` (head) → return.

---

## Tests

`kernel/test/test_sched.c` registers two tests:

### `scheduler.dispatch_smoke`

- Boot kthread creates two threads (`ta`, `tb`); `ready()`s both.
- Calls `sched()`. The scheduler picks `ta` (head of NORMAL band — min `vd_t = 0`); `ta` runs the entry function (increments `g_test_sched_state[0]`); `ta` calls `sched()` (advances its `vd_t` past `tb` and the inserted boot kthread; picks `tb`); `tb` runs (increments `g_test_sched_state[1]`); `tb` calls `sched()` (picks boot kthread which has min `vd_t` now); boot resumes.
- Boot verifies counters reached exactly 1 (each fresh thread ran once before yielding back).
- `thread_free(ta)` and `thread_free(tb)` reclaim — both still RUNNABLE in the tree from their suspended-in-`sched()` state; `sched_remove_if_runnable` unlinks before reclamation.

### `scheduler.runnable_count`

- Empty tree at entry (`sched_runnable_count() == 0`).
- `ready(t)` increments count to 1 in the NORMAL band.
- `thread_free(t)` (state still RUNNABLE) restores count to 0 via `sched_remove_if_runnable`.

What the tests do NOT cover (deferred):
- Wait/wake protocol (P2-Bb).
- Scheduler-tick preemption (P2-Bc).
- Band priority — INTERACTIVE preempts NORMAL (P2-Bb or P2-Bc with multi-thread workloads).
- EEVDF weight + slice fairness (P2-Bc).
- SMP work-stealing (P2-C).

---

## Spec cross-reference

`specs/scheduler.tla` at P2-Cg models:
- Thread state machine (RUNNING / RUNNABLE / SLEEPING).
- Per-CPU dispatch with the four state-consistency invariants.
- Wait/wake atomicity (the canonical missed-wakeup race; `NoMissedWakeup` invariant).
- **Cross-CPU work-stealing (`Steal`) with `NoDoubleEnqueue` invariant** *(P2-Cg)*.
- **Per-(src, dst) FIFO IPI delivery (`IPI_Send` / `IPI_Deliver`) with `IPIOrdering` invariant** *(P2-Cg, ARCH §28 I-18)*.

P2-Ba's dispatch implementation conforms to the spec's `Yield(cpu)` action — the spec's "pick from runqueue" via CHOOSE is non-deterministic (any choice satisfies the state-machine invariants). P2-Ba picks min-`vd_t`, which is one valid instantiation of the spec's CHOOSE.

P2-Cg lifts the SMP discipline introduced at P2-Ce/Cf to the spec level. Three buggy configs (one per invariant) act as executable documentation of the bug each invariant defends against:

| Config | Flag | Invariant violated | Counterexample shape |
|---|---|---|---|
| `scheduler_buggy.cfg`       | `BUGGY = TRUE`           | `NoMissedWakeup`  | BuggyCheck → WakeAll → BuggySleep splits cond-check from sleep-transition; the wakeup that fired between observes empty waiters, so the buggy waiter sleeps after cond=TRUE. |
| `scheduler_buggy_steal.cfg` | `BUGGY_STEAL = TRUE`     | `NoDoubleEnqueue` | BuggySteal adds a thread to stealer's runq without removing from victim's — thread is in two runqueues. |
| `scheduler_buggy_ipi.cfg`   | `BUGGY_IPI_ORDER = TRUE` | `IPIOrdering`     | IPI_Send twice; BuggyIPI_Deliver pops the second-sent IPI first; head of queue ≠ next-expected delivery seq. |

The deferred refinements (post-P2-Cg):
- `PickIsMinDeadline` — sched picks min `vd_t` among runnable threads in the highest-priority non-empty band.
- `LatencyBound` (I-17) — delay between RUNNABLE and RUNNING ≤ slice_size × N_runnable. Requires weak fairness; Phase 2 close adds.
- Full EEVDF math: `vd_t = ve_t + slice × W_total / w_self`. Meaningful with weight ≠ 1 (Phase 5+).
- Per-(src, type, dst) IPI ordering refinement: today's per-(src, dst) version is sound while all IPIs are equal-priority + same-type. P5+ multi-type IPIs (TLB shootdown, halt, generic) refine to per-type queues.

The simplified vd_t advance at P2-Ba (monotonic counter) is sound under the existing state-machine invariants but doesn't yet satisfy the latency bound (which requires the full slice-based math).

---

## Error paths

| Function | Error | Action |
|---|---|---|
| `sched_init` | called twice | extinct |
| `sched_init` | called pre-thread_init | extinct |
| `sched` | sched_init not called | extinct |
| `sched` | no current_thread | extinct |
| `sched` | current corrupted (magic mismatch) | extinct |
| `ready` | NULL t | extinct |
| `ready` | t corrupted | extinct |
| `ready` | t->state != RUNNABLE | extinct |
| `ready` | t->band invalid | extinct |
| `ready` | t already in run tree | extinct |
| `sched_remove_if_runnable` | NULL or non-RUNNABLE or not in tree | no-op (idempotent) |

---

## Performance characteristics

| Operation | Cost |
|---|---|
| `sched()` (no other runnable) | ~3 cycles (pick_next early-return) |
| `sched()` (with switch) | ~30 instructions cpu_switch_context + O(N) insert + O(SCHED_BAND_COUNT) pick = O(N) at v1.0 |
| `ready()` | O(N) — sorted insert (N = band's runnable count) |
| `sched_runnable_count()` | O(N) — walks all bands |
| `sched_remove_if_runnable()` | O(1) — direct unlink |

For v1.0's expected thread counts (tens), O(N) insert is sub-microsecond.

Boot-time impact: P2-A R4 close ~39 ms → P2-Ba ~42 ms (production). +3 ms for the two new tests + scheduler init.

VISION §4 budget: 500 ms. Headroom remains generous.

---

## Status

| Component | State |
|---|---|
| `sched.h` API + `sched.c` impl | Landed (P2-Ba) |
| `struct Thread` EEVDF fields | Landed (P2-Ba) |
| `sched_init()` integration | Landed |
| Run-tree: sorted doubly-linked list per band | Landed (RB tree at P2-Bc / Phase 7) |
| `sched()` + `ready()` | Landed |
| Per-band priority dispatch | Landed |
| `thread_free` integration (`sched_remove_if_runnable`) | Landed |
| In-kernel tests | 2 added: `scheduler.dispatch_smoke`, `scheduler.runnable_count` |
| Wait/wake (`thread_block` / `thread_wake` / `Rendez`) | P2-Bb |
| Scheduler-tick preemption (timer IRQ → sched) | P2-Bc |
| IRQ-mask discipline around `sched()` | P2-Bc |
| Full EEVDF math (weight, slice_size, latency bound) | P2-Bc |
| Spec EEVDF refinement (`PickIsMinDeadline`, latency bound) | Phase 2 close |
| SMP per-CPU run trees | Landed (P2-Cd) |
| Cross-CPU IPIs (GIC SGIs, IPI_RESCHED) | Landed (P2-Cdc) |
| Work-stealing (`try_steal` + finish_task_switch handoff) | Landed (P2-Ce) |
| `on_cpu` flag + SMP wait/wake race close | Landed (P2-Cf) |
| Spec SMP refinement (`Steal`, `IPI_Send`/`Deliver`, `NoDoubleEnqueue`, `IPIOrdering`) | Landed (P2-Cg) |
| `LatencyBound` liveness spec | Phase 2 close |
| Red-black tree refactor | Phase 7 |

---

## Known caveats / footguns

### `vd_t` monotonic counter overflow

`g_vd_counter` is a `s64` advancing on each `sched()` yield. At 1000 yields/sec (timer rate), overflow takes 2^63 / 1000 ≈ 292 million years. Not a v1.0 concern.

### Run tree is global (single-CPU)

P2-Ba uses a global `g_run_tree` array — single-CPU only. P2-C makes per-CPU. The `sched()` API stays the same; the implementation gets a per-CPU indirection.

### `ready()` requires explicit RUNNABLE state

Caller must set `t->state = THREAD_RUNNABLE` before `ready(t)`. `thread_create` does this for fresh threads; `thread_wake` (P2-Bb) does it for sleeping threads. Forgetting to set state → extinct at `ready()`.

### Sorted-list insert is O(N)

For v1.0 expected loads, O(N) insert is fine. If a future workload pushes thread counts into the hundreds, refactor to red-black tree (per ARCH §8.4 design intent). The API doesn't change.

### `band` field defaults to `NORMAL` not `INTERACTIVE`

`SCHED_BAND_INTERACTIVE = 0` and KP_ZERO would default `band = 0 = INTERACTIVE`. To override, `thread_create` and `thread_init` explicitly set `band = SCHED_BAND_NORMAL`. A future caller using `kmem_cache_alloc(thread_cache, KP_ZERO)` directly (bypassing `thread_create`) would get `band = INTERACTIVE` by default. This is a subtle hazard; the convention is "use `thread_create` for all Thread descriptors."

### IRQ-mask discipline at P2-Bc (live)

`sched()` and `ready()` bracket their run-tree mutations with `spin_lock_irqsave(NULL)` / `spin_unlock_irqrestore(NULL, s)`. The `lock` argument is NULL — at v1.0 UP there's no contention, but the IRQ mask is real (PSTATE.DAIF.I) and is what defends against timer IRQ → preempt_check_irq → sched() recursion mid-run-tree-mutation. P2-C SMP makes the spin part real with a per-CPU run-tree lock.

### `prev->state = RUNNABLE` is no longer assumed (P2-Bb refactor)

`sched()` now respects pre-set `prev->state`: RUNNING → yield-insert; SLEEPING/EXITING → don't insert. Callers like `sleep()` set `state = THREAD_SLEEPING` BEFORE calling `sched()`, and the SLEEPING-state branch leaves prev out of the run tree. See [16-rendez.md](16-rendez.md) for the wait/wake protocol.

---

## Preemption (P2-Bc)

The scheduler now drives preemption via the timer IRQ.

### Mechanism

1. **Per-thread slice** — `struct Thread::slice_remaining` (s64) holds the remaining ticks before this thread's quantum expires. Replenished to `THREAD_DEFAULT_SLICE_TICKS = 6` (matching Linux EEVDF default at 1000 Hz) on every RUNNABLE → RUNNING transition (sched()'s pick-next path).

2. **`sched_tick()`** — called from `timer_irq_handler` on every timer fire. Decrements `current->slice_remaining`; when ≤ 0, sets `g_need_resched = true` and replenishes the slice (so the slice doesn't go further negative on subsequent ticks before `preempt_check_irq` runs).

3. **`preempt_check_irq()`** — called from `arch/arm64/vectors.S` IRQ slot, AFTER `exception_irq_curr_el` returns and BEFORE `.Lexception_return` (the eret trampoline). Checks `g_need_resched`; if set + scheduler is initialized + `current_thread` is valid + magic is good, clears the flag and calls `sched()`. `sched()` does its own irqsave/irqrestore + state transitions + cpu_switch_context.

4. **`thread_trampoline` IRQ unmask** — the very first time a fresh thread runs, it lands at `thread_trampoline` (not at sched()'s resumption point). Since it doesn't have a sched() frame to call irqrestore from, we explicitly `msr daifclr, #2` at the start of `thread_trampoline` to unmask IRQs. This makes timer-IRQ-driven preemption fire on the new thread once it starts running. Subsequent re-entries to a preempted thread go through sched()'s frame (which handles irqrestore correctly).

### Spec mapping

The C impl's preempt path (`timer_irq_handler` → `sched_tick` → `g_need_resched` → `preempt_check_irq` → `sched`) maps to the spec's `Yield(cpu)` action — non-deterministic in TLC, observably indistinguishable from cooperative yield. The atomicity that matters for `NoMissedWakeup` is preserved: `sleep()`'s `spin_lock_irqsave` brackets the WaitOnCond body, so a preempt cannot fire mid-cond-check-or-sleep-transition.

LatencyBound (ARCH §28 I-17, slice × N) is a liveness property requiring weak fairness; deferred to a Phase 2 close refinement of `scheduler.tla`.

### Trip-hazards

- **`sched()` deadlock detection assumes UP-no-other-CPU**. With `prev->state == SLEEPING` AND empty run tree, `sched()` extincts (`sched: deadlock — current is blocking, no runnable peer`). At v1.0 P2-Bc single-CPU UP this is correct (only an IRQ could wake, IRQs are masked). P2-C lands SMP idle-WFI: an idle CPU with no runnable thread parks at WFI, gets woken via IPI / IRQ — extinction narrows to genuine deadlocks.

- **`thread_trampoline`'s IRQ unmask is asymmetric**: the FIRST entry to a thread unmasks; subsequent re-entries inherit DAIF from cpu_switch_context's saved state (which sched()'s irqrestore correctly handles). Don't add a second irqsave/irqrestore around the trampoline body — it would conflict with sched()'s discipline.

- **`g_need_resched` is `volatile bool`, not atomic**. UP single-CPU at v1.0: no race (writer is the timer IRQ handler, reader is the same CPU's preempt_check_irq, both serialized via the IRQ mask). P2-C SMP needs `_Atomic(bool)` with release/acquire ordering for cross-CPU need_resched signals (or a per-CPU need_resched + IPI_RESCHED).

- **`sched_tick` skips when `t->state != THREAD_RUNNING`** — defensive: if current's state somehow became RUNNABLE/SLEEPING between the start of an IRQ and sched_tick's read, don't decrement (the next sched_tick will catch the corrected state). This shouldn't happen at v1.0 UP (the only writers to current's state are sched() itself and sleep/wakeup, both with IRQ masked) but the defensive read is cheap.

- **Slice replenish happens on RUNNABLE → RUNNING transition** (sched()'s pick-next path). NOT on `ready()` — a thread that's been ready'd but not yet picked retains whatever slice_remaining it had previously. This is correct: the slice represents "remaining time this quantum" and a thread that hasn't run yet hasn't consumed any.

---

## Per-CPU dispatch (P2-Cd)

P2-Cd makes the scheduler per-CPU. Each CPU dispatches independently against its own run tree, vd_counter, and need_resched signal. The boot CPU's behavior is unchanged at the test level (all tests run on CPU 0 and observe the same band-rotation semantics); secondaries gain idle-thread infrastructure without yet receiving cross-CPU work (that's P2-Cdc).

### State

Replaces the global singletons:
```c
struct CpuSched {
    struct Thread *run_tree[SCHED_BAND_COUNT];
    s64            vd_counter;
    bool           initialized;
    struct Thread *idle;            // CPU 0 = kthread; secondary = fresh
};
static struct CpuSched g_cpu_sched[DTB_MAX_CPUS];
static volatile bool   g_need_resched[DTB_MAX_CPUS];   // per-CPU IRQ flag
```

`smp_cpu_idx_self()` returns this CPU's index from `MPIDR_EL1.Aff0`. All sched.c entry points fan in through `this_cpu_sched()` to read/write the appropriate slot.

### sched_init takes a cpu_idx

`sched_init(cpu_idx)` is called once per CPU:
- `main.c` calls `sched_init(0)` for the boot CPU after `thread_init`. The boot CPU's idle is `kthread`.
- `per_cpu_main` (kernel/smp.c) calls `sched_init(idx)` for each secondary after parking its idle Thread in TPIDR_EL1 (via `thread_init_per_cpu_idle(idx)`).

The function records `current_thread()` as the per-CPU idle; the run tree starts empty.

### ready / sched on per-CPU trees

- `ready(t)` inserts into THIS CPU's run tree (the one that owns the call site at the moment of insertion). Cross-CPU placement (e.g., from a wakeup whose target lives on a different CPU) is a P2-Ce work-stealing concern.
- `sched()` picks from THIS CPU's tree. The "no peer runnable" path is unchanged from P2-Bc — yield-with-no-peer refills the slice and returns; block/exit-with-no-peer extincts (idle is a regular RUNNING thread that's always re-inserted on yield, so the deadlock path is unreachable in practice unless the idle itself blocks).
- `sched_remove_if_runnable(t)` walks every CPU's tree to find which one owns `t`. At v1.0 a thread is in at most one tree; the same scan generalizes to P2-Ce migration.
- `sched_runnable_count()` aggregates across all CPUs — diagnostic-only; not a hot path.

### Per-CPU idle thread

Two flavors at v1.0 P2-Cd:
- **CPU 0 (boot)**: `kthread`, set up by `thread_init` in main.c. It serves dual roles — boot-time test runner AND CPU 0's idle. After tests finish, kthread runs `_hang()` (WFI loop) which IS the boot CPU's idle.
- **CPU N (secondary)**: a fresh `Thread` allocated by `thread_init_per_cpu_idle(idx)` in `per_cpu_main`. Doesn't own a kstack — it runs on the per-CPU boot stack assigned by `secondary_entry`. Lives in `SCHED_BAND_IDLE` so it sorts below any NORMAL-band runnable thread.

`sched_idle_thread(cpu_idx)` exposes the per-CPU idle pointer for diagnostic + test use.

### per_cpu_main idle loop

At v1.0 P2-Cd:
```c
for (;;) {
    __asm__ __volatile__("wfi" ::: "memory");
}
```
— a pure WFI park. P2-Cdc lands the GIC SGI infrastructure that lets the boot CPU send `IPI_RESCHED` to a secondary; the loop body then becomes:
```c
for (;;) {
    sched();
    __asm__ __volatile__("wfi" ::: "memory");
}
```
— sched() yields to peers placed by IPI-driven cross-CPU wakeups, falling back to WFI when nothing is queued. We do NOT install this loop body at P2-Cd because WFI on QEMU returns on architectural events even with masked IRQs — without IPIs as a wake source, sched() would spin without ever scheduling productive work, inflating boot time from ~100 ms to >1000 ms.

### Tests added

`smp.per_cpu_idle_smoke` (test_smp.c): verifies `sched_idle_thread(0)` equals `kthread()`, secondaries' idles are non-NULL Thread objects in `SCHED_BAND_IDLE`, and slots beyond `smp_cpu_count()` return NULL.

---

## Work-stealing + finish_task_switch (P2-Ce)

P2-Ce extends the per-CPU scheduler with cross-CPU thread placement (work-stealing) and the lock handoff a stolen thread needs to resume cleanly on the new CPU.

### Real spinlocks

`<thylacine/spinlock.h>`'s `spin_lock`/`spin_unlock`/`spin_trylock` use `__atomic_exchange_n` with acquire/release ordering. On ARMv8.1+ this lowers to `swpa` (LSE single-instruction); on ARMv8.0 to `ldaxr`/`stxr` (LL/SC). The fast-path "spin on relaxed load while held" avoids hammering the contended cacheline with LSE traffic.

`spin_lock_irqsave`/`spin_unlock_irqrestore` now thread NULL through gracefully — `NULL` lock skips the spin part (still does IRQ mask/restore). Preserves the prior contract for callers that used the API as "IRQ mask only."

### Per-CPU run-tree lock

`struct CpuSched.lock` protects each CPU's run tree. Operations:
- `ready(t)`: take THIS CPU's lock, insert, drop. Cross-CPU placement is the work-stealer's job.
- `sched()`: take THIS CPU's lock, do everything (pick/steal/insert prev/set next), drop on resume.
- `sched_remove_if_runnable(t)`: scan all CPUs, take each lock to check membership.
- `try_steal(cs)`: holds `cs`'s lock; uses `spin_trylock` to grab peer locks (avoids deadlock + back-pressure).

### try_steal logic

```c
static struct Thread *try_steal(struct CpuSched *cs) {
    unsigned self = (unsigned)(cs - g_cpu_sched);
    for (unsigned i = 0; i < DTB_MAX_CPUS; i++) {
        if (i == self) continue;
        struct CpuSched *peer = &g_cpu_sched[i];
        if (!peer->initialized) continue;
        if (!spin_trylock(&peer->lock)) continue;
        struct Thread *stolen = NULL;
        for (unsigned b = 0; b < SCHED_BAND_COUNT && !stolen; b++) {
            stolen = peer->run_tree[b];
            if (stolen) unlink(peer, stolen);
        }
        spin_unlock(&peer->lock);
        if (stolen) {
            stolen->vd_t = cs->vd_counter++;   // rebase to local clock
            return stolen;
        }
    }
    return NULL;
}
```

Called in sched() when `pick_next(cs)` returns NULL — before falling back to "yield with no peer."

### finish_task_switch handoff

The race: per-CPU lock must be held across `cpu_switch_context` (otherwise a peer's `try_steal` could pull `prev` mid-save). On resume, the thread may have migrated to a different CPU. The `cs` variable in the resuming frame is stale.

Solution: `cs->pending_release_lock`. Prev's sched sets it just before the switch:
```c
cs->pending_release_lock = &cs->lock;
cpu_switch_context(&prev->ctx, &next->ctx);
// On resume — possibly on a different CPU.
struct CpuSched *cs_now = this_cpu_sched();
spin_lock_t *lk = cs_now->pending_release_lock;
cs_now->pending_release_lock = NULL;
spin_unlock(lk);
__asm__ __volatile__("msr daif, %x0\n" :: "r"(s) : "memory");
```

The destination CPU's `pending_release_lock` was set by whoever switched-to-us on this CPU — exactly the lock we need to release.

For fresh threads (first run via `thread_trampoline`), the same handoff applies. `thread_trampoline` (asm) calls `sched_finish_task_switch`:
```c
void sched_finish_task_switch(void) {
    struct CpuSched *cs = this_cpu_sched();
    spin_lock_t *lk = cs->pending_release_lock;
    cs->pending_release_lock = NULL;
    if (lk) spin_unlock(lk);
}
```

NULL-guard handles `thread_switch` (P2-A direct-switch primitive used by context tests) which doesn't acquire the per-CPU lock.

### Tests

`smp.work_stealing_smoke`: boot creates N threads, ready()s on its tree, sends IPI_RESCHED to each secondary; secondaries' WFI wakes, post-WFI sched() finds local tree empty and try_steals from boot. Test threads increment per-CPU counters; verify at least one secondary's counter > 0. Threads sleep on per-thread Rendezes after exit signal so thread_free can safely reap them in SLEEPING state.

---

## WFI-aware work-stealing (P3-G)

Closes R5-H **F77** (try_steal extincts on transient peer-lock contention) + **F78** (ready/wakeup don't IPI an idle peer in WFI). Without these fixes, secondaries — which have no per-CPU timer at v1.0 — can sit in WFI indefinitely while runnable work waits on a peer's tree.

### Mechanism

Three pieces collaborate:

1. **idle_in_wfi flag** (`struct CpuSched.idle_in_wfi`). Volatile bool per-CPU. Set TRUE by THIS CPU immediately before its `wfi` instruction in `per_cpu_main`'s idle loop; cleared FALSE immediately after WFI exits. Boot CPU stays FALSE forever (boot's post-init flow is `_hang`'s asm wfi loop, no C-level set hook). Maps to scheduler.tla's `wfi[cpu]` variable + `EnterWFI(cpu)` action.

2. **`sched_notify_idle_peer()`**. Walks `g_cpu_sched[i]` for `i ≠ self`; first peer with `idle_in_wfi==true` receives `gic_send_ipi(IPI_RESCHED)`. Stops on first send (single-peer wake; multiple wakes would be a thundering herd). Called from `ready()` AFTER releasing the local lock so the peer's IPI handler doesn't contend. Maps to scheduler.tla's `NotifyWFIPeer(src, dst)` action.

3. **`try_steal` contention sentinel + retry**. `try_steal(cs, &contended_out)` sets `*contended_out` TRUE if at least one peer was skipped due to `spin_trylock` failure. `sched()` distinguishes "all peers genuinely empty" from "some peer's lock momentarily held": on the SLEEPING-prev path, NULL+contended triggers a brief CPU-relax + one retry before extinction. Closes F77's spurious-extinction false positive.

### Test-mode toggle

`sched_set_notify_enabled(bool)` gates `sched_notify_idle_peer()`. Disabled by default during in-kernel tests so they keep their UP-like single-CPU assumptions (e.g., `rendez.basic_handoff` assumes the readied consumer runs on the same CPU that readied it — work-stealing breaks that assumption). Boot calls `sched_set_notify_enabled(true)` AFTER `test_run_all()` and BEFORE `init_run()`. Once enabled, every ready/wakeup placing work wakes an idle peer.

### Boot-flow cadence

```
boot_main()
   │
   ├── ... bringup ...
   ├── test_run_all()              # in-kernel tests; UP-like (notify off)
   ├── fault_test_run()
   │
   ├── sched_set_notify_enabled(true)   ← P3-G: SMP behavior live
   │
   ├── init_run()                  # /init runs; ready/wakeup IPI peers
   │
   └── "Thylacine boot OK"
```

Production /init's `wait_pid` benefits: when /init's child exits + parent sleeps + child eventually wakes parent via wakeup, the wakeup IPIs an idle secondary if any. Cross-CPU placement becomes real.

### Spec cross-reference

`specs/scheduler.tla` extended with:

- **`wfi: [CPUs -> BOOLEAN]`** variable.
- **`EnterWFI(cpu)`** action: pre `current[cpu]=NULL ∧ runq[cpu]={} ∧ ~wfi[cpu] ∧ Cardinality(CPUs)>1`. Effect: `wfi[cpu]=TRUE`. Multi-CPU only — single-CPU configs can't IPI themselves; existing 1C tests rely on Resume/Yield/WakeAll fairness to avoid this state.
- **`NotifyWFIPeer(src, dst)`** action: pre `wfi[dst] ∧ ∃c: runq[c]≠{}`. Sends an IPI; structurally identical to `IPI_Send` with stronger precondition.
- **`Resume(cpu)`** + **`Steal(stealer, victim)`** preconditions strengthened with `~wfi`.
- **`IPI_Deliver(src, dst)`** + **`BuggyIPI_Deliver`** also clear `wfi[dst]=FALSE` — IPI delivery wakes the dst from WFI.
- **`Liveness_Wfi`**: `Liveness ∧ ∀pair: SF(NotifyWFIPeer(pair)) ∧ SF(IPI_Deliver(pair))`. Strong fairness on both clauses ensures a WFI'd CPU never starves a runnable thread.
- **`Spec_Live_Wfi == Init ∧ [][Next] ∧ Liveness_Wfi`**.

Configs:

- `scheduler_liveness_wfi.cfg` (correct): `SPECIFICATION Spec_Live_Wfi`. 3T × 2C. LatencyBound holds — proves the fix is sound. ~5760 distinct states.
- `scheduler_buggy_wfi.cfg` (buggy): `SPECIFICATION Spec_Live` (no WFI fairness). Same 3T × 2C. LatencyBound violated — TLC produces a counterexample showing a CPU stuck in WFI while a thread starves on a peer's runq. Proves the fix is necessary.

Existing buggy configs (`scheduler_buggy.cfg`, `scheduler_buggy_steal.cfg`, `scheduler_buggy_ipi.cfg`, `scheduler_buggy_starve.cfg`) still produce their original counterexamples (P3-G's wfi extension is orthogonal). Existing `scheduler.cfg` state count grew from 10188 → 25416 due to `wfi` adding a new state dimension.

### Tests

- **`scheduler.idle_in_wfi_observability`** — accessor smoke. Out-of-range cpu_idx returns false; boot CPU stays FALSE (no wfi loop hook); after settle window, at least one secondary reports `idle_in_wfi==true`.
- **`scheduler.notify_idle_peer_smoke`** — load-bearing. Toggle notify on; ready a thread on boot; verify a secondary's `g_ipi_resched_count[i]` increments. Restores toggle off for subsequent tests.
- **`scheduler.notify_disabled_no_ipi`** — UP-like default. Toggle notify off; ready a thread; verify NO secondary IPIs fired. Confirms tests stay in UP-like mode.

### Trip-hazards

- **WFI-aware test mode**: tests assume `sched_set_notify_enabled(false)` (the default at boot, restored by P3-G tests after toggling on). New tests that depend on cross-CPU placement must explicitly enable. Tests that don't care should NOT toggle.
- **First-ready-on-secondary loop**: when /init runs (notify enabled), the first ready/wakeup wakes one secondary. Subsequent ready/wakeup on the same CPU — if no other secondary is in WFI — falls through. The selection is rotation-style (self+1, self+2, ...); under sustained load, IPIs distribute across all online secondaries.
- **try_steal retry budget**: 256-iteration CPU-relax bounded. Truly-empty system still extincts in finite time; transient contention absorbed. Cap is conservative (microseconds at typical clock rates).
- **The boot CPU is intentionally NOT a wake target**: its post-init flow is `_hang`'s asm wfi loop with no C-level idle_in_wfi hook. If a future change adds a sched/wfi loop on boot post-init (like Linux's idle task), this hook needs adding. Today's invariant: only secondaries are wake candidates.

---

## Build + verify

```bash
# Build + tests (production: 13/13 PASS expected)
tools/build.sh kernel
tools/test.sh

# UBSan
tools/build.sh kernel --sanitize=undefined
tools/test.sh --sanitize=undefined

# Hardening matrix
tools/test-fault.sh                    # 3/3 expected

# KASLR
tools/verify-kaslr.sh -n 5             # 5/5 distinct expected

# All TLA+ specs (1: scheduler.tla; primary + 3 buggy configs)
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config scheduler.cfg scheduler.tla              # 10188 states clean
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config scheduler_buggy.cfg scheduler.tla        # NoMissedWakeup counterexample at depth 6
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config scheduler_buggy_steal.cfg scheduler.tla  # NoDoubleEnqueue counterexample at depth 4
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config scheduler_buggy_ipi.cfg scheduler.tla    # IPIOrdering counterexample at depth 6
```
