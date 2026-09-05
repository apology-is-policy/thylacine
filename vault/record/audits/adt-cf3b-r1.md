---
id: adt-cf3b-r1
type: adt
title: "CF-3 B focused round (bulk rings + the #354 role park)"
date: 2026-07-08
scope: [sub-kernel-srvconn, sub-kernel-devsrv, sub-kernel-ninep-attach, sub-kernel-ninep-client]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 1}
findings: [fnd-cf3b-r1-f1, fnd-cf3b-r1-f2, fnd-cf3b-self-freeb]
round-of: chg-2026-07-08-cf3b-bulk-ring
created: 2026-07-31
---
## Scope

`DMSRVBULK` → `ring_msize` → heap per-conn rings + `srvconn_msize` → the
9p_attach msize proposal → the p9_client two-tier out_buf; the #354 role
park; the new blocking byte-client send + the server-side drain-wakes;
pouch 0020 (SO_SNDBUF ≥ 128 KiB → DMSRVBULK) and the Stratum listener
sockbuf. Fable-5-max holotype-reviewer (MODEL end == the pinned agent
model — no mid-run fallback) + a concurrent self-audit. NOT dirty (two
localized fixes, no wait/wake-protocol restructure); both formal
findings fixed in the chunk commit.

## Convergence

The self-audit's verified-sound trace: ring lifetime (every blocking op
holds a conn ref across its park → no parked party can exist at the last
unref); the role-park teardown chains (release wakes unconditionally);
TIMEDOUT-vs-INTR `client_timed_out` separation; the deadline/role
composition on 9p-mode conns (reader_active serializes transport recvs →
the s2c reading role is never contended there); the drain-wake reaching
exactly its consumer; the non-blocking sends now test-only; the
negotiation chain probe-pinned end-to-end (the exact 131049/131061
cf3-bulk asserts fail loudly on any class fallback or clamp drift). The
pre-audit in-chunk finds were the freeb wedge ([[fnd-cf3b-self-freeb]] —
found by ground truth, not the theory loop; the lesson: a
sloppy-exclusion grep is not a completeness proof), the `chan_copy`
strict-aliasing UB (fixed via the aligned(1) may_alias typedef), and a
missed Stratum-tests caller sweep (caught by the full cmake build).
Posture: suite 1042 → 1047 green; SMP gate 40/40 twice (pre-fix and
final bytes), 0 corruption; the fresh-goroot bench twins became the new
baseline.
