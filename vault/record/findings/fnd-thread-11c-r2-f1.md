---
id: fnd-thread-11c-r2-f1
type: fnd
round: adt-thread-11c-r2
severity: P2
status: fixed
title: "The write-behind cancellation test read the claim, and a thread-directed note unwinds with none"
surface: [sub-kernel-ninep-dev9p]
threatens: [inv-i19]
fixed-by: chg-2026-11-03-item-11c
regression: "dev9p.wb_interrupted_flush_keeps_run, wb_server_eintr_latches and 9p_client.note_flush_direct_note_marks; K56 red on them"
created: 2026-11-03
---
## Prosecution

A flush cancelled by a thread-directed note carries no claim, so `wb_note_cancelled` latched it as a server failure and dropped bytes `write` had reported written. Latent: no call that unwinds reaches a staged Twrite today. It now reads `note_call_unwound`, one rule for both kinds of note. So a server's EINTR under a claim latches, as the server failing the write. I had argued the opposite in f99b1ad4d, and this overturned it.
