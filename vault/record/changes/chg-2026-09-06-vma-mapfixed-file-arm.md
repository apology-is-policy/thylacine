---
id: chg-2026-09-06-vma-mapfixed-file-arm
type: chg
title: "kernel-vma de-stale: the DISTRO D-3 file-backed-mmap surface -- the MAP_FIXED split/replace (vma_replace_range_in), the sleeping-free-under-lock deferral (vma_free_deferred, F1/F5), and vma_next_overlap_in (#199)"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-vma
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
[[sub-kernel-vma]] read `updated: 2026-08-16`, entirely about the anon/COW list;
the DISTRO D-3 arc (five commits `ac337061..HEAD` on `kernel/{vma.c,vma.h}`,
+278 lines) added a whole file-backed-mmap surface it did not mention.
Ground-truthed by reading the diff and the current code, not the stale tool's
merge date. The header comments are essentially the spec; three real additions
folded:

- **The MAP_FIXED split/replace** (`vma_replace_range_in`, D-3b). Places a
  mapping at a chosen address, splitting whatever is there around it -- the
  primitive musl's `map_library` needs (reserve a whole-span, overlay each
  PT_LOAD). Two shapes only: wholly-inside-one-VMA (split into left/mid/right) or
  entirely-free (plain fixed insert); the free arm is NOT optional (refusing it
  answered `ENOMEM`, unreadable from real pressure -> OOM, #196), and spanning /
  partial-overlap is refused (partial unmap is post-v1.0). Two load-bearing
  properties: NO HOLE on any failure path (the old VMA is REUSED as the survivor,
  shrunk in place, so a rollback restores three fields rather than re-inserting a
  torn-out mapping; the exact-cover re-insert is provably infallible under the
  held lock into the vacated range below the cap), and the survivor keeps its
  `(burrow, offset)` EXACTLY (`burrow_offset + (va - vaddr_start)` invariant),
  which lets the caller uninstall only the replaced window's PTEs and makes the
  file-fault arm's #190 post-sleep geometry check correct against a concurrent
  split. SHARED_IN / COW / CODE-alias refused (F8 parity guard).
- **The sleeping-free-under-lock deferral** (`vma_free_deferred`, D-3c F1/F5).
  A 9P-backed FILE Burrow's free reaches a possibly-sleeping `spoor_clunk`, and
  every mutator holds `as->lock` (a spinlock) -- an inline free is the
  lock-across-sleep extinction. The deferred twin settles the I-32 uncharge under
  the lock but hands the physical free back to the caller (past the unlock, via
  `burrow_free_deferred`). `vma_drain_in` drains a `deferred_free_next` stack;
  the split's exact-cover arm returns its dead Burrow through a MANDATORY
  `out_free` (F7: a NULL `out_free` LEAKS it -- worse than F6's inline free).
  Fourth site of the F1 hazard; latent today (devramfs execs do not sleep), live
  for a 9P-paged exec text Burrow (D-4/D-5).
- **`vma_next_overlap_in`** (#199): the lowest VMA overlapping `[lo,hi)`, the
  range scan the phenotype munmap row needs to tell a boundary-straddle (refused)
  from a wholly-unmapped range (Linux success) -- the point-probe `vma_lookup`
  is blind to a VMA lying strictly inside a range.

Folded into Contract, Mechanism (a new split subsection), Concurrency (the
deferred-free discipline), Invariants (the split adds no second I-12 gate),
Prosecution, and Seams. `updated:` -> 2026-09-06. Stale backlog 41 -> 40.
