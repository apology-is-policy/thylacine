---
id: haz-budget-stored-not-derived
type: haz
title: "A budget on stored bytes does not bound the derived working set"
applies-to: [global]
instances: [adt-kt1-r2, adt-kt1-r3]
created: 2026-09-05
updated: 2026-09-05
---
## The failure shape

A container is budgeted by what it RETAINS (a byte cap on the stored set, enforced on push and eviction), and the fix is declared complete. But a consumer derives a working set FROM the retained set -- a layout of every block, a serialization of every record, a diff against every row -- and that derived set is a transient of the same order as the retained one (here ~1.8x), allocated on the same heap, charged to no budget. The retained cap is met; the process still dies at the first pass that builds the derived set from a retained set near its cap. Round 2 of the KT-1 audit found it in halcyond's `Tile::render` (every frozen block laid out per paint: one tile with ~20K rows of history, well under its 32 MiB share, OOM'd the whole session), and again in the kaua-term producer (one ScrollOff was capped; the number a single feed piles up before the first write was not).

## The tell

Round 3 added the class-shaped variant: a sink keyed on ONE record class (the capped ScrollOff) left the OTHER screen-sized class (the alt-screen full diff) piling up exactly as the first had -- eight bytes of toggle per full screen. A bound that names a class is not a bound on the container; the shipping trigger must measure what is HELD.


A fix that says "the sum must fit the heap" and budgets only what is stored. A per-item bound presented as a per-container bound (the per-record cap that leaves the per-feed count free). A render/serialize/diff loop that walks the whole retained set without a viewport or a flush inside it.

## The countermeasure

Bound the DERIVED set explicitly: window the derivation to what the consumer needs now (lay out the blocks in view, cache the cheap per-block facts -- a height -- that position the rest) and ship each capped unit as it forms (a sink inside the loop, not an emit after it). Then witness the transient, not the retained bytes: a host test that counts the units derived per pass and asserts they scale with the view, and one that feeds a pathological input and asserts what the sink sees at once.
