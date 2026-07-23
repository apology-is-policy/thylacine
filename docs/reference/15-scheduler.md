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
| **SMP redesign: per-CPU `cpu_pinned` in-tree idle (retires `g_bootcpu_idle`); `idle_in_wfi` F7 fix; steal/pick invariant assert** | **Landed (deep-smp-review #863; ARCH §8.4.2/§8.4.5; gate `specs/sched_alpha.tla`; closes #860)** |
| **HMP foundation: `select_target_cpu` placement hook + `ready_on` cross-CPU enqueue + per-CPU `capacity` (DTB) + per-task `util` + `balance_pull`** | **Landed (deep-smp-review #864; ARCH §8.4.3/§8.4.4; logic-verified vs synthetic asymmetric DTB; inert on uniform v1.0 targets)** |
| **#866 formal adversarial audit (Opus + self-audit) of #863+#864** | **CLOSED CLEAN (0 P0 + 1 P1 + 0 P2 + 5 P3); F1 cross-CPU `need_resched` + F2 steal band-walk + F3 capacity release/acquire + F4 self-OOB guard + F6 doc fixed; F5 deferred (dormant `r->lock` coupling). `memory/audit_smp_redesign_closed_list.md`** |
| **TI-4 work-conservation under tickless: push-placement (TI-4b) + the busy-tick overload kick (TI-4c, `sched_rebalance.tla::Overload`) + the affinity-ready `thread_may_run_on` seam + the 4 ms re-poll backstop (TI-4e)** | **Landed (TI arc; #304/#307/#309). Focused Opus-4.8-max audit + SMP gate. The tickless boot slowdown root-caused to HVF deep-park vCPU resume latency (99.85% IPI wakes — not a guest bug; the design is correct for the bare-metal target). See "Work-conservation under tickless idle (TI-4)" above.** |
| `LatencyBound` liveness spec | Phase 2 close |
| Red-black tree refactor | Phase 7 |
| **Empirical EAS tuning (PELT decay, energy model, schedutil/DVFS, misfit push)** | **Deferred to real heterogeneous HW (ARCH §8.4.4 verification boundary)** |

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

> **#107 invariant — read the per-CPU slot under the IRQ mask.** `this_cpu_sched()` is a per-CPU pointer keyed on the *live* CPU; reading it and then acting on it (acquiring `cs->lock`, advancing `cs->vd_counter`, …) is a TOCTOU on the CPU identity if the thread can migrate in between. `sched()` therefore masks IRQs (`msr daifset, #2`) **before** `cs = this_cpu_sched()`: with IRQs masked there is no preempt → no switch-out → no work-steal, so the CPU is pinned and `cs` stays valid until `cpu_switch_context` (after which the resume path deliberately re-reads `this_cpu_sched()` as `cs_now`). Reading `cs` with IRQs enabled was the #104 SMP deadlock (a `sched()` entered unmasked — e.g. a kthread yield — migrated mid-call and then acquired the *origin* CPU's run-queue lock while running elsewhere, leaking it via the `pending_release_lock` handoff).

### sched_init takes a cpu_idx

`sched_init(cpu_idx)` is called once per CPU:
- `main.c` calls `sched_init(0)` for the boot CPU after `thread_init`. The boot CPU's idle is `kthread`.
- `per_cpu_main` (kernel/smp.c) calls `sched_init(idx)` for each secondary after parking its idle Thread in TPIDR_EL1 (via `thread_init_per_cpu_idle(idx)`).

The function records `current_thread()` as the per-CPU idle; the run tree starts empty.

### ready / sched on per-CPU trees

- `ready(t)` inserts into THIS CPU's run tree (the one that owns the call site at the moment of insertion). Cross-CPU placement (e.g., from a wakeup whose target lives on a different CPU) is a P2-Ce work-stealing concern.
- `sched()` picks from THIS CPU's tree. Yield-with-no-peer refills the slice and returns. Block/exit-with-no-peer: under the SMP redesign (ARCH §8.4.2) every CPU has a pinned **in-tree** idle in `run_tree[IDLE]`, so `pick_next` always finds at least the idle — the old `"sched: deadlock"` extinction is now structurally unreachable (kept only as a defensive backstop for the boot window before `sched_install_bootcpu_idle` or a secondary mis-init).
- `sched_remove_if_runnable(t)` walks every CPU's tree to find which one owns `t`. At v1.0 a thread is in at most one tree; the same scan generalizes to P2-Ce migration.
- `sched_runnable_count()` aggregates runnable WORK across all CPUs — diagnostic-only; not a hot path; consulted by no scheduling decision. It EXCLUDES `SCHED_BAND_IDLE`: the per-CPU idle threads are always-runnable infrastructure. Under the SMP redesign (ARCH §8.4.2) EVERY CPU's idle — cpu0's `bootcpu_idle` AND each secondary's — is an ordinary in-tree `BAND_IDLE` thread, so the band filter uniformly excludes them all (the old off-tree `g_bootcpu_idle` is retired); not pending work. Counting them reported a phantom backlog on an idle multi-CPU system and made the in-kernel `sched_runnable_count()==0` quiescence assertions race a secondary idle thread that — under host load — was in-tree (not the running thread) at the check instant. That was the #857 "smp8 cons.* flake": never `console_mgr`, never a kernel fault, just a benign idle thread miscounted as work. `sched_runnable_count_band(b)` is the unfiltered per-band query (still counts IDLE). Real work (`BAND_INTERACTIVE`/`BAND_NORMAL`) is always counted, so a genuinely stranded work thread is never masked. Regression: `scheduler.runnable_count_excludes_idle`.

### Per-CPU idle thread (SMP redesign, ARCH §8.4.2)

Every CPU — **including the boot CPU** — has its own idle thread that is (a) **`cpu_pinned`** (`try_steal` never migrates a pinned thread) and (b) **in-tree** (it lives in `run_tree[SCHED_BAND_IDLE]` and is dispatched by the ordinary `pick_next`, exactly like any other thread). This **retires the old off-tree `g_bootcpu_idle` + its deadlock-path dispatch**, which was the **#860** root cause.

- **CPU 0 (boot)**: `bootcpu_idle`, allocated by `thread_create_bootcpu_idle(bootcpu_idle_main, smp_bootcpu_idle_stack_top())` in `boot_main` and readied into cpu0's `run_tree[IDLE]` by `sched_install_bootcpu_idle` (which also records it as `g_cpu_sched[0].idle`, overriding the `kthread` placeholder `sched_init(0)` set). It runs on a **dedicated BSS stack** (`g_bootcpu_idle_stack`, a `struct secondary_stack` — leading **guard page** + 16 KiB usable — `kstack_base == NULL`) symmetric with the secondaries' boot stacks; cpu0's `_boot_stack` belongs to `kthread` (which after bringup blocks in `joey_run`/`wait_pid`, leaving its frame on `_boot_stack`). The guard page is mapped no-access by `build_page_tables` (bounds-checked fail-loud) + recognized by `fault.c::stack_guard_overflow_msg` → `"kernel stack overflow (bootcpu-idle guard)"`, exactly like the secondary guards (#867; proven via `tools/test-fault.sh bootcpu_idle_guard`). Unlike a secondary's idle (born running `per_cpu_main`), cpu0's idle is switched **into** when kthread first blocks, so it carries first-switch-in ctx (`thread_trampoline` → `blr bootcpu_idle_main`).
- **CPU N (secondary)**: a fresh `Thread` allocated by `thread_init_per_cpu_idle(idx)` in `per_cpu_main`, born running (it **is** `per_cpu_main`). `kstack_base == NULL` (runs on the per-CPU boot stack from `secondary_entry`), `cpu_pinned == true`.

`kthread` is **also** `cpu_pinned` (it runs on cpu0's `_boot_stack`) — the same boot-stack-thread non-migration rule. So `cpu_pinned` is the single clean unstealability predicate that **generalizes** the secondaries' old `kstack_base==NULL` skip and **replaces** both it and the `g_bootcpu_idle` special case in `try_steal`'s gate (`if (cand && !cand->cpu_pinned)`).

**Why this is sound by construction (closes #860 + the entire off-tree class):** the per-CPU idle is always *either* current *or* in its CPU's `run_tree[IDLE]` (it is displaced when a non-idle thread is picked, and the unconditional yield-insert puts it back). So whenever a non-idle thread is current, its CPU's idle is in the tree — `pick_next` always finds at least the idle, making the old `"sched: deadlock"` condition structurally impossible (kept only as a defensive, unreachable extinction). And a pinned idle never enters a peer's `try_steal` consideration, so the double-run that #860 exploited cannot arise. Modeled by `specs/sched_alpha.tla` (`IdleAvailable` + `IdleStaysHome`).

`sched_idle_thread(cpu_idx)` exposes the per-CPU idle pointer for diagnostic + test use (now `bootcpu_idle`, not `kthread`, for cpu0).

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

**As-built gate (SMP redesign, ARCH §8.4.2/§8.4.5):** the per-band candidate is taken only if `!cand->cpu_pinned` (skip the per-CPU idles + kthread, which run on a static boot/idle stack belonging to one CPU); the victim is then `ASSERT_OR_DIE`'d `state==RUNNABLE && !on_cpu` (the run-tree-holds-only-off-cpu-threads invariant, fail-loud), and claimed `on_cpu` under `peer->lock` before the unlock (the #801/F1 steal-in-flight close). `pick_next` carries the same victim assert.

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

#### `thread_switch` IRQ-mask discipline (#101)

`thread_switch` brackets its entire mutate+switch+resume window with `spin_lock_irqsave(NULL)` / `spin_unlock_irqrestore(NULL, sw_irq)`, exactly like `sched()` (see the IRQ-mask discipline above). This is **load-bearing**, not cosmetic: `thread_switch` does `set_current_thread(next)` **before** `cpu_switch_context`, leaving a torn state (`current_thread()==next` while the CPU still runs `prev`'s SP + registers). The test phase runs IRQs-unmasked (`main.c` `daifclr #2`) and preemptible (`vectors.S` IRQ-from-EL1 tail → `preempt_check_irq` → `sched()`), so a timer tick landing in that window drove a nested `sched()` that saved `prev`'s live state into `next->ctx` → a stack-canary smash (the `context.round_trip` smash root-caused at #101 — **distinct from** the directmap-BBM guard-page fault of F-B/#806, which was fixed at d80e160). Pre-#101 the invariant was assumed by a comment but never enforced (the primitive was written for single-CPU + no-preemption).

The mask is balanced **per-thread**: `sw_irq` rides `prev`'s kstack (preserved across the switch by `cpu_switch_context`'s SP save/restore) and is restored when `prev` is switched back to. `cpu_switch_context` does not touch DAIF, so the mask is carried across the switch by the live CPU; a **fresh** `next` reached via `ret` runs `thread_trampoline`, which `msr daifclr #2`s **before** its entry — so the entry stays preemptible, only the switch itself is atomic. Both `cpu_switch_context` callers (`thread_switch` + `sched()`) now mask, so no torn-window path remains. Regression: `context.switch_irq_safe` (a test-only `g_thread_switch_test_window_ns` hook forces a preempt into the window; non-vacuous — reverting the mask smashes deterministically).

### Tests

`smp.work_stealing_smoke`: boot creates N threads, ready()s on its tree, sends IPI_RESCHED to each secondary; secondaries' WFI wakes, post-WFI sched() finds local tree empty and try_steals from boot. Test threads increment per-CPU counters; verify at least one secondary's counter > 0. Threads sleep on per-thread Rendezes after exit signal so thread_free can safely reap them in SLEEPING state.

---

## WFI-aware work-stealing (P3-G)

Closes R5-H **F77** (try_steal extincts on transient peer-lock contention) + **F78** (ready/wakeup don't IPI an idle peer in WFI). Without these fixes, secondaries can sit in WFI while runnable work waits on a peer's tree.

> **Update (#810): secondaries now run a per-CPU timer in production.** When this section was written, secondaries had no per-CPU timer, so the WFI-wake IPI was the *only* way an idle secondary discovered stealable work, and a CPU-bound EL0 thread on a secondary could never be preempted (it monopolized the CPU — the pouch-hello-exitgroup boot hang). As of #810 each secondary arms its own banked generic timer (`timer_arm_this_cpu`) at the production transition (`smp_enable_secondary_preemption()`, called from `boot_main` after the UP-like in-kernel test suite). This gives secondaries a ~1 kHz preemptive tick (invariants I-8 / I-17 now hold on every CPU) **and** an independent self-wake that strengthens stealable-work discovery — the F78 WFI-wake IPI remains as the *prompt* (low-latency) path, while the timer is the *floor*. The arming is DEFERRED past `test_run_all()` (mirroring the `sched_set_notify_enabled` gate) so the deterministic, single-CPU-scheduled in-kernel tests stay quiescent — arming during the test phase let a secondary self-wake and steal a test thread, surfacing as `thread_free of RUNNING thread` in `scheduler.preemption_smoke`. See `docs/reference/11-timer.md` (`timer_arm_this_cpu`) and `kernel/smp.c::smp_enable_secondary_preemption`.

### Mechanism

Three pieces collaborate:

1. **idle_in_wfi flag** (`struct CpuSched.idle_in_wfi`). Volatile bool per-CPU. Set TRUE by THIS CPU immediately before its `wfi` instruction in `per_cpu_main`'s idle loop; cleared FALSE immediately after WFI exits. Boot CPU stays FALSE forever (boot's post-init flow is `_torpor`'s asm wfi loop, no C-level set hook). Maps to scheduler.tla's `wfi[cpu]` variable + `EnterWFI(cpu)` action.

2. **`sched_notify_idle_peer()`**. Walks `g_cpu_sched[i]` for `i ≠ self`; first peer with `idle_in_wfi==true` receives `gic_send_ipi(IPI_RESCHED)`. Stops on first send (single-peer wake; multiple wakes would be a thundering herd). Called from `ready()` AFTER releasing the local lock so the peer's IPI handler doesn't contend. Maps to scheduler.tla's `NotifyWFIPeer(src, dst)` action.

3. **`try_steal` contention sentinel + retry**. `try_steal(cs, &contended_out)` sets `*contended_out` TRUE if at least one peer was skipped due to `spin_trylock` failure. `sched()` distinguishes "all peers genuinely empty" from "some peer's lock momentarily held": on the SLEEPING-prev path, NULL+contended triggers a brief CPU-relax + one retry before extinction. Closes F77's spurious-extinction false positive.

### Test-mode toggle

`sched_set_notify_enabled(bool)` gates `sched_notify_idle_peer()`. Disabled by default during in-kernel tests so they keep their UP-like single-CPU assumptions (e.g., `rendez.basic_handoff` assumes the readied consumer runs on the same CPU that readied it — work-stealing breaks that assumption). Boot calls `sched_set_notify_enabled(true)` AFTER `test_run_all()` and BEFORE `joey_run()`. Once enabled, every ready/wakeup placing work wakes an idle peer.

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
   ├── joey_run()                  # /init runs; ready/wakeup IPI peers
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
- **The boot CPU is intentionally NOT a wake target**: its post-init flow is `_torpor`'s asm wfi loop with no C-level idle_in_wfi hook. If a future change adds a sched/wfi loop on boot post-init (like Linux's idle task), this hook needs adding. Today's invariant: only secondaries are wake candidates.

---

## Work-conservation under tickless idle (TI-4)

Tickless idle (TI-3) stopped the 1 kHz periodic tick on a genuinely-idle CPU — but that tick was *also*, silently, the **work-steal re-poll**: every tick an idle CPU re-ran `sched()` → `try_steal` and pulled any queued work within 1 ms (`sched_notify_idle_peer` wakes only ONE peer per `ready()`; the tick was the catch-all for the rest). Removing it stranded queued work until the 100 ms backstop → a **2.4× boot slowdown**. TI-4 restores work-conservation with three composing pieces (the Linux `NO_HZ_IDLE` shape: idle CPUs deep-park; a still-ticking busy CPU drives rebalancing), an affinity-ready seam, and a backstop retune. All are production-only (`g_sched_notify_enabled`); the in-kernel test phase runs periodic idle, byte-identical to pre-tickless.

### The enqueue side: push-placement (TI-4b)

`select_target_cpu(t, prev_cpu)` (the HMP placement hook, above) prefers an **idle peer** for a waking thread in production: `select_idle_target` rotates `g_cpu_sched[]` for the first `idle_in_wfi` peer and `ready_on` enqueues `t` there + `sched_notify_cpu` IPIs it. The common case — a wake with an idle CPU available — runs `t` promptly there, no steal needed.

### The pull side: `try_steal` at idle entry (existing)

When `pick_next` returns NULL (nothing local) a CPU runs `try_steal` *before* parking (the `sched()` in `sched_idle_park`), so a CPU going idle already does one steal pass. The gap tickless opened is the **already-deep-parked** CPU: work that strands on a busy peer *after* a CPU parked has no local re-poll to catch it.

### The push-on-overload kick (TI-4c) — `sched_rebalance.tla::Overload`

`sched_tick()` runs at 1 kHz on every **running** CPU (tickless stops only the *idle* tick). At the tail of the tick, if THIS running CPU holds **surplus** stealable work AND a peer is parked in WFI, it kicks ONE peer to come steal:

```c
struct CpuSched *cs = &g_cpu_sched[cpu];
if (g_sched_notify_enabled && t != cs->idle && cpu_has_surplus_for_kick(cs))
    (void)sched_notify_idle_peer();
```

- `cpu_has_surplus_for_kick(cs)` is a **lock-free** read of the INTERACTIVE + NORMAL `run_tree[]` head pointers (`__atomic_load_n` RELAXED, never deref — the `sched_has_runnable_work` discipline; safe from the IRQ-context tick without the run-tree lock; a racy stale read costs only a benign spurious/skipped kick, self-correcting next tick). A non-NULL head is a RUNNABLE thread queued *behind* the running current (current is unlinked while running) = genuine migratable surplus. `SCHED_BAND_IDLE` is excluded (the pinned idle + at most best-effort IDLE work — never worth a cross-CPU kick).
- The kick **reuses `sched_notify_idle_peer`** — it finds a registered-idle peer (`idle_in_wfi`) and sends `IPI_RESCHED`, "stops on first send" (no thundering herd; never self). It *lifts the peer's park* exactly as a placement IPI does: the peer set `idle_in_wfi` BEFORE its WFI under the IRQ-masked idle region (register-then-observe), so the IPI is never lost (I-9). The migration is **pull-realized** — the kicked peer's `sched()` → `try_steal` pulls the surplus — inside `sched_alpha.tla`'s proven arbitrary-placement envelope.
- `t != cs->idle` suppresses the kick when THIS CPU runs its own idle (no surplus to push — it would run any queued work itself via preempt).

This is structurally the Linux `NO_HZ_IDLE` model. Modeled by `specs/sched_rebalance.tla` (`Overload` / `NoLostWake` / `EventuallyParallelized`; clean + `buggy_nokick` [the TI-3 regression] + `buggy_nolift` [the kick that forgets the park-lift] counterexamples, TLC-green). Regression: `scheduler.cpu_surplus_for_kick`.

### The affinity-ready seam (the priority/affinity pluggable point)

`thread_may_run_on(const struct Thread *t, unsigned cpu)` is the single future plug point for a per-thread affinity mask. v1.0 has no mask, so it is **unconditionally `true`** — a trivially-true gate today that becomes load-bearing the day a `SYS_SCHED_SETATTR` affinity mask lands (then `return (t->affinity_mask >> cpu) & 1u;`). It is consulted at the two CPU-binding decisions so the work-conserving machinery never binds a thread to a forbidden CPU: `select_target_cpu` (placement — the idle-target + capacity-target returns) and `try_steal`'s victim pick (steal — beside the `cpu_pinned` skip). Inert today (always-true → the gates collapse to their pre-change behavior); the future mask plugs into THIS one function, so the redesign does not foreclose affinity. `cpu_pinned` (the idle/kthread hard pin) stays a separate, stronger predicate. The kick is band-aware by construction (it scans the real-work bands; `try_steal` then pulls highest-band-first).

### The #363 park-guard — never park over your own queue (the #33-audit F1)

`sched()` picks **before** it requeues: `pick_next` runs while prev (RUNNING)
is not in the tree; prev is inserted only afterwards. So a slice-expiry
preempt (or a yield) of a thread with an otherwise-empty local queue always
dispatches the pinned in-tree idle, and the preempted thread lands in
`run_tree[NORMAL]` right after the pick. The dispatched idle does not restart
its loop — it resumes inside `sched_idle_park` past its own `sched()` call,
headed for the one-shot arm + WFI, and pre-#363 there was **no re-check of
the local queue** on that resume path: the CPU parked up to
`TICKLESS_IDLE_BACKSTOP_NS` (4 ms) over its own just-requeued RUNNABLE
thread (no IPI exists for a local self-requeue; the one-shot deasserted the
periodic tick). Cost: up to ~4 ms per 6 ms slice for a solo compute-bound
thread. The TI-4d multi-ms `max_starved` records (e.g. the 103 ms
full-backstop park) were this — previously misattributed to peer backlog
("even when that work's home CPU handles it promptly" — false: the home CPU
*was* the parked CPU), and the `scale` bench is structurally blind to it
(a self-ratio; the solo baseline suffers the identical loss).

The fix is the #33 predicate applied at the park commit: after `sched()`
returns in `sched_idle_park`, loop `while (cpu_has_surplus_for_kick(cs))
sched();` before arming the one-shot — two relaxed head loads; the park is
deferred, never lost (a peer's concurrent insert still rides the
`idle_in_wfi` register-then-observe IPI, I-9; the deferred park is a stutter
on `sched_tickless.tla`'s park action, so no spec change). Witness: the
boot-wc `tickless starved` counters (pre-fix ~9.6 s starved-park time per
boot; see the commit for the post-fix numbers).

### The 4 ms re-poll backstop (TI-4e) + the wake-latency finding

`TICKLESS_IDLE_BACKSTOP_NS = 4 ms` (was 100 ms at TI-3) → an idle CPU re-polls at ~250 Hz. **Why:** TI-4e root-caused the residual tickless boot slowdown to wake **latency**, NOT a guest bug. The wake path is IPI-prompt — measured **99.85% of tickless parks woken by an IPI** (`sched_wc_stats.tickless_ipi_wakes` vs `tickless_oneshot_wakes`; `wake-ipi=`/`wake-oneshot=` in the `boot-wc:` banner), not stranding on the backstop. But **resuming a DEEP-parked vCPU via SGI costs ~0.85 ms under HVF vs ~7 µs when hot** — an emulation artifact (HVF GICv2-MMIO vmexits + the host vCPU-thread resume; #299/#890), not a scheduler defect: on bare metal an SGI to a WFI'd core is hardware-fast (~ns), so deep-park there already gives fast boot + ~0% idle. The 4 ms re-poll keeps the dev-loop vCPUs warm enough that IPI resumes drop to ~0.09 ms → HVF boot ~7 s (≈ tickful) vs ~17–35 s at 100 ms, at ~5% HVF idle (vs 0.3%). The kick still earns its place: it is the fast (~1 ms) parallel-surplus catch, validated by the `cpubench` work-conservation counter (starvation −34…−60% across the parallel modes) even though the *sequential* boot is dominated by the per-wake latency the re-poll mitigates. A v1.x adaptive (warm-while-active / deep-when-idle) or accel-gated backstop reclaims 0.3% HVF idle without the dev-boot cost. (Bare-metal confirmation is owed at the Lazarus/RPi bring-up.)

### Telemetry (`/ctl/sched` + the `boot-wc:` banner)

`struct sched_wc_stats` (read by `sched_wc_stats`) carries the work-conservation counters sampled in `sched_idle_park`: `park_events` / `idle_ns` (denominators), `starved_events` / `starved_ns` / `max_starved_ns` (a park committed while work was queued — the steal-gap signal), the `tickless_*` subset (production only; the regression lives there), and the TI-4e wake-source split `tickless_ipi_wakes` / `tickless_oneshot_wakes` (the wake-path health signal). `cpubench` (`/bin/cpubench`) reads the deltas; the boot-window snapshot is the `boot-wc:` banner line.

---

## Voluntary yield — `SYS_YIELD` / `sched_yield_hint` (#33)

`SYS_YIELD` (87) is the EL0 entry to the yield transition `sched()` has always
implemented (prev `RUNNING` → requeue at the back of the band + dispatch — the
`sched_alpha.tla` `StartSwitch kind="yield"` action, the same one tick
preemption drives). Until #33 nothing but preemption drove it from EL0; the Go
runtime's `osyield` was a `torpor_wait` mismatch-return that made a syscall
round-trip without ever giving up the CPU, degrading the spinbit-mutex passive
tier and every runtime spin loop (36.8M calls per `go build`).

`sched_yield_hint()` (`kernel/sched.c`) is the whole mechanism:

```c
bool sched_yield_hint(void) {
    struct CpuSched *cs = this_cpu_sched();
    if (!cpu_has_surplus_for_kick(cs)) return false;
    sched();
    return true;
}
```

- **The fast path is the point.** The per-CPU pinned idle is ALWAYS in-tree
  while a non-idle thread runs (`IdleAvailable`), so an unconditional `sched()`
  on an otherwise-empty queue would dispatch the idle — two context switches
  per call (thread → idle → the #363 park-guard re-dispatches the requeued
  yielder), on a syscall issued from spin loops. Pre-#363 the cost was far
  worse: the dispatched idle resumed inside `sched_idle_park` headed for the
  WFI and parked up to the backstop over the requeued yielder (the #33-audit
  F1 — see "The #363 park-guard" under TI-4 below). The peek reuses the TI-4c
  `cpu_has_surplus_for_kick` predicate verbatim: INTERACTIVE + NORMAL head
  pointers, relaxed loads, never a `Thread` deref. Band IDLE is deliberately
  excluded — it holds only the pinned idle (+ at most best-effort work the
  tick path serves at slice granularity); yielding to it is never useful.
- **Advisory, racy in both directions, both benign**: a stale non-NULL costs
  one wasteful-but-correct idle bounce inside `sched()` (which re-picks under
  `cs->lock`; the #363 park-guard re-dispatches and the idle parks only on a
  genuinely-empty queue); a stale NULL skips one yield — the placer's
  wake-preemption `need_resched_set` is served at this very syscall's return
  tail (`preempt_check_irq` runs on the EL0 sync-return path in `vectors.S`,
  not merely "the next IRQ"), and yield callers loop. The CPU-identity
  staleness note (the #104/#107 TOCTOU shape) is HYPOTHETICAL today: syscalls
  run IRQ-masked end-to-end (`spinlock.h`), so no preempt can land inside the
  peek from the SVC path, and the test callers run on the `cpu_pinned`
  kthread; a future IRQ-enabled kthread caller migrating mid-peek would read
  a foreign CPU's heads — still benign (no lock taken, nothing mutated on
  the peeked slot; `sched()` re-derives the CPU under its own entry mask).
  In the model the skipped call is a stutter (no state change).
- The syscall handler (`sys_yield_handler`, `kernel/syscall.c`) discards the
  bool and returns 0 always — the POSIX `sched_yield(2)` shape; a hint has no
  observable success/failure.
- `need_resched` is deliberately untouched on the fast path (pure read-only).
  A pending cross-CPU placement kick is always served: the flag's consumer
  (`preempt_check_irq` → `sched()`) acquires `cs->lock`, which pairs with the
  placer's release, so the insert is visible to whoever consumes the flag —
  the sound argument is consumer-under-the-lock, NOT "flag implies the peek
  sees the head" (a later plain store may be observed before an earlier
  store-release on ARM64, so flag-visible ∧ head-not-yet-visible is
  architecturally permitted; behavior is unaffected either way).

Consumers: the Go runtime `osyield` (`runtime·osyield` in
`sys_thylacine_arm64.s` — the linux `SYS_sched_yield` shape), musl
`sched_yield`/`thrd_yield` via the pouch seam (`__NR_sched_yield` 0xFFFF → 87;
proven boot-fatally by `pouch-hello`'s `sched_yield ok` probe), `t_yield` in
libt + libthyla-rs.

Regressions: `scheduler.yield_fast_path_no_work` (empty queue → no dispatch;
a queued band-IDLE thread is not competition) +
`scheduler.yield_dispatches_queued_work` (a queued NORMAL thread runs across
the yield and the caller resumes — the dispatch_smoke rotation driven through
`sched_yield_hint`).

---

## Build + verify

```bash
# Build + tests (production: 13/13 PASS expected)
tools/build.sh kernel
tools/test.sh

# UBSan
tools/build.sh kernel --sanitize=undefined
tools/test.sh --sanitize=undefined

# SMP soundness gate (single boots lie -- multi-boot or it didn't happen).
# Builds default + UBSan kernels, multi-boots smp4/smp8 x default/UBSan at N>=10.
make smp-gate                          # full matrix, N=10
SMP_GATE_CONFIGS="default-smp4 ubsan-smp4" tools/ci-smp-gate.sh   # amplifier subset

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

---

## HMP foundation (#864, ARCH §8.4.3 / §8.4.4)

The scheduler is **HMP-ready by design; v1.0 ships homogeneous-treatment**. Placement **policy** is separated from the enqueue **mechanism**, per-CPU capacity is parsed from the DTB, per-task utilization is tracked, and `balance()`'s shape can host a future capacity-aware push — but the *empirical* EAS tuning is deferred to real heterogeneous hardware (the **verification boundary**). On the uniform v1.0 targets (QEMU virt, RPi) this whole layer is **inert**: `select_target_cpu` returns the prev CPU, so `ready()` is byte-identical to the pre-#864 "enqueue on the CPU that woke you" behavior.

### Placement: `select_target_cpu` + `ready_on`

- **`unsigned select_target_cpu(struct Thread *t, unsigned prev_cpu)`** — the policy. Returns `prev_cpu` for a CPU-pinned thread (idles/kthread never migrate) or on a uniform topology (`!sched_topology_hetero()`). On a declared-heterogeneous topology it biases a high-`util` ("misfit") task toward a higher-capacity CPU. The core decision is the pure, testable `sched_place_by_capacity()`.
- **`void ready_on(unsigned target_cpu, struct Thread *t)`** — the mechanism. Inserts RUNNABLE `t` into `target_cpu`'s run tree under that CPU's lock; wakes the right CPU after releasing it (local → `sched_notify_idle_peer`; cross-CPU → `sched_notify_cpu`). Holds exactly **one** `CpuSched` lock and never nests, so it cannot cycle with `try_steal` (try-locks peers while holding its own) or `sched_remove_if_runnable` (one lock at a time). An out-of-range / uninitialized target falls back to the caller's CPU.
- **`void ready(struct Thread *t)`** is now `ready_on(select_target_cpu(t, smp_cpu_idx_self()), t)`.

Safety under an **arbitrary** target is the composition result proved by `specs/sched_alpha.tla` (its `Place` action picks the target CPU non-deterministically), so any `select_target_cpu` / `balance()` policy composes with correctness by construction.

A cross-CPU `ready_on` sets the target's `need_resched` (`#866` F1) so a *busy* target reschedules and considers the placed thread at its next preempt point rather than after a full slice; `sched_notify_cpu` then sends a (gated) `IPI_RESCHED` for promptness. `g_need_resched` is a RELAXED cross-CPU atomic for this (the producer is the placing CPU, the consumer is the target's `preempt_check_irq`).

**Known caveat (`#866` F5, dormant at v1.0):** the cross-CPU `ready_on` wake path holds `r->lock` across the target's full `cpu_switch_context` (it takes the target's CpuSched lock as a full blocking acquire while `wake_rendez_waiter` still holds `r->lock`). This is **bounded** (the peer always releases its lock when its switch completes; `sched()` never takes `r->lock`, so there is **no deadlock**) — it inflates the worst-case `r->lock` hold by one context switch. Inert on the uniform v1.0 topology (cross-CPU placement never fires). When misfit-push lands, prefer to drop `r->lock` before the cross-CPU enqueue+notify (the same off-lock discipline `wake_rendez_waiter` already uses for its `on_cpu` spin).

### Capacity (DTB) + util

- **`struct CpuSched.capacity`** — normalized `[0, SCHED_CAPACITY_SCALE]` (1024 = the most-capable core). Parsed once on the boot CPU by `sched_capacity_init()` from each cpu node's `capacity-dmips-mhz` (`lib/dtb.c::dtb_cpu_capacity`); composes with **I-15**. Absent on QEMU virt → every CPU is `SCHED_CAPACITY_SCALE`, `sched_topology_hetero()` is false.
- **`struct Thread.util`** — a PELT-style estimate (occupies `on_cpu`/`cpu_pinned` tail padding; `sizeof(struct Thread)` unchanged at 1136). Accrued while the thread RUNS (`sched_tick` → `sched_util_accrue`, saturating below the scale) and decayed when it blocks (`sched()` SLEEPING path → `sched_util_decay`). The v1.0 estimator is a simple EWMA (`SCHED_UTIL_SHIFT`); the tuned PELT geometric series is deferred.

### `balance()` — pull now, push-capable shape

`balance_pull()` (v1.0) wraps `try_steal` (pull-only work-stealing). The abstraction is deliberately shaped to host a future capacity-aware **push** (misfit migration) — the one HMP mechanism a pull-only stealer cannot express. The push **enqueue primitive already exists** (`ready_on` + `sched_notify_cpu`), so push is additive (a tick-time misfit scan → `ready_on(high_cap_cpu)` + IPI), not a rewrite. Push is deferred to real heterogeneous HW.

### The verification boundary

- **Verifiable now → built + tested**: the placement *logic* is unit-tested against a synthetic asymmetric DTB — `scheduler.capacity_normalize_synthetic_dtb` (raw `capacity-dmips-mhz` → normalized caps + hetero verdict) and `scheduler.place_by_capacity_synthetic_dtb` ("heavy task → high-capacity CPU; light task stays; heavy task already on the biggest core stays") — plus `scheduler.select_target_cpu_homogeneous_is_prev` (the v1.0 behavior-preservation guarantee on the real uniform topology) and `scheduler.ready_on_cross_cpu_enqueue` (the cross-CPU enqueue mechanism). Safety composition is `specs/sched_alpha.tla` (arbitrary placement).
- **NOT verifiable until real heterogeneous HW → deferred**: the empirical EAS tuning (PELT decay constants, energy model, schedutil/DVFS, misfit thresholds). QEMU virt declares a homogeneous DTB; HVF runs guest vCPUs on real P/E cores but the host floats them (no stable declared asymmetry). HVF closes the speed gap, not the heterogeneity gap. The EAS layer is additive + pre-modeled, landing when it becomes verifiable.

### Wake-preemption + the realized INTERACTIVE band (RW-11 SA-1b)

Closes the empirically-pinned 6 ms "slice cliff" (HT11.SA-1 / R2-F1): a
newly-runnable higher-priority-band thread now preempts on the *wake* path,
instead of waiting up to a full slice for the next tick-driven preempt. The
mechanism is additive — it weakens no invariant and composes with the `on_cpu`
protocol / I-8 / I-21.

- **`bool sched_wake_preempts(u32 woken_band, u32 cur_band, bool cur_is_idle)`** —
  PURE policy (the verification boundary, like `sched_place_by_capacity`). True
  iff the wakee outranks the current thread: a CPU running its idle yields to any
  real wake; a strictly-higher-priority (lower-number) band preempts; same band
  is EEVDF-fair (no wake-preempt). Unit-tested by `scheduler.wake_preempts_policy`.
- **`ready_on`'s same-CPU branch** consults it under `cs->lock` (where
  `current_thread()` + `cs->idle` are stable) and sets THIS CPU's `need_resched`
  when it returns true — the same-CPU analog of the `#866` F1 cross-CPU
  `need_resched` (the half that was missing). Verified by
  `scheduler.wake_preempt_same_cpu` (an INTERACTIVE wake sets the flag; a NORMAL
  wake does not).
- **The syscall-return preempt point: removed (#104), root-caused + re-added
  safely (#107).** As shipped in RW-11 SA-1b it `bl preempt_check_irq`'d at the SVC
  tail to consume a wake-set `need_resched` as the waker returned to EL0. Under
  QEMU TCG `-smp 4` through stratumd's mount IRQ-wait load it reliably deadlocked
  SMP (`sched()` spins forever acquiring a leaked `cs->lock`); #104 removed the
  preempt as the mitigation. **#107 root-caused it: NOT a mid-handler-specific
  steal, but a per-CPU `cs` TOCTOU in `sched()` itself.** `sched()` read
  `cs = this_cpu_sched()` (= `&g_cpu_sched[smp_cpu_idx_self()]`, MPIDR-derived)
  *before* masking IRQs; for a `sched()` entered with IRQs enabled (e.g. a kthread
  yield), a timer-IRQ preempt in the `cs`-read..lock-acquire window switched the
  thread out, a peer stole it, and it resumed on ANOTHER CPU still inside the same
  `sched()` call with `cs` pointing at the ORIGIN CPU — so it acquired the origin
  CPU's lock while running elsewhere, breaking the `pending_release_lock` handoff
  and leaking that lock. The syscall-return preempt merely cranked migration churn
  (every syscall return) high enough to hit the window; the bug was latent under
  the timer-tick-only preempt. Caught red-handed by an in-kernel "sched() holds a
  foreign CPU's lock" assertion (`STALE-CS sched(): cs_idx=0 running_cpu=1`).
  **Fix (root cause):** mask IRQs at the TOP of `sched()`, before `this_cpu_sched()`
  — pinning the CPU so `cs` cannot go stale before `cpu_switch_context` (after
  which the resume path already re-reads `cs_now`). **Re-add (safe):** the
  syscall/fault-return preempt now fires at the VECTOR level (`vectors.S`
  `.Lel0_sync_return`, reached after `exception_sync_lower_el` returns and its
  halls frame closes) — a *clean* saved frame structurally identical to the proven
  0x480 IRQ slot, so #60's wake-preempt latency win returns with no mid-handler
  hazard. The wake-preempt DECISION above is unchanged. Verified: 14/14 clean boots
  with the prior buggy placement + the fix (was ~30-50% deadlock pre-fix) + the SMP
  gate. See `docs/reference/08-exception.md` (the EL0 sync-return tail).
- **`void sched_mark_interactive(struct Thread *t)`** realizes the INTERACTIVE
  band (ARCH §8.3): a USER thread blocking in `kobj_irq_wait` (a device-IRQ
  driver) or `devcons_read` (the console session reader) is promoted
  NORMAL → INTERACTIVE (sticky, one-way) so its wake preempts NORMAL work. Gated
  to user threads (a kproc — notably the in-kernel test runner that drives both
  paths synchronously — stays NORMAL); verified by
  `scheduler.wake_preempt_same_cpu` part (c). **Each caller adds its own TRUST
  gate** so the set stays narrow (wake-preemption audit F1): the IRQ leg is
  implicitly `CAP_HW_CREATE`-gated (you need an IRQ kobj to reach
  `kobj_irq_wait`); the console leg (`kernel/cons.c`) promotes only the trusted
  console session — `proc_is_console_owner` (the shell) **or**
  `proc_is_console_attached` (login/corvus) — never an arbitrary foreground
  program that inherited `/dev/cons` as stdin (which would otherwise self-promote
  above NORMAL and starve it, since `/dev/cons` has no per-open cap gate + PTY is
  unbuilt).

**Known caveat — no cross-band aging (ARCH §8.3).** Bands are strict fixed
priority; a CPU-bound INTERACTIVE thread starves NORMAL on its CPU. Bounded by
the deliberately-narrow, mostly-blocked realized set (CAP-gated drivers + the
trusted console session: the owner shell + console-attached login/corvus) + the
per-Proc CPU quota (#65). The dynamic boost-on-wake / demote-on-
quantum classifier (Plan 9) + the controlling-tty rule are the deferred EEVDF
lift. **Adjacency note:** the `virtio-input` driver busy-polls (`yield` hint)
after its `t_irq_wait`, so it now runs that poll in INTERACTIVE — a faster,
CPU-monopolizing poll that marginally narrows the host QMP-injection window of
the already-tracked, intermittent #34 timing flake (verified intermittent, not a
new deterministic failure mode — virtio-input passed 3/3 on re-run post-change).

## Multi-boot CI gate + timing soft-warn (#865)

**Single boots lie.** The #788/#806/#860 SMP context-corruption races are layout-/timing-sensitive and pass a single boot most of the time — a one-shot `tools/test.sh` is the verification gap that masked #860 for weeks. The soundness gate is therefore **multi-boot**:

- **`tools/ci-smp-gate.sh`** (`make smp-gate`) builds the needed kernels once and runs the matrix — `default-smp4` / `default-smp8` / `ubsan-smp4` (the #860 amplifier) / `ubsan-smp8` — at **N≥10** boots each (env `SMP_GATE_N`, `SMP_GATE_CONFIGS`). It composes **`tools/smp-multiboot.sh`**, which re-runs `tools/test.sh` against one built kernel and classifies each failure: **CORRUPTION** (a ctx/stack-corruption signature — `invalid prev state`, `stack canary mismatch`, `kernel stack overflow`, `already on_cpu`, `#860`, …) FAILS the gate; benign host-**TIMING** fragility is reported, not failed; **OTHER** is surfaced for investigation. `test.sh` itself is the *primitive* the gate multi-boots — it is deliberately NOT a soundness gate, and the gate is a separate CI entry point to avoid recursion. `smp-multiboot.sh` clears its own label's prior `build/multiboot-fails/*.log` at the start of each run, so the captures dir only ever reflects the latest run — stale logs (including any written by a since-fixed classifier) cannot masquerade as current findings.

- **`TEST_SOFT_WARN(cond, msg)`** (`kernel/test/test.h`) is a host-fragility budget that logs `[SOFT-WARN]` + bumps a counter surfaced in the boot summary (`tests: N/M PASS (K soft-warn)`) **without failing the suite**. `test_irq_latency_bench`'s QEMU-TCG p99 budget (`IRQ_BENCH_CI_BUDGET_NS`, 50 ms) now soft-warns instead of hard-extincting: a `TEST_ASSERT` there turned host throttling into a kernel "crash" AND — because `boot_main` extincts on any suite failure — masked any real fault later in the SAME boot (the #860 class lived in the post-test production bringup). A true pathological regression is still caught (an infinite hang trips `BOOT_TIMEOUT`; broken counter math trips the hard `valid >= N-2` assert). **Use `TEST_SOFT_WARN` only for host-timing budgets** — never to soften a correctness check. The cons/torpor `sched_runnable_count()==0` quiescence asserts are a *correctness* class (tasks #857/#858/#859), deliberately left hard so a leaked-runnable helper still fails.

---

## Preemption discipline: the per-thread spinlock preempt count (#359/#360)

Plain `spin_lock` disables preemption for the holding THREAD (the Linux
"spin_lock disables preemption" rule, realized per-thread). Landed as the
general fix for #359 — the parallel-`go build` whole-guest wedge.

### The bug class (#359)

Syscalls (and EL0 faults) run IRQ-masked end-to-end, so a syscall spinning on
a contended plain spinlock is non-preemptible. Before #360, a holder running
IRQ-ENABLED — a kproc kthread (loom sqpoll, the dev9p_poll pump, console_mgr),
or a spawn thunk on a fresh thread (`thread_trampoline` unmasks) — could be
preempted MID-HOLD: it goes RUNNABLE off-CPU still holding the lock, sibling
IRQ-masked spinners occupy every CPU waiting for it, and the holder never gets
a CPU again. Permanent whole-guest deadlock. #359's confirmed instance: the
REVENANT eager exec read holding the shared dev9p pool client's `c->lock`
preemptibly under a parallel `go build` (~1-in-1.5 boots); the same shape was
latent on `l->lock`, `g_dev9p_poll_lock`, the poll hook-list locks — any lock
shared between a preemptible context and syscall paths.

### Mechanism

- **`Thread.preempt_count`** (`thread.h`): the number of plain spinlocks the
  thread currently holds. `spin_lock`/`spin_trylock`-success increment BEFORE
  the acquire; `spin_unlock` decrements AFTER the release store
  (`spin_preempt_inc`/`spin_preempt_dec`, out-of-line in `sched.c` because
  `spinlock.h` cannot see `struct Thread`).
- **The gate** — `preempt_check_irq` returns WITHOUT consuming `need_resched`
  while the interrupted thread's count is nonzero. The flag stays pending
  (it may be the #866-F1 cross-CPU placement kick, set exactly once), so the
  deferred preempt fires at the first IRQ-return after the hold drops
  (≤ 1 tick — the granularity preemption already had).
- **The assert** — `sched()` extincts if entered with `count != 0`
  ("plain spinlock held across sched()"): sleeping or yielding while holding
  a plain spinlock is forbidden (the lock-across-sleep deadlock class). A
  per-CPU breadcrumb (`g_spin_outer_acquire`) names the outermost acquire
  site in the extinction. `spin_preempt_dec` extincts on an unlock at
  count==0 (an unbalanced release — it would otherwise silently poison the
  gate).
- **The raw pair** — `spin_lock_raw`/`spin_unlock_raw` (uncounted) exist
  EXCLUSIVELY for sched()'s `cs->lock` pending-release handoff: the one lock
  acquired by one thread (prev, inside sched) and released by another (the
  resuming thread / a fresh thread's trampoline via
  `sched_finish_task_switch`), which a per-thread count cannot balance. The
  hold needs no count: sched runs fully IRQ-masked from its entry mask
  through `cpu_switch_context` to the release. Three release sites pair with
  the one raw acquire: `sched_finish_task_switch`, sched's resume block, and
  sched's nothing-runnable early return. Any other use of the raw variants
  is a bug (it opts a lock out of the discipline).

### Why per-THREAD and not per-CPU (the first-cut bug)

The first cut used a per-CPU count. It has an unfixable-in-place tear: the
increment is `ldr/add/str` on a slot whose ADDRESS is computed first; an IRQ
landing mid-RMW reads the pre-increment value (0), the gate passes, the
thread is preempted and MIGRATED, and the `str` then lands in the OLD CPU's
slot — poisoning that CPU permanently non-preemptible (an EL0 thread pinned
there livelocks the box) while the thread's later unlock underflows the NEW
CPU's slot. Reproduced under the parallel go build (two simultaneous
extinctions = the two halves of one migration event). Per-thread is
structurally immune: the count travels with the thread, so the gate and the
RMW always target the same thread — an IRQ reading a mid-increment pre-value
may preempt, but the thread holds nothing at that point and the half-done
RMW completes correctly wherever it resumes.

### The one lock-across-sleep site the assert found

`p9_client_handshake` held the fresh client's `c->lock` across the serial
NOTAG Tversion exchange's BLOCKING recv (by design — "unshared client").
Sound pre-#360 only because nothing could contend; the assert rightly
rejected it, and `client_run`'s NOTAG branch now drops `c->lock` across
`p9_transport_exchange`, aligning the handshake with the elected reader's
drop-before-recv discipline (`9p_client.c`).

### Tests

- `spinlock.preempt_count_balance` — inc/dec bookkeeping across lock,
  trylock success/failure, irqsave-with-lock, and the raw variants (which
  must NOT count).
- `scheduler.preempt_gate_defers_while_locked` — the gate regression: an
  armed `need_resched` survives 3 tick-returns while a plain lock is held
  (pre-#360 it was consumed at the first tick-return, deterministically),
  and is consumed within a bounded number of ticks after release.
- The end-to-end regression witness is the parallel-`go build` roll test
  (the #359 reproducer): N boots × 2 parallel cold builds, 0 wedges.

### Trip-hazards

- A syscall RETURNING to EL0 with a plain lock held (a leak) now pins its
  CPU non-preemptible forever — the assert cannot see it (no sched call).
  The underflow extinction catches the double-release flavor; a pure leak
  surfaces as a livelock. Pre-existing bug class, sharper consequence.
- `spin_lock_irqsave(l)`/`spin_unlock_irqrestore(l, s)` with a non-NULL lock
  COUNT (they wrap spin_lock/spin_unlock); the mask-only NULL forms do not.
- Pre-thread boot code (TPIDR_EL1 not yet parked) skips counting entirely —
  single-CPU, IRQs masked, no gate needed. A lock held ACROSS the TPIDR
  park would underflow at its release; don't do that.

## Per-thread on-CPU time accounting (prowl-1)

`Thread.run_ns` + `Thread.switched_in_at` (`thread.h`) give the scheduler a
cumulative **on-CPU time** telemetry counter per thread. They are stamped at the
`sched()` context-switch boundary, immediately before `cpu_switch_context`:

```c
u64 sched_now = timer_now_ns();
if (prev->switched_in_at)
    __atomic_store_n(&prev->run_ns,
                     prev->run_ns + (sched_now - prev->switched_in_at),
                     __ATOMIC_RELAXED);   // fold prev's slice out
next->switched_in_at = sched_now;          // stamp next's switch-in
```

**READ-ONLY telemetry.** No scheduling decision reads `run_ns` — EEVDF placement,
the `vd_t` math, I-8 (liveness), I-17 (latency), the tickless machinery are all
byte-unchanged. This is the substrate a process monitor (`prowl`, PROWL-DESIGN.md)
reads through `/proc/<pid>/status` (`cpu_ns`) and `/ctl/procs` (`CPU_NS`); the
reader derives **%CPU** by diffing the cumulative counter across two polls (the
htop method), so the kernel keeps no instantaneous-rate state.

**Cost.** One `timer_now_ns()` (a `CNTVCT` read the timer path already performs
elsewhere) + a guarded `__atomic` store + a plain store, all under `cs->lock`
with IRQs masked. Negligible, but it *is* the hot path — hence the SMP gate at
prowl-1 and the focused audit at prowl-5.

**Correctness properties:**

- **Single writer.** `run_ns` is written only by that thread's own switch-out,
  which runs on exactly one CPU at a time (a thread is on `<=1` CPU, I-21), so
  there is no writer-writer race. The `__atomic_store_n`/`__atomic_load_n`
  (RELAXED) pair gives a lockless cross-Proc reader a coherent snapshot — the
  `page_count`/`thread_count` pattern. `switched_in_at` is owner-local (set at
  switch-in, read at switch-out, both on the thread's own CPU — a thread does not
  migrate *while running*), so it needs no atomics.
- **The `switched_in_at != 0` guard.** The boot thread and the per-CPU idles
  become current *without* a `sched()` switch-in stamp (they are installed as
  current at init), so `switched_in_at` is 0 for their first run. Without the
  guard their first switch-out would fold in `(now - 0)` — a bogus ~uptime delta.
  The guard drops that one boot-era fragment; every subsequent slice is exact.
- **`proc_cpu_ns` (per-Proc sum).** The per-Proc CPU time is `Σ run_ns` over
  `p->threads`, computed at read time (`proc_cpu_ns`, `proc.c`). **Precondition:
  the caller holds `g_proc_table_lock`** — the threads list is mutated only under
  that lock (`thread_link_into_proc`/`thread_unlink_from_proc`), and the
  formatters call it from inside `proc_for_each` (which holds it). Walk-safety is
  the #95 argument: a Proc reachable via `proc_for_each` (the kproc-rooted tree)
  has not been unlinked, so its threads are not being freed. The sum omits the
  currently-running thread's in-flight slice since its last switch-in (< 1 slice;
  negligible over a poll interval).

Validated by `proc.cpu_ns_accounting` (`kernel/test/test_devproc.c`: the
`proc_set_name` basename cases + forced-yield `run_ns` accrual + monotonicity)
and the SMP gate (default+UBSan × smp4/smp8, 0 corruption).

## Per-thread scheduler counters + per-CPU idle time (prowl-3a)

prowl-3a extends the accounting substrate with the scheduler-introspection
counters that `/proc/<pid>/sched` (prowl-3b) surfaces. Four more per-thread
fields (`thread.h`), stamped at the **same single switch chokepoint** as
`run_ns`, and one per-CPU field (`struct CpuSched`, `sched.c`):

| Field (`struct Thread`) | Stamped | Meaning |
|---|---|---|
| `nsched` | switch-**in** of `next` | times this thread got the CPU. A busy-yield storm reads an astronomical rate — the signal that would have named the HVF-idle regression (`DEBUGGING-PLAYBOOK.md §6.17`) on sight. |
| `nsleeps` | switch-**out** of `prev` when `prev->state == THREAD_SLEEPING` | voluntary sleeps (the "parks" the process list surfaces, OQ-5). A yield-requeue (`RUNNING→RUNNABLE`) and an `EXITING` switch-out are **not** counted. |
| `nmigrations` | switch-**in** of `next` | times dispatched on a different CPU than the previous dispatch (guarded by `nsched != 0` so the first-ever dispatch is not miscounted as a move off the `KP_ZERO` `last_cpu`). |
| `last_cpu` (u16) | switch-**in** of `next` | the CPU this thread most recently ran on (meaningful once `nsched > 0`; the Linux `/proc/<pid>/stat` "processor" field). |

```c
// at the switch chokepoint, in the same block as the run_ns fold:
if (prev->state == THREAD_SLEEPING)
    __atomic_store_n(&prev->nsleeps, prev->nsleeps + 1, __ATOMIC_RELAXED);
u16 this_cpu   = (u16)(unsigned)(cs - g_cpu_sched);   // == smp_cpu_idx_self()
u64 old_nsched = next->nsched;
__atomic_store_n(&next->nsched, old_nsched + 1, __ATOMIC_RELAXED);
if (old_nsched != 0 && next->last_cpu != this_cpu)
    __atomic_store_n(&next->nmigrations, next->nmigrations + 1, __ATOMIC_RELAXED);
__atomic_store_n(&next->last_cpu, this_cpu, __ATOMIC_RELAXED);
```

The per-CPU field `CpuSched.idle_ns` (charged in `sched_idle_park` alongside the
global `g_wc_idle_ns`, read via `sched_cpu_idle_ns(cpu)`) is the `/ctl/cpu`
per-core **meter denominator**: utilization = `1 - d(idle_ns)/d(wall)` diffed
across polls. `cs` is cpu-pinned-stable across the park (the idle thread never
migrates), so each CPU is the sole writer of its own slot.

**READ-ONLY telemetry**, same as `run_ns`: no scheduling decision reads any of
them; EEVDF / `vd_t` / I-8 / I-17 / the tickless machinery are byte-unchanged.

**Correctness properties** (the run_ns argument, extended):

- **Single writer.** `nsched`/`nmigrations`/`last_cpu` are written only when the
  thread is switched **in**, by the single CPU that picked it — and `next->on_cpu`
  is already `true` at the stamp (set earlier in the same frame), so no peer can
  pick or touch `next`. `nsleeps` is written only at `prev`'s switch-out on its
  one running CPU. RELAXED `__atomic` gives the lockless cross-Proc reader a
  coherent snapshot; a stale cross-CPU read of `last_cpu` (written by the previous
  dispatcher) is at worst a cosmetically-off migration count — the run_ns
  plain-read-then-atomic-store pattern, since this is telemetry, never a decision.
- **The first-dispatch guard.** `last_cpu` inits to `KP_ZERO` 0; the `nsched == 0`
  guard prevents a spurious "migrated off CPU 0" the first time a thread lands on
  a non-zero CPU. `last_cpu` is only read for display once `nsched > 0`.
- **Not rfork-propagated.** A fresh thread (`KP_ZERO`) starts every counter at 0;
  a child accrues its own — same as `run_ns`.

Validated by `scheduler.prowl_counters` (`kernel/test/test_devproc.c`: `nsched`
growth across forced yields + a valid `last_cpu` + counter monotonicity + the
`sched_cpu_idle_ns` bounds guard) and the SMP gate (default+UBSan × smp4/smp8,
0 corruption). The focused audit is prowl-5.
