---
id: inv-i9
type: inv
title: "I-9 — no wakeup lost between cond-check and sleep"
number: I-9
guards: [sub-kernel-rendez, sub-kernel-sched-smp, sub-kernel-death, sub-kernel-ninep-client, sub-kernel-ninep-dev9p-poll, sub-kernel-srvconn, sub-netd-server, sub-kernel-poll, sub-kernel-pipe, sub-kernel-torpor, sub-pouch-thread, sub-kernel-irqfwd, sub-tapestryd]
validated-by: [spec-scheduler, spec-tsleep, spec-sched-tickless, spec-sched-rebalance, spec-death-wake, spec-reader-frame, spec-9p-client, spec-net-poll, spec-net-poll-teardown, spec-poll, spec-pipe, gate-smp]
strength: spec
created: 2026-07-31
updated: 2026-08-02
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
> ARCH §28 row also binds the cons and Weft surfaces (specs
> `cons_poll`/`weft_readiness`). Those edges join as their dossiers land
> in the sweep. (dev9p.poll joined at the 9P-area sweep; srvconn at the
> srv-area sweep; netd's userspace analog at the netd sweep; the
> **death-wake leg DISCHARGED** at the execution-area sweep; the
> **scheduler / tsleep / tickless legs DISCHARGED** at the
> scheduling-area sweep — [[sub-kernel-rendez]] is the primitive the
> invariant is stated about; the **poll / pipe / torpor legs DISCHARGED**
> at the ipc-wake sweep — [[spec-poll]] + [[spec-pipe]] above the line,
> torpor's leg at PROSE strength by design, the suspension's first
> worked example.)

## Enforcement

On the death path ([[sub-kernel-death]]) — the GENERALIZATION, and the one
the other surfaces specialize: `sleep`/`tsleep` register-then-observe under
the per-Thread `wait_lock` (the Plan 9 `p->rlock` analog), and
`proc_group_terminate` publishes `group_exit_msg` BEFORE walking
`p->threads` and taking each peer's SAME `wait_lock` to read
`rendez_blocked_on` and `wakeup()` it. The two critical sections are
mutually exclusive on that lock, so every Thread either observes the flag
in its own register-then-observe and dies without sleeping, or is found
SLEEPING by the walk and woken — no third interleaving. `wait_lock` is held
ACROSS `wakeup` (Option A) because `rendez_blocked_on` can point into a
sleeping peer's kernel stack frame; the pin is what stops the frame being
popped under the waker. Acyclicity rests on only the OWNER ever writing
`rendez_blocked_on`.

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

On the netd surface ([[sub-netd-server]], the USERSPACE analog): the
deferred-reply engines' no-lost-completion rests on serve-loop ORDER,
not a lock — `net.poll` (observe every stack edge delivered this tick)
runs before the `poll_*` delivery passes and before any dispatch that
can park a new pending, and netd is single-threaded, so no edge lands
between an empty-observe and its park unobserved. The net-4d F1 guards
(second-read-empty + re-write-reject on a deferred cs/dns fid) close
the held-tag clobber; `Disp::Deferred` guarantees at most one reply per
held tag. This is the server half [[spec-net-poll]] abstracts as the
readiness edge.

On the ipc-wake surfaces: [[sub-kernel-poll]] — install-and-sample in
one locked step per fd (`dev->poll` under the object lock; the hook
flag is the cross-lock handoff, `ready`-before-`wakeup` on the
producer walk); [[sub-kernel-pipe]] — every state-enabling mutation
carries its wake (the four wakes ↔ [[spec-pipe]]'s four buggy cfgs);
[[sub-kernel-torpor]] — compare + register under `torpor_lock`, the
same lock every wake walk takes (the PROSE proof; plus the
post-register die-pending re-check that closes the cascade race,
[[inv-i24]]'s futex leg).

## Validation

[[spec-death-wake]] pins the generalization itself: `NoLostDeathWake` +
`NoStuckSleeper`, with `BUGGY_OBSERVE_BEFORE_REGISTER` as the executable
counterexample — a sleeper that checks the flag before registering and
OUTSIDE its `wait_lock`, which reproduces the non-reaping hang.
[[spec-reader-frame]] pins the frame-atomic refinement (NoDesync +
UnwindAtBoundary + EventuallyUnwinds; the buggy cfg is the pre-#90 mid-frame
unwind). [[spec-9p-client]] composes beneath it. [[gate-smp]] is the
empirical backstop for the SMP park interleavings. **blind-to:** the specs
model protocol shape, not the memory-ordering of lock-free fast paths (those
rest on the documented atomics contracts); the deterministic multi-in-flight
interleavings remain owed — [[seam-841-mi-harness]].
