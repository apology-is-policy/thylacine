---
id: chg-2026-09-28-f2-share-move
type: chg
title: "A backgrounded leaf is transparent to a newcomer's share and to a move: the mean over the divided siblings, the nearest visible neighbour"
date: 2026-09-28
arc: arc-tapestry
commits: ["*(pending)*"]
touched:
  - sub-tapestryd
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-28
---
The manual chunk's device run found a new pane taking two thirds of an equal
share: after a divider drag and a double-click, Super+H's newcomer got 313 px
beside 471 and 470. A probe (`cat /dev/tapestry/layout` in a tile) showed the
session's root row holding the console renderer's leaf -- backgrounded, weight
1 -- beside tiles whose weights the drag had made their extents, and
`sibling_mean` averaging all three. The same probe showed a second missed
consumer of KT-1.5d-3 F2's structural transparency: `move_dir` traded the
row's first tile with the hidden leaf, changing the tree and not the screen.
The mean now runs over `divide_list`'s children and the swap takes the nearest
sibling that is not backgrounded ([[sub-tapestryd]]); HALCYON-INSTRUMENT 5.2
and TAPESTRY.md (d) say so. Three host tests on the device's tree, red on the
old code, pin both. The operator-ratified rule already covered these ops by
its own words ("transparent to a session's structural ops"); this applies it
where the first pass missed.

The fix's own Fable round found the class in halcyond too: its stack facts
(`parse_tree`) and its RESET planner (`reset_plan`) read the dump's rows
without the `backgrounded` token, so a root row stacked by Super+S numbered
its shown tiles 02 and 03 of 3 -- the last shown tile's close box closed it,
and the session with it -- and RESET planned a focus on the console leaf,
which the compositor refuses ([[sub-halcyond]]). The chunk's round 3 added
one Tab-arm case: an empty open tab of several took the placard's geometry
while halcyond painted a header across it (`place_frame` now takes the
container's shown count).
