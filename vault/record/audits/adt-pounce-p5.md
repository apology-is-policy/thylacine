---
id: adt-pounce-p5
type: adt
title: "POUNCE P-5 — the arc-close focused round"
date: 2026-07-07
scope: [sub-kernel-stalk, sub-kernel-ninep-client, sub-kernel-ninep-wire, sub-kernel-ninep-dev9p]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 1}
findings: [fnd-pounce-p5-f1]
round-of: chg-2026-07-07-pounce
created: 2026-08-01
---
## Scope

The whole POUNCE arc (P-2 wire + P-3 pounce + P-4 consumers + the pouch
probe) against the ARCH §25.4 POUNCE prosecution row. Fable
holotype-reviewer (MODEL start==end) + a concurrent 24-check self-audit
(no new findings). Single round, converged.

## Convergence

The fail-ordering invariant — the designated #1 target — SOUND under
every constructed adversarial path: present-vs-missing masking under a
forbidden dir, mount-mid-run, `.`/`..` interleaving, partial at index 0
and >0, the query form; the check ORDER proven byte-identical to the
per-component loop, non-vacuously (`pounce_acces_masks_noent` on a real
0644 dir + CAP_NONE proc). Fid/Spoor/Path lifecycle balanced on ALL
early exits (shape-violation / sentinel / first-miss / partial /
split-discard / split-race / query-success / X-denial / 3 OOM arms);
NOFID binds nothing on either end; the sentinel is BSS,
address-compared, intercepted before `walkqid_free` at BOTH call sites.
Three P-3 PRE-COMMIT self-audit catches are part of this preamble
(fixed in `d9fdff5e`, severities never assigned): SA-1 — the split
re-walk's error path could route the STATIC sentinel into
`walkqid_free` (heap corruption) → explicit guard; SA-11 — the shape
check accepted a full BIND walk whose Dev failed to transition nc
(pushing it would clunk the parent's SHARED fid) → `shape_ok`; SA-15 —
the query arm required `stat_out` but not `stat_done` → both. The one
formal P3 (the latched-fallback double base X-check) was TRACKED, not
fixed at close — reordering would churn the just-audited deny/release
ladder → [[seam-372-latched-double-xcheck]]. Posture: SMP gate 40/40
(0 corruption), 1038/1038 in-VM, the P-4 measure + the Phase-2
attr-cache DEFER decision on the recount.
