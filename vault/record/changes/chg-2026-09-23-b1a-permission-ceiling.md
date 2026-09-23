---
id: chg-2026-09-23-b1a-permission-ceiling
type: chg
title: "B-1a: the permission ceiling -- SYS_BURROW_RESERVE 124 / SYS_BURROW_PROTECT 125, the multi-mapping reprotect, the fork's per-Burrow clone dedupe, the phenotype mprotect row, the lazy-piece detach refund"
date: 2026-09-23
arc: arc-boosty
commits: ["839c1745"]
touched:
  - sub-kernel-vma
  - sub-kernel-burrow
  - sub-kernel-addrspace
  - sub-kernel-fault
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-vivarium
  - sub-stratum-boot
  - spec-cow
  - inv-i12
  - inv-i44
  - moc-kernel-memory
established: [sub-kernel-protect-witness]
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-23
---
## Synthesis

The build of B-1 scripture `96f24314` (ARCH 6.5 "The permission ceiling"),
landed at `839c1745`: every VMA carries a mint-time ceiling in `flags` bits
8..10, `SYS_BURROW_RESERVE` (124) mints a reservation at a prot under an RW
ceiling, `SYS_BURROW_PROTECT` (125) moves a range among {none, R, RW} under
it, X is never a target (refused at the boundary before any lookup AND in the
mechanism), `PROTECT_SEAL` lowers the ceiling for good, and no capability gates
the call. Spec first: `cow.tla` gained the three ways a protect reaches the
COW model behind `ALLOW_PROTECT`, additive by measurement (580 / 231
reproduced; `cow_protect` clean at 10636 under `SpecProtect`).

## What the dossiers gained

The edges are the point of this note. [[sub-kernel-vma]] owns the ceiling and
the multi-mapping reprotect (precheck / cut / apply / merge); [[sub-kernel-burrow]]
the one-locked-step `burrow_protect_in` (uninstall BEFORE the prot write) and
the fork's `clone_cursor`; [[sub-kernel-addrspace]] the ceiling-keyed
eager-anon share and the one-clone-per-source dedupe; [[sub-kernel-fault]] the
three consequences for a dispatcher whose code did not change (a range at
none is a guard; the re-fault installs at the NEW prot; the break still keys on
the flag); [[sub-kernel-syscall-abi]] the two numbers (ceiling 125,
`SYS__NATIVE_TOP` 126; the census re-measured at 123 live / 123 arms);
[[sub-kernel-syscall-dispatch]] the boundary refusal of X, the two
`_for_proc` bodies, the `mprotect` arm, the exact anon mints, the lazy-piece
refund, and the OWNED-not-fixed phenotype `munmap` leak below the window;
[[sub-kernel-vivarium]] the `mprotect` row and the ENDED 6.21 degradation;
[[sub-stratum-boot]] the two probes in joey's ladder; [[spec-cow]] the four
new cfgs, the action map and the three B-1a counterexamples; [[inv-i12]]
mechanism 5 rewritten from "structural absence" to "never a target";
[[inv-i44]] the three new mechanisms. [[sub-kernel-protect-witness]] is new:
the suite, the EL0 probe and the expect-fault guard child were unowned.

## Why the build departed from the ratified letter, once

ARCH 6.5 said "within ONE mapping; refused at v1, no producer". Built: a range
may span SEVERAL mappings, all-or-nothing with a merge pass -- because the
merge makes an engine's grow / shrink ladder exactly two mappings, so a
whole-region protect over a partially committed reservation IS a two-mapping
range and Linux serves it; and the precheck makes a refusal change NOTHING,
which is stronger than Linux's partial failure. ARCH 6.5 is amended AS BUILT
with that reasoning (the operator is told in the chunk's report, not asked).

## The three findings the split made live, all closed in the same commit

1. A lazy VMA that is one PIECE of a Burrow uncharged the WHOLE Burrow's
   resident count at detach, once per piece (an I-32 under-count the D-3b
   split could already reach). Now `burrow_decommit` over the piece's own
   range before the unmap -- which also returns a detached piece's pages at
   once, the memory bar.
2. `clone_one_vma` shared an eager anon mapping across a fork when its PROT
   was read-only; with a raise available that is one address space's writes
   landing in another's. The test is on the CEILING now.
3. `addrspace_clone` minted one COW clone per VMA, so a split Burrow forked
   into k clones (k shares per page, the child charged k x resident, the
   parent never able to take a page in place). One clone per source Burrow,
   through the cursor.

Pre-existing, OWNED, not fixed: the phenotype `munmap` is window-confined,
so a MAP_FIXED mapping below `0x100000000` leaks silently (viv-pheno-probe
L21, every boot). Home: B-1a'.

## Verification

`specs/check-cow.sh` (all nine cfgs as claimed); the kernel suite 1632/1632 at
`-smp 4` and `-smp 1`, zero extinctions, the banner with `/protect-probe` (8
legs), `/protect-guard-child` (snare:segv, the expect-fault census) and the
rewritten `viv-pheno-probe` L22..L23h; four REDs each failing exactly its own
assertions (1630 / 1629 / 1630 / 1631 of 1632). The holotype audit and the
five-row SMP gate ride the chunk's close commit.

## The holotype audit (round 1, Fable 5.1, MODEL start == end; 0 P0 / 1 P1 / 1 P2 / 4 P3)

Read-only; every finding re-derived from the code. Not dirty by count; all
fixed in the close commit except F5, which is owned by B-1a'.

- **F1 [P1]** the FILE page-in slow path installed at the CURRENT `vma->prot`
  after a sleep during which a sibling's protect could lower the mapping to
  none: a user-readable RO leaf on a guard, no fault ever re-running step 2.
  Fixed: `file_fault_still_admitted` in both install paths; the page-in kept
  ([[sub-kernel-fault]]).
- **F2 [P2]** four quadratic list passes + an uncapped per-page PTE uninstall
  under a non-preemptible lock, reachable by an unprivileged Proc with 65535
  adjacent reserves. Fixed: one scan then successors in all six loops (the
  reprotect's four, the phenotype munmap's two); `mmu_uninstall_user_range`
  walks by subtree ([[sub-kernel-vma]], [[sub-kernel-mmu]]).
- **F3 [P3]** `clone_one_vma`'s `vma_alloc` failure arm cleared the CLONE's
  cursor through a rebound local, not the source's; masked by the sweep.
  Fixed ([[sub-kernel-addrspace]]).
- **F4 [P3]** the fixed-anon arm admitted `PROT_WRITE` alone and answered it
  ENOMEM. Fixed: `mmap_fixed_window` promotes W to RW
  ([[sub-kernel-syscall-dispatch]]).
- **F5 [P3]** the per-piece refund never reaches orphaned slots after a D-3b
  window replacement inside a touched lazy mapping -- an over-charge, the safe
  direction. OWNED, named at the site; B-1a' fixes it in
  `vma_replace_range_in`.
- **F6 [P3]** `mprotect(unaligned, 0, prot)` answered 0 (Linux: EINVAL); a
  no-op protect at `PROC_VMA_MAX - 1` answered ENOMEM. Both fixed.

Six regressions (`protect.file_pagein_racing_protect_bails_{single,cluster}`,
`protect.range_walk_is_linear`, `protect.uninstall_range_skips_absent_subtrees`,
`protect.noop_protect_needs_no_headroom`, `sys_mmap.fixed_anon_w_alone_maps_rw`)
+ viv-pheno-probe L23i; a combined RED fails exactly the six. Gaps the audit
named: the pouch (musl-native) substrate still maps `mprotect` to ENOSYS and
its `mmap` ignores prot -- VIVARIUM 6.21 "ended" is true for the Linux
phenotype only; that is B-1b's scope (browser-status).
