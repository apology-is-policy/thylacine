---
id: inv-i12
type: inv
title: "I-12 — no page is ever writable and executable at once"
number: I-12
guards: [sub-kernel-mmu, sub-kernel-vma, sub-kernel-fault, sub-kernel-elf, sub-kernel-exec]
validated-by: [prose, gate-smp]
strength: prose
created: 2026-08-03
updated: 2026-08-03
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

Five mechanisms, and it is worth being precise about which does what, because
the scripture's own list is not accurate (see Caveats).

**1. The VMA gate — the single user-side chokepoint.** `vma_alloc` rejects
`WRITE|EXEC` outright, and it is the *only* place a user mapping is born:
`burrow_map` is `vma_alloc`'s sole production caller, and the three
`mmu_install_user_pte` call sites are all in the fault handler, all passing
`vma->prot`. So every user PTE's permission bits trace to one `if` statement.
`vma_alloc` also rejects write-without-read, because AArch64 has no write-only
AP encoding and a W-only request would silently map readable.

**2. The kernel PTE composites.** `PTE_KERN_TEXT` (RO + PXN clear),
`PTE_KERN_RO` and `PTE_KERN_RW` (both PXN set) are constructed so the
combination cannot arise, and seven `_Static_assert`s pin the bits. A refactor
that made kernel text writable fails the build.

**3. The ELF loader.** A `PT_LOAD` segment requesting both W and X is rejected
at load, so a hostile or broken binary cannot ask for it.

**4. The self-patcher's transient alias.** The boot-time LSE patcher writes
kernel `.text` through a scratch mapping that is RW + PXN + UXN, while the
canonical mapping stays RO + X. Two aliases of one physical page, each
W^X-clean; no PTE is ever both, not even momentarily. The JIT surface (I-42)
generalizes exactly this shape outward to userspace.

**5. Structural absence.** There is no protection-changing syscall. Nothing can
flip an existing mapping from writable to executable, because the operation
does not exist — which is stronger than rejecting it, since there is no call to
get wrong.

## Caveats

**The scripture credits a function that enforces nothing.** `pte_violates_wxe`
is a correct W^X predicate with **zero callers** anywhere in the tree. It is
named first in `ARCHITECTURE.md`'s I-12 validation cell, counted as one of
"3 legs" by the holotype consistency pass, described by
`docs/reference/12-hardening.md` as continuing "to enforce", and given a row in
`03-mmu.md`'s error-path table that reads *"`pte_violates_wxe()` returns true;
callers `extinction(...)`"* — describing the behaviour of callers that do not
exist. A handoff document states it is used by `exception_sync_curr_el`; the
kernel's actual W^X diagnostic is a permission-fault-plus-address-range test
that never inspects a PTE.

The invariant holds anyway, on the five mechanisms above. What is wrong is the
**inventory**: an enforcement list that names a dormant function ahead of the
`vma_alloc` line that is doing the work. See task #59, and
[[chg-2026-08-03-mapping-core-sweep]] for the full chain.

**A sixth document names a syscall that does not exist.** `kernel/elf.c`'s file
header calls the loader "one of three layers (PTE bits + mprotect + ELF
loader)". There is no `mprotect` in this kernel — searching `kernel/`, `arch/`
and `mm/` for it returns exactly one hit, that comment. Mechanism 5 above *is*
about `mprotect`, but as an **absence**: what protects the invariant is that no
such call exists to get wrong. Listing an absence as a layer alongside two real
checks turns a strength into a phantom, and the same sentence omits
`vma_alloc` — making [[sub-kernel-elf]] the sixth document to do so. Folded into
task #59.

The pattern across all six is worth stating once: **every document that
enumerates this invariant's enforcement names something that cannot fire, and
none names the single line that always does.** The enumerations were written
from the design and never re-derived from the code.

**The encoder translates faithfully; it does not judge.** `make_user_pte_l3`
given `WRITE|EXEC` emits a writable, user-executable PTE. It is not a gate and
was never meant to be one — but a comment on the JIT fault arm asserts that
"the W^X decision therefore stays entirely in `make_user_pte_l3`", which is
false in a place where it matters, since the JIT is the one surface that
deliberately holds two mappings of one code region.

## Where it is enforced

[[sub-kernel-vma]] (the gate) · [[sub-kernel-mmu]] (the encoders, the asserts,
the patcher's alias) · [[sub-kernel-fault]] (every install passes `vma->prot`
through unchanged).
