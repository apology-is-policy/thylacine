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
updated: 2026-09-28
---
## Purpose

The kernel side of remote readiness: makes `poll()` on a file whose readiness
lives in the server that serves it — a netd `ready` file
(`/net/<proto>/N/ready`) or a ptyfs `<n>ready` file, their qids marked
`QTPOLL` — answer truthfully and block until the file is ready. Since #98
(NP-4c, 2026-09-28) it does that with TWO reads, one per job: a SNAPSHOT the
server answers at once, which is the only thing a verdict rests on, and an
ARM the server holds until the file is ready, which is only ever a wake. The
poll core ([[sub-kernel-poll]]) drives both through three Dev slots. Nothing
synchronous waits on either reply, so a boot-spawned GLOBAL poll-pump
kthread drives the 9P elected reader (#841) for them — the cons_poll
`console_mgr` / Loom-4 SQPOLL analog.

Before #98 one deferred read did both jobs, read back through a cache, and a
truthful "not ready" was unrepresentable on the wire: a zero-timeout poll of a
plainly writable socket returned 0 off a cache the fresh file did not have,
and the vivarium widened a zero timeout to 10 ms to hide it.

## Contract

- `dev9p_poll_snapshot(c, events, s)` — `.poll_snapshot`. A file without
  `QTPOLL`, or on a client whose transport has no recv deadline, is answered
  here: POSIX always-ready (`events & POLL_REQUESTABLE`), `s->state`
  ANSWERED, no request. Otherwise it sets `s->remote`, builds the request on
  first use (reused on a resend), marks the slot SENT and submits a Tread at
  offset `mask | P9_POLL_SNAPSHOT`, count 4. A shortage leaves the slot
  UNSENT for the core to resend.
- `dev9p_poll_snapshot_release(c, s)` — `.poll_snapshot_release`. The
  barrier: after it returns no answer can write the slot, and a snapshot
  still unanswered has been flushed at its server. No-op on a slot with no
  request.
- `dev9p_poll_arm(c, events, pw)` — `.poll_arm`. Registers `pw` on the
  file's poll-state list, then ensures an arm covering `events` is on the
  wire. Returns 1 covered, 0 not (a shortage, a dead session, no memory) —
  the core then bounds its park by the retry timer. Returns 1 at once for a
  file with nothing remote to wait for.
- `dev9p_poll_init` (boot) + `dev9p_poll_pump_main` (the kproc kthread
  entry, spawned once from `kernel/main.c` before the kernel tests run).
- `dev9p_poll_priv_release(p)` — the #294 cancel-at-close hook
  `dev9p_close` calls BEFORE the `ready`-fd Tclunk.
- Test accessors: `dev9p_poll_op_count_for_test` (the arm registry's
  length), `dev9p_poll_snap_count_for_test` (snapshots still linked),
  `dev9p_poll_parked_for_test` (the kthread asleep on its park), and the
  collector hold `dev9p_poll_test_hold_gc` / `dev9p_poll_gc_held_for_test` /
  `dev9p_poll_test_hold_release` (the runner's release after every test).

## Mechanism

The kernel half of [[spec-net-poll]]'s action map:

**SNAPSHOT (`Scan`, `SnapshotReply`).** The request (`struct
dev9p_poll_snap`) lives from the first send attempt to the core's release,
always inside one pass of one poll call, whose held Spoor ref keeps the priv
and so the session alive; the request's own session ref is for the kthread's
borrow. It is linked on `g_dev9p_poll_snaps` and counted in the atomic
`g_dev9p_poll_snap_live` from the moment it is built — even UNSENT, because
the reply that frees a tag (an Rflush) has to be read by someone. Its
completion `dev9p_poll_snap_complete` runs under `c->lock` (the kthread's
demux, or `client_mark_dead_locked`) or, for a failure inside the submit, in
the submitting poller: `-P9_E_AGAIN` means the read never left the kernel
and touches nothing; anything else writes the revents (a 9P error is
POLLERR, a short reply 0, masked to the asked events plus the output-only
bits), RELEASE-stores ANSWERED, clears `live` exactly once
(`dev9p_poll_snap_unlive`, an exchange: the answer and the release race for
it), and wakes the poller's private rendez. The slot is on the poller's stack
and stays there: the poller cannot return before its release, which takes
`c->lock` after the completion ran.

**The release (`dev9p_poll_snapshot_release`).** `p9_client_abandon_async`
under `c->lock` first — an answer being delivered finishes before it returns,
and one still due is flushed (Tflush; its late reply is discarded ownerless)
and can no longer fire; for an UNSENT request it is a no-op — then the unlink
and the unlive under the registry lock, the session unref, the free.

**ARM (`PollerArm` / `PollerArmFails`, `ArmReply`).** The hook FIRST
(`poll_waiter_list_register` on `ps->poll_list`), so the arm's answer always
has a hook to walk, including one that beats the call's return. Then under
[[lock-dev9p-poll-glock]]: a non-terminal arm that already covers the events
is reused; otherwise a fresh arm for the UNION of the live arm's mask and
these events is submitted (refs taken first). It is linked, and published as
`ps->op`, only when `p9_client_submit_async` returned 0 — so the registry
holds only arms that are on the wire — and a widen unlinks the arm it
replaces only then, to flush and free it after the unlock: its pollers are
covered throughout, and a widen that cannot be sent keeps the old arm. The
arm's completion `dev9p_poll_arm_complete` sets `terminal` and wakes the
kthread; its bitmap is never read — the socket may be drained again before
the poller looks, so the woken poller samples again.

**The kthread (`dev9p_poll_service_once`, `KthreadWalk` / `GcArm`).**
- Phase 1 (under the registry lock): terminal arms to the reap list;
  STRANDED arms — non-terminal, with an empty hook list, because every
  poller that wanted them has moved on — are unlinked, `ps->op` cleared, AND
  FLUSHED (`p9_client_abandon_async`, g_lock → c->lock) in the same locked
  step. A close that takes the lock after this finds no arm to cancel, so the
  arm's read must already be off the fid: while it is live the session
  refuses the close's Tclunk (`any_outstanding_on_fid`), `dev9p_close` has no
  fallback, and the server's slot would stay bound for the session's life
  ([[spec-net-poll-teardown]] `BUGGY_SPLIT_GC` — the collector was split this
  way from #294 until NP-4c). The empty-check is nested under the registry
  lock (g_lock → poll_list lock), atomic with the unlink against a concurrent
  `dev9p_poll_arm`, which registers its hook before it takes the lock: a
  poller already on the list defeats the collector; one that registers after
  finds `ps->op` cleared and submits fresh. Snapshots are never collected
  (`buggy_gc_snapshot`): they have no hook by design, and their pollers
  release them.
- Phase 2 (outside the lock): each reaped arm's list is walked
  (`poll_waiter_list_wake`, process context), then the arm is freed. Phase
  2b frees the stranded arms, already flushed.
- Phase 3: `dev9p_poll_collect_clients` gathers the DISTINCT clients with a
  read out — a non-terminal arm or a live snapshot (dedup by pointer, bounded
  `DEV9P_POLL_MAX_PUMP` 16) — each with an extra session ref as the
  borrow-guard, and pumps each client's elected reader once with a 20 ms
  frame-boundary deadline (`DEV9P_POLL_IDLE_NS`). BUSY (a sync reader holds
  the role) or DEAD (only an UNSENT snapshot can still name a dead client,
  until its poller resends into it) yields the CPU rather than spin.
- Nothing out → park on the rendez; the cond re-reads both atomic counts
  under the rendez lock (`op_count || snap_live`), each bumped before the
  submitter's wake — register-then-observe, the cons_mgr discipline.

**A SHORTAGE IS NOT AN ANSWER.** `p9_client_submit_async` reports no free tag
or a full send ring as `-P9_E_AGAIN` (NP-4b, [[sub-kernel-ninep-client]]) and
fires the completion with it; both completions leave everything alone,
because the read is its submitter's again. A snapshot waits UNSENT for the
core's resend; a failed arm is freed and the core bounds its park.

**#294 cancel-at-close (`dev9p_poll_priv_release`).** An arm pins the
poll-state + the SESSION (`p9_attached_ref`), NOT the Spoor — pinning the
Spoor deferred `dev9p_close` past the user's fd-close, the permanent
netd-slot-leak root. At close: grab `ps->op` from the registry if still
there (whoever unlinks owns the teardown; the collector may have taken it,
and then it has already flushed it), abandon it at the client, free it, drop
the priv's poll-state ref, NULL `p->poll`. The caller then clunks the
`ready` fid — delivered deterministically at fd-close. No snapshot can be out
here: the core releases each before it drops its Spoor ref. The session-core
half (the `any_outstanding_on_fid` exclusion of a flushed entry, which lets
the Tclunk follow the Tflush at once) lives in [[sub-kernel-ninep-session]].

## Data structures

`struct dev9p_poll_op` (the arm): `p9_rpc` at **offset 0**
(`_Static_assert`-pinned — the completion recovers the container by cast, the
audited Loom idiom), `ps` (+1 ref), `attached_owner` (+1 session ref; NULL
only on the externally-owned-client test path), borrowed `client`, `fid`,
`mask`, atomic `terminal`, registry `next`. `struct dev9p_poll_snap` (the
snapshot's request): `p9_rpc` at offset 0, the poller's slot `s`,
`attached_owner`, `client`, `fid`, `mask`, atomic `live`, `next`. `struct
dev9p_poll_state`: `poll_waiter_list` (own lock), `op` (the newest arm, under
g_lock), atomic `refs` (priv 1 + one per arm; freed at 0 —
[[spec-net-poll-teardown]] NoUseAfterFreePs). The poller's slot is the
core's `struct poll_snap` (`poll.h`). Globals: the arm registry + its atomic
count, the snapshot list + its atomic live count, the rendez, the init flag,
the test hold.

## Concurrency

Lock order (verified acyclic, documented at the file head):
`g_dev9p_poll_lock → c->lock` (the arm submit; the collector's flush),
`g_dev9p_poll_lock → poll_list lock` (the collector's empty-check),
`poll_list lock → g_timerwait → rendez → cpu_sched` (a walk's wakes, OUTSIDE
g_lock), `c->lock → rendez` (a completion's wake — leaf). No completion takes
g_lock, so the edge from g_lock to c->lock cannot close a cycle. The registry
lock is never held across a wakeup, a pump, a snapshot submit or an unref.
Memory ordering: a snapshot's ANSWERED is a RELEASE store the settle's cond
ACQUIRE-loads under the rendez lock; `terminal` and `live` are RELEASE/ACQUIRE
pairs; both counts are RELEASE-mutated and ACQUIRE-read by the park cond.
The `poll_list is empty at close` premise rests on the poll core's discipline:
a registered poller's Spoor obj-ref is retained until after its unregister
sweep (the 2C-F1 held[] rule), so the last-ref close cannot run with a live
poller.

## Invariants enforced

![[inv-i9#Statement]]

Here as hook-THEN-arm: when a poller parks, its hook is on the list and an
arm covering it is on the wire, or its park is bounded by the retry timer
([[spec-net-poll]] NoMissedNetPoll; `buggy_lost_ready`, `buggy_no_retry`),
and the server evaluates the arm's level on arrival, so readiness that rose
after the snapshot is answered at once (`buggy_edge_arm` — netd's and
ptyfs's obligation). A verdict rests only on a snapshot answered in its own
pass (`NoFalseNotReady` / `NoFalseReady`; `buggy_cache_only_sample`,
`buggy_stale_cache`). The teardown half — the slot-freeing clunk delivered
deterministically at fd-close with no arm UAF — is [[spec-net-poll-teardown]].

## Error paths

No memory for a snapshot request → UNSENT (resent). No memory for the
poll-state or an arm → `dev9p_poll_arm` returns 0 (the retry timer covers
the park). A shortage → the same two outcomes. A synchronous submit failure
on a dead session → the completion's POLLERR, which the verdict reports.
Client death → `client_mark_dead_locked` completes every read in flight —
snapshots answered POLLERR, arms terminal — and a dead client's reads drop
out of the collect.

## Performance

One server round trip per poll pass, whatever the fd count: every snapshot of
a pass is sent before the core waits. The 20 ms idle deadline makes a parked
arm cost the kthread a 50 Hz wake ([[seam-221-idle-pump-wake]]); it is
load-bearing — it lets the kthread collect stranded arms and notice new work
instead of wedging in a never-ready server's recv. KNOWN (in OPEN-BUGS,
needs design): the one pump serializes clients, and a new snapshot's wake
does not interrupt a pump already blocked in another client's recv, so a
snapshot's answer can wait up to one 20 ms idle pump — each pass, now that
every sample is a snapshot. Correctness holds (the settle waits; the 1 s bound
is far away).

## Prosecution

- **The completion contexts**: under `c->lock` from the demux or mark_dead,
  or in the submitter for a synchronous failure; never sleep, never g_lock,
  never re-enter `p9_client_*`; `-P9_E_AGAIN` touches nothing.
- **The release barrier**: `abandon_async` before the unlink and the free —
  the answer writes a slot on the poller's stack, so a release that let a
  completion run after it would write a dead frame.
- **Linked means on the wire**: an arm reaches the registry only after a
  successful submit; a widen flushes the old arm only after the new one is
  out; a failed widen keeps it.
- **The collector is one step**: unlink, `ps->op` clear and flush under the
  registry lock ([[spec-net-poll-teardown]] `BUGGY_SPLIT_GC`).
- **The borrow-guard**: the kthread never derefs a request after the unlock
  without a pin it took under the lock; one session ref per collected client,
  dropped per pump.
- **The fairness cap's cliff**: more than 16 distinct QTPOLL clients STARVES
  the tail outright (LIFO head-anchored collect, no rotation) —
  [[seam-223-pump-tail-starvation]]; a per-client work queue must use a fair
  start.

## Seams

- [[seam-221-idle-pump-wake]] — the 20 ms re-poll while an arm is parked
  (v1.x: transport wake-on-write).
- [[seam-223-pump-tail-starvation]] — the >16-client LIFO cliff.
- The pump's cross-client serialization (Performance) — the same per-client
  pump would close both.
- The pouch ready-fd slot-reuse ABA (net-6b F4, task #222) lives on the
  pouch surface.
- The deterministic two-QTPOLL-client fairness regression for F1 remains
  owed (no in-tree test drives two clients); [[seam-841-mi-harness]] is the
  family's umbrella.

## Caveats

- An arm's session ref means a stranded arm holds the whole attach session
  alive until collected — bounded by the kthread's 20 ms cycle.
- A terminal arm superseded by a fresh one stays in the registry until the
  kthread reaps it (`ps->op` now names the fresh one); its walk still wakes
  the pollers it served, and each arms again for itself. The registry, not
  `ps->op`, is the ownership root.
- On the multi-queue loopback test transport an armed recv deadline returns
  at once on an empty ring, so while a test holds an arm the kthread pumps
  in a loop rather than every 20 ms; kthreads are preemptible, so it costs
  CPU, not progress.

## Provenance

(generated from incoming `touched` edges — net-6b-2b
[[chg-2026-06-18-net6b-poll-bridge]], the net-6b-4 close
[[chg-2026-06-18-net6b4-close]], #294
[[chg-2026-06-21-294-cancel-at-close]].) #98's SAMPLE/ARM split:
`dec-2026-09-28-poll-sample-arm-split`; NP-4c rewrote this file.

## Tests

The `dev9p.poll_*` set in `kernel/test/test_dev9p.c`, on the multi-queue
loopback with a scripted readiness server and the live kthread (the teardown
waits for the kthread to park before it destroys the client):
`regular_file_always_ready` (the QTPOLL gate),
`snapshot_answers_at_zero_timeout` (#98 itself),
`local_and_remote_both_reported` (the settle), `snapshot_shortage_is_resent`,
`unanswered_snapshot_fails_safe` (the fixed bound, the flush, the count),
`arm_wakes_the_parked_poller` (the walk), `retry_timer_is_a_wake`,
`widen_keeps_the_old_arm_until_replaced`, `cancel_at_close`, and
`gc_flushes_with_the_unlink` (the kthread held between its collect and its
frees while the file closes) — each shown RED on its sabotaged kernel, on
the assert that names the rule. The live path: the joey net-6b boot probe,
`netd: net-6b ready E2E PASS`, the pty-probe's ready wire, viv-pheno-probe
L113 (a ready socket at timeout 0), and [[gate-smp]].

**The tests are built to fail cleanly.** A failing assert returns early, and
what the test had linked stays linked. So the fixture, the hooks a test
registers through `dev9p_poll_arm`, and its poller thread live in static
storage, never on the test's stack: a hook on a dead frame would be written
by the next arm's walk. `np_setup` refuses while a failed test's fixture is
still up, so it never re-initialises a client the kthread may be pumping.
`np_teardown` takes the fixture down in dependency order (hooks, poller,
file, kthread parked, client) and leaves it up (a leak, never a UAF) when a
thread will not let go. The kernel test runner releases a fixture a failed
test left up, printing `NP-FIXTURE`, alongside the two poll knobs
(`POLL-KNOB`). The regular-file test reads and releases everything before
it asserts anything. Before this, one failing test cascaded: under a broken
QTPOLL gate the regular-file test left a live snapshot whose slot was on its
dead stack, and the next test hung the boot.
