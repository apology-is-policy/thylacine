---
id: chg-2026-11-02-b3h-audit-r4-fixes
type: chg
title: "B-3h audit round 4's fixes"
date: 2026-11-02
arc: arc-boosty
depth: skeletal
commits: ["f1007a408"]
touched:
  - sub-kernel-filelock
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-session
  - sub-kernel-ninep-client
  - sub-kernel-ninep-attach
  - sub-kernel-larder
  - sub-kernel-handle
  - sub-kernel-spoor
  - sub-kernel-dev
  - sub-kernel-syscall-dispatch
  - sub-kernel-thread
  - sub-pouch-fs
---

Round 4 read round 3's close: 1 P1, 4 P2 and 16 P3 over three parts, on Opus. Nothing here
has booted.

- **R4a-F1 (P1), spawn's endowment was no descriptor.** The parent's close was taken for the
  last close, which dropped the child's flock and ended a redirect's write-behind. It counts
  as a descriptor now, from the call until the child's slot holds the file.
- **R4C-1 (P2), a job past the last close installed OWN pages.** A flush with the anchor gone
  drops them, and the job drops what a peer installed.
- **R4a-F3 (P2)**: that job now gives back the buffer. **R4b-F1 (P2)**: a live owner whose
  reply beat its Rflush no longer forgets the Larder. **R4b-F2 (P2)**: a forget is one store,
  an epoch, instead of a scan under `c->lock` at a rate EL0 sets.
- P3s: R4a-F4 and R4C-2 (a job alone reports its own failure, and has no retry because its
  flush drops the run), R4a-F5 (attach root), R4a-F6 (a closer sleeps out a peer's flush),
  R4b-F3 (a parked fid carries the epoch), R4b-F4 (lock order stated), R4C-3 and R4C-4
  (EBADF, in the manual and from Pouch's probe), and the stale texts and missing legs of
  R4a-F7, R4b-F5, R4b-F6, R4C-5, R4C-6.

Left by decision: a forget cannot name what the op touched, so a Proc killing its own writers
in a loop keeps the session's cache cold for everyone on it. Nothing answers wrongly. Tracked
in OPEN-BUGS. R4a-F2 was already closed by SA4-1.
