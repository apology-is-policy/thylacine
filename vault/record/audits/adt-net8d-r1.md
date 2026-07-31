---
id: adt-net8d-r1
type: adt
title: "net-8d: the whole net-8 sub-arc close — the network-arc EXIT"
date: 2026-06-19
scope: [sub-netd-server, sub-netd-nic]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 4}
findings: [fnd-net8d-r1-f1, fnd-net8d-r1-f3]
round-of: chg-2026-06-19-net8-resident-lo
prior-round: adt-net4d-r2
created: 2026-07-31
---
Scope: net-8a (the resident lo dual-stack) + net-8b (#239 FILE_RW +
the live over-mount accept) + the net-8c TLS/soak commits (tls +
net-echo surfaces — their findings stay in this body until those
sweeps). Kernel byte-unchanged across all four commits (diff-verified).
Both prosecutors CONFIRMED the net-8a routing partition sound via a
grep-complete socket-touch enumeration (every slot-keyed access routes
by `slot.lo`; the direct `self.sockets` sites are exactly the DNS/DHCP
sockets, the clone mints, the ensure_lo_stack old-socket removal, and
the selftest throwaways — the wrong-set panic class unreachable), #239
as the narrowest possible change, and the two-thread net-echo
orchestration's register-then-observe. The prosecutor's F1 (the
one-way lo migration mis-route — [[fnd-net8d-r1-f1]]) was the one the
self-audit missed; F3 ([[fnd-net8d-r1-f3]]) is the selftest-non-fatal
posture. Out-of-surface P3s recorded here, not as fnds: **F2/SP3-1**
(#241 — the shared TLS `handshake` loop has no overall bound; a
hostile peer dripping never-advancing bytes loops forever;
death-interruptible, no v1.0 network-exposed TLS server — the tls
sweep's backfill) and **F4** (#243 — the fixed 4 MiB ThylaAlloc heap
would OOM-abort a future high-volume TLS service — libthyla-rs's
backfill); SP3-2/SP3-3 (net-echo probe ergonomics). SMP gate 40/40,
0 corruption, every timing boot ground-truthed to the healthy
end-state. THE NETWORK ARC (net-1..net-8) CLOSED at this round.
