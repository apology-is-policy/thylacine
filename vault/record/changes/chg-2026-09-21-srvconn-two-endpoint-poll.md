---
id: chg-2026-09-21-srvconn-two-endpoint-poll
type: chg
title: "poll: a wake is a hint (the re-arm), and a /srv connection is pollable from BOTH endpoints with a list walk on every ring mutation"
date: 2026-09-21
arc: arc-boosty
commits: ["b8b27f1d", "1f14b6c5"]
touched: [sub-kernel-poll, sub-kernel-srvconn, sub-kernel-devsrv, spec-poll, sub-pouch-net]
established: []
closed: [fnd-pouchb0-r3-f1, fnd-pouchb0-r3-f2, fnd-b0self-r1-f3]
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-21
---
**What.** Three kernel defects behind one red gate. (1) `devsrv_poll` sampled
the SERVER's end of a byte-mode connection for a client, so a client was told
"readable" because of its own unread request. (2) The connection's hook list
was walked only on a client send and at teardown: a client was never woken by
its reply, and a nonblocking server polling for write room was never woken by
a client drain. (3) `sys_poll_for_proc` returned 0 whenever a wake turned out
to be for something the caller had not asked about: a timed poll "timed out"
in microseconds and `poll(-1)` could return 0. As built:
`srvconn_poll(cn, client, events, pw)` with mirror-image rows, ONE hook list,
a walk after EVERY ring mutation (four edges + teardown), a kernel-attached
client endpoint answering POLLNVAL; and the RE-ARM in `sys_poll_for_proc` --
clear the flags, THEN re-sample, sleep again against the same absolute
deadline on an empty re-sample, with the loop's own deadline test because
`tsleep` prefers a set flag to a passed deadline. The libc half is pouch 0041:
`poll()` gives a connected AF_UNIX socket the stream-socket shape at EOF.

**Why.** The ci fleet went 15 / 77 red and the SMP gate 17 / 40, 74 of those
75 boots on one line, `client: ppoll(socket) = 1 revents=0x18` -- the author's
own prover leg, decided by which thread ran first over defect (1). It had been
green because `tools/test.sh` is one HVF boot.

**Alternatives rejected.** The auditor's minimum -- POLLNVAL for a client
endpoint -- is indistinguishable from the libc defect pouch 0039 fixes, so the
prover could no longer tell the two apart, and the browser's IPC would have
had nothing to poll. Walking the list only for the edges each endpoint asked
about needs per-endpoint lists and buys nothing once a flag is a hint.

**Verification.** `specs/poll.tla` was extended FIRST (it modelled readiness
as a one-way edge and a flag as a verdict, so it could not see defect 3):
`Retract`, `OtherEvent`, `seen`, clear-then-sample, `NoSpuriousZero`,
`StableReadyReturns`, two new buggy cfgs; `specs/check-poll.sh` pins the clean
state counts and the NAMED invariant each buggy cfg violates. Five kernel
tests; sabotaged kernels fail four of them (`poll`) and the fifth
(`deadline`) at their own assertions -- the fifth only after it was rebuilt,
since its first form passed with the defect present ([[fnd-b0self-r1-f3]]).
Rounds: [[adt-pouchb0-r3]], then a fourth on this surface. Not covered,
recorded: an ACCEPTED pouch socket is an untagged kernel handle and keeps the
pipe-like shape.
