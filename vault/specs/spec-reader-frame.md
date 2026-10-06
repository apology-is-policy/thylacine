---
id: spec-reader-frame
type: spec
title: "reader_frame.tla"
models: [sub-kernel-ninep-client]
pins: [inv-i9]
cfgs:
  - "reader_frame.cfg -- clean, Spec (no server fairness): INVARIANT Safety (TypeOk + NoDesync + ResumePoint); PROPERTY EventuallyUnwinds -- an interrupted reader leaves its recv even if the server never sends again"
  - "reader_frame_delivery.cfg -- clean, FairServerSpec: Safety + FrameDelivered + EventuallyUnwinds -- the frame reaches its reader although the one that began it left mid-frame"
  - "reader_frame_blockthrough_fair.cfg -- clean CONTROL: the superseded block-through under a server that always finishes a frame (the 2026-07-19 model's claim, reproduced)"
  - "reader_frame_buggy.cfg -- buggy: NoDesync violated (an unwind that discards the partial frame)"
  - "reader_frame_blockthrough.cfg -- buggy, temporal: EventuallyUnwinds violated, Safety intact (the superseded block-through under a server that stops: the seam-90 hang)"
gate: "Re-run specs/check-reader-frame.sh for any change to reader_recv_frame / do_reader_recv_frame (rx_got), a transport recv's copy-then-fail behaviour, or the sched.c die-checks and stop detour."
created: 2026-07-31
updated: 2026-10-06
---
## Abstraction

One frame of N chunks. The server sends chunks (`sent`) and, in `Spec`, may
stop at any point for good -- no fairness on `Send`, the case a hostile server
forces (any process can serve a mount over pipes). `pos` is how far the wire
has been read, `rx` the client's resume count (`c->rx_got`). Reader A holds the
role and is the one an async event reaches (`interrupted`: death, stop and
caught note leave the recv the same way); reader B waits on the same session
and takes the free role. A frame is delivered when its last chunk is read with
`rx` in step with `pos`. Rewritten 2026-10-06 for
[[dec-2026-10-06-seam90-unwind-any-byte]]; until then it modeled the
frame-atomic block-through (one reader, a die-check guarded to boundaries,
server delivery fair).

Deliberately outside: the transport recv itself (taken to return the bytes it
copied or none -- each sleeps only before it copies); tags and the dying op's
flush ([[spec-9p-client]], I-10); more than one frame; what A does after it
leaves (dies, parks and re-elects, or flushes); and the srvconn reading role
(`ch->reading`), taken to be released on every recv exit
(`chan_role_release`) -- one left held would strand the next reader in
`chan_role_acquire`, which `ElectB` cannot show.

## Action-site map

| Spec action | Impl |
|---|---|
| `Read(X)` | `do_reader_recv_frame`'s two recv loops, resuming at `c->rx_got`; `rx + 1 = N` is the frame complete (`rx_got = 0`, demux) |
| `UnwindA` | any async unwind of a blocking reader: the die-checks in `sleep()`/`tsleep()` (register-then-observe and prompt), the stop detour's `stop_unwinds` branch, the caught arms; `rx' = rx` is the `incomplete:` exit leaving `c->rx_got` |
| `ElectB` | the role handoff (`client_handoff_reader_locked`) and any later election: `client_wait`, the send-path self-pump, `p9_client_reader_pump_ready` |
| `BUGGY_DISCARD` | a reader whose count is frame-local (the pre-`loom-mc` reader) |
| `BUGGY_BLOCK_THROUGH` | the deleted `thread_reader_blocks_death` guard |

Checker: `specs/check-reader-frame.sh` (pins each cfg's verdict by name and
every count). TLC 2026-10-06, N = 3: clean 39 / 39 (delivery) / 34
(blockthrough_fair); `reader_frame_buggy` NoDesync at 41;
`reader_frame_blockthrough` EventuallyUnwinds at 34 (a stutter with A in its
recv after the server stops mid-frame). Both counterexamples read.
Regressions: `rendez.reader_recv_unwinds_death` (tsleep) ·
`rendez.reader_recv_unwinds_death_sleep` (sleep, prompt path) ·
`rendez.reader_recv_unwinds_caught_note` · and the client end to end,
`9p_srvconn_transport.reader_unwinds_mid_frame_death` /
`.reader_unwinds_mid_frame_stop` (a real SrvConn; the server stops 20 bytes
into a 160-byte reply).
