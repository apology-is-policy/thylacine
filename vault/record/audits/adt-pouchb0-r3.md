---
id: adt-pouchb0-r3
type: adt
title: "Pouch 0033-0040 round 3: the restructured tmpfile survived; the new ppoll leg was decided by a scheduling race over a kernel poll that sampled the wrong end"
date: 2026-09-21
scope: [sub-pouch-seam, sub-pouch-net, sub-pouch-fs, sub-pouch-thread, sub-kernel-devsrv, sub-kernel-srvconn, sub-kernel-poll]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 8}
findings: [fnd-pouchb0-r3-f1, fnd-pouchb0-r3-f2]
round-of: chg-2026-09-21-pouch-b0-libc
prior-round: adt-pouchb0-r2
created: 2026-09-21
---
## Scope

Branch `browser-b0` @ 1de0692a: the round-2 fix commit (0036 restructured, 0039, 0040, the four provers). Read-only. The reviewer built the whole patched musl in scratch (1,341 TUs), linked the provers, and modelled 0036's locking as an explicit-state machine (8 configurations, up to 22,021 states: no UAF, double free, list corruption or deadlock; two positive controls discriminate).

## Convergence

Not dirty by count; recorded `dirty` because the fix for its two real findings restructured a wait/wake mechanism. The leg added in round 2 to pin the tag-aware `ppoll` passed or failed on which thread ran first ([[fnd-pouchb0-r3-f1]]), and underneath it the kernel's `devsrv_poll` sampled the SERVER's end of a connection for a client ([[fnd-pouchb0-r3-f2]]). The combined gate had already said so before the report was read: 57 of 58 failed fleet boots and 17 of 17 failed SMP boots carried one line, `client: ppoll(socket) = 1 revents=0x18`. The P3s were a 32-bit truncation in the `fd_set` guard, a diagnostic written to an fd 2 that is routinely not stderr, a live `posix_spawn` coupling missing from 0036's list, a descriptor with two closers, four false sentences in 0040 (one of them deleting the honest concurrent-appender caveat), a mis-derived P-4 paragraph, `T_OAPPEND` defined natively with zero users, and prover notes. All fixed or tracked; a fourth round follows on the kernel poll surface.
