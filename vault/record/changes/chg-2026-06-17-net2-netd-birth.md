---
id: chg-2026-06-17-net2-netd-birth
type: chg
title: "net-2: netd is born — smoltcp on the PCI NIC, the persistent /net 9P server, the live TCP fid machine, the net-2d close"
date: 2026-06-17
arc: arc-net
commits: ["a2a26142", "3bf89781", "bbed134d", "93686340", "3547635f", "aa364f51"]
touched: [sub-netd-nic, sub-netd-server]
established: []
closed: [fnd-net2d-r1-f1, fnd-net2d-r1-f2, fnd-net2d-r1-f3]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
net-2a (`a2a26142`): the warden-bound driver acquires a DHCP lease
through smoltcp over the virtio-PCI NIC — the whole lower stack
end-to-end. net-2b-1 (`3bf89781`): `lifecycle = persistent` (libdriver
Lifecycle + the warden leave-running arm); netd signals READY and stays
resident. net-2b-2 (`bbed134d`): the /net 9P server skeleton + the
MAY_POST_SERVICE one-hop conferral + the joey mount. net-2c-1
(`93686340`): the qid-encoded refcounted `/net/tcp` clone fid machine +
the ninep Treaddir codec. net-2c-2 (`3547635f`): the live TCP data path
(the Net table owns the iface + socket set; connect/hangup ctl verbs;
status/local/remote). net-2d (`aa364f51`): the first focused netd audit
close — [[adt-net2d-r1]].
