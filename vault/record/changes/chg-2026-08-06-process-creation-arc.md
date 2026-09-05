---
id: chg-2026-08-06-process-creation-arc
type: chg
title: "The process-creation arc, and three comments that outlived their code"
date: 2026-08-06
arc: arc-vault
commits: []
touched: [sub-kernel-vivarium, sub-kernel-addrspace, sub-viv, lock-vma]
established: [inv-i44, spec-cow, lock-cow]
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-06
---
Batch 55: the surfaces the LINEAGE and VIVARIUM arcs landed unowned —
5992 lines across eight files, holding two of the enumerated invariants
and every one of them arriving in the merge that made batch 54's ledger
readable.

**WHAT.** Three dossiers — [[sub-kernel-vivarium]] (the Linux syscall
translation table), [[sub-kernel-addrspace]] (the refcounted address space
and the copy-on-write break), [[sub-viv]] (the container runner) — plus
the supporting notes they could not honestly do without: [[inv-i44]],
minted because nothing stated the fork's divergence invariant; [[spec-cow]],
because `cow.tla` is real and is what validates it; and [[lock-cow]],
because the break's global leaf lock had no note. [[lock-vma]] was
corrected rather than written: it still described `Proc.vma_lock` as "a
per-Proc spinlock" nine months after L-1 moved it onto the AddrSpace,
where two Procs genuinely share one.

Coverage went **362 → 370 of 434 files (83% → 85%)**, unswept lines
**17804 → 11837**. And the spec ledger closed: **34 dossiered, 0
missing** — `cow.tla` was the last module in the tree with no note, which
is only visible because the view counts modules on disk rather than notes
in the vault.

**THE THREE FINDINGS ARE ONE SHAPE, AND IT IS NOT THE ONE THE LAST BATCH
BUILT A TOOL FOR.** `quaestor stale` answers "has the code moved under
this dossier". It cannot see the drift this batch actually found, which is
one layer down: **a comment that has moved out from under its own code**,
in the same file, sometimes in the same paragraph.

- `vivarium.h`'s opening block states that nothing in the file is wired
  into `syscall_dispatch`, that nothing can set `PHENO_LINUX`, and that
  the dispatch branch "would today be branching on a field that is
  provably always 0". V-1b and V-7 both landed. **The same header's own
  body calls `viv_linux_dispatch` twice.** The file refutes its opening
  without either half noticing. (task #163)
- `VIV_NATIVE_CEILING` is 105 and the paragraph declaring it says "above
  102". That paragraph exists *because* the number had already rotted in
  prose four times — it says so, two inches up, and then does it again.
  The same comment says "the two rows below it"; there are seventeen.
  (task #164)
- `addrspace.h` states as a **PRECONDITION** that callers hold `as->lock`,
  "which is what makes each cap EXACT" — three times, in three places.
  `addrspace.c` opens the same six functions with "All six are CAS loops,
  so they are correct with **no lock held**", names the live call site
  that holds none, and says the bound is a floor. `proc.c` carries both
  versions **three lines apart**. (task #165)

**WHY THE THIRD IS THE DANGEROUS ONE.** The first two mislead a reader.
The third makes the *review criterion* wrong in both directions at once: a
new charge site that takes no lock reads as a contract violation when it
is the supported case, and an I-32 auditor reading the declaration site is
told the caps are exact when the implementation documents that two
concurrent charges can both pass and both land. `proc_page_charge` — which
the header itself calls "the thin wrapper, what ordinary code calls" —
takes no lock. So most real charge sites already fail the stated
precondition, correctly.

The nuance the fix must keep is why the claim was ever written: the lock
is not useless. It no longer serialises the arithmetic — the CAS does —
but holding it across check-then-charge is still what makes a **cap
decision** exact against a sibling. `proc.c`'s body comment states that
correctly, three lines below the block that does not, and is the model for
the rewrite.

**WHAT THE CODE GOT RIGHT, RECORDED BECAUSE IT IS THE HARDER HALF.** The
translation layer's governing rule — *never silently mistranslate; either
produce an exactly-equivalent call or decline* — is enforced by
construction rather than by review, and the file's worked counterexamples
are better than anything a dossier could invent. `munmap` and
`SYS_BURROW_DETACH` take the same two words in the same order and are
wrong in two directions. `writev`'s second argument is an entry count
where `SYS_WRITE`'s is a byte length, so a renumber would write the
guest's own pointer array to its fd. `F_DUPFD_CLOEXEC`'s is a *rights
mask* under `SYS_DUP`, which would hand back a descriptor with arbitrary
authority for a legal input. Registers lining up is not arguments meaning
the same thing, and this file is the place that rule is enforced.

The clone's phase order is the same quality: uninstalling the parent's
writable PTEs **before** sharing is not defensive, it is the whole safety
argument, because holding the address-space lock only reaches a peer that
*faults* and a peer with an installed writable PTE stores in hardware.

**VERIFICATION.** All 5992 lines read, not skimmed. Every claim that could
be checked against the tree was: `SYS_RFORK = 105` and `SYS_EXECVE = 104`
(so 102 is stale); seventeen `VIV_LINUX_*` rows below the ceiling, listed;
`viv_linux_dispatch` called from `syscall.c` and `PHENO_LINUX` assigned in
the spawn thunk (so the header's four claims are four falsehoods);
`specs/cow.tla` present with its four cfgs (so the citation is not the
phantom-document class); and 34 `.tla` modules against 33 spec notes,
which is how the missing one was found.

LEDGER read off the rendered view after the merge, for the sixth
consecutive batch. The merge itself was clean — the first in five, because
the two reference docs main touched are not yet absorbed, which is task
#161's whole subject stated as a coincidence.
