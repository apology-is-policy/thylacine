---
id: sub-kernel-ninep-session
type: sub
title: "9P session state machine (9p_session)"
parent: moc-kernel-ninep
code: [kernel/9p_session.c, kernel/include/thylacine/9p_session.h]
audit: hard
guarded-by: [inv-i10, inv-i11]
validated-by: [spec-9p-client, spec-tag-pool, gate-smp]
locks: []
hazards: [haz-shared-stream-desync]
abis: []
design: []
created: 2026-07-31
updated: 2026-10-07
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
  `p9_session_dispatch_flushed_rmsg` (2026-09-30) is its flush(5) twin for an
  `awaiting_flush` tag whose owner still waits: the same parse, mutation and
  result, but the tag is not freed (`-1` on a tag with no flush); it clears
  `owner_waits`, because the op has now acted.
- Repair surface (#845/#52/#53): `p9_session_send_flush(oldtag)`,
  `p9_session_abort_unsent(tag)`, `p9_session_retract_unsent(tag)`,
  `p9_session_flush_rollback(oldtag)`, `p9_session_flush_retract(oldtag)`,
  `p9_session_flush_owner_waits(oldtag, waits)`,
  `p9_session_mark_abandoned(tag)`.
- Queries: `is_open`, `fid_bound`, `inflight`, `has_free_tag` (an op would
  get a tag now: the client asks before every build, ARCH 21.11 part 3),
  `has_flush_tag`, `async_room`, `n_bound_fids`, `n_reserved_slots`.
  `has_free_tag` and `has_flush_tag` may grow the table (the chunk stays), so
  they take a non-const session.
- The tag table, for the client (2026-10-07): `p9_session_entry(tag)` (NULL
  past the table), `p9_session_next_active(&tag)` (the active entry at the
  lowest tag at or above it, idle chunks skipped), `p9_session_owner` /
  `p9_session_set_owner` (the client's rpc registered on an active tag; an
  inactive tag takes none and the call returns false, so an owner never
  outlives its tag), `p9_session_next_sync_owned(&tag)` (as `next_active`, over
  the entries a sync waiter owns), and `p9_session_mark_async(tag)` (counts the
  op against the async share).
- `retract_unsent` returns `0` when it took the op back and `-1` on a guard
  (inactive, flushed or abandoned tag) or a failed re-bind; the tag is freed
  either way once it passed the guards.
- `p9_dispatch_result.bound_new_fid`: the new fid a walk's reply bound, or
  `P9_NOFID`. An ownerless dispatch hands it to the client's orphan sink.

## Mechanism

**State machine**: INIT → (Rversion, NOTAG, out-of-band) → VERSIONED →
(Tattach/Rattach, binds `root_fid`) → OPEN → CLOSED. Tversion never enters
the tag table — it uses NOTAG (0xFFFF, never allocated) and the dispatcher
special-cases Rversion in state INIT, negotiating msize DOWN to
`min(server, proposed)`.

**Tag table (2026-10-07, ARCH 21.11, `dec-2026-10-07-tag-pool`)**: tag value
== table index, tags 0..0xFFFE (`P9_TAG_LIMIT`). Chunk 0 (`tags0`,
`P9_TAG_CHUNK` = 64 entries) lives in the session; `alloc_tag` takes the
lowest inactive entry and, when every entry is held, `grow` kmallocs the next
64-entry chunk (`KP_ZERO`; the 1024-pointer directory with the first one)
under the client's spinlock -- kmalloc never sleeps -- and keeps it until
`p9_session_destroy` frees it. A failed allocation is no free tag, never an
error. Each chunk counts its active entries, so `alloc_tag` skips full chunks
and `next_active` skips idle ones: a walk over a grown table costs what is in
flight. Each chunk also counts its sync-owned entries (`n_sync`: an owner on an
entry not counted as async), kept by `set_owner`, `mark_async` and
`clear_outstanding`; the client's reader handoff walks only those, so thousands
of async ops in flight do not lengthen it (audit r1 F1). The table never
shrinks: a session that once held 16384 deferred async ops keeps its 256 chunks
(about 1 MiB) until destroy, at most 1023 chunks and the directory (about
4 MiB), charged to no Proc -- bounded, so I-32 holds.

The shares (part 2): an op (any T but Tflush) is admitted only while
`n_active - n_flush < ops_max` (`P9_OPS_MAX` = 32767); a Tflush takes any
free entry. A victim has at most one Tflush and keeps its tag until the
Rflush, so flushes never outnumber ops and `2 * P9_OPS_MAX <= P9_TAG_LIMIT`
(a `_Static_assert`) leaves a Tflush a tag always, short of a failed chunk
allocation. Async ops (`mark_async`, `n_async`) are admitted by the client
only while `n_async < async_max` (`P9_ASYNC_MAX` = 16384). `ops_max`,
`async_max` and `tag_limit` are session fields set from the constants at
init; tests lower them (a `tag_limit` at the share stands in for a failed
allocation). Back-pressure still surfaces as a send-side `-1`, never a silent
overwrite; above it the client waits for a tag ([[sub-kernel-ninep-client]]).

Each entry also carries the client's registration (`owner`, which replaced
the client's `inflight[]` array: an entry cleared drops its owner), the
`async` flag, and, on a flush victim, `flush_tag` -- the tag of its Tflush, so
`flush_unstage` finds the flush without a search.

**Fid table**: `bound_fids[P9_SESSION_MAX_FIDS]` (**1024** since the #198
fid-ceiling chain; 256 before), linear scan,
swap-with-last unbind. `SendClunk` **unbinds at send time** — the canonical
client discipline: no further op can target the fid even while the Rclunk is
in flight, and an Rlerror on the clunk leaves it unbound (the client already
treated it as gone). `send_walk` pre-checks fid-table capacity (RW-4 R-B-F1)
so exhaustion fails closed *before* the round trip.

**Reserved slots (2026-09-29, FID-LIFECYCLE section 9).** Every outstanding
op that may leave a fid bound when it ends holds a slot of the table
(`p9_outstanding.holds_slot`, counted in `n_reserved_slots`): a walk naming a
new fid reserves the slot that fid will bind, and a Tclunk keeps the slot of
the fid its build unbound until its reply or a take-back. A new reservation
needs `n_bound_fids + n_reserved_slots` below `P9_SESSION_MAX_FIDS`
(`slot_available`); a walk's bind or a take-back only turns a reserved slot
into a bound one (`slot_bind`: release, then `fid_bind`). So bound plus
reserved never exceeds the table, and neither a walk's Rwalk nor a take-back
can find it full, even when the caller dropped `c->lock` to park after the
build. This retired the walk's old dispatch-time capacity race (a peer
bound the last slot while the walk waited, and the walk failed with EIO
although the server had bound its fid). `clear_outstanding` releases a slot
still held. The dispatch-side bind failure that remains is only a duplicate
bind, which completes the op as a **synthetic Rlerror EIO** rather than
returning `-1`, because the client latches the whole shared session dead on
a dispatch `-1` (the R3-F1 lesson).

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
(#52/#53 R2-F2). It EXCLUDES `abandoned` entries and `awaiting_flush` entries
whose owner is gone: a cancelled op will never act on its fid, so it must not
block a fid op — this is what makes Tflush-then-immediately-Tclunk (the #294
cancel-at-close) legal before the Rflush arrives. A flushed op whose owner
still waits (`owner_waits`, flush(5), 2026-09-30 round 3 F4) stays LIVE: a
reply that beats its Rflush is applied in full, so it may yet act on its fid.
The client sets the bit when it stages that Tflush and clears it when the
owner dies in the flush wait; `dispatch_flushed_rmsg` clears it once the op
has acted, and `flush_unstage` and `clear_outstanding` reset it. The Tflush
entries themselves are skipped too: a flush acts on no fid, and its entry
carries `root_fid` only as a placeholder, which refused a setattr of a
raw attach root fd while any flush was in flight (flush(5) round 4 F4,
pre-existing since #845; flush(5) made flushes routine).

**The retirement rules (I-10 mechanized).** A tag frees by exactly one of:

1. Its reply arrives → `clear_outstanding` in the dispatch tail.
2. It was abandoned with a Tflush in flight (#845): `send_flush` sets
   `victim->awaiting_flush` and records `flush_oldtag` on the flush's own
   tag. A late original reply on an `awaiting_flush` tag is
   **absorbed-without-completing** (dispatch returns 0, no clear) — the
   **Rflush is the sole authority** that frees the victim (the TFLUSH
   dispatch arm). Freeing on the late reply would allow reuse while a stray
   twin reply is still possible — the exact I-10 mis-attribution the naive
   fix introduces. flush(5) says a reply that arrives before the Rflush is
   honoured as though the request had not been flushed, and the only fid
   state a reply creates is a walk's new fid: `honour_late_walk` binds it
   into the slot the walk reserved (a TWALK on any Rwalk, a TWALKGETATTR on
   a full walk only) and reports it in `bound_new_fid`. `holds_slot` doubles
   as "not yet honoured", so a duplicate late reply binds nothing; an
   Rlerror binds nothing, and the Rflush then releases the reservation.
   When the owner still waits (a caught note, 2026-09-30), the client uses
   `dispatch_flushed_rmsg` instead: `apply_rmsg` -- the shared body of the
   type check, per-kind parse, mutation and result -- runs in full, and the
   tag stays reserved all the same. The walk arms' `slot_bind` releases the
   slot, so a duplicate still binds nothing.
3. It was never sent (#52): `abort_unsent` clears it immediately — sound
   only because the transport send contract is all-or-nothing (zero bytes
   pushed ⇒ the server never saw the tag ⇒ no late reply can exist).
   Fail-soft guards: inactive / `awaiting_flush` / `abandoned` tags are left
   alone.
   `retract_unsent` (NP-4b, the async submit's full-ring path) also re-binds
   the fid a never-sent Tclunk unbound at build, into the slot the Tclunk
   kept: the server still holds it, and 9p_client.tla has no step for a send
   that never happened. Since 2026-09-29 every never-sent Tclunk on a live
   session is taken back this way (the client then returns `-P9_E_AGAIN` and
   the fid goes to the closer, [[sub-kernel-ninep-attach]]); `abort_unsent`
   keeps a dead session's, whose fids died with it.
4. Its owner is gone with NO flush in flight (#53): `flush_rollback` (the
   flush frame itself hit EAGAIN — undo: free the never-sent flush tag,
   clear `awaiting_flush`, set `abandoned`) or `mark_abandoned` (the flush
   could not even be built — pool full / wrong state). An `abandoned` tag is
   freed by its late original reply, drained ownerlessly. The `abandoned`
   bit exists because a rolled-back victim without it counts LIVE in
   `any_outstanding_on_fid` and refuses the #294 cancel-then-close Tclunk —
   re-opening the netd slot leak on exactly the congestion path #53 targets
   (the #53-audit F1). Its owner STILL WAITS and the flush never left
   (2026-09-30): `flush_retract` frees the flush's tag and clears
   `awaiting_flush` WITHOUT setting `abandoned`, so the victim is an ordinary
   live op again, guards its fid, and is freed by its reply (case 1). Both
   undo through `flush_unstage`.

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
negotiated_msize, `bound_fids[1024]` + count, the tag table (`tags0`, the
inline `struct p9_tag_chunk`; `tag_dir`, `n_chunks`; the counters `n_active`,
`n_flush`, `n_async`; the limits `ops_max`, `async_max`, `tag_limit`),
monotonic `next_op_id`, sent/completed counters. `struct p9_tag_chunk`: 64
entries + `n_active` + `n_sync`. `struct p9_outstanding` (40 bytes): `active`, `kind`
(the T-opcode), `fid`, `new_fid`, `op_id`, `awaiting_flush`, `abandoned`,
`holds_slot` (a reserved fid-table slot), `flush_oldtag`, `wga_nwname` (the
walkgetattr full-walk comparand), `flush_tag` (a victim's Tflush), `async`,
`owner` (the client's registered rpc). `n_reserved_slots` counts
the held slots. Compile-time: MAX_OUTSTANDING ∈ [1, 0xFFFE] (room for NOTAG),
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
per-family preconditions (I-11). Bound plus reserved slots never exceed
`P9_SESSION_MAX_FIDS`, so a take-back restores exactly the binding the build
removed (I-11) and a walk's server-side bind is never lost to capacity. The dispatcher's type check (`expected_r ==
kind + 1`, Rlerror always admissible) plus tag-echo verification per parse
arm closes reply mis-pairing (the spec's `OutOfOrderCorrectness`).

## Error paths

Send: `-1` on state/magic/precondition/window-full/codec failure. Dispatch:
`-1` on malformed header, inactive tag, tag out of range, type mismatch,
parse failure — the CLIENT treats a dispatch `-1` as a protocol violation
and latches the session dead ([[haz-shared-stream-desync]]), which is why
the two LOCAL failure arms (a duplicate bind on walk/walkgetattr, the only
refusal a reserved slot leaves) deliberately complete with a synthetic
`T_E_IO` error instead.

## Performance

`alloc_tag`: O(chunks + 64) with full chunks skipped; `inflight` and the
share checks are O(1) counters; the reader handoff walks only chunks holding a
sync waiter. `any_outstanding_on_fid` (seven fid-exclusive builds), the client's
`mark_dead` and `/ctl` snapshot walk every active entry, idle chunks skipped:
O(in flight) under the client's lock, up to the op share on a session that holds
that many (OPEN-BUGS, audit r1 F1). O(n_bound) fid scan. The tag wait is not
FIFO: a waiter re-tests on every freed tag and a fresh op may take one first
(`tag_pool.tla`'s strong fairness on `Take` is an abstraction, r1 F7).
The table allocates only when every entry is held -- a 4 KiB page per chunk
and an 8 KiB directory once -- and frees at destroy.

## Prosecution

- **The retirement matrix**: any new path that clears an `awaiting_flush`
  tag outside the Rflush arm is an I-10 break (`dispatch_flushed_rmsg` applies
  a reply and must never free its tag); any path that widens
  `abort_unsent` beyond the zero-bytes-pushed set mis-reclaims a live tag
  (a misclassified partial push breaks the stream AND I-10).
- **`any_outstanding_on_fid` caller-list currency** (seven today) and its
  exclusions (an `abandoned` op, a flushed op whose owner is gone, a Tflush
  entry) — removing either of the first two re-opens the #294 clunk-refusal
  leak, and removing the third refuses root-fid ops during any flush; adding
  an exclusion without the will-never-act-on-the-fid argument breaks the
  live-op discipline.
- **Dispatch `-1` vs synthetic-error discipline**: a new dispatch arm that
  returns `-1` for a LOCAL condition kills the shared session for every
  mount that resolves through it (the R3-F1/R-B-F1 class).
- **Send-time unbind ordering** (unbind BEFORE `mark_outstanding`) and the
  walkgetattr full-walk-only bind.
- The `t != oldtag` argument in `send_flush` (alloc_tag skips the active
  victim, so `mark_outstanding(t)` cannot clobber the victim pointer; a growth
  adds a chunk and moves no entry, so the pointer also survives a grow).
- **The tag table's counters and shares** (ARCH 21.11): `n_active`, `n_flush`,
  `n_async` and each chunk's `n_active` change only in `mark_outstanding` /
  `clear_outstanding`, and `mark_async`; a path that sets or clears `active`
  any other way breaks the share arithmetic, and with it `FlushAlwaysFits`.
  Prosecute an op admitted without the op-share check (a new `alloc_tag(s,
  true)` caller that is not a Tflush voids the headroom), a victim with two
  Tflushes, an entry pointer held across a `kfree` (only destroy frees), a tag
  at or above `tag_limit` handed out, an `owner` set on an inactive entry, and
  an owner written other than through `set_owner` or `entry_zero` (it would
  skew `n_sync`; an undercount hides a sync waiter from the reader handoff).
- **Slot accounting**: every path that ends an op must release its
  reservation (`clear_outstanding` does, first) or turn it into a binding
  (`slot_bind`); only a NEW reservation may check `slot_available`. A path
  that reserves without the check, or binds without releasing, lets bound
  plus reserved exceed the table and a take-back fail.

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
- Tversion is unflushable (NOTAG, never in the tag table) — `send_flush`
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
their landing). The reservation and flush(5) (2026-09-29, each seen RED by a
sabotage): `9p_session.walk_fid_full_no_latch` (leg (b): a peer's walk cannot
take a walk's reserved slot, and the Rwalk binds into it),
`9p_session.clunk_retract_after_peer_fill`,
`9p_session.flushed_walk_late_reply_binds`. The living owner's flush(5)
(2026-09-30): `9p_session.flushed_reply_honoured_for_waiting_owner` (the whole
reply is applied, the tag stays reserved, a duplicate binds nothing, a
wrong-type reply is refused, an Rlerror is honoured) and
`9p_session.flush_retract_restores_live_op` (the retracted op still guards
its fid, and its reply frees it), `9p_session.flush_owner_waits_keeps_fid_live`
(a waiting owner's op guards its fid until it has acted; a dead owner's does
not) and `9p_session.flush_names_no_fid` (a Tflush in flight does not hold the
root fid; a live op on it does).
