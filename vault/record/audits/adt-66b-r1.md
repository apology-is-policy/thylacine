---
id: adt-66b-r1
type: adt
title: "#66b (PgrpMount.mp_path + /proc/<pid>/ns) focused round"
date: 2026-06-12
scope: [sub-kernel-territory, sub-kernel-path]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-66b-r1-f1, fnd-66b-r1-f2, fnd-66b-r1-f3]
round-of: chg-2026-06-12-66b-mp-path
created: 2026-08-01
---
## Scope

The territory-side mirror of the Path substrate: `mp_path`'s four-hook
lifecycle, `territory_format_ns`, the `DEVPROC_READ_BUF` growth, and the
`/proc/<pid>/ns` consumer. Fable formal round (MODEL start==end, no
fallback) plus a concurrent self-audit.

## Convergence

CLEAN. Fable INDEPENDENTLY re-derived the entire self-audit sound set
with the same chains — the four-hook refcount balance including the
clone-OOM rollback, MREPL's ref-new-before-unref-old (which survives the
degenerate shared-`Path` case), unmount's drop-before-overwrite with the
moved entry's ref transferring via the struct copy, the acyclic
`g_proc_table_lock -> ns_lock` edge, the cross-Proc lifetime envelope,
and [[inv-i33]]'s grep-complete write-only property.

The value Fable ADDED over the self-audit is a good illustration of what
a second prosecutor is for: it VERIFIED a claim the self-audit had only
ASSERTED. The code justifies running `path_unref` in place under
`ns_lock` (while `spoor_clunk` is deferred outside) on the grounds that
it is "non-sleeping" — Fable actually walked the secondary
`ns_lock -> slub c->lock` edge into `kfree`, confirmed it is a
non-sleeping leaf with `spin_lock_irqsave` and no reverse edge, and thus
that the nesting is sound rather than merely asserted to be.

The three P3s are cosmetic or coverage. [[fnd-66b-r1-f2]] sharpened a
self-audit note into a real malformed-output bug; [[fnd-66b-r1-f3]]
corrected a headroom claim; [[fnd-66b-r1-f1]] tracked a coverage gap
with explicit justification rather than dropping it. The withdrawn item
— that the render extends an IRQs-off window — was correctly judged a
latency note against the pre-existing `format_status` precedent.
