---
id: chg-2026-09-06-halcyond-pl4-proportional-tail
type: chg
title: "sub-halcyond de-stale: the normal-screen tail is now PROPORTIONAL (PL-3/PL-4), retiring the mono paint_grid there; the run-menu geometry inverts through the laid tail (grid_hit + grid_run_rect)"
date: 2026-09-06
arc: arc-vault
commits: ["adc26ed0"]
touched:
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-halcyond]] (updated 2026-09-06, `audit: hard` -- the untrusted transcript
renderer, format-fuzz class), flagged by main on yip 0058: PL-4 (proportional-
live, @ae509368, commits 6270b132..5a1ce719) made the dossier's "fixed mono grid
tail" line stale for the normal screen. Folded the halcyond-specific delta after
fast-forwarding origin/main to 17b38711 (PL-4 in-tree).

- **The normal-screen tail is now PROPORTIONAL.** `Transcript::live_block` joins
  the live grid's soft-wrapped rows into logical lines (PL-4a carries the
  soft-wrap state; PL-3b/c rejoined scrollback likewise), and `tile.rs::render`'s
  normal path lays them via `live_block -> layout_block -> render_block`, RETIRING
  the mono `paint_grid` tail there. `paint_grid` survives for the ALT screen + the
  repainting-TUI case. Char-index caret, proportional selection banding, obj
  underline. Verified: the untrusted-drop clamp stays in `grid.rs` BELOW the
  layout swap (security semantics unchanged); `main.rs`'s console renderer
  untouched (it drives the `Transcript` run path).
- **The run-menu geometry inverts through the cached laid tail.** New
  `Tile::grid_hit` + `grid_run_rect` map a position to a run and back to its
  proportional rectangle (no longer a cell product), wired at BOTH menu-summon
  sites -- the mouse `click()` and the self-caught keyboard `act()` (the twin a
  one-site fix would miss). Verified present at tile.rs:206/:234.

Scope-checked so the fold is neither short nor bloated: `SpanTag` is still
`{block, obj, em, hdr}` (transcript.rs:145) -- PL-1's Beacon `pre` block rides
the existing `block` field, NO new field, so the dossier's SpanTag line is
current. PL-2 (Genera typography -- hdr by size+italic, bold reserved) lives in
the SHARED `layout.rs` (`laid_line_for`, used by console + tile), documented with
that layer, not duplicated here; the dossier already ties the tail to "the same
proportional layout as scrollback."

Note: `updated:` was ALREADY 2026-09-06 (the H-arc fold 29b3267c preceded PL-4
the same day), so the stale tool -- which dates by the `updated:` field -- could
NOT see this drift; main caught it by hand. A live instance of the same-day
blind spot behind this run's systemic findings (yip 0036).
