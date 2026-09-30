---
id: chg-2026-09-30-kernel-chunk
type: chg
title: "The kernel chunk: the trusted episode's lock re-checks, and walks without recursion"
date: 2026-09-30
arc: arc-holotype-rw
commits: ["6c0c4b36"]
touched:
  - sub-kernel-cons
  - sub-kernel-proc
  - sub-kernel-caps
established: []
closed:
  - seam-devcap-plain-caps-read
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-30
---
The kernel findings of the Fable-diversity passes item (5) of the 2026-09-28
work order owed. The trusted episode ([[sub-kernel-cons]]): a consctl mode
write and a renderer feed byte were checked against the episode without
`g_cons.lock` and applied under it with no re-check, so a `+echo` in flight at
BEGIN opened the episode with ECHO on (IM F1, P1) and a feed byte in flight
became corvus's first input (F2, P2); both are now refused under the lock
BEGIN takes. A SAK repeated during an open episode no longer replaces the
saved pre-SAK owner (F5), and devcap's `/grant` gates load the writer's caps
atomically, closing [[seam-devcap-plain-caps-read]] ([[sub-kernel-caps]]). The
table walks ([[sub-kernel-proc]]): `proc_for_each_walk` and
`proc_find_by_pid_walk` recursed once per tree level and a fork chain's depth
is unbounded, so an unprivileged fork chain could carry any `/proc` lookup
into the kernel stack's guard (H3+C F2, re-rated a P1 candidate); one stepper,
`proc_walk_next`, now drives all three walks. cmdline's seal comment and
IMPERIUM's imperium set were corrected (H3+C F1, IM F4). The kernel round
(Fable 5.1 reviewing Opus 5.5) was clean at 0 P0 / 0 P1 / 0 P2 / 3 P3; the
pre-BEGIN echo residue is kept with row 155's and goes to the operator; main
reviewed the diff with no findings. Six new tests, each seen red first;
`tools/test.sh` 1788/1788. tools/ci-smp-gate.sh at the merged tip b0002334: 50
of 50 boots PASS across default-smp1, default-smp4, default-smp8, ubsan-smp4
and ubsan-smp8 (N=10 each), 0 corruption.
