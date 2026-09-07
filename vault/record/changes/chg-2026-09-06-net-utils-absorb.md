---
id: chg-2026-09-06-net-utils-absorb
type: chg
title: "absorb docs/reference/124-net-utils (nslookup/ping/curl/wget): fold two client-facing atoms into sub-netd-server, multi-redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["26d61946"]
touched: [sub-netd-server]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
The native network CLI tools. Verified atom-by-atom across FOUR owning dossiers.

WHERE EACH ATOM LIVES (verified, not assumed):
- curl/wget engine (HTTP/1.0 explicit-close, TLS client, baked-CA self-test,
  AND the randomness-capability-for-https dependency -- sub-net-clients:115-116,
  already covered) -> sub-net-clients (owns usr/curl/*).
- nslookup/ping binaries -> sub-coreutils-presenters (owns ping.rs/nslookup.rs),
  though that dossier treats them as colour PRESENTERS, not for net semantics.
- The /net protocol they consume -- cs/dns resolver (numeric -> ndb -> DNS), the
  ICMP echo path (rotating Echo ident), resident loopback -> sub-netd-server
  (comprehensive on the SERVER side).
- net::resolve + net::IcmpSocket -> sub-libthyla-rs (owns net.rs; lists it in
  code: but has no net section -- owned, thinly described).

THE FOLD (the atoms that lived only in the doc):
Two CLIENT-facing consequences of the /net protocol were uncovered:
1. The /net/cs 0-service footgun -- a resolve for the IP only must still pass a
   NON-ZERO service, because /net/cs does not special-case 0: it falls through to
   an ndb lookup of the literal "0" and misses. nslookup/ping pass 80 even when
   the port is immaterial. Folded into netd-server's cs resolver mechanism (where
   the numeric-first order that causes it lives).
2. ping's seam #256 -- an ICMP error (Destination Unreachable) quoting our ident
   makes the socket readable, so a recv that assumes the first readiness edge is
   the EchoReply consumes the error and waits for a reply that may never come,
   bounded only by the client's 1s poll. v1.x fix: a per-recv deadline (net-8d F2
   lever). The server-side WouldBlock mechanism was already in netd-server's ICMP
   caveat; this names the numbered client seam + its fix there.

Folded into sub-netd-server (audit: hard). NOT REFUTED: everything else in the
doc is current and home. Multi-redirect stub. Zero code change.
