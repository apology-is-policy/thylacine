---
id: chg-2026-09-24-b1c-round1-close
type: chg
title: "B-1c holotype round 1 close: a line bound, filters that stop quietly when their reader leaves, small over-aligned blocks in dlmalloc, witness legs that can fail (0 P0 / 1 P1 / 1 P2 / 10 P3, four self-found)"
date: 2026-09-24
arc: arc-boosty
commits: ["96b51346"]
touched:
  - sub-thyla-heap
  - sub-libthyla-rs
  - sub-coreutils-lib
  - sub-coreutils-filters
  - sub-coreutils-presenters
  - sub-halcyond
established: []
closed: [fnd-b1c-r1-f1, fnd-b1c-r1-f2, fnd-b1c-r1-f4, fnd-b1c-r1-sa4]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Closes [[adt-b1c-r1]], extending [[chg-2026-09-24-b1c-native-heap]] and
[[chg-2026-09-24-b1c-filters-stream]]. The round's P1 ([[fnd-b1c-r1-f1]]) was
the streaming fix's own gap: a line with no end grew until the pool ran out, and
the fault kill read as grep's "no match". `coreutils::stream` now bounds a line
at `LINE_MAX` (64 MiB) and refuses past it, and `grep -r` skips the character
devices it walks into. [[fnd-b1c-r1-f2]] and [[fnd-b1c-r1-sa4]] are the two
halves of #54, HT09.RND2-F2's follow-up, unblocked since #100: a filter whose
reader leaves stops reading, prints nothing and keeps its status
(`OutSink::reader_gone` / `finish`, nineteen filters). [[fnd-b1c-r1-f4]] was a
witness leg that could not fail, and it now builds its own premise. thyla-heap
gives a small over-aligned block to dlmalloc (F9) and refuses a direct block
past `isize::MAX` before counting it (F12); `/heap-probe` gained a live sentinel
across each trim and an automatic reservation release (F5, F6). Closing the round
turned up two defects in its own tests, both fixed before landing: the release
rule's test called the helper and stayed green with the call deleted, so the
loops take their buffers from the caller and uniq's grouping moved into the
library as `stream::runs`; and the first device check lent its producer
descriptors coreutil-smoke does not have. Verified: host thyla-heap 12/12 and
coreutils 22/22, with every host sabotage of the chunk re-run on the final code
RED by name (twenty-six of `stream`, thirteen of the heap); the device at -smp 4 and -smp 1 (1667/1667; heap-probe and coreutil-smoke,
77 checks, all OK), with three device sabotages RED by name. Owed to the
operator: victim selection, or per-consumer caps, for the unbounded consumers
(F3), and R4-F2's pressure-driven half (the exit-status ABI).
