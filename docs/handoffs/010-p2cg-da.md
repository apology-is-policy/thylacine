# Handoff 010 — P2-Cg + P2-Da landed (SMP spec closed; multi-process lifecycle live)

**Date**: 2026-05-05
**Tip**: `9335425` (P2-Da hash fixup)
**Phase**: Phase 2 in progress. P2-A through P2-Da landed. The SMP scheduler section's spec side is closed (`Steal` + `IPI_Send`/`IPI_Deliver` actions; `NoDoubleEnqueue` + `IPIOrdering` invariants pinned in TLC). Multi-process lifecycle (rfork(RFPROC) + exits + wait_pid) is live for single-thread Procs. **P2-Db (per-thread guard pages + 1000-iter cross-CPU stress) is the next sub-chunk.**

---

## Pickup pointer

**Read in this order:**

1. `CLAUDE.md` (root) — operational framework. Mandatory.
2. **This file** — canonical pickup at P2-Da close.
3. `docs/handoffs/009-p2ce-cf.md` — predecessor (P2-Ce/Cf and the call-out for P2-Cg).
4. `docs/handoffs/008-p2cc-cd-cdc.md` — P2-Cc..Cdc.
5. `docs/handoffs/007-p2bb-bc-ca-cb.md` — P2-Bb..Cb.
6. `docs/VISION.md` + `docs/ARCHITECTURE.md` + `docs/ROADMAP.md` — binding scripture.
7. `docs/phase2-status.md` — sub-chunk plan with all landed rows. P2-Da is row #1 in the landed-chunks table; P2-Cg is row #2.
8. `docs/REFERENCE.md` snapshot + `docs/reference/14-process-model.md` (rfork + exits + wait_pid sections) + `15-scheduler.md` (Spec cross-reference now lists P2-Cg actions/invariants).
9. `specs/scheduler.tla` + `specs/SPEC-TO-CODE.md` — formal spec. P2-Cg added Steal + IPI actions + invariants.
10. `memory/project_active.md` — quick state summary.

---

## What landed in this session (2 sub-chunks, 4 commits)

### P2-Cg — scheduler.tla SMP refinement (spec-only)

`8b8516a` substantive + `16311b3` hash fixup.

**Goal**: Lift the SMP discipline introduced at P2-Ce (work-stealing) and P2-Cf (on_cpu wait/wake race close) from impl level to spec level. Closes the spec-first deviation per CLAUDE.md (invariant-bearing implementations should land WITH spec refinement; P2-Ce/Cf added impl ahead of spec).

**Spec extensions** (`specs/scheduler.tla`, ~389 → ~620 LOC TLA+):

- **`Steal(stealer, victim)` action** — atomic cross-CPU thread transfer modeling `kernel/sched.c::try_steal`. Stealer with empty runq pulls one runnable thread from victim's runq. Atomic at the spec level because the impl's spin_trylock-bracketed window is observably indistinguishable from a single atomic step (no CPU can read victim's tree mid-unlink — peer's spin_trylock fails).
- **`IPI_Send(src, dst)` + `IPI_Deliver(src, dst)` actions** — per-(src, dst) FIFO queue with monotonic seq numbers. Maps to `arch/arm64/gic.c::gic_send_ipi` (write `ICC_SGI1R_EL1`) and `kernel/smp.c::ipi_resched_handler` (consume head of queue).
- **Three buggy variants**: `BuggyCheck`/`BuggySleep` (existing missed-wakeup), `BuggySteal` (forgotten unlink), `BuggyIPI_Deliver` (out-of-order pop). Each gated by an orthogonal flag.
- **Two new invariants**: `NoDoubleEnqueue` (thread in ≤1 runq) + `IPIOrdering` (head of queue = next-expected delivery seq, ARCH §28 I-18).

**Three buggy configs** — executable documentation of the bug each invariant defends:

| Config | Flag | Invariant | Counterexample |
|---|---|---|---|
| `scheduler.cfg`              | all FALSE         | (clean)           | 10188 distinct states / depth 17 |
| `scheduler_buggy.cfg`        | `BUGGY=TRUE`      | `NoMissedWakeup`  | depth 6 / 245 states |
| `scheduler_buggy_steal.cfg`  | `BUGGY_STEAL=TRUE`| `NoDoubleEnqueue` | depth 4 / 38 states  |
| `scheduler_buggy_ipi.cfg`    | `BUGGY_IPI_ORDER=TRUE` | `IPIOrdering` | depth 6 / 470 states |

Updated: `specs/SPEC-TO-CODE.md`, `docs/REFERENCE.md`, `docs/reference/15-scheduler.md`, `docs/phase2-status.md`. Phase tag P2-Cf → P2-Cg.

### P2-Da — rfork(RFPROC) + exits + wait_pid

`4465408` substantive + `9335425` hash fixup.

**Goal**: Multi-process lifecycle. Plan 9 rfork as the universal Proc/Thread creation primitive (ARCH §7.4); exits + wait complete the lifecycle (§7.9).

**struct Proc grew 24 → 80 bytes**:

```c
enum proc_state { INVALID = 0, ALIVE = 1, ZOMBIE = 2 };

struct Proc {
    u64               magic;
    int               pid;
    int               thread_count;
    enum proc_state   state;
    int               exit_status;
    struct Thread    *threads;
    struct Proc      *parent;
    struct Proc      *children;
    struct Proc      *sibling;
    const char       *exit_msg;
    struct Rendez     child_done;
};
```

`_Static_assert(sizeof(struct Proc) == 80)`. proc_init_fields sets state=ALIVE + initializes child_done. proc_free now requires state==ZOMBIE (lifecycle gate).

**API**:

- **`rfork(unsigned flags, void (*entry)(void *), void *arg)`** — only RFPROC supported at v1.0. Other RF* flags trigger an extinction with their assigned phase (RFNAMEG at P2-E, RFFDG at P2-F, RFMEM at Phase 5+, etc.). Allocates child Proc + initial Thread, links to parent's children list, ready()s the thread. Returns child PID on success, -1 on OOM with rollback. Kernel-internal entry signature; full userspace rfork-from-syscall split lands at P2-G with the ELF loader.

- **`exits(const char *msg)`** — `__attribute__((noreturn))`. Re-parents orphan children to kproc. Captures exit_msg by reference (caller-owned, typically string literal) and translates "ok" → status 0, anything else → status 1. Marks Proc ZOMBIE + Thread EXITING + wakes parent's child_done Rendez. Yields via sched(); never returns.

- **`wait_pid(int *status_out)`** — blocks on parent's child_done Rendez until any child enters ZOMBIE. Cond predicate "any child zombie OR no children" covers the edge case where children exit between scan and sleep. On zombie-found: unlinks from children list, copies exit_status, calls thread_free + proc_free, returns PID. Returns -1 if no children at all.

**thread_create_with_arg** + trampoline `mov x0, x20` — extends thread_create to pass an arg via x0. thread_trampoline does `mov x0, x20; blr x21` (was just `blr x21`); backward-compatible since x20 defaults to 0 via KP_ZERO.

**No new spec at P2-Da** — proc lifecycle's wait/wake is structurally covered by `scheduler.tla`'s `WaitOnCond`/`WakeAll` (NoMissedWakeup invariant). wait_pid's sleep + child_done's wakeup map directly to existing actions.

**3 new in-kernel tests**:

- `proc.rfork_basic_smoke` — rfork → child runs entry → exits("ok") → parent wait_pid → verifies PID + status 0.
- `proc.rfork_exits_status` — same shape with exits("err") → status != 0.
- `proc.rfork_stress_100` — 100 iterations of rfork → exits → wait_pid; verifies proc_total_created/destroyed + thread_total_created/destroyed all advance by exactly ITERS (no leak in proc_cache, thread_cache, or kstack pages).

Updated: `docs/REFERENCE.md`, `docs/reference/14-process-model.md`, `docs/phase2-status.md`. Phase tag P2-Cg → P2-Da.

---

## Current state at handoff

- **Tip**: `9335425`.
- **Phase**: Phase 2 in progress. P2-A → P2-Da landed. **P2-Db (per-thread guard pages + 1000-iter cross-CPU stress) is the next sub-chunk.**
- **Working tree**: clean (only `docs/estimate.md` + `loc.sh` untracked, pre-existing).
- **`tools/test.sh`**: PASS. **25/25 in-kernel tests** (3 new at P2-Da); ~196 ms boot (production), ~226 ms (UBSan).
- **`tools/test.sh --sanitize=undefined`**: PASS.
- **`tools/test-fault.sh`**: PASS (3/3).
- **`tools/verify-kaslr.sh -n 5`**: PASS (5/5 distinct).
- **Specs**: `scheduler.tla` TLC-clean at 10188 distinct states. 3 buggy configs each produce their counterexample at depths 4-6 in <1 s.
- **LOC**: ~7170 kernel/asm + ~620 TLA+ ≈ ~7790 LOC total.
- **Kernel ELF**: ~308 KB debug.
- **Open audit findings**: 0. R5 audit deferred to Phase 2 close.

## What's NEXT

### P2-Db: per-thread guard pages + 1000-iter cross-CPU stress

**Per-thread guard pages** close P2-A trip-hazard #1: kstack underflow detection. Currently kstack is `alloc_pages(THREAD_KSTACK_ORDER=2, KP_ZERO)` = 4 contiguous pages = 16 KiB. A stack-overflow hit detects via the SP_EL1 per-CPU exception stack (P2-Cc) — recursive faults are caught. But stack-underflow (a buggy kthread that pops too deep into garbage) just runs into adjacent allocations silently.

**Approach options**:
- **(A) Allocate 5 pages, mark bottom one PROT_NONE**: order=3 = 8 pages = 32 KiB which gives 16 KiB stack + 16 KiB guard. Wasteful but simple.
- **(B) Allocate 4 pages contiguous + separate guard**: alloc 4 pages for kstack via existing path; separately reserve a guard page below via VA range manipulation. Requires per-thread VA management — heavier.
- **(C) Use existing SP_EL1 detection only**: argue that with per-CPU exception stack at P2-Cc, underflow recovers gracefully even without an explicit guard page. Not a full solution though — silent memory corruption is the failure mode, not faulted recursion.

Recommend (A): 5-page allocation, bottom page's PTE marked no-access in TTBR1. Underflow → page fault → fault handler → extinction("kstack underflow"). ~80 LOC mm + thread + a deliberate-fault test variant.

Add a deliberate-fault variant `kstack_underflow` to `tools/test-fault.sh` (parallel to `canary_smash`, `wxe_violation`, `bti_fault`).

**1000-iter cross-CPU stress**: extends `proc.rfork_stress_100` to 1000 iterations AND verifies cross-CPU placement. Possible mechanism: have the child entry function record `smp_cpu_idx_self()` into a shared array; after the stress, verify multiple CPU indexes appear. Confirms work-stealing carries new Procs across CPUs cleanly under sustained load.

Best-case 100-150k tokens; iterative case 200k+.

### After P2-Db

- **P2-E**: namespace + bind/mount. New spec `namespace.tla` (cycle-freedom I-3, isolation I-1).
- **P2-F**: handle table + VMO. New spec `handles.tla` (rights monotonicity I-2/I-6, transfer-via-9P I-4, hardware-handle non-transferability I-5, VMO refcount I-7).
- **P2-G**: ELF loader + minimal init.
- **P2-H**: closing audit (R5).

R5 audit at Phase 2 close covers cumulative P2-A..P2-H surface.

---

## Trip-hazards summary (cumulative as of P2-Da)

83 trip-hazards documented in `docs/phase2-status.md`. Highlights from this session:

**P2-Cg (#0 — spec-only, no compiler hazards)**:
- `MaxIPIs` constant must be set to a finite value in every .cfg; bounds state space.
- `BUGGY`, `BUGGY_STEAL`, `BUGGY_IPI_ORDER` are orthogonal flags — buggy actions only fire when their flag is TRUE.
- `CpuPair == {<<s,d>> \in CPUs \X CPUs : s # d}` excludes self-IPIs by design.
- IPI handler effects (e.g., setting need_resched) are NOT modeled — only the ordering discipline.
- vd_t rebasing in Steal is NOT modeled — deferred to LatencyBound refinement (Phase 2 close).

**P2-Da (#78-83)**:
78. rfork only supports RFPROC (other flags extinct loudly).
79. rfork's kernel-internal entry signature `void (*)(void *)` with arg in x0 via trampoline `mov x0, x20`.
80. Single-thread Procs only (multi-thread coordination at Phase 5+).
81. proc_free requires state==ZOMBIE (lifecycle gate).
82. exit_msg captured by reference (caller-owned lifetime).
83. Orphan re-parenting targets kproc (PID 0) at v1.0; PID 1 (init) at Phase 5+.

P2-Cf (#71-77) + earlier sub-chunks (#1-70): see `docs/phase2-status.md` for the full list.

---

## Verify on session pickup

```bash
git log --oneline -5
# Expect: 9335425 P2-Da: hash fixup / 4465408 P2-Da: rfork(RFPROC) + exits + wait /
#         16311b3 P2-Cg: hash fixup / 8b8516a P2-Cg: scheduler.tla SMP refinement /
#         c1d839e P2-Cf: handoff 009 ...

git status
# Expect: clean (only docs/estimate.md + loc.sh untracked)

tools/build.sh kernel
tools/test.sh
# Expect: 25/25 PASS, ~196 ms boot, full SMP banner with smp:/exception:/ipi: lines
#         + 3 new tests: proc.rfork_basic_smoke, proc.rfork_exits_status,
#                        proc.rfork_stress_100

tools/test.sh --sanitize=undefined
# Expect: 25/25 PASS, ~226 ms boot

tools/test-fault.sh
# Expect: 3/3 PASS

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct

export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler.cfg scheduler.tla 2>&1 | tail -3
# Expect: 79201 states generated, 10188 distinct states found
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler_buggy_steal.cfg scheduler.tla 2>&1 | tail -3
# Expect: counterexample at depth 4 (NoDoubleEnqueue violated)
java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock -config scheduler_buggy_ipi.cfg scheduler.tla 2>&1 | tail -3
# Expect: counterexample at depth 6 (IPIOrdering violated)
```

If any fails on a clean checkout, something regressed since the handoff — investigate before proceeding.

---

## Open follow-ups

- **U-30 (kstack zeroing on free)**: deferred at P2-A R4 F48. KP_ZERO is the documented escape hatch.
- **R5 audit**: covers P2-B + P2-C + P2-D cumulative. Run at Phase 2 close.
- **LatencyBound liveness spec**: deferred to Phase 2 close. Requires weak fairness + explicit Slice variable.
- **Full EEVDF math**: deferred — meaningful only with weight ≠ 1 (Phase 5+).
- **Per-thread guard page**: P2-Db (next sub-chunk) — closes P2-A trip-hazard #1.
- **Multi-thread Procs (RFMEM-style)**: Phase 5+ with the syscall surface. Requires IPI-based termination of sibling threads.
- **Userspace rfork-from-syscall split**: P2-G with ELF loader. Parent gets PID, child gets 0 via register-set tweak in syscall-return path.
- **try_steal rotate-start**: scan peers starting at a rotating CPU index per call to spread load. v1.0 P2-Ce uses fixed CPU order; refinement post-v1.0.
- **IPI_TLB_FLUSH/HALT/GENERIC**: deferred until use-cases arrive (TLB shootdown for namespace rebind in Phase 5+).
- **Per-IPI-type ordering refinement**: when multiple IPI types coexist (P5+), ordering refines to per-(src, type, dst) FIFO. Today's per-(src, dst) version is sound while all IPIs are equal-priority + same-type.
- **Pi 5 / multi-cluster Aff{1,2,3}**: Phase 7 hardening pass — current ICC_SGI1R_EL1 encoding pinned for QEMU virt's flat-Aff0 cluster.
- **Orphan reaping at Phase 5+**: kproc adopting orphans is fine at v1.0 (no test scenario stresses it); Phase 5+ retargets to PID 1 (init).

---

## Naming holds (cumulative)

- **Plan 9 idiom names KEPT**: `sleep`, `wakeup`, `Rendez`. (P2-Bb)
- **Plan 9 process names KEPT**: `rfork`, `exits`, `wait_pid` (kernel-internal name; full POSIX-translatable `wait` lives in Phase 5+ syscall layer). RF* flag constants pinned by Plan 9 / 9front idiom. (P2-Da)
- **Linux/Plan 9 standard preempt names KEPT**: `sched_tick`, `need_resched`, `preempt_check_irq`. (P2-Bc)
- **Standard SMP names KEPT**: `smp_init`, `smp_cpu_count`, `secondary_entry`, `g_cpu_online`, `g_cpu_alive`, `smp_cpu_idx_self`. (P2-Ca/Cd)
- **PSCI names pinned by Arm DEN 0022D**: PSCI_CPU_ON_64, PSCI_SUCCESS, etc. (P2-Ca)
- **GIC names pinned by ARM IHI 0069**: GICR_*, GICD_*, ICC_SGI1R_EL1, IPI_RESCHED. (P2-Cdc)
- **Standard ARM names KEPT**: `g_exception_stacks`, `EXCEPTION_STACK_SIZE`. (P2-Cc)
- **Linux finish_task_switch idiom KEPT**: `sched_finish_task_switch`, `pending_release_lock`, `prev_to_clear_on_cpu`, `on_cpu`. (P2-Ce/Cf)
- **Spec-action names follow Linux/POSIX vocabulary**: `Steal`, `IPI_Send`, `IPI_Deliver`, `BuggyIPI_Deliver`, `NoDoubleEnqueue`, `IPIOrdering`. (P2-Cg)

Held for explicit signoff:
- `_hang` → `_torpor` (marsupial deep-sleep state).
- Audit-prosecutor agent → stays "prosecutor" for Stratum continuity.

---

## Closing notes

This session closed two important Phase 2 chunks:

- **P2-Cg** lifted the SMP scheduler discipline from impl level to spec level. The `Steal` action + per-(src,dst) IPI queues + `NoDoubleEnqueue` + `IPIOrdering` invariants pin the work-stealing + IPI ordering correctness in TLC-checkable form. Three buggy configs each produce their specific counterexample. ARCH §28 I-18 (FIFO IPI ordering) is now formally specified. **Closes the spec-first deviation introduced at P2-Ce/Cf.**

- **P2-Da** made multi-process execution real. rfork(RFPROC) + exits + wait_pid implement the Plan 9 lifecycle (single-thread Procs at v1.0). 100-iteration stress verifies no leak. struct Proc grew 24→80 bytes; thread_create gained an arg-passing variant; the trampoline learned to load x0 from x20.

Posture is excellent: 25/25 tests + UBSan-clean + fault matrix + KASLR distinct all PASS; spec TLC-clean; 3 buggy configs all working; 0 audit findings open. Next session has a clean foundation.

P2-Db (per-thread guard pages + cross-CPU stress) closes P2-A trip-hazard #1 and verifies the multi-process model under sustained cross-CPU load. Then P2-E (namespace), P2-F (handles + VMO), P2-G (ELF + init), P2-H (closing audit).

The thylacine runs again — and now its children run too.
