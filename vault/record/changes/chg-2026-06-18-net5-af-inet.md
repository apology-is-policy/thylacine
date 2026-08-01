---
id: chg-2026-06-18-net5-af-inet
type: chg
title: "net-5: AF_INET sockets over netd's /net"
date: 2026-06-18
arc: arc-net
commits: ["a3b11a60"]
touched:
  - sub-pouch-net
established: []
closed: []
opened: ["seam-pouch-errno-channel"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The Genode `socket_fs`-in-libc model: `socket()` opens
`/net/<proto>/clone`, ctl verbs drive connect/announce, data rides the
`data` file, names come from `local`/`remote`. Stacks on 0006 by adding a
`family` field rather than parallel files -- `FAM_UNIX` is 0 so a zeroed
slot defaults to the older path. No new kernel surface: `SYS_open` /
`read` / `write` / `close` only.

[[adt-net5-r1]] closed with one P2, and its disposition is the one worth
keeping: the missing `shutdown`/`sendto`/`recvfrom` were phased to net-6
rather than built, because the read path was still non-blocking and
building shutdown over a broken read is a half-feature
([[fnd-net5-r1-f1]]).
