---
id: fnd-8c3-r1-f1
type: fnd
title: "Mid-frame stop-unwind desyncs the shared 9P byte stream"
round: adt-8c3-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
hazard: haz-shared-stream-desync
fixed-by: chg-2026-07-17-8c3-reader-role
regression: "9p_client.handoff_skips_debug_stopped_owner (+ spec-reader-frame for the frame mechanics)"
created: 2026-07-31
---
## Prosecution

The draft's stop-unwind assumed the reader is interruptible only at frame
boundaries because the server delivers whole frames. FALSE: delivery is
CHUNKED (ring short-reads/short-writes under pipelining depth >= 2 + ring
pressure), so the reader consumes a partial frame and its next recv sleeps
MID-FRAME. A stop-unwind there discards the consumed bytes; the survivor
reads the frame TAIL as a header -> desync -> shared-session death
(whole-FS DoS) or silent misframing (the task-#50 corruption class). The
death twin is a pre-existing #841/#811 latent no prior audit had analyzed;
the stop WIDENS the trigger from death (terminal, rare) to a repeatable
debug stop.

## Disposition

Fixed (the stop half): the frame-atomic recv -- stop_no_park held for the
recv tenure, stop_unwinds = (got == 0) per-chunk, block-through mid-frame.
The death half was escalated (a section-28 I-9 refinement needs signoff)
and enqueued -> [[seam-90-death-half]], closed by #90. The refuted
"whole-frame delivery" claim is the hazard note's canonical tell.
