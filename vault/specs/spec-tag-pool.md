---
id: spec-tag-pool
type: spec
title: "tag_pool.tla"
models: [sub-kernel-ninep-client, sub-kernel-ninep-session]
pins: []
cfgs:
  - "tag_pool.cfg -- clean: INVARIANTS TypeOK + TagsFit + FlushAlwaysFits; PROPERTY SyncProgress -- a sync op waiting for a tag gets one, with async ops deferred forever and a stopped waiter stopped forever; Limit = 2 * OpsMax, the tight case"
  - "tag_pool_buggy_no_async_cap.cfg -- buggy, temporal: SyncProgress violated (deferred async ops take the whole op share)"
  - "tag_pool_buggy_waiter_applies.cfg -- buggy, temporal: SyncProgress violated (a stored reply keeps its tag until the waiter runs; the waiter is stopped)"
  - "tag_pool_buggy_no_flush_headroom.cfg -- buggy: FlushAlwaysFits violated (ops take every tag, so an abandon finds none for its Tflush)"
gate: "Re-run specs/check-tag-pool.sh for any change to tag admission (the op share P9_OPS_MAX, the async share P9_ASYNC_MAX, a Tflush's tag), to where a sync reply is applied, or to what may hold a tag."
created: 2026-10-07
updated: 2026-10-07
---
## Abstraction

One session, tags counted rather than named. Two sync threads each send one
op and wait for its reply; the server answers them fairly, a stop may hold a
thread forever (no fairness on `Resume`), and a death abandons the op with a
Tflush that holds a second tag until the Rflush. Two async issuers (Loom ring
ops, dev9p poll arms) hold a tag each, and the server may defer their replies
forever. Written 2026-10-07 for [[dec-2026-10-07-tag-pool]] (ARCH 21.11)
before the code, because the design's central claim -- the wait it adds ends
-- is a liveness claim.

Deliberately outside: tag identity and the fid lifecycle ([[spec-9p-client]],
I-10, I-11); a sync op the server defers by design (it is a thread that
waits, and holds one tag); the flush(5) wait, whose tags are a death's
abandon's (`Die` models both); the reader role and the transport (the waiter
is taken to be woken on every freed tag; `Take` is strongly fair).

## Action-site map

| Spec action | Impl |
|---|---|
| `Take(s)` | a sync op's tag: `p9_session_send_*`'s `alloc_tag`, behind the wait for a free op tag (`client_drain_until_free_tag`) |
| `ReplySync(s)` | `demux_frame_locked` dispatching a sync reply into the op's result (`BUGGY_WAITER_APPLIES`: the reply stored for `client_run` to dispatch) |
| `Die(s)` / `Rflush(s)` | the #845 abandon's Tflush (`client_run`'s `CLIENT_WAIT_DIED` arm) and the ownerless Rflush in the demux; the flush-less abandon is `p9_session_mark_abandoned` |
| `Submit(a)` | `p9_client_submit_async` (Loom, the dev9p poll arm and snapshot), refused with `-P9_E_AGAIN` past the async share |
| `OpRoom` | the op share, `P9_OPS_MAX` (`BUGGY_NO_FLUSH_HEADROOM`: any free tag) |

Checker: `specs/check-tag-pool.sh` (each cfg's verdict by name, the counts
pinned). TLC 2026-10-07: clean 268 distinct states; `BUGGY_NO_ASYNC_CAP` and
`BUGGY_WAITER_APPLIES` fail `SyncProgress` (304, 360); `BUGGY_NO_FLUSH_HEADROOM`
fails `FlushAlwaysFits` (127 at the halt).
