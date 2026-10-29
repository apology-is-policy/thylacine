---
id: chg-2026-10-04-hidden-storage-design
type: chg
title: "Approve cooperative hidden terminal pixel storage"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-tapestryd, sub-libtapestry]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The operator selected option 1 in HALCYON-HIDDEN-WEAVE-REVIEW: preserve hidden
terminal identity and semantic state while cooperatively retiring pixel buffers,
then recreate/repaint on reveal. TAPESTRY-STORAGE fixes the opt-in event/ctl
contract, generation-bound fids, bounded failure/retry and backend reference
obligations. No kernel mapping ceiling or private heap limit is raised.

This is the scripture checkpoint only. Formal lifecycle and implementation
verification, original full-size pressure recovery and clipboard/modal completion
remain owed. The original HI1-R30 run remains failed; no runtime result is claimed
for this design. Single-agent review under the operator's standing direction.
