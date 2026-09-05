---
id: chg-2026-09-05-f2-hosting-fan
type: chg
title: "KT-1.5d-3 F2: backgrounded-leaf tiling + structural transparency + the hosting-fan defect (calc_geom_sig folds the hosted incarnation)"
date: 2026-09-05
arc: arc-tapestry
commits: ["53ee407f"]
touched: [sub-tapestryd, seam-tapestry-battery-unowned]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-05
---
F2 completes d-1b: the compositor decides backgrounding from the tree before
recompute and excludes the backgrounded (console) leaf from tiling; a
backgrounded leaf is transparent to a session's structural ops (two operator
votes). Adapting the acceptance battery to the new geometry (a third vote)
dug up a PRE-EXISTING defect: `calc_geom_sig` hashed only leaf ids + rects,
so a hosting into an already-split empty leaf ran non-structural and the new
surface never received its CONFIGURE offer -- every explicit-split-then-spawn
and the H-4b claim placement were exposed, masked by gates that hosted
through the split arm and clients that pre-size to the pane. The fix folds
the hosted surface incarnation into the signature; test-mode logs every
resize-ack rejection's discriminant. [[sub-tapestryd]] gains the section
"Backgrounded-leaf tiling, structural transparency, and the hosting-fan
defect". Not audit-closed here: rides the batched KT-1 holotype. Lesson: a
state signature that omits a dimension makes every change along it
invisible; read an error's discriminant before theorizing.
