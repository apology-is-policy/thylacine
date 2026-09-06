---
id: chg-2026-09-07-utopia-skeleton-doc-absorb
type: chg
title: "absorb docs/reference/91-utopia (the U-3 ut skeleton): clean redirect to sub-utopia-interactive (superseded skeleton, ahead of the doc)"
date: 2026-09-07
arc: arc-vault
commits: ["ed03545c"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
The U-3 skeleton doc (libutopia palette/ansi/path + a banner-and-exit ut). quaestor
owner: all 3 sources -> sub-utopia-interactive (audit:light, updated 2026-09-05 --
fresh, laps the skeleton entirely). Verified atom-by-atom.

ALREADY COVERED (verified, sub-utopia-interactive is AHEAD):
- The whole libutopia crate (line_editor/repl/completion/palette/ansi/path/lib) +
  the ut binary -> sub-utopia-interactive (owns all of usr/utopia/libutopia/src/*).
- The palette: the doc's 4 Pale Fire roles (BG/FG/PATH/GLYPH) grew to NINETEEN
  semantic roles, and Pale Fire was RENAMED to Bonfire -- sub-utopia-interactive
  carries the rename AND is ahead (the finding that the rename "reached the
  definition and nothing else", 12 stale Pale-Fire refs across 7 files incl. this
  doc's own sources). Role names are the stable interface, hex is not.
- ansi helpers, abbreviate_home (~ / partial-component-not-matched), the
  turnstile/continuation glyphs, banner-via-SYS_PUTS-not-fd1 -> all there.
- U-5 parser / U-6 eval (listed as future in the skeleton) -> sub-utopia-parser +
  sub-utopia-eval.

Zero-fold. Superseded skeleton (banner-and-exit ut is now the full interactive
shell). Redirect stub. Zero code change.
