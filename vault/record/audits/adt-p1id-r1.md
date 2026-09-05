---
id: adt-p1id-r1
type: adt
title: "P1-I-D — the Phase-1 closing audit (allocator slice)"
date: 2026-05-05
scope: [sub-kernel-mm-phys, sub-kernel-mm-slub]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: clean
counts: {p0: 0, p1: 2, p2: 4, p3: 5}
findings: []
round-of: chg-2026-05-05-p1id-closing-audit
created: 2026-08-01
---
## Scope

The Phase-1 exit prosecution — wider than mm (kaslr, mmu, boot
relocations rode the same round); this record keeps the whole
round's counts and the mm-relevant finding list in the chg
([[chg-2026-05-05-p1id-closing-audit]]): F29/F34 reservation
disjointness, F32 kfree interior-pointer validation, F33 explicit
full-slab tracking, F35 the struct-page size pin, F37 the
order-corruption guard.

## Why findings: [] on a round with 11 findings

The close commit attributes severities only in aggregate; no
per-finding tags survive. Rather than mint fnd notes with guessed
severities, the Record keeps the aggregate here — the individual
mechanisms are documented where they live, in the dossiers'
prosecution and error-path sections.

## Verdict

Clean close (all fixed; 2 P3 deferred by name). Fourteen months on,
every F-mechanism is still in the tree and still load-bearing; what
did not survive was the documentation — both reference docs kept
teaching the pre-audit behavior ([[chg-2026-08-01-mm-ipc-sweep]]).
