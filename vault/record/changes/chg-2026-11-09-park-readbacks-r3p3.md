---
id: chg-2026-11-09-park-readbacks-r3p3
type: chg
title: "Vote 81 audit round 3 closed: the latch's readers witnessed, DEADLINE's timeline rule, P3s"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Follows [[chg-2026-11-09-park-readbacks-r3f1]]. Round 3's P3s. warp-prove's ABANDON now asks each reader of the released latch for the answer only the latch gives, after the vindication: the fence file must answer EOF at once (nothing read it, so a build without the latch there answers the vindicated count, also at once), and a submit and `verify` must each be refused with exactly E_IO (`write_rc` resends a stall's EAGAIN within the bound and reports an open refusal apart). DEADLINE takes DEEP's construction rule by the fence timeline: an attempt is unconstructed when any of its queue had retired by the time the readback's write returned, not only when a send met a stall. tapestryd's ctx ctl emits `released N` after the prefix a client parses. GPU-DESIGN 4.5.13 now says a refused compositor readback is a sample of `cost readback-wait` (it retired); `FenceTag.released`'s doc and warp-readback.exp's pass text name what is there. The Pi line gains three reader mutants (fence file, admission, verify).
