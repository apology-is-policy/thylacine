---
id: chg-2026-10-04-pty-fid-capacity
type: chg
title: "Account all six ordinary terminal handles"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-ptyfs, sub-halcyond, sub-substrate-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The live clipboard pressure test revealed two independent terminal limits.
Full-size hidden tabs retained triple-buffer weaves until the 128 MiB shared-map
ceiling refused another tab. A smaller-layout witness then failed at ptyfs's
80-fid table. An exact inventory showed six handles per normal terminal, against
a formula budgeting four. Corrected to 112 fids for the existing sixteen pairs,
with unchanged 80-operation queue bounds. The queue coupling was caught during
self-review; a new actual-dispatch regression guards it. Failed runs and the
fixture's response-buffer setup correction are recorded in the review.

Fresh final CI image passes CPU1 boot 1830/1830 and all ptyfs startup
selftests. The native graphical run retains fourteen authenticated controllers
across sixteen live PTYs, enters physical Ctrl+Alt+F10 SAK, then verifies every
old connection is closed and all fourteen controllers reconnect/read/cancel
successfully. Final run: 107.68 s at 1280x800. Captures and exact source/image
manifests are in work/oct4-hi-pressure/graphics-1791094456386770000.

This repairs HI1-R31 only. Hidden-buffer suspension is a separate, unratified
scope/lifecycle proposal; the original larger pressure failure remains open.
The full clipboard ledger and client/modal integration remain unfinished.
Single-agent WIP checkpoint, no Main landing or fresh broad gate claims.
