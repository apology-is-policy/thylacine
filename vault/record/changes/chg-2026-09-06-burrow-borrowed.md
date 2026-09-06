---
id: chg-2026-09-06-burrow-borrowed
type: chg
title: "sub-kernel-burrow re-verified borrowed: the only post-update change is a comment-only round-3 refinement the dossier's prose already reflects"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-burrow
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Flagged stale (~334 lines, "changed 2026-09-05" -- the merge date; the last actual
burrow commit is 2026-08-24). Ground-truthed by diffing the last burrow commit
(f7021c7a) to HEAD: burrow.h is unchanged, and burrow.c changed 14 lines in exactly
one commit -- `3de39ad0` (warp V-3b-1c-2b round-3 audit close, 2026-08-24 16:29, 37
minutes after f7021c7a). That change is COMMENT-ONLY: it rewrites `burrow_total_refs`'s
rationale from round-2's "IRQ-preemptible" to the true "SMP cross-CPU" (IRQ-masking
cannot serialize two CPUs; only `v->lock` can). The code -- the atomic sum under
`v->lock` -- is unchanged.

The dossier's `burrow_total_refs` prose already carries the SMP reasoning ("a peer
CPU mutating one count between them can make the sum read reclaim-safe while a
reference is genuinely in flight"), i.e. it was written reflecting round-3, not the
superseded round-2 IRQ framing. So nothing is owed: `updated:` -> 2026-09-06 and a
Provenance note, the caps stale-by-cotenancy pattern. guarded-by unchanged [inv-i7,
inv-i32].
