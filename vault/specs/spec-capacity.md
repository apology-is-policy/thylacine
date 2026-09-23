---
id: spec-capacity
type: spec
title: "capacity.tla"
models: [sub-kernel-vma, sub-kernel-burrow, sub-kernel-pagemap, sub-kernel-fault, sub-kernel-addrspace]
pins: [inv-i32]
cfgs:
  - "capacity.cfg -- clean, four slots x two Burrows: TypeOk + ChargeConserved + NoOrphan + LiveIsMapped; 625 distinct states, pinned"
  - "capacity_buggy_replace_orphans.cfg -- the D-3b window keeps the old Burrow's slots resident and charged after the swap (the B-1a audit's F5): NoOrphan, at the first Replace of a touched slot; ChargeConserved listed ahead of it and HOLDING"
  - "capacity_buggy_detach_no_refund.cfg -- a detach unmaps a resident slot without releasing it (the pre-B-1a piece-detach shape): NoOrphan, at the first Detach of a touched slot; ChargeConserved listed ahead of it and HOLDING"
gate: "any change to vma_detach_range_in's phase order (the release BEFORE the reshape), to vma_replace_range_in, to burrow_release_lazy_range_in / burrow_decommit_in, to the ANON_LAZY fault arm's charge, to burrow_free_internal's Proc-agnostic free, or to alloc_user_pages / free_pages' pool charge and return (mm/phys.c), or to the MMU's table charge and reclaim; specs/check-capacity.sh runs the three cfgs, pins the clean count and judges each buggy cfg by the NAME of the invariant TLC reports"
created: 2026-09-23
updated: 2026-09-23
---
## Abstraction

The I-32 page-accounting **conservation law**, written model-first in the
same chunk as the code it constrains (B-1a', 2026-09-23): the module landed
in the chunk's first WIP commit before `vma_detach_range_in` existed, and the
range detach core was then written to it -- the release BEFORE the geometry
change, and the D-3b replace as detach-then-insert. ARCH 6.5 "Capacity"
states the bar in prose (memory a program relinquishes RETURNS to the
system); the kernel's half of that bar is an accounting law, and this module
is that law where a model checker can break it.

One address space; one lazy reservation ("old") that a MAP_FIXED window
("new") may be overlaid into; and the `page_count` that charges it. A
**slot** is one page of a reservation, and mappings are modelled per slot
(`mapped[b][s]`: a VMA piece covers `s`), because the range detach, the
protect split and the D-3b replace all reduce, for the accounting, to
"which slots of which Burrow are still covered by SOME piece of this
address space". Four actions: `Touch` (the demand-zero fault: a mapped,
non-resident slot gains a page and a charge), `Decommit` (a mapped resident
slot loses its page and its charge; the mapping stays), `Detach` (release
the slot's page -- free + uncharge -- BEFORE the geometry changes, then
unmap; if that was the Burrow's last mapped slot the Burrow frees) and
`Replace` (the window takes the slot over: a detach of "old"'s slot followed
by "new" mapping it). `FreeIfLast` is the Burrow free that follows the last
unmap: every remaining resident page dropped, `charge` NOT touched -- the
load-bearing fact, because `burrow_free_internal` has no Proc to refund. So
every path that unmaps a slot MUST release it first, or the charge outlives
the page.

The two invariants say two different things, and the second exists because
the first is blind. `ChargeConserved` (`charge = ResidentCount`) is
"page_count == RSS". `NoOrphan` (a resident slot is always mapped) is what
makes a future refund POSSIBLE: the only refund paths walk mappings, so a
slot that lost its mapping while resident is charged for the rest of the
address space's life -- and it is resident AND charged, so the counter
still balances. That is exactly the B-1a audit's F5 (a D-3b window replaced
inside a touched lazy mapping) and exactly the pre-B-1a piece-detach bug
before it: two occurrences of one shape, a path that unmapped a slot without
releasing it in a system whose free is Proc-agnostic. Both buggy cfgs are
judged on `NoOrphan` with `ChargeConserved` listed ahead of it and holding;
a run that reported the counter instead would mean the model no longer says
the counter is blind. `LiveIsMapped` (a Burrow is live iff some slot of it
is mapped) is the Tier-1 free shape carried over from [[spec-burrow]]; the
handle count is already zero throughout.

**Deliberately beneath the model**, listed so a green run reads no larger:

- **pieces, merges and prots.** A VMA piece, a protect's cut, the merge pass
  and the ceiling are all below "is slot `s` still covered"; their soundness
  is [[spec-cow]]'s and [[sub-kernel-vma]]'s;
- **metadata.** The pagemap's node pages are charged and released by the
  same paths as the pages they index and are not modelled separately: they
  ride the slot ([[sub-kernel-pagemap]]);
- **the fork.** One address space, so no clone of the pagemap;
  `pagemap_mirror` and the per-page share counts are `cow.tla`'s territory;
- **the user pool**, the second bound above `page_count` -- PHYSICAL since the
  round-1 close, charged at `alloc_user_pages` and returned at `free_pages`
  ([[sub-kernel-mm-phys]]), a mirror of no counter the model has;
  `capacity.pool_refuses_users_keeps_tcb` (refused at exactly K pages, nodes
  and tables counted; the TCB not) and
  `capacity.memory_bomb_leaves_the_reserve` are its witnesses;
- **the hardware page tables**, charged to the space and reclaimed as they
  empty ([[sub-kernel-mmu]]; the round-1 audit's F1): the model's slots have
  no tables; `capacity.page_tables_charged_and_reclaimed` is the witness;
- **the Image cache**: its pages are charged to each space that maps them per
  leaf and reclaimed from idle images under pressure ([[sub-kernel-image]];
  the round-2 audit's F8) -- the model has no cache;
  `demand_page.file_pages_charge_the_holder` and
  `demand_page.idle_image_reclaimed_under_pressure` are the witnesses (the
  latter with a physical control since round 3 -- the buddy gains exactly
  what the pool released -- and the strip bounded to the pages wanted, F14);
- **the address space's death.** The model's one address space never dies;
  on hardware the drain frees Proc-agnostically and every freed page returns
  its pool charge at `free_pages`, so `addrspace_unref` settles nothing
  (`capacity.death_returns_charges_to_pool`, tables included). The path found
  the chunk's first defect: before the pool, a charge that died with its
  counter cost nothing; with a counter-shaped bound above it every death
  leaked its RSS forever, and the first fix -- a death return -- was
  superseded by the physical pool, under which it would double-return;
- **the walk bound.** Whether the release costs what was touched or what was
  reserved is `pagemap_walk_steps`' question
  (`capacity.window_sized_reservation_releases_in_bounded_steps`);
- **the lock.** Transitions are atomic; the mutual exclusion that makes the
  arithmetic hold on hardware is `as->lock` and `v->lock`, prose and audit
  ([[sub-kernel-burrow]] Concurrency).

## Action-site map

| Action | Site |
|---|---|
| `Init` | a fresh lazy reservation: `burrow_create_anon_lazy` + `burrow_map`; no slot resident, `page_count` untouched |
| `Touch(b, s)` | `arch/arm64/fault.c`, the ANON_LAZY miss in `demand_page_locked`: the data page charged first (`proc_page_charge(p, 1)`), then `pagemap_install(&v->pm, .., p->as, ..)` charges the node pages it allocates to the SAME address space; a refused charge installs nothing and takes the graceful per-Proc terminate ([[sub-kernel-fault]]) |
| `Decommit(b, s)` | `burrow_decommit_in` (`SYS_BURROW_DECOMMIT` 84): admission over every mapping in the range, the PTEs cleared, then `burrow_release_lazy_range_in` per mapping -- take, free, uncharge per slot; the mapping kept ([[sub-kernel-burrow]]) |
| `Detach(b, s)` | `vma_detach_range_in`, phase 3: `burrow_release_lazy_range_in(as, v, lo, hi)` over the overlap BEFORE the head/tail trim, the middle split or the whole removal. The release walks the pagemap by PRESENT nodes (`pagemap_take_next`), puts each page (`cow_page_put`) and uncharges the data pages plus the nodes each take emptied. The mapping's own free is `vma_free_deferred` -> `burrow_free_deferred` -> `burrow_free_internal`, after `as->lock` drops ([[sub-kernel-vma]]) |
| `Replace(s)` | `vma_replace_range_in` = allocate the new piece, `vma_detach_range_in(.., extra_vmas = 1, ..)` the window (which releases the window's slots of the OLD Burrow), insert. F5 closed by construction: there is no second copy of the release to forget |
| `FreeIfLast` | `burrow_free_internal`'s ANON_LAZY arm: `pagemap_destroy` with the COW put. Proc-agnostic, refunds nothing -- the fact every other row is built around |
| `BUGGY_DETACH_NO_REFUND` / `BUGGY_REPLACE_KEEPS_ORPHANS` | no sites -- the first is the exact-match detach's pre-B-1a shape (unmap, then refund the whole Burrow's count per piece, or nothing for a piece), the second the D-3b surgery as it stood before B-1a' (cut the old VMA around the window, uninstall the window's PTEs, leave the old Burrow's slots resident and charged) |

| Invariant | Obligation |
|---|---|
| `ChargeConserved` | [[inv-i32]]: `page_count == true RSS`. Runtime: `detach.range_trims_left_right_middle`, `detach.four_gib_reservation_round_trips` (8 pages + 13 nodes charged, 4 + 6 back at the middle detach, 0 at the end), `capacity.pagemap_nodes_charged_and_reclaimed` (root included) |
| `NoOrphan` | every path that unmaps a slot releases it FIRST, because the free cannot refund. Runtime: `capacity.replace_window_releases_orphans` (the Replace) and `detach.range_across_burrows_and_holes` (a held Burrow ref sees the whole mapping ALIVE with nothing resident: the release ran before the mapping went) |
| `LiveIsMapped` | the Tier-1 free shape ([[spec-burrow]]'s `NoUseAfterFree` as one direction): a Burrow with no mapped slot has been freed |
| `TypeOk` | the state space's shape |

## The two counterexamples

`capacity_buggy_detach_no_refund` skips the release: `Detach` unmaps the
slot with its page still resident and its charge still on the counter.
Nothing can ever refund it -- no mapping names the slot, so no later detach
or decommit reaches it, and the Burrow's eventual free drops the page
without touching `charge`. `NoOrphan` fails on the first `Detach` of a
touched slot; `ChargeConserved` holds throughout, which is the whole point.
Before B-1a the whole-Burrow resident count was refunded once per PIECE (a
double refund, the other direction); the B-1a fix made the refund the
piece's OWN range, and a detach that refunds NOTHING for its range is the
remaining way to get it wrong.

`capacity_buggy_replace_orphans` is the D-3b surgery as it stood before this
chunk: the old VMA cut around the window, the window's PTEs uninstalled, the
old Burrow's slots under the window left resident and charged. No mapping
names them any more, so no detach or decommit can ever refund them, and the
Burrow's last free returns the pages to the system with the charge still on
the address space -- an over-charge, the safe direction, and a leak of the
budget for the Proc's life. `NoOrphan` fails at the first `Replace` of a
touched slot. As built, the replace IS a detach of the window followed by an
insert, so the release runs by construction rather than by a second copy of
the code that could be forgotten.

## The gate

`specs/check-capacity.sh` is `specs/check-cow.sh`'s shape: the clean cfg
explores the whole state space, so its distinct-state count (625, four slots
by two Burrows) is a deterministic fingerprint -- a change means the MODEL
changed; each buggy cfg halts at the first violation and is judged on its
verdict, the exit status plus the NAME of the invariant that fired. A
violation drops a `capacity_TTrace_*` pair beside the spec; the script sweeps
only the ones its own run made, because quaestor's spec census counts files.
**blind-to:** everything under "deliberately beneath the model" above -- one
address space, no metadata, no pool, no death, no walk bound, no lock; the
kernel tests on [[sub-kernel-pagemap]] are their witness. The header comment
of `capacity.tla` names `burrow_decommit_in` as the Detach's release site;
the site as built is `burrow_release_lazy_range_in`, the per-mapping half
that `burrow_decommit_in` also loops over.
