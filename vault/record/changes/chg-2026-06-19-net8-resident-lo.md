---
id: chg-2026-06-19-net8-resident-lo
type: chg
title: "net-8: the resident loopback dual-stack + the live over-mount accept (#239) + the deferred connect (#257)"
date: 2026-06-19
arc: arc-net
commits: ["a8a7be13", "e90e6459", "25ca2856"]
touched: [sub-netd-server, sub-netd-nic]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
net-8a (`a8a7be13`): the resident loopback interface — a second
ISOLATED smoltcp stack (own Loopback device + iface + set, 127.0.0.1/8;
isolation load-bearing per the net-3d routing proof), opt-in
(`enable_loopback`), with per-slot `lo` routing (`set_ref`/`set_mut`)
and 127.x dial/announce migration. net-8b (`e90e6459`): the first LIVE
over-the-mount /net TCP accept (net-echo), which surfaced + fixed #239
— `FK_LISTEN` served FILE_RO while the accept opens it ORDWR (its fid
is rebound onto the accepted rw ctl), so the kernel's A-3 perm_check
denied the open before the Tlopen ever reached netd; latent since A-3
because the direct-method E2Es bypass perm_check. #257 (`25ca2856`,
post-[[adt-net8d-r1]]): the deferred `data` open — h_lopen on a
still-connecting TCP socket holds the Rlopen to ESTABLISHED /
ECONNREFUSED / ETIMEDOUT (the 4th deferred-reply leg); an immediate
Rlopen had let every real-RTT outbound client write into a SynSent
socket, masked entirely by the ~0-RTT loopback E2Es. The net-8c
TLS/soak commits are tls/net-echo-side (their sweeps). The net-8d
whole-arc audit ([[adt-net8d-r1]]) closed the arc.
