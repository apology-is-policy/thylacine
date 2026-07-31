---
id: fnd-net3d-r1-f1
type: fnd
title: "A clunked half-open listen fid stranded its PendingAccept — cross-proto slot re-mint type-confused the typed get (smoltcp panic → network DoS)"
round: adt-net3d-r1
severity: P1
status: fixed
surface: [sub-netd-server]
threatens: []
fixed-by: chg-2026-06-17-net3-server-side
regression: "netd loopback_e2e (boot-asserted; drives the fixed poll_accepts/accept_swap in-guest)"
created: 2026-07-31
---
## Prosecution

h_lopen's FK_LISTEN branch set defer + returned without marking the fid
`opened`, leaving it HALF-OPEN with a committed PendingAccept. A native
client clunks it (`fid_clunk` did not gate on `opened` and did not
cancel the pending), clunks the ctl fid → N frees → `clone` re-mints
the index CROSS-PROTO → the stranded `PendingAccept{listening_n=N}`
(no generation guard existed) resolves via
`get::<tcp::Socket>(udp_handle)` — a smoltcp downcast PANIC (verified:
`get` `.expect()`s on type mismatch; `add` reuses the freed index) in
the sole NIC owner → whole-network DoS ([[haz-driver-panic-dos]]).
Facets off the same root: walk-from, double-defer, connection-hijack
(an unsolicited Rlopen onto a reused fid). Latent in-VM (the trusted
kernel client abandons only via Tflush) but reachable from any native
open=connect client — the soundness bar makes it P1, and the
self-audit's "the listen fid pins N" reasoning is the canonical
latent-P1 trap.

## Disposition

Fixed by FOUR complementary layers: the per-slot monotonic mint `gen`
(+ `PendingAccept.listening_gen`; N keeps its gen on re-arm — required
for sibling pendings) + the poll_accepts proto+gen GUARD (panic and
strand both become a harmless drop — locally sound regardless of the
fid-pin invariant) + `cancel_accept_fid` in fid_clunk + the FK_LISTEN
`opened=true` busy-mark (complete_accept's rebind deliberately ignores
it). Prosecuted again by [[adt-net3d-r2]]; the guard discipline became
the standing template for every later pending engine.
