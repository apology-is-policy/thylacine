---
id: inv-i12
type: inv
title: "I-12 — no page is ever writable and executable at once"
number: I-12
guards: [sub-kernel-mmu, sub-kernel-vma, sub-kernel-fault, sub-kernel-elf, sub-kernel-exec, sub-kernel-syscall-dispatch]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-03
updated: 2026-10-27
---
## Statement

Every page-table entry the kernel installs is writable **or** executable,
never both — at either exception level. A page that can be written cannot be
fetched from; a page that can be fetched from cannot be written.

The "at either level" clause is load-bearing and was once wrong in the tree's
own checker: a user PTE always sets `PXN` (the kernel never executes user
pages), so an EL1-only test reads *every* user page as non-executable and would
pass a writable, user-executable page. The two legs are separate bits — `PXN`
for EL1, `UXN` for EL0 — and W^X means neither may be clear while the AP bits
say writable.

## Why it is the one memory invariant with its own number

W^X is what makes a memory-corruption bug stay a memory-corruption bug. Without
it, any primitive that writes attacker-controlled bytes into a mapped page is
one jump away from executing them. Every other defence in the tree — the
canaries, KASLR, the guard pages — raises the cost of getting there; this one
removes the destination.

## Enforcement, as it actually is

Six mechanisms, and it is worth being precise about which does what, because
the scripture's list was not accurate for twelve weeks (see Caveats).

**1. The VMA gate — the single user-side chokepoint.** `vma_alloc` rejects
`WRITE|EXEC` outright, and it is the *only* place a user mapping is born:
`burrow_map` is `vma_alloc`'s sole production caller, and the three
`mmu_install_user_pte` call sites are all in the fault handler, all passing
`vma->prot`. So every user PTE's permission bits trace to one `if` statement.
`vma_alloc` also rejects write-without-read, because AArch64 has no write-only
AP encoding and a W-only request would silently map readable.

**2. The kernel PTE composites.** `PTE_KERN_TEXT` (RO + PXN clear),
`PTE_KERN_RO` and `PTE_KERN_RW` (both PXN set) are constructed so the
combination cannot arise, and fifteen `_Static_assert`s pin the bits of all
seven composites: read-only or `PXN` on each, and `UXN` on all seven. A
refactor that made kernel text writable, or any kernel memory executable at EL0,
fails the build.

**3. The ELF loader.** A `PT_LOAD` segment requesting both W and X is rejected
at load, so a hostile or broken binary cannot ask for it.

**4. The self-patcher's transient alias.** The boot-time LSE patcher writes
kernel `.text` through a scratch mapping that is RW + PXN + UXN, while the
canonical mapping stays RO + X. Two aliases of one physical page, each
W^X-clean; no PTE is ever both, not even momentarily. The JIT surface (I-42)
generalizes exactly this shape outward to userspace.

**5. The protect surface never targets X — the structural absence became a
stated refusal at B-1a (2026-09-23).** Until B-1a there was no
protection-changing syscall at all, and this mechanism was that absence.
`SYS_BURROW_PROTECT` (and the phenotype `mprotect` row over it) now moves a
mapping's `prot` among {none, R, RW} under a mint-time ceiling, and EXEC is
outside the target set at BOTH refusal sites: `burrow_prot_word_check` at the
syscall boundary (`-EACCES`, before any lookup -- observable, since
`protect(X)` over an unmapped range is EACCES where `protect(R)` is ENOMEM)
and `vma_reprotect_precheck_in` in the mechanism, so no in-kernel caller
reaches an RX target by a path the boundary did not see
([[sub-kernel-syscall-dispatch]], [[sub-kernel-vma]]). Nothing raises a
ceiling, so an RX mapping -- image text -- can only descend to R or none and
never returns. The gate count is unchanged: `vma_alloc` remains the single
place where a user mapping's W and X are decided together, the protect never
adds X to anything, and the one path by which bytes a Proc wrote become
executable is still `CAP_JIT`'s dual map (I-42, the JIT capability). Witnesses:
`protect.x_refused_before_lookup`, `protect.ceiling_bounds_raise`, the
`noxcheck` sabotage (both refusals removed -> exactly the three X assertions
fail and nothing else), and `/protect-probe`'s fifth leg.

**6. The leaf check -- fail-closed, at both levels (operator vote 22,
2026-10-27).** Every leaf descriptor the kernel builds, kernel or user, page or
block, comes from one of four encoders in `arch/arm64/mmu.c`, and each passes
its value through `pte_violates_wxe` before a table can hold it: writable with
`PXN` or `UXN` clear is an extinction (`PTE violates W^X at install (kernel L1
block)`, `(kernel L2 block)`, `(kernel page)` or `(user page)`). It is a
backstop beneath the five above, not a gate -- nothing reaches it today. Where it
is decided differs by leaf. Every kernel caller passes a constant composite, so
the compiler decides the kernel leaves: a clean composite folds the check away
(neither shipped kernel contains the kernel bodies), and one edited past its
asserts folds to an unconditional extinction wherever it is used. The user
encoder's prot is a run-time value, so its check runs on every user leaf and
reads the bits it is about to return. Either way a regression in the others
stops the kernel instead of mapping the page. Witnesses: `mmu.wxe_predicate` (the predicate over every AP x PXN x UXN
combination, written out), and `tools/test-fault.sh`'s four `wxe_install_*` variants, one per encoder,
which have it build a W+X leaf and pass only on its extinction body (their
provokers pass constants too, so they prove each encoder's wiring in the source,
not a run-time decision). It holds by convention in one respect: a leaf composed by
hand would skip it, and three were (block leaves from clean composites,
converted in the same change; [[sub-kernel-mmu]] Seams).

## Caveats

**Until 2026-10-27 the scripture credited a function that enforced nothing.** `pte_violates_wxe`
is a correct W^X predicate with **zero callers** anywhere in the tree. It is
named first in `ARCHITECTURE.md`'s I-12 validation cell, counted as one of
"3 legs" by the holotype consistency pass, described by
`docs/reference/12-hardening.md` as continuing "to enforce", and given a row in
`03-mmu.md`'s error-path table that reads *"`pte_violates_wxe()` returns true;
callers `extinction(...)`"* — describing the behaviour of callers that do not
exist. A handoff document states it is used by `exception_sync_curr_el`; the
kernel's actual W^X diagnostic is a permission-fault-plus-address-range test
that never inspects a PTE.

The invariant held anyway, on mechanisms 1 to 5. What was wrong was the
**inventory**: an enforcement list that named a dormant function ahead of the
`vma_alloc` line doing the work. The task filed for it (#59) lost its number to
a later reuse and had no owner for twelve weeks; operator vote 22 chose to wire
the function rather than delete it (mechanism 6), so the scripture cell,
`12-hardening.md`'s "continues to enforce" and `03-mmu.md`'s error-path row are
now true. The handoff's `exception_sync_curr_el` claim is still false: that
path's W^X diagnostic is the permission-fault-plus-address-range test. Full
chain: [[chg-2026-08-03-mapping-core-sweep]].

**A sixth document names a syscall that did not exist.** `kernel/elf.c`'s file
header calls the loader "one of three layers (PTE bits + mprotect + ELF
loader)". When this was written there was no `mprotect` in this kernel —
searching `kernel/`, `arch/` and `mm/` for it returned exactly one hit, that
comment — and mechanism 5 was an **absence**. Since B-1a (2026-09-23) there IS
a protect syscall, and it is still not a W^X layer: it cannot produce X, so
the sentence is wrong in a new way rather than right. Listing it as a layer
alongside two real checks turns a strength into a phantom, and the same
sentence omits `vma_alloc` — making [[sub-kernel-elf]] the sixth document to
do so. Folded into task #59.

The pattern across all six is worth stating once: **every document that
enumerated this invariant's enforcement named something that could not fire,
and none named the single line that always does.** The enumerations were
written from the design and never re-derived from the code.

**The encoders did not judge until vote 22.** `make_user_pte_l3` given
`WRITE|EXEC` emitted a writable, user-executable PTE, while a comment on the
JIT fault arm said "the W^X decision therefore stays entirely in
`make_user_pte_l3`" -- false in the one place it mattered, since the JIT is the
surface that deliberately holds two mappings of one code region (B-2a rewrote
the comment to name `vma_alloc`). The encoder now extincts on that leaf
(mechanism 6), but the decision is still `vma_alloc`'s: the check fires only on
a request the gate should already have refused.

## Where it is enforced

[[sub-kernel-vma]] (the gate, and the mechanism's refusal of X as a protect
target) · [[sub-kernel-mmu]] (the encoders and their leaf check, the asserts, the patcher's alias) ·
[[sub-kernel-fault]] (every install passes `vma->prot` through unchanged) ·
[[sub-kernel-syscall-dispatch]] (the boundary's refusal of X, before any
lookup).
