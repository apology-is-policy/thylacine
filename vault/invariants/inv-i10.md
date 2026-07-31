---
id: inv-i10
type: inv
title: "I-10 — per-9P-session tag uniqueness"
number: I-10
guards: [sub-kernel-ninep-client]
validated-by: [spec-9p-client]
strength: spec
created: 2026-07-31
updated: 2026-07-31
---
## Statement

Within one 9P session, a tag uniquely names one outstanding request (the tag
IS the outstanding-table index) and is never reused until that request
retires. Retirement is: the reply arrives; or — for an op abandoned by a
dying owner — its `Rflush` arrives (#845: the abandoned tag stays reserved
`awaiting_flush`, so a late original reply can never be mis-attributed to a
reused tag); or — for an op whose frame never reached the wire — immediately
(#52: the transport's all-or-nothing contract means zero bytes were pushed,
so the server never saw the tag and `p9_session_abort_unsent` reclaims it);
or — for an op whose owner is gone with no flush in flight — its late
original reply, drained ownerlessly (the `abandoned` bit, #53).

## Enforcement

`p9_session` tag allocation (`alloc_tag` skips active tags) +
`clear_outstanding` · the `awaiting_flush` reservation + the `Rflush`-only
free (`p9_session_send_flush` / the ownerless demux TFLUSH arm) · the
`abandoned` bit (`p9_session_mark_abandoned`, `p9_session_flush_rollback`) ·
`p9_session_abort_unsent` gated on the never-sent classification
(`CLIENT_SEND_NEVER` in `client_send_flow`).

## Validation

[[spec-9p-client]]: `TagAndOpAccounting` — clean cfg green; the
`tag_collision` and `async_clunk_tag_leak` buggy cfgs are the executable
counterexamples. **blind-to:** a non-conformant SERVER duplicating replies
(9P carries no per-tag wire generation — the one-reply-per-tag trust
envelope, [[seam-845-untrusted-server]]); the never-sent classification's
correctness rests on the per-transport all-or-nothing send contract, which
the spec does not model.
