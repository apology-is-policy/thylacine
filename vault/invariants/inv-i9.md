---
id: inv-i9
type: inv
title: "I-9 — no wakeup lost between cond-check and sleep"
number: I-9
guards: [sub-kernel-ninep-client, sub-kernel-ninep-dev9p-poll, sub-kernel-srvconn]
validated-by: [spec-reader-frame, spec-9p-client, spec-net-poll, spec-net-poll-teardown, gate-smp]
strength: spec
created: 2026-07-31
updated: 2026-07-31
---
## Statement

No wakeup is lost between a sleeper's condition check and its sleep. This
includes:

- the **death-wake generalization** (#811): every rendez sleep is
  death-interruptible via register-then-observe under the per-Thread
  `wait_lock`;
- the **frame-atomic refinement for the elected 9P reader recv** (#90, ARCH
  §8.8.1.1): a mid-frame death defers its unwind to the next frame boundary —
  the reader still dies, at the boundary, never mid-frame;
- the **terminate-`interrupt` extension** (LS-5): the death-or-terminate wake
  predicate;
- the **Weft readiness poke**: the single-cache-line store-buffer
  register-then-observe.

> Backfill note: the guard and validator sets above are PARTIAL — the full
> ARCH §28 row also binds the scheduler, poll, pipe, cons, tsleep, torpor,
> and Weft surfaces (specs `scheduler`/`poll`/`cons_poll`/
> `weft_readiness`/`tsleep`/`death_wake`). Those edges join as their
> dossiers land in the sweep. (dev9p.poll joined at the 9P-area sweep;
> srvconn at the srv-area sweep.)

## Enforcement

On the 9P-client surface: `sleep`/`tsleep` register-then-observe (the #811
contract) · the send-side park in `client_send_flow` (hook registered +
`send_progress` snapshotted under `c->lock`, own-rendez re-check — the
poll.tla pattern) · `client_mark_dead_locked` as the SOLE `c->dead` setter,
waking both the per-rpc rendez set and the parked-sender list (no strand on
death) · `reader_recv_frame` + `thread_reader_blocks_death` (frame-atomicity:
`stop_no_park` held for the recv tenure, `stop_unwinds = (got == 0)`
per-chunk, guarding all four `thread_die_pending` sites in `sleep()`/
`tsleep()`).

On the dev9p.poll surface: PROBE-then-observe — the poller's hook is
registered and a covering non-terminal readiness probe is outstanding
BEFORE the not-ready sample returns (`dev9p_poll`); the kthread's park
cond re-checks the registry count under the rendez lock
([[sub-kernel-ninep-dev9p-poll]]).

On the srvconn surface ([[sub-kernel-srvconn]]): the role park's
register-then-observe (`chan_role_acquire` — hook registered under the
chan lock before tsleep re-samples the flag) · the chan-cond/wake
happens-before pairing at all five blocking loops · the drain-wakes on
every consume path · teardown's complete wake set (both rendezes, both
wrendezes, both role lists, the poll list) · the per-chunk POLLIN edge
in the blocking client send (the deferred edge was [[fnd-cf3b-r1-f1]]).

## Validation

[[spec-reader-frame]] pins the frame-atomic refinement (NoDesync +
UnwindAtBoundary + EventuallyUnwinds; the buggy cfg is the pre-#90 mid-frame
unwind). [[spec-9p-client]] composes beneath it. [[gate-smp]] is the
empirical backstop for the SMP park interleavings. **blind-to:** the specs
model protocol shape, not the memory-ordering of lock-free fast paths (those
rest on the documented atomics contracts); the deterministic multi-in-flight
interleavings remain owed — [[seam-841-mi-harness]].
