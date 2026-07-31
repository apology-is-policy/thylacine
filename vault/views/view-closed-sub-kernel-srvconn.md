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
12 closed findings on [[sub-kernel-srvconn]] — do NOT re-report
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
- [[fnd-stalk3c-r1-f2]] [P3] Residual stale references to the retired /srv symbols across seven files' comments (fixed) — Fixed: all reworded to create=post / open=connect /
<!-- generated:end -->
