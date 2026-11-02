---
id: chg-2026-11-02-b3h-audit-r5-fixes
type: chg
title: "B-3h audit round 5's fixes"
date: 2026-11-02
arc: arc-boosty
depth: skeletal
commits: ["11fbd521b"]
touched:
  - sub-kernel-filelock
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-session
  - sub-kernel-ninep-client
  - sub-kernel-ninep-attach
  - sub-kernel-larder
  - sub-kernel-spoor
  - sub-kernel-content
---

Round 5 read round 4's close: 4 P2 and 21 P3 over three parts, on Opus, with no P0 or P1. The
spawn endowment's counting rule survived whole. Nothing here has booted.

- **R5a-F2 (P2), the job past the last close decided by what it found.** A flush that retired
  before the closer looked kept its OWN pages past the last close, which on a lock's path is
  every time. It goes by what stood at the close now.
- **R5a-F1 (P2), a leg that could not pass.** `p9_closer.dying_last_close_with_holder` read a
  page the grant had dropped, and its "installs none" asserts held on the bug. It installs the
  page it needs, so a flush that installs extends it.
- **R5C-1 (P2), a take named the Proc's principal to a caped server**, the number the attach
  withheld. A caped session names nobody.
- **R5C-2 (P2), a take, its unlock and a test named different client_ids**, so a server keying
  owners by `proc_id` and `client_id` saw two owners. Every request on a fid names what its
  latest take named.
- **P3s.** A key holds one Larder slot across forgets, since the epoch had moved the scan to the
  next fill; the dentry CLOCK reads live; a forgotten page counts as invalidated no more; a lost
  run on a lock's two jobs is reported; texts false since rounds 3 and 4. Stratum's half is in
  its own branch.
