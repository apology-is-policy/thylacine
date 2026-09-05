---
id: adt-net3d-r1
type: adt
title: "net-3d round 1: the deferred-accept strand class (F1 P1)"
date: 2026-06-17
scope: [sub-netd-server, sub-netd-nic]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 2}
findings: [fnd-net3d-r1-f1, fnd-net3d-r1-f2, fnd-net3d-r1-f3, fnd-net3d-r1-f4]
round-of: chg-2026-06-17-net3-server-side
prior-round: adt-net2d-r1
created: 2026-07-31
---
The canonical self-audit latent-P1 trap, caught by the second
prosecutor: the self-audit reasoned "the listen fid pins N so it cannot
free + re-mint" — but the deferred-listen fid was left HALF-OPEN
(`opened == false` with a committed PendingAccept) and `fid_clunk` does
not gate on `opened`, so a hostile client could clunk it, free N, and
re-mint the index cross-proto; the stranded pending then type-confused
`get::<tcp::Socket>` — a smoltcp downcast PANIC in the sole NIC owner
(whole-network DoS; the [[haz-driver-panic-dos]] shape). "The trusted
kernel client only abandons via Tflush" is not a safety argument. The
self-audit's independent contribution: the smoltcp ROUTING PROOF (a
loopback iface sharing the NIC set mis-routes — the default route steals
127.x egress), which forced the isolated-stack design of the loopback
E2E; plus SA-1/SA-2/SA-3 (ident rotation == F3; oversize-ICMP
fail-closed; the announce-fd-keeps-listening idiom — dossier caveats).
DIRTY (a P1 on the deferred-reply lineage) → [[adt-net3d-r2]] on the
fix.
