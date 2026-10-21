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

The partial frame belongs to the CLIENT, not the reader (`c->rx_got`, as Plan 9
devmnt's `m->q` and Linux trans_fd's `rc.offset`; since
[[chg-2026-10-06-loom-multiclient]]): every exit of the frame reader leaves the
bytes it read for the next reader, which resumes there. So the reader may
unwind at any byte for a death, a stop or a caught note, and since
[[dec-2026-10-06-seam90-unwind-any-byte]] it does. [[spec-reader-frame]] models
it (`NoDesync`, `ResumePoint`; `reader_frame_buggy.cfg` is the discard). A NEW
shared-stream reader that keeps its partial frame on its own stack reopens the
hazard, and so does a transport recv that copies bytes and then returns an
error.

From 2026-07-19 to 2026-10-06 the countermeasure was frame-atomicity instead:
unwind only at `got == 0`, block through mid-frame (`stop_no_park` +
`thread_reader_blocks_death`), bounded only by the server's delivery. Its cost
was [[seam-90-hung-server]]: a server that stopped inside a frame held the
reader until it died.
