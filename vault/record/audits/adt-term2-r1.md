---
id: adt-term2-r1
type: adt
title: "term-2 round: the wga narrowing (name-specific dentry invalidation)"
date: 2026-07-12
scope: [sub-kernel-larder]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-term2-r1-f1, fnd-term2-r1-f2, fnd-term2-r1-f3]
round-of: chg-2026-07-12-term2-dentry-name
created: 2026-07-31
---
Opus-4.8-max (Fable ran out of credits mid-run → re-run on the Opus
fallback tier; MODEL start==end on the recorded round — the
independence forfeit noted per the reviewer-model rule). Surface A (the
narrowing) SOUND with three P3s, all pre-existing or hygiene — the
narrowing itself introduced nothing: the mutation enumeration proven
complete for name-specificity (no synchronous op changes a SIBLING's
existence without a create/unlink/rename on that sibling; mknod/symlink/
link have no Dev vtable slot; Loom async mutations never touched the
Larder in EITHER version); the unconditional gen bump preserves the
resurrection close even when the mutated name was not cached; negative
create/rename fills dropped by exact key; the whole-parent drop proven
NOT accidentally load-bearing. The round's second surface (B — the
"FOUNDATIONAL" disposition of the remaining S3 gap) was a
measurement-honesty prosecution, out of this sweep's code surface: it
demoted the headline to FIXABLE-VOTED-pending on two owed measurements
(the never-run W2 split of compile-CPU vs the spawn floor; the
unmeasured bulk-prefetch ceiling) — recorded in the go-build mission
register, not as code findings here.
