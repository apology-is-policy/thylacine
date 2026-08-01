---
id: view-closed-sub-pouch-net
type: view
title: "Do-not-re-report preamble — sub-pouch-net"
query: closed:sub-pouch-net
---
# Do-not-re-report preamble — sub-pouch-net

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-pouch-net`). Paste or transclude
into a prosecutor prompt as the closed-findings preamble.

Read it WITH two standing facts:

- **Tag-awareness completeness is this surface's recurring obligation.**
  Every fd-consuming call must dispatch on the tag bit, and the set has
  been found incomplete twice — `poll` (0015) and the three data calls
  ([[fnd-net5-r1-f1]]). Both were fail-CLOSED, which is the tag design
  working; a finding that some call is missing is a real finding, and one
  that a tagged fd reaches a kernel syscall would be a much bigger one.
- **The single-user-per-socket envelope is inherited, not accidental**
  ([[seam-pouch-sock-single-user]]): the slot lock guards the table's
  structure, not the socket's state machine, and every patch since 0006
  writes slot fields through a resolved pointer on that assumption.

<!-- generated:begin -->
5 closed findings on [[sub-pouch-net]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-net5-r1-f1]] [P2] shutdown / sendto / recvfrom were not tag-aware — ENOSYS on an AF_INET socket (fixed)
- [[fnd-sockets12-r1-f1]] [P1] A server-side read on a byte-mode SrvConn returned EOF racing the client's first write (fixed)
- [[fnd-sockets12-r1-f11]] [P3] bind/connect accepted an unterminated sun_path, passing caller stack as a service name (fixed)
- [[fnd-sockets12-r1-f2]] [P1] A tombstone-then-rebind with a mode change could land a wrong-mode SrvConn in the new poster's backlog (fixed)
- [[fnd-sockets12-r1-f8]] [P3] pouch_sock_kernel_fd read in_use again outside the lock, mis-categorizing errno (fixed)
<!-- generated:end -->
