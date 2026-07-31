---
id: adt-term4-r1
type: adt
title: "term-4 batched round: G1 + G2 + G3/G4 (+ the Stratum reader/shelf chunks)"
date: 2026-07-13
scope: [sub-kernel-larder, sub-kernel-ninep-dev9p]
reviewer: fable
model-start: claude-fable-5
model-end: claude-fable-5
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 4}
findings: [fnd-term4-r1-f3, fnd-term4-self-sa1]
round-of: arc-go-build
created: 2026-07-31
---
One Fable-5-max round batched over four Thylacine chunks (G1
write-populate, G2 dir-fid cache, G3 downgrade + G4 qid-scoped ring) and
two Stratum chunks (A-1 buffered frame reader, A-2 dirty-buffer shelf) +
a concurrent self-audit. NOT dirty (the P2 fix is a one-line free). The
Thylacine-surface findings are listed; the other three were
STRATUM-side, closed on that branch: F1 [P2] the A-1 64 KiB reader
buffer leaked on the all-pthread-create-failed teardown (the
pre-existing block not updated when the alloc landed above it); F2 [P3]
the shelf reuse-pool detectability caveat (a reused over-cap buffer
returns stale bytes where a fresh malloc ASan-traps — every consumer
verified to serve strictly the record length); F4 [P3] the rdbuf
ensure-contract made explicit (cross-confirmed by the self-audit's
independent unreachability proof). Verified sound on THIS surface
(do-not-re-litigate): G1's own-serve ≡ server bytes (install only on the
flush's err==0 full-land arm; the flush-start HARD attr-invalidate
precedes every own-install; the append-chain extend only onto an OWN
page ending at the run start; single-writer holds — only
create/OTRUNC-born privs are wb-eligible); G3's perm-preservation real
(only wstat edits parent perm bits, and wstat full-drops; every
perm_only consumer reads fresh-by-construction fields); G4's ring math
airtight (n ≤ 128 → each in-window slot holds its own event; overflow
fail-safes at all three consumers; the ALL-events install scan vs
HARD-only donate scan is the correct split at each site) with the
event-logging completeness swept over every mutation site; G2's
exclusive take (one Spoor per live fid), the three-layer stale-fid
defense under the single-writer premise, and the evicted-dentry residual
bounded to a fid leak, never a wrong serve. Coverage notes for the next
prosecutor: TLC not machine-run in the round (the EnableFlushPopulate
delta hand-verified); the SMP interleavings witnessed by the 40/40 gate,
not a deterministic harness.
