---
id: chg-2026-09-06-halcyond-freeze-pre-soundness
type: chg
title: "sub-halcyond de-stale: the freeze-mid-pre style-index soundness invariant (PL-arc R1 F1 [P0] + R2 F6) + session_init.rs added to code + test count 99->127 (13 modules)"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-halcyond]] (audit:hard, the untrusted transcript renderer -- format-fuzz
class), flagged by main on yip 0063: the halcyon PL-arc audit closed (R1 @46c3d9e5
+ R2 @7f96febd/38eeb85a) and its F1 [P0] fix + F6 [P3] regression are a soundness
property the dossier did not yet carry. MEASURE (the de-stale discipline) surfaced
two more staleness dimensions main's flag did not name.

- **The freeze-mid-`pre` style-index soundness invariant** (new bullet in
  Invariants enforced). When `freeze_open` freezes an open block while a `pre` is
  open, the pre is finalized INTO that block -- the one whose `styles` vec its
  cells' style indices name -- never carried to a fresh block; else `layout_block`
  OOB-panics on a stale index (`len is 0 but the index is 0`). `intern_style` is
  append-only / degrade-to-last (a handed-out index survives a ScrollOff interning
  more styles pre-freeze), `self.open` reassigned in the one `freeze_open`
  `mem::replace`. Both triggers reach the arm and are now regression-witnessed:
  the tile-split `set_max_cost` (R1 F1) and the `finalize_scroll_pending` ScrollOff
  (R2 F6), each discrimination-proven vs the layout.rs:473 OOB panic.
- **code: += usr/halcyond/src/session_init.rs** -- on disk (3 tests) but absent
  from the dossier's code list; it was the only file in the diff of on-disk src vs
  the code list.
- **Tests: 99 -> 127 across 12 -> 13 lib modules** (MEASURED via grep of #[test];
  matches main's "halcyond 127"). New breakdown: transcript 39, tile 17, input 12,
  tiles 10, layout 8, grid 8, menu 7, raster 6, chrome 5, status/select/downq 4
  each, session_init 3. Added the two freeze-mid-pre regressions to the pinned set.

updated: was already 2026-09-06 (my PL-4 fold @82e47377 was earlier today); the
F1/F6 landed after it, so the same-day date could not re-flag this via the stale
tool -- main caught it by hand. The upkeep model end-to-end: main rang (via
0063 + the No-dossier-change trailer on 7f96febd), the vault folds. This is the
first flag folded under the dossier-gate that landed earlier this run.
