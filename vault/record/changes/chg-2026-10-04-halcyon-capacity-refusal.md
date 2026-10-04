---
id: chg-2026-10-04-halcyon-capacity-refusal
type: chg
title: "Reject excess Halcyon service connections promptly"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-substrate-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
HI1-R29 is fixed: the native owner keeps polling the listener when its slots
are full, accepts one endpoint per pass and closes excess endpoints before
allocating connection/protocol state. The former omission stranded a third
kernel 9P attach indefinitely behind two retained media clients. HSC processing
still precedes peer work, admitted peers retain their slots, and teardown still
returns quota only after descriptor close. No connection limit or ABI changes.

The actual native adapter fixture failed before the fix with both media peers
retained through the rejection deadline. After the fix it verifies prompt
refusal, readable admitted peers and slot reuse over three consecutive waves.
The repaired production source passes CPU1 boot 1830/1830. The final native capacity fixture passes in 35.76 s; existing native service-wire, deadline teardown and media-route regression pass on its matching image in 65.94 s. Only fixture tracing and bounded setup reclamation changed after the ordinary boot.

Evidence and single-agent review: work/oct4-hi-capacity. Four protected drafts
preserved; no Main landing or fresh SMP/sanitizer/Pi claim. The nondefault
interaction gate remains. HI1-R24 still owns the complete memory ledger,
maximum live-tile/multi-session demand and pending/partial-output SAK checks.
The 32-slot control reserve is an upper bound shared with MAX_PANES; containers
also consume that pane limit, so it is not a promise of 32 application tiles.
