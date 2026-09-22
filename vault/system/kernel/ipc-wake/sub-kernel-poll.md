---
id: sub-kernel-poll
type: sub
parent: moc-kernel-ipc-wake
title: "poll — one thread waiting on N readiness sources"
code: ["kernel/poll.c", "kernel/include/thylacine/poll.h"]
audit: hard
guarded-by: [inv-i9]
validated-by: [spec-poll, spec-tsleep, gate-smp]
locks: [lock-poll-list, lock-rendez, lock-wait, lock-timerwait]
created: 2026-08-01
updated: 2026-09-22
---
## Purpose

`poll(fds, nfds, timeout_ms)` parks the caller until one of N fds is
ready or the timeout lapses. A `Rendez` is single-waiter and a thread
can wait on only ONE, so poll does not make Rendez multi-waiter: the
poller sleeps on its OWN private stack Rendez via `tsleep`, and
installs a lightweight `poll_waiter` hook on each polled object's
embedded hook list. Producers walk the list at every readiness edge.
This is the primitive under every select/poll-shaped consumer — the
`/srv` servers, netd's event loop via the pouch 0018 translation, the
dev9p.poll bridge's userside.

## Contract

- `sys_poll_for_proc(p, kfds, nfds, timeout_ms)` → count of pollfds
  with `revents != 0`, or -1 (bad args). `timeout_ms < 0` blocks
  indefinitely, `== 0` is a non-blocking probe, `> 0` bounds the park.
  **It returns 0 only at its deadline** (and never for `timeout_ms < 0`):
  a wake that turns out to be for nothing the caller asked about is
  followed by another sleep, not by a return. Until 2026-09-21 it was
  followed by a return — see Mechanism step 5. A DYING caller returns 0
  from any pass (it dies at its EL0-return tail); a STOPPED caller parks
  inside the call and resumes the same poll against the same deadline --
  with no hook listed when the loop's own check catches the stop, STILL
  listed when `tsleep`'s detour catches it first (a walk then only sets a
  flag the resumed `tsleep` reads; this said "no hook listed" flat until
  audit round 5 F5). A caller kept awake by noise crosses a preemption
  point each re-loop (step 5), where its CPU takes every pending
  interrupt; it adds no latency of its own.
- `nfds` ∈ [1, `POLL_MAX_NFDS` = 64]. **Deliberately decoupled from
  `PROC_HANDLE_MAX`**, which is now **1024** — 64 at the decoupling,
  256 by [[chg-2026-06-24-355-poll-decouple]], 1024 since the #198
  fid-ceiling chain. The frame stack-allocates `waiters[]` + `held[]` at
  the bound, 32 B + 24 B per fd (`sizeof(struct Handle) == 24` is
  `_Static_assert`-pinned), so restoring the identity now costs
  **56 KiB on a 16 KiB kstack** (`THREAD_KSTACK_SIZE`; the other 16 KiB of
  `THREAD_KSTACK_TOTAL_SIZE` is the guard region that exists to catch this
  exact overrun, so it is not headroom — #198's own stale-constant sweep
  compared against the 32 KiB total and understated the margin twofold,
  arriving at the same 56 KiB numerator by the same arithmetic).
  **The decoupling changed character without changing text**: at 256 it
  was prudence — 14 KiB of 16, leaving nothing for the rest of the frame
  but not itself an overrun — and at 1024 it is the only thing between
  `poll` and a guard-page walk. Lifting past 64 needs heap-backed arrays
  — [[seam-poll-heap-waiters]].
- Event bits are Linux-valued (`POLLIN` 0x001, `POLLOUT` 0x004, and
  output-only `POLLERR`/`POLLHUP`/`POLLNVAL`); `struct pollfd` is
  8 bytes, offset-pinned ABI.
- Per-fd semantics: negative fd or dead handle ⇒ `POLLNVAL` (which
  COUNTS as ready — POSIX); a Dev with no `.poll` slot ⇒ always-ready
  for the requested bits (the regular-file answer); a NULL-obj Spoor ⇒
  `POLLNVAL`, never always-ready (a buggy caller must not spin on fake
  readiness); `KOBJ_SRV` dispatches through `srv_handle_poll` (magic
  discriminates listener vs connection); every other kobj kind ⇒
  `POLLNVAL`. No `RIGHT_READ`/`RIGHT_WRITE` gate — polling asks about
  readiness, not access, and POSIX permits polling a write-only fd.
- The `Dev.poll` vtable op: `dev->poll(spoor, events, pw)` returns
  current revents and, iff `pw != NULL`, registers it — **atomically
  with the sample, under the object's own lock**. `sys_poll_for_proc`
  passes a hook on EVERY pass (since audit round 4); `pw == NULL` is a
  pure sample for other in-kernel callers and tests. A `.poll` MAY choose
  which of its lists to register on by the state it samples (the
  console's episode list) — the choice holds for one pass.

## Mechanism

1. **Register scan**: `dev->poll(c, events, &waiters[i])` per fd —
   [[spec-poll]]'s `Register`. Install-then-sample under the object
   lock means no readiness edge between the sample and the park can
   miss the hook.
2. Fast path: any ready, or `timeout_ms == 0` ⇒ jump to the sweep.
3. `tsleep` on the private Rendez with cond `any waiter.ready` — the
   cond reads `pw->ready` without object locks; sound because the
   producer writes it under the list lock and then `wakeup` takes the
   same rendez lock the cond runs under (release/acquire).
4. `TSLEEP_INTR` (#811 death/terminate) ⇒ skip the re-sample — the
   thread dies at its EL0-return check — but the sweep still runs:
   the hooks are stack memory and MUST be unlisted.
5. **The re-arm** (2026-09-21; [[spec-poll]] `Rearm` → `LoopCheck` →
   `Resample` → `EvaluateWake`). A flag is a HINT, not a verdict: one
   hook list serves every poller of an object whatever each asked for (a
   SrvConn's list carries four readiness edges for two endpoints —
   [[sub-kernel-srvconn]]), and readiness is a LEVEL a competing reader
   can lower between the wake and the look. Each pass therefore
   RE-REGISTERS: `poll_unhook_all` takes every hook off its list, drops
   every retained ref, and clears every hook (off its list no producer
   can reach it, so the clear needs no lock); then the loop's own death
   and stop checks (below); then each fd's `.poll` runs WITH its hook
   again — the first scan's install-and-sample. An event before an fd's
   install is seen by its sample; one after reaches the fresh hook. BEFORE
   the rescan, with hooks off and no lock held, the loop crosses the
   preemption point (below). Ready, or a `TSLEEP_TIMEDOUT` (the sleep is
   always to the poll's own deadline) ⇒ the sweep. Otherwise an explicit
   `timer_now_ns() >= deadline_ns` test, then `sched_yield_hint` (a noise
   pass bought nothing; queued work on this CPU runs first), then **loop
   to step 3 against the same absolute deadline**. The explicit
   test is load-bearing: `tsleep` prefers a set flag to a passed
   deadline, so a producer that never stops walking a list would hold
   the poller past its timeout for ever (`PollTerminates`).
   *Why re-register, not re-sample* (audit round 4 F1): the first form of
   the re-arm kept every hook where the first scan put it and only
   re-sampled. That is sound for an object with one list and wrong for a
   Dev that chooses its list by state: the console files a frozen poller
   on `episode_poll_list`, and a hook left there after the episode ENDed
   never saw another keystroke — `poll(-1)` on the console hung until the
   next SAK ([[sub-kernel-cons]]; `cons_poll.tla` `BUGGY_NO_REREGISTER`).
   Re-registration also makes the fd re-resolve WITH its hook (a closed fd
   reports `POLLNVAL` at the next wake), where the re-sample had sampled a
   re-bound fd while sleeping on the old object's list.
   *The loop's own death and stop checks* (round 4 F2): `tsleep`'s #811
   die-check and its 8c-2 stop detour sit BEHIND its cond test, so a
   producer that sets a flag inside every re-sample window keeps every
   `tsleep` returning `AWOKEN` before either: a noise-driven `poll(-1)`
   was unkillable and unstoppable. Each pass checks `thread_die_pending`
   (⇒ the sweep, 0) and parks on `proc_stop_sleeper_park` when a stop is
   pending — with every hook already off, so no producer walks to a
   parked poller; the park returns `SLEEP_INTR` on death (DEATH WINS).
   *The preemption point lived here for part of one day, and is gone*
   (round 5 F1 + round-6 S1; operator decision 2026-09-22; removed by ARCH
   8.12 the same day). Worth keeping the shape, because the DEFECT it
   addressed was real and only its remedy changed: syscall bodies ran
   IRQ-masked end to end, so a noise-driven poll held its CPU's interrupts
   -- the SAK included -- for as long as the noise lasted, and an
   unprivileged pipe, a writer, a reader and an `events=0` poller were
   enough to make that noise. Round 4 filed it as the operator's
   preemption-model question on the premise that nothing unprivileged
   could drive it; the premise was false. Round 5 slept a bounded interval
   once a per-thread budget lapsed, but keyed on the THREAD while the
   obligation is the CPU's: two masked pollers on one CPU each really
   sleep and hand the CPU back and forth, still masked (round-6 S1).
   Round 6's answer was a point crossed on every re-loop. **ARCH 8.12's
   answer is that the body is never masked in the first place**, so the
   CPU is interruptible at every instruction of this loop rather than at
   one chosen spot in it, and `ASSERT_IRQS_ENABLED` in
   `syscall_dispatch_body` asserts it on every syscall of every boot
   instead of a bespoke witness sampling it once per run.

**The retain discipline** (RW-2 2C-F1, [[fnd-rw2-2cf1]]): a
registered hook lives on the OBJECT's embedded list across the whole
sleep, and a **sibling thread** sharing the handle table can close the
last handle mid-sleep — `spoor_clunk` frees the object and its
embedded list, leaving `pw->list` dangling and the unregister
spin-locking freed memory. So the register scan RETAINS the #844
`handle_get` obj ref whenever it actually registered
(`pw->list != NULL`), transferring it to `held[i]`; every re-arm pass and
the sweep release all of them after unregistering, and each re-register
scan takes them afresh. The retain is transitively
sufficient for both real registering paths (pipe ring and devsrv
connection — each frees its embedded list only at the Spoor's last
clunk). The **listener** retain is INERT ([[fnd-rw2-r2poll-f1]],
[[seam-poll-srv-registry-retain]]): `handle_acquire_obj` is a no-op
for `KObj_Srv`, so listener-poll lifetime rests solely on the boot
registry being immortal.

## Data structures

`struct poll_waiter` — magic ("POLW") + `ready` + private-rendez
backref + `list` backpointer (set at register, cleared at unregister;
the sweep's route home without a Dev vtable op) + `next`.
`struct poll_waiter_list` — a spinlock + singly-linked head, embedded
in the pollable object. `struct pollfd` — the 8-byte pinned ABI.
Diagnostics: `poll_total_calls` / `poll_total_slept` — the latter
counts "committed to the slow path", not "actually parked" (a
producer racing register-to-tsleep still increments it) —
and `poll_total_resleeps`: wakes whose re-sample found nothing asked
about, so the poller slept again. It is the tests' witness that a walk
for someone else's edge did not end a poll.

## Concurrency

Producer order inside `poll_waiter_list_wake`, load-bearing: write
`pw->ready = true` FIRST, then `wakeup(pw->rendez)` — the rendez-lock
release/acquire pair carries the flag to the woken cond. **The walk
wakes even a hook whose `ready` is already set, and must** (round 5 F6
proposed skipping it, "sound since ready only rises while listed"; it is
not): `ready` means something only to `sys_poll_for_proc`, whose cond
reads it. The one-shot role waiters that share the list type -- the
cons tx-role, episode and reader-slot waiters, the SrvConn role waiter,
the 9P send waiter -- sleep on a cond that reads the ROLE, not `ready`;
a spurious wake re-sleeps them with `ready` still set, and a deduped
walk would then skip the only wake the next release gives them. The full
chain: object lock → list lock → (the wake enters the wait chain:
`g_timerwait` → rendez → runq). Unregister takes ONLY the list lock —
that asymmetry is what lets the sweep run without deadlocking against
a producer holding the object lock. **Every list op takes the lock
IRQSAVE** (audit round 4 F3, pre-existing): no IRQ handler enters the
list, but it nests under object locks IRQ handlers DO take
(`g_cons.lock` and `g_cons_drain.lock`, by the UART RX IRQ). Taken plain,
console_mgr — a kthread with IRQs on — could be interrupted inside its
walk by an RX IRQ spinning on `g_cons.lock` while another CPU held
`g_cons.lock` in `cons_poll` spinning on the list lock: CPU0 dead in IRQ
context, the other IRQ-masked, a guest wedge. The rule is lockdep's
"IRQ-unsafe lock nested under an IRQ-safe one": a lock taken while
holding a lock some IRQ takes is masked everywhere. The console still
relays its IRQ-side readiness to process context — that keeps the
per-byte IRQ's work O(1) — but no longer because the list forbids it.
No deterministic test exists (it needs an IRQ inside a list hold on one
CPU while another holds the object lock); the guard is the comment at
the list ops, the audit row, and the SMP gate.

Double-register, a stale magic mid-walk, or `pw->list` set but absent
from the list are all extinctions — corruption, not recoverable
states.

## Invariants enforced

[[inv-i9]] across N fds — `NoMissedPoll` in [[spec-poll]]: a poller
is never left asleep while a registered fd is ready. The single-fd
core is [[spec-scheduler]]'s NoMissedWakeup; the deadline leg is
[[spec-tsleep]]; poll adds the N-sources-behind-N-locks composition,
with the hook flag as the cross-lock handoff. `NoStaleHook` pins the
sweep; `NoSpuriousZero` the re-arm (0 only at the deadline);
`PollTerminates` its loop bound; `StableReadyReturns` replaces the
retired `PollReturnsWhenReady` ("a set flag leads to a return" is false
by design now); `DeathTerminates` and `StopHonoured` the loop's own
checks. `IrqLatencyBounded` is GONE with the point (ARCH 8.12): there is
no masked span left for this module to bound, and the CPU-level
obligation is [[spec-syscall-irqs]]'s `CpuGetsItsInterrupts`.
`specs/check-poll.sh` runs the four clean + seven buggy cfgs (two of them
liveness: `no_loop_die_check`, `no_loop_stop_check`) and asserts WHICH
property each buggy one violates; the clean runs measure 2146 / 944
states, down 48 from the point's era -- exactly the `atpoint` states
removed. The list-choosing half of re-registration is
[[spec-cons-poll]]'s (`BUGGY_NO_REREGISTER`, `_CADENCE`).

## Error paths

-1 for `p`/`kfds` NULL, `nfds` 0 or > 64. Per-fd failures are
`POLLNVAL` in revents, never a call failure. The user-VA wrapper
(`sys_poll_handler`) validates the whole array range before copy-in
and scrubs partially-written revents on a writeback fault.

## Performance

O(nfds) lock pairs per scan; no allocation anywhere — `waiters[]`
(~2 KiB) + `held[]` (~1.5 KiB) are frame-resident, which is exactly
why `POLL_MAX_NFDS` is a frame bound, not an fd-table bound.

## Prosecution

- The sweep's three phases must keep their order: unregister →
  scribble → put. Each inversion is a distinct UAF/extinction.
- A new registering path must either be reachable from a retained
  handle kind or add its own ref — the inert-listener caveat is the
  worked counterexample.
- `poll_scan_one`'s retain condition is `pw->list != NULL` AFTER the
  dev call — a Dev that registers conditionally is still covered;
  a Dev that registers on a DIFFERENT list than the one it samples
  breaks the atomicity argument.
- The INTR arm must never skip the sweep.
- The re-arm: every pass unhooks (unregister-all THEN put-all THEN
  clear) before it re-registers; the die and stop checks run on every
  pass, with no hook listed; `timeout_ms == 0` never enters it; the
  loop's own deadline test stays; nothing in a pass may sleep while a
  hook is listed except `tsleep` itself (the stop park runs unhooked).
- Interrupt service: no longer this loop's concern at all (ARCH 8.12).
  The syscall body runs interrupts-on throughout, so the CPU takes its
  interrupts at every instruction of the loop. Nothing here must be
  written as though it were masked.
  The point and its `poll_total_points` counter are both GONE -- the
  counter's declaration outlived its definition by one commit and was
  deleted in the ARCH 8.12 audit round (F4), where it would have been a
  link error for the next caller. What replaced the witness is not another
  counter but the unmask itself: `syscall_dispatch` asserts interrupts-on
  at the top of every syscall body, on every boot, so the property the
  point had to demonstrate with a bespoke test is now continuously
  asserted. `sched_yield_hint` remains in the re-loop and is still
  load-bearing -- interrupts-on is NOT preemption, so without it a noise
  loop would hold its CPU against a runnable peer indefinitely.
  `poll.death_ends_a_noise_driven_poll` / `poll.stop_parks_a_noise_
  driven_poll` pin the checks (a real Proc's thread on the busy Dev
  below), `cons.episode_frozen_poller_follows_end` /
  `cons.episode_prior_poller_not_woken_by_keys` the re-registration.
  `poll.timeout_survives_a_busy_list` is the deadline's device witness, and
  its first form did NOT discriminate: a send/recv producer on another
  thread lands a walk between a clear and the next `tsleep` only by luck,
  and with the deadline test removed the test still passed (measured).
  The producer is now the polled object itself — a test Dev whose `.poll`
  walks its own hook list on every sample and is never ready — so every
  re-sample re-flags the hook inside the window. The walking stops after
  1 s so a kernel without the test still returns; what separates the two
  is WHEN the last sample happened (at the 50 ms deadline, or when the
  producer went quiet) — measured from the FIRST sample since round 4
  F9, so a poller the scheduler starts late cannot read as one that
  returned late.
- **A wake site may walk its list for ANY state change; what it may
  never do is fail to walk it for one.** That licence is what lets one
  list serve two endpoints, and it exists only because of the re-arm.

## Seams

[[seam-poll-srv-registry-retain]] · [[seam-poll-heap-waiters]].

## Caveats

- `docs/reference/72-poll.md` (absorbed) still asserted "poll does
  NOT take a reference on the polled Spoors … no such path exists at
  v1.0" — a soundness argument inverted by the multi-thread lift and
  closed by the retain it says doesn't exist — and sized `waiters[]`
  by `PROC_HANDLE_MAX` throughout, the identity whose restoration
  would overflow the kstack.
- **`syscall.h`'s `SYS_POLL` enum comment still documents `nfds` as
  `1..PROC_HANDLE_MAX = 64`,
  and arrives at the right number by two cancelling errors.** It names
  the wrong constant (the bound is `POLL_MAX_NFDS`; the handler is
  correct) and asserts a value that constant has not held since the
  decoupling — `PROC_HANDLE_MAX` is 1024, so the stated equation is now
  wrong by 16x. It reads as current because 64 is still the right
  answer; nothing about the comment would change if the bound moved.
  Tracked as task #166, the sibling of #87.
- P5-poll F3 ([[fnd-poll-r1-f3]]) is this surface's origin story for
  the batch-8 lesson: a P1 "doc-fixed" by documenting a
  single-thread precondition that a later lift silently voided.

## Provenance

[[chg-2026-05-20-p5-poll]] (mechanism + devpipe + devsrv `.poll` +
the close [[adt-poll-r1]]) → #811 INTR arm →
[[chg-2026-06-10-rw2-poll-retain]] (the retain) → #844 snapshot API →
net-6b-2b `poll_waiter_list_empty` (the dev9p GC's atomic
emptiness probe) → [[chg-2026-06-24-355-poll-decouple]] → the re-arm
(2026-09-21, B-0 libc audit r3 F1/F2: spec extended first, scripture
`6684e7da`; five `poll.*` tests, incl.
`poll.devsrv_client_wakes_on_reply_only`, which pins `NoSpuriousZero`
on a real parked poller) → audit round 4 the same day (scripture
`e55b86ef`: re-registration, the loop's death/stop checks, the irqsave
list lock; two more poll tests and two cons tests) → audit round 5 (the
noise bound, spec first) and round 6 + the operator's "point now, model
next" (2026-09-22): the sleep backstop became the preemption point, which
ARCH 8.12 deleted the same day in favour of an interrupts-on syscall body
(so `IrqLatencyBounded`, `BUGGY_NO_POINT` and `sched_preempt_point` are
all gone, and the two point tests were retargeted onto the re-loop
counter they already had; every poller test entry parks terminally and
publishes its result with a release store).
