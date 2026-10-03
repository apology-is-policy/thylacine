---
id: chg-2026-10-03-weighted-srv-admission
type: chg
title: "Charge service connections by immutable ring allocation"
date: 2026-10-03
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-srvconn, sub-kernel-devsrv, sub-kernel-devctl]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
Implement operator-approved option A after scripture6d9cc4647. Default1/bulk4
credits preserve the32MiB global ring ceiling with96/192/256 local/combined/
global limits. Connection counts remain distinct. Credits live through final
storage destruction; partial allocation returns the same reserved charge.
The admission summary precedes detailed rows so truncation cannot hide usage.

Fifty clean default/SMP/UBSan boots, the eleven intended source mutants and
native service/media/F10 SAK results are recorded in HALCYON-INTERACTION-STATUS
and work/oct3-hi-credits. Full clipboard qualification remains open. Review is
single-agent.
No clipboard activation or Main landing is implied by this kernel prerequisite.
