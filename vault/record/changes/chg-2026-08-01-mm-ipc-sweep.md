---
id: chg-2026-08-01-mm-ipc-sweep
type: chg
title: "Vault sweep batch 9: memory + ipc-wake"
date: 2026-08-01
arc: arc-vault
commits: []
touched: [sub-kernel-mm-phys, sub-kernel-mm-slub, sub-kernel-poll, sub-kernel-pipe, sub-kernel-torpor]
established: [moc-kernel-memory, moc-kernel-ipc-wake, sub-kernel-mm-phys, sub-kernel-mm-slub, sub-kernel-poll, sub-kernel-pipe, sub-kernel-torpor, spec-poll, spec-pipe]
closed: []
opened: []
mirrors-checked: [inv-i9, inv-i24]
depth: rich
---
## What

Two areas established from full code reads (mm/ 1591 lines,
poll/pipe/torpor 1996 lines), six reference docs absorbed
(06-allocator, 07-slub, 51-pipe, 52-sys-pipe, 72-poll, 80-torpor),
two spec notes, six locks, eight seams, and the Record backfill:
two arcs, eighteen chgs, eight adts, fourteen fnds.

## The staleness harvest (the fourth consecutive instance)

Every absorbed doc carried the additive-maintenance signature; three
carried the assert-and-opposite form batch 8 named:

- 06-allocator's head claims refill amortizes one lock acquisition
  per 8 pages; its own appended #807 paragraph describes per-page
  acquisition (the code header explicitly corrects the head's
  claim). `struct page` given as 32 bytes throughout — 48 since
  P1-E — with the array math wrong alongside.
- 07-slub teaches "we don't track full slabs separately" twice in
  prose while its struct listing's field comment says "full list" —
  the F33 author updated the comment and left the prose.
- 51-pipe pins the ring at 72+4096 while 72-poll next door documents
  the true 88+4096 — the corpus asserted two sizes for one
  `_Static_assert`-pinned struct.
- 72-poll's "no fd ownership transfer … no such path exists" caveat
  is a soundness argument INVERTED by the multi-thread lift and the
  RW-2 retain that closed it.
- Three docs carried a "no EINTR" fossil across #811/LS-5c/#19.

Beyond docs, three CODE headers are stale (`pipe.h`'s non-blocking
semantics block, `phys.h`'s P1-D PA-as-void* comment, `syscall.h`'s
SYS_POLL `PROC_HANDLE_MAX = 64` bound) and CLAUDE.md's audit table
names a phantom file (`mm/vmo_pages.c` does not exist). Code and
scripture fixes are main-track work; the dossiers carry them as
caveats until then.

## Structural notes

- mm deliberately guards no numbered invariant — recorded as such
  rather than inventing an attachment ([[moc-kernel-memory]]).
- The P1-I-D round's per-finding severities are unattributed in the
  close commit; the adt keeps aggregate counts and no fnd notes were
  minted for it ([[chg-2026-05-05-p1id-closing-audit]]).
- [[fnd-poll-r1-f3]] → [[fnd-rw2-2cf1]] is recorded as a linked
  pair: document-the-precondition at P5, the precondition voided by
  the lift, the class closed at RW-2.
