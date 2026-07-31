---
id: haz-shared-stream-desync
type: haz
title: "Mid-frame unwind of a shared byte-stream reader"
applies-to: [sub-kernel-ninep-client]
instances: [fnd-8c3-r1-f1]
created: 2026-07-31
updated: 2026-07-31
---
## The failure shape

The elected reader of a SHARED framed byte stream unwinds (death, stop, or
any new interrupt path) after consuming part of a frame: the consumed bytes
are discarded, the survivor that takes the reader role reads the frame TAIL
as a header, and the stream desyncs — shared-session death (whole-FS DoS for
every Proc on the mount) or silent misframing (the task-#50
wrong-reply/poisoned-dentry corruption class).

## The tell

- Any NEW unwind/interrupt/park path out of a recv loop on the shared
  client.
- A "delivery is whole-frame, so the reader only ever sleeps at a boundary"
  claim. Delivery is CHUNKED: the srvconn rings short-read/short-write under
  pipelining depth ≥ 2 + ring pressure, so a mid-frame sleep is reachable —
  this exact claim was refuted by ground truth once already.

## The countermeasure

Frame-atomicity as a DESIGNED property: unwind only at `got == 0`
(`stop_unwinds`), BLOCK THROUGH mid-frame (`stop_no_park` +
`thread_reader_blocks_death`), bounded by the trusted server's whole-frame
delivery. Modeled by [[spec-reader-frame]]; the stop half shares the
mechanism. Any interruption a future change adds to the reader recv must
route through the same boundary latch, not a new flag.
