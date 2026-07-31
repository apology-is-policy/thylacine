---
id: adt-d44-r1
type: adt
title: "D44 read-band round (the false-mid-file-EOF catch)"
date: 2026-07-11
scope: [sub-kernel-ninep-dev9p, sub-kernel-larder]
reviewer: fable
model-start: claude-fable-5
model-end: claude-fable-5
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 2}
findings: [fnd-d44-r1-f1, fnd-d44-r1-f2, fnd-d44-r1-f3]
round-of: chg-2026-07-11-d44-read-band
created: 2026-07-31
---
Fable-5-max prosecutor over the aligned-wire-read + attr-served-EOF
chunk. The concurrent 16-trace self-audit MISSED F1: its sweep verified
every big reader LOOPS on short reads, but never confronted what each
loop's TERMINATION condition means under the new return values — the
prosecutor grounded F1 in the R-5 scripture ("a single Rread may
legitimately short-return for an interior page"). The recorded lesson:
**an "every consumer loops" sweep is incomplete until you ask what each
loop's `n == 0` exit MEANS under every value your change can return — a
termination condition is itself a consumer contract a new return path
must honor.** Verified sound (do-not-re-litigate): the overlap-safe
forward `co_copy` (word path implies disjointness; byte path covers the
short leads); the populate-loop bounds with `wire_off` (the front page
installs full, no OOB); the shift success-case byte-exactness; the
attr-EOF cvers gate (own-write invalidates; stale-large conservative; no
stale-small under the premise); the QTDIR gate; created/reused files
carrying no cached attr; the cacheable gate covering all three new
behaviors (netd byte-exact); the gen guard covering the shifted fetch's
front-page install (composing with the B1 witness); I-13 kernel-only
copies; I-32 zero-alloc common path; the loopback fixture's under-msize
buffer latent closed.
