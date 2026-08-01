---
id: sub-kernel-death
type: sub
title: "The death path: the ZOMBIE chokepoint and the universal death-wake"
parent: moc-kernel-execution
code: ["kernel/proc.c"]
audit: hard
guarded-by: [inv-i24, inv-i9]
validated-by: [spec-death-wake, gate-smp]
locks: [lock-proc-table]
design: ["docs/ARCHITECTURE.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Terminating a Proc is a **cascade, not a call**. No Thread is ever torn down
from outside; a terminator sets a flag, wakes everything that could be
asleep, and kicks everything that could be running, and each Thread then
kills *itself* at its next EL0-return checkpoint. This dossier owns that
machinery: the flag, the wake, the checkpoint, the shared ZOMBIE chokepoint
every death path funnels through, and the close window that had to be
opened before it.

This is the most bug-prone lineage in the tree —
#788/#806/#807/#808/#860/#809/#811/#926/#68 — and the reason is structural.
Death is the one operation where a Proc's state is being dismantled while
other CPUs may still be reading it, where a wake that arrives a moment too
late is indistinguishable from a wake that never arrives, and where the
consequence of getting it wrong is a hang rather than a crash.

## Contract

| Entry | Caller state | Effect |
|---|---|---|
| `exits(msg)` | the Proc's own thread | terminate the *program*; with live peers, routes through the group cascade then self-exits |
| `thread_exit_self()` | any thread | terminate *this Thread*; the last live one out zombies the Proc |
| `proc_group_terminate(p, msg)` | **holds [[lock-proc-table]]** | flag + wake + kick; does **not** wait |
| `el0_return_die_check()` | at every return-to-EL0 | if flagged, `thread_exit_self()` (noreturn) |
| `proc_fault_terminate(name, addr)` | EL0 unhandled fault | diagnose + `exits(snare:*)`; noreturn |

`proc_group_terminate`'s lock precondition is not stylistic: the cascade
walks `p->threads`, and that list is mutated only under the table lock, so a
lockless walk races `thread_free` into a use-after-free. Holding it also
*serializes every group termination*, which is why the set-once CAS on
`group_exit_msg` only has to guard idempotency, never a genuine race.

## Mechanism

**The four steps of a cascade** (`proc_group_terminate`):

1. Revoke the hardware allowance first (I-34) — folding it here makes
   "killgrp the driver" revoke-then-terminate atomically, so an in-flight
   `SYS_*_CREATE` observes `revoked` at its commit re-check.
2. CAS `group_exit_msg` (RELEASE, first msg wins).
3. Wake, in two passes: `torpor_wake_all_for_proc` for futex sleepers, then
   the **#811 universal death-wake** — walk `p->threads`, take each peer's
   `wait_lock`, read `rendez_blocked_on`, `wakeup()` it.
4. `smp_resched_others()` so a peer running at EL0 on another CPU traps to
   its IRQ-from-EL0 die-check without waiting for a tick. The periodic
   timer is the floor if the IPI is missed.

**Why step 3 is the crux.** The flag is set *before* the walk, and the
sleeper's registration and its re-check of the flag both happen *under its
own `wait_lock`* — the same lock the cascade takes to read
`rendez_blocked_on`. The two critical sections are mutually exclusive, so
every Thread either observes the flag in its register-then-observe and dies
without sleeping, or is found SLEEPING by the walk and woken. There is no
third interleaving. [[spec-death-wake]] is the machine-checked statement of
exactly this, and its buggy cfg is the version where the sleeper checks the
flag *before* registering and outside the lock — which reproduces the
#809-audit F1 **non-reaping hang**.

`wait_lock` is held **across** `wakeup()` (Option A). That is a lifetime
pin, not a lock-order accident: `rendez_blocked_on` can point into a
sleeping peer's *kernel stack frame* (a torpor waiter's `w.rendez`), and the
peer cannot pop that frame because its own resume must re-acquire
`wait_lock` before returning.

**The ZOMBIE chokepoint.** `proc_become_zombie_locked` is the single point
every live Proc's ALIVE→ZOMBIE transition passes through, from both
`exits()` and `thread_exit_self()`. Putting the following there rather than
in `exits()` alone is what makes them fire on *every* death path — a clean
exit and a kill alike:

- the A-4a legate-scope teardown if this Proc is a legate root (audit F1);
- clearing `g_console_owner`, `g_console_trusted_proc`, `g_console_renderer`
  and `g_init_proc` if this Proc held them, so none ever dangles;
- the POSIX 2.4.3 orphan rule, **before** the reparent (the children list is
  consumed there);
- reparenting orphans to init, else `kproc` — and NAMING each one on the
  uart (#80): `proc: orphan pid=N name="X" (parent pid=M name="Y" exiting)
  -> adopted by pid=A`. This is the one point where the kernel still holds
  BOTH Procs, and joey's later reap sweep sees only a pid, so without the
  pair the sweep's report is undecidable from the log alone. Adoption is
  rare and notable by construction — Thylacine has no daemonize idiom, so it
  means some Proc exited with a live child, and a **kproc**-adopted one
  (`adopted by pid=0`, init not yet up) is never reaped at all. The direct
  `uart_puts` path is deliberate: bounded FIFO, no TX ring, no sleep, no
  lock, therefore safe under the table lock;
- capturing status/msg, flipping to ZOMBIE;
- waking the parent's `child_waiters` **under the lock** and posting the
  synthetic `child_exit` note.

The wake-under-lock is the R5-H F75 close: between releasing the lock and
waking, the parent could be reaped and freed by the *grandparent*'s
`wait_pid`, and the wake would touch freed memory.

**The close-at-exit window** (#926, completed by #68). A Proc's fds must
close when the *process* terminates, not when its parent later reaps it —
otherwise a shell draining `$(cmd)`'s stdout to EOF hangs forever, because
EOF needs the reap, the reap needs the parent's wait, and the wait is
waiting on EOF. So the last live Thread out deliberately opens a window
**before** the ZOMBIE flip: drop the lock, `proc_close_handles_at_exit`,
re-take, assert the determination held. Three properties make it sound:

- `t` is still RUNNING, so a **sleep-capable** close hook (a 9P clunk's
  Tclunk/Rclunk wait) is legal — sleeping while EXITING trips `sched()`'s
  assert;
- `p` is still ALIVE, so `wait_pid` cannot reap and `thread_free` the closer
  mid-close;
- `live_peers == 0` means every peer has committed EXITING (whose residual
  execution never touches the handle table) and no new peer can spawn
  without a RUNNING thread.

The window runs under `Thread.exit_close_active`, which makes
`thread_die_pending()` read **false** for the closer. That flag is #68's
whole finding: `group_exit_msg` is set on *every* `SYS_EXIT_GROUP` — a clean
`exit_group(0)` included — so without it the orderly final close read as
"dying" and every sleep-capable hook short-circuited, silently dropping the
dev9p write-behind flush and skipping the close-time Tclunk.

**The stop park.** Two independent owners can park a thread —
`debug_stop_req` (I-39) and `job_stop_req` (I-20) — and they share one park
(`el0_return_stop_check`, the `sleep`/`tsleep` detour, and each Thread's own
`debug_rendez`). Each resume clears **only its own owner**; the park
predicate is the disjunction. Death overrides both: the stop-check runs
*after* the die-check at the tail, and the park loop re-checks
`group_exit_msg` on every wake, so a kill racing a stop terminates the
thread inside the park rather than eret-ing to EL0.

## Data structures

`Proc.group_exit_msg` — NULL means no termination; non-NULL is both the die
flag and the last-out status source (`"ok"` → 0, else 1). Set once by CAS,
never cleared. Read ACQUIRE at every die-check.

`Proc.debug_stop_req` / `job_stop_req` — the two stop owners, deliberately
in the same cache line so the tail's fast path reads both in two ACQUIRE
loads. `Thread.debug_rendez` is per-Thread, so a multi-thread target parks
each thread on its own single-waiter rendez.

`Proc.stop_report_pending` / `cont_report_pending` — the PTY-1e latches a
`WAIT_UNTRACED`/`WAIT_CONTINUED` wait reports and consumes, without reaping.

## Concurrency

Lock order, exhaustively:
`g_proc_table_lock → wait_lock → g_timerwait.lock → r->lock → cs->lock`,
plus the torpor leg `g_proc_table_lock → torpor_lock → g_timerwait → r->lock`.
`torpor_lock` and `wait_lock` never nest (torpor drops its lock before
`tsleep`). `smp_resched_others` and the IPI handler take no locks. No ABBA.

Acyclicity rests on one asymmetry: only the **owner** writes
`rendez_blocked_on`; the cascade only reads it. Every waker→sleeper edge is
therefore read-only, and no path takes a rendez lock and then reaches for
the table lock.

Double-wake is idempotent — `torpor_wake_all` and the rendez walk can both
target the same waiter, and the second `wakeup()` no-ops on `waiter == NULL`.

## Invariants enforced

- [[inv-i24]] — group termination is atomic (one CAS), exactly-once (the
  last-out determination is unique under the lock), and no Thread runs at
  EL0 after ZOMBIE.
- [[inv-i9]] — the death-wake generalization: no wake lost between a
  sleeper's cond-check and its sleep, for **every** rendez sleep. Extended
  by LS-5 to the terminate-disposition `interrupt` latch, which is read
  lock-free by the sleep predicate precisely because the sleep path can
  never take the notes-queue lock.
- I-39/I-20 compatibility (`StopCompatI39`): neither resume may clear the
  other's owner.
- #713 composition: the die-check runs *before* the DAIF-masked
  ELR-set..eret window, and the die path is noreturn, so it never enters it.

## Error paths

Death has no error returns — it extincts or it proceeds. `exits` extincts on
a corrupted thread/proc, on kproc, on a non-ALIVE Proc (double exits), and
on "a peer appeared during handle close" (structural corruption, since
EXITING is one-way and no spawner exists). `proc_fault_terminate` guards its
`name` against NULL — a latent case today, but the value is passed straight
to `uart_puts` and `strcmp`.

The one genuinely soft path: the clear-child-tid handoff is **best-effort**.
An unmapped tidptr skips the wake without extincting (a userspace bug), and
an unaligned one is refused up front because the fault-fixup table does not
catch alignment faults — an unguarded unaligned store would extinct the
kernel.

## Performance

Death is not a hot path and the code says so where it costs something (the
orphan rule's O(procs)-per-candidate walks). The cascade is O(threads) plus
one broadcast IPI, bounded by `ncpus-1`. The close window's cost is one
extra lock round-trip on the exit path.

## Prosecution

The #811 audit's **verified-sound set** is the do-not-re-prosecute preamble
for this surface: both I-9 interleavings, the Option-A stack pin, lock-order
acyclicity, `on_cpu`-spin termination, the walk-vs-`thread_free` UAF
closure, double-wake idempotency, the `exits`/self-kill lock balance, the
torpor `TSLEEP_INTR → TORPOR_OK` absorption, and all nine `*_INTR` arms
(each releases its lock before blocking and returns directly on INTR — no
re-sleep livelock). Re-prosecute only what a change touches.

What a change **must** re-establish:

- the flag-set-before-walk order and the register-then-observe pairing;
- `wait_lock` held across `wakeup` (delete it and a stack rendez can be
  popped under the waker);
- every `proc_group_terminate` caller holding the table lock;
- the ZOMBIE chokepoint's completeness — anything that must fire on *every*
  death path belongs in `proc_become_zombie_locked`, not in `exits()`;
- the close window's three properties, and that `exit_close_active` stays
  owner-set, bounded to the one close pass, and checked *first* in
  `thread_die_pending`;
- death winning over both stop owners at every branch.

## Seams

- [[seam-exiting-tails-never-sleep]] — the recorded property a future
  anon-COW/pageout must re-establish.
- [[seam-close-flush-unbounded]] — a wedged trusted server can strand a
  flagged close, unbreakable by a further kill.
- [[seam-death-cascade-smp-harness]] — the 3-way interleaving no
  deterministic test reaches.

## Caveats

- **The multi-thread `exits()` gate is gone.** `exits()` with live peers is
  no longer an extinction — since #811 it routes through the same cascade as
  `exit_group` and then self-exits. The absorbed reference doc still said
  otherwise, *four lines above* a paragraph describing the machinery in
  detail.
- **`group_exit_msg` set does not mean "killed".** A clean `exit_group(0)`
  sets it too. Treating the two as the same was #68 R1-F1
  ([[fnd-68-r1-f1]]) and cost silent data loss.
- **The re-admitted strand is not breakable.** `exit_close_active` suppresses
  both death legs, so a wedged flagged close parks the dying Proc unreapably.
  That is a deliberate trade (the alternative was the parent hanging), whose
  precondition is an already-degraded system.
- The interrupt-terminate wake deliberately omits both
  `torpor_wake_all_for_proc` and `smp_resched_others` — the former because
  torpor waiters are reachable via `rendez_blocked_on` anyway, the latter
  because the IRQ-from-EL0 tail evaluates only `group_exit_msg`, so an IPI
  cannot accelerate an interrupt-death. The no-IPI shape is also what lets
  the unit test drive the real waker on the single-CPU harness.

## Provenance
