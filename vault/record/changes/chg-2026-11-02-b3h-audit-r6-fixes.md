---
id: chg-2026-11-02-b3h-audit-r6-fixes
type: chg
title: "B-3h audit round 6's fixes"
date: 2026-11-02
arc: arc-boosty
depth: skeletal
commits: ["1ba1a305a"]
touched:
  - sub-kernel-filelock
  - sub-kernel-handle
  - sub-kernel-dev
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-attach
---

Round 6 read round 5's close: 4 P3, on Opus, with no P0, P1 or P2. Nothing here has booted.

- **R6-1, a loss a last close read and told nobody.** A dying close that found a retired flush's
  latch returned it to a thread that never reaches EL0, and a live exit, a replace or an endowment
  no table took dropped theirs at the caller. A caller says whether it hands the answer to a
  program (`Dev.fd_close`'s `heard`), and a loss nobody will hear is the close's own to report.
- **R6-2, two false sentences in manual 22.** A close does not end a wait; and a lock request that
  divides a range counts the pieces against its own user, as an unlock does not.
- **R6-3, test_locks' limit.** Stratum's half: a longer ctest limit for `locks_table_bound`.
- **R6-4, legs that passed on broken code.** A refused take keeping the fid's name, a take of
  unknown outcome taking it, and a job with no run keeping its pages each have a leg now.
- **Found writing those legs.** Stratum's half: 8 sites sent the host's EPROTO rather than the wire's.
