---
id: chg-2026-06-18-net6a2-datacalls
type: chg
title: "net-6a-2: shutdown / sendto / recvfrom become tag-aware"
date: 2026-06-18
arc: arc-net
commits: ["9874ce0e"]
touched:
  - sub-pouch-net
established: []
closed: ["fnd-net5-r1-f1"]
opened: ["seam-pouch-sendmsg"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The completion net-5 deferred, landing now that netd's data read
BLOCKS. `shutdown(SHUT_WR)` -> the `hangup` ctl verb (smoltcp's
`tcp::close` is a send-side FIN, so the socket keeps receiving until the
peer FINs -- exactly SHUT_WR); `sendto` with a dest re-points a UDP
datagram per call; `recvfrom` fills `src` from the recorded remote.
`sendmsg`/`recvmsg`/`socketpair` deliberately stay `ENOSYS`.
