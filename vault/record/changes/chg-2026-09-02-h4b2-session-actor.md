---
id: chg-2026-09-02-h4b2-session-actor
type: chg
title: "H-4b-2: the Session(principal) actor -- session-wide mutual pane authority in tapestryd"
date: 2026-09-02
arc: arc-tapestry
commits: ["636937b0", "f6e306ae"]
touched: [sub-tapestryd]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-02
---
H-4b-2 adds a third tapestryd actor, `Session(principal)`, keyed on the
kernel's durable per-Proc PRINCIPAL rather than the per-process `stripes`, so
two processes of one user gain rio-style mutual pane authority (the
precondition for layout RESTORE: the tool and the programs it spawns are
different processes of the same user). `actor()` is 3-way -- Renderer /
SYSTEM|NONE|INVALID -> Client(stripes) / else -> Session(principal); system
daemons, the unauthenticated, and unknown peers stay per-process (the boot
chain never becomes one session), and the console + other users stay
protected. Surfaces and empty leaves carry `owner_principal` (empty leaves
stamped at split from the splitting actor); the three authority checks gained
Session(p) arms; `actor_names` lets a session NAME an empty leaf it owns (the
tool tags at claim time); the H-4b-1 claim mint narrowed from "any peer" to
owner-only (this CLOSES the [[sub-tapestryd]] Seams bullet on the any-peer
mint); `reap_session_empties` closes a departed session's empty scaffolding on
its principal's last conn death. REGRESSION-SAFE: the panes E2E is 33/33 under
`Session(michael)` (the claim leg still mints, the michael->SYSTEM negative
still refuses). The POSITIVE cross-process mutual-authority witness is owed at
H-4b-3 (the restore tool); the batched holotype over H-4b-1..3 + the
AUDIT-TRIGGERS row land at the H-4b arc close. Preceded by four pure-`cargo
fmt` reformat commits over the tapestry crate family (fb626450 / 0949963d /
452ff1d2 / d9334feb; operator-approved; proven pure). [[sub-tapestryd]] gains
"The Session actor" note.
