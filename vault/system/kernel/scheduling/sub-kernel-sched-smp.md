---
id: sub-kernel-sched-smp
type: sub
parent: moc-kernel-scheduling
title: "The SMP protocol — on_cpu, work-stealing, the idle, and the tickless park"
code: ["kernel/sched.c", "kernel/smp.c", "arch/arm64/context.S"]
audit: hard
guarded-by: [inv-i21, inv-i18, inv-i8, inv-i9]
validated-by: [spec-sched-alpha, spec-sched-oncpu, spec-sched-tickless, spec-sched-rebalance, spec-sched-ctxsw, gate-smp]
locks: [lock-runq]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Let a Thread cross CPUs without two of them ever writing its saved
context, and let an idle CPU sleep without losing the work that arrives
while it is asleep. Both are the same difficulty stated twice: **a
context switch is not atomic**, and every window inside it is reachable
from another CPU.

This is the tree's most bug-prone surface. #788, #806, #807, #808, #860,
#801-F1, #104/#107, #866-F1, #363 all live here, and each one is a window
between two steps of a sequence that reads, in source, like a single act.

## Contract

- A Thread is claimed by exactly one CPU at a time. The claim is
  `t->on_cpu`; the claiming CPU is the only one that may load or save
  `t->ctx`.
- `on_cpu` is a **one-way safety signal**: a reader may act on
  `on_cpu == false`, and must never trust `on_cpu == true` for anything
  but "wait". The only inter-CPU edge that guards context reuse is the
  RELEASE-clear paired with an ACQUIRE-load.
- A CPU-pinned Thread (`cpu_pinned`) never migrates. Every per-CPU idle
  and `kthread` are pinned.
- An idle CPU announces itself (`idle_in_wfi`) **before** it parks, so a
  peer that places work either sees the announcement and kicks it, or the
  IPI it sends lands pending and the WFI exits on it. This is
  register-then-observe, and it is [[inv-i9]] on the placement path.
- Secondary CPUs are quiescent during the in-kernel test phase: no
  self-wake, no stealing, no timer. Production turns all three on at one
  transition point.

## Mechanism

**The claim/clear protocol.** A switch runs, in order:

1. `next->on_cpu = true` (RELAXED — consumed in program order on this
   CPU; a stolen `next` may already read true, and the re-store is
   harmless).
2. `cs->prev_to_clear_on_cpu = prev` and `cs->pending_release_lock = &cs->lock`
   — two handoffs parked in the *CPU's* slot, not the thread's.
3. `cpu_switch_context(&prev->ctx, &next->ctx)`.
4. Whoever resumes on this CPU — either a thread returning from its own
   `sched()`, or a fresh thread arriving at `thread_trampoline` and
   calling `sched_finish_task_switch` — reads *that CPU's* two handoff
   slots, RELEASE-clears `prev->on_cpu`, and releases the lock.

Step 4 is the subtle one. The resuming thread re-reads `this_cpu_sched()`
rather than using the `cs` in its own stack frame, because after
`cpu_switch_context` it may be on a **different CPU** than when it
entered — work-stealing moved it. The handoff is keyed to the CPU, so
whoever lands here finds the right `prev` and the right lock.

**Work-stealing.** `try_steal` (wrapped as `balance_pull`, a shape that
can later host a push path) runs with this CPU's lock **held** and
acquires peers with `spin_trylock` only — a peer mid-mutation is skipped,
never waited on. It scans peers from a rotating start (`g_try_steal_rotate`,
so concurrent stealers do not all hit CPU 0 first), and within a peer
scans bands in priority order, walking **past** pinned candidates rather
than giving up on a band whose head is pinned (#866-F2: the head-only
scan diverged from the model's "any non-pinned member" and would strand a
thread queued behind `kthread`).

The victim is claimed — `on_cpu = true` — **under the peer's lock, before
that lock is released** (#801-F1). Without that, the stolen thread sits
out-of-tree, RUNNABLE and unclaimed in an unlocked limbo, and a
concurrent `thread_free` can observe it free-able and reclaim its context
mid-steal. With it, a racing `thread_free` either unlinked the thread
first (so the steal does not find it) or observes the claim after its own
walk and waits the steal out on its `on_cpu` spin.

The stolen thread is then rebased into the stealer's clock
(`vd_t = cs->vd_counter++`).

`try_steal` distinguishes "all peers empty" from "some peer's lock was
held" via `contended_out`, because on the blocking path the two mean
opposite things: contended is transient (retry once after a bounded
relax), genuinely empty is a bug.

**The idle.** Every CPU has its own idle Thread, and it is an **ordinary
in-tree thread**: it lives in `run_tree[IDLE]`, it is dispatched by
ordinary `pick_next`, and it is skipped by `try_steal` because it is
pinned. That single change closes #860 by construction — the retired
design had cpu0 dispatch an *off-tree* `g_bootcpu_idle` through a global
pointer on a deadlock path, and that thread had a real kernel stack, so
leaking it into a tree made it stealable and a peer would then build
exception frames on a stack cpu0 still owned.

The consequence worth stating: while a non-idle thread is current, its
CPU's idle is *in* the tree (displaced when that thread was picked), so
`pick_next` can never come back empty. The old deadlock path is now
unreachable rather than merely unused.

cpu0's idle runs on `g_bootcpu_idle_stack` — a `struct secondary_stack`
identical to a secondary's, with a leading guard page mapped no-access —
because cpu0's `_boot_stack` belongs to `kthread`, which is suspended
mid-`wait_pid` once init blocks.

**`idle_in_wfi` accuracy (F7).** The flag means "this CPU's current
thread is its idle", and it is maintained in two places: the idle loop
sets it TRUE before its own `sched()` (covering the born-running first
iteration and the stays-idle no-switch case), and **`sched()` sets it to
`(next == cs->idle)` at every switch** (so a CPU that switched its idle
away to stolen work no longer advertises itself as idle — the bug both
review prosecutors found independently).

**Two notify paths, deliberately different.** `sched_notify_idle_peer`
picks *any* peer announcing idle and stops on the first send — one peer
waking to steal is enough, and waking several is a thundering herd where
only one gets the work. `sched_notify_cpu` targets a *specific* CPU, and
is the promptness half of a cross-CPU placement. Both are gated by
`g_sched_notify_enabled` so the test phase stays UP-like; the
`need_resched_set` that accompanies the targeted one is **not** gated,
because it is the correctness half.

**Push-complete placement (TI-4b).** In production, `select_target_cpu`
prefers an *idle* peer for a waking thread — `select_idle_target` rotates
across distinct idle CPUs — so a burst from one busy producer spreads
instead of piling onto the waker's tree behind a single best-effort kick.
When the waker is itself idle it keeps the thread (no needless
migration); when no peer is idle it keeps it local (the saturated regime,
where the busy-side kick rebalances instead).

**The idle park** (`sched_idle_park`, shared by cpu0's `bootcpu_idle_main`
and each secondary's `per_cpu_main` tail) is one IRQ-masked region:

    mask -> idle_in_wfi = true -> sched() -> [#363 re-check loop]
         -> sample starved -> arm one-shot -> WFI
         -> idle_in_wfi = false -> classify wake source
         -> re-arm periodic tick -> timerwait_tick() -> charge -> unmask

Three parts of that are load-bearing rather than incidental:

- The **flag before the arm and the WFI** is the register-then-observe
  that makes the arm-race safe ([[spec-sched-tickless]]).
- The **#363 re-check loop** (`while (cpu_has_surplus_for_kick(cs)) sched();`)
  stops the CPU parking over its *own* just-requeued thread. `sched()`
  picks before it requeues prev, so a slice-expiry preempt of a thread on
  an otherwise-empty queue dispatches this idle and the preempted thread
  lands in the tree right after the pick. The dispatched idle does not
  restart its loop — it resumes *inside* the park, past its own
  `sched()`, headed for the arm and the WFI — and no IPI exists for a
  local self-requeue.
- The **wake-to-running restore** does two jobs. `timer_arm_this_cpu()`
  re-arms the periodic tick so a CPU woken from tickless idle runs the
  placed thread with slice accounting live. But re-arming also
  *deasserts* the one-shot's pending IRQ, so `timerwait_tick()` must be
  called explicitly right here: had the one-shot fired on a passed
  deadline, deasserting it would stop the handler ever running the scan
  for it, and the sleeper would never wake — a busy-spin of re-arm, fire,
  deassert, repeat.

**The busy-side kick (TI-4c).** Tickless removed something nobody had
named: the never-stopped 1 kHz tick *was* the work-steal re-poll, so any
work-arrival the single best-effort kick missed got pulled within a
millisecond anyway. Without it, queued work stranded on a busy CPU until
the backstop — a 2.4x boot slowdown. The fix is the Linux NO_HZ_IDLE
shape: a **still-ticking busy** CPU with surplus queued work and a parked
peer kicks one peer to come steal.

**Secondary bring-up** (`kernel/smp.c`). PSCI `CPU_ON` at the computed PA
of the `secondary_entry` trampoline; the trampoline flips `g_cpu_online`,
applies PAC keys, programs the MMU, and long-branches to `per_cpu_main`
at the high VA. `per_cpu_main` sets VBAR, enables FP and EL0 counter
access, brings up per-PE debug and hardware identity, creates and parks
this CPU's idle in TPIDR_EL1, runs `sched_init`, brings up the per-CPU
GIC and attaches the IPI, then flips `g_cpu_alive` — the flag the boot
CPU actually waits on, because it is the one that proves PAC/MMU/VBAR all
worked.

Two gates keep secondaries out of the deterministic test phase: `#810`
defers each secondary's own timer arming until
`smp_enable_secondary_preemption`, and `sched_set_notify_enabled` keeps
wake-IPIs off. Before those flip, a secondary is parked in WFI and woken
only by an explicit IPI. (Without the timer gate, a secondary self-waking
on its own tick stole a test thread and surfaced as
`thread_free of RUNNING thread`.)

## Data structures

| Symbol | Role |
|---|---|
| `g_cpu_online[]` | trampoline reached — the earliest signal |
| `g_cpu_alive[]` | `per_cpu_main` finished — the signal `smp_init` waits on |
| `g_secondary_boot_stacks[]` | 7 × (4 KiB guard + 16 KiB usable), page-aligned |
| `g_bootcpu_idle_stack` | the same shape, for cpu0's idle (#867) |
| `g_exception_stacks[]` | RESERVED — unused at runtime under uniform EL1h |
| `g_pac_keys[8]` | derived once by the primary, applied by every CPU |
| `g_ipi_resched_count[]` | per-CPU IPI receive counter (observability) |
| `g_secondary_preempt_enabled` | the #810 production gate |
| `g_try_steal_rotate` | pull-side scan rotation |
| `g_idle_place_rotate` | push-side placement rotation |
| `g_wc_*` | work-conservation telemetry (park/starved/wake-source) |

## Concurrency

- **`spin_trylock` for every peer.** `try_steal` never blocks on a peer's
  lock; `sched_remove_if_runnable` *does* take each CPU's lock in turn,
  because `thread_free` needs unconditional cleanup — but one at a time,
  so it cannot cycle.
- **The RELEASE/ACQUIRE pair on `on_cpu`** is the only inter-CPU edge
  guarding context reuse. Set is RELAXED; clear is RELEASE; every waiter
  (`wake_rendez_waiter`'s spin, `thread_free`'s gate, `timerwait_tick`'s
  filter, the two run-tree asserts) ACQUIRE-loads.
- **The capacity publish** (#866-F3) uses `g_sched_hetero` as its
  release/acquire point: capacities are stored plain, then the flag is
  RELEASE-stored; `select_target_cpu` ACQUIRE-loads the flag *first* and
  only touches a capacity when it reads true. Dormant on uniform targets,
  correct the day a heterogeneous DTB activates it.
- **`per_cpu_main` asserts `cpu_idx == smp_cpu_idx_self()`** (RW-2 2A-F4).
  The PSCI context id names the slot the CPU initializes; MPIDR.Aff0
  names the slot every *runtime* access resolves. On a sparse or
  cluster-MPIDR board they diverge, and the CPU then silently aliases
  another CPU's live `CpuSched` — whose single-slot handoffs would be
  written by two CPUs. A bounds check cannot detect aliasing; the
  equality check makes the dense-Aff0 assumption fail loud on the first
  board that breaks it.

## Invariants enforced

- **[[inv-i21]]** — a Thread runs on at most one CPU, and its context is
  never written by two. The claim protocol, the pinned idle, and the
  `RUNNABLE && !on_cpu` asserts are its three enforcement points; the
  EL1h half is [[spec-sched-ctxsw]].
- **[[inv-i18]]** — IPIs from one CPU to another are processed in send
  order (modelled per-(src,dst) queues in [[spec-scheduler]]).
- **[[inv-i8]]** — the cross-CPU half of "eventually runs": stealing,
  push placement, the busy-side kick, and the backstop between them.
- **[[inv-i9]]** — the idle arm-race: no work-arrival is lost between
  announcing idle and parking.

## Error paths

`smp_init` degrades rather than extincts: a malformed DTB is treated as
UP; missing PSCI holds the secondaries with a message; a CPU count over
`DTB_MAX_CPUS` is capped; a PSCI failure or bring-up timeout logs which
stage failed (trampoline vs `per_cpu_main`) and continues with fewer
CPUs. The boot proceeds on however many came up.

Inside `per_cpu_main` the posture inverts — every failure is an
extinction (invalid index, the MPIDR aliasing check, a failed
`gic_init_secondary`/`gic_attach`/`gic_enable_irq`, a NULL idle), because
a half-initialized CPU that reaches the idle loop is worse than no CPU.

## Performance

- Stealing is O(peers × band-prefix) under trylock, on the idle path
  only.
- `TICKLESS_IDLE_BACKSTOP_NS` is 4 ms (was 100 ms). The retune is an
  **HVF dev-loop knob, not a scheduler fix** — the measured wake path is
  push-complete (99.85% of tickless parks are IPI-woken, not
  backstop-woken), but resuming a *deep*-parked vCPU via SGI costs
  ~0.85 ms under HVF against ~7 µs hot, an emulation artifact. On bare
  metal an SGI to a WFI'd core is nanoseconds, so deep-park already gives
  both fast boot and ~0% idle there. The 4 ms re-poll keeps dev-loop
  vCPUs warm (HVF boot ~7 s vs ~17–35 s at 100 ms) at ~5% HVF idle.
- The work-conservation counters split total vs tickless deliberately: a
  starved *periodic* park ends at the next ≤1 ms tick and is the correct
  pre-tickless baseline, so only the *tickless* starved figures are a
  regression signal.

## Prosecution

- **The claim happens under the victim's lock.** Moving `on_cpu = true`
  outside the `peer->lock` hold in `try_steal` re-opens #801-F1
  directly.
- **`on_cpu == true` is never trusted for safety.** Any new reader that
  branches on true must wait for false instead.
- **`idle_in_wfi` is maintained at BOTH sites.** The idle loop's explicit
  set and `sched()`'s `(next == cs->idle)` cover disjoint cases; dropping
  either re-opens a stale-idle advertisement.
- **The cross-CPU placement IPI must never become `idle_in_wfi`-gated.**
  A peer that places *onto* another CPU IPIs unconditionally; the flag is
  read only by `select_idle_target` and by the local-place notify. Gating
  the placement IPI on a bare-volatile flag a peer can read stale-FALSE
  reintroduces up-to-backstop placement latency — the exact class #363
  closed. The comment at the site forbids it in words for this reason.
- **The park's ordering is fixed**: announce, then `sched()`, then the
  re-check loop, then arm, then WFI. Any reordering breaks either the
  register-then-observe or the #363 guard.
- **The wake restore must keep `timerwait_tick()` after `timer_arm_this_cpu()`.**
  Dropping it turns an expired deadline into a busy-spin.
- **The dense-Aff0 assertion stays.** It is the only thing standing
  between a cluster-MPIDR board and silent per-CPU slot aliasing.
- **Secondary quiescence during tests is a property, not a convenience.**
  Both gates (`g_secondary_preempt_enabled`, `g_sched_notify_enabled`)
  must flip after `test_run_all`, never before.

## Seams

- [[seam-hmp-push]] — `balance()` is pull-only; the misfit push the HMP
  shape exists for is deferred to real heterogeneous hardware.
- [[seam-affinity-mask]] — `thread_may_run_on` is consulted at both
  CPU-binding decisions and is unconditionally true.
- [[seam-tickless-bare-metal]] — the deep-park latency finding is owed a
  bare-metal confirmation.
- [[seam-sparse-mpidr]] — the dense-Aff0 assumption fails loud but is
  unimplemented.

## Caveats

- `g_exception_stacks` is allocated and asserted but **unused at
  runtime**: under uniform EL1h every exception frame is built on the
  interrupted thread's own kernel stack. It is a pre-allocated landing
  pad for a future dedicated overflow/SError stack.
- A spurious IPI is harmless by design — the handler is a counter bump —
  which is what lets every `idle_in_wfi` read be a relaxed hint.
- The steal-invariant asserts in `pick_next` and `try_steal` are not
  paranoia: they exist because the alternative to failing loud is
  resuming a half-saved context, which presents as arbitrary corruption
  far from the cause.
- `THYLACINE_NO_TICKLESS` (a build flag) forces the old always-periodic
  idle. It exists so the redesign's numbers can be measured against the
  pre-tickless gold standard rather than against themselves.

## Provenance

Per-CPU dispatch, stealing and the handoff landed at P2-Cd/Ce/Cf
([[chg-2026-05-05-p2c-smp-dispatch]]); uniform EL1h at
[[chg-2026-05-15-el1h-uniform]]; the steal claim at
[[chg-2026-05-31-801-steal-claim]]. The redesign that retired the
off-tree boot idle is [[arc-deep-smp-review]]
([[chg-2026-06-05-863-smp-soundness-core]], audited [[adt-866-r1]]); the
per-CPU TOCTOU root fix is [[chg-2026-06-13-107-sched-toctou]]. Tickless
idle and the work-conservation response are [[arc-tickless-idle]]
([[chg-2026-06-21-ti-tickless-idle]],
[[chg-2026-06-22-ti4-work-conservation]]).

Absorbed `docs/reference/17-smp-bringup.md` and the SMP half of
`docs/reference/15-scheduler.md` at [[chg-2026-08-01-sched-sweep]].
