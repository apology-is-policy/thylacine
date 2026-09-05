---
id: adt-net2d-r1
type: adt
title: "net-2d: the first focused netd audit (net-2a..2c-2)"
date: 2026-06-17
scope: [sub-netd-server, sub-netd-nic]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 4}
findings: [fnd-net2d-r1-f1, fnd-net2d-r1-f2, fnd-net2d-r1-f3, fnd-net2d-r1-f4, fnd-net2d-r1-f5, fnd-net2d-r1-sf4]
round-of: chg-2026-06-17-net2-netd-birth
created: 2026-07-31
---
One Opus-4.8-max prosecutor + a concurrent self-audit over the whole
netd surface. Both converged on the 10-argument SOUND set (fid refcount
balance, socket add/remove balance, disjoint-field borrows, parser
bounds, fail-closed non-live ops, single-threadedness, the I-5 probe
gate, the MAY_POST_SERVICE persistent gate, Treaddir bounds, the
Tgetattr trio) — the arguments are recorded so later rounds do not
re-derive them; the prosecutor additionally verified the ninep codec
contracts (nwname/count/frame bounds) netd's safety leans on. It caught
F1/F2 beyond the self-audit; the self-audit's SF1/SF2/SF3 folded into
F3/F5/the net-6 leg. NOT dirty (1 P2; formula/guard fixes). The owed
regression note became the standing host-test seam
([[seam-netd-host-tests]]): the fix triggers are architecturally
unreachable from the only in-VM client, and netd cannot host-test —
correctness rests on parity with the audited h_read budget + fail-closed
guards until the in-guest `proto_selftest` battery landed at net-4d.
