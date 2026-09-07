---
id: chg-2026-09-06-vma-addrspace-absorb
type: chg
title: "docs/reference retirement: absorb 26-vma + 146-addrspace -- fold gaps first (incl. a FACTUAL dossier bug: sub-kernel-vma claimed no test suite, test_vma.c has 6), then stub (50 absorbed / 107 live)"
date: 2026-09-06
arc: arc-vault
commits: ["7d1ecdfd"]
touched: [sub-kernel-vma, sub-kernel-addrspace]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The memory-area absorption batch, third and fourth files under the retirement
routing flip ([[chg-2026-09-06-docs-reference-retirement-flip]]; after 03-mmu
and 22-asid). Gap analysis was fanned out to an Explore agent (read-only,
gap-FINDER not completeness-confirmer); every flagged gap was verified against
the code before folding.

VERIFY-BEFORE-STUB CAUGHT A FACTUAL DOSSIER BUG, not just a coverage gap.
[[sub-kernel-vma]]'s Tests section asserted "There is no dedicated `vma.*`
suite; the structure is proven by its users" -- the OPPOSITE of reality:
`kernel/test/test_vma.c` exists (16987 bytes) with six tests
(alloc_free_smoke, alloc_constraints, insert_lookup_smoke,
insert_overlap_rejected, insert_sorted_invariant, drain_releases_all). A blind
stub-on-COVERED would have retired the legacy doc that listed them and left the
dossier asserting they do not exist. Corrected to the real suite; also folded
the half-open overlap semantic (`a < d && c < b`; adjacent ranges insert) the
dossier's Mechanism lacked.

[[sub-kernel-addrspace]] folds (verified in-tree): the mmu API deliberately
kept its bare `paddr_t pgtable_root` (7 such params confirmed in mmu.c) -- the
`pgtable_root == 0` sites split into kernel-Proc-test (-> `as == NULL`) vs
parameter-validation (stayed), so "finishing" the conversion into the mmu layer
would be wrong; the grep-rename footgun (page_count also on struct Burrow,
context_id also a psci_cpu_on param -- both confirmed -- so the L-1 move was
compiler-driven, delete-from-Proc-first); and a Tests section
(test_addrspace.c, 6 tests incl. proc_alloc_in_shares + share_drains_at_last_ref,
both verified present -- Explore under-listed them). D2's enumerated NULL-deref
sites were NOT folded: the loud-failure CONCEPT is already in the dossier and
the specific sites belong to their consumer dossiers (devctl/devproc), not here.

Stubs redirect single-owner (each doc's secondary-file mentions are cross-refs
owned elsewhere -- no orphan). "What it got wrong" per stub: 26-vma predates the
AddrSpace extraction (list-on-Proc -> list-on-AddrSpace, lock moved), COW, and
D-3; 146-addrspace is L-1-only (predates cow.c/clone/L-3 drain) with a pre-budget
`addrspace_alloc(void)`.

No code touched -- documentation curation of already-audited behavior, no audit
owed. Both dossiers updated -> 2026-09-06. view-absorption re-rendered: 48 -> 50
absorbed, 107 live.
