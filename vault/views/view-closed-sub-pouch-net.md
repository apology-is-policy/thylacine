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
8 closed findings on [[sub-pouch-net]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-net5-r1-f1]] [P2] shutdown / sendto / recvfrom were not tag-aware — ENOSYS on an AF_INET socket (fixed)
- [[fnd-pouchb0-r1-f4]] [P2] the stdio backends hand a pouch SOCKET fd to the kernel raw: fdopen(sock) never worked and fclose() strands the slot (fixed) — Fixed in 0038: read / write through `pouch_sock_kernel_fd`, close through `pouch_sock_close`, `ESPIPE` for a seek. The census that followed claimed every remaining tag-unaware call fails visibly; round 2 showed that false for `FD_SET` and `ppoll` ([[fnd-pouchb0-r2-f2]]).
- [[fnd-pouchb0-r2-f2]] [P2] a pouch socket fd cannot live in an fd_set: FD_SET(sock, &s) is a store 128 MiB past the set, and ppoll() hands the tag to the kernel raw -- while the census says every remaining call fails visibly (fixed) — Fixed as the honest minimum in 0039: `FD_SET` / `FD_CLR` / `FD_ISSET` `abort()` with a message outside `[0, FD_SETSIZE)` (glibc's `__fdelt_chk`), in a comma form that keeps upstream's value and raises no unused-value warning; `ppoll()` goes through the tag-aware `poll()`. The patch header says it is not the fix. Small-integer socket fds -- a placeholder kernel handle per slot and an fd -> slot side table, retiring the tag from every fd-consuming call -- are a redesign of 0006 / 0016 with their own audit, OWED and recorded in [[sub-pouch-net]].
- [[fnd-pouchb0-r3-f1]] [P1] the new ppoll prover leg is decided by a scheduling race, and its usual green is the kernel calling the client's OWN unread request 'readable' (fixed) — Fixed, and the gate measured it before the report did: under TCG the server consumes first about 98 % of the time, under HVF usually not. The leg is sequenced by barriers, requires exactly `POLLIN`, and has a second leg for the EOF shape; both sides `_exit(1)` on failure so a bailed thread cannot strand the other at a barrier. The false sentences were rewritten to say what was claimed, what was true, and what is true now. The underlying kernel defect is [[fnd-pouchb0-r3-f2]].
- [[fnd-sockets12-r1-f1]] [P1] A server-side read on a byte-mode SrvConn returned EOF racing the client's first write (fixed)
- [[fnd-sockets12-r1-f11]] [P3] bind/connect accepted an unterminated sun_path, passing caller stack as a service name (fixed)
- [[fnd-sockets12-r1-f2]] [P1] A tombstone-then-rebind with a mode change could land a wrong-mode SrvConn in the new poster's backlog (fixed)
- [[fnd-sockets12-r1-f8]] [P3] pouch_sock_kernel_fd read in_use again outside the lock, mis-categorizing errno (fixed)
<!-- generated:end -->
