---
id: seam-rfork-flags-unimplemented
type: seam
title: "Seven of the nine Plan 9 rfork flags extinct"
status: open
surface: [sub-kernel-proc]
opened-by: chg-2026-05-05-p2d-rfork-exits-wait
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-15
---
## Owed

`rfork` accepts `RFPROC` and `RFPROC|RFMEM`, and nothing else. `RFNAMEG`,
`RFFDG`, `RFCRED`, `RFNOTEG`, `RFNOWAIT`, `RFREND` and `RFENVG` are all
DEFINED in `proc.h` and all EXTINCT the kernel if passed.

That is the right failure direction — loud, not silent — and each remaining
flag is reserved to make its own case rather than inheriting approval from
`RFMEM`. But it still means the Plan 9 primitive the OS advertises is two
fixed modes rather than a composable word: the Territory is always cloned,
the handle table is fresh-or-copied but never *shared*, and the environment
is always copied.

## What closes it

Per-flag work, each with its own sharing question. `RFNAMEG` (shared
Territory) is the one with visible pressure: it would make
[[sub-kernel-territory]]'s refcount genuinely multi-Proc, which several
comments already anticipate. `RFENVG` is reserved and explicitly deferred in
the env-group design.

## Risk while open

None to soundness. Everything downstream is written against the
narrow-mode assumption, and several invariants hold BY CONSTRUCTION because
the corresponding sharing does not exist.

**The `RFMEM` landing is the first test of the risk this note named, and it
is worth recording how the prediction fared.** The stated risk was that
proofs relying on "always cloned" would get quietly weaker the day a share
flag landed, and that the places relying on it were not all marked. A share
flag has now landed — and [[inv-i1]] held anyway, because `RFMEM` shares
*memory* and the namespace proof rests on the Territory. The flag word is
exactly what made that separation available: two Procs on one address space
still hold two independent namespaces.

So the shape of the warning was right and its specific target was not
reached. That is not the same as the warning having been unnecessary. The
proof survived because the axis that landed happened to be orthogonal to the
one under it, which is luck of ordering rather than a property anyone
arranged — and the note's own point, that the reliant places are not all
marked, is untouched: nothing in the tree records *which* invariants rest on
*which* unimplemented flag, so the next flag has to be checked by hand
against the same unmarked set. [[inv-i44]] is now the invariant carrying the
sharing that did land.
