---
id: fnd-net2d-r1-f1
type: fnd
title: "h_readdir's budget omitted the Rreaddir frame overhead — a small-msize client could receive an over-msize reply"
round: adt-net2d-r1
severity: P2
status: fixed
surface: [sub-netd-server]
threatens: []
fixed-by: chg-2026-06-17-net2-netd-birth
created: 2026-07-31
---
## Prosecution

The readdir budget was `count.min(msize)` while `h_read`'s data cap
reserves the 11-byte frame overhead (`P9_HDR_LEN + 4`) — so a populated
directory read by a small-msize client yields an Rreaddir frame LARGER
than the negotiated msize: a wrong invariant in audit-bearing framing
code. Latent in-VM (the only client is the large-msize kernel mount;
the /net tree is shallow).

## Disposition

Fixed in the close: the `rreaddir_budget(count, msize)` helper =
`min(count, msize − (P9_HDR_LEN+4))`, parity with the audited h_read
cap; `h_readdir` calls it. No deterministic runtime regression exists
(the trigger is architecturally unreachable from the trusted mount and
netd cannot host-test — the origin of [[seam-netd-host-tests]]);
correctness rests on the data-path parity + the ninep `build_rreaddir`
length guard.
