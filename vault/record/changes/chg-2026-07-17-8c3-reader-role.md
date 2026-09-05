---
id: chg-2026-07-17-8c3-reader-role
type: chg
title: "8c-3 (#89): frame-atomic release of the reader role across a debug stop"
date: 2026-07-17
arc: arc-go-ide
commits: ["a58403fb"]
touched: [sub-kernel-ninep-client]
established: []
closed:
  - fnd-8c3-r1-f1
  - fnd-8c3-r1-f2
  - fnd-8c3-r2-f1
  - fnd-8c3-r2-f2
  - fnd-8c3-r3-f1
opened: [seam-90-death-half]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
A debug-stopped elected reader used to park IN PLACE holding
`reader_active`, freezing every survivor Proc sharing the SYSTEM Stratum
client for the stop's duration. The fix releases the role -- and the
holotype forced it to be FRAME-ATOMIC: delivery is chunked, so the recv
wrapper (`reader_recv_frame`) holds `stop_no_park` for its tenure, sets
`stop_unwinds = (got == 0)` per-chunk, and a stop unwinds only at a frame
boundary while blocking through mid-frame; all four `reader_active` sites
handle a stop-unwound recv without latching the session; the handoff skips
debug-stopped owners; classification uses the stable per-Thread
`stop_unwound` latch (a re-read of debug_stop_req races an async resume --
the R2 P1). The death twin of the mid-frame hazard was escalated and
enqueued as task #90 ([[seam-90-death-half]]). Converged over three Fable
rounds pre-commit ([[adt-8c3-r1]] -> [[adt-8c3-r3]]); all fixes landed in
this single commit. Prose: the commit message + DEBUG-FS-DESIGN 5c.6.
