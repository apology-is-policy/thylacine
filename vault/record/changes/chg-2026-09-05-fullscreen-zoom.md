---
id: chg-2026-09-05-fullscreen-zoom
type: chg
title: "The fullscreen-zoom fix: the #56 patchwork latch re-keyed on slot rotation; the letterboxed compose clips to the damage's projection"
date: 2026-09-05
arc: arc-tapestry
commits: ["*(pending)*"]
touched: [sub-tapestryd, sub-libtapestry]
established: [adt-zoom-r1, haz-latch-keyed-on-proxy]
closed: [fnd-zoom-r1-f1]
opened: []
mirrors-checked: []
depth: skeletal
no-dossier-change: "the vault peer owns the sub-tapestryd dossier edit (yip 0052 turn 9) and folds the #56 latch discriminator in from these records after the landing; sub-libtapestry likewise (its H-3c-2 rewrite is in flight there)"
created: 2026-09-05
---
The operator's Cmd+F report, reproduced by aux (yip 0048) and run to ground here ([[adt-zoom-r1]]): DOSBox-X, a single-slot SDL client that presents partial rects, was latched an accumulator by the #56 patchwork latch and cropped at the content origin -- native at the corner in its tile, native top-left on black when zoomed ([[fnd-zoom-r1-f1]]). The latch now keys on the property it exists for: partial damage latches only on a surface that has presented two or more distinct slots (`Surface.slots_presented`); a single-slot presenter stays letterboxed and each partial present redraws only its damage's projection through the scale (`ComposeOp.clip` from `libhalcyon::place::scaled_clip`, host-tested against the compose's own mapping). `letterbox` lives in `libhalcyon::place` now, shared with the battery. libtapestry gains `Surface::set_single_slot` (thyla_tap's discipline for native clients + the battery's leg). Witness: the `singleslot` leg of ls-gfx-panes. The prose home is the commit message and `docs/TAPESTRY.md` (the placement paragraph); the class is [[haz-latch-keyed-on-proxy]]. Unaudited by the double-the-distance rule -- the prosecution notes ride AUDIT-TRIGGERS row 42 for the next tapestryd round. Real-DOSBox re-run owed to aux after the merge.
