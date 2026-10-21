---
id: dec-2026-10-06-seam90-unwind-any-byte
type: dec
title: "A blocking 9P reader unwinds at any byte; the client keeps the partial frame"
date: 2026-10-06
status: standing
decided-by: user-vote
affects: [sub-kernel-ninep-client, sub-kernel-rendez, sub-kernel-notes, haz-shared-stream-desync, inv-i9, spec-reader-frame]
created: 2026-10-06
---
## Fork

ARCH 8.8.1.1 (task #90, voted 2026-07-19) made the elected 9P reader's recv
frame-atomic: a death, a stop (8c-3) or a caught note (11b-9p) unwound a
reader only at a frame boundary, and a reader that had read part of a frame
blocked through until the frame was whole (`thread_reader_blocks_death`). The
reason was the stream: the bytes read lived in the reader's frame-local count,
so an unwind mid-frame lost them and the next reader read the frame's tail as a
header (the task-#50 class). Block-through is bounded only by the server. A
server that stops inside a frame holds its dying, stopped or signalled reader
until the server dies, and `SYS_ATTACH_9P` takes pipes from any process, so any
process can do it ([[seam-90-hung-server]]).

Since `loom-mc` (main f6f4c0397) the partial frame is the client's:
`do_reader_recv_frame` resumes at `c->rx_got` and leaves what it read there on
every exit without a whole frame. The stream no longer needs block-through. The
fork: keep the voted policy, or let every async event unwind the reader at any
byte.

## Research

- **Plan 9 (`port/devmnt.c`).** `mountio` reads replies under `waserror`; any
  error, a note's `Eintr` included, ends the reader's `mntrpcread`/`doread` at
  whatever byte it reached. `mntgate` clears `m->rip` and wakes a waiting RPC
  to take the reader role, and the bytes read stay in the mount's queue `m->q`
  for the next reader. The interrupted RPC is flushed (`mntflushalloc`).
- **Linux (`net/9p/trans_fd.c`).** The connection reads on a workqueue
  (`p9_read_work`); a partial frame is `m->rc.offset` in the connection. A
  signalled caller never reads, so nothing waits on its behalf.
- **The tree.** Every transport recv sleeps only before it copies and returns
  either the bytes it copied or an error having copied none
  (`srvconn_client_recv`; the pipe read under `9p_spoor_transport.c`, the only
  rx EL0 can attach). Each async event already has a boundary path that
  releases the role and hands it on: a death takes the #845 abandon (a Tflush,
  the tag reserved until the Rflush, I-10), a stop parks role-free and
  re-elects, a caught note takes flush(5). The waiters' pump leaves
  `rx_got > 0` whenever a frame's tail has not arrived, so a resumed partial
  frame is an exercised state.

## Options

1. **Unwind at any byte.** The blocking reader holds `stop_unwinds` for its
   whole recv; `thread_reader_blocks_death` is deleted; the client keeps the
   partial frame. Plan 9's shape.
2. **Keep block-through.** The voted policy; `seam-90-hung-server` stays open
   as its cost.

## The call

Option 1, in two votes (operator, 2026-10-06). At 21:03Z: "Close seam-90" -- a
death or a stop unwinds a blocking reader at any byte. At 22:15Z, asked because
the vote named death and stop while the same guard held caught notes too: a
caught note unwinds the reader at any byte as well ("Yes, all three").

## Rationale

The rule existed for the stream, and the stream no longer needs it; what was
left was its cost, a reader any process could hold. Plan 9 has always unwound
an interrupted reader at any byte and kept the partial message with the mount.
Keeping block-through for caught notes alone would have kept the hang for any
program that catches the signal, with no reason left to pay it. The model
(`specs/reader_frame.tla`, rewritten) proves the new obligation: every reader
exit leaves the partial frame for the next reader (`NoDesync`, `ResumePoint`,
`FrameDelivered`), and an interrupted reader leaves its recv with no fairness on
the server (`EventuallyUnwinds`); `reader_frame_blockthrough.cfg` shows the old
rule failing that liveness under a server that stops.
