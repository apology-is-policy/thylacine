---
id: fnd-d44-r1-f3
type: fnd
title: "OTRUNC open invalidated NOTHING — truncate coherence rested on an unverified cross-project guarantee"
round: adt-d44-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: [inv-i38]
fixed-by: chg-2026-07-11-d44-read-band
regression: dev9p.open_trunc_invalidates
created: 2026-07-31
---
## Prosecution

`dev9p_open` with OTRUNC truncates server-side but dropped no cached
state: the attr + page caches' soundness rested on Stratum bumping
qid.version on truncate — an UNVERIFIED cross-project assumption, and
the new attr-served EOF was about to lean on it harder (a stale cached
size after a truncate would keep answering the old EOF). PRE-EXISTING
since L1e; owned and fixed here rather than walked past.

## Disposition

Fixed: an OTRUNC open drops the file's cached attr + pages exactly like
a write — the truncate IS an own write (own-write-through needs no
server-version reasoning at all). The same guarantee-hygiene rule as the
reused-ino page fix: data coherence never rests on an unstated
cross-project property.
