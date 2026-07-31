---
id: chg-2026-06-24-349-flow-control
type: chg
title: "#349: c2s back-pressure is flow control, not session death"
date: 2026-06-24
arc: arc-go-build
commits: ["9bfe0851"]
touched: [sub-kernel-ninep-client]
established: []
closed: [fnd-349-self-sa1, fnd-349-r1-f1, fnd-349-r1-f2]
opened: [seam-350-async-eagain]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
A transiently-full c2s ring killed the whole shared session (a peer's
REVENANT text page-in included -- the on-device go-build failure). The fix
threads P9_TRANSPORT_EAGAIN (only at sent==0, the all-or-nothing contract)
into `client_send_flow`: self-pump one s2c frame when no reader is active
(the deadlock-breaker), else park until the reader signals progress, then
retry. The park is multi-waiter (`send_waiters_list`) after the formal
round's P1 ([[fnd-349-r1-f1]]: a single-waiter rendez was an unprivileged
SMP panic). Converged clean over two rounds + a concurrent self-audit
([[adt-349-self]] -> [[adt-349-r2]]). Prose: the commit message.
