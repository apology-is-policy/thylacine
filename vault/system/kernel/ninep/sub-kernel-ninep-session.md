---
id: sub-kernel-ninep-session
type: sub
title: "9P session state machine (9p_session)"
parent: moc-kernel-ninep
code: [kernel/9p_session.c, kernel/include/thylacine/9p_session.h]
audit: hard
guarded-by: [inv-i10, inv-i11]
validated-by: [spec-9p-client, gate-smp]
locks: []
hazards: [haz-shared-stream-desync]
abis: []
design: []
created: 2026-07-31
updated: 2026-08-14
---
## Purpose

The per-session tag pool + fid table + outstanding-request bookkeeping — the
code realization of `specs/9p_client.tla`'s state machine. It composes the
[[sub-kernel-ninep-wire]] codec (builders on send, parsers on dispatch) and
is itself composed by [[sub-kernel-ninep-client]], which owns the
concurrency (the session has **no locks**: every call runs under the
client's `c->lock`). The flush/abandon machinery here is where I-10's
retirement rules are mechanically enforced.

## Contract

- Lifecycle: `p9_session_init(s, root_fid, msize)` / `p9_session_close`
  (refuses while any op is in flight — the spec's `CloseSession`
  precondition) / `p9_session_destroy` (clobbers `P9_SESSION_MAGIC` first so
  use-after-destroy fast-fails).
- Send side: `p9_session_send_version/attach/walk/walkgetattr/clunk/flush`
  plus the IO (`lopen/lcreate/read/write`), metadata (`getattr/setattr/
  readdir/statfs/fsync`), mutation (`symlink/mknod/rename/readlink/link/
  mkdir/renameat/unlinkat`), and Weft (`weft/weftio`) families. Each
  validates preconditions, allocates a tag, builds the frame into the
  caller's buffer, and records the outstanding entry. Returns frame bytes or
  `-1`.
- Receive side: `p9_session_dispatch_rmsg(s, rmsg, len, out)` — tag-indexed
  pairing, per-kind parse + state mutation, results surfaced in
  `struct p9_dispatch_result` (zeroed on every call by the dispatcher; the
  caller must not read fields after a `-1`).
- Repair surface (#845/#52/#53): `p9_session_send_flush(oldtag)`,
  `p9_session_abort_unsent(tag)`, `p9_session_flush_rollback(oldtag)`,
  `p9_session_mark_abandoned(tag)`.
- Queries: `is_open`, `fid_bound`, `inflight`, `has_free_tag` (the
  async-clunk pool-full pre-check), `n_bound_fids`.

## Mechanism

**State machine**: INIT → (Rversion, NOTAG, out-of-band) → VERSIONED →
(Tattach/Rattach, binds `root_fid`) → OPEN → CLOSED. Tversion never enters
`outstanding[]` — it uses NOTAG (0xFFFF, outside the 0..63 index range) and
the dispatcher special-cases Rversion in state INIT, negotiating msize DOWN
to `min(server, proposed)`.

**Tag pool**: tag value == index into
`outstanding[P9_SESSION_MAX_OUTSTANDING]` (64). `alloc_tag` returns the
lowest inactive slot or `-1` — back-pressure surfaces as a send-side
refusal, never a silent overwrite. A full table is the flow-control trip the
client's #349 machinery sits above.

**Fid table**: `bound_fids[P9_SESSION_MAX_FIDS]` (**1024** since the #198
fid-ceiling chain; 256 before), linear scan,
swap-with-last unbind. `SendClunk` **unbinds at send time** — the canonical
client discipline: no further op can target the fid even while the Rclunk is
in flight, and an Rlerror on the clunk leaves it unbound (the client already
treated it as gone). `send_walk` pre-checks fid-table capacity (RW-4 R-B-F1)
so exhaustion fails closed *before* the round trip; the dispatch-side
`fid_bind` failure that remains (the TOCTOU residual — a peer bound the
last fid during this op's recv) completes the op as a **synthetic
Rlerror EIO** rather than returning `-1`, because the client latches the
whole shared session dead on a dispatch `-1` and a local fid exhaustion
must never kill every other Proc's mount (the R3-F1 lesson).

**Exhaustion here is silent at BOTH endpoints, which is what made it the
invisible layer of the #198 hunt.** `fid_alloc` refuses before any
T-message is built, so the client sees a generic failure and the server
never learns a request existed — three rounds of theorizing at either end
died on a refusal that sat between them. The ceiling was lifted 256 ->
1024 rather than made dynamic; the refusal path is unchanged, so the same
blindness returns at 1024. A future ceiling hunt should instrument here
first, not last.

**Per-op-family send preconditions** (the spec's "no other in-flight op on
the same fid" discipline, enforced via `any_outstanding_on_fid`):

| Family | Concurrency on one fid |
|---|---|
| lopen, lcreate, setattr, rename | EXCLUSIVE (server-side fid/identity mutation) |
| read, write, getattr, readdir, statfs, fsync, readlink, weft, weftio | CONCURRENT (offset/identity explicit on the wire) |
| symlink, mknod, mkdir, renameat, unlinkat, link | CONCURRENT on the dirfid (server serializes per-entry) |
| clunk | EXCLUSIVE + send-time unbind |
| walk / walkgetattr | destination fid must be unbound, un-targeted, non-root, ≠ NOFID |

`any_outstanding_on_fid` has **seven callers** (clunk, walk-new_fid, lopen,
lcreate, walkgetattr, setattr, rename) — the in-code comment demands the
list stay current because a stale list narrows future audit scoping
(#52/#53 R2-F2). It EXCLUDES `awaiting_flush` and `abandoned` entries: a
cancelled op will never act on its fid, so it must not block a fid op —
this is what makes Tflush-then-immediately-Tclunk (the #294 cancel-at-close)
legal before the Rflush arrives.

**The retirement rules (I-10 mechanized).** A tag frees by exactly one of:

1. Its reply arrives → `clear_outstanding` in the dispatch tail.
2. It was abandoned with a Tflush in flight (#845): `send_flush` sets
   `victim->awaiting_flush` and records `flush_oldtag` on the flush's own
   tag. A late original reply on an `awaiting_flush` tag is
   **absorbed-without-completing** (dispatch returns 0, `*out` stays zeroed,
   no fid mutation, no clear) — the **Rflush is the sole authority** that
   frees the victim (the TFLUSH dispatch arm). Freeing on the late reply
   would allow reuse while a stray twin reply is still possible — the exact
   I-10 mis-attribution the naive fix introduces.
3. It was never sent (#52): `abort_unsent` clears it immediately — sound
   only because the transport send contract is all-or-nothing (zero bytes
   pushed ⇒ the server never saw the tag ⇒ no late reply can exist).
   Fail-soft guards: inactive / `awaiting_flush` / `abandoned` tags are left
   alone.
4. Its owner is gone with NO flush in flight (#53): `flush_rollback` (the
   flush frame itself hit EAGAIN — undo: free the never-sent flush tag,
   clear `awaiting_flush`, set `abandoned`) or `mark_abandoned` (the flush
   could not even be built — pool full / wrong state). An `abandoned` tag is
   freed by its late original reply, drained ownerlessly. The `abandoned`
   bit exists because a rolled-back victim without it counts LIVE in
   `any_outstanding_on_fid` and refuses the #294 cancel-then-close Tclunk —
   re-opening the netd slot leak on exactly the congestion path #53 targets
   (the #53-audit F1).

**Rflush residual** (documented in the dispatch arm): a NON-conformant
server's duplicate Rflush after the flush tag was freed+reused is
indistinguishable on the wire (9P has no per-tag generation) — the generic
"one reply per tag" trust envelope, [[seam-845-untrusted-server]].

**Walkgetattr partial-walk nuance**: the TWALKGETATTR dispatch arm binds
`new_fid` ONLY on a full walk with a real destination (`nwqid ==
wga_nwname && new_fid != P9_NOFID`) — correct 9P2000.L partial-walk
semantics, required by the multi-name POUNCE. The plain TWALK arm still
binds unconditionally (its callers send 0/1 names, where partial cannot
exist — the deferred refinement is noted in place).

## Data structures

`struct p9_session`: magic (`0x50395345` "P9SE"), state, root_fid, msize +
negotiated_msize, `bound_fids[1024]` + count, `outstanding[64]`, monotonic
`next_op_id`, sent/completed counters. `struct p9_outstanding`: `active`,
`kind` (the T-opcode), `fid`, `new_fid`, `op_id`, `awaiting_flush`,
`abandoned`, `flush_oldtag`, `wga_nwname` (the walkgetattr full-walk
comparand). Compile-time: MAX_OUTSTANDING ∈ [1, 0xFFFE] (room for NOTAG),
MAX_FIDS ≥ 1.

## Concurrency

None internal — deliberately. The session is a pure state machine mutated
only under the client's `c->lock` ([[lock-9p-client-c-lock]]); its
dispatch runs from the elected reader's demux and from synchronous submit
failures, both lock-held. Any future caller outside the client must bring
its own serialization.

## Invariants enforced

![[inv-i10#Statement]]

![[inv-i11#Statement]]

Enforcement sites: `alloc_tag`/`clear_outstanding` + the four retirement
rules above (I-10); `fid_bind`/`fid_unbind` + send-time clunk-unbind + the
per-family preconditions (I-11). The dispatcher's type check (`expected_r ==
kind + 1`, Rlerror always admissible) plus tag-echo verification per parse
arm closes reply mis-pairing (the spec's `OutOfOrderCorrectness`).

## Error paths

Send: `-1` on state/magic/precondition/window-full/codec failure. Dispatch:
`-1` on malformed header, inactive tag, tag out of range, type mismatch,
parse failure — the CLIENT treats a dispatch `-1` as a protocol violation
and latches the session dead ([[haz-shared-stream-desync]]), which is why
the two LOCAL failure arms (fid-table exhaustion on walk/walkgetattr bind)
deliberately complete with a synthetic `T_E_IO` error instead.

## Performance

O(64) tag scan, O(n_bound) fid scan — both cache-tight linear arrays;
`p9_session_inflight`/`has_free_tag` are pure scans. No allocation.

## Prosecution

- **The retirement matrix**: any new path that clears an `awaiting_flush`
  tag outside the Rflush arm is an I-10 break; any path that widens
  `abort_unsent` beyond the zero-bytes-pushed set mis-reclaims a live tag
  (a misclassified partial push breaks the stream AND I-10).
- **`any_outstanding_on_fid` caller-list currency** (seven today) and its
  two exclusions — removing either exclusion re-opens the #294 clunk-refusal
  leak; adding an exclusion without the will-never-act-on-the-fid argument
  breaks the live-op discipline.
- **Dispatch `-1` vs synthetic-error discipline**: a new dispatch arm that
  returns `-1` for a LOCAL condition kills the shared session for every
  mount that resolves through it (the R3-F1/R-B-F1 class).
- **Send-time unbind ordering** (unbind BEFORE `mark_outstanding`) and the
  walkgetattr full-walk-only bind.
- The `t != oldtag` argument in `send_flush` (alloc_tag skips the active
  victim, so `mark_outstanding(t)` cannot clobber the victim pointer).

## Seams

- [[seam-845-untrusted-server]] — the one-reply-per-tag trust envelope
  (duplicate Rflush / duplicate replies from a non-conformant server; wire
  tag generations are the v1.x ABI lift).
- Partial-walk binding on the plain TWALK arm (bind-unconditional; safe for
  its 0/1-name callers, refined only if a multi-name TWALK caller appears —
  noted in the dispatch arm).

## Caveats

- `p9_dispatch_result` is a large zeroed-per-call struct; never read fields
  after a `-1` return.
- Tversion is unflushable (NOTAG, never in `outstanding[]`) — `send_flush`
  rejects it structurally; it is also valid in VERSIONED (so a hung Tattach
  IS flushable).
- The session knows nothing of msize payload clamps — those live in the
  client (CF-3 `client_max_read_count`/`client_max_write_payload`); the
  dispatcher's Rread/Rreaddir `data_cap` is derived from
  `negotiated_msize - 11`.

## Provenance

(generated from incoming `touched` edges — shaped by P5-session,
P5-wire-io/-meta/-mutation, #845 [[chg-2026-06-04-845-tflush]], #294
[[chg-2026-06-21-294-cancel-at-close]], #52/#53
[[chg-2026-07-13-5253-send-dispositions]], POUNCE P-2/P-3, RW-4 R-B-F1.)

## Tests

`kernel/test/test_9p_session.c` — ~51 registered `9p_session.*` cases: the
handshake, per-family round trips with synthesized Rmsgs, every send-side
refusal (unbound fid, bound destination, root violations, in-flight
conflicts, state gates), dispatch rejection (wrong tag / wrong type /
inactive), and the flush machinery regressions
(`9p_session.flush_reclaims_both`,
`9p_session.late_reply_does_not_free_awaiting_flush`,
`9p_session.abort_unsent_reclaims_tag`,
`9p_session.flush_rollback_restores_victim` — the last two revert-probed at
their landing).
