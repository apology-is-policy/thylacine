---
id: seam-90-death-half
type: seam
title: "The death half of the mid-frame unwind (the pre-existing #811 latent)"
status: closed
surface: [sub-kernel-ninep-client]
opened-by: fnd-8c3-r1-f1
closed-by: chg-2026-07-19-90-death-block-through
tracker: "task #90"
created: 2026-07-31
updated: 2026-07-31
---
## Owed (was)

8c-3's holotype established that a Proc DYING as the elected reader
mid-frame desyncs the shared stream identically to the stop case — a
pre-existing #841/#811 latent no prior audit had analyzed. Fixing it
required narrowing #811's universal death-interruptible sleep for exactly
the reader recv (death deferred to a frame boundary) — a §28 I-9 refinement,
so it was escalated for user signoff rather than fixed in the 8c-3 chunk;
the stop half landed there, this half was enqueued as task #90.

## What closed it

[[chg-2026-07-19-90-death-block-through]]: the user-voted block-through
design (mirror 8c-3), spec-first via [[spec-reader-frame]], reusing the
existing `stop_no_park`/`stop_unwinds` latches — the die-check guard widened
to `thread_reader_blocks_death` on all four sleep-site checks. The residual
liveness face against a hung server is the separate open
[[seam-90-hung-server]].

## Risk while open (was)

A Proc killed while holding the reader role mid-frame corrupted the shared
SYSTEM Stratum session — rarer than the stop trigger (death is terminal,
stops are repeatable) but the same whole-FS blast radius.
