---
id: spec-pipe
type: spec
title: "pipe.tla"
models: [sub-kernel-pipe]
pins: [inv-i9]
cfgs:
  - "pipe.cfg -- clean, two threads: Invariants (TypeOk + EofMonotonic + NoStuckReader + NoStuckWriter) + the action property NoByteAfterEof"
  - "pipe_multi.cfg -- clean, three threads, so two sleep on one side at once: the same"
  - "pipe_buggy_write_no_wake_reader.cfg -- NoStuckReader violated"
  - "pipe_buggy_read_no_wake_writer.cfg -- NoStuckWriter violated"
  - "pipe_buggy_close_write_no_wake_reader.cfg -- NoStuckReader violated"
  - "pipe_buggy_close_read_no_wake_writer.cfg -- NoStuckWriter violated"
  - "pipe_buggy_wake_one_reader.cfg -- NoStuckReader violated: a write wakes one chosen reader, not every hook"
  - "pipe_buggy_hangup_no_wake_writer.cfg -- NoStuckWriter violated: the hangup wakes readers only, as a close does"
  - "pipe_buggy_hangup_takes_bytes.cfg -- NoByteAfterEof violated: a hung-up write end still takes bytes"
gate: "any change to the read/write/close/hangup paths' wake set, the EOF flags, or the write arms' refusal"
created: 2026-08-01
updated: 2026-10-06
---
## Abstraction

Two or three threads over one bounded ring with two EOF flags and one bit
saying the write end is still held. Each clean-side action pairs a
state-enabling mutation with its wake, and every wake wakes all sleepers on
its side, as the single `poll_waiter_list` does. Each wake buggy cfg deletes
exactly one wake or narrows it to one waiter.

The hangup (2026-10-05, ARCH 10.3 and 21.10's "A death hangs up") is EOF
without the close: `HangupWrite` sets `writeEof` while the end stays held, and
wakes every sleeper on both sides, so readers see EOF once the ring drains and
writers are refused. It wakes writers as well as readers, unlike a close of
the write end, because a writer on the hung-up end itself is blocked on a full
ring and must meet the refusal.

## What it pins

- **NoStuckReader / NoStuckWriter** — [[inv-i9]] specialized to the
  two-direction state machine: no thread stays waiting while its
  direction's condition holds. The impl's five wake sites (after a read,
  after a write, each close, the hangup) map onto the buggy cfgs, which are
  the executable checklist for "did you keep the wake".
- **EofMonotonic** — once set, never cleared; the close and hangup flags
  are latches.
- **NoByteAfterEof** — an action property: once `writeEof` is set the ring
  never grows. Both write arms must refuse a hung-up end, not only a closed
  read end.

`SingleWaiter` is retired. It mirrored the rendez single-waiter contract,
which the multi-waiter lift removed, and `pipe_multi.cfg` now checks two
sleepers on one side.

## Composition

The atomic cond-check-vs-sleep under one Rendez is
[[spec-scheduler]]'s NoMissedWakeup, and the poll list's
register-then-observe is [[spec-poll]]'s; this module proves the layer
above them -- every mutation that could enable a waiter issues the wake.
Together they close the missed-wakeup hazard end-to-end for the pipe.

## What it cannot see

Refcounts and frees: the F234 torn-RMW double-free ([[fnd-r15b-f234]]) is
below the abstraction, and so is the hangup's leaving the ring ref with its
holder -- the model tracks only that the end is still held. The `pipe` note,
and the rule that a mounted queue posts none, are the kernel tests'
(`pipe.cnbframe_refusal_posts_no_note`).

## Binding

`specs/SPEC-TO-CODE.md::pipe.tla`: ReadDrain/WriteAppend ↔ the acting arms
and their wakes; CloseRead/CloseWrite ↔ `devpipe_close`'s two branches;
HangupWrite ↔ `pipe_hangup_write` and both write arms' refusal on
`write_eof`; the sleep arms ↔ `pipe_block_locked`.
