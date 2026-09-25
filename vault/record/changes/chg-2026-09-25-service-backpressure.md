---
id: chg-2026-09-25-service-backpressure
type: chg
title: "Retain Halcyon service replies and own non-duplicable readiness handles"
date: 2026-09-25
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-halcyond, sub-halcyond-service-wire, sub-libthyla-rs, sub-substrate-interactive]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Both media adapters explicitly enable nonblocking I/O and retain partial replies
through a shared bounded stream pump. Complete buffered frames keep the UI
runnable. The readiness worker accepts ownership of /srv descriptors, whose
kernel contract forbids duplication, and lends them to UI I/O without holding
the state mutex. It is not yet connected to Halcyon's loop.

The real-SrvConn native probe verifies stalled-reader fairness, exact reply
bytes, short-write readiness and owned-handle retirement; the actual per-user
media adapter handles two routed image uploads. Source, fixture differences,
failed-run dispositions and remaining activation gates are in the interaction
status. No live clipboard, graphical redraw evidence or Main integration is
claimed by this checkpoint.

All 341 Halcyon host tests pass. Lost-offset and lost-buffered-wake mutants fail
their named tests; the restored pump tests pass. Final native Halcyon/probe links
and the existing readiness gate pass. The kernel and original paired production
artifacts are unchanged. Evidence is retained in work/hi1-service-evidence/.
