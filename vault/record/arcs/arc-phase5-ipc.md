---
id: arc-phase5-ipc
type: arc
title: "Phase 5 IPC: pipe, then poll, then the atomic-ref close"
status: complete
design: ["docs/ARCHITECTURE.md 10.3", "docs/ARCHITECTURE.md 23.3"]
chunks:
  - chg-2026-05-14-p5-pipe
  - chg-2026-05-14-r15b-atomic-refs
  - chg-2026-05-20-p5-poll
follow-ons: [seam-poll-heap-waiters, seam-poll-srv-registry-retain]
created: 2026-08-01
---
## Goal

The blocking-IPC slice of Phase 5: a real pipe with modelled wait/wake
([[spec-pipe]]), then the N-fd multiplexer over stack hooks
([[spec-poll]]), each with its audit round.

## Shape

- **P5-pipe** ([[chg-2026-05-14-p5-pipe]]) — the primitive, then
  blocking + the spec in the same fortnight.
- **r15-b** ([[chg-2026-05-14-r15b-atomic-refs]]) — the two
  non-atomic refcounts (Spoor F233, pipe ring F234) made ACQ_REL
  before SMP could tear them.
- **P5-poll** ([[chg-2026-05-20-p5-poll]]) — mechanism + devpipe +
  devsrv `.poll` + the close ([[adt-poll-r1]]).

## The arc's long shadow

Two of its audit dispositions became later eras' work: F3's
"doc-fixed" single-thread borrow precondition was voided by the
multi-thread lift and closed structurally at RW-2
([[fnd-poll-r1-f3]] → [[fnd-rw2-2cf1]]); the poll bound's identity
with `PROC_HANDLE_MAX` was cut when the table grew
([[chg-2026-06-24-355-poll-decouple]]).
