---
id: chg-2026-09-24-b1c-filters-stream
type: chg
title: "B-1c WIP 3: a direct block counted for as long as it is held; the six filters that do not need their whole input stream it (HT09.R4-F2 reopened by the uncapped slurp, closed)"
date: 2026-09-24
arc: arc-boosty
commits: ["96b51346"]
touched:
  - sub-thyla-heap
  - sub-libthyla-rs
  - sub-coreutils-lib
  - sub-coreutils-filters
  - sub-coreutils-presenters
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Two defects self-found in B-1c before its holotype round, extending
[[chg-2026-09-24-b1c-native-heap]]. First, thyla-heap counted a direct block
after its reservation came back, outside the heap's lock, so a reader on
another thread could see a footprint below what the heap held. It is now
counted before the reserve (uncounted if refused) and uncounted only after the
detach, pinned by a host backend that reads the footprint at each reserve and
detach, as a peer thread would. Second, `io::slurp`'s 2 MiB cap, which B-1c
removed with the fixed heap it was sized to, had also been closing HT09.R4-F2.
Reservations are lazy, so an input larger than free memory ends the program at
a fault, v1.0 exit status 1 -- `cmp`'s "differ", `grep`'s "no match". The
streaming that finding had deferred is pulled forward for all six filters that
do not need their whole input, through one pure module, `coreutils::stream`:
`lines` for grep, cut and uniq; `compare` for cmp; `Tail`, a window of the
answer, for tail; and wc counting a buffer at a time. It is host-tested against
reads cut at every boundary and against the old whole-input answers (thirteen
sabotages red), and coreutil-smoke checks it on the device (a device sabotage
failed six checks by name). Only `sort` still holds its input; its status 1 is
an error status. The same review corrected a claim that a program survives
exhaustion with the fallible allocation forms: those see only the address space
running out.
