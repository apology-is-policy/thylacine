---
id: chg-2026-11-03-b3h-first-boots
type: chg
title: "B-3h's first boots: two wrong tests, a survivor, a budget"
date: 2026-11-03
arc: arc-boosty
depth: skeletal
commits: ["7df0cc4f4", "c0184d5dc", "5711f3c3e"]
touched:
  - sub-kernel-filelock
  - sub-pouch-fs
---
B-3h booted for the first time on real Stratum, after six audit rounds. The kernel was
right in both reds, and both fixes are to tests.

`dev9p.cached_open_install_race` invented its peers' fids, and `p9_session_send_lock`
refuses a fid it never bound, so the take answered EIO before the wire. The leg now binds
real fids (`7df0cc4f4`).

`pouch-hello-locks` cached-ro expected a read from its snapshot after a child's take. A
child inherits the parent's open file, so the take moved the parent off the snapshot as
well, by design. The leg now marks its descriptor close-on-exec, and a new leg,
cached-inherited, pins the arm it had tripped on (`c0184d5dc`). The manual and
[[sub-kernel-filelock]] now state that behaviour.

Mutant G33, a late Rwalk's parse capped at `P9_MAX_WALK`, survived. The guard on the next
line of `honour_late_walk` already refuses any count but the walk's, so it is equivalent
on this tree. Writing its control found two holes in `9p_session.walk_reply_overcount`:
the late arm drove only Twalk, and every late case asserted that nothing binds. The leg
now drives both walks and a full reply that must bind (`5711f3c3e`), and G33b, G106, G107
and G108 go red at it.
