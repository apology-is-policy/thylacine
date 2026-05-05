# Handoff 006 — P2-A: process-model bootstrap

**Date**: 2026-05-05
**Tip commit**: `df78693` (P2-A: process-model bootstrap)
**Author**: Claude Opus 4.7 (1M context)
**Audience**: the next session (or human collaborator) picking this up.
**Predecessor**: `005-phase1-close.md` (Phase 1 CLOSED at `ceecb26`).

**Phase 2 OPEN.** P2-A landed: the kernel can now describe processes (`struct Proc`), threads (`struct Thread`), and saved register contexts (`struct Context`); allocate them via SLUB; and switch CPU control between two threads via `cpu_switch_context` + `thread_switch` — all without a scheduler. P2-B (EEVDF + wait/wake) layers the dispatcher on top of P2-A's primitives.

Audit round 4 spawned in the same session as the P2-A commit; its closed-list will be appended at `memory/audit_r4_closed_list.md` once findings are dispositioned.

---

## TL;DR

- **Phase 1 momentum** carried into Phase 2 entry. `ceecb26` (Phase 1 CLOSED) → `df78693` (P2-A: process-model bootstrap).
- **Posture**: 11/11 in-kernel tests PASS each boot (the 9 from Phase 1 + `context.create_destroy` + `context.round_trip`). Boot time ~42 ms (well under 500 ms budget). UBSan-clean. Deliberate-fault matrix 3/3 PASS. KASLR variance 5/5 distinct. `scheduler.tla` TLC-clean at sketch level (99 distinct states explored).
- **ELF**: ~227 KB debug (production); ~248 KB (UBSan); 38 KB flat binary.
- **Specs**: 1 written. The `scheduler.tla` sketch establishes the framing — thread state machine + per-CPU dispatch with four state-consistency invariants. P2-B refines (EEVDF deadline math + wait/wake atomicity + IPI ordering); P2-C refines (work-stealing fairness); Phase 2 close adds `namespace.tla` + `handles.tla`.
- **Next chunk: P2-B** (EEVDF scheduler + wait/wake). Estimated 800-1200 LOC + ~150-300 LOC TLA+.

---

## What landed in P2-A

### `df78693` — P2-A: process-model bootstrap

**Headers** (`kernel/include/thylacine/`):
- `proc.h`: `struct Proc` — pid + threads list + thread_count. Grows by appending fields per sub-chunk.
- `thread.h`: `struct Thread` — tid + state + proc + ctx + kstack + list links. `enum thread_state {INVALID, RUNNING, RUNNABLE, SLEEPING, EXITING}`. Inline `current_thread()` / `set_current_thread()` via `mrs/msr tpidr_el1` (per-CPU OS-reserved register).
- `context.h`: `struct Context` — 14 × u64 = 112 bytes; layout = x19..x28 + fp + lr + sp + tpidr_el0. Field offsets pinned by `_Static_assert`s; `arch/arm64/context.S` hardcodes the same offsets — drift is a build break.

**Asm** (`arch/arm64/context.S`):
- `cpu_switch_context(prev, next)`: `bti c` (BLR landing); `stp` save callee-saved + fp + lr; `mov/str` SP; `mrs/msr` TPIDR_EL0; `ldp` load; `ret`. No `paciasp`/`autiasp` — x30 in the function is the unsigned bl-return-address (the C caller's pac-ret discipline pushed its OWN signed lr to its stack at its prologue).
- `thread_trampoline`: `bti c` (defensive — passes under any BTYPE); `blr x21` (entry function — entry's own `bti c` matches BTYPE=10); halt on WFE if entry returns (Phase 2 close adds real `thread_exit` + reap).

**Impl** (`kernel/proc.c`, `kernel/thread.c`):
- `proc_init` / `thread_init` allocate kproc (PID 0) + kthread (TID 0) via SLUB caches; thread_init parks kthread in TPIDR_EL1 as the boot CPU's current.
- `thread_create(proc, entry)`: SLUB-alloc Thread; `alloc_pages(2, KP_ZERO)` for 16 KiB kstack; lay out ctx so the first switch-into lands at `thread_trampoline` which then `blr`s entry. Cleans up Thread on stack-alloc OOM.
- `thread_free(t)`: extincts on null/kthread/current/RUNNING; unlinks from proc list; `free_pages` kstack; `kmem_cache_free`.
- `thread_switch(next)`: updates state (prev→RUNNABLE, next→RUNNING) + `set_current_thread(next)` + `cpu_switch_context`. No IRQ masking at P2-A — the only IRQ source live is the timer; its handler doesn't touch thread state.

**Tests** (`kernel/test/test_context.c`):
- `context.create_destroy`: thread_create + thread_free without ever switching in. Verifies counters + list management.
- `context.round_trip`: boot kthread → fresh kthread → entry increments shared counter → thread_switch back → boot kthread verifies. Exercises the entire context-switch primitive end-to-end.

**Spec** (`specs/scheduler.tla` + `specs/scheduler.cfg` + `specs/SPEC-TO-CODE.md`):
- Sketch — TLC-clean at 99 distinct states (Threads={t1,t2,t3}, CPUs={c1,c2}); depth 9.
- Invariants: `StateConsistency`, `NoSimultaneousRun`, `RunnableInQueue`, `SleepingNotInQueue`.
- Actions: `Yield`, `Block`, `Wake`, `Resume`. Block is atomic (P2-B refines into CheckCond + Sleep with `NoMissedWakeup`).
- `SPEC-TO-CODE.md` stub populated through Phase 2.

**Bug fixes** (collateral, surfaced by P2-A):
- `kernel/test/test_phys.c`: `phys.alloc_smoke` + `phys.leak_10k` now `magazines_drain_all()` BEFORE taking baseline. Phase 1's tests didn't need this — no SLUB allocations happened pre-test so the magazine was empty. P2-A's proc_init + thread_init each `kmem_cache_create` + `kmem_cache_alloc`, leaving ~5 pages resident in the order-0 magazine. Without pre-drain, the post-drain comparison reads HIGHER than baseline → false drift.
- `tools/verify-kaslr.sh`: now reads `build/test-boot.log` directly instead of `tools/test.sh` stdout. P2-A added `kproc:` + `kthread:` banner lines pushing the KASLR offset off the test.sh tail-20.

**Banner** (`kernel/main.c`):
- `kproc:   pid=0 threads=1`
- `kthread: tid=0 state=RUNNING (current_thread = kthread)`
- Phase tag bumped P1-I → P2-A (CMakeLists.txt cache var; `rm -rf build/kernel` needed for incremental rebuilds — known cache footgun).

**Reference doc**: `docs/reference/14-process-model.md`. Snapshot in `docs/REFERENCE.md` bumped (11/11 tests, ~4330 LOC, ~227 KB ELF, 1 spec).

**Status doc**: `docs/phase2-status.md` (new) — sub-chunk plan, ROADMAP §5.2 exit criteria checklist, trip-hazards.

---

## Trip hazards (cumulative; new at P2-A marked NEW)

### NEW at P2-A

1. **TPIDR_EL1 reset value is UNKNOWN per ARM ARM**. P2-A does not initialize it in start.S — pre-`thread_init` reads of `current_thread()` are undefined. All current callers post-date `thread_init`; defensively initializing TPIDR_EL1 in start.S would make pre-init reads deterministic-NULL but is held for explicit signoff (start.S edit).
2. **No guard page below `thread_create` kstacks**. Boot kthread inherits the boot stack with its P1-C-extras Part A guard; thread_create kstacks have NO guard at P2-A. Stack overflow silently corrupts adjacent SLUB / buddy / page-array memory. **Phase 2 close** lands the per-thread guard page (bumps order to 3 = 32 KiB with bottom page unmapped via `mmu_unmap`).
3. **No IRQ masking around `thread_switch`**. Timer IRQ handler at v1.0 doesn't touch thread state, so reentrancy is trivially safe. **P2-B** adds `spin_lock_irqsave` discipline once scheduler-tick preemption (timer IRQ → reschedule) becomes possible.
4. **`thread_switch` unconditionally sets prev's state to `THREAD_RUNNABLE`**. Correct for "yield to next" semantics. For "block on a condition" (sleep), prev must be `THREAD_SLEEPING`. **P2-B** adds the separate `thread_block(cond)` primitive.
5. **`thread_trampoline` halts on entry-return (WFE)**. No `thread_exit` plumbing yet — descriptor isn't reclaimed; kstack remains allocated. Mitigated at P2-A: test entries call `thread_switch(boot_kt)` instead of returning. **Phase 2 close** lands real `thread_exit` + reap.
6. **`scheduler.tla` is a sketch — `_buggy.cfg` deferred to P2-B**. The CLAUDE.md "executable documentation" pattern (primary `.cfg` clean + `_buggy.cfg` showing a specific invariant violation) is unfulfilled because the relevant bug-classes (missed wakeups, deadline starvation, IPI reordering) are introduced by P2-B's actions. P2-B lands `scheduler_buggy.cfg`.
7. **`struct Context` size pinned at 112 bytes**. Adding a field (e.g., FPSIMD state at Phase 5 for userspace) requires updating `_Static_assert`s in `context.h` AND offsets / `stp/ldp/str/ldr` immediates in `context.S` together. The build break is loud but multi-step.
8. **`kstack_base` is PA-cast-to-void* at v1.0** (consistent with `kpage_alloc`). Phase 2 introduces the kernel direct map; both `kpage_alloc` and `thread_create` convert to high-VA pointers in the same chunk.
9. **`tools/test.sh` truncates the log to tail-20**. P2-A added kproc + kthread banner lines, pushing the KASLR offset off the tail. `tools/verify-kaslr.sh` was updated to read `build/test-boot.log` directly. Future banner growth doesn't break the verifier; but a future debugger looking at test.sh stdout might miss earlier banner lines.
10. **`phys.alloc_smoke` + `phys.leak_10k` now drain magazines BEFORE baseline**. Phase 1's tests didn't need this; P2-A's proc_init + thread_init residency would cause false drift. The pre-drain is the durable fix.

### Carried from handoff 005 (selected — full list there)

- Boot banner ABI (`Thylacine boot OK` + `EXTINCTION:`) is kernel-tooling contract.
- `KASLR_LINK_VA = 0xFFFFA00000080000` lives in TWO places (kaslr.h + kernel.ld); linker `ASSERT()` enforces equality.
- `volatile` on `g_kernel_pa_*` in kaslr.c is load-bearing under `-O2 -fpie -mcmodel=tiny`.
- TTBR0 stays active post-KASLR for low-PA access; Phase 2 retires when user namespaces move there (P2-D).
- `kfree` requires head pointer (P1-I F32); `kmem_cache_destroy` extincts on `nr_full != 0` (P1-I F33).
- `spin_lock_irqsave` actually masks IRQs (P1-I F30) — don't revert to no-op.
- `mmu_map_device` rejects ranges hitting kernel-image L3 block (P1-I F31).
- Volatile in `be32_load`, `g_kernel_pa_*`, `g_ticks` — load-bearing.

---

## What's next

**Phase 2 — P2-B (EEVDF scheduler + wait/wake)**. Per ROADMAP §5.1 + ARCH §8.

### Decision tree for the next chunk

**Option A (recommended) — P2-B as a single chunk**: EEVDF + wait/wake together.

Sub-deliverables:
- `kernel/sched.c` — per-CPU run trees (one per band per CPU); three priority bands (INTERACTIVE / NORMAL / IDLE); `sched()` / `ready()` / `sleep()` / `wakeup()` / `rendezvous()` Plan 9 idiom layer.
- `kernel/eevdf.c` — virtual eligible time + virtual deadline; band-aware insertion; pick-earliest-deadline.
- `kernel/run_tree.c` — red-black tree on vd_t.
- `thread_block(cond)` / `thread_wake(t)` — lock-protected enqueue protocol per ARCH §8.5.
- Scheduler-tick preemption via timer IRQ extension.
- IRQ-mask discipline around `thread_switch` (close P2-A trip-hazard #3).
- `scheduler.tla` refinement: split Block into CheckCond + Sleep; prove `NoMissedWakeup`; land `scheduler_buggy.cfg` showing the missed-wakeup race; model EEVDF deadline math + IPI ordering.

Estimated 800-1200 LOC + ~150-300 LOC TLA+; 250-400k tokens.

**Option B — Split P2-B into P2-Ba (scheduler core) + P2-Bb (wait/wake + spec refinement)**: Lower-risk individual commits but longer overall path.

**Option C — Spec-first single chunk**: Refine `scheduler.tla` to TLC-clean with EEVDF + wait/wake atomicity proofs FIRST; implement against the spec second. Per CLAUDE.md spec-first policy this is canonically correct. Trade-off: no immediate runtime evidence; the spec lands and code follows.

**Recommendation**: A with the spec refinement landing in parallel. Per ARCH §25.2 the full scheduler.tla is mandatory for Phase 2 close, not P2-B entry — but starting design with the spec produces better code.

---

## Posture summary

| Metric | Value |
|---|---|
| Tip commit | `df78693` (P2-A: process-model bootstrap) |
| Phase | **Phase 2 in progress (P2-A landed)** |
| Next chunk | P2-B (EEVDF scheduler + wait/wake) |
| Build matrix | default Debug — green; UBSan trapping — green |
| `tools/test.sh` | PASS (~42 ms boot) |
| `tools/test.sh --sanitize=undefined` | PASS (~71 ms boot; UBSan-clean) |
| `tools/test-fault.sh` | PASS (3/3 protections fire under attack) |
| `tools/verify-kaslr.sh -n 5` | PASS (5/5 distinct) |
| In-kernel tests | 11/11 PASS (Phase 1 9 + context.create_destroy + context.round_trip) |
| Specs | 1/9 — `scheduler.tla` (TLC-clean at sketch level, 99 distinct states) |
| LOC | ~3120 C99 + ~440 ASM + ~75 LD + ~330 sh/cmake + ~290 process model + ~75 spec ≈ ~4330 |
| Kernel ELF | ~227 KB debug (production); ~248 KB (UBSan) |
| Kernel flat binary | ~38 KB |
| Boot time | ~42 ms (production), ~71 ms (UBSan) — VISION §4 budget 500 ms |
| Page tables | 40 KiB BSS |
| struct page array | 24 MiB BSS |
| GIC handler table | 16 KiB BSS |
| Boot banner reserved | ~26 MiB |
| RAM free at boot | ~2022 MiB / 2048 MiB |
| Open audit findings | round 4 in flight at commit time (post-commit pattern); will be triaged after the prosecutor agent reports |

---

## Stratum coordination

Stratum's Phase 8 (POSIX surface) is in progress; Phase 9 (9P server + Stratum extensions) is Thylacine Phase 4's integration target. **Phases 2 + 3 (devices) proceed in parallel with Stratum's 8 + 9** — no dependency until Phase 4 entry.

---

## Format ABI surfaces in flight

(All carry forward from handoff 005 unchanged. P2-A introduces no new ABI surfaces — the new banner lines (kproc, kthread) are diagnostic. struct Proc / struct Thread / struct Context are kernel-internal contracts; not exposed to userspace at v1.0.)

**NEW at P2-A** (kernel-internal contracts):
- `struct Context` layout (14 × u64 = 112 bytes; offsets pinned via _Static_assert) — `kernel/include/thylacine/context.h`. arch/arm64/context.S hardcodes the offsets.
- TPIDR_EL1 = current thread pointer per CPU — kernel-internal convention; load-bearing internal contract.

---

## Things I would NOT recommend deviating from (cumulative)

(All from handoff 005 carry forward. New at P2-A:)

- **`current_thread()` / `set_current_thread()` use TPIDR_EL1** — don't move to a global pointer; SMP at Phase 2 close needs per-CPU naturally.
- **`cpu_switch_context` does NOT use paciasp/autiasp** — relies on x30 being the unsigned bl-return-address. The C caller's pac-ret discipline pushes its OWN signed lr to its stack at its prologue.
- **`struct Context` layout is the asm contract** — never reorder fields without bumping the `_Static_assert` offsets AND the `stp/ldp/str/ldr` immediates in context.S together.
- **`thread_trampoline` reached via ret (BTYPE=00)** — the `bti c` is defensive. A future indirect-jump dispatch into the trampoline (br x16) would also work because BTYPE=01 from `br` doesn't match `bti c`, so an audit must catch any `br thread_trampoline` regression.
- **kproc + kthread are permanent** — `proc_free` / `thread_free` extincts on attempt.
- **`thread_switch(prev, next)` updates state + `set_current_thread` BEFORE `cpu_switch_context`** — the window is invisible at P2-A but P2-B's IRQ-mask discipline makes it explicitly atomic.
- **`scheduler.tla` is binding for Phase 2 close** — refinements at P2-B / P2-C / Phase 2 close land the EEVDF + wait/wake + IPI + work-stealing proofs.

---

## Open questions / future-work tags

(All from handoff 005 carry forward. New at P2-A:)

- **U-25** (NEW, P2-A trip-hazard #1): TPIDR_EL1 reset value is UNKNOWN. Defensive `msr tpidr_el1, xzr` in start.S would harden pre-thread_init reads. Held for explicit signoff (start.S edit).
- **U-26** (NEW, P2-A trip-hazard #2): per-thread guard page below kstack. Phase 2 close target.
- **U-27** (NEW, P2-A trip-hazard #4): `thread_block(cond)` primitive. P2-B target.
- **U-28** (NEW, P2-A trip-hazard #5): real `thread_exit` + reap. Phase 2 close target.
- **U-29** (NEW, P2-A spec): `scheduler_buggy.cfg` showing missed-wakeup race. P2-B target.

---

## Sign-off

Phase 2 has opened. The kernel can now multitask — primitively — and the spec discipline is binding from this phase forward. **P2-A landed.** P2-B picks up the EEVDF dispatcher; the scheduler.tla refinement lands in parallel.

(Project motto reserved for milestone moments. Phase 2 close is the next canonical such moment.)
