---
id: chg-2026-09-24-b1c-round4-close
type: chg
title: "B-1c holotype round 4 close: tail's signed counts and --, a bounded end, the #54 docs' reach (0 P0 / 0 P1 / 0 P2 / 9 P3)"
date: 2026-09-24
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-coreutils-filters
  - sub-coreutils-presenters
  - sub-warden
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Closes [[adt-b1c-r4]], extending [[chg-2026-09-24-b1c-round3-close]]. `tail`
takes POSIX's signed count (`-n -N` is `-n N`), and `--` ends the options in
`tail` and `head`. The smoke's capture pauses a millisecond on a write that finds
too little room, rather than spinning on a poll that calls one free byte
writable; it ends a child with a bounded kill and reap, reporting one that
outlives its kill rather than waiting on it forever; it counts `yes`'s second
from its first output; and it prints why a spawn failed. The docs name the
eight presenters whose #54 behaviour the dossier states and the network tools'
exceptions (`nc` and `con` report a gone reader as a failure; six others
swallow every failed write), add `seq` and `yes` to the tools that exit zero on
a failed write, and bound a held line, not every line, by `LINE_MAX`; four
places that still described the replaced capture and one that described the
fixed heap are corrected. Closing the round found three more. `head`'s attached
and legacy counts read an overflow as 10 (`head -99999999999999999999` printed
ten lines and exited zero); it is refused now. The fixed heap was still named
in the Halcyon traps list, which told a binary to declare `ThylaAllocN`, in five
AUDIT-TRIGGERS rows and one index line, and in AUX-ROADMAP's reclaim item; and
B-1c's own trigger row said every native program's memory rides thyla-heap,
where corvus's does not. And warden's three comments and its dossier said a
child's pipes close at its reap, false since #68, whose kernel test also
resolves the holotype register's HT02.SA-3, still marked tracked. Round 3's
finding note and change note
are corrected on the regression and on which checks its sabotage failed.
Verified: host coreutils 42/42 with no new clippy warning; the device at -smp 4
and -smp 1 (1667/1667; heap-probe ALL OK; coreutil-smoke, 105 checks, all OK);
and `tail` and `head` back at their round-3 code fail exactly the four new
checks, each by name.
