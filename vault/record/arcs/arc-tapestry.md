---
id: arc-tapestry
type: arc
title: "Tapestry — the graphics stack"
status: active
start: 2026-07-01
chunks:
  - chg-2026-07-20-g7a-sdl-seam
  - chg-2026-07-20-g7b-quake
  - chg-2026-07-22-55c-cons-winsize
created: 2026-08-01
---
The graphics arc: tapestryd (the compositor), the Weave share, aurora
(the console renderer), the pane tree, and the ports that prove it --
SDL2, TyrQuake. Its dossiers land with the graphics sweep; this arc note
exists so the pouch-side landings it drove ([[sub-pouch-thread]]'s
`nanosleep`, [[sub-pouch-fs]]'s `fopen` reroute, [[sub-pouch-net]]'s
nonblocking sockets, [[sub-pouch-tty]]'s console arm) have their true
provenance rather than a convenient one.

The pattern worth keeping: each of those four was found by a PORT
failing, not by a review. A frame pacer that busy-returns, a game that
finds no pak file, a multiplayer socket that cannot go nonblocking, a
console that is not a terminal -- four latent pouch gaps that no prover
had ever exercised.
