---
id: fnd-pouchb0-r3-f2
type: fnd
title: "client-side poll on a byte-mode /srv connection is neither implemented nor fail-closed: it samples the server's end, and the reply wakes nobody"
round: adt-pouchb0-r3
severity: P2
status: fixed
surface: [sub-kernel-devsrv, sub-kernel-srvconn, sub-kernel-poll]
threatens: [inv-i9]
fixed-by: chg-2026-09-21-srvconn-two-endpoint-poll
regression: "poll.devsrv_client_row, poll.devsrv_client_wakes_on_reply_only, poll.devsrv_server_pollout_wakes_on_client_drain, poll.devsrv_client_kernel_attached_pollnval, poll.timeout_survives_a_busy_list; specs/poll.tla NoSpuriousZero + StableReadyReturns with poll_buggy_clear_after_sample.cfg and poll_buggy_return_on_wake.cfg"
created: 2026-09-21
---
## Prosecution

**File**: `kernel/devsrv.c` `devsrv_poll` (no `c->flag & CSRVCLIENT` branch, though `devsrv_read` / `devsrv_write` do branch); `srv_handle_poll`'s fail-closed guard sits on the dead `KObj_Srv` path no client holds since stalk-3b
**Invariant**: I-9
**Prosecution**:
1. A pouch client `poll(sock, POLLIN)` reports POLLIN while its own request is unconsumed -> the following `read()` BLOCKS.
2. The reply arrives with c2s empty -> no wake -> the poller is parked until timeout or teardown.
3. WebKit-style AF_UNIX poll loops (the browser arc) land exactly here; 0039 created the first in-tree consumer.
**Suggested fix**: a `CSRVCLIENT` arm plus walks from both s2c producers and the client-recv drain, as its own chunk on the poll / devsrv surface. Minimum before merge: fail closed with POLLNVAL.

## Disposition

Fixed in full, not fail-closed: POLLNVAL for a client endpoint is indistinguishable from the libc defect 0039 fixes, so the prover could no longer discriminate, and the browser's IPC would still have had nothing to poll. `srvconn_poll(cn, client, events, pw)` gives the two endpoints mirror-image rows, and reading `srvconn.c` end to end found the wake set incomplete in BOTH directions -- a nonblocking server polling for write room was never woken by a client drain either -- so every ring mutation now walks the one hook list. That is sound only with a second fix nobody had asked for: `sys_poll_for_proc` returned 0 when a post-wake re-sample found nothing, so a timed poll "timed out" in microseconds and `poll(-1)` could return 0. `specs/poll.tla` modelled readiness as a one-way edge and could not see it; the spec was extended first. The libc half is pouch 0041 (the stream-socket shape at EOF). Not covered and recorded: an ACCEPTED socket, untagged by 0006's design.
