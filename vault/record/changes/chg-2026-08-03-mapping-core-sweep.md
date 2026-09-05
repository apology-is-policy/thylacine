---
id: chg-2026-08-03-mapping-core-sweep
type: chg
title: "the mapping core -- a W^X checker with no callers, named as enforcement by five documents"
date: 2026-08-03
arc: arc-vault
commits: []
touched:
  - sub-kernel-mmu
  - sub-kernel-vma
  - sub-kernel-fault
  - inv-i12
  - moc-kernel-memory
  - sub-ptyfs
established:
  - sub-kernel-mmu
  - sub-kernel-vma
  - sub-kernel-fault
  - inv-i12
closed: []
opened: []
depth: skeletal
created: 2026-08-03
---
Batch 29, the first sweep off batch 28's census: the memory-mapping core --
`arch/arm64/mmu.{c,h}`, `arch/arm64/fault.{c,h}`, `kernel/vma.{c,h}`, ~3,900
lines, the largest unowned area and the one holding **I-12**. Main had moved to
`#95`; synced first. L-1 absent on the SEVENTEENTH check.

Three dossiers and [[inv-i12]], which did not exist -- for the reason batch 27
minted I-20 and I-40 late: an invariant note is written from its enforcement,
and the enforcement was unread.

**F1 -- A GUARD NAMED AS ENFORCEMENT THAT CANNOT FIRE.** `pte_violates_wxe` is
a correct W^X predicate at `mmu.c:263`. It has **zero callers** -- not the
install path, not a test, not a tool. What names it:

- `ARCHITECTURE.md`'s section-28 I-12 validation cell leads with it: *"PTE bit
  check (`pte_violates_wxe` + mint-site asserts)"*.
- `holotype/10-consistency.md`: *"I-12 | VERIFIED on 3 legs -- PTE asserts +
  `pte_violates_wxe` + ELF reject + W1.5 transient alias"* (four items named
  for three legs, separately).
- `docs/reference/12-hardening.md`: *"`pte_violates_wxe` continues to
  enforce."* It never began.
- `docs/reference/03-mmu.md`'s **error-path table**: *"Future PTE construction
  violating W^X | `pte_violates_wxe()` returns true; **callers**
  `extinction(...)`"* -- describing the behaviour of callers that do not exist.
- `docs/handoffs/002`: *"used by `exception_sync_curr_el`"*. The actual W^X
  diagnostic is a permission-fault-plus-address-range test that never reads a
  PTE.

And the sharpest part is in the audit record. `holotype/00-register.md`
HT01.B-F5 **fixed a real bug inside it** -- it tested only the kernel-execute
bit, so a writable *user*-executable page read as clean -- and recorded
*"(dormant; 0 callers)"* as a parenthetical. **The audit improved a function it
had just observed could never run, and left the invariant table still counting
it.** Task #59.

**I-12 HOLDS, on five mechanisms, and the inventory names the wrong one first.**
The real user-side gate is one line: `vma_alloc` rejects `WRITE|EXEC`, and it is
the *only* way a user mapping is born -- `burrow_map` is its sole production
caller, and all three `mmu_install_user_pte` call sites are in the fault handler
passing `vma->prot` unchanged. Every user PTE's permission bits in the system
trace to that one `if`. Add the seven PTE `_Static_assert`s, the ELF W+X segment
reject, the patcher's transient read-write-execute-never alias, and the
structural absence of any protection-changing syscall -- and the invariant is
solid. **None of the five documents names `vma_alloc`.**

**F2 -- THE COMMENT POINTS AT THE WRONG GUARD, ON THE SURFACE WHERE IT MATTERS
MOST.** The JIT fault arm (I-42, the newest code here) says the W^X decision
*"stays entirely in `make_user_pte_l3`, which is what makes 'no PTE is ever W
AND X' a property of the encoder rather than of this dispatch."* The encoder
does no such thing: handed `WRITE|EXEC` it emits a writable, user-executable PTE
faithfully. It is a translator, not a gate. The JIT is precisely the surface
that deliberately holds two mappings of one code region, so it is the one place
a reader most needs to know that the safety comes from `vma_alloc` refusing to
create such a VMA. Folded into #59.

**F3 -- THREE HEADERS CALL A LOCK FUTURE WORK THAT IS TAKEN AT 116 SITES.**
`mmu.h`, `vma.h` and `fault.h` each say multi-thread Procs *"need a per-Proc
lock ... documented as a trip-hazard"*. `Proc.vma_lock` has existed since #713.
Each header's own `.c` documents the correction directly: `fault.c` describes
the #713 root-cause fix in detail and holds the lock across the whole pipeline;
`vma.c` states *"every `vma_insert` caller holds it"*. And `vma.h`
**contradicts itself twenty-five lines apart** -- `vma_insert`'s docblock calls
the lock hypothetical, `vma_find_gap`'s requires the caller to hold it. A reader
implementing new multi-thread work reads the header. Task #60.

**F4 -- THREE OF SEVEN W^X COMPOSITES CARRY NO ASSERT.** The assert set covers
four PTE constructors; the two block-form kernel mappings and the page-grain
device mapping have none. All correct today. The device one encodes every
driver's registers. Recorded as a seam, not a task -- the fix is one line each
and belongs to whoever next edits them.

**COUNTERWEIGHT, and this file earns it.** The scar tissue in the fault
dispatcher is documented at the point of the scar: the re-entrancy guard exists
because a fault *inside* the handler recursed until the boot stack crossed its
own guard and **the real bug masqueraded as a stack overflow**; the guard-page
message names which of four stacks, because a wild pointer landing in the wrong
guard reads as an overflow that provably could not have happened; a wild CPU
index is clamped rather than skipped so the guard stays live under exactly the
corruption it exists for.

And the MMU's best idea is structural: the block-to-page demote that would race
a concurrent walker is not locked, it is made **unreachable at runtime** by
pre-demoting the whole allocator zone at boot, single-CPU. The race was removed
rather than guarded.

**THE PATTERN, SIX BATCHES.** b24 the assertions pin the values, not their
description. b25 the models pin the mechanisms, not their own scope. b26 each
copy is pinned to itself, not to the others. b27 the guard travelled, the reason
did not. b28 the ledger pins the areas, not the areas to the tree. **b29 the
enforcement list names a guard that cannot fire, and omits the one that does.**

Which is the same shape and its sharpest instance: every previous case was a
guard narrower than its claim. This one is a *claim with no guard under it at
all*, standing next to a real guard nobody wrote down -- and it survived an
audit that had the evidence in its hand.

**AND THE CORPUS ALREADY KNEW.** `moc-kernel-memory`, written 2026-08-01, ended
with *"VMAs, the demand-page fault arms ... and the MMU live with their own
areas, still unswept."* Batch 27 declared the subsystem sweep complete the next
day. The census at batch 28 was not needed to falsify that claim -- **the vault
contained its own counter-evidence, one note away, and nothing compares prose to
prose.** The linter checks structure and resolvable references; two notes may
contradict each other indefinitely. That is batch 27's probe lesson arriving
from the other direction.

ALSO. [[sub-ptyfs]] refreshed for `#95`, which landed on batch 27's F2: main
instrumented the exact three cooked-arm drop sites in the same taxonomy, driven
by a live bug -- `sleep 30` arriving as `sleep 3`. Independent confirmation the
drops are reachable in practice, so the [[spec-pty]] gap in task #48 is a live
divergence, not a documentation nit. Its `arm_drop_report` gate is worth
naming: counting is unconditional, but the one-shot report arms only after the
selftest, because the selftest drops a byte on purpose and an ungated report
would spend the latch -- **the instrument disarmed by its own test**, which is
this arc's probe discipline appearing in the kernel tree independently.

LEDGER. Corpus 798 -> **803**. Coverage 133 -> **139 owned of 421 (33%)**, six
files: `arch` 15 unowned -> 11, `kernel` 44 -> 42. Invariant notes gain [[inv-i12]];
**no `inv-i36`** -- its other half is in exec (task #52), and minting it from
half its enforcement is the error above.
