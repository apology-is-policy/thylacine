---
id: chg-2026-09-06-tapestryd-doc-absorb
type: chg
title: "absorb docs/reference/139-tapestryd (I-40 compositor): zero-fold, dossier is larger + ahead of the doc"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/139-tapestryd.md -> ABSORBED (I-40 audit-trigger surface)

Absorbed the 1219-line tapestryd compositor reference doc into a multi-redirect
stub. The owning dossier `sub-tapestryd` (1630 lines -- LARGER than the doc --
guarded-by inv-i40/i5/i34/i1/i45/i9, updated 2026-09-06) is ahead of the doc and
covers every atom, grep-verified: the I-40 present half / surface lifecycle (5
hits), weave-generation/resize (13), the orphan reaper (6), ring-scope/drain-cap
(9), pane tree (33), menus/grab/dismiss (28), chord/focus/zoom (38), and the
quiesce/scanout-release present bracketing (7 -- "scanout off before the resource
dies").

Zero fold. The doc is a compositor-arc snapshot (G-3/G-6a-c/H-3c/gather-grant/
reaper/idle-throttle); the dossier grew the later H-4 layouts + the Warp/WSI GPU
present classes on top (the V-3/W-3 rows of AUDIT-TRIGGERS.md).

Redirects: the compositor -> sub-tapestryd; the kernel R2-F3 orphan reaper
(kernel/weft.c) -> sub-kernel-weft (its orphan-reaper section, confirmed at the
125-weft absorption); the gather grant -> sub-libdriver-grant; libtapestry ->
sub-libtapestry; the model -> tapestry_present.tla.

92 -> 93 absorbed of 157. lint 0-fail.
