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

- **Tip**: P1-I closing audit landed at commit `*(pending)*` — Phase 1 is closed. P1-I-AB landed at `45f23fb` (10000-iter alloc/free leak checks, boot-time measurement, UBSan trapping, 10-boot KASLR script). P1-I-C landed at `a9e0caa` (deliberate-fault matrix: canary_smash + wxe_violation + bti_fault all confirmed firing under attack). P1-I-D + closing audit closed 2 P1 + 4 P2 + 3 P3 + 2 P3-deferred findings (audit_r3_closed_list.md): F29 phys_init reservation overlap, F30 spin_lock_irqsave DAIF, F31 mmu_map_device kernel-image rejection, F32 kfree pointer-boundary check, F33 kmem_cache_destroy full-slab tracking via new full_list, F34 explicit firmware reservation, F35 _Static_asserts on FDT_MAGIC + struct page size, F37 buddy_free order bound, F39 apply_relocations strict on non-RELATIVE. F36 + F38 deferred to post-v1.0 with rationale. Predecessor: `e8c9c5c` (P1-H + audit R2 close).: stack canaries (`-fstack-protector-strong` + `__stack_chk_guard` initialized from KASLR entropy), PAC return-address signing (`-mbranch-protection=pac-ret`; APIA key + SCTLR_EL1.EnIA from start.S), BTI indirect-branch guards (`-mbranch-protection=bti`; SCTLR_EL1.BT0 + PTE_GP on kernel text), LSE atomics permission (`-march=armv8-a+lse+pauth+bti`), stack-clash protection, NX stack. New `arch/arm64/hwfeat.{h,c}` reads ID_AA64ISAR0/1 + PFR1 to populate `g_hw_features` for banner reporting. CFI / MTE / `_FORTIFY_SOURCE` deferred with explicit rationale. Predecessor: `39eafb4` (P1-G + audit R1 close).
- **Phases**: Phase 0 done; **Phase 1 CLOSED**. P1-A through P1-I all landed; closing audit (round 3) prosecuted the entire boot path + hardening + memory subsystem and closed 11 findings. Phase 2 (process model + scheduler + handles + VMO + first formal specs) is next.
- **Tests**: **9/9 in-kernel tests PASS** every boot (kaslr.mix64_avalanche, dtb.chosen_kaslr_seed_present, phys.alloc_smoke, phys.leak_10k, slub.kmem_smoke, slub.leak_10k, gic.init_smoke, timer.tick_increments, hardening.detect_smoke). `tools/test.sh` PASS (production: 41 ms boot). `tools/test.sh --sanitize=ubsan` PASS (no UB triggered). `tools/verify-kaslr.sh -n 10` PASS (10/10 distinct offsets). `tools/test-fault.sh` PASS (3/3: canary, W^X, BTI all fire under attack).
- **Specs**: 0 written; 9 planned (Phase 2's `scheduler.tla` is first-mandatory).
- **LOC**: ~3120 C99 + ~440 ASM + ~75 linker-script + ~330 CMake/shell ≈ 3965 LOC.
- **Kernel ELF**: ~216 KB debug (production) / ~232 KB debug (UBSan). Flat binary: 29 KB. Page tables: 40 KiB BSS. struct page array: 24 MiB BSS. GIC handler table: 16 KiB BSS. Vector table: 2 KiB in `.text`.

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
| 05-process.md (planned) | Proc + Thread + rfork + notes + errstr | medium | Phase 2 |
| 06-scheduler.md (planned) | EEVDF + per-CPU + work-stealing | medium | Phase 2 |
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
