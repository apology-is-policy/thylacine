---
id: adt-net4d-r2
type: adt
title: "net-4d round 2 (precautionary): the F1 guards + the new selftests — clean, no finding"
date: 2026-06-18
scope: [sub-netd-server]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 0}
findings: []
round-of: chg-2026-06-18-net4-cs-dns-ipifc
prior-round: adt-net4d-r1
created: 2026-07-31
---
Discretionary round-2 (below the dirty bar — 1 P2, minimal guards —
but on the most-bug-prone deferred-reply family), overlapped with the
SMP gate. Clean, zero findings: the facet-1 guard preserves the held
tag and STRENGTHENS the single-completion discipline (the second read
skips dns_poll entirely); the facet-2 reject cannot wedge (deferred
always clears within the DNS timeout / Tflush / clunk / teardown) and
is fid-specific; `build_dns_response` is bounds-safe on any
malformed/pointer/truncated query; the new selftests are bounded,
panic-free, and non-tautological (`dns_defer_guard_selftest` fails on
pre-fix code by construction); the Content bump is inert; the round-1
SOUND set stands. CONVERGED clean over 2 rounds — the net-4 arc closed.
