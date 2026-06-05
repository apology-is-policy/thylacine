# SMP-REVIEW-HANDOFF — deep, systematic, clean-room review of the SMP layer

**Status: BINDING directive for the next session. Branch: `deep-smp-review` (off `main` @ `f30043f`). v1.0 SOUNDNESS BLOCKER — outranks A-5c / Utopia / everything else.**

The user's instruction, verbatim intent: *do an **extremely deep and pedantic SMP review** -- extremely deep, systematic, complete, rigorous -- and **potentially redesign the SMP system from scratch if it is found to be a patchwork**. Also **consider HMP** (this dev Mac has efficiency + performance cores; real ARM64 targets are mostly heterogeneous) and **assess the magnitude of a proper implementation**.*

This is NOT "land another point fix." The SMP layer has revealed itself to be a **stack of latent context-corruption races** (the #788/#789/#806/#807/#808/#809/#810/#811 saga, and now #860 + its sibling). Each prior fix was a point patch. The next session must decide, with evidence, whether the SMP scheduler + thread-lifecycle is **fundamentally sound (and just needs the residual races closed)** or **a patchwork that should be redesigned from first principles** (with a TLA+ model — see "Re-enable spec-first" below).

---

## 0. Branch state (what is on `deep-smp-review`, what is NOT)

The branch carries one WIP commit on top of `main`@`f30043f`. It contains TWO logically-separable bodies of work, intertwined in `kernel/sched.c`:

- **#857 (the `cons.*` smp8 "flake") — DONE + VERIFIED. Should eventually land on main.** Root cause: `sched_runnable_count()` (a diagnostic) counted secondary-CPU **idle threads** as runnable work, tripping the `==0` quiescence assertions under host load. Benign (kernel sound; a miscount). Fix: the count excludes `SCHED_BAND_IDLE`; + a deterministic regression test (`scheduler.runnable_count_excludes_idle`); + `sched_dump_runnable()` (a permanent failure-path diagnostic, wired into `test_fail`); + 9 test-helper-thread-leak hygiene fixes (`test_pipe_blocking.c`, `test_poll.c`). Verified: default smp4 710/710 + login E2E, UBSan-smp4 30/30, smp8 (pre-#860-fix) 20/20. **This part is good; the deep review should preserve it (or fold its intent into the redesign).**

- **#860 (a P0 SMP context-corruption race) — ROOT-CAUSED; the fix-in-tree REGRESSES smp8. DO NOT MERGE AS-IS.** See section 2. The branch holds the **option-A** fix (which regresses smp8 10/10). **Option B (section 2.3) is the surgical fix, NOT yet applied.**

`main` is untouched (still `f30043f`, the #828 close, **awaiting the user's push** independently of this branch).

---

## 1. The headline finding: the SMP layer is a masking-bug stack

The single-boot UBSan/smp CI gates have been **masking flaky-rate soundness bugs for a long time.** A ~10-43% intermittent crash passes a one-boot gate ~60-90% of the time, so every historical "UBSan GREEN / smp8 GREEN" was *true-of-that-boot, false-of-the-kernel*. Concretely, this session:

1. A benign #857 diagnostic addition (`sched_dump_runnable`) shifted `.text` layout and re-tuned a latent race into the open under UBSan-smp4 (base ~0% -> #857-build ~43%). That race was **#860** (g_bootcpu_idle steal/double-run; section 2).
2. Fixing #860 (option A) shifted `g_bootcpu_idle`'s dispatch from `pick_next` to the rarely-exercised deadlock path, which **unmasked ANOTHER race** that crashes smp8 10/10 (`sched: invalid prev state` + stack-canary + boot-stack wild-PC -- same #788 family).

**This is the playbook's masking-bug-stack (DEBUGGING-PLAYBOOK §6.1): touch one scheduler thing, expose the next.** The number of residuals is unknown. That is why a *systematic* review (not point fixes) is required.

### Process fix (do this regardless): harden the gates to MULTI-BOOT

The single-boot UBSan + smp8 gates are the reason this hid. **`tools/test.sh` (and any CI gate) must boot the sanitizer + smp8 matrices N>=10 times and fail if ANY boot crashes.** A flaky-rate soundness bug must not be able to pass by luck. This alone would have caught #860 and the sibling at their introduction.

---

## 2. #860 in full (the worked example the review starts from)

### 2.1 Root cause (CONFIRMED four ways: Opus prosecutor F1 + self-audit + `-smp` discriminator + a deterministic tripwire)

`g_bootcpu_idle` is cpu 0's idle, dispatched by `sched()`'s deadlock-path fallback. Unlike the SECONDARY idles (`thread_init_per_cpu_idle`, `kstack_base==NULL`, correctly unstealable), it is `thread_create`'d in `boot_main` with a **real kstack** (cpu 0's boot stack belongs to `kthread`, which blocks forever in `joey_run`/`wait_pid`). `try_steal`'s ONLY gate is `kstack_base != NULL` -> g_bootcpu_idle is the lone idle-band thread that's stealable IF in a tree. And the yield path **inserted it into `run_tree[BAND_IDLE]`** on its first yield-to-work (breaking the "off-tree" the comments claimed). Then a secondary `try_steal`s it AND cpu 0's deadlock path re-picks it (no `on_cpu` guard) -> **two CPUs run one thread on one kstack/ctx** -> cross-write SP/LR/x29 -> wild-PC (0x340) / smashed-stack / canary crash, "right after stratumd serves" (stratumd's pthreads make cpu 0 cycle idle<->work while secondaries steal).

Discriminator: smp1 0/12, smp2 0/12, smp4 ~33% (needs >=2 secondaries). Tripwire (extinction if g_bootcpu_idle enters a tree) fired **4/4 (100%) pre-fix** (the insertion is the deterministic precondition; the double-run is the ~33% timing window on top).

### 2.2 Option A (ON THE BRANCH) — wrong shape, regresses smp8

Keep g_bootcpu_idle OFF-tree (yield never inserts it) + try_steal skips it + a deadlock-path `on_cpu` guard + the insert_sorted tripwire. **Verified to CLOSE #860 at smp4** (UBSan 30/30 clean, tripwire 0). **But it REGRESSES smp8 (was 20/20, now 0/10)**: off-tree means g_bootcpu_idle is now dispatched constantly via the **deadlock-path fallback** (previously rare), which is itself racy at 8 CPUs (or unmasks a sibling). My #860 guards did NOT fire -> the off-tree mechanic works; the **dispatch-path shift** is what bit.

### 2.3 Option B (NOT applied) — the surgical fix the review should evaluate first

Leave g_bootcpu_idle **IN the tree exactly as before** (no dispatch change at all -> can't regress smp8), and ONLY make it **unstealable**: the one-line `try_steal` skip (`cand != g_bootcpu_idle`). A secondary then can never steal it -> never double-runs it -> #860 closed, with ZERO scheduling-dynamics change. (`#857`'s count fix already excludes BAND_IDLE, so g_bootcpu_idle being in the tree doesn't re-break the count.) **But verify it does NOT also regress smp8** -- if it does, the smp8 crash is a SIBLING race independent of the g_bootcpu_idle dispatch, and the masking stack is deeper. Deterministic confirmation for option B: a counter on the `try_steal`-skip (proves g_bootcpu_idle IS a steal candidate that's now blocked) + the multi-boot matrix.

**The review should treat option B as a hypothesis to prove/disprove, not a given.** If smp8 is still red with option B, do NOT keep patching -- escalate to the systematic review / redesign.

---

## 3. The deep-review scope (be exhaustive; this is the whole SMP surface)

Read EVERY file FULLY. For each, enumerate every cross-CPU interaction and every invariant, and prove or break it. Suspect that the "off-tree exception", the "deadlock path", the on_cpu protocol, and the steal/migration dance are an accreted patchwork.

- **Scheduler core** `kernel/sched.c`: `sched()` (the whole state machine: yield/block/exit dispatch, the `prev==next` branch, the `!next` keep-running path, the deadlock path, the switch tail + `pending_release_lock` cross-CPU migration release, `set_current_thread`), `ready()`, `pick_next`, `insert_sorted`/`unlink`/`in_run_tree`, `try_steal` (the `kstack_base` gate + the claim-on_cpu-under-peer-lock), `sched_remove_if_runnable` (all-CPU walk), the `on_cpu` protocol (`prev_to_clear_on_cpu` / `sched_finish_task_switch` / `sched_arm_clear_on_cpu`), `g_bootcpu_idle` + `bootcpu_idle_main`, `sched_notify_idle_peer` + `g_sched_notify_enabled`, `sched_runnable_count`.
- **Thread lifecycle** `kernel/thread.c`: `thread_create`/`thread_create_user`, `thread_free` (the #788 on_cpu spin-gate -- is it on EVERY free/recycle path?), `thread_init_per_cpu_idle`, kstack alloc/free + the guard-page demote, the SLUB/buddy recycle of Thread + kstack (the #788 corruption substrate).
- **Context switch** `arch/arm64/context.S`: `cpu_switch_context` (exact save/restore ordering vs on_cpu publish), `thread_trampoline`, `thread_user_trampoline` (eret-to-EL0). Where EXACTLY is a thread's ctx valid vs reusable.
- **SMP bringup + transition** `kernel/smp.c`: `per_cpu_main` idle loop, `smp_enable_secondary_preemption`, `smp_resched_others`, secondary bring-up; `kernel/main.c` the production transition (`sched_set_notify_enabled(true)` + `smp_enable_secondary_preemption()` -- the window where this all goes live).
- **Multi-thread Proc** `kernel/proc.c`: `exits`, `thread_exit_self`, `wait_pid` (multi-thread reap: walk `p->threads`, on_cpu-spin each, thread_free each), `proc_group_terminate` (#809/#811 cascade), `proc_free`, reparent. `arch/arm64/exception.c` + `vectors.S`: `el0_return_die_check`, the IRQ-from-EL0 return tail.
- **Wait/wake** `kernel/sched.c` (`sleep`/`tsleep`), `kernel/torpor.c`, `kernel/rendez.*`, the per-Thread `wait_lock` / `rendez_blocked_on` (#811). I-9 across EVERY rendez.
- **SMP memory model** `mm/buddy.c`/`slub.c`/`magazines.c` (#807 per-CPU magazine), `arch/arm64/mmu.c` (#806/#808 direct-map demote BBM).
- **IPI/GIC** `arch/arm64/gic.c` (SGI ordering I-18), `kernel/smp.c` IPI logic.

Invariants (ARCH §28): I-8 (every runnable thread runs), I-9 (no lost wakeup), I-17 (latency bound), I-18 (IPI order), I-21 (EL1h / per-thread kstack exclusivity). The #860 race is an I-21 violation (one kstack, two CPUs).

### Method (rigorous, not vibes)
1. **Multi-boot everything.** Never trust a single boot. UBSan-smp4 + smp8 + smp1/smp2 discriminators, N>=20 each, under E-core skew (see HMP, section 4).
2. **Make races deterministic via their preconditions** (the #860 tripwire pattern / #806 honest-dump): a timing race can't be A/B'd by rebuild (it detunes), but its PRECONDITION often can be turned into a 100%-deterministic tripwire.
3. **`-smp` + `taskpolicy -b` discriminators** to prove "needs a secondary" / amplify the window.
4. **Adversarial prosecutors (Opus) per surface** + your own self-audit in parallel (the audit-in-flight discipline). The #860 prosecutor prompt is a good template.
5. **gdbstub** (`run-vm.sh --gdb` = `-s -S`; lldb `gdb-remote 1234` + `target modules load --slide <KASLR-from-banner>`) for hangs / honest fault state.

### Re-enable spec-first for THIS (recommend to the user)
Spec-to-code is suspended (CLAUDE.md), but the user flagged the natural re-enabling point as "an invariant-bearing feature that genuinely benefits from machine-checked exploration." **A from-scratch SMP scheduler / thread-lifecycle is exactly that.** `specs/scheduler.tla` + `specs/sched_ctxsw.tla` already exist. A redesign should be MODELED in TLA+ first (state = per-CPU run queues + per-thread state + on_cpu + the migration/steal actions; invariants = I-8/I-9/I-17/I-18/I-21 + "a thread runs on <=1 CPU at a time" + "a ctx/kstack is never written by two CPUs"), TLC-checked, THEN implemented. The #860 class (two CPUs, one ctx) is exactly what a model catches in minutes.

---

## 4. HMP (the user's point) — assessment + magnitude

**Two distinct angles; keep them separate.**

### 4a. Host HMP as a race AMPLIFIER (use it now, in the review)
The dev Mac (Apple Silicon) has P-cores + E-cores. macOS places QEMU's vCPU threads across them; an E-core drags a synchronized guest vCPU ~2.5x (this is the **#789 substrate**, already root-caused). That timing skew is precisely what widens the SMP race windows (the SLEEPING..on_cpu-cleared gap, the steal-vs-re-pick window). **The review should deliberately USE this:** `taskpolicy -b <qemu>` forces the throttled/E-core tier and is a strong race amplifier (it turned #788/#806 deterministic-ish). HMP-host is a *tool* here, not a target concern.

### 4b. HMP as a TARGET design axis (for the redesign)
Real ARM64 targets are mostly heterogeneous: Apple Silicon (P+E), virtually all modern phone/laptop SoCs (Arm big.LITTLE / DynamIQ). The Lazarus first board (RPi 400, 4x Cortex-A72) is the **homogeneous exception**. So a clean-room SMP scheduler SHOULD be *designed to accommodate* HMP even if v1.0 treats cores uniformly.

**What HMP-aware scheduling requires, and the magnitude:**
- **Topology from DTB** (Thylacine already does hardware-from-DTB, I-15): parse `cpu-map` (clusters), `capacity-dmips-mhz` per cpu -> classify cores by capacity (P vs E). *Small* (a DTB parse + a per-CPU capacity field). **This is the cheap, high-value first step and the design should bake it in.**
- **Capacity-aware placement** (bias latency-sensitive/foreground threads to high-capacity cores, background/idle to low): *moderate* -- it's a placement heuristic layered on the run-queue selection (`ready()` target-CPU choice + load-balance). A few hundred LOC if the abstractions (per-CPU capacity, core-type) exist.
- **Full EAS (Energy-Aware Scheduling)** -- per-task utilization tracking (PELT-style), an energy model, asymmetric load-balancing, DVFS/frequency coupling: *large*. This is a Linux-EAS-scale subsystem (thousands of LOC + years of tuning). **Clearly v1.x / post-v1.0.**

**Recommendation:** the redesign should **design the abstractions for HMP** (per-CPU `capacity`, core-type-tagged run queues, a placement hook) so capacity-aware placement can be added incrementally WITHOUT another redesign -- but **v1.0 should ship homogeneous-treatment + the topology plumbing**, not full EAS. The magnitude of *proper full* HMP/EAS is large (post-v1.0); the magnitude of *HMP-ready abstractions + basic capacity bias* is moderate and worth doing in the redesign. **Crucially: HMP is ORTHOGONAL to the correctness races.** Fix correctness first (the soundness blocker); HMP is a performance/efficiency axis that the redesign should not preclude.

---

## 5. Footguns / do-not (this box)

- **Single boots lie.** Multi-boot or it didn't happen.
- A **rebuild detunes a timing race** (layout shift). Never conclude "fixed" from one post-rebuild clean run; use the precondition-tripwire + the multi-boot matrix + correctness-by-construction.
- The **dev Mac sleeps mid-batch** -> long QEMU boots log as spurious TIMEOUT (not real hangs). Discount TIMEOUTs straddling a sleep.
- Build: `tools/build.sh kernel [--sanitize=undefined]`. Test default: `tools/test.sh`. smp8: `THYLACINE_TEST_CPUS=8 ... ; pkill -KILL -f qemu-system-aarch64`. Direct boot loop: `THYLACINE_BUILD_DIR=$PWD/build/kernel[-undefined] tools/run-vm.sh --no-share --cpus N < /dev/null`. Watch the UART log for `Thylacine boot OK` vs `^EXTINCTION`.
- `kernel/sched.c` + `kernel/thread.c` + `arch/arm64/context.S` are audit-trigger surfaces; any change is audit-bearing.
- Plain-ASCII commits; the user pushes (you commit); NEVER stage `.claude/`.

---

## 6. Concrete first moves for the next session

1. Read this doc + `memory/bug_860_bootcpu_idle_steal.md` + DEBUGGING-PLAYBOOK §6.1/§6.11/§6.12/§6.13/§6.14 + ARCH §8 (scheduler) + §28 (invariants).
2. Harden the gate to multi-boot (section 1) -- so the review's own verifications are honest.
3. Reproduce the smp8 0/10 on this branch (option-A), then apply option B (section 2.3) and multi-boot smp4-UBSan + smp8. This single experiment tells you whether the masking stack is 1-deep (option B closes it) or N-deep (a sibling remains).
4. If a sibling remains (or even if not), do the systematic per-surface review (section 3) with a TLA+ model (section "Re-enable spec-first"). Decide patchwork-vs-sound with evidence.
5. If patchwork: redesign the scheduler + thread-lifecycle from first principles, model-first, HMP-ready abstractions (section 4b), homogeneous v1.0.

The thylacine is real. So is rock-stable SMP. Build it right.
