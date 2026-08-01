---
id: fnd-rw4-rev3-f1
type: fnd
title: "RW-4 R3-F1: an owned-reply dispatch failure leaked the tag slot ungated"
round: adt-rw4-r1
severity: P2
status: fixed
surface: [sub-kernel-ninep-session]
threatens: []
fixed-by: chg-2026-06-10-rw4-fixes
created: 2026-08-01
---
## Prosecution

A well-framed reply whose TYPE is neither the expected R-message nor
Rlerror (or a strict-parse / tag-echo failure) made
`p9_session_dispatch_rmsg` return < 0 BEFORE `clear_outstanding` — the
`outstanding[tag]` slot leaked (≤ 64 → a session wedge), and a later
well-formed reply on the still-active tag dispatched ownerlessly,
mutating fid state. Asymmetric with #841-F5's fail-closed discipline.

## Disposition

Fixed at `ee30f559`: `client_mark_dead_locked(c)` on a negative
dispatch in BOTH the sync DONE path and the async demux path (verified
a benign Rlerror returns 0 and never trips it). The fix was then
REFINED by the round-2 catch [[fnd-rw4-rb-f1]]: the latch premise was
over-broad for the local fid_bind-full leg — the two notes together are
the "latch-vs-per-op-error" classification lesson.
