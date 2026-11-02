---
id: chg-2026-11-02-f3-what-it-is
type: chg
title: "What review finding F-3 is, read from main's code"
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
Corrects [[chg-2026-11-02-thread-overhaul-position]], whose sentence on F-3 misdescribes it. C4 is open on every ref
here, main included. The phenotype's `tkill` and `tgkill` (`viv_kill`, `postnote_walk_cb`) use the tid only to find the
Proc, then post a note any unmasked thread may take; they do not go through pouch. Native `pthread_kill` is not
retargeted: musl's `SYS_tkill` is the 0xFFFF sentinel, so it gets ENOSYS, although pouch 0007's header says it is
served. `raise()` self-posts through pouch 0007.
