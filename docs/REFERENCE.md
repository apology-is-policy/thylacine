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

- **Tip**: P1-F landed at commit `(pending hash-fixup)` — exception vector table + sync handler with stack-overflow / W^X-violation detection. Phase 1 momentum: P1-A → P1-F complete. Invariants I-12 (W^X), I-15 (DTB-driven HW discovery), and I-16 (KASLR) all live; the deferred fault paths from P1-C and P1-C-extras Part A finally close.
- **Phases**: Phase 0 done; Phase 1 in progress (P1-A through P1-F complete; P1-G ARM generic timer next, OR P1-F-extras GIC + IRQ-driven UART before that).
- **Tests**: 0 unit tests; 1 integration check (`tools/test.sh` boot-banner verify). PASS. Boot smoke tests exercise both phys + SLUB; 5+ consecutive boots produce distinct KASLR offsets and successful smokes; vector table verified by disassembly + readelf. Test harness lands in P1-I.
- **Specs**: 0 written; 9 planned.
- **LOC**: ~2080 C99 + ~370 ASM + ~75 linker-script + ~220 CMake/shell ≈ 2745 LOC.
- **Kernel ELF**: ~165 KB debug. Flat binary: 16 KB. Page tables: 40 KiB BSS. struct page array: 24 MiB BSS. Vector table: 2 KiB in `.text`.

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
| 07-irq.md (planned) | GIC + exception vectors + IPI | small | Phase 1-2 |
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
