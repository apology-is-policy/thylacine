---
id: chg-2026-08-02-absorption-reconciliation
type: chg
title: "the absorption ledger, reconciled -- the sweep is a third done, and three stubs claimed more than they absorbed"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - view-absorption
  - arc-vault
established:
  - view-absorption
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 23. No subsystem swept: this one reconciles the arc's own ledger, which
batch 21 found drifting and could not stop to fix. Main had not moved since
batch 21 (`631c8ade`), so the branch was already synced; L-1 checked for the
ELEVENTH time and still absent.

**THE PREMISE WAS WRONG, AND IT WAS MY PREMISE.** The carried belief -- written
into the tracking task and into memory -- was that "the absorption ledger is
~25% done while the sweep is ~complete." Ground truth: **46 of 147 reference
documents are absorbed and 101 are live.** The sweep is a third done by
document, not complete. The belief came from a real event generalized one level
too far: `devices/` finished at batch 22, an AREA completed, and *area complete*
became *sweep complete*. Nothing checked it because nothing could -- the count
lived in prose, in two places, and prose does not disagree with a tree.

**WHAT THE MECHANICAL CHECK SAID, AND WHY READING CUT IT BY THREE-QUARTERS.**
Cross-referencing every stubbed document's pre-stub source citations against
every note's coverage flagged **eleven** documents as having absorbed code no
note covers. Reading them reduced that to **three**. Two citations were to files
that do not exist (a comparative reference to Linux's tree, a since-renamed
path); six were cross-references -- `132-larder.md` names `exec.c` once, as the
call site of an eager read; `39-hw-handles.md` names `fault.c` once, as where a
page-table entry is actually installed -- and those files' own documents
(`21-elf.md`, `27-exec.md`, `25-fault-dispatcher.md`, `30-dev-spoor.md`) are all
still live, so nothing was lost. **A path mention is a screen, not a verdict**,
and the screen over-reported by more than three to one.

**F1 -- THREE STUBS CLAIM MORE THAN THEY ABSORBED, AND ONE OF THEM IS
YESTERDAY'S.** A stub asserts that the document's content "now lives,
code-verified and current" in the notes it names. For three documents that
assertion is false in part:

- **`34-devramfs.md`** also documented **the cpio parser** (`kernel/cpio.c` +
  its header, 213 lines) -- the newc header's field-offset table, the iterator,
  the trailer. `sub-kernel-content` covers the filesystem that *consumes* the
  archive; nothing covers the parser that *reads* it. Stubbed at **batch 22**,
  in the area that batch declared complete.
- **`11-timer.md`** also held the only account of **the vDSO clock page**
  (`kernel/vdso.c`) -- the shared-page layout and the magic-and-version
  handshake a reader uses to detect a mismatch and fall back to the syscall.
- **`01-boot.md`** also held the only account of **the PL011 driver**
  (`arch/arm64/uart.c`, 473 lines) -- device-tree base discovery, the hardcoded
  fallback covering the pre-parse window, the register programming, the receive
  interrupt, and the line-break detection **the trusted path's attention key
  rests on**. The console dossier covers the console's *use* of the UART; the
  driver beneath it is in no note.

About 750 lines of kernel code whose only current description was deleted from
the place a reader would look. Not destroyed -- it is in git history, and the
stub names where the rest went -- but **orphaned**, and asserted absorbed.

**THE MECHANISM, WHICH IS THE POINT: A DOCUMENT'S NAME IS NOT THE EXTENT OF ITS
CONTENT.** Absorption keyed on the title. A sweep read `devramfs.c`, saw
`34-devramfs.md`, and stubbed it -- and the document had been documenting the
cpio parser too, under a name that said nothing about it. Coverage was tracked
by code file; absorption was decided by filename; the two were never compared.
Every instance has that shape, and it predicts the rest: any document covering
more code than its title names is a candidate, which is why the check has to
read the pre-stub text rather than trust the name.

**THE SECOND, DEEPER BLOCKER: A DOSSIER IS PROSE-SHAPED BY DESIGN, AND SOME
DOCUMENTS ARE MOSTLY TABLE.** Twelve live documents have a note that plainly
covers their subject -- `19-handles.md`/`sub-kernel-handle`,
`107-loom.md`/`sub-kernel-loom`, and ten more -- and stubbing them looks
overdue. It is not. The dossiers are markedly shorter (loom 334 lines against
the document's 1342; allowance 188 against 804), and the difference is not only
compression of chunk-history narration. `107-loom.md` carries the wire
structures field by field with their byte sizes and the operation-code roster;
its dossier says *"a 64-byte header, the submission index array, the 64-byte
entry array"* -- the sizes survive as prose, the layout does not. The schema's
fixed sections have no place for a layout table, and that is correct: layouts
belong to boundary notes. **The vault has exactly one, and it is a contract
(two strings on a serial line), not a layout.** So absorption of a
table-bearing document is blocked on a registry that has not been built.

**THE ARC'S ORDER WAS THEREFORE WRONG, AND IS CORRECTED.** The plan ran
`sweep -> registries -> cutover -> stub deletion`, with registries as a
tidying pass after the sweeps. In fact the registry passes are a **prerequisite
for completing absorption**: a document cannot be replaced until everything in
it has somewhere to live, and tables have nowhere until the boundary notes
exist. The exit criteria now say so.

**THE FIX IS A COMPUTED LEDGER, BECAUSE THE HAND-KEPT ONE IS WHAT ROTTED.**
[[view-absorption]] reads `docs/reference/` directly and reports, per document,
absorbed-or-live and into which notes -- so the count is derived from the tree
and cannot disagree with it. It gains a property the prose ledger never had:
since a stub lives in `docs/reference/`, **editing one without re-rendering now
fails the linter.** It also names what it does not check -- whether a note
actually covers everything its stub claims, which needs the pre-stub text and
therefore git history, which a renderer cannot read. That check stays manual and
its standing result is this note.

The renderer surfaced two anomalies on its first run, which is the argument for
it: a dangling link (mine, a forward reference to this note) and nine stubs
"citing" a note called `schema` -- a false positive, since `vault/meta/` is
deliberately outside the registry, now excluded.

PROBE. Three, each asserted on disk before linting, restored from copies taken
after the last real edit. **P1** -- mark a live document absorbed: caught,
stale generated body. **P2** -- point a stub at a note that does not exist: the
marker rendered and **the linter passed**, which is a half-measure in a batch
about silent drift, because a broken reference survives a re-render and so
would be reported forever without ever blocking. Fixed during the probe:
`checkViews` now fails on the marker, naming the missing note. Re-run: caught.
**P3** (control) -- an unrelated prose edit inside a stub: correctly passes.
That is the check's deliberate limit, and it is the one that makes this batch's
own repairs legal: state and references are guarded, narrative is not. So a
stub can still come to say something false about what it absorbed -- which is
exactly the defect found here, and no linter will catch the next one.

**THE THREE STUBS NOW NAME THEIR OWN GAPS** and point at task #32. Correcting
the claim was preferred to quietly sweeping the three files, for the same
reason the arc exists: the wrong thing to do with a false assertion is to make
it true in silence.

LEDGER, RECONCILED. **46 absorbed** (three of them over-claiming, now
annotated). **101 live**, in three states: twelve with a note covering their
prose and tables awaiting a boundary registry; the rest genuinely unswept --
most of userspace (the shell, the editor, the identity daemon, the compositor,
the ports) and a substantial kernel remainder (memory mapping, the fault
dispatcher, exec and the ELF loader, the syscall surface, the debug and
pseudoterminal surfaces). That is the real remaining sweep, and it is now
enumerable rather than estimated.
