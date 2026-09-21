---
id: sub-netperf
type: sub
title: "netperf — native loopback and NIC measurements"
parent: moc-userspace-netd
code: [usr/netperf/src/main.rs, usr/netperf/Cargo.toml]
audit: light
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/NET-PERF.md", "docs/NET-CLOSE-DESIGN.md"]
created: 2026-09-17
updated: 2026-09-21
---
## Purpose

Native measurements through the public `/net` API: loopback RTT, bulk byte-copy
throughput, connection latency, Weft throughput and the separate NIC workload.
Joey runs the default loopback probe and requires successful completion.

## Contract

## Mechanism

M1 reuses a single established connection for one-byte ping-pong. M2 uses a
second thread as receiver and measures the transfer through receiver completion.
M3 repeatedly connects, accepts and closes both ends. The Weft comparison uses
mapped rings and readiness backpressure; its breakdown separates data movement
from readiness stalls.

M3 and M6 connection-churn measurements use `connect_with_admission`, and so do the phases that FOLLOW a churn (MW after M3, the M6 throughput leg after its connect leg): a plain connect there made the phase, and with it the boot, depend on whether a retiree had aged out yet (one boot in about eighty extincted on it). Only
ENOMEM (the existing fixed-resource exhaustion error) retries, every 25 ms,
with a 35-second deadline. All admission waiting remains inside the measured
dial latency and a diagnostic reports refusals and recovery. Other errors fail
immediately. This is needed because [[sub-netd-server]] retains closing TCP
transports through TIME-WAIT within its bounded pool; closing a public fd does
not immediately restore transport capacity.

The NIC command uses consecutive host ports for immediate echo, delayed echo
and bulk sink. It is a best-effort measurement program: its successful process
exit alone does not prove that the requested work ran. The bulk measurement
reports accepted sends and closes immediately; it does not itself verify host
receipt. The `pci-net-load` interactive gate therefore rejects ERR/SKIP,
requires every measurement witness and independently verifies all 8 MiB at
the host, including payload contents and EOF.

## Data structures

Per-metric counters and sample extrema; shared atomic progress for bulk
receivers, separately allocated thread stacks, and readiness poll sets.

## Concurrency

Bulk receivers use separately allocated thread stacks and explicit join/progress
coordination. Failure must not release a stack while its worker still runs.
The benchmarks consume the same bounded network resources as applications;
there is no privileged bypass, larger hidden transport pool, or delayed close.

## Invariants enforced

An outstanding bulk worker keeps its stack until joined. No retry changes
stream contents or makes accepted sends equivalent to peer receipt.

## Error paths

Default probe errors terminate nonzero. NIC metrics may report ERR/SKIP and
still exit zero; consuming gates must inspect witnesses and independent data.
Admission retries are errno-specific and bounded.

## Performance

Metrics print workload size and elapsed time. Admission waits are included in
dial latency and separately reported; no historical timing is an acceptance bar.

## Prosecution

Test exhausted admission, failed thread creation, stalled readiness, EOF before
full receive, and workers that terminate while their parent is waiting. Never
accept the NIC program's exit status alone as proof.

## Seams

M6 bulk timing is send-completion only; host byte verification belongs to the
interactive fixture. It does not itself wait for an application acknowledgement.

## Caveats

These measurements include the guest scheduler, 9P transport, and network
stack; they are not isolated NIC hardware timings.

## Provenance

`docs/NET-PERF.md` records the original metric workloads. The bounded retirement
contract is [[dec-2026-09-17-tcp-transport-retirement]].

## Tests

Guest boot exercises M1/M2/M3/Weft through the mounted service. The NIC gate
exercises immediate and delayed RX, connection churn, and host-observed stream
completion. It is blind to packet loss/reordering outside QEMU, real-hardware
interrupt delivery, and application protocol acknowledgements beyond the test
fixture. Historical timing values in NET-PERF describe their recorded workload
and must not be compared to runs with admission waits without disclosing them.
