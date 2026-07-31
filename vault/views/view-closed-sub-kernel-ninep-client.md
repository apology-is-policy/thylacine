---
id: view-closed-sub-kernel-ninep-client
type: view
title: "Do-not-re-report preamble — the 9P client"
query: closed:sub-kernel-ninep-client
---
# Do-not-re-report preamble — the 9P client

Generated from `fnd-*` notes (`quaestor render`; also emitted
on-demand by `quaestor closed sub-kernel-ninep-client`). Paste or
transclude into a prosecutor prompt as the closed-findings preamble.
Replaces: `memory/audit_{841,845,349,375_spill,5253,8c3,90}_closed_list.md`
for this surface.

<!-- generated:begin -->
33 closed findings on [[sub-kernel-ninep-client]] — do NOT re-report
these in a future round (open/deferred findings are NOT listed
here; see the seam inbox):

- [[fnd-349-r1-f1]] [P1] N senders parked on ONE single-waiter Rendez -- unprivileged panic (fixed) — Fixed: send_rendez -> the multi-waiter `send_waiters_list`
- [[fnd-349-r1-f2]] [P2] The regression covered only the self-pump branch, never the park (fixed) — Fixed: added send_backpressure_multi_waiter (two registered waiters + the
- [[fnd-349-r1-f3]] [P3] A self-pump recv error latches the SHARED session dead (documented) — Documented as sound: a genuine peer-gone/break IS a death for everyone
- [[fnd-349-r2-f1]] [P3] The multi-waiter test asserts the wake walk, not the live park (documented) — Documented honestly (test comment + the closed list); the full concurrent
- [[fnd-349-self-sa1]] [P1] mark_dead missed the parked sender's wake (strand on death) (fixed) — Fixed (self-found, pre-formal-round): mark_dead -- verified the SOLE
- [[fnd-375-r1-f1]] [P2] Never-sent ops leak outstanding[tag] on a LIVE shared session (fixed) — Tracked as task #52 at the round; fixed by the #52/#53 chunk:
- [[fnd-375-r1-f2]] [P2] DIED-path Tflush + abandon_async treat EAGAIN as a broken stream (fixed) — Tracked as task #53 at the round; fixed by the #52/#53 chunk:
- [[fnd-375-r1-f3]] [P3] The out_buf field contract still described the pre-#349 world (fixed) — Fixed: the contract now states out_buf is undefined across any lock drop
- [[fnd-375-r1-f4]] [P3] The reference's 'never re-read after drop' claim was one notch broad (fixed) — Fixed: the carve-out named. An over-broad soundness claim erodes exactly
- [[fnd-5253-r1-f1]] [P2] The rollback's cleared awaiting_flush refused the #294 clunk (fixed) — Fixed: the `abandoned` bit -- the #294 exclusion extended (owner gone, will
- [[fnd-5253-r1-f2]] [P3] Two pre-send validation exits leaked the just-marked tag (fixed) — Fixed: mirrored the fail-closed latch (defense-in-depth on paths whose
- [[fnd-5253-r2-f1]] [P3] The flush-BUILD-failure sibling skipped the abandon marking (fixed) — Fixed: p9_session_mark_abandoned (fail-soft) called from both flen<=0
- [[fnd-5253-r2-f2]] [P3] The four-caller comment had rotted to seven callers (fixed) — Fixed: refreshed to seven + the freed-by contract names the teardown case.
- [[fnd-841-r1-f1]] [P1] Reply-buffer UAF: dispatch results alias a freed rpc reply_buf (fixed) — Fixed: the `c->done_reply_buf` deferred-free slot -- freed at the next
- [[fnd-841-r1-f2]] [P2] A dying owner leaks its outstanding tag slot until a late reply (fixed) — Fixed cross-chunk by #845 (Tflush-on-abandon) -- NOT the per-tag
- [[fnd-841-r1-f3]] [P3] Send is all-or-nothing-fail, not the sketched block-on-room (documented) — Reconciled as documentation at the time (bounded + dormant at the v1.0
- [[fnd-841-r1-f4]] [P3] Byte-granular reader recv accepted (documented) — Accepted: bounded by msize, mirrors do_recv, cannot hang against the
- [[fnd-841-r1-f5]] [P3] Demux-level protocol violations also latch the session dead (documented) — Reconciled: correct fail-closed (a malformed shared stream is unrecoverable
- [[fnd-841-r2-f6]] [P1] Reader-role loss strands survivors on hand-off-target death (fixed) — Fixed: on the DIED return, `if (rpc->be_reader) { clear;
- [[fnd-841-r2-f7]] [P2] be_reader not cleared on election-race loss -> busy-spin (fixed) — Fixed: clear be_reader at the top of the sleep branch (re-woken by the
- [[fnd-841-r2-f8]] [P3] destroy freed done_reply_buf without c->lock (doc-contract drift) (fixed) — Fixed by taking the lock: the code now matches its stated contract rather
- [[fnd-845-r1-f1]] [P2] Duplicate Rflush on a reused flush tag frees the wrong reservation (documented) — Closed with justification (no client-side fix exists): this is the GENERIC
- [[fnd-845-r1-f2]] [P3] The reuse-race regression never exercised tag REUSE (fixed) — Fixed: extended to drive a fresh walk to completion on the reclaimed tag
- [[fnd-845-r1-f3]] [P3] A late Rwalk for an abandoned walk no longer fid_binds (divergence) (documented) — Documented as the (arguably more correct) behavior -- don't bind for a dead
- [[fnd-845-r1-f4]] [P3] The early awaiting_flush consume wrote a misleading partial out (fixed) — Fixed: dropped the write; out stays zeroed; the 0 return documented as
- [[fnd-8c3-r1-f1]] [P1] Mid-frame stop-unwind desyncs the shared 9P byte stream (fixed) — Fixed (the stop half): the frame-atomic recv -- stop_no_park held for the
- [[fnd-8c3-r1-f2]] [P2] The role-release covered one of FOUR reader_active sites (fixed) — Fixed: the frame-atomic wrapper centralizes the flags so all four sites get
- [[fnd-8c3-r1-f3]] [P3] A real transport break concurrent with a stop is DEFERRED to resume (documented) — Documented safe for the trusted server: the only reachable break is a
- [[fnd-8c3-r2-f1]] [P1] The boundary stop classifier races an async proc_debug_resume (fixed) — Fixed: the stable per-Thread stop_unwound latch -- set by the detour's
- [[fnd-8c3-r2-f2]] [P3] Comments claimed t->proc == NULL for kproc threads (wrong immunity) (fixed) — Fixed (three comments). A guard justified by the wrong mechanism invites a
- [[fnd-8c3-r3-f1]] [P3] The fix's own doc-rot: comments described the removed re-read (fixed) — Fixed: both reworded to the stop_unwound latch; an optional symmetry clear
- [[fnd-90-r1-f1]] [P3] The revert-probe covered tsleep() only; production rides sleep() (fixed) — Fixed in-close: added the sleep()-path test (blocks a pending death through
- [[fnd-b1-r1-f3]] [P3] p9_client_init did not explicitly zero the loose/cacheable/wga latches (fixed) — Fixed: the three flags explicitly zeroed in `p9_client_init` — init
<!-- generated:end -->
