---
id: sub-kernel-ninep-dev9p-poll
type: sub
title: "dev9p.poll — the readiness bridge + the global poll-pump kthread"
parent: moc-kernel-ninep
code: [kernel/dev9p_poll.c]
audit: hard
guarded-by: [inv-i9]
validated-by: [spec-net-poll, spec-net-poll-teardown, gate-smp]
locks: [lock-dev9p-poll-glock, lock-9p-client-c-lock]
hazards: [haz-death-path-wake]
abis: []
design: [docs/NET-DESIGN.md]
created: 2026-07-31
updated: 2026-07-31
---
## Purpose

The one kernel surface of the net arc: makes `poll()` on a netd readiness
file (`/net/<proto>/N/ready`, qid marked `QTPOLL`) block until the socket
satisfies the requested events. The poller PARKS in `sys_poll_for_proc` —
it does not block-read — so no synchronous reader drives the elected 9P
reader for the outstanding readiness Tread; a boot-spawned GLOBAL poll-pump
kthread does (the cons_poll `console_mgr` / Loom-4 SQPOLL analog). This is
I-9 generalized to an elicited-readiness relay over the most-audited
mechanism family in the tree (the #841 elected reader).

## Contract

- `dev9p_poll(c, events, pw)` — the Dev `.poll` slot. QTPOLL-marked Spoors
  are probed; ANY other dev9p file is POSIX always-ready
  (`events & POLL_REQUESTABLE`) — the fail-safe gate (an unmarked file or a
  plumbing slip degrades to always-ready, never an unsound probe of a
  regular file). `pw == NULL` is the sample-only re-scan.
- `dev9p_poll_init` (boot) + `dev9p_poll_pump_main` (the kproc kthread
  entry, spawned once from `kernel/main.c`).
- `dev9p_poll_priv_release(p)` — the #294 cancel-at-close hook
  `dev9p_close` calls BEFORE the `ready`-fd Tclunk.
- `dev9p_poll_op_count_for_test` — the registry-length witness the
  teardown regression asserts against.

## Mechanism

The three spec actions of [[spec-net-poll]] map onto:

**`dev9p_poll` (PollerRegister).** Gates: QTPOLL on the cached qid, then
`p9_client_recv_is_deadline_capable` (the kthread's frame-boundary recv
deadline REQUIRES a deadline-capable transport — srvconn is; a
non-capable QTPOLL server would hang the kthread, so it degrades to
always-ready). Then:

1. Lazily allocate the per-Spoor `dev9p_poll_state` — candidate kmalloc'd
   OUTSIDE the registry lock, RELEASE-published under it (double-checked;
   the loser frees; the lockless fast-path ACQUIRE-load pairs with the
   publish — net-6b F5). `cand->refs = 1` is set BEFORE publish — the
   priv's ref; without it the first op's teardown takes refs 1→0 and frees
   the state out from under `p->poll` (the #294 self-audit F-self-1
   would-be-P1).
2. **Register the hook FIRST** (`poll_waiter_list_register`), then under
   [[lock-dev9p-poll-glock]]: fold this poller's events into
   `wanted_mask`, sample `cached_revents` (ACQUIRE).
3. Not ready → **probe-then-observe** (the I-9 obligation): reuse a
   covering live op; or WIDEN — unlink the narrower live op (making it
   this caller's to abandon+free outside the lock, via Tflush #845) and
   submit the union; or submit fresh. `dev9p_poll_submit_locked` links the
   op into the registry + publishes `ps->op` + zeroes the stale cached
   bitmap BEFORE `p9_client_submit_async` — a synchronous submit failure
   fires the completion (POLLERR) on an already-linked op the kthread can
   reap, and the post-submit re-sample surfaces it without a park.
4. **F2/R2-F2 degrade**: if OOM left no fresh probe AND no covering live op
   exists, return always-ready — a safe spurious wake the app re-checks;
   the alternatives were an unwakeable infinite park (no live op) or a
   no-progress spurious-wake loop (narrower live op under sustained OOM).
5. Returning ready CONSUMES the cached bitmap (zeroed) — the cache is a
   one-shot bridge between the async completion and the poller's
   re-sample, NOT persistent readiness state; a stale "readable" after the
   app drained the data would busy-loop it (level-triggered semantics).

**The probe itself**: a Tread on the `ready` fid whose OFFSET carries the
event mask and whose count is 4 — netd defers the reply until satisfiable
and answers a 4-byte LE revents bitmap. The op is a `p9_client`
async submission whose completion (`dev9p_poll_complete`) runs UNDER the
client's `c->lock` (from the demux or `client_mark_dead_locked`) and
therefore does exactly three things: RELEASE-store
`VALID | revents` into `cached_revents`, RELEASE-store `terminal`, wake
the kthread rendez — no sleep, no poll-state lock, no `p9_client_*`
re-entry (the on_complete seam contract of
[[lock-9p-client-c-lock]]). A 9P error maps to POLLERR (a ready
condition); a malformed/short reply to 0 (not ready).

**The kthread (`dev9p_poll_service_once`, KthreadWalk).**
- Phase 1 (under the registry lock): sweep the chain — terminal ops to the
  reap list; non-terminal ops with an EMPTY `poll_list` to the abandon
  list (the poll that needed them ended — the GC). The empty-check is
  NESTED under the registry lock (g_lock → poll_list lock) so it is atomic
  with the unlink + `ps->op` clear against a concurrent `dev9p_poll` reuse
  (which registers its hook BEFORE taking g_lock: a poller already on the
  list defeats the GC; one that registers after sees `ps->op` cleared and
  submits fresh — no lost wake either way).
- Phase 2 (outside): for each reaped op, `poll_waiter_list_wake` (process
  context) then `dev9p_poll_op_free`. Phase 2b: abandon each stranded op at
  the client (Tflush) then free.
- Phase 3: `dev9p_poll_collect_clients` — the **F1 fairness fix**: collect
  the DISTINCT clients of the remaining non-terminal ops (dedup by
  pointer, bounded `DEV9P_POLL_MAX_PUMP` 16), taking an EXTRA session ref
  per client (the borrow-guard: the client must survive the unlock + the
  blocking pump even if a concurrent reaper frees the op it was borrowed
  from). Pump EVERY collected client's elected reader once with a 20 ms
  frame-boundary deadline (`DEV9P_POLL_IDLE_NS`), drop each borrow, yield
  if any pump reported the reader role held by a sync reader. Pre-fix the
  pump drove only the head op's client — a perpetually-parked op on client
  A starved client B's pending reply (v1.0-safe with ONE QTPOLL client;
  latent under per-user netd).
- Empty registry → park on the rendez with a cond that re-checks the
  atomic op count (register-then-observe under the rendez lock — the
  cons_mgr discipline). kproc never group-terminates; a defensive
  SLEEP_INTR just re-loops.

**#294 cancel-at-close (`dev9p_poll_priv_release`).** The op deliberately
pins the poll-state + the SESSION (`p9_attached_ref`) but NOT the Spoor —
pinning the Spoor deferred `dev9p_close` past the user's fd-close, which
was the permanent netd-slot-leak root (the pre-#294 design). So at close:
grab the outstanding op from the registry if still present (whoever
unlinks owns the teardown — the kthread may have collected it first;
`ps->op` is registry-consistent under g_lock), abandon it at the client
(clears `c->inflight[tag]` + Tflush — no late completion can fire on the
freed op, and netd releases the held Tread), free it, drop the priv's
poll-state ref, NULL `p->poll`. The caller then clunks the `ready` fid —
delivered deterministically at fd-close, not hinged on the kthread GC.
The session-core half of the fix (the `any_outstanding_on_fid`
awaiting_flush exclusion that lets the Tclunk follow the Tflush
immediately) lives in [[sub-kernel-ninep-session]].

## Data structures

`struct dev9p_poll_op`: `p9_rpc` at **offset 0** (`_Static_assert`-pinned —
the completion recovers the container by cast, the audited Loom idiom),
`ps` (+1 ref), `attached_owner` (+1 session ref; NULL only on the
externally-owned-client test path), borrowed `client`, `fid`, `mask`,
atomic `terminal`, registry `next`. `struct dev9p_poll_state`:
`poll_waiter_list` (own lock), atomic `cached_revents`
(`DEV9P_POLL_VALID` bit 16 + 16 revents bits), `op` + `wanted_mask` (under
g_lock), atomic `refs` (priv 1 + one per op; freed at 0 —
`specs/net_poll_teardown.tla` NoUseAfterFreePs). Globals: the registry
chain + atomic count + rendez + init flag.

## Concurrency

Lock order (verified acyclic, documented at the file head):
`g_dev9p_poll_lock → c->lock` (submit / abandon), `g_dev9p_poll_lock →
poll_list lock` (the GC empty-check nesting), `poll_list lock → …` (the
wake, OUTSIDE g_lock), `c->lock → rendez` (the completion's wake — leaf).
The registry lock is NEVER held across a wakeup or the blocking pump.
Memory ordering: state refs RELAXED-add (a holder exists) / ACQ_REL-sub;
`cached_revents` + `terminal` RELEASE/ACQUIRE pairs; the op count
RELEASE-mutated, ACQUIRE-read by the park cond. The poll-state's
`poll_list is empty at close` premise rests on the poll-scan discipline:
a registered poller's Spoor obj-ref is retained until after the unregister
sweep (the 2C-F1 held[] rule), so the last-ref close cannot run with a
live poller.

## Invariants enforced

![[inv-i9#Statement]]

Here as PROBE-then-observe: the hook is registered AND a covering
non-terminal probe is outstanding BEFORE the not-ready sample returns, so
no readiness edge between the sample and the park is lost
([[spec-net-poll]] NoMissedNetPoll; the `BUGGY_LOST_READY` cfg is the
counterexample). The teardown half — the slot-freeing clunk delivered
deterministically at fd-close with no op UAF — is
[[spec-net-poll-teardown]] (Fix=TRUE clean; Fix=FALSE reproduces the #294
leak).

## Error paths

OOM on the poll-state → always-ready. OOM on the op candidate → the F2
degrade matrix above. Synchronous submit failure → POLLERR via the normal
completion path. Client death → `client_mark_dead_locked` error-completes
every async op (POLLERR cached + terminal); the next cycle reaps them and
wakes pollers; a dead client's ops drop out of the collect.

## Performance

The 20 ms idle deadline means a parked-forever poll costs the kthread a
50 Hz wake ([[seam-221-idle-pump-wake]]); the deadline is LOAD-BEARING
(it is what lets the kthread GC stranded ops and notice widens — an
unbounded recv would wedge it on a never-ready socket). netd-side,
`c1e49fb1` (2026-06-21) taught the serve loop to honor `poll_delay` while
a probe is pending (~6× loopback throughput) — the netd half of the same
economics.

## Prosecution

- **The I-9 window**: any reordering of register-hook / ensure-probe /
  sample, or a submit that returns before the op is registry-linked,
  reopens the lost-readiness park.
- **The borrow-guard**: the kthread must never deref an op after the
  unlock without a pin it took under the lock; the collect's extra session
  ref must balance exactly (pin per collected client, drop per pump).
- **Teardown races**: close-grab vs kthread-collect (both unlink under
  g_lock — whoever unlinks owns the free); abandon must precede the free
  (a late completion on a freed op is the UAF the Tflush prevents);
  the reap's wake must precede `op_free` (the wake touches `ps` the op's
  ref keeps alive).
- **The consume-on-ready rule** (level-triggered) and the widen's
  abandon+union (a `poll(POLLOUT)` must never hang behind a live
  `poll(POLLIN)` probe).
- **The fairness cap's cliff**: >16 distinct QTPOLL clients STARVES the
  tail outright (LIFO head-anchored collect, no rotation) —
  [[seam-223-pump-tail-starvation]]; the v1.x per-client work-queue must
  use a fair start.

## Seams

- [[seam-221-idle-pump-wake]] — the 20 ms re-poll while a probe is parked
  (v1.x: transport wake-on-write).
- [[seam-223-pump-tail-starvation]] — the >16-client LIFO cliff.
- The pouch ready-fd slot-reuse ABA (net-6b F4, task #222) lives on the
  pouch surface — records with its sweep.
- The deterministic two-QTPOLL-client fairness regression for F1 remains
  owed (no in-tree config drives two clients);
  [[seam-841-mi-harness]] is the same family's umbrella.

## Caveats

- The op's session ref means a stranded op can hold the whole attach
  session alive until GC'd — bounded by the 20 ms cycle.
- `dev9p_poll_complete` may fire from `client_mark_dead_locked` with
  status < 0 and `dr == NULL`; the revents mapper handles both.
- A previous terminal op is left in the registry when a fresh one is
  submitted over it (`ps->op` overwritten); the kthread reaps it from the
  chain — the chain, not `ps->op`, is the ownership root.

## Provenance

(generated from incoming `touched` edges — net-6b-2b
[[chg-2026-06-18-net6b-poll-bridge]], the net-6b-4 close
[[chg-2026-06-18-net6b4-close]], #294
[[chg-2026-06-21-294-cancel-at-close]].)

## Tests

`dev9p.poll_regular_file_always_ready` (the QTPOLL gate),
`dev9p.poll_cancel_at_close` (the #294 regression: a deferred readiness
Tread outstanding at close → no extinction + op torn down + Tflush
submitted + the `ready`-fd Tclunk delivered + fid unbound; the racy
mid-test snapshot was removed at the #294 formal round's F1). The live
path: the joey net-6b boot probe (POLLOUT-ready full loop + POLLIN
park-then-timeout with kthread GC) + `netd: net-6b ready E2E PASS` +
[[gate-smp]].
