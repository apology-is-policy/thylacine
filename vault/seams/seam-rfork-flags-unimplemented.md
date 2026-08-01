---
id: seam-rfork-flags-unimplemented
type: seam
title: "Eight of the nine Plan 9 rfork flags extinct"
status: open
surface: [sub-kernel-proc]
opened-by: chg-2026-05-05-p2d-rfork-exits-wait
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`rfork` accepts `RFPROC` and nothing else. `RFMEM`, `RFNAMEG`, `RFFDG`,
`RFCRED`, `RFNOTEG`, `RFNOWAIT`, `RFREND` and `RFENVG` are all DEFINED in
`proc.h` and all EXTINCT the kernel if passed.

That is the right failure direction — loud, not silent — but it means the
Plan 9 primitive the OS advertises is a single fixed mode: always a new
Proc, always a cloned Territory, always a fresh handle table, always a
copied environment.

## What closes it

Per-flag work, each with its own sharing question. `RFNAMEG` (shared
Territory) is the one with visible pressure: it would make
[[sub-kernel-territory]]'s refcount genuinely multi-Proc, which several
comments already anticipate. `RFENVG` is reserved and explicitly deferred in
the env-group design.

## Risk while open

None to soundness — everything downstream is written against the
single-mode assumption, and several invariants ([[inv-i1]] most directly)
currently hold BY CONSTRUCTION because sharing does not exist. The risk is
the reverse: those proofs get quietly weaker the day a share flag lands, and
the places relying on "always cloned" are not all marked.
