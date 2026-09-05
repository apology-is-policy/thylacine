---
id: moc-userspace-netd
type: moc
title: "netd — the network daemon"
parent: moc-userspace
created: 2026-07-31
updated: 2026-07-31
---
netd (`usr/netd`) is the NIC-owning network stack Proc — the #68
charter's center: the Menagerie warden binds it NARROWED to the
`virtio-pci:1` allowance (I-34), it claims the device (the claimer IS
the stack — the I-5 non-transferable-handle consequence), embeds smoltcp
0.12, and serves the Plan 9 `/net` tree as a 9P server over `/srv/net`.
Every `/net` access from every Proc in the namespace multiplexes over
ONE kernel dev9p session; the namespace is the firewall (I-1/I-28 — a
Proc that can name `/net` can dial).

netd is **single-threaded by construction** — one Proc, one serve loop,
every 9P frame across every session processed sequentially — so the
global connection table and the smoltcp socket sets need no lock, and
the deferred-reply engines' no-lost-wakeup property rests on loop
ordering, not a wait/wake protocol. Both properties are load-bearing
(the dossiers' Concurrency sections carry the obligations a future
concurrency lift must re-establish).

## Children

- [[sub-netd-nic]] — the driver + stack half (`main.rs`): the warden
  bind, the smoltcp `phy::Device` over the virtio NIC, DHCP bring-up +
  re-apply, the boot selftest battery, and the resident serve loop (the
  poll-cadence policy + the delivery passes).
- [[sub-netd-server]] — the `/net` 9P server half (`server.rs` +
  `ndb.rs`): the qid-encoded tree, the refcounted connection table over
  the shared TCP/UDP/ICMP slot pool, the five deferred-reply engines,
  cs/dns/ndb/ipifc, the readiness (`ready`) file, and the weft zero-copy
  drive.

## Cross-cutting

- Invariants: [[inv-i9]] (the deferred-reply engines' userspace
  register-then-observe — serve-loop ordering) · composes I-1/I-5/I-23/
  I-28 (their inv notes land with the kernel-side sweeps).
- Spec: [[spec-net-poll]] — the readiness bridge; netd's `ready` file is
  the model's server half. [[spec-net-poll-teardown]] models the kernel
  cancel-at-close it answers to.
- Hazards: [[haz-driver-panic-dos]] — a netd panic is a whole-network
  DoS (the sole-owner consequence); the recurring prosecution frame.
- Carriers: posts via [[sub-kernel-devsrv]] (create=post, 9P-mode);
  reached via [[sub-kernel-ninep-dev9p]] over the srvconn transport;
  probed by [[sub-kernel-ninep-dev9p-poll]] (the readiness bridge).
- Record: [[arc-net]] (the charter arc) · [[arc-weft]] (the zero-copy
  dataplane that grew beside it).
