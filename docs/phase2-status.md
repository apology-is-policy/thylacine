# Phase 2 — status and pickup guide

Authoritative pickup guide for **Phase 2: Process model + scheduler + handles + VMO + first formal specs**.

## TL;DR

Phase 2 brings up the process / thread model, EEVDF scheduler, handle table, VMO manager, and the first three formal specs (`scheduler.tla`, `namespace.tla`, `handles.tla`). Multi-process concurrent execution; preemptive EEVDF with three priority bands; SMP-aware on 4 vCPUs; rfork / exec / exits / wait fully working; init runs a UART debug shell. **Spec-first discipline binding from this phase forward** — no invariant-bearing implementation merges without the corresponding TLA+ refinement.

Per `ROADMAP.md §5`. Phase 2 entry chunk **P2-A** (process-model bootstrap) has landed.

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| `df78693` | **P2-A**: process-model bootstrap. struct Proc + struct Thread + struct Context + cpu_switch_context + thread_trampoline + thread_switch + current_thread (via TPIDR_EL1). proc_init (kproc PID 0) + thread_init (kthread TID 0). SLUB caches. specs/scheduler.tla sketch (TLC-clean, 99 states). 2 new tests (context.create_destroy + context.round_trip). Banner: kproc + kthread lines. Phase tag bumped to P2-A. Collateral bug fixes: alloc_smoke + leak_10k drain magazines pre-baseline; verify-kaslr.sh reads full log. Reference doc 14-process-model.md. | 11/11 tests; ~42 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; spec TLC-clean. |
| `a212038` | **P2-Ba**: scheduler dispatch. New `kernel/include/thylacine/sched.h` + `kernel/sched.c` (~150 LOC) + `kernel/test/test_sched.c`. struct Thread extended (168→200 bytes; `_Static_assert` updated) with `vd_t` (s64) + `weight` (u32) + `band` (u32) + `runnable_next`/`runnable_prev`. Plan 9 idiom: `sched()` (yield) + `ready(t)` (mark runnable + insert into per-band run tree). 3 priority bands (INTERACTIVE/NORMAL/IDLE). Run tree: per-band sorted-by-vd_t doubly-linked list (head = min vd_t). Pick: highest non-empty band; min vd_t. On yield: prev's vd_t advances past tree max via `g_vd_counter++`; prev inserts at back of band; next removes from band; switch. `thread_free` calls `sched_remove_if_runnable` before unlink. `main.c` calls `sched_init` after `thread_init`. Banner adds `sched:` line. Phase tag bumped to P2-Ba. New reference doc `docs/reference/15-scheduler.md`. **Simplified EEVDF at P2-Ba**: monotonic vd_t advance gives FIFO-equivalent rotation; full math (weight + slice + latency bound) lands at P2-Bc. Spec EEVDF refinement deferred to P2-Bc. | 13/13 tests; ~42 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (still 283 states — spec untouched at P2-Ba). New tests: `scheduler.dispatch_smoke` (3-thread round-robin via `sched()`), `scheduler.runnable_count` (counter sanity). |
| `bbd7cbf` | **P2-A R4 audit close**: 1 P1 + 3 P2 + 5 P3 fixed; 1 P3 deferred-with-rationale. **F40 (P1)**: KP_ZERO on all proc/thread kmem_cache_alloc (defends future struct growth against UMR). **F41 (P2)**: BTI BTYPE comments corrected per ARM ARM D24.2.1 (BL→00, BR→01, BLR→10) in context.S + start.S + reference doc. **F42 (P2)**: u64 magic field at offset 0 of struct Proc (PROC_MAGIC) and struct Thread (THREAD_MAGIC); SLUB freelist write naturally clobbers magic on free, so subsequent free reads clobbered value and extincts with explicit double-free diagnostic. **F43 (P2)**: THREAD_STATE_INVALID checks in thread_switch + thread_free. **F44+F45 (P3)**: msr tpidr_el0/el1, xzr in start.S step 4.5 (closes U-25). **F46 (P3)**: _Static_asserts for THREAD_STATE_INVALID==0, sizeof(struct Proc)==24, sizeof(struct Thread)==168, magic at offset 0. **F47 (P3)**: closed-universe modeling assumption documented in scheduler.tla preamble. **F48 (P3)** deferred to U-30 (kstack zeroing on free). Plus **P2-B-1 (not a finding)**: scheduler.tla extended with wait/wake atomicity (cond + waiters + WaitOnCond + BuggyCheck/BuggySleep + WakeAll + NoMissedWakeup); scheduler_buggy.cfg produces missed-wakeup counterexample. struct Proc grew 16→24 bytes; struct Thread grew 160→168 bytes. | 11/11 tests; ~39 ms boot; UBSan-clean; fault matrix 3/3; KASLR 5/5 distinct; scheduler.tla TLC-clean (283 states); scheduler_buggy.cfg counterexample at depth 4. |
| `4e108a8` | **P2-Bb**: Plan 9 wait/wake. New `kernel/include/thylacine/rendez.h` + sleep/wakeup impl in `kernel/sched.c` (~110 LOC). struct Thread extended (200→208 B; new field `rendez_blocked_on`). `sched()` refactored to respect pre-set `prev->state`: RUNNING → yield (RUNNABLE+insert); SLEEPING/EXITING → don't insert (caller already transitioned). Defensive `prev==next` re-insert+return guard for the SMP race window between sleep's `spin_unlock(&r->lock)` and `sched()` entry (P2-C closes properly via finish_task_switch pattern). spin_lock_irqsave on `r->lock` for IRQ-mask discipline at UP — P2-Bc's timer-IRQ scheduler tick exercises the path. Single-waiter Rendez (Plan 9 convention); structurally a special case of `scheduler.tla`'s multi-waiter spec. New tests: rendez.{sleep_immediate_cond_true, basic_handoff, wakeup_no_waiter}. New reference doc `docs/reference/16-rendez.md`. Phase tag bumped P2-Ba → P2-Bb. | 16/16 tests; ~42 ms boot; UBSan-clean (~70 ms); fault matrix 3/3; KASLR 5/5 distinct; `scheduler.tla` TLC-clean (283 states — spec unchanged at P2-Bb; impl maps to existing actions); `scheduler_buggy.cfg` counterexample at depth 4. |

## Remaining work

Sub-chunk plan (refined as Phase 2 progresses):

1. ✅ **P2-A: process-model bootstrap.** Landed. struct Proc + struct Thread + cpu_switch_context + thread_switch + scheduler.tla sketch.
2. ✅ **P2-Ba: scheduler dispatch.** Landed. sched() + ready() + 3-band run tree.
3. ✅ **P2-Bb: wait/wake.** Landed. Rendez + sleep + wakeup. scheduler.tla WaitOnCond / WakeAll mapping (spec was extended at P2-A R4; impl lands here).
4. **P2-Bc (next): preemption + EEVDF math**. Scheduler-tick preemption via timer IRQ — current g_ticks handler extends to trigger reschedule when slice expires. IRQ-mask discipline around `sched()` / `ready()` (close P2-A trip-hazard #3 fully). Full EEVDF math: `vd_t = ve_t + slice × W_total / w_self` with weighted virtual time advance; pick-min-deadline. `scheduler.tla` refinement: PickIsMinDeadline + LatencyBound (ARCH §28 I-17). Estimated 400-600 LOC + ~80 LOC additional TLA+.
5. **P2-C: SMP secondary bring-up + work-stealing**. PSCI CPU bring-up of vCPUs 1..3; per-CPU IRQ routing; cross-CPU IPIs (`IPI_RESCHED`, `IPI_TLB_FLUSH`, `IPI_HALT`, `IPI_GENERIC`); work-stealing dispatch from idle CPUs. Per-CPU exception stacks (closes the P1-F shared-stack limitation). `finish_task_switch` pattern closes the SMP wait/wake race (see P2-Bb trip-hazard #15). `scheduler.tla` work-stealing fairness refinement + IPI ordering.
6. **P2-D: rfork + exits + wait**. `kernel/proc.c` rfork(flags) implementing the resource-share / clone matrix per ARCH §7.4; `exits()` terminate; `wait()` reap. Address-space tear-down on exit; thread cleanup; child re-parenting to PID 1 (init). Phase 2 close adds the per-thread guard page (closes the P2-A trip-hazard #1).
7. **P2-E: namespace + bind/mount**. `kernel/namespace.c` — `Pgrp`, `bind`, `mount` (stub: ramfs only at this phase), `unmount`. Namespace cloned on rfork(RFPROC); shared on rfork(RFPROC | RFNAMEG). New spec `namespace.tla`: cycle-freedom (I-3), isolation (I-1).
8. **P2-F: handle table + VMO**. `kernel/handle.c` — typed kernel object handles per ARCH §18 (Process / Thread / VMO / MMIO / IRQ / DMA / Chan / Interrupt); rights monotonicity (I-2, I-6); transfer-via-9P invariant (I-4) — placeholder pending Phase 4's 9P client; hardware-handle non-transferability (I-5). `kernel/vmo.c` — VMO manager per ARCH §19; refcount + mapping lifecycle (I-7). New spec `handles.tla` (mandatory; Phase 2 close).
9. **P2-G: ELF loader + minimal init**. `arch/arm64/elf.c` — ELF loader; reject RWX segments per W^X invariant. `init/init-minimal.c` — first userspace process; UART debug shell.
10. **P2-H: Phase 2 closing audit + spec finalization**. Round-4 audit on the entire Phase 2 surface; spec mappings in `specs/SPEC-TO-CODE.md` populated and CI-checked. ROADMAP §5.2 exit criteria all met.

## Exit criteria status

(Copy from `ROADMAP.md §5.2`; tick as deliverables complete.)

- [ ] Two processes run concurrently on a single CPU; timer preemption works. (P2-B)
- [ ] Four processes run concurrently on 4 vCPUs; EEVDF latency bound holds. (P2-C + P2-B)
- [ ] `rfork(RFPROC)` + `exits()` + `wait()` lifecycle works without leak (1000-iteration stress test). (P2-D)
- [ ] `exec()` loads and runs a static ELF from ramfs; rejects RWX segments. (P2-G)
- [ ] init starts a UART shell; `echo hello` works via pipe. (P2-G)
- [ ] Page fault handler allocates demand pages; stack growth works. (P2-D / P2-F + Phase 3 dependency)
- [ ] Handle table: 10,000 handles open/close cycle without leak; rights reduction enforced; hardware-handle transfer attempt panics cleanly. (P2-F)
- [ ] VMO: create, map, write, read, unmap, close cycle correct; pages freed on last-handle-close + last-mapping-unmap. (P2-F)
- [ ] Stress: 1000 `rfork`/`exits`/`wait` cycles across 4 CPUs without leak or panic. (P2-D + P2-C)
- [ ] Wakeup atomicity: 1000 producer/consumer pairs across 4 CPUs in tight loop; no missed wakeups (verified by counter). (P2-B + P2-C)
- [ ] Work-stealing: 4-CPU test with imbalanced load; load redistributes within 5ms. (P2-C)
- [ ] **TSan clean** on the SMP test suite. (P2-C)
- [ ] `specs/scheduler.tla`, `specs/namespace.tla`, `specs/handles.tla` clean under TLC. (P2-A landed scheduler sketch; refinement at P2-B + P2-C; namespace + handles at P2-E + P2-F)
- [ ] `SPEC-TO-CODE.md` for all three specs maintained. (Stubbed at P2-A; populated through Phase 2)
- [ ] No P0/P1 audit findings. (Closing audit at P2-H)

## Build + verify commands

```bash
# Build the kernel ELF (build/kernel/thylacine.elf, ~227 KB debug)
tools/build.sh kernel
make kernel

# Run interactively
tools/run-vm.sh
make run

# Integration test: 11/11 in-kernel tests + boot banner within 10s
tools/test.sh
make test

# UBSan build (build/kernel-undefined; isolated cache)
tools/build.sh kernel --sanitize=undefined
tools/test.sh --sanitize=undefined

# Deliberate-fault matrix (regression check for Phase 1 hardening)
tools/test-fault.sh

# KASLR variance
tools/verify-kaslr.sh -n 10

# All TLA+ specs (1 spec at P2-A: scheduler.tla)
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && for s in *.tla; do
    echo "== $s =="
    java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
        -config "${s%.tla}.cfg" "$s" 2>&1 | tail -3
done
```

Reference output of a successful boot (banner subset):

```
Thylacine v0.1.0-dev booting...
  arch: arm64
  el-entry: EL1 (direct)
  cpus: 1 (P1-C-extras; SMP at P1-F)
  mem:  2048 MiB at 0x0000000040000000
  ...
  gic:  v3 dist=0x0000000008000000 redist=0x00000000080a0000
  timer: 1000000 kHz freq, 1000 Hz tick (PPI 14 / INTID 30)
  kproc:   pid=0 threads=1
  kthread: tid=0 state=RUNNING (current_thread = kthread)
  tests:
    [test] kaslr.mix64_avalanche ... PASS
    ...
    [test] context.create_destroy ... PASS
    [test] context.round_trip ... PASS
  tests: 11/11 PASS
  ticks: 25 (kernel breathing)
  boot-time: 42.0 ms (target < 500 ms per VISION §4)
  phase: P2-A
Thylacine boot OK
```

## Trip hazards

(Cumulative from Phase 1; new at Phase 2 marked NEW.)

### NEW at P2-A

1. ~~**TPIDR_EL1 reset value is UNKNOWN per ARM ARM**~~. **CLOSED at P2-A R4 (F44)**: start.S step 4.5 now emits `msr tpidr_el1, xzr` AND `msr tpidr_el0, xzr` after BSS clear, making pre-`thread_init` reads of `current_thread()` deterministic-NULL.
2. **No guard page below `thread_create` kstacks**. The boot kthread (TID 0) inherits the boot stack which has a guard from P1-C-extras Part A; threads created via `thread_create` get a 16 KiB stack from `alloc_pages(2, KP_ZERO)` with NO guard page. Stack overflow in a P2-A kthread silently corrupts neighbor memory. **Phase 2 close** lands the per-thread guard page.
3. **No IRQ masking around `thread_switch`**. The only IRQ source live at v1.0 is the timer; its handler increments `g_ticks` and returns — does not touch thread state. Reentrancy is trivially safe at P2-A. **P2-B** adds the IRQ-mask discipline once scheduler-tick preemption becomes possible (timer IRQ → `thread_block` / dispatch).
4. **`thread_switch` unconditionally sets prev's state to `THREAD_RUNNABLE`**. Correct for "yield to next" semantics. For "block on a condition" (sleep), prev must be `THREAD_SLEEPING`. **P2-B** adds the separate `thread_block(cond)` primitive.
5. **`thread_trampoline` halts on entry-return (WFE)**. No `thread_exit` plumbing yet — if a thread's entry returns, the descriptor isn't reclaimed and the kstack remains allocated. Mitigated at P2-A: test entries don't return (call `thread_switch(kt0)` instead). **Phase 2 close** lands real `thread_exit` + reap.
6. **`scheduler.tla` is a sketch — `_buggy.cfg` deferred to P2-B**. The CLAUDE.md "executable documentation" pattern (primary `.cfg` clean + `_buggy.cfg` showing a specific invariant violation) is unfulfilled at P2-A because the relevant bug-classes (missed wakeups, deadline starvation, IPI reordering) are introduced by P2-B's actions. P2-B lands `scheduler_buggy.cfg` showing the missed-wakeup race when wait/wake atomicity is violated.
7. **`struct Context` size pinned at 112 bytes**. Adding a field (e.g., FPSIMD state at Phase 5 for userspace) requires updating `_Static_assert`s in `context.h` AND offsets / stp/ldp/str/ldr immediates in `context.S` together. The build break is loud but multi-step.
8. **`kstack_base` is PA-cast-to-void* at v1.0** (consistent with `kpage_alloc`). Phase 2 introduces the kernel direct map; both `kpage_alloc` and `thread_create` convert to high-VA pointers in the same chunk.
9. **`tools/test.sh` truncates the log to tail-20**. P2-A added kproc + kthread banner lines, pushing the KASLR offset line off the tail. `tools/verify-kaslr.sh` was updated to read `build/test-boot.log` directly instead of test.sh stdout. Future banner growth doesn't break the verifier; but a future debugger looking at test.sh stdout might miss earlier banner lines.
10. **`phys.alloc_smoke` + `phys.leak_10k` now drain magazines BEFORE baseline**. Phase 1's tests didn't need this — no SLUB allocations happened pre-test so the magazine was empty. P2-A's proc_init + thread_init each `kmem_cache_create` + `kmem_cache_alloc`, leaving ~5 pages resident in the order-0 magazine. Without the pre-drain, the post-drain comparison reads HIGHER than baseline by the resident count → false drift. The pre-drain is the durable fix.
11. **U-30 (NEW at P2-A R4 F48)**: `thread_free` returns kstack pages with stale stack-frame contents to buddy. At P2-A there's no userspace + only kernel callers of subsequent allocations + `KP_ZERO` is the documented escape hatch. Phase 3's user-facing allocators must use `KP_ZERO` consistently. Deferred — no immediate fix at P2-A.
12. **`struct Proc` size pinned at 24 bytes; `struct Thread` size pinned at 168 bytes (P2-A R4 F46)**. `_Static_assert`s in proc.h/thread.h. Adding a field breaks the build until the assert is bumped; the bump must be deliberate.
13. **`magic` field at offset 0 of struct Proc + Thread (P2-A R4 F42)**. SLUB's freelist write on `kmem_cache_free` naturally clobbers it; subsequent `proc_free`/`thread_free` reads the clobbered value and extincts with explicit double-free diagnostic. The contract is "magic is at offset 0 SO that the SLUB clobber kills it" — moving the field elsewhere defeats the defense.
14. **BTI BTYPE values correctly stated everywhere (P2-A R4 F41)**. Per ARM ARM D24.2.1: BL → BTYPE=0b00, BR → BTYPE=0b01, BLR → BTYPE=0b10. The previous comments in `context.S` + `start.S` + reference doc had values swapped or wrong. Now consistent.

### NEW at P2-Bb

15. **`sched()` now respects pre-set `prev->state`** (P2-Bb refactor). Caller protocol: set `prev->state = THREAD_SLEEPING` (or `EXITING`) BEFORE calling `sched()` to block; leave `prev->state == THREAD_RUNNING` to yield. `sched()` sees the state and dispatches accordingly (yield-insert vs leave-alone). Any callers that forget to set state will get yield semantics by default — safe but wrong for blockers (the thread becomes runnable again immediately). The `sleep` impl is the canonical example; future block paths (timer-IRQ preemption that puts a thread to sleep on its own slice expiry; postnote stop) follow the same pattern.

16. **SMP wait/wake race window (deferred to P2-C)**. Between `sleep`'s `spin_unlock(&r->lock)` and entering `sched()`, on SMP a peer wakeup can fire on another CPU, transition `current` → RUNNABLE, ready() it (insert into a runqueue). When `current` then enters `sched()`, `pick_next` may pull `current` out of its own runqueue. The current `prev == next` defensive check (re-insert + return-without-switch) is a soft mitigation; the proper fix is the `finish_task_switch` pattern at P2-C — drop the runqueue lock only AFTER the context switch completes. UP at v1.0 P2-Bb: race cannot fire (single CPU + IRQ-mask held throughout sleep).

17. **`sleep` keeps the IRQ mask held across `sched()`**. Per the pattern in 9front and Linux: the IRQ mask saved at sleep entry is restored only after the loop exits. Between `spin_unlock(&r->lock)` and `sched()`, IRQ is masked locally — what makes the wait/wake protocol robust against IRQ-context wakeups at UP. The next thread switched in inherits "live" IRQ state for the duration of its own critical sections (which are themselves balanced via irqsave/irqrestore). P2-Bc's scheduler-tick preemption tests this discipline at scale.

18. **Single-waiter Rendez (Plan 9 convention)**. At most one thread per Rendez at a time; second sleeper extincts. Multi-waiter wait queues land at Phase 5 (poll, futex). Today's kernel use cases (timer expiry, basic producer/consumer) fit the single-waiter pattern naturally; userspace primitives that need many waiters (futexes for pthread cond vars) get a wait-queue generalization.

19. **`struct Thread` size pinned at 208 bytes (P2-Bb)**. P2-Ba's 200 + 8 (rendez_blocked_on). `_Static_assert` updated. Future Thread fields grow this further; deliberate per-bump.

20. **`sched()`'s deadlock detection assumes UP-no-other-CPU**. `sched: deadlock — current is blocking, no runnable peer` extincts when prev is SLEEPING + run tree is empty. At v1.0 P2-Bb single-CPU UP, this is the correct behavior — only an IRQ could wake us, and IRQs are masked. P2-C lands SMP idle-WFI: an idle CPU with no runnable thread parks at WFI, gets woken via IPI / IRQ. The extinction path then narrows to genuine deadlocks (all CPUs sleeping with no waker scheduled).

### Carried from Phase 1 (selected — see `docs/handoffs/005-phase1-close.md` for full list)

- Boot banner ABI (`Thylacine boot OK` + `EXTINCTION:`) is kernel-tooling contract.
- `KASLR_LINK_VA = 0xFFFFA00000080000` lives in TWO places (kaslr.h + kernel.ld); linker `ASSERT()` enforces equality.
- `volatile` on `g_kernel_pa_*` in kaslr.c is load-bearing under `-O2 -fpie -mcmodel=tiny`.
- TTBR0 stays active post-KASLR for low-PA access; Phase 2 retires when user namespaces move there (P2-D).
- `kfree` requires head pointer (not interior); enforced in P1-I-D.
- `kmem_cache_destroy` extincts on `nr_full != 0`; Phase 2 callers MUST drain caches before destroy.
- `spin_lock_irqsave` actually masks IRQs; don't revert to no-op (P1-I F30).
- `mmu_map_device` rejects ranges hitting kernel-image L3 block.

## Known deltas from ARCH

- ARCH §7.2 documents `struct Proc` with namespace, addr_space, fds, handles, cred, parent/children, notes — P2-A lands only `pid + thread_count + threads` because that's what P2-A's bringup uses. The struct grows by appending across sub-chunks (P2-B through P2-G); existing offsets stay stable.
- ARCH §7.3 documents `struct Thread` with errstr, EEVDF data, etc. — P2-A lands the minimal subset (tid, state, proc, ctx, kstack, list links). The rest lands at P2-B / Phase 2 close.
- ARCH §8 specifies EEVDF + per-CPU run trees + work-stealing — P2-A has neither. P2-B adds EEVDF + per-CPU; P2-C adds SMP + work-stealing.
- `scheduler.tla` at P2-A is a sketch — TLC-clean at small bounds but doesn't yet model EEVDF deadline math, wait/wake atomicity, or IPI ordering. ARCH §25.2 mandates the full spec by Phase 2 close; P2-B/P2-C/Phase 2 close land the refinements.

## References

- `docs/ARCHITECTURE.md §7` (process and thread model), `§8` (scheduler), `§9` (namespace), `§18` (handles), `§19` (VMO), `§20` (per-core SMP), `§28` (invariants — I-1 through I-9, I-17, I-18 land at this phase).
- `docs/ROADMAP.md §5` (Phase 2 deliverables, exit criteria, risks).
- `docs/handoffs/005-phase1-close.md` (Phase 1 close pickup pointer).
- `docs/reference/14-process-model.md` (P2-A as-built).
- `specs/scheduler.tla` + `specs/scheduler.cfg` (P2-A sketch).
- `specs/SPEC-TO-CODE.md` (action ↔ source location mapping; populated through Phase 2).
- `CLAUDE.md` (operational framework, audit-trigger surfaces, spec-first policy).

## Audit-trigger surfaces introduced this phase

| Surface | Files | Why | Landed at |
|---|---|---|---|
| Process model | `kernel/proc.c`, `kernel/include/thylacine/proc.h` | Lifecycle, rfork semantics (P2-D) | P2-A (descriptor only); P2-D (lifecycle) |
| Thread model | `kernel/thread.c`, `kernel/include/thylacine/thread.h` | Lifecycle, current_thread (TPIDR_EL1) | P2-A |
| Context switch | `arch/arm64/context.S`, `kernel/include/thylacine/context.h` | Register state save/load correctness; layout pinning | P2-A |
| Scheduler | `kernel/sched.c` (P2-B), `kernel/eevdf.c` (P2-B), `kernel/run_tree.c` (P2-B), `arch/arm64/ipi.c` (P2-C) | EEVDF correctness, SMP, wakeup atomicity (I-8, I-9, I-17, I-18) | (planned) |
| Namespace | `kernel/namespace.c` (P2-E) | Cycle-freedom (I-3), isolation (I-1) | (planned) |
| Handle table | `kernel/handle.c` (P2-F) | Rights, transfer, type discipline (I-2, I-4, I-5, I-6) | (planned) |
| VMO | `kernel/vmo.c` (P2-F) | Refcount, mapping lifecycle (I-7) | (planned) |
| Page fault | `arch/arm64/fault.c` (P2-D) | Lifetime, COW | (planned) |
| ELF loader | `arch/arm64/elf.c` (P2-G) | RWX rejection | (planned) |
