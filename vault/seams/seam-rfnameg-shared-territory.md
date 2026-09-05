---
id: seam-rfnameg-shared-territory
type: seam
title: "rfork(RFNAMEG) — cross-Proc shared namespaces"
status: open
surface: [sub-kernel-territory]
opened-by: chg-2026-05-13-p5-attach-mount
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Plan 9's `rfork(RFNAMEG)` gives a child the PARENT's Territory rather
than a copy, so a later `mount` by either is visible to both — the basis
of a shared-namespace process group. Thylacine extincts on the flag; the
`Territory.ref` field exists and is exercised, but at v1.0 the only
multi-holder case is the peer Threads of one Proc.

## What closes it

Small at the mechanism (skip `territory_clone`, `territory_ref` the
parent's, reserve the flag) and large at the consequences. Every place
that reasons "one Territory, one Proc" gets a second holder class:
[[inv-i1]]'s isolation stops being structural and becomes a real
property to check; `specs/territory.tla` must model the sharing (its
preamble already names this as the point where Isolation becomes a state
invariant rather than a data-model consequence); and
[[lock-territory-ns-lock]] plus [[lock-territory-dot-lock]] go from
"serializes peer Threads" to "serializes unrelated Procs" — which is the
same code but a much wider blast radius for any hold that is too long.

## Risk while open

None as an omission — the flag fails closed. The risk is that the
absence has been LOAD-BEARING in reasoning: several arguments across the
tree lean on "a Territory has one Proc" to bound a lifetime. Those need
re-walking when the flag lands, not just the locks.
