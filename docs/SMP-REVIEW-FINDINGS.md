# SMP Layer Deep Review — Findings

**Branch**: `deep-smp-review` @ `4f70e9c`. **Status**: REVIEW COMPLETE; fix DECIDED
(user-voted 2026-06-05: **α with a logic-verified HMP foundation**). This is the
evidence record for the user-directed deep SMP review (`docs/SMP-REVIEW-HANDOFF.md`).
The empirical experiments (§4) and the two model-first artifacts (`specs/sched_oncpu.tla`
diagnostic + `specs/sched_alpha.tla` target design, both TLC-green) are committed. The
implementation arc is scripture-first (#862 = the binding ARCH §8 rewrite) → impl
(#863 soundness core, #864 HMP foundation) → CI multi-boot (#865) → adversarial audit
(#866). The HMP-foundation analysis (HVF closes the speed gap, not the heterogeneity
gap; placement *logic* verifiable now via a synthetic asymmetric DTB; EAS empirical
tuning deferred to real heterogeneous hardware) is the basis for §7–§9 below.

---

## 0. Bottom line

The SMP layer is a **patchwork — in a precise, demonstrable sense**, not a vague one,
and not a five-alarm fire either:

1. **The formal model that was supposed to gate this subsystem is structurally blind
   to the entire class of bugs that actually occur.** `specs/scheduler.tla` models
   work-stealing as a *single atomic transfer* and has **no `on_cpu` variable** — so it
   cannot represent the multi-step claim/load/clear window or the boot-CPU
   deadlock-dispatch route where #788 / #806 / #860 all live. It proved the high-level
   state machine sound *under an atomicity assumption the implementation does not
   satisfy*. That false confidence is the root of the accretion.

2. **#860 is real** and now **machine-checked**: a faithful model (`specs/sched_oncpu.tla`,
   written for this review, *with* `on_cpu` + the multi-step switch + the deadlock-path
   pointer) reproduces the two-CPUs-one-thread corruption in **10 states (<1 s)**. The
   bug is also **deterministically reproducible on this box** (the in-tree-insert
   precondition tripwire fires 3/3 under UBSan-smp4).

3. **`g_bootcpu_idle` — the boot-CPU off-tree idle special case — is the locus.** It
   spawned #860, an off-tree-RUNNABLE state invisible to the lifecycle walks, a racy
   deadlock guard, and a pair of **mutually contradictory comments** in the tree
   (`kernel/main.c:528-537` calls folding-it-into-the-run-tree "a deferred cleanup item";
   `kernel/sched.c:624-634` says folding-it-in "would REINTRODUCE the bug"). The project's
   own source disagrees with itself about whether this is cleanup or hazard. That
   contradiction *is* the patchwork, in the codebase's own words. The `scheduler.tla`
   authors explicitly flagged the boot-CPU idle dispatch as **"architectural ... warrants
   its own session"** and deferred it (P4-Ic6 / R12-sched comment block). This is that
   session.

4. **The handoff's premise does not survive ground truth.** It scoped this review on
   "option A regresses smp8 10/10 ⇒ masking stack is N-deep ⇒ consider a from-scratch
   redesign." 40+ boots (default + UBSan + E-core, smp4/smp8) did **not** reproduce that
   regression (25/25 clean plain-smp8). **Option B closes #860 (UBSan-smp4 4/4 where the
   precondition build crashed 3/3) AND passes smp8 (15/15)** — the masking stack is
   **1-deep at the `g_bootcpu_idle` locus**, no independent sibling. So: **patchwork —
   yes (the blind gate + the fragile special case + the self-contradicting comments + a
   real `idle_in_wfi` latency bug + a host-fragile test suite); from-scratch rewrite —
   not warranted.** The targeted simplification (≈ option B, cleanly expressed) is the
   fix. (This is the same shape as this session's earlier correction of #857.)

**Recommended response** (detail in §7): re-enable spec-first for the SMP mechanism and
adopt `sched_oncpu.tla` as its gating model; apply the **targeted simplification** that
retires `g_bootcpu_idle` (option B, cleanly expressed) — closing #860, the off-tree
state, and the racy guard *by construction*; fix the genuinely-real secondary findings
(stale `idle_in_wfi`; unasserted steal invariant); harden CI to **multi-boot** (the
process failure that masked all of this); and design **HMP-ready abstractions** without
building full EAS (v1.0 treats cores uniformly — ARCH §8.1 non-goal).

---

## 1. Method

- **Full static read** of the SMP surface: `kernel/sched.c`, `kernel/thread.c`,
  `kernel/smp.c`, `arch/arm64/context.S`, the multi-thread reap/terminate paths in
  `kernel/proc.c`, the boot bringup in `kernel/main.c`, and the two existing specs
  (`specs/scheduler.tla`, `specs/sched_ctxsw.tla`).
- **A faithful formal model** (`specs/sched_oncpu.tla`) that re-introduces the mechanism
  `scheduler.tla` abstracts away (`on_cpu`, the per-CPU lock held across the switch, the
  deadlock-path dispatch, BootIdle's stealability), parameterized over the impl variants,
  TLC-checked.
- **Empirical multi-boot** under default-smp8, UBSan-smp8/smp4, and the `taskpolicy -b`
  E-core amplifier (the #789 substrate the handoff flags as a race amplifier), with
  full-UART capture (`/tmp/smp-repro*`).
- **Two independent adversarial Opus prosecutors** + self-audit (the audit-in-flight
  discipline), with the findings triaged against ground truth.

---

## 2. FINDING 1 — the gating spec is blind to the bug class (the structural root)

`specs/scheduler.tla` (929 lines) models:
- `Steal(stealer, victim)` as a **single atomic step** ("Modeling as a single atomic step
  is sound because the impl's two-lock window contains the entire transfer (no observable
  intermediate state between unlink and rebase)" — scheduler.tla ~509-524). The impl's
  reality is a *claim* (`on_cpu := true` under the peer lock) **then** a *load* (ctx
  switched in) **then** a *clear* (on_cpu := false in the resuming thread's tail) — three
  steps across two CPUs.
- **No `on_cpu` variable at all.** The flag that the entire SMP safety argument rests on
  (the signal `thread_free` / `wait_pid` / `wake_rendez_waiter` spin on) is unmodeled.
- `RunnableInQueue` as an **iff**: "a thread is RUNNABLE *iff* it sits in some runqueue"
  (scheduler.tla ~737-741). The impl's `g_bootcpu_idle` is **RUNNABLE and in no
  runqueue** by design (the off-tree resting state) — a legal impl state the spec
  declares impossible.
- The boot-CPU idle dispatch was **explicitly deferred** at the spec level: "Impl deferred
  (the on_cpu protocol intersection with WFI-in-place requires either an on_cpu refactor
  or a separate-idle-thread refactor on the boot CPU; choice is architectural and warrants
  its own session)" — scheduler.tla ~74-77.

**Consequence**: every historical "scheduler.tla GREEN" was true of an abstraction that
omits the exact windows where #788/#806/#860 occur. The spec gave false confidence; the
implementation accreted a point-patch at each window as bugs surfaced empirically (the
masking-bug stack).

The implementation has *also* drifted from the committed design independently of the
spec: ARCH §8.4 specifies **red-black trees keyed on `vd_t`**; the impl uses **O(n)
sorted doubly-linked lists** (`insert_sorted`, `kernel/sched.c:217`). Not a soundness
issue, but a marker of design/impl divergence.

---

## 3. FINDING 2 — #860 formally reproduced; A/B at the spec level

`specs/sched_oncpu.tla` re-introduces `on_cpu[t]`, a per-CPU `locked[c]` held across the
multi-step switch (the faithful image of `cs->lock` held until `finish_task_switch`,
which is what makes a peer's `spin_trylock` skip a mid-switch CPU), and the boot-CPU
`DeadlockDispatch` of `BootIdle` via a runqueue-independent pointer. It is parameterized:

| Config (`specs/sched_oncpu_*.cfg`) | in-tree | unstealable | guard | TLC result |
|---|:---:|:---:|:---:|---|
| `prefix860` (pre-fix) | yes | no | no | **Safety VIOLATED** — `NoSimultaneousRun` (the #860 double-run), 110 states |
| `intree_guard` | yes | no | yes | **Availability VIOLATED** — the guard *fires* (a loud extinction); proves the guard alone is not a fix |
| `optionB` | yes | **yes** | yes | **CLEAN** |
| `optionA` (on the branch) | no | yes | yes | **CLEAN** |

The `prefix860` counterexample (10 states, the exact #860 mechanism the prior session
root-caused empirically):

```
State  9: <StartSwitch(c1)>  current = (c0 :> w1 @@ c1 :> bi)   -- c1 STOLE bi from runq[c0]
State 10: <StartSwitch(c0)>  current = (c0 :> bi @@ c1 :> bi)   -- c0's deadlock path re-dispatched bi
                                                                   => bi RUNNING on BOTH cpus (I-21 violation)
```

**Two decisive reads:**
1. The old `scheduler.tla` *cannot* produce this — its atomic `Steal` and missing
   `on_cpu` smooth over exactly the claim/dispatch race. The new model finds it in <1 s.
   This is the concrete demonstration that the gating spec was blind.
2. **Both option A and option B are model-clean.** So whatever empirically distinguishes
   them at smp8 (below) is **below this state-machine abstraction** — memory ordering /
   idle-loop dynamics / kstack lifecycle. That a point-fix can be model-clean yet
   empirically differ is the patchwork signature.

---

## 4. FINDING 3 — empirical ground truth

Method note (DEBUGGING-PLAYBOOK): single boots lie; a rebuild detunes a timing race;
distinguish failure modes carefully.

- **Anchor — the box genuinely exercises #860.** Reverting just the off-tree guard
  (so `g_bootcpu_idle` goes in-tree) and booting **UBSan-smp4 ⇒ 3/3 EXTINCTION**
  `"#860: g_bootcpu_idle entered a run tree (must stay off-tree)"` — the permanent
  precondition tripwire fires *deterministically*, past the test suite, at production
  bringup. The harness sees the bug; the off-tree change is load-bearing.

- **Option-A default-smp8, unloaded ⇒ 10/10 PASS.** No scheduler corruption observed
  without amplification. (This was initially misleading — see the next line. Single
  unloaded boots are *under-powered* against a load-dependent race.)

- **Option-A default-smp8 under the `taskpolicy -b` E-core amplifier ⇒ 12/12
  EXTINCTION — but it is the WRONG failure.** All 12 die at
  `FAIL: IRQ-to-userspace p99 exceeds CI sanity budget` — the `test_irq_latency_bench`
  CI-sanity ceiling (50 ms; ~8 ms typical under QEMU) blown by E-core ~2.5x throttling.
  This is a **host artifact** (the #859 class) that kills the suite *before* production
  bringup — so `taskpolicy -b` cannot observe the production-bringup scheduler
  corruption (the boot never gets there). The E-core amplifier is the *wrong tool* for
  this particular bug. (It is, separately, a real **CI fragility**: a host-timing
  assertion that hard-extincts the kernel — see §7(d).)

- **Crux — option-A UBSan-smp8 ⇒ 15/15 PASS.** The handoff's `"sched: invalid prev
  state"` production-bringup corruption did **NOT** reproduce. Combined with the
  default-smp8 10/10 PASS, that is **25/25 clean plain-smp8 boots**.

- **E-core amplifier cannot reach the production path.** `taskpolicy -b` smp8 dies at
  the **in-kernel test suite** every time — the latency gate (50 ms), or with the gate
  relaxed, ~4 *other* timing-fragile tests (`test_stalk_lifetime_no_leak`, `stalk`, the
  torpor bound, ...) at 706/710. These are **host-throttling artifacts** (the #859
  class), not scheduler corruption, and they kill the suite *before* production bringup —
  so a uniform-throttle amplifier structurally cannot observe the production-bringup race.

- **Net: across 40+ boots (default + UBSan + E-core, smp4/smp8), the handoff's specific
  option-A smp8 corruption did not reproduce on this box.** This is a material correction
  to the handoff's premise (its "option A regresses smp8 10/10" is *unconfirmed* on this
  host — most likely host-specific or confounded, the same pattern as this session's
  earlier correction of #857's "console_mgr" framing). #860 itself remains rock-solid
  confirmed; the structural verdict (§6) and the option-B recommendation (§7) are
  independent of whether option A's specific corruption reproduces.

- **A second, separate finding fell out of this**: the in-kernel **test suite is
  host-timing-fragile** — multiple tests hard-fail (and `extinction` the kernel) under
  E-core throttle. That is a real CI-robustness problem (§7(d)), distinct from any
  scheduler soundness bug.

- **Decisive — option B (g_bootcpu_idle in-tree + unstealable + guard, tripwire removed)
  ⇒ CLEAN.** **UBSan-smp4 4/4 PASS** — the *exact* config where the in-tree+tripwire build
  extincted 3/3; option B's unstealability closes the #860 path (g_bootcpu_idle enters the
  tree but is never stolen → no double-run). **Default smp8 15/15 PASS** — no regression
  (the pre-#860 `pick_next` dynamics are smp8-clean). Zero extinctions. This is the
  handoff's decisive experiment: **option B closes #860 and does NOT leave smp8 red** — so
  **the masking stack is 1-deep at the `g_bootcpu_idle` locus; no independent sibling race
  surfaced.** Matches the model (`optionB.cfg` clean) and both prosecutors' recommendation.

---

## 5. FINDING 4 — prosecutor-1 triage (10 findings, ground-truthed)

An independent Opus prosecutor on the on_cpu/steal/deadlock surface returned 10 findings.
Triaged against the code:

| # | Claim | Disposition |
|---|---|---|
| F1/F2/F8 | smp8 sibling = `sched()` re-entrancy via the idle-loop unmask corrupting the single-slot `prev_to_clear_on_cpu` handoff | **REFUTED as a corruption mechanism.** The idle-loop unmask happens *after* `sched()` returns, so a preempt there starts a *fresh* (not nested) `sched()`; and `cs->lock` is held across the entire switch+resume, so the per-CPU handoff slots cannot be clobbered by another same-CPU activation. The *observation* that option A makes the deadlock path the sole dispatcher is valid (and is the structural smell). |
| F4 | the off-tree RUNNABLE `g_bootcpu_idle` state is invisible to `sched_remove_if_runnable` / `sched_dump_runnable` / `in_run_tree` (they walk trees only) | **REAL** structural smell (P3). Contained today (g_bootcpu_idle is never freed), but it is a RUNNABLE thread no tree-walk can see — exactly the kind of special-case the review should excise. |
| F7 | `idle_in_wfi` is left stale-TRUE while `sched()` switches the idle thread away to real work; a busy secondary then looks idle to `sched_notify_idle_peer`, which spuriously IPIs it and *stops searching* (first-match), so a genuinely-idle peer is skipped | **REAL** (P2/P3 latency/efficiency). Bounded by the periodic timer (~1 ms), not a hang, but the flag genuinely lies for the whole work-runtime. |
| F3 | `try_steal` asserts neither `state==RUNNABLE` nor `!on_cpu` on the victim; the load-bearing "a tree only ever holds on_cpu==false threads" invariant is undocumented + unasserted | **REAL** hardening gap (P2/P3). The lock-across-switch closes the window today; any future lock-hold shortening silently opens a steal-a-running-thread UAF. Add the assertion. |
| F5 | the deadlock-path `on_cpu` guard is racy | **REFUTED** for option A: `g_bootcpu_idle` is cpu0-only (unstealable), so `on_cpu[bi]` has a single writer (cpu0) — the guard is reliable same-CPU program order. |
| F6 | `wake_rendez_waiter`'s on_cpu spin runs under `r->lock`, risking a circular stall | **REFUTED as deadlock** (the completer — the resume tail — needs only `cs->lock`, never `r->lock`); a mild latency hazard at most (P3). |
| F9 | `g_cpu_online_count` read without release/acquire | minor, boot-only (P3). |
| F10 | `thread_switch` sets on_cpu/current lock-free + unmasked, divergent from `sched()` | **REAL but dormant** (test-only, no preemption during tests) (P3). A redesign should delete or align it. |

Net: the headline smp8 hypothesis is **refuted**, but the pass surfaced **three genuine
(if minor) issues** (F7, F3, F4) that a clean redesign should close. **No confirmed
P0/P1 in option A from the model or this prosecutor** after triage — consistent with the
unloaded 10/10 PASS. (A second prosecutor is hunting the *sub-abstraction* mechanism for
the crux corruption; result pending.)

---

## 6. Verdict — patchwork (the precise sense)

Not "all is well" (dismissive of the user's correct instinct), not "five-alarm fire":

- **Correctness rests on un-modeled, empirically-discovered details.** The gating spec is
  blind to the on_cpu protocol; the impl's safety depends on the lock-held-across-switch
  discipline, RELAXED-vs-RELEASE orderings, and the off-tree resting state — none of which
  the spec captures and several of which are unasserted.
- **A structural special case keeps spawning bugs.** `g_bootcpu_idle` exists only because
  cpu0's boot stack belongs to `kthread`, so cpu0's idle cannot be a bare-boot-stack
  thread like the secondaries' — so it got a *real kstack*, which makes it *stealable*,
  which is #860; and an *off-tree* workaround, which is F4 + the racy-guard surface + the
  contradictory comments. Every property of this special case is load-bearing for a
  *different* reason than it was introduced.
- **The masking-bug stack is real**: the #788/#806/#807/#808/#860 saga is one subsystem's
  windows surfacing one at a time, each closed by a point patch, because the gate (single
  boot + an abstraction-blind spec) could never see them as a class.

The right response is therefore **not** another point fix and **not** necessarily a
ground-up rewrite — it is to (1) make the gate honest (model the mechanism; multi-boot the
CI) and (2) excise the structural special case so the bug class cannot recur.

---

## 7. Recommended forward path

**STATUS (2026-06-05): the soundness core (#863) is LANDED + multi-boot-verified.** It
implements (b) + (c) + the model gate from (a): `bootcpu_idle` is now a CPU-pinned
(`cpu_pinned`) in-tree `BAND_IDLE` thread on a dedicated BSS stack
(`g_bootcpu_idle_stack`), dispatched by ordinary `pick_next`; the deadlock path /
off-tree state / `insert_sorted` tripwire / `g_bootcpu_idle` are all retired; the steal
gate is `!cand->cpu_pinned` (generalizing the secondaries' old `kstack_base==NULL` skip,
to which `kthread` is also folded); `idle_in_wfi` is fixed (F7: set `=(next==cs->idle)`
at every switch so a CPU running stolen work no longer looks idle); and `pick_next` /
`try_steal` `ASSERT_OR_DIE` `state==RUNNABLE && !on_cpu` on every victim (F3). Validated:
`sched_alpha.tla` TLC-green (gate); **default smp4 710/710 PASS**; **UBSan-smp4 multi-boot
clean** (the #860 amplifier, was ~33-43% crash on the broken option-A build → now 0
corruption across the run); smp8 + UBSan-smp8 multi-boot clean (see `tools/smp-multiboot.sh`,
the (d) deliverable). The handoff's "option A regresses smp8" premise did NOT reproduce.
The HMP foundation (e) is #864; the formal audit is #866. **Follow-up #867 LANDED**: the
cpu0-idle stack now gets a real MMU guard page (`g_bootcpu_idle_stack` is a `struct
secondary_stack` mapped no-access by `build_page_tables`, bounds-checked fail-loud,
recognized by `fault.c`, and proven to fault via `tools/test-fault.sh bootcpu_idle_guard`)
-- restoring the protection the retired thread_create'd g_bootcpu_idle had, which matters
most on headless small SoCs where a silent stack-corruption is brutal to chase. **Follow-up
#868 LANDED**: cpu0 now ATTACHES IPI_RESCHED (`smp_boot_cpu_ipi_init`), so a peer's
`sched_notify_idle_peer` wakes cpu0's idle immediately like any secondary -- cpu0 is a full
SGI scheduling peer (its always-armed timer remains a <=1ms backstop). Proven by the
`smp.ipi_resched_smoke` test's new cpu0-self-reception check. Both #863 deferrals are now
closed; the two SMP follow-up tasks are done.

**STATUS (2026-06-05): the HMP foundation (e) = #864 is LANDED + multi-boot-verified.** It
implements the placement-seam + capacity + util + `balance()`-shape half of the redesign
(ARCH §8.4.3/§8.4.4), all INERT on the uniform v1.0 targets so runtime behavior is
byte-identical to #863:
- **`select_target_cpu(t, prev_cpu)`** (policy) + **`ready_on(target_cpu, t)`** (mechanism)
  separated; `ready()` = `ready_on(select_target_cpu(t, self), t)`. Homogeneous → returns
  prev → `ready_on(self)` == the pre-#864 enqueue. Cross-CPU enqueue holds exactly one
  `CpuSched` lock (no nesting; cannot cycle with `try_steal`/`sched_remove_if_runnable`).
- **Per-CPU `capacity`** parsed from the DTB `capacity-dmips-mhz` (`lib/dtb.c::dtb_cpu_capacity`
  + `sched_capacity_init`, composes with I-15); **per-task `util`** (PELT-style EWMA, accrue
  on tick / decay on block; in `struct Thread`'s tail padding -- `sizeof` unchanged 1136).
- **`balance_pull`** = the v1.0 pull-only abstraction; the push enqueue primitive (`ready_on`
  + `sched_notify_cpu`) already exists, so misfit-push is additive, not a rewrite.
- **Logic-verified** vs a synthetic asymmetric DTB: `scheduler.capacity_normalize_synthetic_dtb`
  + `scheduler.place_by_capacity_synthetic_dtb` (heavy→big, light stays, heavy-on-biggest
  stays) + `scheduler.select_target_cpu_homogeneous_is_prev` (v1.0 behavior-preservation) +
  `scheduler.ready_on_cross_cpu_enqueue` (the cross-CPU mechanism). Safety under arbitrary
  placement is `sched_alpha.tla`'s `Place` (no model change needed; the impl refines it).
- The EMPIRICAL EAS tuning (PELT decay, energy model, schedutil/DVFS, misfit push) stays
  DEFERRED to real heterogeneous HW (the verification boundary, §8.4.4).

Validated: `sched_alpha.tla` TLC-green (97 distinct states); **default smp4 714/714 PASS**
(710 + the 4 new HMP tests); **UBSan-smp4 multi-boot 12/12, 0 corruption** (the #860
amplifier); default-smp8 + UBSan-smp8 multi-boot clean. The remaining SMP work is #865 (wire
`smp-multiboot.sh` into CI + soft-warn the host-fragile timing tests) and #866 (the formal
adversarial audit of the #863+#864 surface).

(a) **Re-enable spec-first for the SMP mechanism.** Adopt `specs/sched_oncpu.tla` as a
    gating model alongside `scheduler.tla`; extend it as the mechanism evolves. The
    natural re-enabling point the user flagged ("an invariant-bearing feature that
    genuinely benefits from machine-checked exploration") is exactly here.

(b) **Targeted simplification — retire `g_bootcpu_idle`.** Make the boot CPU's idle a
    normal **in-tree, unstealable** idle thread, `pick_next`-dispatched like every other
    CPU's idle — eliminating the deadlock-path special case, the off-tree RUNNABLE state
    (F4), and the racy guard (F5) *by construction*. This is **option B**, cleanly
    expressed: it keeps the pre-#860 `pick_next` dispatch dynamics (which the handoff
    records as smp8-clean — "smp8 20/20 before the fix") while making the idle unstealable
    (closing #860). The unstealability should be a clean per-Thread **pin** mechanism
    (`cpu_pinned`), not the current `cand != g_bootcpu_idle` special-case in `try_steal`,
    and `kstack_base==NULL` threads (the secondaries' idles) fold into the same pin rule.
    The model says option B is clean; the empirical option-B-at-smp8 experiment (§4,
    pending) is its confirmation.

(c) **Fix the genuine secondary findings**: clear/`set` `idle_in_wfi` so it means "about
    to wfi" not "entered the idle loop body" (F7); assert `state==RUNNABLE && !on_cpu` on
    every `try_steal` / `pick_next` victim (F3), making the load-bearing invariant loud;
    delete-or-align `thread_switch` (F10).

(d) **Harden CI to multi-boot — the process fix that masked everything.** `tools/test.sh`
    (and any gate) must boot the sanitizer + smp8 matrices **N≥10** and fail if ANY boot
    crashes. Separately, reconsider whether `test_irq_latency_bench` should *hard-extinct
    the kernel* on a host-timing budget miss (it converts host throttling into a "kernel
    crash"); a soft-warn or host-tier-aware budget would stop it masking real failures.

(e) **HMP-ready abstractions, homogeneous v1.0** (see §8).

The depth of (b) — a targeted simplification vs a from-scratch scheduler rewrite — is the
**architectural fork for the user** (§9).

---

## 8. HMP assessment (the user's question — magnitude)

ARCH §8.1 already lists "Energy-aware scheduling (big.LITTLE). v1.0 treats all cores
equal" as an explicit **non-goal** — so HMP-ready-but-homogeneous is *already* the
committed posture; this review only sharpens it.

- **Host HMP as a tool (now):** this dev Mac's P/E cores are *why* these races surface
  (macOS parks QEMU vCPUs across them; an E-core drags a synchronized guest ~2.5x — the
  #789 substrate). `taskpolicy -b` is a real race amplifier and was used in this review.
  Caveat learned here: it amplifies the *latency-budget* test to the point of masking the
  scheduler path, so it is the wrong amplifier for production-bringup races (use UBSan +
  many boots there).
- **Target HMP as a design axis:** real ARM64 targets are mostly heterogeneous (Apple
  Silicon, big.LITTLE/DynamIQ); the Lazarus first board (RPi 400, 4×A72) is the
  homogeneous exception. Magnitude:
  - **Topology from DTB** (parse `cpu-map` + `capacity-dmips-mhz` → per-CPU capacity
    class): **small**, composes with I-15 (hardware-from-DTB). Worth baking into the
    redesign's abstractions now.
  - **Capacity-aware placement** (bias latency-sensitive → P, background/idle → E): a
    **moderate** placement heuristic on `ready()` target-CPU choice + load balance, *if*
    the per-CPU-capacity abstraction exists.
  - **Full EAS** (PELT utilization + energy model + asymmetric balance + DVFS):
    **large** (Linux-EAS-scale) — **post-v1.0**.
- **Recommendation**: design the abstractions HMP-ready (per-CPU `capacity`,
  core-type-tagged queues, a placement hook) so capacity bias can land incrementally;
  ship homogeneous-treatment + topology plumbing for v1.0. **HMP is orthogonal to the
  correctness races — fix correctness first.**

---

## 9. The architectural fork (for user signoff)

The evidence supports model-first + excising the `g_bootcpu_idle` special case. The open
question is **scope**:

- **Option α — Targeted simplification** (§7(b)+(c)): retire `g_bootcpu_idle`, clean
  pin-based unstealability, model it with `sched_oncpu.tla`, fix F7/F3/F10, multi-boot CI.
  Smaller, lower-risk, reuses the audited surrounding machinery. Closes #860 + the
  off-tree class. ~A focused chunk + its audit.
- **Option β — From-scratch SMP scheduler + thread-lifecycle**, model-first: re-derive the
  on_cpu protocol, the idle model, steal, and the wait/wake integration from first
  principles against `sched_oncpu.tla` extended to the full mechanism, with HMP-ready
  abstractions built in. Larger; the cleanest end state; the biggest risk surface.

**The experiments have now decided this (§4): α is sufficient; β is not warranted.**
Option B closes #860 (UBSan-smp4 4/4 where the precondition build crashed 3/3) AND passes
smp8 (15/15) — the masking stack is **1-deep at the `g_bootcpu_idle` locus**, with no
independent sibling race surfaced by the model, the empirical sweeps, or either
prosecutor. β (from-scratch) would be re-deriving machinery that the targeted excision
already makes sound; it is the higher-risk path with no soundness payoff the evidence
demands. **Recommendation: α.** The clean implementation of α should express
unstealability as a per-Thread `cpu_pinned` field (folding in the secondaries'
`kstack_base==NULL` idles under the same rule) rather than the `cand != g_bootcpu_idle`
special-case, retire the deadlock-path special case + the off-tree state + the racy guard,
fix `idle_in_wfi` (F7), assert the in-tree⇒not-on_cpu steal invariant (F3), adopt
`sched_oncpu.tla` as a gating model, and harden the CI to multi-boot. The from-scratch
β remains available as a v1.x/post-v1.0 cleanliness exercise if desired, but the soundness
case does not require it.

**This corrects the handoff's premise honestly.** The handoff scoped this review on the
belief that "option A regresses smp8 10/10 ⇒ the masking stack is N-deep ⇒ consider a
from-scratch redesign." Ground truth (40+ boots) does not reproduce that regression, and
option B's clean close shows the stack is 1-deep. The deep review was the right call — it
found the genuine #860 + the blind gate + the real `idle_in_wfi` bug + the test fragility,
and it produced a model-proven fix — but its conclusion is **targeted simplification, not
from-scratch rewrite.** (Same shape as this session's earlier correction of #857.)
