---
id: view-closed-sub-kernel-srvconn
type: view
title: "Do-not-re-report preamble — sub-kernel-srvconn"
query: closed:sub-kernel-srvconn
---
# Do-not-re-report preamble — sub-kernel-srvconn

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-srvconn`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.

<!-- generated:begin -->
14 closed findings on [[sub-kernel-srvconn]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-348-r1-f1]] [P2] The writing guard refuses a 2nd concurrent blocking writer with −1 — which a POSIX write_full treats as fatal (fixed) — Deferred at the round as the documented v1.0 SINGLE-WRITER PRECONDITION
- [[fnd-348-r1-f2]] [P3] A partial return after TSLEEP_INTR is safe by construction (documented) — Documented (in the decl comment): no — the dying Proc unwinds at its
- [[fnd-348-r1-f3]] [P3] chan_cond_writable's || eof means STOP-BLOCKING, never ROOM-AVAILABLE (documented) — Documented (clarifying comment at the predicate): no — a producer woken
- [[fnd-348-r1-f4]] [P3] A future non-zero server-send deadline needs a caller-visible server_timed_out signal (documented) — Documented as a forward caveat at the dead branch: any server deadline
- [[fnd-cf3b-r1-f1]] [P1] The blocking client send deferred its POLLIN edge to end-of-delivery — a circular wait against a poll-then-read server (fixed) — Fixed in-commit: `poll_waiter_list_wake(&cn->poll_list)` fires on EVERY
- [[fnd-cf3b-r1-f2]] [P3] The role-wait conds' || eof term made a teardown-woken contender busy-spin against the unwinding holder (fixed) — Fixed in-commit: the role conds wait purely on role-free; liveness rests
- [[fnd-cf3b-self-freeb]] [P1] The all-or-nothing send's free-space bound still read the compile-time ring cap — the first bulk frame never fit (fixed) — Fixed before landing — found by GROUND TRUTH (the boot hang plus a
- [[fnd-p5srv-r1-f1]] [P1] Production /srv ops never armed client_deadline_ns — a hung server wedged its caller indefinitely (fixed) — Fixed in the audit-close commit: `srvconn_set_client_deadline` armed
- [[fnd-p5srv-r1-f10]] [P3] client_deadline_ns defaults to 0 — unsafe-by-default for any future blocking caller (documented) — Documented: the default-0 is retained deliberately as the
- [[fnd-p5srv-r1-f12]] [P3] client_fid uninitialized at create — soundness rested on the handshake_done gate alone (documented) — Documented at the time; RETIRED WHOLESALE at
- [[fnd-p5srv-r1-f8]] [P3] A burst of hung handshakes can transiently exhaust SRV_MAX_CONNS (documented) — Documented, no code: with the F1 deadline fix even a hung handshake
- [[fnd-pouchb0-r3-f2]] [P2] client-side poll on a byte-mode /srv connection is neither implemented nor fail-closed: it samples the server's end, and the reply wakes nobody (fixed) — Fixed in full, not fail-closed: POLLNVAL for a client endpoint is indistinguishable from the libc defect 0039 fixes, so the prover could no longer discriminate, and the browser's IPC would still have had nothing to poll. `srvconn_poll(cn, client, events, pw)` gives the two endpoints mirror-image rows, and reading `srvconn.c` end to end found the wake set incomplete in BOTH directions -- a nonblocking server polling for write room was never woken by a client drain either -- so every ring mutation now walks the one hook list. That is sound only with a second fix nobody had asked for: `sys_poll_for_proc` returned 0 when a post-wake re-sample found nothing, so a timed poll "timed out" in microseconds and `poll(-1)` could return 0. `specs/poll.tla` modelled readiness as a one-way edge and could not see it; the spec was extended first. The libc half is pouch 0041 (the stream-socket shape at EOF). Not covered and recorded: an ACCEPTED socket, untagged by 0006's design.
- [[fnd-rw4-rev2-f1]] [P1] RW-4 R2-F1: byte-mode /srv blocking recv extincts on a 2nd concurrent reader (fixed) — Fixed at `ee30f559`: a per-`srvconn_chan` `bool reading` single-reader
- [[fnd-stalk3c-r1-f2]] [P3] Residual stale references to the retired /srv symbols across seven files' comments (fixed) — Fixed: all reworded to create=post / open=connect /
<!-- generated:end -->
