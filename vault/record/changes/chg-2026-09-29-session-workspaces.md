---
id: chg-2026-09-29-session-workspaces
type: chg
title: "A session's workspaces are the session's: the owner stamp, fresh panes that wait to be asked, the kept last pane, the departure and the takeover"
date: 2026-09-29
arc: arc-tapestry
commits: ["4fd4cb65"]
touched:
  - sub-tapestryd
  - sub-halcyond
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-29
---
The manual chunk's device run pressed Open shell on a new workspace and
found no placard to press: `halcyond: chrome for pane 12 failed Create`. The
tree minted every workspace root with owner 0, the environment's, although
HALCYON-WORKSPACES makes a session's workspaces the SESSION's, and an empty
leaf's recorded owner is what the chrome bind and the placement claim ask.
Every root the tree mints for a workspace now records the declared session's
principal, else 0, read at each mint ([[sub-tapestryd]]). The stamp made the
root claimable, so halcyond filled a new workspace at once and an empty one
never vanished; a first fix keyed the wait on the tree's SHAPE and swallowed
a one-leaf restore onto a new workspace (Fable round 4, F2). The rule now
keys on how a pane came to be: tapestryd marks ` fresh` an empty leaf it made
so a workspace has a pane, and halcyond fills one only when asked -- Open
shell, or Super+N on it ([[sub-halcyond]]). Round 4 also found three ways a
session's workspaces ended wrong: closing workspace 1's last tile while
workspace 2 held tiles sent the keys to the hidden console renderer (F3); a
logout from workspace 2 left the login prompt on a dormant workspace (F1); a
takeover kept the old principal's stamps (F5). A workspace's last usable pane
now stays, emptied in place under a new id, while the session goes on; the
departure closes the fresh panes and shows the console renderer's workspace;
a takeover re-stamps the old seat's empty panes. Round 5 was clean (two P3s,
fixed). Host tests red under their sabotages and `ls-halcyon-manual` legs 9a,
16 and 17a-17c pin it.
