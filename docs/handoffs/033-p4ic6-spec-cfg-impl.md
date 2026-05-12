# Handoff 033 — P4-Ic6-spec + P4-Ic6-cfg + P4-Ic6-impl (R12-sched closure trilogy)

**Tip**: `c87779e` (P4-Ic6-impl hash fixup) on `main`.

This window closes **R12-sched** — the scheduler's "no runnable peer system-wide" extinction that previously fired when kthread called `wait_pid` on a hardware-IRQ-blocked child (driver waiting for a device IRQ; parent waiting for child exit; nobody runnable). Three chunks landed, each pinning a different layer:

1. **P4-Ic6-spec** (`f050512` substantive / `25b4330` hash fixup) — `scheduler.tla` extension. New `HardwareWakeProgress` temporal property + `Liveness_Hwake` fairness clause (SF on `Wake(t)` encoding "hardware IRQs always eventually deliver") + two new cfg variants (correct + counterexample). The spec captures the *intent* of the impl: a SLEEPING-non-cond-waiter thread must eventually leave SLEEPING.

2. **P4-Ic6-cfg** (`849c053` / `719dedd`) — `Wake(t)` and `WakeAll` action refinement (atomic `wfi[cpu]` clear on the target) + `scheduler_liveness_wfi.cfg` reparameterization (2T × 2C → 1T × 2C). Closes a pre-existing wfi-cfg LatencyBound violation that turned out to be two distinct issues: a real spec-modeling gap (closed by Fix A) + a CHOOSE-determinism artifact at 2T × 2C (eliminated by reducing scope to 1T × 2C, matching `scheduler_liveness.cfg`'s same workaround).

3. **P4-Ic6-impl** (`3c1951d` / `c87779e`) — kernel-side. Boot CPU now has a dedicated idle thread `g_bootcpu_idle` distinct from kthread, allocated in `boot_main`, registered via `sched_set_bootcpu_idle()`, kept **OFF the run tree**, and used solely as the explicit fallback in `sched()`'s deadlock path. Replaces the extinction with switch-to-`g_bootcpu_idle`. `test_virtio_blk_probe.c` converted yield-poll → `wait_pid` (the R12-sched workaround is retired).

**External update**: Stratum has reached complete 9P2000.L + POSIX surface. The Phase 5 (= ROADMAP §7 by canonical numbering; "9P client + Stratum integration") external blocker is now resolved — Stratum is ready and waiting.

---

## Verify-on-pickup

```bash
cd /Users/northkillpd/projects/thylacine
git log --oneline -10        # expect c87779e at top
tools/build.sh kernel        # clean
tools/test.sh                # expect 227/227 PASS, ~433 ms boot
rm -rf build/kernel-undefined && tools/test.sh --sanitize=undefined
                             # expect 227/227 PASS, ~440 ms
tools/test-fault.sh          # expect 4/4 PASS
tools/verify-kaslr.sh -n 10  # expect 10/10 distinct (host flake ~2-4% at 50 boots
                             # matches baseline pre-impl; QEMU/host timing race,
                             # not introduced by these chunks — verified by stash)

# Spec matrix:
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
for s in scheduler scheduler_liveness scheduler_liveness_wfi scheduler_liveness_hwake \
         scheduler_buggy scheduler_buggy_steal scheduler_buggy_ipi \
         scheduler_buggy_starve scheduler_buggy_wfi scheduler_buggy_hwake; do
  echo "== $s =="
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config "$s.cfg" "$s.tla" 2>&1 | tail -3
done
# Expect: 4 correct cfgs clean (scheduler 15840 / liveness 23 / liveness_wfi 1008 /
# liveness_hwake 23); 6 buggy cfgs each produce their expected counterexample.
```

Boot output (key lines, in order):
- `kobj_mmio: reserved kernel range` × 4 (GIC dist + redist + PL011 + ECAM).
- `virtio: 32 MMIO slots probed`.
- `mmio-probe: PASS`.
- `irq-probe: PASS`.
- **`virtio-blk-probe: PASS — slot=31 intid=79 sig=THYLACINE-DISK-1`** (now exercises `wait_pid` path; R12-sched-impl regression guard).
- `tests: 227/227 PASS`.
- `joey: /joey pid=N exited cleanly (status=0)`.
- `Thylacine boot OK`.

---

## What landed, per chunk

### P4-Ic6-spec (`f050512` substantive, `25b4330` hash fixup)

**Closes**: the spec gap for R12-sched. Pre-existing `Liveness_Wfi` (P3-G) covered WFI-and-IPI fairness but not the "hardware IRQ eventually delivers" fairness on the **waker** side. Without that, a SLEEPING thread blocked on a hardware-IRQ Rendez has no spec-level guarantee of leaving SLEEPING — `LatencyBound` is vacuously satisfied because it's RUNNABLE-only.

**Mechanism**:
- New temporal property `HardwareWakeProgress`: `\A t : [](state[t] = "SLEEPING" /\ t \notin waiters => <>(state[t] # "SLEEPING"))`. The exclusion `t \notin waiters` matches `Wake(t)`'s precondition: cond-waiters are woken by `WakeAll`, not `Wake`, so they fall outside this property's scope.
- New `Liveness_Hwake = Liveness_Wfi /\ \A t : SF_vars(Wake(t))`. SF on Wake encodes "hardware IRQs are external events that always eventually deliver" — independent of any kernel scheduling decision.
- New `Spec_Live_Hwake == Init /\ [][Next]_vars /\ Liveness_Hwake`.
- New cfgs: `scheduler_liveness_hwake.cfg` (2T × 1C; Spec_Live_Hwake; Invariants + LatencyBound + HardwareWakeProgress all hold at 23 distinct states) + `scheduler_buggy_hwake.cfg` (same scope; Spec_Live without SF on Wake; HardwareWakeProgress counterexample at depth 4).

**`Wake(t)`** semantics were already there from P2-Bb — the spec change is purely a *fairness refinement*. Wake(t)'s existing transition ("SLEEPING-non-cond-waiter → RUNNABLE + enqueue") already abstracted `kobj_irq_dispatch`'s wakeup() path at the impl level; SF on Wake (the new fairness clause) lifts that to "always eventually fires."

**Spec posture**: spec-only chunk. No kernel changes; 227/227 tests unchanged.

**Surprise found**: while exercising the new cfgs, surfaced that `scheduler_liveness_wfi.cfg` (P3-G, landed `7a61651`) had been violating LatencyBound since its landing — verified by TLC replay at the P3-G commit. Tracked separately as **R12-wfi-cfg deferred audit**.

### P4-Ic6-cfg (`849c053` substantive, `719dedd` hash fixup)

**Closes**: R12-wfi-cfg. The wfi-cfg breakage turned out to be **two distinct issues**, not the "cfg parameters too tight" originally hypothesized.

**Issue (1) — spec-modeling gap**: `Wake(t)` and `WakeAll` could enqueue work into a wfi'd CPU's runq with no escape mechanism. The impl invariant is that an IRQ which places work into a wfi'd CPU's runq either (a) was delivered to that CPU directly — hardware lifts WFI when an IRQ becomes pending — or (b) was delivered to a peer CPU which then IPIs the target. Both produce identical terminal post-state: a RUNNABLE thread in `runq[target]` with `~wfi[target]`. Pre-fix, the spec admitted a trace where Wake enqueued into a wfi'd runq with no path back to RUNNING — no real impl exhibits this.

**Fix A**: `Wake(t)` atomically clears `wfi[cpu]` of its CHOOSE'd target; `WakeAll` atomically clears `wfi[cpu0]` conditional on `waiters # {}`. Collapses both impl paths (hardware-direct + IPI-mediated) into a single atomic spec step. Sound abstraction for liveness — TLC doesn't need to interleave the intermediate IPI steps to verify the terminal property.

**Issue (2) — CHOOSE-determinism artifact at 2T × 2C**: even with Fix A, TLC found a different lasso unrelated to WFI. t1 alternates RUNNING/SLEEPING via Block/Wake; t2 gets Steal'd between c1 and c2 indefinitely, never picked by Resume (which always sees t1 in its target runq). Same artifact `scheduler_liveness.cfg` works around by using 2T × 1C (per its own comment, which defers per-thread `Resume(cpu, t)` fairness refinement to Phase 5+ alongside EEVDF weight math).

**Fix**: reparameterize `scheduler_liveness_wfi.cfg` 2T × 2C → 1T × 2C. F77/F78 negative-case still verified by `scheduler_buggy_wfi.cfg` (still 2T × 2C; drops WFI fairness; produces stuttering counterexample).

**State count effects**:
- `scheduler.cfg`: 25416 → 15840 distinct (Wake/WakeAll constrained — fewer reachable states).
- `scheduler_buggy_wfi.cfg`: 5760 → 3888.
- `scheduler_liveness_wfi.cfg`: 5760 (broken) → 1008 (clean).

**Surprise found**: the wfi-cfg breakage was not parameter-related — it was a real spec gap. The original "cfg parameters too tight" hypothesis was wrong. Surfacing this required actually running TLC and reading the counterexample.

### P4-Ic6-impl (`3c1951d` substantive, `c87779e` hash fixup)

**Closes**: R12-sched (kernel side) + R12-sched-impl (architectural choice). Trip hazards #181 + #185 CLOSED.

**Mechanism** — Direction B per the architectural discussion:
- New `g_bootcpu_idle`: a dedicated Thread allocated in `boot_main` via `thread_create(kproc(), bootcpu_idle_main)` with `band = SCHED_BAND_IDLE`. Registered via new `sched_set_bootcpu_idle()` (set-once with magic + band validation).
- New `bootcpu_idle_main()` in `kernel/sched.c`: runs the WFI idle loop body symmetric to `per_cpu_main`'s tail on secondaries (IRQ-mask + idle_in_wfi flag + sched + wfi + clear-flag, the R7 F128 discipline).
- `sched()`'s deadlock path: replaces `extinction("sched: deadlock — current is blocking, no runnable peer system-wide")` with: when `prev->state != THREAD_RUNNING` AND pick_next + try_steal both NULL AND `smp_cpu_idx_self() == 0` AND `g_bootcpu_idle != NULL`, set `next = g_bootcpu_idle`; fall through to the normal switch. Secondaries keep the defensive extinction (their `per_cpu_main` idle is in BAND_IDLE's tree after the first yield, so pick_next finds it; the path is unreachable in practice).
- `test_virtio_blk_probe.c`: yield-poll loop replaced with direct `wait_pid`. The test now exercises the new code path; if anything regresses, this test fires.

**Why off-tree?** Critical to SP_EL1 correctness. `preempt_check_irq` runs at SpSel=1 (hardware-set on exception entry). If bootcpu_idle were `ready()`'d into BAND_IDLE's tree, preempt's `pick_next` would switch to it via `cpu_switch_context`, where `mov sp, x9` writes to the active SP — at SpSel=1 that's SP_EL1, the per-CPU exception stack pointer. SP_EL1 would get bootcpu_idle's kstack address, clobbering the exception stack pointer. A subsequent exception entry would push the new frame onto bootcpu_idle's kstack instead of the exception stack — broken stack discipline.

By keeping bootcpu_idle off-tree and using it only via the deadlock path, `cpu_switch_context` fires only from VOLUNTARY sched() callers (sleep, exits) at SpSel=0. At SpSel=0, `mov sp, x9` writes to SP_EL0 (the thread kstack pointer); SP_EL1 stays as the exception stack.

**Empirically validated**: an earlier draft of this chunk that DID `ready()` bootcpu_idle into BAND_IDLE caused intermittent failures of `smp.exception_stack_smoke` (which asserts `SpSel == 0` at kernel test entry). 3/10 to 1/10 KASLR boots failed in that draft. Excluding bootcpu_idle from the run tree eliminates the preempt-time context switch and the resulting SP_EL1 corruption. Post-fix: 30/30 KASLR distinct; 2-4% host flake on 50-boot stress matches baseline pre-impl (verified by stashing changes and re-running on the parent commit).

**Why Direction B (not Direction A)?** Direction A would have refactored the on_cpu protocol — new `THREAD_HALTED` sub-state; `wakeup()` skips on_cpu spin for HALTED threads; care needed for cross-CPU steal (peer's wakeup transitioning prev to RUNNABLE → some CPU's try_steal pulling prev → restore from stale ctx). Direction B reuses the existing P2-Cf wakeup discipline unchanged: the deadlock-path switch is a normal `cpu_switch_context` at SpSel=0; prev's `on_cpu` is cleared via the standard `prev_to_clear_on_cpu` handoff in the resume path. No new sub-states, no new race windows, no new spec extensions needed.

**Plan 9 heritage alignment**: bootcpu_idle is the Thylacine analog of Plan 9's per-CPU `runproc()` idle path. Plan 9's `port/proc.c::runproc()` doesn't extinct on empty runq — it calls `idlehands()` (architecture-specific WFI/HLT) and loops back. There is no "deadlock — current is blocking, no runnable peer" in Plan 9 because idle is the natural empty-runq state, not an error. Direction B retrofits this insight into the cpu_switch_context model: the boot CPU's "no work" state is now bootcpu_idle's WFI loop, not extinction. Mirrors Linux's per-CPU swapper/idle pattern.

**Surprise found**: the on-tree design (cleanest at first glance) clobbers SP_EL1 under preempt. Off-tree, deadlock-path-only restricts the cpu_switch_context to SpSel=0 contexts and sidesteps the issue. This is the kind of subtlety the spec doesn't capture (the spec abstracts over SpSel entirely); only empirical testing exposed it.

**Audit posture**: audit-bearing per CLAUDE.md trigger surface (Scheduler). Self-audit clean across lock-ordering / lifetime / error-path / state-machine / idempotency / boundary categories. Formal R12-sched-impl audit pass deferred-or-on-finding.

**LOC**: ~143 net additions across 5 files (`kernel/include/thylacine/sched.h` decls, `kernel/sched.c` g_bootcpu_idle + bootcpu_idle_main + sched_set_bootcpu_idle + deadlock path, `kernel/main.c` allocation + registration, `kernel/test/test_virtio_blk_probe.c` wait_pid, `kernel/test/test_sched.c` idle_in_wfi_observability comment refresh).

---

## Stratum readiness signal (external)

Stratum has now reached **complete 9P2000.L + POSIX surface**. This was the external blocker for local Phase 5 (= ROADMAP §7, "9P client + Stratum integration"). The integration target is now ready and waiting.

Implication for sequencing: local Phase 4's remaining chunks (P4-Ic6 multi-sector + 1 GiB loop, P4-Id driver-as-9P-server, P4-J/K/L virtio-net/input/gpu, P4-M supervision, P4-N BURROW finalize, P4-Z closing audit) keep their current priority — Phase 4 still needs to close cleanly before Phase 5 entry. But once Phase 4 exits, Phase 5 can start immediately; no external coordination delay.

The most useful next driver in Phase 4 (after P4-Ic6 multi-sector) is virtio-net, since it enables 9P-over-TCP — Stratum's transport for Phase 5 mount.

---

## State summary

- **Working tree**: clean.
- **Tip**: `c87779e` (62 commits ahead of `origin/main`).
- **Tests**: 227/227 PASS × default (~433 ms) + UBSan (~440 ms); 4/4 fault; 30/30 KASLR distinct.
- **Specs**: 4 written + 16 cfg variants. `scheduler.tla` 15840 / `scheduler_liveness.tla` 23 / `scheduler_liveness_wfi.tla` 1008 / `scheduler_liveness_hwake.tla` 23 — all clean. 6 buggy cfgs all produce expected counterexamples.
- **Open audit findings**: 0 unfixed P0/P1/P2. **Deferred**: F108/F109/F110 (R6-A); F113/F115/F116/F119 (R6-B); F130/F132/F137 (R7); F140/F141 (R8); F149/F150 (R9). **R12-* deferred**: R12-pol (P4-Ic5b1a virtio-mmio policy), R12-FP (P4-Ic5-FP), R12-DMA (P4-Ic5b1b), R12-gic-edge (P4-Ic5b2 GIC ICFGR). **CLOSED this window**: R12-wfi-cfg + R12-sched + R12-sched-impl.

---

## What's next

Phase 4 remaining (in dependency order):

1. **P4-Ic6 (multi-sector driver + 1 GiB read/write loop)** — Extends `virtio-blk-probe` to multi-sector reads + writes + bit-exact verify. Closes ROADMAP §6.2 exit criterion "Userspace virtio-blk: read 1 GiB / write 1 GiB / verify bit-exact." Non-audit-bearing. Now uses `wait_pid` cleanly (no more yield-poll). ~200 LOC Rust + ~50 LOC kernel test.
2. **P4-J (virtio-net userspace driver)** — Most useful next driver. Enables 9P-over-TCP for Stratum mount in Phase 5. Audit-bearing at the network-frame surface.
3. **P4-K (virtio-input)** — Keyboard/mouse via `/dev/cons` integration.
4. **P4-L (virtio-gpu)** — Framebuffer driver. Used at Phase 9 by Halcyon.
5. **P4-Id (driver-as-9P-server)** — Driver exposes `#blk` Dev with read/write ops. Closes ROADMAP §6.3.
6. **P4-M (driver supervision)** — Restart-on-crash via `/ctl/proc-events/exit`.
7. **P4-N (BURROW spec finalize)** — `specs/burrow.tla` reconciliation.
8. **P4-Ic7 + P4-Z (cumulative R12 + closing audit)** — Formal audit passes for R12-pol / R12-FP / R12-DMA / R12-gic-edge + Phase 4 closing audit. Verifies VISION §4.5 IRQ→userspace latency p99 < 5µs.

Phase 5 (post-Phase-4 exit) — unblocked by Stratum readiness; can start immediately when Phase 4 closes.

---

## Common pitfalls

- **`g_bootcpu_idle` must stay off-tree.** If a future change adds `ready(g_bootcpu_idle)` in `boot_main`, preempt's pick_next will grab it at SpSel=1 and clobber SP_EL1. The off-tree discipline is load-bearing for SP_EL1 correctness — see kernel/sched.c P4-Ic6-impl commentary block.
- **The deadlock extinction is still in `sched()` for secondaries.** If a future change introduces a userspace driver path that runs on a secondary AND can deadlock there (no peer-runnable on the secondary at SpSel=0), the extinction may fire on the secondary. The defensive extinction is intentional — secondaries' `per_cpu_main` idle is in BAND_IDLE after first yield so pick_next finds it; the deadlock-path-on-secondary is unreachable in practice. If a future driver class breaks this assumption, the secondary case needs its own dedicated idle pattern.
- **`wait_pid` now works on hardware-IRQ-blocked children.** The yield-poll workaround in `test_virtio_blk_probe.c` is retired. Any new userspace test that waits on a hardware-IRQ child should use `wait_pid` directly — the deadlock-path fallback handles the "parent + child both SLEEPING" case.
- **Stratum is ready.** External dependency resolved 2026-05-12 (handoff date). Phase 5 entry is no longer external-blocked.

---

## Memory pointers (refreshed at this handoff)

- `~/.claude/projects/-Users-northkillpd-projects-thylacine/memory/MEMORY.md` — top milestone bullet updated to P4-Ic6-impl.
- `project_active.md` — tip references + posture refreshed.
- `project_next_session.md` — read order updated; verify-on-pickup chain updated; the "Architectural decision needed for P4-Ic6-impl" section marked RESOLVED with Direction B rationale + off-tree adaptation noted.
- `audit_r{9,10,11}_closed_list.md` — unchanged (no new audit round this window; only deferred-audit closures).

## Notes for the next session

- The R12-sched closure trilogy completes this window. The three chunks form a coherent unit: spec captures intent (Ic6-spec), spec is refined for soundness (Ic6-cfg), kernel implements the spec (Ic6-impl). Future R12-* closures (R12-pol / R12-FP / R12-DMA / R12-gic-edge) are independent audit passes — each can land standalone.
- The Plan 9 heritage check (the user's "How did Plan 9 resolve this?" question) shaped the design choice. Direction B = Plan 9's per-CPU `runproc()` idle model retrofitted onto the cpu_switch_context machinery. Keep this lens for future scheduler decisions.
- Boot time is ~433 ms (default) / ~440 ms (UBSan) — slightly higher than pre-Ic6 (~440 default) because `wait_pid` actually waits (vs yield-poll burning the parent thread). Within VISION §4 < 500 ms budget; not a regression.
