---
id: chg-2026-10-04-async-buffer-pool-core
type: chg
title: "Implement bounded internal payload pool transitions"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-loom, sub-kernel-loom-pools]
established: [sub-kernel-loom-pools]
closed: []
opened: []
mirrors-checked:
  - "kernel/test/test_loom.c: unchanged seal/identity tests; added shared pool fixture"
depth: skeletal
---
Under scripture30695b43e, ABI3eb14ae73 and modele6a50ae1e, add the allocation-free
pool metadata core, compiled but not connected to private dispatch. Actual-source
ASan/UBSan and eleven intended mutations pass; native shared fixture passes in a
fresh1830/1830 boot. Broad qualification remains owed for this new source.
Caller authority, locks, pins/charges, transport and CQ ordering remain explicit
integration gates. Four protected drafts are separate. Single-agent self-review.
