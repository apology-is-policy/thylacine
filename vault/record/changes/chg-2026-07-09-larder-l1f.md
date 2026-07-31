---
id: chg-2026-07-09-larder-l1f
type: chg
title: "Larder L1f: the arc close — audit (1 P1 fixed) + 40/40 SMP gate + the honest re-measure"
date: 2026-07-09
arc: arc-go-build
commits: ["f5549b48"]
touched: [sub-kernel-larder, sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The L1 arc close: the focused adversarial round over the whole surface
([[adt-l1f-r1]] — 0 P0 / 1 P1 / 0 P2 / 2 P3, NOT dirty; the P1 = the
reused-ino PAGE-invalidate-on-create gap, the exact page twin of the L1c
attr defense, fixed in this commit with the non-vacuous
`create_invalidates_reused_child_pages` regression), the FULL SMP gate
(default+UBSan × smp4/smp8, N=10 = 40/40 PASS, 0 corruption — the boot
chain's shared-client storm as the live SMP witness), and the gofmt
re-measure: warm 1352 → 1147 ms (−15%), cold ~flat. The re-measure was
CHASED to ground rather than banked: a trivial warm hello build is
already 987 ms, so the warm build is ~86% fixed go-tool overhead — the
Larder captured its full addressable FS-redundancy band and the §10
prediction had over-attributed the warm cost to eliminable FS ops. The
next lever named honestly (exec/page-in/build-graph), which is what the
fid-lifecycle + loose-mode arcs then attacked.
