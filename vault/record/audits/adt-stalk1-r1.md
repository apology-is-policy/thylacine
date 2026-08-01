---
id: adt-stalk1-r1
type: adt
title: "stalk-1 (the resolver + SYS_OPEN) round 1"
date: 2026-06-02
scope: [sub-kernel-stalk]
reviewer: opus
model-start: "claude-opus-4"
model-end: "claude-opus-4"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-stalk1-r1-f1, fnd-stalk1-r1-f2, fnd-stalk1-r1-f3]
round-of: chg-2026-06-02-stalk1
created: 2026-08-01
---
## Scope

The multi-component resolver + `SYS_OPEN = 65` (commit `acd95470`).
Background Opus prosecutor (session agent `a0e76cb5`; clean kernel
compile, static prosecution) + an in-session self-audit, merged.

## Convergence

CONVERGED on SOUND: the I-28 `..` containment (pop only at depth > 0;
no negative depth; excess `..` clamps at `start`), the per-component
X-search with no skippable hop (O_PATH skips only the final R/W + open,
never the path X-search), and the N-hop Spoor lifetime (the shared-aux
detach-vs-clunk discipline correct on every failure branch; `start`
never reffed/clunked; the popped quarry clunked exactly once; the
0-component clone-walk mints a fresh fid so clunking it never touches
`start`'s). Tokenizer bounds exhaustive (clen ≥ 1 always; over-long
rejected before the namebuf write; depth checked before the push). All
12 unit tests verified non-vacuous. Matrix at close: default(smp4) +
smp8 + UBSan 698/698 + the joey E2E + 0 EXTINCTION. The dev9p multi-hop
lifetime was covered E2E-behaviorally (the joey probe), not by an
isolated dev9p unit test — the design's test split, noted not defected.
