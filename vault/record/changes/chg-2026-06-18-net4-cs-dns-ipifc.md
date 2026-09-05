---
id: chg-2026-06-18-net4-cs-dns-ipifc
type: chg
title: "net-4: /net/cs + the compiled-in ndb, /net/dns, /net/ipifc + /net/ndb + ipconfig, the net-4d close"
date: 2026-06-18
arc: arc-net
commits: ["c49bb544", "1f7a9719", "525348f3", "e52d8958"]
touched: [sub-netd-server, sub-netd-nic]
established: []
closed: [fnd-net4d-r1-f1, fnd-net4d-r1-f2, fnd-net4d-r1-sa3]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
net-4a (`c49bb544`): `/net/cs` (the dial front door) + the compiled-in
ndb(6) subset (`ndb.rs` + the baked `/lib/ndb/local` twin — the
confined-leaf config-at-construction decision). net-4b (`1f7a9719`):
`/net/dns` + cs→dns delegation over the shared lease-seeded resolver
socket — the deferred cs/dns read joins the held-reply family; the
smoltcp single-completion query discipline is the central hazard.
net-4c (`525348f3`): `/net/ipifc/0` + `/net/ndb` + the native
`ipconfig` (static config mutates iface + snapshot together). net-4d
(`e52d8958`): the audit close — [[adt-net4d-r1]] (the F1
deferred-overwrite lost-tag class + guards) → the precautionary
[[adt-net4d-r2]] (clean) — plus `proto_selftest`,
`dns_defer_guard_selftest` (the F1 regression), and the loopback DNS
E2E.
