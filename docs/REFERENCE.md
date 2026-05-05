# Thylacine OS — Technical Reference

This document is the **as-built** reference for Thylacine OS. It describes what exists in the tree today, where the relevant code lives, which formal specs pin which invariants, and how the subsystems compose. It is NOT a roadmap and NOT a design document — see `docs/ARCHITECTURE.md` for design intent and `docs/ROADMAP.md` for phased scope.

---

## How to read this

The reference is split by subsystem, one file per layer of the stack. Each file follows the same template:

- **Purpose** — one paragraph on what the layer does and where it sits in the stack.
- **Public API** — every exported function with its contract.
- **Implementation** — structure + invariants + known caveats.
- **Spec cross-reference** — formal modules that pin invariants for this layer.
- **Tests** — which suites exercise the layer and what they cover.
- **Status** — what's implemented today vs. what's stubbed or deferred. Commit hashes cite the landing points.

When a section describes a detail enforced by a spec, the spec's action / invariant name is in `backticks`. When a section cites a file, the form is `path/to/file.c:line` so editors can jump there.

---

## Snapshot

- **Tip**: P2-Ca landed (SMP secondary bring-up via PSCI). Phase 2 in progress. Predecessor: `374c687` (P2-Bc hash fixup).
- **Phases**: Phase 0 done; Phase 1 CLOSED at `ceecb26`. **Phase 2 in progress.** P2-A landed (process-model bootstrap) + R4 audit closed. P2-Ba landed (scheduler dispatch). P2-Bb landed (wait/wake over Rendez). P2-Bc landed (scheduler-tick preemption + IRQ-mask discipline). **P2-Ca landed** (SMP secondary bring-up via PSCI — secondaries flip per-CPU online flag + park at WFI). Next: P2-Cb (per-CPU init: MMU + PAC + VBAR + TPIDR + sched_init); P2-Cc..Cg (per-CPU exception stacks + IPIs + work-stealing + finish_task_switch + spec); then P2-D..H. Full EEVDF math (vd_t = ve_t + slice × W_total / w_self) deferred — meaningful only with weight ≠ 1 which is Phase 5+.
- **Tests**: **18/18 in-kernel tests PASS** every boot (Phase 1's 9 + context.{create_destroy, round_trip} from P2-A + scheduler.{dispatch_smoke, runnable_count} from P2-Ba + scheduler.preemption_smoke from P2-Bc + rendez.{sleep_immediate_cond_true, basic_handoff, wakeup_no_waiter} from P2-Bb + smp.bringup_smoke from P2-Ca). `tools/test.sh` PASS (production: ~112 ms boot, 4-CPU QEMU with 3 secondaries via PSCI HVC). `tools/test.sh --sanitize=undefined` PASS. `tools/verify-kaslr.sh -n 5` PASS (5/5 distinct). `tools/test-fault.sh` PASS (3/3 protections fire under attack).
- **Specs**: 1 written (`scheduler.tla` — TLC-clean at 283 distinct states; `scheduler_buggy.cfg` produces canonical missed-wakeup counterexample at depth 4). P2-Ca touches no spec — PSCI bring-up is hardware/firmware coordination, not a kernel concurrency invariant. Cross-CPU spec work begins at P2-Cd (IPI ordering, ARCH §28 I-18). 9 planned. Phase 2 close adds LatencyBound liveness refinement + `namespace.tla` + `handles.tla`.
- **LOC**: ~5400 (P2-Bc) + ~150 PSCI primitives + ~140 smp.c + ~70 secondary_entry asm + ~150 DTB cpu/psci enumeration + ~50 test_smp.c + minor edits ≈ ~6000 LOC.
- **Kernel ELF**: ~280 KB debug (production) / ~300 KB debug (UBSan). Flat binary: ~48 KB. Page tables: 40 KiB BSS. struct page array: 24 MiB BSS. GIC handler table: 16 KiB BSS. Vector table: 2 KiB in `.text`. `g_cpu_online`: 8 bytes BSS.

For phase-level status see `docs/phaseN-status.md`. The reference below covers the as-built layers in bottom-up order. Per-subsystem files appear as their subsystems land during Phase 1 onward.

---

## Contents

| File | Layer | Size guide | Status |
|---|---|---|---|
| [00-overview.md](reference/00-overview.md) | Layer cake + cross-cutting concerns | medium | scaffolded |
| [01-boot.md](reference/01-boot.md) | Boot path: start.S (incl. Linux ARM64 image header), kernel.ld, MMU, KASLR | medium | **P1-A + P1-B landed**; extended at P1-C (MMU + KASLR) |
| [02-dtb.md](reference/02-dtb.md) | FDT parser: lib/dtb.c. Memory + compat-reg + kaslr-seed lookups. | medium | **P1-B landed**; kaslr-seed consumed at P1-C-extras |
| [03-mmu.md](reference/03-mmu.md) | MMU + W^X invariant I-12: arch/arm64/mmu.{h,c}; PTE constructors with _Static_asserts; TTBR0 identity + TTBR1 high-half (KASLR-aware) | medium | **P1-C + P1-C-extras landed**; SMP secondary bring-up at Phase 2 |
| [04-extinction.md](reference/04-extinction.md) | Kernel ELE — extinction(msg) + ASSERT_OR_DIE; EXTINCTION: ABI prefix per TOOLING.md §10 | small | **P1-C landed**; first deliberate caller at P1-F (fault handler) |
| [05-kaslr.md](reference/05-kaslr.md) | KASLR (invariant I-16): arch/arm64/kaslr.{h,c}; entropy chain (DTB kaslr-seed / rng-seed / cntpct fallback); .rela.dyn relocator; long-branch into TTBR1 | medium | **P1-C-extras Part B landed**; entropy bump (13 → 18 bits) is post-v1.0 hardening |
| [06-allocator.md](reference/06-allocator.md) | Physical allocator: struct page, mm/buddy.{h,c}, mm/magazines.{h,c}, mm/phys.{h,c} — DTB-driven bootstrap, buddy free lists, per-CPU magazines, public API | medium | **P1-D landed**; SLUB layered on alloc_pages at P1-E |
| [07-slub.md](reference/07-slub.md) | SLUB kernel object allocator: mm/slub.{h,c} — per-cache partial slab list, embedded freelist, kmalloc / kfree / kmem_cache_*, bootstrap via meta cache | medium | **P1-E landed**; multi-page slabs (>2 KiB caches) post-v1.0 |
| [08-exception.md](reference/08-exception.md) | Exception handling: arch/arm64/{vectors.S, exception.{h,c}} — 16-entry vector table, KERNEL_ENTRY save, sync handler with stack-overflow + W^X violation detection, exception_unexpected catch-all | medium | **P1-F landed**; per-CPU exception stack at Phase 2; GIC + IRQ at P1-G |
| [09-test-harness.md](reference/09-test-harness.md) | In-kernel test harness: kernel/test/{test.{h,c},test_*.c} — sentinel-terminated registry, TEST_ASSERT macro, per-test PASS/FAIL reporting; 6 leaf-API tests at v1.0 | small | **landed**; host-side sanitizer matrix + 10000-iteration leak check at P1-I |
| [10-gic.md](reference/10-gic.md) | GIC v3 driver: arch/arm64/gic.{h,c} — DTB v2/v3 autodetect, distributor + redistributor + sysreg CPU interface, IRQ enable/disable/ack/eoi, handler dispatch table | medium | **P1-G landed**; v2 path + ITS + SMP redist walk deferred |
| [11-timer.md](reference/11-timer.md) | ARM generic timer: arch/arm64/timer.{h,c} — EL1 non-secure phys at 1000 Hz on PPI 14 (INTID 30), CNTP_TVAL_EL0 reload pattern, WFI-based busy wait | small | **P1-G landed**; oneshot + per-CPU SMP at Phase 2 |
| [12-hardening.md](reference/12-hardening.md) | Hardening enablement: toolchain flags, kernel/canary.c (`__stack_chk_guard` + `__stack_chk_fail`), arch/arm64/start.S PAC keys + SCTLR enable, arch/arm64/hwfeat.{h,c} ID-register inspection, PTE_GP for BTI on kernel text | medium | **P1-H landed**; CFI / MTE / `_FORTIFY_SOURCE` deferred with explicit rationale |
| [13-verification.md](reference/13-verification.md) | Phase 1 verification infrastructure: 10000-iter alloc/free leak checks, boot-time measurement, multi-boot KASLR variability, UBSan trapping build, deliberate-fault matrix (canary/W^X/BTI) | medium | **P1-I landed**; PAC deliberate-fault + KASAN deferred with rationale |
| [14-process-model.md](reference/14-process-model.md) | Process model bootstrap: struct Proc + struct Thread + struct Context + cpu_switch_context (asm) + thread_trampoline + thread_switch wrapper. TPIDR_EL1 = current_thread per CPU. SLUB caches for proc + thread. specs/scheduler.tla sketch (TLC-clean). | medium | **P2-A landed**; EEVDF math + wait/wake at P2-Bb/c; work-stealing at P2-C |
| [15-scheduler.md](reference/15-scheduler.md) | Scheduler dispatch: sched() + ready() + 3-band run tree (sorted by vd_t doubly-linked list) + per-CPU current via TPIDR_EL1 + struct Thread EEVDF fields. Simplified EEVDF (monotonic vd_t advance); full math at P2-Bc. | medium | **P2-Ba landed**; sched() refactored at P2-Bb to respect prev->state; preemption + EEVDF math at P2-Bc |
| [16-rendez.md](reference/16-rendez.md) | Plan 9 wait/wake (sleep + wakeup over Rendez): kernel/include/thylacine/rendez.h + sleep/wakeup impl in kernel/sched.c. spin_lock_irqsave for IRQ-mask discipline; single-waiter convention; cond-check + state-transition atomic under r->lock. Maps scheduler.tla WaitOnCond/WakeAll. | medium | **P2-Bb landed**; multi-waiter wait queues at Phase 5 (poll/futex) |
| [17-smp-bringup.md](reference/17-smp-bringup.md) | SMP secondary bring-up via PSCI: lib/dtb.c cpu/psci enumeration + arch/arm64/psci.{h,c} HVC/SMC + arch/arm64/start.S secondary_entry asm trampoline + kernel/smp.{h,c} smp_init. Minimal at P2-Ca: secondaries flip per-CPU online flag + WFI; no MMU/PAC/vectors yet. | medium | **P2-Ca landed**; per-CPU MMU+PAC+vectors+sched at P2-Cb; IPIs at P2-Cd |
| 07-namespace.md (planned) | Pgrp + bind + mount | small | Phase 2 |
| 08-handles.md (planned) | KObj_* + rights + transfer-via-9P | medium | Phase 2 |
| 09-vmo.md (planned) | VMO manager + zero-copy + lifecycle | small | Phase 2-3 |
| 10-dev.md (planned) | Dev vtable + Chan + kernel-internal Devs | medium | Phase 3 |
| 11-virtio.md (planned) | VirtIO transport + userspace driver model | medium | Phase 3 |
| 12-9p.md (planned) | 9P client (pipelined) + Stratum extensions | medium | Phase 4 |
| 13-syscall.md (planned) | Syscall surface + Linux shim | medium | Phase 5 |
| 14-poll-futex.md (planned) | poll, futex, rendezvous | small | Phase 5 |
| 15-pty.md (planned) | PTY infrastructure + termios | small | Phase 5 |
| 16-network.md (planned) | smoltcp / Plan 9 IP / VirtIO-net userspace | medium | Phase 6 |
| 17-container.md (planned) | thylacine-run + namespace construction | small | Phase 6 |
| 18-halcyon.md (planned) | Scroll buffer + framebuffer + image + video | medium | Phase 8 |

---

## Document maintenance

When a chunk lands (bug fix, refactor, new module), the author is responsible for:

1. Updating the relevant `reference/NN-*.md` section(s).
2. Refreshing the [Snapshot](#snapshot) figures.
3. If the chunk introduces or refutes an invariant, updating the spec catalog entry.
4. If a new term or acronym enters the lexicon, updating the glossary.

Reference sections are PR-first like any code change; the audit policy in `CLAUDE.md` extends here: a change to a documented invariant updates the spec FIRST, then the reference, then the code. If the three disagree, the spec wins.

---

## Glossary

See `ARCHITECTURE.md §29` for the definitive glossary. As implementation lands and new terms enter the lexicon, they're appended to that glossary; this reference points to it rather than duplicating.

---

## Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Scaffolded (Phase 0 complete). | Template for the as-built reference. Per-subsystem files (`reference/01-...md`, etc.) appear as subsystems land during Phase 1 onward. |
| 2026-05-04 | P1-G landed: 10-gic.md + 11-timer.md added; snapshot bumped (6/6 tests, 3415 LOC, 190 KB ELF). 08-exception.md updated for live IRQ slot + `.Lexception_return` trampoline. | GIC v3 + ARM generic timer chunk closed; the kernel now receives 1000 Hz ticks. |
| 2026-05-04 | P1-H landed: 12-hardening.md added; snapshot bumped (7/7 tests, 3685 LOC, 206 KB ELF). 03-mmu.md (PTE_GP) + 05-kaslr.md (canary_init slot) + 01-boot.md (PAC + canary entry sequence) updated. | v1.0 hardening commitment closed: canaries + PAC + BTI + LSE + stack-clash + NX stack. CFI / MTE / FORTIFY deferred with rationale. |
| 2026-05-04 | **P1-I landed: Phase 1 CLOSED.** 13-verification.md added; snapshot bumped (9/9 tests, ~3965 LOC, ~216 KB ELF). All ROADMAP §4.2 exit criteria met (boot < 500 ms; 10000-iter leak; KASLR 10/10 distinct; UBSan clean; canary/W^X/BTI fire under attack). Audit round 3 closed 9 findings + 2 deferred. | Foundation phase complete. Phase 2 (process model + scheduler + handles + VMO + first formal specs) opens. |
| 2026-05-05 | **P2-A landed.** 14-process-model.md added; snapshot bumped (11/11 tests, ~4330 LOC, ~227 KB ELF). struct Proc + struct Thread + struct Context + cpu_switch_context + thread_trampoline + thread_switch + context.create_destroy / context.round_trip tests + specs/scheduler.tla sketch (TLC-clean). | Phase 2 opens with the first multitasking primitives. Pre-EEVDF — direct switches only. P2-B layers the scheduler. |
| 2026-05-05 | **P2-A R4 audit close**: 1 P1 + 3 P2 + 5 P3 fixed; 1 P3 deferred (U-30). KP_ZERO on proc/thread alloc; magic guards (struct Proc 16→24 B, struct Thread 160→168 B); BTI BTYPE comments corrected; THREAD_STATE_INVALID guards; TPIDR_EL0/EL1 init in start.S; _Static_asserts pinned. Plus scheduler.tla extended with wait/wake atomicity (NoMissedWakeup invariant; scheduler_buggy.cfg counterexample). | Defensive measures + spec refinement land before P2-B impl. |
| 2026-05-05 | **P2-Ba landed.** 15-scheduler.md added; snapshot bumped (13/13 tests, ~4790 LOC, ~242 KB ELF). struct Thread extended (168→200 B) with vd_t/weight/band/runnable_{next,prev}. sched.h + sched.c + 2 new tests. sched_init wired in main.c. | Plan 9 sched() + ready() idiom layer over a simplified EEVDF (monotonic vd_t). 3 priority bands. Full EEVDF math at P2-Bc. |
| 2026-05-05 | **P2-Bb landed.** 16-rendez.md added; snapshot bumped (16/16 tests, ~5200 LOC, ~253 KB ELF). New rendez.h + sleep/wakeup impl in sched.c + test_rendez.c (3 tests). struct Thread extended (200→208 B) with rendez_blocked_on. sched() refactored to respect pre-set prev->state (RUNNING → yield-insert; SLEEPING/EXITING → don't insert; defends against SMP race with prev==next check). | Plan 9 wait/wake protocol implemented against the scheduler.tla `WaitOnCond` + `WakeAll` actions. Single-waiter Rendez convention. spin_lock_irqsave for IRQ-mask discipline. Multi-waiter wait queues (poll, futex) at Phase 5. |
| 2026-05-05 | **P2-Bc landed.** 15-scheduler.md extended with preemption section; snapshot bumped (17/17 tests, ~5400 LOC, ~255 KB ELF). New `sched_tick()` + `preempt_check_irq()` in sched.c + scheduler.preemption_smoke test. struct Thread extended (208→216 B) with slice_remaining (s64). vectors.S IRQ slot inserts `bl preempt_check_irq` after exception_irq_curr_el (28 of 32 instructions). thread_trampoline `msr daifclr, #2` to unmask IRQs at first thread entry. `sched()` and `ready()` use `spin_lock_irqsave(NULL)` for IRQ-mask discipline (closes P2-A trip-hazard #3). Spec docs updated (no model changes); SPEC-TO-CODE.md maps preempt path to existing Yield action. | Timer-IRQ-driven preemption live: CPU-bound threads share the CPU under round-robin slice rotation. Full EEVDF math + LatencyBound liveness deferred — current impl is a valid instantiation for weight=1 (which is v1.0). |
| 2026-05-05 | **P2-Ca landed.** 17-smp-bringup.md added; snapshot bumped (18/18 tests, ~6000 LOC, ~280 KB ELF). DTB cpu/psci enumeration (`dtb_cpu_count`/`dtb_cpu_mpidr`/`dtb_psci_method`); arch/arm64/psci.{h,c} for HVC/SMC SMCCC; secondary_entry asm trampoline in start.S; kernel/smp.{h,c} with smp_init. mmu_enable refactored into mmu_program_this_cpu + mmu_enable for future per-CPU re-program at P2-Cb. New banner `smp:` line. New test smp.bringup_smoke. Boot now reports 4/4 cpus online via PSCI HVC on QEMU virt. | First SMP step. Secondaries are alive but inert — they flip online flag + WFI. No MMU/PAC/vectors per-CPU yet. P2-Cb adds full per-CPU init bringing them into the scheduler. P2-C split into Ca-Cg sub-chunks for tractability (Ca landed; Cb..Cg open). |
