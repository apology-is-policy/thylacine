---
id: sub-kernel-sched
type: sub
parent: moc-kernel-scheduling
title: "Dispatch — per-CPU run trees, the bands, and preemption"
code: ["kernel/sched.c", "kernel/include/thylacine/sched.h"]
audit: hard
guarded-by: [inv-i8, inv-i17, inv-i21]
validated-by: [spec-scheduler, spec-sched-alpha, gate-smp]
locks: [lock-runq]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Decide which Thread runs next on this CPU, and take the CPU back when its
turn is over. The Plan 9 idiom (`sched()` = yield, `ready()` = make
runnable) over per-CPU run trees sorted by a virtual deadline.

The name "EEVDF" describes the intended design, not the as-built one.
What is built is a monotonic yield counter: on yield a thread's `vd_t` is
stamped past every currently-queued thread, so a band rotates FIFO. The
weighted virtual-time math that would make [[inv-i17]] a *bound* rather
than a *target* is not written — see Seams. What IS built, and is
load-bearing, is the fixed-priority band structure and the preemption
machinery around it.

## Contract

- `sched()` — yield. **Reads `prev->state` and dispatches on it**: the
  caller sets the state *before* calling. `RUNNING` requeues prev at the
  back of its band; `SLEEPING` and `EXITING` leave prev out of the tree
  entirely (a peer will requeue a sleeper, and an exiting thread never
  returns). `RUNNABLE` is a contradiction — prev is current — and
  extincts.
- `ready(t)` — make a RUNNABLE `t` schedulable. The caller sets
  `t->state = THREAD_RUNNABLE` first; `ready` extincts otherwise.
  `ready` is policy + mechanism: `ready_on(select_target_cpu(t, self), t)`.
- `ready_on(cpu, t)` — the enqueue mechanism alone, onto an arbitrary
  CPU's tree. An out-of-range or uninitialized target silently falls back
  to the caller's CPU.
- `sched_yield_hint()` — `SYS_YIELD`'s body. Yields only if a non-idle
  thread is queued locally; returns whether it dispatched.
- `sched_tick()` — called from every timer IRQ. Ages the slice and, at
  expiry, requests preemption.
- `preempt_check_irq()` — called from the IRQ-return and syscall-return
  tails. Consumes a pending request and calls `sched()`.
- **A plain spinlock may never be held across `sched()`.** This is
  checked, not documented: `sched()` extincts on `prev->preempt_count != 0`
  and names the outermost acquire site.

## Mechanism

**Bands.** Three, fixed priority, lower number wins:
`INTERACTIVE (0)` / `NORMAL (1)` / `IDLE (2)`. `pick_next` scans them in
order and takes the first non-empty band's head. There is no aging across
bands, so a CPU-bound INTERACTIVE thread starves NORMAL — bounded in
practice because the realized INTERACTIVE set is narrow and mostly
blocked (see Caveats).

`sched_mark_interactive` is the only promoter: sticky, one-way
`NORMAL -> INTERACTIVE`, USER threads only (`proc->pgtable_root != 0`, so
the in-kernel test runner stays NORMAL), and it never touches an IDLE
thread. Its two callers each apply their own trust gate — `kobj_irq_wait`
is implicitly `CAP_HW_CREATE`-gated by needing an IRQ kobj, and
`devcons_read` gates on the trusted console session.

**The sort.** Each band is a doubly-linked list kept ascending by `vd_t`,
so the head is the minimum. Ties insert *after* equal keys — FIFO within
a tie. On yield, `prev->vd_t = cs->vd_counter++` puts it behind everything
currently queued.

Two places re-key a thread's `vd_t`, and both exist for the same reason —
**each CPU's `vd_counter` is an independent clock**, so a key minted on
one CPU is meaningless on another:

- `ready_on` **clamps down**: `if (t->vd_t > cs->vd_counter) t->vd_t = cs->vd_counter`.
  A cross-CPU wake carrying a high key from a fast clock would otherwise
  tail the thread behind every fresh yielder, starving it by the
  inter-CPU counter gap (RW-2 2A-F1 — an [[inv-i17]] violation). The
  clamp places it at "now": never penalized, never unfairly credited,
  and the benign same-CPU "brief sleeper wakes near the front" case
  survives because a lower key is left alone.
- `try_steal` **rebases unconditionally** (`stolen->vd_t = cs->vd_counter++`),
  putting a migrated thread at the back of its new CPU's rotation.

**The yield/block split.** `sched()` picks *before* it requeues prev.
That ordering is deliberate — it is what lets a blocking prev leave the
tree — but it means a yielding thread is momentarily absent from the tree
while `next` is chosen, and the thread that gets picked can therefore be
this CPU's own idle. #363 is the consequence; see Prosecution.

**Preemption** is a three-step chain, all per-CPU:

1. `sched_tick` (timer IRQ, 1 kHz) decrements `slice_remaining`; at `<= 0`
   it sets this CPU's `need_resched` and *replenishes the slice* so the
   flag is not re-set on every subsequent tick.
2. `preempt_check_irq` (IRQ-return and the EL0 syscall-return tail) reads
   the flag, and if it may preempt, clears it and calls `sched()`.
3. `sched()` clears the flag again at entry, absorbing a stale set from a
   voluntary caller's path.

The wake path is the fourth producer. `ready_on` decides, under the
target's lock where `current_thread()` and `cs->idle` are stable, whether
the just-enqueued thread outranks the running one
(`sched_wake_preempts`: an idle yields to anything; a strictly higher
band preempts; same band stays EEVDF-fair) and sets `need_resched`
locally. The cross-CPU branch sets the *target's* flag unconditionally
and additionally sends a gated IPI. The split matters: the flag is the
**correctness** half (a busy target reschedules at its next tick even
with wake-IPIs suppressed), the IPI only the **promptness** half.

**The preempt count** (#360) makes a plain `spin_lock` hold
non-preemptible. `Thread.preempt_count` increments before the acquire and
decrements after the release store. `preempt_check_irq` returns
*without consuming* `need_resched` when it is non-zero — the flag may be
the once-set cross-CPU placement kick, so consuming it would lose the
placement; the deferred preempt fires at the first IRQ-return after the
hold drops, within a tick.

The count is per-**thread**, not per-CPU, and the reason is a real bug
the first cut hit: an IRQ landing mid-RMW read the pre-increment `0`,
passed the gate, the thread migrated, and the store then poisoned the
*old* CPU's slot permanently non-preemptible. A per-thread count travels
with the migration, so the gate and the RMW always name the same object.

## Data structures

`struct CpuSched g_cpu_sched[DTB_MAX_CPUS]` — one slot per CPU, indexed
by `smp_cpu_idx_self()` (MPIDR.Aff0).

| Field | Role |
|---|---|
| `lock` | this CPU's run-tree lock — [[lock-runq]] |
| `run_tree[3]` | per-band head pointer, min-`vd_t` first |
| `vd_counter` | this CPU's independent virtual clock; starts at 1 |
| `initialized` | one-shot; a second `sched_init` for the same CPU extincts |
| `idle` | this CPU's idle Thread (cpu0's is overridden post-`sched_init`) |
| `pending_release_lock` | the cross-thread lock handoff — [[sub-kernel-sched-smp]] |
| `prev_to_clear_on_cpu` | the `on_cpu` clear handoff — same |
| `idle_in_wfi` | "this CPU's current is its idle"; read by peers |
| `capacity` | normalized DTB capacity, `[0, 1024]`; write-once at boot |
| `idle_ns`, `nctxt` | read-only telemetry (prowl, VIVARIUM) |

Two file-scope arrays sit beside it: `g_need_resched[]` (per-CPU u8,
RELAXED atomics because `ready_on` made it a genuine cross-CPU producer)
and `g_spin_outer_acquire[]` (a per-CPU breadcrumb recording the return
address of a thread's *outermost* spinlock acquire, read only by the two
#360 extinction reports).

The Thread fields this layer owns: `vd_t`, `band`, `weight`,
`slice_remaining`, `runnable_next`/`runnable_prev`, `util`,
`preempt_count`, `cpu_pinned`. `in_run_tree` tests membership three ways
(`next || prev || head == t`) because a sole list element has both links
NULL.

## Concurrency

**The mask-before-read rule.** `sched()` masks IRQs *before* reading
`this_cpu_sched()`, and `ready_on` masks before reading
`smp_cpu_idx_self()`. Reading a per-CPU pointer with preemption enabled
is the #104 bug in one sentence: a timer IRQ in the read..lock-acquire
window switches the thread out, a peer steals it, and it resumes on
another CPU still inside the same call — with `cs` naming the *origin*
CPU. It then acquires and leaks a foreign run-queue lock, and the next
`sched()` on the origin CPU spins on it forever. A loud-fail assert
(`(cs - g_cpu_sched) == smp_cpu_idx_self()`) is the durable regression
for a race that is otherwise timing-only.

Masking only `I` (not `A`/`F`) is deliberate and sufficient: the only
migration vector in the window is a taken IRQ.

**The run-queue lock is acquired RAW.** `sched()` uses
`spin_lock_raw`/`spin_unlock_raw` — the uncounted forms — because this is
the one cross-thread lock handoff in the kernel: `prev` acquires it, and
the *resuming* thread (or a fresh thread's trampoline) releases it via
`pending_release_lock`. A per-thread count cannot balance that. It is
sound only because the hold is IRQ-masked end to end, so it is
non-preemptible by masking rather than by counting. Three raw releases
pair the one raw acquire; the first cut released one of them counted, and
the underflow probe caught it.

**Lock order.** [[lock-runq]] is the innermost of the wait chain
([[lock-wait]] → [[lock-timerwait]] → [[lock-rendez]] → [[lock-runq]]).
`ready_on` holds exactly one `CpuSched` lock and never nests, so it
cannot cycle with `try_steal` (which trylocks peers while holding its
own) or with `sched_remove_if_runnable` (which takes one at a time).
`g_cpu_sched` and its lock are file-private to `kernel/sched.c`, so no
external caller can arrive already holding one.

## Invariants enforced

- **[[inv-i8]]** — every runnable thread eventually runs. Enforced by the
  band scan plus the preemption chain plus the wake-preempt hook; the
  cross-CPU half is [[sub-kernel-sched-smp]]'s.
- **[[inv-i17]]** — the quantitative latency bound. This dossier is where
  it *would* be enforced. It is not: see Seams.
- **[[inv-i21]]** — a Thread runs on at most one CPU. This layer's share
  is that `pick_next` and `try_steal` both assert their victim is
  `RUNNABLE && !on_cpu` before taking it, and that `prev == next` is an
  extinction rather than a re-insert.

## Error paths

Everything here is an extinction, not an error return — this layer has no
caller to report to. The set is worth reading as a list of the things
that must never happen:

| Site | Condition |
|---|---|
| `sched_init` | called twice for one CPU; called before the idle is in TPIDR_EL1 |
| `this_cpu_sched` | CPU index out of range |
| `sched` | no current; corrupted current; **`preempt_count != 0`**; invalid prev state; `cs` mismatches the running CPU; `pick_next` returned current; blocking with no runnable thread *and* no in-tree idle |
| `ready_on` | NULL / corrupted / non-RUNNABLE / bad band; already in a tree; self CPU index out of range |
| `pick_next`, `try_steal` | victim not `RUNNABLE && !on_cpu` |
| `sched_install_bootcpu_idle` | not IDLE band; not pinned; not on cpu0; before `sched_init(0)` |
| `spin_preempt_dec` | release at count 0 — an unbalanced release |
| `sched_report_el0_leak` | a counted lock still held at an EL0 return (#361) |

The "deadlock" extinction in `sched()` deserves its own note: after the
redesign it is *structurally unreachable*, because every CPU's idle is
in-tree and `pick_next` therefore always finds at least it. Reaching it
means either the boot window before `sched_install_bootcpu_idle`, or a
secondary mis-init. It is kept as a loud failure rather than deleted.

## Performance

- Insert is O(N) in the band (linear scan); remove and pick are O(1) and
  O(bands). Deliberate: thread counts are tens, and a red-black tree's
  advantage is invisible there.
- Slice: `THREAD_DEFAULT_SLICE_TICKS = 6` at 1 kHz = 6 ms, matching
  Linux EEVDF's default granularity.
- `sched_yield_hint`'s fast path exists because the Go runtime issues
  ~36.8M `osyield` calls per `go build`. Without it, every yield on an
  otherwise-empty queue dispatches the pinned idle and bounces straight
  back — two context switches for nothing.
- `sched_runnable_count` deliberately **excludes** `BAND_IDLE`. Counting
  the in-tree idles reported a phantom backlog on an idle multi-CPU
  system and made the `runnable_count == 0` quiescence assertions race a
  benign idle thread. That was #857 — presented as an "smp8 cons.* flake",
  actually a measurement bug, and never a kernel fault at all.

## Prosecution

- **The mask-before-read rule holds at every per-CPU read.** Any new site
  that resolves `this_cpu_sched()` or `smp_cpu_idx_self()` for a
  *decision* (not a stale-tolerant hint) must mask first. `ready()`'s
  read is explicitly a hint — `ready_on` re-derives under its own mask —
  and `sched_yield_hint`'s peek is a documented hypothetical (syscalls
  run IRQ-masked end to end, so no preempt can land inside it from the
  SVC path; a future IRQ-enabled kthread caller would reintroduce it,
  benignly, since the peek takes no lock and mutates nothing).
- **The raw/counted spinlock split stays exactly one acquire and three
  releases.** A counted release of the raw acquire underflows the count;
  a raw release of a counted acquire poisons the gate. Both are
  extinctions, and both fired during bring-up.
- **`preempt_check_irq` must not consume `need_resched` when it defers.**
  The flag can be the once-set cross-CPU placement kick; consuming it
  loses the placed thread until the next tick.
- **The park-commit re-check (#363).** `sched_idle_park` loops
  `while (cpu_has_surplus_for_kick(cs)) sched();` before arming and
  parking. Deleting it re-opens a park of up to the tickless backstop
  over the CPU's *own* just-requeued thread — there is no IPI for a local
  self-requeue. The loop must stay deref-free and must not consume
  `need_resched`.
- **The `vd_t` clamp and rebase are not interchangeable.** `ready_on`
  clamps (preserving a lower key); `try_steal` rebases (always to the
  back). Swapping either direction is a starvation bug.
- **Band promotion stays narrow.** `sched_mark_interactive` has no
  demotion path, so every new caller permanently widens the set that can
  starve NORMAL. Each caller owes its own trust gate.
- Deliberately **not** prosecuted here: the telemetry counters
  (`run_ns`, `nsched`, `nmigrations`, `last_cpu`, `idle_ns`, `nctxt`).
  They are stamped at the single switch chokepoint, single-writer by the
  `on_cpu`/`I-21` discipline, and **no scheduling decision reads any of
  them** — a property that has been grep-verified twice (prowl-1,
  prowl-5) and must stay true.

## Seams

- [[seam-eevdf-math]] — the weighted virtual-time math, and with it a
  real [[inv-i17]].
- [[seam-runq-rbtree]] — the O(N) insert.
- [[seam-affinity-mask]] — `thread_may_run_on` is a plugged, always-true
  predicate awaiting a per-thread mask.
- [[seam-hmp-push]] — `balance()` is pull-only; misfit push is deferred
  to real heterogeneous hardware.

## Caveats

- **No aging across bands.** ARCH §8.3 says so explicitly, and it is now
  load-bearing rather than theoretical, because the INTERACTIVE band is
  realized. The general CPU-DoS bound is the per-Proc quota
  ([[inv-i32]]), not the scheduler.
- **`band` would default to INTERACTIVE.** `SCHED_BAND_INTERACTIVE == 0`,
  so a `KP_ZERO` Thread allocated outside `thread_create` gets the
  *highest* band by accident. Both constructors set NORMAL explicitly.
- **The HMP layer is inert on every v1.0 target.** `g_sched_hetero` is
  false on QEMU virt and RPi, so `select_target_cpu` returns `prev_cpu`
  and the whole capacity path short-circuits before reading a capacity.
  Its findings were therefore reachable only by reasoning, never by the
  runtime matrix.
- **`sched_dump_runnable` walks lock-free on purpose.** It is called from
  the test-fail path, where seeing the thread matters more than a
  consistent count. It is the instrument that cracked #857.

## Provenance

Dispatch landed at P2-Ba/Bb/Bc ([[chg-2026-05-05-p2b-sched-dispatch]]);
per-CPU trees, stealing and the `on_cpu` handoff at P2-Cd/Ce/Cf
([[chg-2026-05-05-p2c-smp-dispatch]]). The redesign that retired the
boot-CPU special case is [[arc-deep-smp-review]]
([[chg-2026-06-05-863-smp-soundness-core]],
[[chg-2026-06-05-864-hmp-foundation]], audited in [[adt-866-r1]]).
Wake-preemption and the realized INTERACTIVE band are
[[chg-2026-06-11-rw11-wake-preemption]]; the #104/#107 TOCTOU root fix is
[[chg-2026-06-13-107-sched-toctou]]; the preempt count is
[[chg-2026-07-04-360-preempt-count]]; `SYS_YIELD` and the #363 park-guard
are [[chg-2026-07-05-33-sys-yield]].

Absorbed `docs/reference/15-scheduler.md` at
[[chg-2026-08-01-sched-sweep]].
