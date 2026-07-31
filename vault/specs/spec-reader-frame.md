---
id: spec-reader-frame
type: spec
title: "reader_frame.tla"
models: [sub-kernel-ninep-client]
pins: [inv-i9]
cfgs:
  - "reader_frame.cfg -- clean (INVARIANT Safety = NoDesync + UnwindAtBoundary; PROPERTY EventuallyUnwinds -- the dying reader always unwinds, at a boundary)"
  - "reader_frame_buggy.cfg -- buggy: counterexample of NoDesync (the pre-#90 mid-frame death-unwind; violated at got=2)"
gate: "Re-run for any change to reader_recv_frame / do_reader_recv_frame, the sched.c die-check guards, or thread_reader_blocks_death."
created: 2026-07-31
updated: 2026-07-31
---
## Abstraction

A single elected reader consuming one frame in CHUNKS (`got` increments —
the chunked-delivery ground truth that refuted the "whole-frame delivery"
assumption, [[fnd-8c3-r1-f1]]), with death arriving at any point.
`AtBoundary(got ∈ {0, N})` maps got==0 → the `stop_unwinds` boundary latch
and got==N → the client-layer frame-complete unwind. `ReceiveChunk` carries
weak fairness — the TRUSTED server's whole-frame delivery (CF-3 B rings) is
the liveness assumption; an untrusted/hung server voids it
([[seam-90-hung-server]]). Deliberately beneath the model: the debug-STOP
half of the same mechanism (8c-3 — `debug_stop.tla`'s domain, itself below
that model too); the survivor's role takeover; multi-reader election.

## Action-site map

| Spec action | Impl |
|---|---|
| `ReceiveChunk` | the `t->ops.recv` loop in `do_reader_recv_frame` (header loop sets `stop_unwinds = (got == 0)` per-chunk; body loop clears it — mid-frame never unwinds) |
| `Unwind` (single action, guard `AtBoundary`) | the four guarded `thread_die_pending` sites in `kernel/sched.c` — `sleep()` register-then-observe + resume-path, `tsleep()` register-then-observe + resume-path — each `&& !thread_reader_blocks_death(t)` |
| block-through | `thread_reader_blocks_death(t) == stop_no_park && !stop_unwinds`: the die-check falls through to register+sched; the reader finishes the frame |

Regressions: `rendez.reader_frame_predicate` ·
`rendez.reader_frame_blocks_death` (tsleep path) ·
`rendez.reader_frame_blocks_death_sleep` (the production `sleep()` path —
[[fnd-90-r1-f1]]; revert-probed per-guard).
