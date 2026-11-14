---
id: spec-payer
type: spec
title: "payer.tla"
models: [sub-kernel-burrow, sub-kernel-fault, sub-kernel-vma, sub-kernel-addrspace]
pins: [inv-i7, inv-i32, inv-i44]
cfgs:
  - "payer.cfg -- clean, three spaces x two slots: RecordIsResidency + ChargeExact + NoFreedLeaf + PayerIsLive + DeadUntouched + LapseOnlyAtDeath; 15603 distinct states and 89443 generated, both pinned at the first TLC run (2026-11-14)"
  - "payer_buggy_handover_no_owed.cfg -- the faulter that takes over a lapsed record pays only the page it mints, not the residency its dead predecessor paid: ChargeExact"
  - "payer_buggy_no_adopt.cfg -- the new payer keeps its leaves charged, though its mint now pays every page (each such page paid twice): ChargeExact"
  - "payer_buggy_fork_copies_payer.cfg -- the child's mapping inherits the parent's payer flag, so its leaves are free and nobody pays them: ChargeExact"
  - "payer_buggy_loser_keeps_charge.cfg -- the faulter that loses the install keeps its mint: RecordIsResidency, with ChargeExact listed ahead and HOLDING (the payer pays the record exactly; the record is what is wrong)"
  - "payer_buggy_unmap_releases.cfg -- an unmap releases the slot as a private lazy detach does, every book kept exact: NoFreedLeaf, with RecordIsResidency and ChargeExact listed ahead and HOLDING (no counter can see a freed page a peer still maps)"
  - "payer_buggy_no_lapse.cfg -- death leaves the record naming the dead space: PayerIsLive"
  - "payer_buggy_unmap_lapses.cfg -- the payer's last unmap settles the record as an eager region's drop does (#131), refunded and lapsed while the payer lives: LapseOnlyAtDeath, with ChargeExact listed ahead and HOLDING"
gate: "any change to the payer record (burrow_payer_set, burrow_mint_charge / mint_payer_locked, burrow_mint_refund, burrow_payer_release, burrow_payers_lapse), to the ANON_SHARED fault arm's mint, install or leaf charge, to vma_uninstall_range_in's per-leaf refund, to vma_payer_mark_fresh / vma_payer_adopt_in, to clone_one_vma's ANON_SHARED or FILE arm, or to the ANON_SHARED free arm; specs/check-payer.sh runs the eight cfgs, pins the clean count and judges each buggy cfg by the NAME of the invariant TLC reports"
created: 2026-11-14
updated: 2026-11-14
---
## Abstraction

The second page-accounting law, for pages that outlive the mapping that
faulted them: a private FILE image's (operator vote 118) and shared anonymous
memory's (votes 117 + 119). [[spec-capacity]] is the I-32 law for memory one
address space owns, and a record-paid Burrow breaks both of its premises on
purpose: several address spaces map it (a fork shares it and never clones
it), its pages stay resident when a mapping goes, and its free CAN refund,
because the record names who pays. Model-first: the module landed in the
chunk's scripture commit, before the shared fault arm existed.

One Burrow; `Slots` its pages; `Spaces` the address spaces. The creator maps
it whole and pays from the start; every other space comes from a `Fork`,
which maps the slots its parent maps. Nothing maps a slot again once it was
unmapped (such a Burrow has no handle and is never extended or re-attached).
A space's own steps are atomic, because its `as->lock` serializes them; only
the mint is split, at the two locks it takes in turn (`g_payer_lock`, then
`v->lock`), so another SPACE may act between `Charge` and `Install`. That
split is what makes the install's loser branch live for shared memory.
`flagged[sp]` is the payer flag on sp's mappings, and the invariants check
the decision it drives (charge a leaf or not) against the truth (`payer = sp`).
Death is one step: the drain, then the lapse.

Beneath the model, said so the green reads no larger: the cap, exemption,
node pages and the pool (they bound HOW MUCH is charged, never TO WHOM); the
descriptor's lifetime (the lapse after the drain and before the last lifetime
drop is the host fixture's, `tools/host-tests/addrspace-lifetime.c`); and all
geometry below "which slots a space maps" -- a split's carriage of the payer
flag, the protect paths and the fixed overlay are the kernel tests'
(`shared_anon.*`, [[sub-kernel-burrow]]).

## Action-site map

The row-by-row binding is `specs/SPEC-TO-CODE.md`, section "payer.tla"; in
brief:

| action | code |
|---|---|
| `Fork(p, c)` | `clone_one_vma`'s ANON_SHARED arm: the child names the parent's Burrow with the ceiling as its only flags (no COW, no PAYER), charging nothing ([[sub-kernel-addrspace]]) |
| `Charge(sp, s)` | the shared fault arm's miss: `burrow_mint_charge` under `g_payer_lock` (a lapsed record makes the faulter the payer, owed the residency), then `payer_adopt_if_due` ([[sub-kernel-fault]]) |
| `Install(sp)` | `pagemap_install(.., NULL, ..)`: rc 1 means a peer won, so the loser frees its page and `burrow_mint_refund`s whoever pays now; the leaf is charged iff the mapping is not the payer's |
| `Uninstall` / `Unmap` | `vma_uninstall_range_in`'s per-leaf refund for an unflagged mapping; the detach releases nothing for the type ([[sub-kernel-vma]]) |
| `FreeOrKeep` | `burrow_free_internal`'s ANON_SHARED arm: `burrow_payer_release`, then `pagemap_destroy` ([[sub-kernel-burrow]]) |
| `Die(sp)` | `addrspace_destroy`: `vma_drain_in`, then `burrow_payers_lapse` |
