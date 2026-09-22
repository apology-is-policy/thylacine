---
id: dec-2026-09-17-tcp-transport-retirement
type: dec
title: "Retain TCP transports through bounded graceful close"
date: 2026-09-17
status: standing
decided-by: user-vote
affects: [sub-netd-server, sub-netd-nic, sub-netperf, sub-kernel-ninep-dev9p]
created: 2026-09-17
---
## Observation

The aux integration's 8 MiB NIC test verified 8,333,292 bytes rather than
8,388,608 after successful writes and immediate close, on both shared INTx
and ITS. Last clunk removed the socket with queued TX still inside it.

## Decision

The operator explicitly approved the bounded TCP close design and implementation
in `docs/NET-CLOSE-DESIGN.md`: separate public and transport lifetimes, bounded
graceful draining, admission and expiry diagnostics, followed by byte-verified
backend checks. Work remains single-agent. The proof obligation is received
bytes and lifecycle cleanup, never a benchmark's successful write count alone.

## Consequences

Closed public slots are immediately reusable; private transports consume
capacity until completion or the deadline. Connection-churn workloads must
handle resource refusal. The kernel must preserve the existing server errno
through open so ENOMEM can be distinguished from an I/O failure.
