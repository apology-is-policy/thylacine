---
id: chg-2026-05-23-p6-sockets
type: chg
title: "P6 sub-chunk 12: AF_UNIX SOCK_STREAM over the byte-mode /srv"
date: 2026-05-23
arc: arc-phase6-pouch
commits: ["df74fe12"]
touched:
  - sub-pouch-net
established: []
closed: ["fnd-sockets12-r1-f1", "fnd-sockets12-r1-f2", "fnd-sockets12-r1-f8", "fnd-sockets12-r1-f11"]
opened: ["seam-pouch-sock-single-user", "seam-pouch-errno-channel"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The socket API with no socket syscalls: a userspace slot table, a tag
bit above `PROC_HANDLE_MAX`, and `read`/`write`/`close`/`getsockopt`
taught to dispatch on it. Paired with the kernel's new byte-mode
SrvConn.

[[adt-sockets12-r1]] found both P1s on the KERNEL half it dragged in --
the non-blocking server read that returned a spurious EOF when the
accept-wake raced the client's first write ([[fnd-sockets12-r1-f1]]), and
the tombstone-rebind that could land a wrong-mode conn in a poster's
backlog ([[fnd-sockets12-r1-f2]]). The userspace patch's own findings
were the locked-read discipline ([[fnd-sockets12-r1-f8]]) and the
unterminated `sun_path` ([[fnd-sockets12-r1-f11]]).

Later amended twice in place: stalk-3c retired the by-syscall post and
connect for create=post / open=connect, and A-5b added the one-component
walk so a pouch client can reach a 9P-mode service's sub-fid.
