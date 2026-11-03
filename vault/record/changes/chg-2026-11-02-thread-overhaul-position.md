---
id: chg-2026-11-02-thread-overhaul-position
type: chg
title: "The thread-model overhaul's position: C7 was not its last chunk"
date: 2026-11-02
arc: arc-phase6-pouch
commits: []
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-02
---
Corrects [[chg-2026-10-29-thread-model-c7]], which calls C7 "the last chunk of the thread-model overhaul". Against
the chunk list in `docs/thread-model-review.html`, landed on aux-3: C0, C2, C7a, C1, C3, C5, C6, the poll chunk
([[chg-2026-10-28-poll-hook-walks]]) and C7. Not landed: C4, thread-directed notes (operator vote 5 YES; review
finding F-3, P2: `tkill` and `tgkill` still post to the whole process through pouch 0007, and `pthread_kill` reaches
any thread), and the three performance chunks C8 (torpor and the timed-wait list), C9 (switch fast paths) and C10
(the kernel-stack address range). The corona join ([[chg-2026-10-29-aux-corona-join]]) was not on that list; it was
owed to the Go fork's env-b.
