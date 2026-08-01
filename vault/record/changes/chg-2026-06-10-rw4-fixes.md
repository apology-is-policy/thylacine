---
id: chg-2026-06-10-rw4-fixes
type: chg
title: "HOLOTYPE RW-4 fixes: the ns_lock + the reviewer findings"
date: 2026-06-10
arc: arc-holotype-rw
commits: ["ee30f559", "6cf5933c"]
touched:
  - sub-kernel-stalk
  - sub-kernel-srvconn
  - sub-kernel-ninep-session
  - sub-kernel-ninep-client
  - sub-kernel-devsrv
established: []
closed: [seam-848-pivot-walk-race]
opened: []
mirrors-checked: []
depth: skeletal
---
The RW-4 (namespace/FS + 9P + Loom-DELTA) holotype round's fix pair.
`6cf5933c` = SA-F1 [P1]: the per-Territory `ns_lock` serializing
`mounts[]`/`binds[]`/`root_spoor` against the P6 multi-thread lift —
`mount_lookup`'s contract goes borrow→OWNED (ref under the lock, never
held across `clone_walk_zero`), and the new `territory_root_ref` gives
the six FROM_ROOT readers an atomic read+ref. This CLOSES the #848
pivot-vs-walk race ([[seam-848-pivot-walk-race]]) that #844's F3 had
tracked as dormant — RW-4 overruled "dormant" to P1 (the kernel must be
sound against any EL0 program). `ee30f559` = the reviewer findings:
the byte-mode srvconn single-reader busy-guard ([[fnd-rw4-rev2-f1]],
a [[haz-single-waiter-rendez]] instance), the 9P dispatch-failure
fail-closed latch ([[fnd-rw4-rev3-f1]] — later refined by the round-2
catch [[fnd-rw4-rb-f1]]), the SQPOLL wait guard, `clone_walk_zero`'s
nqid==0 parity ([[fnd-rw4-rev1-f1]]), and the `dev->seekable` flag
([[fnd-rw4-rev2-f2]]). Rounds: [[adt-rw4-r1]] (4 Fable reviewers +
Opus self-audit) → [[adt-rw4-r2]] (the dirty-close re-prosecution,
converged clean).
