---
id: chg-2026-10-02-ordered-interaction
type: chg
title: "Ordered ownership notifications and admission decisions"
date: 2026-10-02
arc: arc-halcyon-interaction
commits: []
touched: [sub-libhalcyon, sub-libtapestry, sub-tapestryd, sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
One bounded compositor journal orders ownership changes with admission decisions.
The dedicated session owner consumes it independently of HSC cancellation. Exact
wire validation, overflow refusal, separate read/write storage and early-reply
holding preserve that order. Native testing repaired the selector's role mismatch
and a false nonzero-seat assumption; generation zero is the initial normal seat.

Final focused evidence: 691 tests across three host crates, 24 actual-source
producer cases, 57 owner schedules, 16 wire/channel cases and the native identity
predicate; six/nine/eight intended mutations. Full image and CPU1 1830/1830 pass;
native ordered admission passes in 36.71s. Graphical results and screenshots are
recorded in HALCYON-INTERACTION-STATUS. Single-agent self-review and October 2
waivers apply. Application dispatch, reply cancellation, resource accounting,
clipboard clients and modal painting remain activation work; no Main landing.
