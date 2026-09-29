---
id: chg-2026-09-29-tile-selection-bands
type: chg
title: "A session tile draws its whole selection, as the console renderer does"
date: 2026-09-29
arc: arc-tapestry
commits: ["*(pending)*"]
touched:
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-29
---
The manual chunk's device run 11 pressed `v`, `k` and `y` in a session tile
and saw nothing change: a session tile banded only the Normal-mode cursor's
row (one `Mark`), while the console renderer bands every row of the
selection, so the `v` anchor TC-1b rebases in both hosts was never drawn.
`Tile::render_selected` now bands each row of the selection once -- the
frozen blocks, the open block and the live grid -- keyed as a `Mark` keys the
cursor; `render` is the no-selection form, byte-identical
([[sub-halcyond]]). A host test red under four sabotages and
`ls-halcyon-manual` leg 10a pin it.
