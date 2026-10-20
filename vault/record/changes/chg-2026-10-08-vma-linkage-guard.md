---
id: chg-2026-10-08-vma-linkage-guard
type: chg
title: "A VMA its address space's list does not hold is never removed, and the weft reaper unmaps only the space it locked"
date: 2026-10-08
arc: arc-boosty
commits: ["667a5de27", "91a3a6467", "7775ebedd", "61e5d66ac", "b7486e6b4"]
touched:
  - sub-kernel-vma
  - sub-kernel-weft
  - sub-kernel-death
  - sub-kernel-proc
  - sub-kernel-burrow
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-10-08
---
A hardening chunk between capmark and B-2c, taken by main at corona's ask
(yip 0202). Corona found, reading the B-2b hunks, that a second `vma_remove_in`
of the same Vma wipes the whole list: the first remove clears both links, so the
second takes the no-predecessor arm and writes `as->vmas = NULL`. No path does it
(five call sites, each removes once and frees). B-2b's `code_vmas` underflow
extinction was the one loud signal for it, and [[chg-2026-10-08-image-holder-record]]
retired that counter, so the chunk went in before B-2c.

**What** ([[sub-kernel-vma]]). `vma_linked_in(as, v)` asks the predecessor (or
the head, for the first mapping) to point at `v` and the successor to point back.
It is exact only for a Vma in `as`'s list or in none, which is every caller.
`vma_remove_in` extincts on a Vma that test refuses ("vma_remove of a Vma not
linked in this address space"). `vma_insert_in` now also refuses the sole
mapping (`as->vmas == v` with both links NULL), which its link test alone
could not see. The heritage is Linux's `CONFIG_LIST_HARDENED`
(`__list_del_entry_valid`: `prev->next == entry && next->prev == entry`, read
from `include/linux/list.h`). Linux's list is circular, so every linked entry
has two neighbours; ours is NULL-terminated with a separate head, where the sole
mapping and a never-linked Vma both have NULL links, so the head is asked too.

**The reaper's held space** ([[sub-kernel-weft]], [[sub-kernel-burrow]],
[[sub-kernel-proc]]). Audit r1 found a pre-existing P1 in the Weft orphan
reaper: its find callback locked `q->as->lock` under `g_proc_table_lock`, and
the sweep re-read `q->as` three times after the table lock dropped. Exec swaps
`p->as` under the table lock alone, so an exec in that window had the sweep
unlock the NEW space and leave the old one locked; exec's drain then spun on it
forever (I-8). The find now hands back the SPACE it locked
(`weft_reap_find_ctx.locked`) and the sweep works through that pointer alone,
via the new `burrow_unmap_in` / `burrow_unmap_reporting_in` (the Proc forms now
wrap them). No reference is taken: the reference count is an oracle (the
device-death quiesce, the image join and the spawn page stamp read `ref == 1` as
sole ownership), and the held lock already keeps teardown out, since every last
unref drains through `vma_drain_in`, which takes the lock first.

**A use-after-free race closed beside it** ([[sub-kernel-death]]).
`proc_quiesce_owned_devices` walks `p->as->vmas` lock-free while the Proc is
still ALIVE, and the reaper could unmap and free a Vma under it. The find skips
a Proc with `PROC_FLAG_EXIT_CLOSING` whose space it solely holds (both read under
the space lock; a shared space is still swept, so an RFMEM survivor keeps no
stale weave), and the quiesce takes and drops the space lock before its walk,
when it holds the space alone.

**Verification.** Audits by Fable 5.1 (start == end each round): r1 0/1/0/3
(F1 the reaper P1 above); r2 0/1/0/4 plus one self-found (F1: my r1 fix pinned
the space with a reference, which voided the `ref == 1` oracle -- replaced by the
held lock; the self-found item is the quiesce race above); r3 0/0/0/3, clean (F1
the reviewer's refinement of the exit-close skip to a sole space). RED in its own worktree (10:05-10:26Z): base and green 1963/1963 (main's 1959 plus four new witnesses), R1-R5 each red at its predicted assertion and nowhere else, M1/M3/M5 extinct with the guard's message inside the named scratch test, MX (both guards off) the three predicted FAILs; on b7486e6b4: suite 1963/1963, test-fault 8/8, ci-smp-gate N=10 50/50 over default-smp1/4/8 and ubsan-smp4/8, no corruption (10:26-11:49Z). No spec models the VMA list or the reaper; `burrow_unmap_in` is the old body against a named space, so `burrow.tla`'s mapping counts are untouched and no buggy cfg is owed.

**Corrections.** `proc.c`'s exec-swap comment claimed the reaper could not still
hold `old`; it now says the reaper keeps `old`'s lock by its captured pointer and
the drain waits it out. [[sub-kernel-weft]]'s reaper order was rewritten to the
find-and-lock shape.
