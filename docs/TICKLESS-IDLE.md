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

### 3.3 Resume periodic on wake-to-running (the as-built restore, TI-2)

When an idle CPU wakes (one-shot fires, `IPI_RESCHED`, or any device IRQ), the
idle loop -- still inside its IRQ-masked region, before any placed work is
dispatched -- does two things:

```
timer_arm_this_cpu()    // restore the 1 kHz periodic tick
timerwait_tick()        // service any passed deadline explicitly
```

Both are load-bearing, and the TI-2 impl surfaced *why* the naive "just let the
one-shot's handler re-arm periodic" sketch is insufficient:

- **`timer_arm_this_cpu()` covers the IPI-wake I-17 window.** A CPU woken from
  tickless idle by an `IPI_RESCHED` (work placed) has its one-shot *still armed*
  to the far target -- no timer fired. Without an explicit re-arm, the placed
  thread would run up to the backstop (100 ms) with no slice tick, starving a
  co-runnable peer (an I-17 violation). Re-arming periodic here means the placed
  work runs with 1 kHz ticking immediately.
- **`timerwait_tick()` covers the deassert-vs-deadline hazard.** Re-arming
  periodic writes `CNTV_TVAL = g_reload > 0`, which *deasserts* the one-shot's
  pending timer IRQ (the generic timer is level-sensitive: `ISTATUS` drops when
  `TVAL > 0`). So the timer IRQ handler will NOT run for that fire -- and the
  handler is where `timerwait_tick` normally wakes the deadlined sleeper. Left
  there, the sleeper would never wake (a busy-spin: the idle loop re-reads the
  still-past deadline, arms a MIN one-shot, fires, deasserts, repeats). Running
  `timerwait_tick` explicitly here wakes the sleeper now; on an IPI-wake (no
  deadline passed) it is a cheap no-op scan.

The next `sched()` then dispatches the woken sleeper / placed work with periodic
ticking, or re-enters the idle park (re-arming the one-shot) if the wake found
nothing runnable (a redundant one-shot, or a deadline already serviced by
another CPU). The whole restore runs under the same `spin_lock_irqsave(NULL)`
region as the arm + WFI, so it completes before the pending IRQ is taken at the
unmask -- a placed thread never runs un-ticked.

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
D = timerwait_earliest_deadline()              // under g_timerwait.lock
arm one-shot at tickless_target_cnt(now, D, BACKSTOP)   // min(D, now+BACKSTOP)
// --------------------------
wfi
sched_set_idle_in_wfi(false)
// --- wake-to-running restore (new; section 3.3) ---
timer_arm_this_cpu()               // periodic, before any placed work runs (I-17)
timerwait_tick()                   // service the deadline (the deassert covers it)
// --------------------------------------------------
spin_unlock_irqrestore(NULL, s)
```

As-built, this whole body is the shared `sched_idle_park(bool tickless)` in
`kernel/sched.c`, called by `bootcpu_idle_main` (always `tickless=true`) and
`per_cpu_main` (`tickless=timer_armed` -- false until a secondary's timer PPI is
enabled, then the byte-identical pre-preempt WFI-on-IPI behavior). The pure
arm-decision `tickless_target_cnt(now, deadline, backstop)` is unit-tested.

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

## 8. Result (MEASURED at TI-3)

HVF idle: **332% -> ~0.3%** (the #299 method re-run: a direct `-serial file:`
HVF boot to login, steady-state `top`/`ps` = 0.3-0.7% process CPU, the vCPU
threads parked in `hvf_wfi` instead of churning `hvf_vcpu_exec` per-ms). A
~1000x reduction -- and below the 250 Hz stopgap's 5.2%, because a genuinely-
idle CPU now takes effectively zero ticks (the backstop is ~10 Hz) rather than a
4 ms tick. No change to running-thread behavior, the slice model, I-17, or the
kernel-test timing (the suite is 971/971 at `--cpus 4`, the secondary idle loop
tickless). The durable fix; production parity (Linux NO_HZ_IDLE / FreeBSD
dynticks).

**TI-3 close (verified green):** the SMP gate PASS -- 0 corruption across all
configs (default+UBSan x smp4/smp8, N=10 = 40 boots, 0 corruption / 0 timing);
the focused Opus-4.8-max audit 0 P0 / 0 P1 / 0 P2 / 3 P3 (CLEAN, NOT dirty: F1
stale-comment + F2 overflow-boundary-comment FIXED, F3 = this empirical proof,
the documented close gate); spec gate GREEN (`sched_tickless` clean +
`BUGGY_PARK` counterexample).

---

## 9. TI-4 -- the work-conservation regression + the push synthesis

Status: DESIGN (Direction A, user-voted 2026-06-21). TI-4a (the spec) lands
first; TI-4b..e fill in the as-built impl. Detail of the investigation:
`memory/project_ti4_research.md`.

### 9.1 The regression TI-3 missed

TI-3 was gated on CORRECTNESS (the SMP gate = 0 corruption) and IDLE COST (the
HVF re-measure = 332% -> 0.3%), but **never on multi-core THROUGHPUT.** A 2.4x
boot regression sailed through: HVF boot-to-`login:` went **7.2s -> 17.5s**
(ground-truthed, every timing boot to a healthy guest end-state).

Root cause (ground-truthed against the code): the never-stopped 1 kHz tick was
silently the **work-stealing RE-POLL.** Thylacine's v1.0 work-conservation on a
uniform topology is *pull-with-one-kick*: `ready()` enqueues a waking thread on
the WAKING CPU's own run tree, then `sched_notify_idle_peer()` kicks exactly ONE
idle peer (scan from `self+1`, no rotation) to come `try_steal`. The kick is
best-effort -- it wakes one peer, may re-target a not-yet-cleared peer, and is
suppressed entirely during the in-kernel test phase (`g_sched_notify_enabled`
off). The 1 kHz tick was the catch-all: every idle CPU re-ran `sched()` ->
`try_steal` every 1 ms, so whatever the single kick missed was pulled within
1 ms. NO_HZ_IDLE removed that re-poll; queued work then stranded on a busy CPU
until the 100 ms backstop -> the spawn+IPC-heavy boot serialized.

Two load-bearing re-poll consumers: (1) the in-kernel test phase (notify gated
off -> EVERY cross-CPU handoff relied on cpu0's idle re-poll; cpu0's
`bootcpu_idle_main` went tickless during tests because it always passed
`tickless=true`); (2) the production post-test phase (the single kick has gaps
the 1 ms tick covered).

**The process lesson (owned):** a tickless-idle change is a SCHEDULER change;
gating it on idle cost + correctness without a throughput gate was the miss. The
TI-4 fix MUST add a perf gate (section 9.5) so this cannot regress silently again.
The "load-aware backstop" experiments (a park-time "any peer busy" signal, a
50 ms activity-window) all FAILED -- a park-time signal cannot see work readied
AFTER the park, and an activity-window driven by a single ~20 Hz periodic poller
drags every idle CPU to a fast backstop (~100% idle). The fix is not a smarter
poll; it is to stop polling.

### 9.2 Prior art (SOTA convergence -- the two-mechanism minimum)

Three primary-source research passes (Linux NO_HZ_IDLE, FreeBSD ULE, seL4 +
Zircon) converge on splitting work-conservation into TWO distinct mechanisms,
and conflating them is exactly the TI-3 mistake:

- **PLACEMENT is push, never tick-driven -- everywhere.** Linux
  `try_to_wake_up` -> `ttwu_queue` -> unconditional `smp_send_reschedule` to a
  parked CPU; seL4 `possibleSwitchTo` -> enqueue-on-target + reschedule-IPI;
  Zircon `FindTargetCpu` (prioritizes idle cores) + `mp_reschedule`. The
  periodic tick was NEVER how a freshly-woken task reached an idle CPU. So
  stopping the idle tick creates no placement hole -- *if* placement pushes.
- **REBALANCE of ALREADY-QUEUED work is the leg the tick covered**, and a fixed
  idle backstop is the wrong shape for it (it is *periodic* -- the cost being
  deleted -- and *idle-side* -- the parked CPU cannot pull). The SOTA answer is
  the inverse: a BUSY CPU -- still ticking, since tickless stops only the IDLE
  tick -- detects imbalance and PUSHES (kicks) one idle peer. Linux moved nohz
  balancing pull->push specifically so idle CPUs stay fully tickless; ULE kicks
  an idle peer at enqueue when it has excess + keeps a slow (~1 s) `sched_balance`
  backstop; seL4 gives up auto-balancing entirely; Zircon keeps pull-steal but
  triggers it at reschedule context, never an idle poll.

Consensus minimal sound design: **push-complete PLACEMENT (eliminates the wakeup
poll) + push-on-overload REBALANCE (a busy CPU kicks an idle peer) + a coarse
periodic backstop as pure defense-in-depth.** No idle CPU ever polls.

### 9.3 The Thylacine mapping (Direction A)

The fix lands cleanly because most of it already exists:

- **Push-complete placement.** `select_target_cpu` is extended to PREFER an idle
  CPU when one exists (reading the existing `idle_in_wfi` flag), production-gated
  on `g_sched_notify_enabled`. The cross-CPU enqueue MECHANISM already exists --
  `ready_on(target)` + `need_resched_set(target)` + `sched_notify_cpu(target)`
  IPI -- but is gated behind `g_sched_hetero` (HMP-misfit only), inert on v1.0.
  The fix routes uniform-topology placement through it. SAFETY is already proven:
  `sched_alpha.tla` models `Place` with a NON-DETERMINISTIC target, so
  placement-on-idle is inside the proven envelope; `sched_tickless.tla` proves
  the place+IPI no-lost-wake (I-9). This fixes the boot spawn-storm at its root
  (spawns spread across idle CPUs immediately, one IPI each).
- **Push-on-overload rebalance.** A busy CPU's `sched_tick` (still running at
  1 kHz -- tickless stops only the IDLE tick) checks "my runq has surplus (>=2
  non-idle runnable) AND an idle peer exists" and kicks the peer to `try_steal`.
  Rides the tick we keep; replaces "every idle CPU re-polls every 1 ms" with "a
  busy CPU kicks one idle peer only on real surplus." Modeled by
  `sched_rebalance.tla` (section 9.4).
- **The 100 ms backstop stays** -- but becomes pure defense-in-depth (self-heal
  a dropped kick), NOT the work mechanism. So a genuinely-idle CPU still wakes
  ~10 Hz -> ~0.3% idle (the TI-3 win is preserved; it is no longer bundled with
  the regression).
- **cpu0 production-gating.** `sched_idle_park` computes `go_tickless = tickless
  && g_sched_notify_enabled`, so cpu0 stays PERIODIC during the in-kernel test
  phase (byte-identical to pre-tickless) -- closing the test-phase half of the
  regression. The secondaries already gate on `timer_armed`; cpu0 did not.

End state: **~0.3% idle AND full throughput AND proper rebalancing** -- the
complete Linux NO_HZ_IDLE model, mapped onto existing Thylacine mechanisms.

### 9.4 Spec (TI-4a; spec-first re-enabled for this surface)

`specs/sched_rebalance.tla` (a focused sibling, the `sched_oncpu`/`sched_alpha`/
`sched_tickless` precedent) models the ONE new mechanism -- the busy-side push of
already-queued work to an idle peer. Placement needs no new model (covered by
`sched_alpha.tla`). The model proves:

- `EventuallyParallelized` (surplus ~> ~surplus): queued surplus on a busy CPU,
  with an idle peer, is eventually taken off it (work-conservation). The
  property the busy-side kick restores.
- `NoLostWake` (~(parked /\ pending)): the kick respects register-then-observe
  (it lifts the kicked peer's park), so pushed work is never left asleep.

Two executable counterexamples: `buggy_nokick` (no kick -> surplus strands = the
TI-3 regression in model form, `EventuallyParallelized` VIOLATED) and
`buggy_nolift` (kick forgets the park-lift -> `NoLostWake` VIOLATED). The 100 ms
backstop is ORTHOGONAL and deliberately unmodeled, so `buggy_nokick` is a clean
stranded-work counterexample rather than a masked latency hiccup. TLC-green
model-first (clean: 117 distinct states) BEFORE the TI-4c impl. Mapping:
`specs/SPEC-TO-CODE.md`.

### 9.5 The perf gate (the missing throughput gate + the user's tool)

Built two ways, since a STEADY CPU-bound bench would NOT catch this regression
(the regression is wake/steal LATENCY under churn, not steady parallel
throughput):

- **Guest-measured boot-duration counter** (CNTVCT delta, boot-start ->
  `SYS_BOOT_COMPLETE`) wired into `tools/ci-smp-gate.sh` with a threshold (TCG
  for determinism). The cheap durable CI gate; it catches *this exact* regression
  (boot is spawn+IPC churn).
- **`/bin/cpubench`** (native libthyla-rs): single-core (ops/sec) + multi-core
  (N-worker aggregate + the scaling factor multi/single = the scheduler-health
  number) + a cross-CPU PING-PONG churn metric (thread A wakes B on another core,
  round-trips/sec = the wake/steal latency the regression hit). User tool +
  regression sentinel.

### 9.6 Sub-chunk plan

- **TI-4a:** `sched_rebalance.tla` + cfgs (this section's spec), TLC-green
  model-first. The spec commit (no kernel code).
- **TI-4b:** push-complete placement (`select_target_cpu` idle-preference) +
  cpu0 production-gating + the boot-duration counter. Measure the boot recovery
  in-guest.
- **TI-4c:** push-on-overload rebalance kick from `sched_tick`, validated against
  `sched_rebalance.tla`. The 100 ms backstop becomes defense-in-depth.
- **TI-4d:** the perf gate (boot-duration threshold in `ci-smp-gate`) +
  `/bin/cpubench`.
- **TI-4e:** focused Opus-4.8-max audit (the #788/#860/#809/#811/#926 idle/wake
  lineage) + SMP gate + HVF re-measure (confirm ~0.3% idle AND ~7.2s boot both
  return) + docs + close.

### 9.7 TI-4d AS-BUILT (the kernel work-conservation counter + `/bin/cpubench`)

Two instruments landed; together they RESOLVE the rebalance-vs-sequential fork
Direction A left open (and overturn Direction A's premise).

**The kernel work-conservation counter** (`kernel/sched.c`, exposed at
`/ctl/sched` + the `boot-wc:` banner line). A core that parks in WFI while a
runqueue holds queued-but-not-running work is the classic "wasted core" (the
work-conservation invariant -- "A Decade of Wasted Cores", EuroSys'16).
`sched_idle_park` samples `sched_has_runnable_work()` (a bounded, lock-free read
of each band's HEAD pointer only -- never a `runnable_next` walk, so it is safe
on the 3M-park hot path) at the instant it commits to parking; if work is queued,
the WHOLE park duration is charged as STARVED. Accumulators are split into a
TOTAL and a TICKLESS-only subset (`go_tickless == true`, i.e. production): a
starved PERIODIC park ends at the next <=1ms tick (the correct pre-tickless
baseline), so the TICKLESS `starved_ns`/`max` is the regression's clean signal in
isolation from the test-phase periodic re-poll. Diagnostic only -- consulted by
NO scheduling decision; relaxed stat atomics; two `timer_now_ns()` reads (a
CNTVCT register read, no vmexit) bracket the WFI.

**`/bin/cpubench`** (`usr/cpubench`, native libthyla-rs). Five modes mapping the
real load characters, each bracketing the `/ctl/sched` `wc-tickless` delta:
`single` (1-thread ops/sec baseline), `scale` (N long-running CPU threads ->
will-it-scale efficiency T1/Tn), `yield` (long-running threads that periodically
sleep -> stresses the idle/wake path), `storm` (thread spawn/join churn ->
threads/sec), `pingpong` (2-thread cross-CPU futex round-trip -> p50/p99/p99.9/max
us, the schbench + `perf bench sched pipe` shape). Methodology: TAIL percentiles
(p99/p99.9), NEVER the mean -- a "some wakes slow" bug is a tail event the mean
hides. A boot probe (joey, gated on a CLEAN run, not a perf threshold) prints the
numbers every boot; `cpubench <mode> [N] [iters]` runs one mode bigger from the
shell.

**THE FINDING (TCG, -smp 4, the clean production-phase measurement):**
- `scale` efficiency ~= 0.9-1.0x of ideal (near-perfect 4-core parallel scaling)
  -> **throughput is NOT regressed; the scheduler is work-conserving for steady
  parallel load.**
- `pingpong` p50 ~22us / p99 ~70us / p99.9 ~110us / max ~160us -> **cross-CPU
  wakeup latency is BOUNDED and prompt; the wakee does NOT hit the 100ms
  backstop.** The TI-4b push-placement delivers the wake.
- The kernel `wc-tickless` delta during pingpong shows starved parks with a
  `max_starved` ~= 103ms (a full-backstop park) -- BUT this does NOT inflate the
  workload's actual latency (p99.9 = 110us, three orders below 103ms). The
  `starved` counter is the OVER-BROAD strict-work-conservation signal: it counts
  an idle CPU as starved whenever work is queued ANYWHERE, even when that work's
  home CPU handles it promptly.

**Conclusion: the boot regression is PER-PARK OVERHEAD, not a work-conservation /
rebalance gap.** The workload's wakees already get prompt wakes (push-placement
works) and parallel throughput scales -- so TI-4c (push-on-overload rebalance)
would only reclaim idle cores for marginal energy/throughput, not fix latency
(which is already fine). The leading fix for the boot slowdown is
**tickless-only-on-deep-idle** (skip the arm+restore dance on the ~2.9M brief
IPI-woken parks; the tickless one-shot pays ~2-3 HVF VTIMER vmexits per park vs
the periodic ~1) -- now directly measurable with these two instruments. TI-4c is
deferred to a data-driven decision; TI-4e carries the fix + the audit + the HVF
re-measure.

### 9.8 TI-4e AS-BUILT (the kick + the 4 ms backstop + the HVF deep-park-latency finding)

**This supersedes the TI-4d-era "tickless-only-on-deep-idle" leading-fix note above** (that per-park-overhead theory was DISPROVEN: a NO_HZ state machine cut timer writes ~70x yet HVF boot got WORSE -- a write-reduction cannot fix a latency regression). TI-4e did a clean redesign (user-directed: "the fast boot falls out of the correct design, cpubench as the compass, not a smorgasbord of tricks"), root-caused the regression to ground, and landed the work-conservation push side. As-built (`docs/reference/15-scheduler.md` "Work-conservation under tickless idle (TI-4)" is the deep reference):

- **The busy-tick overload kick (TI-4c, BUILT).** `kernel/sched.c::sched_tick`, at the tail of a running CPU's 1 kHz tick: `if (g_sched_notify_enabled && t != cs->idle && cpu_has_surplus_for_kick(cs)) sched_notify_idle_peer();`. `cpu_has_surplus_for_kick` is a lock-free relaxed read of the INTERACTIVE+NORMAL run-tree heads (a queued head = migratable surplus; the running current is unlinked). The kick reuses `sched_notify_idle_peer` (find a registered-idle peer, IPI_RESCHED, stop-on-first-send); the migration is pull-realized (the kicked peer's `try_steal`). Modeled by `specs/sched_rebalance.tla::Overload` (clean + `buggy_nokick` [the TI-3 regression] + `buggy_nolift` [the lost-wake], TLC-green; re-ran GREEN). This is the Linux `NO_HZ_IDLE` shape: idle CPUs deep-park, a still-ticking busy CPU drives rebalancing.
- **The affinity-ready seam.** `thread_may_run_on(t, cpu)` (always-true today) is the single future plug point for a `SYS_SCHED_SETATTR` per-thread affinity mask, consulted at `select_target_cpu` (placement) + `try_steal` (steal). User-directed "build it pluggable"; inert today, so affinity is not foreclosed.
- **The 4 ms re-poll backstop (TI-4e, user-voted).** `TICKLESS_IDLE_BACKSTOP_NS` 100 ms -> 4 ms.

**THE ROOT CAUSE (the reason there is no further guest fix).** The boot "regression" is **HVF deep-park vCPU resume latency, NOT a guest bug.** Wake-source telemetry (`sched_wc_stats.tickless_{ipi,oneshot}_wakes`, in the `boot-wc:` banner) measured **99.85% of tickless parks woken by an IPI** (not the backstop) -- the wake path is push-complete. But resuming a DEEP-parked vCPU via SGI costs **~0.85 ms under HVF vs ~7 us hot** -- an emulation artifact (HVF GICv2-MMIO vmexits + the host vCPU-thread resume; #299/#890), not a scheduler defect. On bare metal an SGI to a WFI'd core is hardware-fast (~ns), so the tickless+kick design gives fast boot + ~0% idle THERE = the ideal. The 4 ms re-poll keeps the dev-loop vCPUs warm -> ~0.09 ms IPI resumes -> **HVF boot ~7 s (= tickful gold) vs ~17-35 s at 100 ms**, at ~5% HVF idle (vs 0.3%). The kick earns its place as the fast (~1 ms) parallel-surplus catch (cpubench starvation -34..-60% across parallel modes); the SEQUENTIAL boot is dominated by the per-wake latency the re-poll mitigates. v1.x: an adaptive (warm-while-active / deep-when-idle) or accel-gated backstop reclaims 0.3% HVF idle without the dev-boot cost. **Bare-metal confirmation owed at Lazarus/RPi.**

**Validation:** spec gate GREEN; in-kernel suite 972/972 (incl. `scheduler.cpu_surplus_for_kick`); HVF boot-ms A/B (tickless+kick @ 4 ms = 7.2-8.6 s vs the tickful `--no-tickless` gold 7.1 s; the 100 ms deep-park was 16-35 s); focused Opus-4.8-max audit + concurrent self-audit CLEAN **0 P0 / 0 P1 / 0 P2 / 2 P3** (both pre-existing test-fragility; `memory/audit_ti4_closed_list.md`); SMP gate. NO new §28 invariant (composes I-8 + I-9). The `cpubench` (13-mode) + the `THYLACINE_NO_TICKLESS` build flag (the tickful gold-standard lever) are the durable compass.
