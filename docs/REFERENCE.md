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

- **Tip**: P2-A landed (process-model bootstrap). Phase 2 is open; the kernel now describes processes, threads, and saved register contexts and can switch CPU control between two threads via `cpu_switch_context` + `thread_switch`. `specs/scheduler.tla` sketch lands TLC-clean. Predecessor: `ceecb26` (P1-I hash fixup + handoff 005 — Phase 1 CLOSED).
- **Phases**: Phase 0 done; Phase 1 CLOSED at commit `ceecb26`. **Phase 2 in progress.** P2-A (process-model bootstrap) is the first sub-chunk; P2-B (EEVDF scheduler + wait/wake) and P2-C (work-stealing) follow.
- **Tests**: **11/11 in-kernel tests PASS** every boot (Phase 1's 9 + context.create_destroy + context.round_trip from P2-A). `tools/test.sh` PASS (production: ~42 ms boot). `tools/test.sh --sanitize=undefined` PASS (no UB triggered). `tools/verify-kaslr.sh -n 5` PASS (5/5 distinct offsets). `tools/test-fault.sh` PASS (3/3: canary, W^X, BTI all fire under attack).
- **Specs**: 1 written (`scheduler.tla` sketch — TLC-clean at 99 distinct states with Threads={t1,t2,t3}, CPUs={c1,c2}); 9 planned. P2-B refines `scheduler.tla` (EEVDF math + wait/wake atomicity + IPI ordering); P2-C adds work-stealing fairness; Phase 2 close adds `namespace.tla` + `handles.tla`.
- **LOC**: ~3120 C99 + ~440 ASM + ~75 linker-script + ~330 CMake/shell + ~290 process model (P2-A) + ~75 spec (P2-A) ≈ 4330 LOC.
- **Kernel ELF**: ~227 KB debug (production) / ~248 KB debug (UBSan). Flat binary: ~37 KB. Page tables: 40 KiB BSS. struct page array: 24 MiB BSS. GIC handler table: 16 KiB BSS. Vector table: 2 KiB in `.text`.

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
| [14-process-model.md](reference/14-process-model.md) | Process model bootstrap: struct Proc + struct Thread + struct Context + cpu_switch_context (asm) + thread_trampoline + thread_switch wrapper. TPIDR_EL1 = current_thread per CPU. SLUB caches for proc + thread. specs/scheduler.tla sketch (TLC-clean). | medium | **P2-A landed**; EEVDF + wait/wake at P2-B; work-stealing at P2-C |
| 15-scheduler.md (planned) | EEVDF + wait/wake + work-stealing + IPI | medium | P2-B / P2-C |
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
