---
id: adt-net4d-r1
type: adt
title: "net-4d round 1: the deferred-overwrite lost-tag class (F1 P2)"
date: 2026-06-18
scope: [sub-netd-server, sub-netd-nic]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 3}
findings: [fnd-net4d-r1-f1, fnd-net4d-r1-f2, fnd-net4d-r1-f3, fnd-net4d-r1-sa3]
round-of: chg-2026-06-18-net4-cs-dns-ipifc
prior-round: adt-net3d-r2
created: 2026-07-31
---
Prosecutor + concurrent self-audit CONVERGED on the deferred-overwrite
root (the audit-in-flight value in both directions: the self-audit
found it first as a P3, under-rating reachability; the prosecutor
elevated it to P2 — the trusted kernel mount reaches it via a LEGAL
multi-threaded Proc, since the dev9p client multiplexes by TAG — and
found the second facet, the re-write drop). The highest-risk class —
the smoltcp `get_query_result` single-completion (a double-poll of a
freed slot PANICS → network DoS) — was verified ROBUST against the real
smoltcp source: the handle lives in exactly one place, nulled on every
result arm; cancel only while pending; a held read is bounded by the
~10 s DNS retransmit timeout. The bare-clunk-while-deferred facet was
dispositioned as a client protocol violation (the trusted mount
Tflushes), consistent with the net-3 close. NOT dirty (minimal guards)
— but the deferred-reply lineage earned the discretionary
[[adt-net4d-r2]].
