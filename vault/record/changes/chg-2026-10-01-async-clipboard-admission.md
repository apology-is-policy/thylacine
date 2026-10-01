---
id: chg-2026-10-01-async-clipboard-admission
type: chg
title: "Asynchronous clipboard admission and one-use pending decisions"
date: 2026-10-01
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-libtapestry, sub-tapestryd, sub-kaua-term]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The pure clipboard broker pins Get snapshots and prepares Commit before an
ordered HIA1 CHECK. One pending decision globally serializes publication and
bounds metadata. Complete authenticated scope and exact connection/fid/request
target remain attached until completion. Old/duplicate receipts cannot consume
a new operation. Ordered graphical focus loss preserves only an earlier
admission; controller/peer retirement and SAK cancel pending work outright.

The native client opens one ctl at setup and uses a four-entry Loom SQPOLL ring
for bounded asynchronous WRITE and positioned READ. One SQE remains in flight,
with stable registered storage until CQE or ring destruction. No per-action
file open, worker thread or blocking wait is added. The compositor caches the
full latest request/decision: exact retries return it without reapplying, higher
IDs replace it, and old/altered IDs fail. A HIA1 fid refuses text verbs.

Evidence in work/oct1-hi-broker covers 476 Halcyon host tests, 30 actual-source
broker/store/codec/exchange tests, seven intended mutation failures, the existing
compositor fixture and six intended negatives, ordinary CPU1 boot 1830/1830,
and a native admission probe passing in 40.16 seconds. The native probe tests
real clipboard bytes and background denial, snapshot lifetime, explicit broker disconnect cleanup and
in-flight channel drop. Formatting and compile-time metadata assertions followed
that native image; final graphical artifacts use the assertions.

No live application clipboard endpoint, app peer-registration proof, mode widget,
Pi qualification or Main landing is claimed. Session notification wiring and
controller nomination remain. Review is single-agent; the operator's October
1-2 waiver excludes 50-boot, SMP, ASan and UBSan gates.

Final graphical image `graphics-1790869744784923000` passes session media
(75.74s) and physical F10 SAK (93.40s), both exit zero. Real sealed terminal
registrations succeed; View, PNG/JPEG Gallery, manual history/theme, Gallery
SAK restore, confer/real authority/abdicate/wrong-key/cancel remain working.
The 1280x800 manual-history workspace and trusted-prompt screenshots were
visually inspected; no new visible UI or fresh Pi/minimum-display qualification
is claimed. Final source pins and four protected draft hashes match.
