---
id: chg-2026-09-07-prowl-doc-absorb
type: chg
title: "absorb docs/reference/144-prowl (the scheduler-aware process monitor): clean redirect to sub-prowl (ahead of the doc)"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-07
---
The native Kaua htop-equivalent (usr/prowl/src/{main,sample,ui}.rs). Kernel
byte-unchanged; pure userspace over prowl-1/3a/3b telemetry. quaestor owner: all
three sources -> sub-prowl (a dedicated dossier). Verified atom-by-atom.

ALREADY COVERED (verified, and sub-prowl is AHEAD of the doc):
- The three-layer split (pure sampler / back-buffer UI / console-owning main),
  the integer tenths htop math (cpu_ns diff over wall; counter-reuse saturating_
  sub), the idle-inversion meter + clamp, cursor-tracks-PID stepping DISPLAY order
  (the tree-mode fix), prowl-4's cycle+orphan-safe tree walk, confirm-gated kill
  vs unconfirmed reversible suspend/resume, the conflated denied-read/vanished
  "unavailable" pane -> sub-prowl Mechanism.
- No-new-authority: I-26 two-axis gate at the ctl write, OQ-4 gate on sched, I-27
  console posture (raw-mode dance + panic-abort restore) -> sub-prowl Invariants/
  Prosecution.
- The truncation: the doc stops at "#62 pagination seam"; sub-prowl carries the
  SHARPER finding -- the kernel computes truncation into an `overflow` field, sets
  it at 15 points, discards it (15 writes / 0 reads), so truncated == complete
  byte-for-byte on the client (#158). AHEAD of the doc.
- CPR-round-trip sizing (no winsize syscall) is a SHARED Kaua mechanism the doc
  frames as "mirrors nora" -> sub-kaua, not a prowl fold.
- The /ctl/procs,/ctl/cpu,/proc/<pid>/sched read surfaces -> devctl/devproc.

Zero-fold. Redirect stub. Zero code change.
