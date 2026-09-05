---
id: chg-2026-06-21-netd-293-connect-sweep
type: chg
title: "netd #293: bound stuck connects (the ARP-storm DNS death) + DHCP re-apply + ipconfig renew"
date: 2026-06-21
arc: arc-net
commits: ["db5f2e4b"]
touched: [sub-netd-server, sub-netd-nic]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
#293: a live NIC's DNS died permanently ~60 s after boot. Root cause —
proven by PCAP + a slot-table dump, NOT the DHCP theory the 60 s timing
suggested: an abandoned boot-probe dial to an unreachable guestfwd
address left a TCP socket stuck SynSent, re-ARPing forever; smoltcp's
single GLOBAL ARP rate-limit then starved the DNS server's re-ARP at
its 60 s neighbor-cache expiry (the T1 coincidence). Fix:
`sweep_stale_connects` every tick — a slot still SynSent past 15 s is
DROPPED by REMOVING its socket from the set, not `abort()` (an abort
RSTs, which to an unreachable peer needs the same unresolved neighbor —
verified: abort left the storm running). The slot survives while fids
ref it; `err` set → check_ready reports POLLERR (stranded probes
complete); `slot_unref` finds socket=None. Companions folded in (the
"netd maintains its config" family, not the #293 fix): `poll_dhcp`
lease re-apply in the resident loop + the `renew` ipifc verb backing
`ipconfig renew`. Regression: `connect_sweep_selftest` (boot-asserted).
