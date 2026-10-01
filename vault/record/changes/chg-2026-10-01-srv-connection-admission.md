---
id: chg-2026-10-01-srv-connection-admission
type: chg
title: "Reserve service-connection capacity before allocation"
date: 2026-10-01
arc: arc-astra-halcyon-followup
commits: ["*(pending)*"]
touched: [sub-kernel-devsrv, sub-kernel-srvconn]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
O1-SRV-2 replaces the racy diagnostic-counter admission check with an atomic
reservation in the constructor. Capacity remains charged through in-flight
allocation and torn-but-retained transports until storage is freed. All three
allocation-failure sites roll back; the public ABI and limit of 64 are unchanged.

Default build and single-CPU boot pass 1830/1830. The native lifecycle fixture
passes its controlled schedules and rejects five intended mutants. Single-agent
self-review; no independent audit. The operator suspended Astra's 50-boot,
ASan, UBSan and SMP gates for October 1-2; no fresh pass is claimed for them.
[[seam-srv-registry-lifecycle]] remains open for O1-SRV-1 session namespace,
poster death, capacity and fairness work. No Main landing or graphical claim.

The unchanged full Corvus model run exceeded its 180-second run limit with
unexplored states and no observed counterexample; it is incomplete, not PASS.
That model does not represent constructor allocation admission. Its existing
eight negative configurations all produce the expected counterexamples; logs
are retained in the checkpoint evidence.
