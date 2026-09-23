---
id: spec-cow
type: spec
title: "cow.tla"
models: [sub-kernel-addrspace, sub-kernel-vma, sub-kernel-burrow, sub-kernel-fault]
pins: [inv-i44]
cfgs:
  - "cow.cfg -- clean, three sharers: Safety (TypeOk + NoAliasedWritable + NoUseAfterFree + NoDoubleFree) and the EventuallyReleased liveness; 580 distinct states, pinned"
  - "cow_buggy_break.cfg -- the drop/decide/act sequence is not atomic: NoAliasedWritable"
  - "cow_buggy_teardown.cfg -- the share is dropped before the copy and no pin is taken: NoUseAfterFree"
  - "cow_buggy_vfork.cfg -- the vfork parent observes outside the lock and parks after: EventuallyReleased (Safety still holds); 231 states, pinned"
  - "cow_protect.cfg -- clean, ALLOW_PROTECT, three sharers under SpecProtect: Safety + ProtectSafety (ShareIsHolderCount + NoWritablePteBeyondProt + BreakOnlyWhenWritable) and EventuallyReleased; 10636 distinct states, pinned (B-1a)"
  - "cow_buggy_protect_keeps_pte.cfg -- a protect changes the prot and leaves the writable PTE installed: NoWritablePteBeyondProt (B-1a)"
  - "cow_buggy_fault_ignores_prot.cfg -- the break arm fires on a mapping protected below RW: BreakOnlyWhenWritable (B-1a)"
  - "cow_buggy_clone_per_piece.cfg -- a fork mints one clone Burrow PER VMA piece (Pieces = 2): ShareIsHolderCount, violated in the initial state (B-1a)"
gate: "any change to kernel/cow.c, to addrspace_clone's phase order or its clone_cursor dedupe, to the vfork suspend/release path, to burrow_protect_in's uninstall-before-prot order, or to vma_reprotect_range_in's cut; specs/check-cow.sh runs all nine cfgs and pins the clean counts and the buggy cfgs by invariant name"
created: 2026-08-06
updated: 2026-09-23
---
## Abstraction

Written **model-first** — TLC-green before the L-4 implementation — under
the spec-first re-enablement, on the same argument that re-enabled
[[spec-asid]] and [[spec-death-wake]]: a break racing a concurrent break,
a sharer's exit, and the vfork release is exactly the subtle SMP class
machine-checked exploration catches and tests do not.

One shared anonymous page reached by N sharers — the address spaces a fork
produced — each mapping it read-only. A write faults and the fault arm
breaks the share.

**Atomicity is modeled by step granularity**, the standard TLA+ idiom: a
sequence performed under one lock hold is ONE action, and the buggy
variants split it so a peer can interleave. An explicit mutex variable
would add state without adding reachable interleavings, because every
critical section in this protocol is straight-line.

**Deliberately beneath the model:**

- **the intra-address-space install-once race** — two *threads* of *one*
  address space faulting one page. That is the already-audited REVENANT /
  lazy-arm shape (loser frees, winner installs) and sits below an
  abstraction that treats a sharer as one agent per address space. L-4b
  reuses that arm rather than inventing a second one;
- **the page table.** "Installed writable" is a program counter, not a
  PTE, so whether `mmu_uninstall_user_range` covers the right range with
  the right TLBI is answered in prose and in [[sub-kernel-mmu]];
- **the Burrow clone itself.** `Sharers` are the cloned Burrows and
  `share` is their common page's `cow_share`; the cloning is assumed to
  have happened correctly;
- **allocation failure.** The copy path always gets its private page. OOM
  during a break is a real path and is handled by prose plus the
  wholesale-discard structure, not here;
- **more than one page.** The model is one page's protocol, so nothing
  about ordering *across* pages — including the clone's walk — is
  covered.

The `ASSUME` restricting the flags to at most one enabled carries its own
lesson in the source: it is counted **arithmetically**, not as the
cardinality of a set of the three flags, because a set collapses
duplicates — `{TRUE, TRUE, FALSE}` has cardinality 2 and would pass a
`<= 1` test with two bugs enabled. The flags are values, not identities.

**The B-1a extension** (2026-09-23, behind `ALLOW_PROTECT`; additive by
measurement, not assertion: with the switch off the four pre-existing cfgs
reproduce exactly -- 580 / 231 states and the two buggy cfgs by name). A
sharer's mapping carries a `prot` in `{"rw", "ro", "none"}` and a `ptew` bit --
"a writable PTE is installed" -- so the model can say the two things the
ceiling makes load-bearing: a PTE may be writable ONLY while the prot is rw,
and a break may fire ONLY on an rw mapping. `ProtectDown` / `ProtectUp` move
the prot (down clears `ptew` unless `BUGGY_PROTECT_KEEPS_PTE`; up leaves it
clear -- the uninstall runs on a raise too), `Reinstall` is the re-fault after a
raise back to rw, `Fault` is guarded on `prot = "rw"` (or
`BUGGY_FAULT_IGNORES_PROT`), and the break arms set `ptew`. `Pieces` and
`BUGGY_CLONE_PER_PIECE` model the fork of a SPLIT mapping: with the bug every
non-parent sharer holds `Pieces` shares (`Held(s)`), so `share` starts at more
than the holder count and `ShareIsHolderCount` fails in the initial state --
the fork itself is the bug. `ProtectSafety` is deliberately kept OUT of
`Safety`, so the four pre-existing cfgs check byte-for-byte what they always
did. `SpecProtect == Spec /\ WF_vars(VChildRelease) /\ WF_vars(VParentCheck)`:
the protect ladder makes the state graph cyclic, and `WF_vars(Next)` alone
admits a protect-forever run that never releases the vfork parent, so the
liveness cfg needs weak fairness on the vfork sub-machine. The one-flag ASSUME
now spans six bug flags.

## Action-site map

| Action | Site |
|---|---|
| `Fault(s)` | the write-fault arm of `userland_demand_page` reaching a `VMA_FLAG_COW` region |
| `DecideLocked(s)` | `cow_page_break_is_sole` — the whole decide under [[lock-cow]] as one step; `share = 1` -> take in place, else pin and copy |
| `BreakFinish(s)` | the break's tail: install the private page, then `cow_page_put` — **the drop happens here, after the copy** |
| `Exit(s)` | `addrspace_unref`'s last drop -> `vma_drain_in` -> the Burrow's slot release -> `cow_page_put` |
| `FreePristine` | `cow_page_put` returning true, and the caller freeing **outside** the lock |
| `VChildRelease` | the vfork child's exec or exit releasing the shared address space |
| `VParentCheck` | the L-3c-2 suspend: check-and-park in one atomic step |
| `DropUnlocked` / `LookUnlocked` / `VParentParkLate` | **no site** — they exist only under a buggy flag |
| `ProtectDown(s)` / `ProtectUp(s)` | `burrow_protect_in` (B-1a): `vma_reprotect_precheck_in` -> `mmu_uninstall_user_range` -> `vma_reprotect_range_in`'s APPLY loop, one `as->lock` hold — the uninstall BEFORE the prot write is `ptew' = FALSE` |
| `Reinstall(s)` | the fault dispatcher re-installing a resident page at the NEW `vma->prot` after a raise (`arch/arm64/fault.c` step 2 + the arm) |
| `Held(s)` / `InitShare` | `addrspace_clone`'s one-clone-per-source-Burrow cursor (`Burrow.clone_cursor`); the buggy value is a clone per VMA piece |

| Invariant | Obligation |
|---|---|
| `NoAliasedWritable` | [[inv-i44]]'s core: at most one sharer holds the pristine page writable. A `private` sharer is writable only through its own copy, which no one else can reach |
| `NoUseAfterFree` | nothing still references the pristine page after it is freed — the break-vs-teardown race |
| `NoDoubleFree` | the page returns to the buddy at most once |
| `EventuallyReleased` | the vfork parent is always resumed (L-3c-2's NoStrand) |
| `ShareIsHolderCount` | (B-1a) `share = Cardinality(Referencing)`: the per-page count equals the number of address spaces holding the page — a fork of a split mapping mints ONE clone |
| `NoWritablePteBeyondProt` | (B-1a) a writable PTE never outlives the permission that justified it — the uninstall runs before the prot changes |
| `BreakOnlyWhenWritable` | (B-1a) the break fires only on an rw mapping — the fault dispatcher checks `vma->prot` before it resolves the Burrow |

`FreePristine` is deliberately **the real free decision** — it trusts
`share` and `pin` exactly as the implementation does — so a protocol that
lets the count lie gets caught there rather than papered over by a guard
the kernel would not have.

## The three counterexamples

`cow_buggy_break` is the headline: two sharers each drop and *then* look,
so both read zero, both conclude "I am the last", and both take the same
page in place, both writable. One address space's writes land in
another's. This is the bug the **global** lock exists to prevent — a
per-Burrow lock cannot, because the two sharers hold different ones.

`cow_buggy_teardown` inverts the drop and the copy and removes the pin. A
concurrent exit then drives the count to zero and frees the pristine page
while the breaker is still reading it. It is why the correct path pins
across the copy rather than trusting the count to stay put — and the pin
is not a separate mechanism, it is *the breaker's own retained share*.

`cow_buggy_vfork` is the odd one out and is instructive for that reason:
**Safety still holds.** The parent observes "has the child released?"
outside the lock and parks after, so a release landing in the window is
lost and the parent parks forever. Nothing is corrupted; the system hangs.
That is why the witness has to be a liveness property, and why a spec that
only ever checks invariants would have called this protocol correct.

The vfork machine was modeled **retroactively** — the mechanism had
already shipped at L-3c-2 — on the [[spec-death-wake]] precedent that a
shipped mechanism on the death lineage earns a model. The release
condition is deliberately not a *record* of the release: it **is** the
release, "the child no longer maps my space".

## The three B-1a counterexamples

`cow_buggy_protect_keeps_pte` lowers a mapping's prot and leaves its writable
PTE installed. Nothing in the protocol notices, because hardware resolves a
PTE without taking a lock: the sharer keeps writing a page its mapping says it
may not, which is exactly what the D-3b rule ("uninstall before the prot that
justified the PTE goes") exists to prevent. `NoWritablePteBeyondProt` fails on
the first `ProtectDown`. The suite's `nouninstall` sabotage is the same shape
in the kernel, failing exactly the two assertions that look through the page
table.

`cow_buggy_fault_ignores_prot` lets the break arm fire on a mapping protected
below rw. A write that had to be refused instead breaks the share and installs
a writable private page: `BreakOnlyWhenWritable` fails. In the kernel this is
the fault dispatcher's step 2 -- `vma->prot` against the access before any
Burrow is resolved -- and `/protect-guard-child` is its boot witness.

`cow_buggy_clone_per_piece` is the odd one out, as `cow_buggy_vfork` was for
the first three: it needs **no action at all**. With `Pieces = 2` and the flag
on, the initial state already has `share = 1 + 2 * (Sharers - 1)` against
`Sharers` holders, so `ShareIsHolderCount` is violated by `Init`. That is the
honest shape of the bug -- the fork IS the defect, not a race after it -- and
it is why `addrspace_clone` dedupes through the source's cursor rather than
fixing anything downstream. `specs/check-cow.sh` pins all nine cfgs: the
clean counts (580 / 10636), the buggy cfgs by the name of the invariant TLC
reports violated, and the vfork liveness by "Temporal property
EventuallyReleased was violated" at 231 states.
