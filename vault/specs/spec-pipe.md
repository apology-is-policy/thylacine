---
id: spec-pipe
type: spec
title: "pipe.tla"
models: [sub-kernel-pipe]
pins: [inv-i9]
cfgs:
  - "pipe.cfg -- clean: TypeOk + SingleWaiter + EofMonotonic + NoStuckReader + NoStuckWriter"
  - "pipe_buggy_write_no_wake_reader.cfg -- NoStuckReader violated"
  - "pipe_buggy_read_no_wake_writer.cfg -- NoStuckWriter violated"
  - "pipe_buggy_close_write_no_wake_reader.cfg -- NoStuckReader violated"
  - "pipe_buggy_close_read_no_wake_writer.cfg -- NoStuckWriter violated"
gate: "any change to the read/write/close loops' wake set or the EOF flags"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

Two threads over one bounded ring with two EOF flags. Each buggy cfg
deletes exactly one of the four wakes; each clean-side action pairs a
state-enabling mutation with its wake.

## What it pins

- **NoStuckReader / NoStuckWriter** — [[inv-i9]] specialized to the
  two-direction state machine: no thread stays WAITING while its
  direction's condition holds. The impl's four wake sites map
  one-to-one; the buggy cfgs are the executable checklist for "did
  you keep the wake".
- **SingleWaiter** — mirrors the rendez single-waiter contract; the
  model never has two sleepers per direction because the impl's
  extinction forbids it.
- **EofMonotonic** — once set, never cleared; the close path's flags
  are latches.

## Composition

The atomic cond-check-vs-sleep under one Rendez is
[[spec-scheduler]]'s NoMissedWakeup; this module proves the layer
above it — every mutation that could enable a waiter issues the
wake. Together they close the missed-wakeup hazard end-to-end for
the pipe.

## What it cannot see

Refcounts and frees: the F234 torn-RMW double-free
([[fnd-r15b-f234]]) is below the abstraction, as is the poll list
(modelled separately in [[spec-poll]]).

## Binding

`specs/SPEC-TO-CODE.md::pipe.tla`: ReadDrain/WriteAppend ↔ the
acting arms + their wakes; CloseRead/CloseWrite ↔ `devpipe_close`'s
two branches; the sleep arms ↔ `sleep(rendez, cond, ring)`.
