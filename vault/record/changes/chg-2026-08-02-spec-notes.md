---
id: chg-2026-08-02-spec-notes
type: chg
title: "four spec notes -- and the coverage gap was a dossier gap wearing a spec-note mask"
date: 2026-08-02
arc: arc-vault
commits: []
touched:
  - sub-kernel-burrow
  - sub-kernel-asid
  - inv-i7
  - inv-i31
  - lock-burrow
  - lock-asid
  - spec-burrow
  - spec-asid
  - spec-handles
  - spec-debug-step
  - moc-kernel-memory
established:
  - sub-kernel-burrow
  - sub-kernel-asid
  - inv-i7
  - inv-i31
  - lock-burrow
  - lock-asid
  - spec-burrow
  - spec-asid
  - spec-handles
  - spec-debug-step
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-02
---
Batch 25, taking up task #37 -- the six TLA+ modules [[view-spec-coverage]]
reported as having no dossier. Main had moved one commit (a harness-only change
to the interactive suite); the branch was synced first. L-1 checked for the
THIRTEENTH time and still absent -- it exists, on `aux-2`, which main does not
contain.

**THE SHAPE OF THE GAP WAS NOT WHAT THE VIEW SAID.** The view reports a count
of modules with no spec note, and the obvious reading of "6 missing" is "write
six notes." Checking each module's subject first showed something else: **three
of the six had nothing in the vault to attach to.**

| module | subject | state before this batch |
|---|---|---|
| `handles.tla` | the handle table | swept |
| `debug_step.tla` | the debug fs | swept |
| `burrow.tla` | `kernel/burrow.c` | **no dossier** -- mentioned in two notes, subject of none |
| `asid.tla` | `arch/arm64/asid.c` | **no dossier, no mention anywhere** |
| `pty.tla` | the ptyfs server | **unswept** (2,168 lines) |
| `tapestry_present.tla` | the compositor | **unswept** (6,746 lines) |

And one layer down: the two invariants those modules pin -- I-7 and I-31 -- had
no notes either, alongside I-4, I-6, I-12, I-36, I-42. So the "missing spec
note" was the visible end of a cluster: **dossier, invariant, and spec note all
absent for the same surface**, and only the last of the three had a view
counting it.

The dependency was therefore pulled forward rather than worked around, per the
chunk-completeness rule: [[sub-kernel-burrow]] and [[sub-kernel-asid]] were
swept from the code, [[inv-i7]] and [[inv-i31]] written, [[lock-burrow]] and
[[lock-asid]] registered, and then the four spec notes that now have subjects.
Ten notes for what the view scored as a four-note gap.

**F1 -- THE FUNCTION THE ASID LAYER IS NAMED AFTER DOES NOT EXIST.** Four
scripture locations name `asid_check_and_switch`: the audit-trigger row in
`CLAUDE.md`, the same row in `ARCHITECTURE.md`, the architecture's own wiring
paragraph, and the action-site map in `SPEC-TO-CODE.md`. The entry point is
`asid_resolve` and has been since it landed; nothing named
`asid_check_and_switch` appears anywhere in the tree.

Where it appears is what makes it more than a typo. The action-site map exists
so a reader can get from a model action to the code realizing it, and the
audit-trigger row is the prosecution list an auditor works from. Both point at
a name that cannot be grepped. The map's fast/slow split is mis-attributed
besides -- it assigns the fast path to the phantom name and the slow path to
`new_context`, where in fact both halves are `asid_resolve` and `new_context`
is the helper the slow half calls under the lock. Task #39.

**F2 -- A HEADER THAT CONTRADICTS ITS OWN ENUM, IN THE SAME FILE.**
`burrow.h` opens by saying the backing type is "`BURROW_TYPE_ANON` at v1.0;
PHYS at Phase 3; FILE post-v1.0" and, twenty lines later, "At v1.0:
`BURROW_TYPE_ANON` only." Sixty lines below that, the enum defines **six**
types -- including the two demand-paged ones and the executable-memory one --
each with a substantial comment of its own. The stale text is the file's
opening, which is what a reader reads first. Task #40.

**F3 -- I-4 HOLDS VACUOUSLY, AND THE CORRECTION WAS FILED ABOVE THE ERROR
RATHER THAN APPLIED TO IT.** `handles.tla` models a cross-Proc handle transfer
and proves no handle arrives by any other route. There is no transfer codepath
in the tree -- not a stub, not a rejected call, nothing; the name survives only
in comments. So the invariant is proven of a system where the sole cross-Proc
route is unbuilt.

That is the safe posture and a fine place to be. What is not fine is that
`SPEC-TO-CODE.md` says both things: a currency note at the top of the section
states plainly that no transfer codepath was ever built, and the table below it
still says the path is "defined as a stub, returns unsupported." The correction
was written **above** the stale text instead of replacing it, so the document
contradicts itself and the wrong half is the one in the table a reader
consults. Task #41.

**F4 -- A FOURTH KOBJ PARTITION IN THE CODE, THREE IN THE MODEL.** The
asynchronous-ring object kind landed as a fourth non-transferable partition
with its own mask; `handle.h` now carries seven static assertions where the
model knows of three sets. Its non-transferability is enforced structurally and
identically to the others, so nothing is unsound -- but it is held by the
compiler alone, with no counterexample behind it, and the model cannot state a
property about it. Same fail-safe direction as F3, which is why neither was
noticed. Task #41.

**F5 -- THE P1 LIVED BETWEEN TWO CORRECTLY-SCOPED SIBLING MODELS.**
`debug_step.tla` has five actions: request-step, tail, step-execute,
death-wake, publish-death. There is **no stop action** -- the stop was the
sibling model's subject. But a step window can be interrupted two ways in the
real system, by a death *and* by a peer-initiated whole-Proc stop, and the
tier's one P1 was exactly the second: a `step` superseded by a stop left the
armed flag set, so the next resume armed a spurious single-step.

Neither model could have caught it. The step model has no stop; the stop model
has no step. Each is sound in its own scope and the bug lived in the space
between them. That is the real cost of the sibling pattern -- which was adopted
for a good reason, to keep an audited base's counterexamples stable -- and it
is worth stating beside the benefit rather than only in the audit that found
it.

**THE THEME, WHICH IS AGAIN BATCH 24'S ONE LAYER DOWN.** Batch 24: the
assertions pin the values and nothing pins the description of the values. Here:
**the models pin the mechanisms and nothing pins the model's own scope against
the code's growth.** Four of the four modules read had drifted from their
subject -- a phantom function name, a three-of-six type coverage, a missing
fourth partition, an unbuilt action still described as a stub -- and in every
case the drift is invisible to a green TLC run, because a model cannot notice
that its subject grew.

Each spec note therefore carries an explicit "deliberately beneath the model"
section, and where the code has outgrown the model the note says so under its
own heading rather than in a footnote.

**WHAT THIS BATCH DELIBERATELY DID NOT DO.** The notes for `pty.tla` and
`tapestry_present.tla` are not written. Both model userspace servers the
sweep has never covered, and a spec note whose action-site map points at code
nobody has read would be a hollow record of exactly the kind this arc keeps
finding. They are blocked on two server sweeps, named with their sizes above,
and the coverage view will keep scoring them missing until then -- which is the
intended pressure.

PROBE. Two, plus a control, on the guard this batch's finding is about: would
the linter have stopped a spec note with no subject?

**P1** -- point a spec note's `models:` at a dossier that does not exist:
**caught**, `unknown id`. So a note attached to nothing cannot land.

**P2** (control) -- point it at a dossier that exists and is **entirely
unrelated** (the ASID allocator's model claiming to model the pouch network
shim): **passes clean**. The guard is *existence*, not *correspondence*.

That is the honest limit and it is the reason this batch's finding came from
reading code rather than from running the linter. "Write six spec notes" could
have been discharged by pointing all six at whatever dossiers happened to
exist, and the corpus would have linted green, the view would have read 33/33,
and four modules would have had a note describing something else. Same shape as
batch 24's control, where a gutted spec note still passed and still read
`dossiered`.

LEDGER. Spec coverage 27 -> **31 dossiered, 2 missing, 33 modules**. Kernel
memory area 2 -> 4 dossiers. Invariants 26 -> 28. Locks 26 -> 28. Corpus 774 ->
784. Absorption unchanged at 46/101/147 -- deliberately; `20-burrow.md`,
`22-asid.md`, `19-handles.md` and `134-debug-fs.md` now have somewhere to be
absorbed *to*, which is the point, but absorbing them is its own pass.
