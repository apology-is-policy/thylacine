---
id: chg-2026-09-29-layout-notice
type: chg
title: "A session's layout notice is no longer lost with the surface that carried it"
date: 2026-09-29
arc: arc-tapestry
commits: ["*(pending)*"]
touched:
  - sub-libtapestry
  - sub-halcyond
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-29
---
The manual chunk's device run 13 restored a layout onto a new workspace and
the restored pane never filled: the session never heard the structural
notice behind it. tapestryd sends a session's TEV_LAYOUT to ONE surface --
the lowest slot the seat owns, after churn as often a chrome as a tile --
and the notice was only as good as its surface: halcyond acted on it only in
the tile loop, and a surface halcyond dropped (every chrome whose pane leaves
the active tree, at each workspace switch) took a queued or reaped notice
with it. The ring now reports a notice whichever surface carries it, also on
a dropped surface's last read ([[sub-libtapestry]]); the session reconciles
on that report and does not block while one is pending ([[sub-halcyond]]);
and tapestryd re-sends a notice still queued on a surface it retires, except
while that surface's own conn is torn down ([[sub-tapestryd]]). No wire
change. The same surface sequence on run 14 filled the pane; the tapestryd
re-send has not yet fired on the device (the client half carried every
notice), so it is verified by reading.
