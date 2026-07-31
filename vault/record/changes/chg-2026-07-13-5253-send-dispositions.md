---
id: chg-2026-07-13-5253-send-dispositions
type: chg
title: "#52/#53: never-sent tag reclaim + flush-EAGAIN rollback"
date: 2026-07-13
arc: arc-go-build
commits: ["e1307453", "8b4ba7eb", "2b6ef74e"]
touched: [sub-kernel-ninep-client]
established: []
closed:
  - fnd-375-r1-f1
  - fnd-375-r1-f2
  - fnd-5253-r1-f1
  - fnd-5253-r1-f2
  - fnd-5253-r2-f1
  - fnd-5253-r2-f2
opened: [seam-56-netd-cancelled-tag]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
The send-path error dispositions: #52 -- never-sent exits (self-dying /
dead-observed / spill-OOM; zero bytes on the wire by the all-or-nothing
contract) return CLIENT_SEND_NEVER and `p9_session_abort_unsent` reclaims
the tag immediately (I-10-safe: the server never saw it); pre-fix each such
abort leaked one of 64 slots on a LIVE shared session. #53 -- the DIED-path
Tflush + abandon_async treat EAGAIN as back-pressure: `p9_session_flush_
rollback` restores the pre-#845 ownerless reclaim instead of latching the
shared session. The R1 close added the `abandoned` bit (the rollback's
cleared awaiting_flush had made the victim look LIVE and refuse the #294
cancel-then-close Tclunk); R2 extended `p9_session_mark_abandoned` to the
flush-BUILD-failure sibling. Converged over two Fable rounds ([[adt-5253-r1]]
-> [[adt-5253-r2]]). Note: closes [[fnd-375-r1-f1]] + [[fnd-375-r1-f2]]
cross-chunk. The chunk's gate run also produced the mv-restore mtime lesson
(a stale object survived rebuilds -> 20/20 false FAIL). Prose: the commit
messages.
