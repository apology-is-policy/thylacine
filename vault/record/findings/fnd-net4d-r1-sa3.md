---
id: fnd-net4d-r1-sa3
type: fnd
title: "The main.rs DHCP comment implied the lease was actively re-applied — it was pinned at bring-up"
round: adt-net4d-r1
severity: P3
status: fixed
surface: [sub-netd-nic]
threatens: []
fixed-by: chg-2026-06-18-net4-cs-dns-ipifc
created: 2026-07-31
---
## Prosecution

A self-audit finding: the comment read as if renewals reached the live
iface, when the resident loop never drained the dhcp `Configured` event
— both iface and snapshot were coherently FROZEN at the bring-up lease
(the renewal re-application being the recorded v1.x seam of the era).

## Disposition

Fixed at the close: the comment clarified (the protocol keeps the lease
alive at L3; the address is pinned at bring-up). SUPERSEDED three days
later by [[chg-2026-06-21-netd-293-connect-sweep]]'s `poll_dhcp`
re-apply pass, which retired the seam — after which the ORIGINAL
bring-up comment became stale in the opposite direction (recorded as a
[[sub-netd-nic]] caveat; the round-time truth here stays frozen).
