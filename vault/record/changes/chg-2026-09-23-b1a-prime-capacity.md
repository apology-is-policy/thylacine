---
id: chg-2026-09-23-b1a-prime-capacity
type: chg
title: "B-1a' (capacity): the range detach over one core (SYS_BURROW_DETACH 38 in the Linux form + the phenotype munmap), the charged sparse pagemap, the user pool + the I-32 default = RAM minus a reserve, the window-confined fixed arms -- LANDING, audit pending"
date: 2026-09-23
arc: arc-boosty
commits: ["387ffcd8"]
touched:
  - sub-kernel-vma
  - sub-kernel-burrow
  - sub-kernel-addrspace
  - sub-kernel-fault
  - sub-kernel-mmu
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-vivarium
  - sub-kernel-proc
  - sub-kernel-devctl
  - sub-kernel-boot-sequence
  - sub-stratum-boot
  - sub-netd-server
  - sub-kernel-protect-witness
  - inv-i32
  - spec-burrow
  - moc-kernel-memory
established: [sub-kernel-pagemap, spec-capacity]
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-23
---
## Synthesis

The kernel's part of ARCH 6.5 "Range detach" + "Capacity, and the I-32
default" (operator-ratified 2026-09-23), built on branch `b1a-prime-wip`
across three WIP commits (`48e89c2b` the spec, the pagemap and the fault
arms; `63b61384` the range detach core and the pool; `a1649f92` the twelve
tests, two fixes they found, the probes) and landing as one chunk whose SHA
this note's `commits` field takes at the close. The holotype audit has NOT
run; this note records the build, not a verdict.

The memory bar is two-sided -- a program is never refused memory while free
memory exists, and memory it relinquishes returns so its footprint shrinks --
and the chunk is the four mechanisms that make the kernel keep it: (i) the
**pagemap** ([[sub-kernel-pagemap]]), a 512-ary radix of on-touch node pages
charged to the address space that touched them, replacing the flat uncharged
`filepages` array, so an untouched reservation costs one struct and
`BURROW_RESERVE_MAX` lifts from 1 GiB to the burrow window (the detach's own
256 MiB bound goes with it); (ii) the **range detach**
(`vma_detach_range_in`, [[sub-kernel-vma]]): phase 1 decides every refusal,
phase 2 uninstalls, phase 3 releases BEFORE it reshapes -- the `NoOrphan` law
of [[spec-capacity]], written first -- and the MAP_FIXED replace is now that
detach followed by an insert, which closes the B-1a audit's F5 by
construction; the native `SYS_BURROW_DETACH` 38 takes the Linux form (an
empty range answers 0) and the phenotype `munmap` is the same core with the
errno through ([[sub-kernel-syscall-dispatch]], [[sub-kernel-syscall-abi]]);
(iii) the **user pool** ([[sub-kernel-addrspace]]): RAM minus max(256 MiB,
RAM/8) clamped to RAM/2, sized once after `phys_init`
([[sub-kernel-boot-sequence]]), held above every address space's cap,
`PRINCIPAL_SYSTEM` counted but never refused, published on `/ctl/memory`
([[sub-kernel-devctl]]); and (iv) the **I-32 default** = the pool --
`PROC_PAGE_MAX` / `PROC_PAGE_HARD_MAX` deleted, the cap now the parent's
narrowing ([[sub-kernel-proc]], [[inv-i32]]). The phenotype's fixed arms are
confined to the window so nothing they place can outlive its `munmap`
([[sub-kernel-vivarium]]).

## What the dossiers gained

[[sub-kernel-pagemap]] is new: the two representations, the retry-loop
install, the hand-back take, the present-node walk and its witness, the
mirror, the lifecycle by site, and the twelve-test suite. [[spec-capacity]]
is new: the conservation law, the action-site map, the two counterexamples
and the gate's blind spots. [[sub-kernel-vma]] owns the three phases and the
replace-as-detach; [[sub-kernel-burrow]] the pagemap arms, the per-mapping
release, the multi-mapping decommit, the chain-aware deferred free and the
lifted `BURROW_RESERVE_MAX`; [[sub-kernel-addrspace]] the pool and the death
return; [[sub-kernel-fault]] the node charge on the ANON_LAZY miss and the
uncharged FILE install; [[sub-kernel-mmu]] one line (its range form now walks
a whole reservation); [[sub-kernel-syscall-dispatch]] the two detach
syscalls over one core, the window-bounded fixed arms with the surgery's
errno through, the deleted helpers, and the phenotype-munmap leak CLOSED;
[[sub-kernel-syscall-abi]] the behaviour changes on 38 / 83 / 84 with no
number moved; [[sub-kernel-vivarium]] `fixed_addr_ok` and L21 / L21c / L21d;
[[sub-kernel-proc]] the pool-valued budget accessors; [[sub-stratum-boot]]
the `/capacity-probe` rung and the CL-5 probe reading its own budget;
[[sub-netd-server]] the retirement oracle that a second detach can no longer
serve; [[sub-kernel-protect-witness]] `/capacity-probe` itself; [[inv-i32]]
the pool as the second bound and the metadata axis; [[spec-burrow]] partial
unmap now existing beneath it; [[moc-kernel-memory]] the two new nodes.

## Two defects the chunk's own tests found (fixed in the chunk; not audit findings)

1. **A dying address space leaked its RSS into the pool.** `vma_drain_in`
   frees Proc-agnostically, and the address space's `page_count` died with
   it -- harmless while that counter was the only bound, a permanent
   shrinking of the machine-wide pool once one existed above it.
   `addrspace_unref` now returns whatever the drain left charged
   (`capacity.death_returns_charges_to_pool`).
2. **The release walked every slot of the range.** A window-sized
   reservation (2^34 slots) with two pages resident spun under `as->lock`
   per slot; `pagemap_take_next` walks present nodes only, and
   `pagemap_walk_steps` pins the bound
   (`capacity.window_sized_reservation_releases_in_bounded_steps`).

## What this documentation pass found against the summary it was given

- The fork clone's node pages were **not charged to the child** as first
  built: `burrow.h` says the caller charges `burrow_lazy_footprint` (resident
  + nodes) after the mapping ref lands, but `clone_one_vma` charged
  `burrow_lazy_resident_count` only and `burrow_lazy_footprint` had no
  caller. The child's later takes refunded those nodes regardless, so its
  `page_count` and the pool dropped below what is held -- the loosening
  direction. Found by the chunk's self-audit and by this pass independently;
  FIXED in the chunk (the one call), with
  `capacity.fork_clone_charges_pages_and_nodes` and the `noclonefootprint`
  RED as witnesses. Recorded on [[sub-kernel-pagemap]] and
  [[sub-kernel-addrspace]].
- `pagemap.h`'s cost summary ("at most one node per 512 pages plus one
  root") is the dense bound only; a sparse touch costs up to `depth` nodes
  per page, and the chunk's own 4 GiB test holds 8 pages under 13 nodes.
- The old reservation cap was 1 GiB (`BURROW_RESERVE_MAX`), not 256 MiB;
  256 MiB was `BURROW_ATTACH_MAX`, the bound the old detach applied inside
  the window. ARCH 6.5's "the old 256 MiB cap is gone" conflates the two.
- `capacity.tla`'s header names `burrow_decommit_in` as the Detach's
  release site; the site is `burrow_release_lazy_range_in`, which the
  decommit also loops over.
- ARCH 6.5 "Spec posture" still says `vma_replace_range_in` refuses a COW
  VMA today; as built the detach's tail piece carries `flags` whole and the
  replace serves a COW survivor.

## Verification (as recorded by the chunk, not re-run here)

The kernel suite 1650/1650 at `-smp 4` and `-smp 1` on `a1649f92` (its
commit message; boot unverified there). `specs/check-capacity.sh` pins
`capacity.cfg` at 625 distinct states and the two buggy cfgs by `NoOrphan`.
`/capacity-probe` as committed opened `/proc/self/status`, which devproc does
not serve (the Plan 9 shape is `/proc/<pid>`), so its first leg failed at
boot; the probe is being corrected to `/proc/<pid>/status` with `t_getpid`,
as joey's own `proc_status_field` does. No sabotage (RED) run and no
holotype round has been recorded for this chunk.
