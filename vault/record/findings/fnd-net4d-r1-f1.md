---
id: fnd-net4d-r1-f1
type: fnd
title: "A held deferred cs/dns read tag could be LOST — the single deferred slot clobbered by a concurrent read or re-write"
round: adt-net4d-r1
severity: P2
status: fixed
surface: [sub-netd-server]
threatens: [inv-i9]
fixed-by: chg-2026-06-18-net4-cs-dns-ipifc
regression: "netd dns_defer_guard_selftest (boot-asserted; fails on pre-fix code by construction)"
created: 2026-07-31
---
## Prosecution

`Query.deferred` is a single (tag, cap) slot. Facet 1: a SECOND
concurrent Tread on the same fid — LEGAL from the trusted kernel mount
via a multi-threaded Proc, since the dev9p client multiplexes by TAG —
OVERWROTE the held marker; poll_dns then delivered only the second tag
and the first read hung until Proc death. Facet 2: a re-write while
deferred (`query_begin → query_drop`) discarded the Query including its
held marker. Facet 3 (bare clunk while deferred) is mitigated on the
trusted mount (Tflush → cancel_dns_flush) and a protocol violation
otherwise — dispositioned consistently with the net-3 listen-clunk
close. No crash, self-inflicted, death-recoverable, loss never
double-delivery → P2 not P1. The self-audit found the root first (as a
P3, under-rating reachability); the prosecutor elevated it and found
facet 2.

## Disposition

Fixed with two MINIMAL guards (deliberately not a wait/wake
restructure): `query_read` answers a second concurrent read with an
empty Rread (the first keeps its real answer — preserving I-9's
no-lost-held-reply) and `h_write` rejects (E_PROTO) a re-write while
`fid_has_deferred`. Prosecuted again clean by [[adt-net4d-r2]]; the
guard strengthens the DNS single-completion discipline (the second
read skips dns_poll entirely).
