---
id: sub-kernel-rendez
type: sub
parent: moc-kernel-scheduling
title: "The wait/wake primitive — Rendez, sleep, tsleep, wakeup"
code: ["kernel/sched.c", "kernel/include/thylacine/rendez.h", "kernel/test/test_rendez.c"]
audit: hard
guarded-by: [inv-i9, inv-i8]
validated-by: [spec-scheduler, spec-tsleep, spec-death-wake, gate-smp]
locks: [lock-wait, lock-timerwait, lock-rendez]
created: 2026-08-01
updated: 2026-10-06
---
## Purpose

The one place a kernel thread blocks. Plan 9's Rendez: a waiter, a waker,
and a caller-supplied condition. Every higher wait in the tree — poll,
pipe, the 9P client, srvconn, the console, `wait_pid`, torpor — is built
on `sleep`/`tsleep` and inherits their properties, including the ones
that are not obvious.

## Contract

- `sleep(r, cond, arg)` blocks until `cond(arg)` is true. Returns
  `SLEEP_OK`, or `SLEEP_INTR` when the thread must **unwind**: its Proc
  is group-terminating, or a terminate-disposition `interrupt` is
  pending. A caller that receives `SLEEP_INTR` releases its locks, frees
  transient state, and returns; the Thread then dies at its EL0-return
  die-check. The value returned to userspace is immaterial — a flagged
  Thread never re-enters EL0.
- `sleep_noteintr` / `tsleep_noteintr` (item 11, ARCH 8.8.3) are the
  caught-note-interruptible variants (`caught_ok`). After the die-check --
  death always wins -- they may also return `SLEEP_NOTEINTR` /
  `TSLEEP_NOTEINTR`: the caller unwinds with `-T_E_INTR` and LIVES, and the
  caught note delivers at its EL0-return tail. The arm fires only when `cond`
  is still false (data wins over the note) and the one predicate
  `thread_caught_note_unwinds` holds, which poll's verdict shares: the Proc is
  a Linux phenotype, a mid-frame 9P reader is at a frame boundary (the #90
  guard), and -- tested LAST -- `thread_caught_note_claim` claims the note's
  family, which it does
  only for a thread whose call is on signal(7)'s list (`note_interruptible`)
  and only once per note: the peers the same wake reached find the family
  claimed and re-park ([[sub-kernel-notes]]). The claim is the claimant's until
  its EL0-return tail, which releases it and, if the note is still queued there,
  wakes the parked peers again. Every kernel wait a call on signal(7)'s list
  can reach opts in since [[chg-2026-10-05-signal7-list]]: the two 9P waits,
  a pipe's read and write, the console's read and write waits, poll's park
  and its timeout-only sleep, `wait_pid_for` and the futex wait (ARCH 8.8.3
  names the three that do not). A caller RETURNS on `*_NOTEINTR`: the claim
  lasts until its tail, so a second wait in the same call would unwind at
  once. Witnesses:
  `rendez.caught_note_one_unwind` -- two sleepers of one Linux Proc, one caught
  `child_exit`: exactly one unwinds and the other re-reads its cond and re-parks,
  and a caught note of another family then unwinds the re-parked one;
  `rendez.caught_note_tail_discards_and_releases` and
  `rendez.caught_note_release_wakes_peer`, which run the real tail.
- `tsleep(r, cond, arg, deadline_ns)` adds a deadline on the
  `timer_now_ns` timebase. Returns `TSLEEP_AWOKEN` / `TSLEEP_TIMEDOUT` /
  `TSLEEP_INTR`. **`cond` has precedence**: a wait satisfied exactly as
  the deadline lapses reports AWOKEN. `deadline_ns == 0` means "no
  deadline" and degrades to `sleep`; `tsleep_noteintr` with no deadline
  degrades to `sleep_noteintr`, carrying the caught-note flag, and maps
  `SLEEP_NOTEINTR` back to `TSLEEP_NOTEINTR`. Six of the signal(7) waits
  sleep this way (the futex, three console waits, poll's park and `pause()`
  with no timeout); dropping the flag in the delegation turns exactly their
  witnesses red.
- `sleep_death_only(r, cond, arg)` is `sleep` for the waits a stop or a
  parent suspend must not leave early (DEBUG-FS-DESIGN 5g): the tail's stop
  park, the birth park, the nested stop park, the vfork suspend and the held
  spawn's birth wait. It returns `SLEEP_INTR` **only** when the Proc is
  group-terminating (`thread_group_death_pending`, which keeps
  `thread_die_pending`'s `exit_close_active` gate). A wake for anything else
  is absorbed: `cond` is re-checked and the thread sleeps again. The two stop
  parks sleep on the thread's own `debug_rendez`, which nothing else sleeps
  on, and the latch's, a caught note's and a second stop's wake walks pass a
  thread blocked there by: the park could only absorb the wake, and a thread
  run to absorb it reads as unsettled to the debugger. The parent suspends,
  on another rendez, take the latch's wake and absorb it. The registration
  and the death wake are `sleep`'s own, so [[inv-i9]] holds unchanged.
  `el0_return_stop_check`, the tail's stop park, has had a third caller since
  2026-10-05, after the two tails: the notes leg, which
  parks for a stop it applied itself (an uncaught `tty:susp`) before the
  thread runs again. `rendez.tail_parks_for_the_stop_it_applies` runs that
  leg, masked, on a kernel thread and requires the park on `debug_rendez` and
  no return until the stop lifts; a `child_exit` is its control, one variable
  away ([[sub-kernel-notes]]).
- `wakeup(r)` wakes the at-most-one sleeper; a no-op if none. Returns
  whether it woke anyone. Safe from IRQ context.
- **The producer's obligation**: make `cond` true *before* calling
  `wakeup`. `cond` is evaluated under `r->lock`, so the producer's write
  must either hold `r->lock` or be followed by `wakeup(r)`, which takes
  it.
- `cond` must be side-effect-free and cheap: it is called repeatedly, and
  in `tsleep` it is called while three locks are held.
- **Single waiter.** At most one Thread per Rendez. A second sleeper is
  an extinction, not a queue — multi-waiter waits are built one layer up
  (`poll_waiter_list`).
- `tsleep`'s deadline is delivered off the 1 kHz tick, so it may
  overshoot by up to a tick. It is a coarse backstop, not a timer.

## Mechanism

**The core protocol** is check-under-lock, register-under-lock, then
drop and yield:

    lock(t->wait_lock);  lock(r->lock);
    while (!cond(arg)) {
        r->waiter = t;  t->rendez_blocked_on = r;  t->state = SLEEPING;
        ...death re-check...
        unlock(r->lock);  unlock(t->wait_lock);
        sched();                       /* prev is SLEEPING -> stays out of the tree */
        lock(t->wait_lock);  t->rendez_blocked_on = NULL;  lock(r->lock);
    }
    unlock(r->lock);  unlock_irqrestore(t->wait_lock);

The window between the unlock and `sched()` is the canonical wait/wake
race, and it is closed on the *waker's* side: `wake_rendez_waiter` spins
on `t->on_cpu` until the sleeper's previous CPU has finished switching it
out, so a waiter is never readied off a half-saved context.

**`wait_lock` is the outermost lock and carries the IRQ mask** for the
entire call, including across the `sched()` yields. It is taken before
`r->lock` (and before `g_timerwait.lock`) so that the group-terminate
cascade — which holds a peer's `wait_lock` while it reads
`rendez_blocked_on` and wakes the rendez — is serialized against this
thread's register-then-observe. It is emphatically **not** held across
`sched()`: a descheduled sleeper holding it would deadlock the cascade.

**Register-then-observe.** After registering and before yielding, the
sleeper re-checks `thread_die_pending(t)` *under `wait_lock`* — the same
lock both wakers take per peer. Either this thread registered before the
waker's walk (so the walk finds and wakes it), or the flag-set
happens-before this thread's `wait_lock` acquire (so the re-check sees
it). There is no third interleaving. On a hit it undoes the **full**
registration — rendez waiter, backref, and in `tsleep` the timer-wait
link — and returns INTR.

The same check repeats on the resume path as the *prompt* path: the
registered check would catch it on the next iteration anyway, but
returning immediately avoids a pointless loop.

**The unwind mode.** `sleep`, `sleep_noteintr` and `sleep_death_only` share
one core, `sleep_common`, which takes what may end the wait early as an enum,
each tier including the one before it: group death (`SLEEP_UNWIND_DEATH`),
the terminate latch (`SLEEP_UNWIND_TERMINATE`), a caught note
(`SLEEP_UNWIND_NOTE`). Both die-checks, the registered one and the prompt one,
read the predicate the mode names: `thread_group_death_pending` for death
alone, `thread_die_pending` otherwise. Neither reads a reader latch: the
elected 9P reader unwinds at any byte, like every sleeper (ARCH 8.8.1.1, since
2026-10-06). A stop-unwinding 9P reader never waits death-only (its recv is an
ordinary sleep), and `sleep_common` extincts if one reaches the stop detour in
that mode: its `SLEEP_INTR` would read as group death to a caller like the
vfork suspend.

**Who clears `rendez_blocked_on`.** Only the owning Thread, on its own
resume, under its own `wait_lock`. `wake_rendez_waiter` deliberately does
*not* clear it (#811): clearing under `r->lock` would race the cascade's
read under `wait_lock`. The owner is still SLEEPING when it is woken, so
the backref stays valid until it resumes.

**Two detours**, both inside the loop, both before registration:

- **The stop detour** (8c-2). If a stop is pending from either owner —
  the debugger or job control, via `proc_stop_requested`: the
  `debug | job` disjunction, which reads false in a dying group, so the
  last thread's exit close (which reads no death, `exit_close_active`)
  never parks for a stop group death did not clear — the sleeper parks on its own
  `debug_rendez` until both clear, then re-loops and re-checks the
  *original* condition. The syscall re-blocks in place: no unwind, no
  restart. Gated `r != &t->debug_rendez` so the nested park cannot
  recurse, and gated on `t->proc` so a kernel thread is skipped.
- **The 9P client's unwind** (8c-3; ARCH 8.8.1.1). A sleep with
  `stop_unwinds` set does not park for a stop: the detour returns
  `SLEEP_INTR` and latches `stop_unwound`, and the client releases the
  reader role and parks the thread itself. The elected 9P reader holds
  `stop_unwinds` for its whole recv, because a reader parked in place would
  hold the role and freeze every survivor sharing the client; every other
  wait inside the client sets it too. The reader unwinds at any byte of a
  frame: the client keeps what it has read (`rx_got`) and the next reader
  resumes it ([[dec-2026-10-06-seam90-unwind-any-byte]]). From 2026-07-19 to
  2026-10-06 a reader mid-frame blocked through a stop, a death and a caught
  note (`thread_reader_blocks_death`, #90), because its bytes were then its
  own and an unwind lost them; a server that stopped inside a frame held it
  until the server died ([[seam-90-hung-server]]).

  Death still wins over a stop at every branch.

**`tsleep`'s third wake source.** A deadlined sleeper is also linked into
one global list, `g_timerwait`, and registered atomically with the rendez
under all three locks. `sched_tick` calls `timerwait_tick` on every fire,
which wakes expired sleepers **one at a time**: each iteration takes
`g_timerwait.lock`, finds one expired sleeper, unlinks and wakes it under
both locks, then *releases the global lock* before the next. Each wake is
still atomic, but a burst of simultaneous timeouts can no longer stall
every other CPU's tick behind one long hold (the P5-tsleep F6 fix). `now`
is sampled once so the set this pass wakes is fixed and the loop
terminates.

`timerwait_tick` **pre-filters on `on_cpu`**: a mid-switch sleeper is
skipped and caught by a later tick, so the wake never spins inside the
timer IRQ handler.

**`wakeup`'s lock order.** It takes `g_timerwait.lock` as the *outer*
lock even for a plain `sleep` waiter that is never on the list, because
it cannot know whether the waiter is deadlined until it holds `r->lock` —
by which point taking the global lock would invert the order. It releases
the global lock the moment the unlink is done, so the `on_cpu` spin and
the `ready()` run under `r->lock` alone.

## Data structures

`struct Rendez { spin_lock_t lock; struct Thread *waiter; }` — 16 bytes,
statically initializable (`RENDEZ_INIT`) or `rendez_init`'d, embedded
freely in other objects.

`g_timerwait { lock; head }` — one global doubly-linked list of deadlined
sleepers, threaded through `Thread.timerwait_next/prev`. One lock, not
per-CPU: deadlined waits are the cold path, the scan is O(timed
sleepers), and the global lock is what [[spec-tsleep]] verifies.
`timerwait_is_linked` uses the same three-way test as the run tree,
because a sole element has both links NULL.

Thread fields owned here: `rendez_blocked_on`, `sleep_deadline`,
`sleep_timedout`, `timerwait_next/prev`, `wait_lock`, `debug_rendez`,
`stop_no_park`, `stop_unwinds`, `stop_unwound`.

## Concurrency

The full chain, outermost first:

    lock-proc-table -> wait_lock -> g_timerwait.lock -> r->lock -> cs->lock

Every one of those edges is taken in that order at every site, and the
reverse of the middle edge (`r->lock` then `g_timerwait.lock`) is exactly
what `wakeup`'s outer acquire exists to avoid.

`timerwait_earliest_deadline` — read by the tickless idle path — is a
**leaf** acquisition of `g_timerwait.lock` alone, irqsave because the
timer IRQ's `timerwait_tick` takes the same lock and an IRQ landing
mid-hold would self-deadlock. Unlike `timerwait_tick` it does *not*
filter `on_cpu`: it reads deadlines and wakes nothing, and a mid-switch
sleeper's near deadline still needs covering.

## Invariants enforced

- **[[inv-i9]]** — no wakeup lost between the condition check and the
  sleep. This dossier is the primitive the invariant is stated about;
  [[sub-kernel-death]] holds the death-wake generalization, and
  [[spec-death-wake]] pins it.
- **[[inv-i8]]** — a woken thread is `ready()`'d, so it re-enters
  dispatch.

## Error paths

`sleep`/`tsleep` return `*_INTR`; everything else is an extinction:

| Condition | Where |
|---|---|
| NULL rendez / NULL cond | entry |
| no current thread; corrupted current | entry |
| a second sleeper on one Rendez | the registration guard |
| current is not RUNNING | the registration guard |
| current already blocked on a rendez | the registration guard |
| already on the timer-wait list | `tsleep` only |
| waker sees a corrupted waiter, a non-SLEEPING waiter, or a backref mismatch | `wakeup` |

The three `wakeup` checks are worth reading as a set: they assert the
waiter is intact, is actually asleep, and agrees with the Rendez about
which Rendez it is asleep on. A violation of any of them means the wait
state has been corrupted by someone else.

## Performance

- `sleep`'s fast path (condition already true) takes two locks and
  returns without any state transition.
- `timerwait_tick`'s rescan-from-head is O(n²) in the per-tick herd size.
  Bounded and cheap for a cold path; per-CPU sharding would make it O(n)
  and is an optimization, not a correctness need.
- `wakeup` holds the global timer lock for exactly one unlink.

## Prosecution

- **The unconditional `r->lock` acquire in `wakeup` is LOAD-BEARING**
  (PTY-4e R2). Even on the no-waiter path. It is the only ordering chain
  delivering a torpor poster's `awoken = 1` — written before the call —
  to a stop-parked waiter's resumed `tsleep` re-loop, whose `cond` read
  pairs with *this* release. A lockless `r->waiter == NULL` fast path
  here looks like free performance and reintroduces a lost wake on the
  preserved-wait path. This is the single most attractive-looking wrong
  change in the file.
- **`wake_rendez_waiter` must not clear `rendez_blocked_on`.** Clearing
  it there races the cascade's read (#811).
- **The register-then-observe must undo the FULL registration.** In
  `tsleep` that means the timer-wait link as well as the rendez waiter;
  leaving the link behind strands an entry the tick will later wake into
  a thread that is no longer sleeping.
- **The detour ordering is fixed**: death check precedes the stop check,
  the stop detour precedes `timerwait_link`, and the detour is gated
  against its own rendez. A stop-parked `tsleep` re-registers with its
  *original* deadline on resume, and a deadline that lapsed while stopped
  correctly reports TIMEDOUT — wall-clock advances while a thread is
  stopped, and that is the accepted freeze semantics.
- **No die-check reads a reader latch**, on either path (the
  register-then-observe check and the prompt post-resume check). The reader
  may unwind at any byte because the client keeps the partial frame
  ([[spec-reader-frame]]); a guard put back on one path is the superseded
  block-through, and `rendez.reader_recv_unwinds_*` turn RED on it.
- **`timerwait_tick`'s `on_cpu` pre-filter stays.** Removing it puts an
  unbounded spin inside a timer IRQ handler.
- **Single-waiter is enforced, not assumed.** Any new caller that could
  see two threads on one Rendez needs a `poll_waiter_list`, not a second
  sleeper — the extinction is an unprivileged panic if it is reachable
  from EL0.

## Seams

- [[seam-timerwait-sharding]] — the one global timer-wait lock.

## Caveats

- `SLEEP_INTR` **aliases**: it means "unwind", not "died". Since LS-5c it
  also covers a terminate-disposition `interrupt`, and since 8c-3 it also
  covers a stop-unwind — which is why the 9P client reads the separate,
  stable `stop_unwound` latch rather than re-reading `debug_stop_req`
  (which races an async resume). From `sleep_death_only` it means group
  death alone.
- The unwind modes start at 2 and `sleep_common` extincts on any other value,
  so a `bool` passed where a mode belongs (0 or 1) is refused rather than read
  as a mode; `tsleep_common`'s no-deadline path maps its caught-note flag onto
  the enum explicitly. Before the values moved, `false` would have converted
  silently to the death-only mode and made every no-deadline `tsleep` ignore
  the latch.
- A caller that ignores `SLEEP_INTR` leaks whatever it was holding. The
  return is documented as ignorable *only* for callers with nothing to
  unwind.
- `tsleep`'s `deadline_ns` is absolute and the caller owns the overflow:
  a wrapped, now-past deadline times out at once, and a wrap to exactly 0
  reads as "no deadline".
- The single-waiter restriction is a special case of the multi-waiter
  spec, not a different protocol — the invariants carry over unchanged
  for a singleton-or-empty waiter set.

## Provenance

`sleep`/`wakeup` landed at P2-Bb ([[chg-2026-05-05-p2b-sched-dispatch]]);
`tsleep` and the timer-wait list at P5-tsleep
([[chg-2026-05-17-p5-tsleep]]). Universal death-interruptibility is
[[chg-2026-06-01-811-death-interruptible]]; the terminate-`interrupt`
widening rides [[arc-life-support]]; the stop detour is [[arc-go-ide]] and
[[arc-pty]]. The reader's block-through came with
[[chg-2026-07-19-90-death-block-through]] and went with
[[dec-2026-10-06-seam90-unwind-any-byte]].

Absorbed `docs/reference/16-rendez.md` at [[chg-2026-08-01-sched-sweep]].

**2026-10-05:** [[chg-2026-10-05-signal7-list]] put the four caught arms
(two in `sleep_common`, two in `tsleep_common`) behind one predicate,
`thread_caught_note_unwinds`, which poll's verdict also calls; their order and
meaning are unchanged, and every wait a listed call reaches now opts in.

**2026-08-16: re-verified, no content owed.** `kernel/sched.c` moved ~48
lines since the last sweep and this dossier was flagged for it, but every
hunk landed in `ready`, `sched_arm_clear_on_cpu`, `sched_install_asid_ttbr0`
and `sched()` — the dispatch half of a file two dossiers share. **Churn is
per FILE; ownership is per SURFACE**, and the two do not line up whenever a
file carries more than one layer. The check was hunk-context against the
function set this dossier owns (`sleep`, `tsleep`, `wakeup`,
`wake_rendez_waiter`, `timerwait_*`); none was touched. The dispatch-side
changes are on [[sub-kernel-sched]] ([[chg-2026-08-16-sched-addrspace-install]]).
