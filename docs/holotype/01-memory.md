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
1. **ASID generation-rollover** (B-F1 proper fix) — IN PROGRESS:
   - **`specs/asid.tla` LANDED `@4fe50f7`** (model-first): clean cfg TLC-GREEN
     (52033 states, 5 invariants) + 4 buggy cfgs confirmed (rollover_steals_active
     -> ActiveClaimed; fast_no_regen -> NoActiveAlias; no_flush_pending +
     fast_no_flush_check -> NoStaleTLB). The clean run SURFACED a design point:
     the fast path needs TWO guards (gen-match AND ~flush_pending / Linux's
     `old_active_asid != 0`); the impl carries both.
   - **impl LANDED `@d742ffa`**: asid.c/h rewrite (asid_resolve fast/slow +
     new_context + flush_context + check_update_reserved); `u64 context_id` on
     Proc (sizeof 264 unchanged); the `sched_install_asid_ttbr0` context-switch
     pre-hook at both cpu_switch_context sites; TCR_EL1.AS from ASIDBits; drop
     asid_alloc/asid_free; teardown TLB-safe via vma_drain's vaae1is +
     flush_pending (no per-Proc flush). **default 806/806 PASS, 0 EXTINCTION,
     clean boot -smp4.** SMP gate on d742ffa: **PASS -- 0 CORRUPTION**
     across default+UBSan x smp4/smp8 (N=10 each; timing classifications =
     benign #894 host-load, expected with the reviewer running concurrently).
   - **Self-audit (Opus) found**: SA-1 [P2] asid.c:172 non-atomic
     `g_asid_generation +=` racing the lockless gen_match load (-> __atomic_store);
     SA-2 [P3] g_asid_rollovers++ vs the lockless diagnostic load; SA-3 [P3/doc]
     per-CPU ASIDBits uniformity assumption. All HELD to merge with the Fable
     reviewer's findings, fix AFTER the SMP gate, re-gate, then close.
   - **Fable `holotype-reviewer` audit IN FLIGHT** on d742ffa (the full SMP-race
     prosecution). NEXT: merge findings + SA-1/2/3, fix, re-build/test/gate,
     append `audit_holotype_rw1_closed_list.md`, then the remaining items below.
   Design: ARCH §6.2.1 + `memory/project_asid_rollover_design.md`.
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
