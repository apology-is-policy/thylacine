# Phase 2 — status and pickup guide

Authoritative pickup guide for **Phase 2: Process model + scheduler + handles + VMO + first formal specs**.

## TL;DR

Phase 2 brings up the process / thread model, EEVDF scheduler, handle table, VMO manager, and the first three formal specs (`scheduler.tla`, `namespace.tla`, `handles.tla`). Multi-process concurrent execution; preemptive EEVDF with three priority bands; SMP-aware on 4 vCPUs; rfork / exec / exits / wait fully working; init runs a UART debug shell. **Spec-first discipline binding from this phase forward** — no invariant-bearing implementation merges without the corresponding TLA+ refinement.

Per `ROADMAP.md §5`. Phase 2 entry chunk **P2-A** (process-model bootstrap) has landed.

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| *(pending)* | **P2-A**: process-model bootstrap. New `kernel/include/thylacine/{proc,thread,context}.h` + `kernel/{proc,thread}.c` + `arch/arm64/context.S`. `struct Proc` (pid + threads list + thread_count); `struct Thread` (tid + state + proc + ctx + kstack_base/size + list links); `struct Context` (14 × u64 = 112 bytes — callee-saved + SP + LR + TPIDR_EL0; offsets pinned by `_Static_assert`); `cpu_switch_context` (asm; bti c; stp/ldp save/load; ret); `thread_trampoline` (asm; bti c; blr x21; halt-on-return); `thread_switch` (C wrapper; updates state + `set_current_thread` + asm switch). `current_thread` parked in TPIDR_EL1 (per-CPU OS-reserved register; inline mrs/msr accessors). `proc_init` (PID 0 = kproc) + `thread_init` (TID 0 = kthread, parked in TPIDR_EL1). SLUB caches for proc + thread. New `specs/scheduler.tla` sketch (TLC-clean at 99 distinct states; Threads={t1,t2,t3}, CPUs={c1,c2}); `specs/SPEC-TO-CODE.md` stub. New `kernel/test/test_context.c` with `context.create_destroy` + `context.round_trip` tests. Banner adds `kproc:` + `kthread:` lines. Phase tag bumped to P2-A. Bug fix: `kernel/test/test_phys.c` `phys.alloc_smoke` + `phys.leak_10k` now `magazines_drain_all()` BEFORE the baseline (P2-A's proc_init + thread_init left ~5 pages in the order-0 magazine; previous behavior saw false drift). Bug fix: `tools/verify-kaslr.sh` reads the full log file instead of `tools/test.sh` stdout (banner growth pushed the KASLR offset off the tail-20). New reference doc `docs/reference/14-process-model.md`. | `tools/test.sh` PASS (11/11; ~42 ms boot). `tools/test.sh --sanitize=undefined` PASS (11/11; ~71 ms boot). `tools/test-fault.sh` PASS (3/3 protections fire under attack). `tools/verify-kaslr.sh -n 5` PASS (5/5 distinct). `scheduler.tla` TLC-clean. |

## Remaining work

Sub-chunk plan (refined as Phase 2 progresses):

1. ✅ **P2-A: process-model bootstrap.** Landed. struct Proc + struct Thread + cpu_switch_context + thread_switch + scheduler.tla sketch.
2. **P2-B (next): EEVDF scheduler + wait/wake**. `kernel/sched.c` — per-CPU run trees, three priority bands (INTERACTIVE / NORMAL / IDLE), EEVDF deadline computation; `kernel/eevdf.c` — virtual eligible time + virtual deadline; `kernel/run_tree.c` — red-black tree on vd_t. Wait/wake (`thread_block` + `thread_wake`) implementing the lock-protected enqueue protocol per ARCH §8.5. Scheduler-tick preemption via timer IRQ. IRQ-mask discipline around context switches. `scheduler.tla` refinement: split Block into CheckCond + Sleep; prove `NoMissedWakeup`; land `scheduler_buggy.cfg` showing the missed-wakeup race; model EEVDF deadline math + IPI ordering. Estimated 800-1200 LOC.
3. **P2-C: SMP secondary bring-up + work-stealing**. PSCI CPU bring-up of vCPUs 1..3; per-CPU IRQ routing; cross-CPU IPIs (`IPI_RESCHED`, `IPI_TLB_FLUSH`, `IPI_HALT`, `IPI_GENERIC`); work-stealing dispatch from idle CPUs. Per-CPU exception stacks (closes the P1-F shared-stack limitation). Plan 9 idiom layer (`sched`, `ready`, `sleep`, `wakeup`, `rendezvous`). `scheduler.tla` work-stealing fairness refinement.
4. **P2-D: rfork + exits + wait**. `kernel/proc.c` rfork(flags) implementing the resource-share / clone matrix per ARCH §7.4; `exits()` terminate; `wait()` reap. Address-space tear-down on exit; thread cleanup; child re-parenting to PID 1 (init). Phase 2 close adds the per-thread guard page (closes the P2-A trip-hazard #1).
5. **P2-E: namespace + bind/mount**. `kernel/namespace.c` — `Pgrp`, `bind`, `mount` (stub: ramfs only at this phase), `unmount`. Namespace cloned on rfork(RFPROC); shared on rfork(RFPROC | RFNAMEG). New spec `namespace.tla`: cycle-freedom (I-3), isolation (I-1).
6. **P2-F: handle table + VMO**. `kernel/handle.c` — typed kernel object handles per ARCH §18 (Process / Thread / VMO / MMIO / IRQ / DMA / Chan / Interrupt); rights monotonicity (I-2, I-6); transfer-via-9P invariant (I-4) — placeholder pending Phase 4's 9P client; hardware-handle non-transferability (I-5). `kernel/vmo.c` — VMO manager per ARCH §19; refcount + mapping lifecycle (I-7). New spec `handles.tla` (mandatory; Phase 2 close).
7. **P2-G: ELF loader + minimal init**. `arch/arm64/elf.c` — ELF loader; reject RWX segments per W^X invariant. `init/init-minimal.c` — first userspace process; UART debug shell.
8. **P2-H: Phase 2 closing audit + spec finalization**. Round-4 audit on the entire Phase 2 surface; spec mappings in `specs/SPEC-TO-CODE.md` populated and CI-checked. ROADMAP §5.2 exit criteria all met.

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

1. **TPIDR_EL1 reset value is UNKNOWN per ARM ARM**. Pre-`thread_init` reads of `current_thread()` are undefined. P2-A's only callers post-date `thread_init`; defensively initializing TPIDR_EL1 in start.S would make pre-init reads deterministic-NULL but is held for explicit signoff (start.S edit).
2. **No guard page below `thread_create` kstacks**. The boot kthread (TID 0) inherits the boot stack which has a guard from P1-C-extras Part A; threads created via `thread_create` get a 16 KiB stack from `alloc_pages(2, KP_ZERO)` with NO guard page. Stack overflow in a P2-A kthread silently corrupts neighbor memory. **Phase 2 close** lands the per-thread guard page.
3. **No IRQ masking around `thread_switch`**. The only IRQ source live at v1.0 is the timer; its handler increments `g_ticks` and returns — does not touch thread state. Reentrancy is trivially safe at P2-A. **P2-B** adds the IRQ-mask discipline once scheduler-tick preemption becomes possible (timer IRQ → `thread_block` / dispatch).
4. **`thread_switch` unconditionally sets prev's state to `THREAD_RUNNABLE`**. Correct for "yield to next" semantics. For "block on a condition" (sleep), prev must be `THREAD_SLEEPING`. **P2-B** adds the separate `thread_block(cond)` primitive.
5. **`thread_trampoline` halts on entry-return (WFE)**. No `thread_exit` plumbing yet — if a thread's entry returns, the descriptor isn't reclaimed and the kstack remains allocated. Mitigated at P2-A: test entries don't return (call `thread_switch(kt0)` instead). **Phase 2 close** lands real `thread_exit` + reap.
6. **`scheduler.tla` is a sketch — `_buggy.cfg` deferred to P2-B**. The CLAUDE.md "executable documentation" pattern (primary `.cfg` clean + `_buggy.cfg` showing a specific invariant violation) is unfulfilled at P2-A because the relevant bug-classes (missed wakeups, deadline starvation, IPI reordering) are introduced by P2-B's actions. P2-B lands `scheduler_buggy.cfg` showing the missed-wakeup race when wait/wake atomicity is violated.
7. **`struct Context` size pinned at 112 bytes**. Adding a field (e.g., FPSIMD state at Phase 5 for userspace) requires updating `_Static_assert`s in `context.h` AND offsets / stp/ldp/str/ldr immediates in `context.S` together. The build break is loud but multi-step.
8. **`kstack_base` is PA-cast-to-void* at v1.0** (consistent with `kpage_alloc`). Phase 2 introduces the kernel direct map; both `kpage_alloc` and `thread_create` convert to high-VA pointers in the same chunk.
9. **`tools/test.sh` truncates the log to tail-20**. P2-A added kproc + kthread banner lines, pushing the KASLR offset line off the tail. `tools/verify-kaslr.sh` was updated to read `build/test-boot.log` directly instead of test.sh stdout. Future banner growth doesn't break the verifier; but a future debugger looking at test.sh stdout might miss earlier banner lines.
10. **`phys.alloc_smoke` + `phys.leak_10k` now drain magazines BEFORE baseline**. Phase 1's tests didn't need this — no SLUB allocations happened pre-test so the magazine was empty. P2-A's proc_init + thread_init each `kmem_cache_create` + `kmem_cache_alloc`, leaving ~5 pages resident in the order-0 magazine. Without the pre-drain, the post-drain comparison reads HIGHER than baseline by the resident count → false drift. The pre-drain is the durable fix.

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
