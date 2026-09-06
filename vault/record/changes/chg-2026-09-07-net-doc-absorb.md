---
id: chg-2026-09-07-net-doc-absorb
type: chg
title: "absorb docs/reference/122-net (libthyla-rs::net, the native /net client): fold the SNTP trust model into sub-net-clients; redirect the protocol to sub-netd-server"
date: 2026-09-07
arc: arc-vault
commits: ["PENDING"]
touched: [sub-net-clients]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-07
---
The native `std::net`-shaped client over netd's `/net` tree (TcpStream/
TcpListener/UdpSocket/IcmpSocket). Kernel surface: none (a pure /net client).
`quaestor owner`: net.rs -> sub-libthyla-rs; net-echo/sntp -> sub-net-clients.
Verified atom-by-atom, and the verification changed the disposition from a clean
redirect to a fold.

WHERE EACH ATOM LIVES (verified against the dossiers + current code, not assumed):
- The /net PROTOCOL (clone idiom, ctl verbs connect/announce/hangup, the
  N/{ctl,data,local,remote,status,err,ready}+listen layout, the readiness split,
  the refcounted connection table whose last-clunk frees N, the deferred-reply
  lifecycle) -> sub-netd-server (audit:hard). The server DEFINES it; net.rs is a
  typed mirror. sub-netd-server is AHEAD of the doc -- it carries #257 (SynSent
  premature-Rlopen), #293 (connect deadline), #239 (FK_LISTEN rw mode) the doc
  never had.
- The ping/IcmpSocket recv seam #256 (an ICMP-error recv fooled into waiting out
  its poll) -> sub-netd-server (folded earlier this run, chg-2026-09-06-net-utils).
- The runtime principles the client composes (RAII/last-clunk fd+slot lifetime,
  the single error decoder, ISV-safe MMIO) -> sub-libthyla-rs, which states its
  scope excludes per-module API catalogs (the typed API surface is code-level /
  rustdoc, deliberately).

THE FOLD (genuine gap -> sub-net-clients, depth rich):
- The SNTP TRUST MODEL was homeless -- grep found era-2036 / originate / off-path
  / NTS NOWHERE in the vault. sub-net-clients carried the clock-step DENIAL
  self-test but not WHY an unauthenticated time source is trustable: the
  per-request originate-nonce (off-path-spoof defense), the on-path attacker as
  the documented boundary (NTS v1.x hardening), the mode-4+stratum[1,15]+non-zero-
  transmit+originate-echo validation battery, and the era-2036 SATURATING
  1900->Unix conversion (`ntp_secs - 2_208_988_800`). Folded into Mechanism (the
  nonce + battery) + Seams (the on-path boundary + era-2036). updated: 09-06 ->
  09-07.

Redirect stub names the fold + the two deliberate non-folds (protocol=server,
API=code). Zero code change.
