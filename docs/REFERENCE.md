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

- **Tip**: (Phase 0 complete; Phase 1 not yet started)
- **Phases**: Phase 0 done; Phase 1 entry pending.
- **Tests**: 0 (no implementation yet).
- **Specs**: 0 written; 9 planned.
- **LOC**: 0 (no implementation yet).

For phase-level status see `docs/phaseN-status.md`. The reference below covers the as-built layers in bottom-up order. Per-subsystem files appear as their subsystems land during Phase 1 onward.

---

## Contents

| File | Layer | Size guide | Status |
|---|---|---|---|
| [00-overview.md](reference/00-overview.md) | Layer cake + cross-cutting concerns | medium | scaffolded |
| 01-boot.md (planned) | Boot path: start.S, MMU, KASLR | small | Phase 1 |
| 02-memory.md (planned) | Buddy + magazines + SLUB + VMAs + W^X | medium | Phase 1-2 |
| 03-irq.md (planned) | GIC + exception vectors + IPI | small | Phase 1-2 |
| 04-process.md (planned) | Proc + Thread + rfork + notes + errstr | medium | Phase 2 |
| 05-scheduler.md (planned) | EEVDF + per-CPU + work-stealing | medium | Phase 2 |
| 06-namespace.md (planned) | Pgrp + bind + mount | small | Phase 2 |
| 07-handles.md (planned) | KObj_* + rights + transfer-via-9P | medium | Phase 2 |
| 08-vmo.md (planned) | VMO manager + zero-copy + lifecycle | small | Phase 2-3 |
| 09-dev.md (planned) | Dev vtable + Chan + kernel-internal Devs | medium | Phase 3 |
| 10-virtio.md (planned) | VirtIO transport + userspace driver model | medium | Phase 3 |
| 11-9p.md (planned) | 9P client (pipelined) + Stratum extensions | medium | Phase 4 |
| 12-syscall.md (planned) | Syscall surface + Linux shim | medium | Phase 5 |
| 13-poll-futex.md (planned) | poll, futex, rendezvous | small | Phase 5 |
| 14-pty.md (planned) | PTY infrastructure + termios | small | Phase 5 |
| 15-network.md (planned) | smoltcp / Plan 9 IP / VirtIO-net userspace | medium | Phase 6 |
| 16-container.md (planned) | thylacine-run + namespace construction | small | Phase 6 |
| 17-halcyon.md (planned) | Scroll buffer + framebuffer + image + video | medium | Phase 8 |

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
