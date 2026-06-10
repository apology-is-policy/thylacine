# HOLOTYPE RW-1 — Memory (FULL)

**Status: IN PROGRESS** (the ASID rollover sub-chunk is mid-flight; finalize
this report + close at the RW-1 close). Full finding catalog + dispositions:
`docs/holotype/00-register.md` (HT01.*). Closed-list (do-not-re-report,
written at close): `memory/audit_holotype_rw1_closed_list.md`.

## Scope + method
Sub-surfaces: A = `mm/phys+buddy+magazines+slub`; B = `arch/arm64/mmu.c` +
`alternatives.c` + `asid.{c,h}` (W^X/I-12/I-13/TLBI/BBM); C = `kernel/burrow.c`
+ `vma.c` + `arch/arm64/fault.c` (I-7 dual-refcount, vma_lock, demand-page).
Two passes per sub-surface: a first default-effort Fable pass, then an
**authoritative max-effort `holotype-reviewer` (Fable) pass** — all three
self-reported `MODEL: Claude Fable 5 (claude-fable-5)` (no mid-run fallback).
Plus an Opus seam self-review. Reports: `/tmp/holotype-rw1-{A,B,C}.md` +
`-{A,C}-max.md` (B-max returned inline). **The authoritative pass earned its
keep: it verified the first-round fixes SOUND *and* found a new P2 (A-F-S1) +
graded the two P1s.**

Ground-truth correction folded in: CLAUDE.md / ARCH §25.4 referenced
`mm/vm.c` + `mm/wxe.c` + `mm/vmo_pages.c` + COW — none exist; W^X is enforced
at the VMA layer + ELF loader + PTE construction, and COW is a properly-recorded
unbuilt seam (ARCH §6.5; RFMEM extincts). (The stale audit-table file refs are
a doc-fold owed at close.)

## Verdict
**The memory surface is fundamentally sound — 0 P0 across all three
sub-surfaces.** Both P1s were *availability* defects (whole-kernel DoS), not
corruption. Extensive verified-sound lists (see the register's RW-1 footer).

## Fixes landed (this arc)
- `baea64e` — A-F1 kmalloc size-wrap guard + A-F2 cache-list lock (2× P2).
- `5b68210` — A-F-S1 slab destroy partial-slab UAF guard (P2) + F-S2/F-S3 (P3).
- `0e9a4c3` — B-F4 map_mmio overflow + B-F5 pte_violates_wxe UXN + B-F2 stale comments (3× P3).
- `2891bf2` — **C-F1** multi-thread-Proc fault no longer extincts (P1) + `/thread-fault-probe`.
- `ef29456` — **B-F1 fail-soft** interim: ASID exhaustion fails the spawn, not the kernel (P1).
- `83bd74e` — **B-F1 scripture**: ASID generation-rollover design (ARCH §6.2.1 + I-31 + audit rows). No code.

## OWED before the RW-1 close (the close-checklist)
1. **ASID generation-rollover** (B-F1 proper fix) — the remaining sub-chunk:
   `specs/asid.tla` (model-first: clean + the `rollover_steals_active` buggy
   cfg, TLC-green) → impl (asid.c rewrite + `_Atomic u64 context_id` on Proc +
   the `asid_check_and_switch` context-switch pre-hook + TCR.AS=1 from ASIDBits;
   drop asid_alloc-at-create / asid_free-at-reap) → focused `holotype-reviewer`
   audit. Design: ARCH §6.2.1 + `memory/project_asid_rollover_design.md`.
2. **C-F2 [P2]** — add the `p->vma_lock` precondition to the `burrow_map` /
   `burrow_unmap` header doc blocks (`burrow.h:258-300`). Trivial.
3. **C-F3 [P3]** — `vma_alloc` reject `WRITE && !READ` (mirror syscall.c:391/544).
4. **C-F4 [P3]** — `burrow_acquire_mapping`: move the both-counts-zero guard +
   liveness read inside `v->lock` (mirror `burrow_ref`).
5. **Doc-folds**: `03-mmu.md` (B-F3) + `20-burrow.md` (C-F6) — both actively
   misdescribe post-fix behavior; refresh.
6. **Register/perf**: the RW-1 perf/SOTA rows (A-H/T, B-F6, C-F5/F7/F8/F9) are
   already in the register, REGISTERED for RW-11/RW-12 — no in-arc fix.
7. **Close**: finalize this report, write `audit_holotype_rw1_closed_list.md`,
   add the RW-1 status row to `docs/HOLOTYPE.md` §10, run the SMP gate
   (default+UBSan × smp4/smp8) over the landed death-path + ASID changes.

## Seam self-review (Opus, parallel to the reviewers) — verified sound
Reap ordering (vma_drain → asid_free → pgtable_destroy; pages freed at drain
are architecturally unreachable); table-page KP_ZERO+dsb; fault-vs-detach both
under `vma_lock`; Loom pin vs detach (I-7 holds the pages); allocator IRQ-safety
(all irqsave; no IRQ handler allocates); exec is not a vma_drain caller.
Self-findings folded into the register (the stale mm/vm.c audit-map refs =
B-F3-adjacent doc-fold; the vma_alloc burrow_offset alignment is structurally-0
at v1.0).
