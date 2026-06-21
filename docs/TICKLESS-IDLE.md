# Tickless idle (NO_HZ_IDLE) -- design

Status: DESIGN (scripture-first; no code). Surfaced by the #299 HVF idle-spin
re-investigation (`memory/bug_299_hvf_idle_tickless.md`). User voted "tickless
idle" (2026-06-21) as the durable fix over the 250 Hz stopgap. This document is
the design pass; on signoff it lands as a scripture commit (this doc + an ARCH
section 8 subsection + the audit-trigger row + the CLAUDE.md row), THEN the impl
arc TI-1..TI-3, THEN the focused audit.

---

## 1. Problem (measured, ground-truthed)

Thylacine runs a **1 kHz periodic timer tick on every CPU and never stops it**,
even when a CPU is idle (`kernel/main.c:352` `timer_init(1000)`; the IRQ handler
`arch/arm64/timer.c:150` unconditionally re-arms `g_reload = freq/hz` every
fire). The tick exists to preempt a *running* thread (slice accounting) and to
expire deadlined sleepers (`timerwait_tick`). An **idle** CPU has no running
thread to preempt, yet it still takes 1000 timer IRQs/s.

Under HVF on Apple Silicon this is expensive: each tick costs a VTIMER exit + two
emulated-GICv2 `GICC_IAR`/`GICC_EOIR` MMIO vmexits + a WFI re-entry that, at a
1 ms period ~= HVF's ~1 ms wake latency, **never parks before the next tick
fires** (the storm regime).

Measured (all ground-truthed to a healthy `login:` end-state; console on a file
backend so the interactive chardev is out of the picture):

| Config | Tick | Idle CPU% |
|---|---|---|
| HVF | 1000 Hz | **332%** |
| HVF | 250 Hz | **5.2%** |
| TCG | 1000 Hz | 21.5% |

The 4x-tick-cut -> 64x-idle-drop confirms the storm/wake-latency amplifier: once
the tick period clears the host wake latency, the host genuinely parks. This
corrects `memory/bug_110...`'s "interactive chardev kick / nothing to do"
closure -- the spin is a general HVF tax of the never-stopped tick, sample-proven
(idle QEMU main thread parked in `mach_msg`; vCPU threads churning
`hvf_vcpu_exec`/`hv_vcpu_run`/`hvf_wfi` with GIC-MMIO data-aborts).

This is primarily a **dev-loop / power cost** (the HVF fast loop burns host CPU
and battery; a real ARM64 board with an in-silicon GIC pays far less per tick but
still wakes a deep-idle core 1000x/s pointlessly). It is not a guest-correctness
bug. But a never-stopped idle tick is a real defect of the design, and the fix is
what every production OS does.

---

## 2. Prior art (heritage + SOTA + fit)

- **Heritage (Plan 9):** Plan 9 runs a periodic `HZ` clock tick per processor,
  same as us. It does not do tickless idle -- so the heritage gives us the
  periodic-tick baseline, not the fix.
- **Modern SOTA (capability microkernels):** seL4 and Zircon are **tickless by
  design** -- there is no periodic tick at all; the timer is programmed to the
  *next deadline* (one-shot), and preemption is driven by per-event timers. This
  is the end state. Linux/FreeBSD, coming from a periodic-tick heritage like ours,
  converged on the intermediate **NO_HZ_IDLE / dynticks-idle**: keep the periodic
  tick while a CPU runs a thread, stop it when the CPU goes idle, arm a one-shot
  to the next deadline, resume periodic on wake. (The FreeBSD-on-M1 datapoint:
  `kern.hz` 1000->100 took idle 300%->5% -- the same lever we measured.)
- **Fit for Thylacine:** Thylacine's preemption model *relies* on the periodic
  tick for a running thread's slice accounting (`sched_tick` decrements
  `slice_remaining`; the I-17 latency budget + the RW-11 #60 6 ms-slice-cliff
  analysis are all framed around the 1 kHz tick). Removing the periodic tick
  globally (full tickless / NO_HZ_FULL) would rework that whole model and is
  unnecessary for the idle problem. **NO_HZ_IDLE is the exact-fit increment:** it
  keeps the 1 kHz periodic tick for any running thread (so I-17, the slice model,
  and the tick-coupled kernel tests are all byte-unchanged) and stops the tick
  ONLY on a genuinely-idle CPU (the actual cost). The seL4-style full
  deadline-driven scheduler is the v2.x end state, out of scope here.

Decision: **NO_HZ_IDLE**, not NO_HZ_FULL.

---

## 3. Mechanism

When a CPU selects its **idle thread** (run tree empty, nothing runnable), it
stops taking the periodic tick and instead arms a **one-shot** timer to the
nearest pending wake; on any wake it resumes the normal periodic tick.

### 3.1 The one-shot deadline

The wake an idle CPU must still honor is a deadlined sleeper
(`tsleep`/`torpor`): a thread on the global `g_timerwait` list whose
`sleep_deadline` (a `CNTVCT` counter value) will pass. The idle CPU arms:

```
target_cnt = min( earliest g_timerwait deadline (if any),
                  now + TICKLESS_IDLE_BACKSTOP )
arm one-shot at target_cnt   (CNTV_TVAL = target_cnt - now, clamped)
```

- `earliest g_timerwait deadline`: a new `timerwait_earliest_deadline()` helper,
  an O(n) min-scan over the global list under `g_timerwait.lock` (n is the count
  of deadlined sleepers -- small). Returns 0 if none.
- `TICKLESS_IDLE_BACKSTOP`: a long bounded cap (proposed **100 ms**; tunable) so
  an idle CPU wakes at most ~10x/s even with no pending deadline. This is the
  defense-in-depth answer to the documented dropped-IPI hazard (smp.c:145: a
  dropped `IPI_RESCHED` was historically masked by the always-armed timer).
  #868 made the cpu0 `IPI_RESCHED` path reliable, so the work-arrival wake is
  authoritative; the backstop exists only so that IF an IPI is ever dropped, an
  idle CPU self-heals within the backstop instead of hanging. 100 ms keeps idle
  cost negligible (~10 wakes/s/CPU vs 1000) while bounding a hypothetical
  dropped-IPI latency to a hiccup, not a hang.

A genuinely-idle CPU with no deadline therefore wakes ~10x/s instead of 1000x/s
-- below the HVF wake-latency threshold, so the host parks. Expected HVF idle:
~0% (better than the 250 Hz datapoint's 5.2%, because idle has effectively no
ticks rather than a 4 ms tick).

### 3.2 Work-arrival wake (already exists, tick-independent)

A CPU becomes runnable-again via `sched_notify_idle_peer()` (`kernel/sched.c:569`):
a peer that `ready()`s work finds an `idle_in_wfi==true` CPU and sends it
`IPI_RESCHED` (`gic_send_ipi`). This path is **independent of the periodic
tick** -- it already works today and is what wakes an idle CPU for new work. The
one-shot is only for *timed* wakes (deadlines); work-arrival rides the IPI.

### 3.3 Resume periodic on wake-to-running

When an idle CPU wakes (one-shot fires, or `IPI_RESCHED`, or any device IRQ) and
`pick_next` selects a *real* thread, the normal periodic tick resumes (the next
`sched_tick` re-arms `g_reload` for slice accounting). If `pick_next` selects the
idle thread again (the wake found nothing runnable -- e.g. a redundant one-shot,
or a deadline already serviced by another CPU), the idle loop simply re-arms the
one-shot and WFIs again.

### 3.4 Per-CPU, global deadline list

Each CPU independently goes tickless when *its* idle thread is selected. The
`g_timerwait` list is global, so multiple idle CPUs may each arm a one-shot to
the same earliest deadline (redundant coverage). At the deadline they all wake,
one wins `g_timerwait.lock` + `timerwait_tick` wakes the sleeper, the others find
nothing expired and re-arm. This is SAFE and simple; the redundant wakes are
bounded by ncpus and occur only at actual deadlines (rare vs the retired
1 kHz x ncpus). A v1.x refinement could designate a single deadline-owner CPU.

---

## 4. Invariant (I-9 generalization) + the arm-race

**I-9 (no wakeup lost between cond-check and sleep) generalizes to the idle
one-shot arm:** an idle CPU must not miss (a) a work-arrival or (b) a deadline
between the moment it computes "nothing runnable / nearest deadline = D" and the
moment it is parked in WFI.

The idle loop already holds the structure that makes this safe
(`bootcpu_idle_main`, sched.c:439; the secondary equivalent in
`per_cpu_main`): the whole `set idle_in_wfi=true -> sched() -> WFI` sequence runs
under `spin_lock_irqsave(NULL)` (IRQs masked), and a masked-pending IRQ makes the
subsequent WFI return immediately. The tickless arm slots in as:

```
spin_lock_irqsave(NULL)
sched_set_idle_in_wfi(true)        // register: peers will now IPI us on work
sched()                            // observe: nothing runnable -> idle selected
// --- tickless arm (new) ---
under g_timerwait.lock: D = timerwait_earliest_deadline()
arm one-shot at min(D, now + BACKSTOP)   // or BACKSTOP if D==0
// --------------------------
wfi
sched_set_idle_in_wfi(false)
spin_unlock_irqrestore(NULL, s)
```

Register-then-observe (the existing R7 F128 ordering) is preserved: `idle_in_wfi`
is set BEFORE the arm + WFI, so a peer that `ready()`s work after the arm sends an
`IPI_RESCHED` that the WFI sees pending and exits on -- the work-arrival wake is
never lost. A deadline added in the arm window is either (i) already <= the armed
target (the one-shot catches it), or (ii) a new *nearer* deadline added by some
CPU which, on its own idle-enter, arms to the new nearest -- and if that adder is
a running CPU it is still periodically ticking and `timerwait_tick` catches it
within a tick. No deadline is lost.

**Lock order:** the arm takes `g_timerwait.lock` (a near-outer lock; `wakeup()`
already takes it as its OUTER lock) only to read the earliest deadline, then
releases it before WFI -- no new cycle (`g_timerwait.lock` -> nothing).

**Timekeeping is unaffected.** `CLOCK_MONOTONIC`/`REALTIME` read `CNTVCT_EL0`
(the always-running counter), NOT the tick (`timer_now_ns`, timer.c:185). The
`g_ticks` counter (cpu0-only, a busy-wait + diagnostic timebase) freezes while
cpu0 is idle -- but `timer_busy_wait_ticks` runs in a *running* thread context
(so cpu0 is not in the idle loop and still ticks), and nothing reads `g_ticks` as
a wall clock. The slice accounting (`sched_tick` decrement) only matters for a
running thread, which always has the periodic tick. So no time- or slice-coupled
behavior changes.

---

## 5. What it does NOT touch (the safety envelope)

- **I-17 / slice model / RW-11 #60:** the periodic 1 kHz tick is byte-unchanged
  for any *running* thread. Preemption granularity is unchanged. The 250 Hz
  stopgap's I-17 tradeoff does NOT apply -- tickless keeps 1 ms slices.
- **The tick-coupled kernel tests:** the 100 Hz experiment broke 4 scheduler-test
  preconditions because it slowed the tick *globally*. Tickless keeps the tick at
  full rate whenever a thread runs (the tests run threads), so the test timing is
  unchanged. (Verified expectation, re-confirmed by the SMP gate at TI-3.)
- **The wake protocol:** `sched_notify_idle_peer`/`IPI_RESCHED`/`idle_in_wfi` are
  unchanged; tickless only changes WHEN the *timer* fires, not how wakes work.

---

## 6. Surface + sub-chunk plan

Audit-trigger surface: **scheduler + timer** (both in the trigger table). The
idle/wake lineage (#788/#806/#807/#808/#860/#809/#811/#926) is the most
bug-prone in the tree, so this gets a focused Opus-4.8-max audit.

- **TI-1 (primitive, no behavior change):** `timer_arm_oneshot_cnt(target_cnt)` /
  `timer_disable_this_cpu()` in timer.c (clamped to `[TIMER_MIN_RELOAD,
  TIMER_MAX_RELOAD]`; a target already <= now arms the minimum so it fires
  immediately) + `timerwait_earliest_deadline()` in sched.c. Unit tests. The
  periodic tick still drives idle -- no observable change yet.
- **TI-2 (integration, the audit-bearing chunk):** wire the tickless arm into
  `bootcpu_idle_main` + the `per_cpu_main` secondary idle loop; the I-9 arm-race
  (section 4); resume-periodic-on-wake. Add `TICKLESS_IDLE_BACKSTOP`.
- **TI-3 (verify + close):** re-measure HVF idle (target ~0%, the #299 method);
  SMP gate (default+UBSan x smp4/smp8 -- the idle/wake path is the SMP-sensitive
  one); the focused audit (I-9 arm-race, the dropped-IPI backstop, lock order,
  timekeeping, the redundant-coverage soundness); kernel tests (a deadlined
  sleeper still wakes on time under tickless; a cross-CPU `ready()` still wakes a
  tickless-idle CPU; an idle CPU's tick count stops advancing); docs (this doc as
  the as-built reference + a `docs/reference/11-timer.md` / scheduler-reference
  update + the audit-trigger row).

---

## 7. Rigor: spec-first (RESOLVED -- user-voted 2026-06-21)

The scheduler surface had **spec-first RE-ENABLED** (the deep-smp-review,
ARCH 8.4) because the original `scheduler.tla` was structurally blind to a bug
class. Tickless idle touches the idle/wake path -- the same surface -- so the
user voted **spec-first** (consistent with the SMP-redesign + death_wake
precedents + the bug-prone idle/wake lineage #788/#860/#809/#811).

The I-9 no-lost-wake property tickless leans on is already modeled in part:
`scheduler.tla` models `EnterWFI` + `IPI_Deliver`-clears-`wfi[dst]`, and
`tsleep.tla` models the deadline wake. The tickless change adds "at idle-enter,
arm a one-shot to the nearest deadline" -- a new action over existing state.

**Plan (model-first, BEFORE TI-2 impl):** extend `specs/scheduler.tla` with an
`ArmIdleOneShot` action (an idle CPU, having registered `idle_in_wfi`, arms a
one-shot to the nearest deadline) and the invariant that no work-arrival wake nor
deadline is lost across it; plus a `scheduler_buggy.cfg` counterexample
(arm-BEFORE-register, i.e. observe-then-register => a work-arrival in the window
is lost => the idle CPU sleeps to the backstop with runnable work pending). TLC
must be green on the clean cfg + violate on the buggy cfg before TI-2 lands. The
extension reuses the existing `scheduler.tla` state (it does not invalidate the
landed cfgs); if reuse proves to entangle the audited model, fall back to a
sibling `sched_tickless.tla` (the `sched_oncpu`/`sched_alpha` precedent of a new
module rather than mutating the load-bearing one). SPEC-TO-CODE + the spec
inventory (ARCH 25.2 + CLAUDE.md) update with the spec commit.

---

## 8. Expected result

HVF idle: 332% -> ~0% (the durable fix; production parity). No change to
running-thread behavior, the slice model, I-17, or the kernel-test timing.
Verified by re-running the #299 measurement at TI-3.
