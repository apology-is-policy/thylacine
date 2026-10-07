---
id: sub-kernel-death
type: sub
title: "The death path: the ZOMBIE chokepoint and the universal death-wake"
parent: moc-kernel-execution
code: ["kernel/proc.c"]
audit: hard
guarded-by: [inv-i24, inv-i9, inv-i44]
validated-by: [spec-death-wake, gate-smp]
locks: [lock-proc-table]
design: ["docs/ARCHITECTURE.md", "docs/LINEAGE.md"]
created: 2026-08-01
updated: 2026-10-06
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

**Scope and session cleanup (2026-09-17).** The imported Imperium teardown
closes the publish-after-sweep race through the child insertion check in
[[sub-kernel-proc]]. Process exit now releases the process's Territory at exit,
rather than leaving namespace/session references pinned until reaping. This
allows a login waiting on its home server to complete after shell exit.
`proc_session_hangup_if_leader` delivers the session hangup to eligible peers
when an armed leader dies; its table walk follows the existing held-lock death
protocol. The cross-user arm of `tools/interactive/ls-imperium.exp` verifies
that michael can abdicate/log out and cora can log in afterward.


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
- **the arm-6 session hangup** (`proc_session_hangup_if_leader`, IDENTITY-DESIGN
  9.9.1): if this Proc is a `PROC_FLAG_SESSION_HANGUP`-armed session leader
  (`sid == pid`), `proc_group_terminate` every OTHER ALIVE Proc sharing its
  `sid`. The structural sibling of the legate teardown, placed here for the same
  reason — so logout reclaims the user's session on *every* leader death path (a
  clean `exit`, a kill alike), and no orphaned session Proc keeps the per-user
  encrypted-home mount pinned (with Part D below, each terminated member releases
  its mount ref at its own exit). It is kernel-driven session-*lifecycle*
  termination, not a userspace cross-Proc kill — login lacks CAP_KILL by design,
  so I-26 is untouched. Its isolation (only genuine session members match) rests
  on pids never recycling (`g_next_pid` is monotonic + extincts at INT_MAX); the
  legate it mirrors keys on a dedicated non-reusable `legate_scope_id` instead —
  an implicit-vs-local dependency recorded at `session_hangup_cb` (arm-6 audit F1);
- clearing `g_console_owner`, `g_console_trusted_proc`, `g_console_renderer`
  and `g_init_proc` if this Proc held them, so none ever dangles;
- the graphical seat's three arms, before the trusted-proc clear: the seat
  SERVICE's death fails the seat; the compositor CLIENT's death fails it only
  mid-episode and otherwise just clears the slot; corvus's death fails it when
  an episode is in progress. A seat failure closes a console episode only when
  the seat opened it ([[sub-kernel-proc]]). The client and service arms are
  driven through this chokepoint by REAL deaths (an `rfork` child takes the
  role, an episode opens over it, it exits and is reaped):
  `cons.graphical_seat_deadline_and_death` and
  `cons.graphical_seat_service_death`;
- the POSIX 2.4.3 orphan rule, **before** the reparent (the children list is
  consumed there) — [[sub-kernel-jobctl]] owns it, and the ordering is the
  whole trick: it asks "orphaned once I am gone" while the answer is still
  computable;
- the birth-hold orphan rule, also **before** the reparent and for the same
  reason: every ALIVE child whose birth hold is still set, neither converted by
  a debugger's `stop` nor released, is group-terminated with "launcher exited"
  ([[sub-kernel-birth-hold]]; operator vote 2026-09-29). A held child whose
  launcher died would otherwise sit parked, adopted by init, forever;
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
- the parent's side, `proc_exit_notify_parent_locked`, all **under the
  lock**: wake the parent's `child_waiters`, post the synthetic `child_exit`
  note, and run the caught-note wake (`proc_caught_note_wake`) so a parent
  that catches `child_exit` -- a SIGCHLD handler, a notes fd -- and is already
  asleep in an interruptible wait takes it now ([[sub-kernel-notes]]). There is
  no terminate wake: `child_exit` defaults to ignore and never arms that latch.

The wake-under-lock is the R5-H F75 close: between releasing the lock and
waking, the parent could be reaped and freed by the *grandparent*'s
`wait_pid`, and the wake would touch freed memory.

**A peer Thread's own exit carries the pthread-join wakeup.** `thread_exit_self`
runs `thread_clear_child_tid_handoff` before it departs: if the Thread registered
a `clear_child_tid` word (via `SYS_SET_TID_ADDRESS`, which stores the tidptr on the
Thread), the kernel atomically zeroes `*clear_child_tid` and `torpor_wake`s
`UINT32_MAX` waiters on that VA — the futex a joining pthread parks on. This is the
kernel half of `pthread_join`, and it fires for **every** exiting Thread, not only
the last one out (that one additionally zombies the Proc through the chokepoint
above). An unmapped or unwritable tidptr **silently skips** both the store and the
wake — via the `uaccess_store_u32` fixup ([[sub-kernel-uaccess]]) — rather than
extincting: a departing Thread must not be able to crash the box with a bad address
it registered earlier.

**The exit byte is the real one now (#91).** The ZOMBIE chokepoint captures
`exit_status` **verbatim** — the byte a userspace `t_exits(42)` or a phenotype
`exit_group(42)` passed — not the 0/1 collapse it was before #91, which lost every
real status at two kernel points (`sys_exits_handler`, `sys_exit_group_handler`).
The collapse survives only for the ~dozen in-kernel `exits("msg")` callers,
through a deliberate split: `exits(msg)` maps `"ok"`→0 / else→1 and calls
`exits_code(code, msg)`, while the syscall entries call `exits_code` directly with
the real byte. So `group_exit_msg` still gates the death and carries the msg,
while the numeric status lives in the separate `exit_status` field the parent's
wait packs (`WAIT_STATUS_EXITED`, in [[sub-kernel-proc]]'s `wait_pid_for`). The
two-field split is the point: the flag that says "die" and the byte that says
"with what status" are no longer one datum.

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

A second kill ends that hold (ARCH 7.9.1 part B, `dec-2026-10-07-exit-close`).
`proc_group_kill` -- the `kill` note's cascade (syscall.c) and the `/proc` ctl
`kill` (devproc.c), never a hangup, `EXITKILL` or a legate scope's end, which
keep the string wrapper -- runs the same core as every termination, and when
its CAS on `group_exit_msg` loses (the group is already terminating), or the
Proc carries `PROC_FLAG_EXIT_CLOSING` (set by `proc_close_handles_at_exit`, so
an `exits()` close, which sets no group exit message, counts as terminating),
it ORs `PROC_FLAG_EXIT_CLOSE_FORCED` into `proc_flags` (RELEASE) before the
wake loop.
notes.c's `thread_death_held` then reads the hold as lifted: the final close's
send is refused, its wait unwinds through Tflush, and what it could not finish
goes to the closer ([[sub-kernel-ninep-dev9p]], part C). The first kill never
forces, so an orderly exit close still has its flush reply before the parent's
`wait` returns (I-38). `loom_free`'s SQPOLL join does not ride
`exit_close_active` any more: it sets its own `kthread_join_active`, which
holds every death, forced or not ([[sub-kernel-loom]]).

Because the closer reads no death, a stop must not park it either: group death
clears no stop owner, and a closer parked for a stop would hold the dying Proc
until the stop cleared (read from the code: a `kill` of a Ctrl-Z'd job with a
dirty 9P file would live on until `tty:cont`, and a debugger that killed its
stopped target and waited for the exit before closing its ctl fd would
deadlock with it). So the park predicate, `proc_stop_requested`, reads false
once `group_exit_msg` is set, and the closer's sleeps never detour into a stop
park (DEBUG-FS-DESIGN 5g). A closer that parked while its group still lived,
as an `exits()` close honours a stop, leaves the park when the group dies: the
death cascade's wake finds the predicate false. A dying Proc is not stopped to
anything else either: both stop delivers refuse it, a parent's wait reports
neither its stop nor its continue, `stop`, `waitstop` and a step's wait read it
as gone, the orphan rule does not count it as a stopped member, and
`/ctl/procs` does not show it STOPPED ([[sub-kernel-jobctl]],
[[sub-kernel-devproc]]).

**The territory release rides the same window** (arm-6 Part D, IDENTITY-DESIGN
9.9.1; extends #926/#68 from the handle table to the namespace). A Proc's
Territory — its per-Proc mounts + name-based cwd — was released only at reap
(`proc_free`), so a zombie or orphan kept a `spoor_ref` on every mount it
inherited, a per-user encrypted-home `--single-session` 9P proxy mount included;
login's synchronous `proxy.wait()` at logout then deadlocked on an orphan the
kernel could not reap in time (joey blocked in `wait(login)`; login blocked in
`proxy.wait`). `proc_release_territory_at_exit` releases it in the SAME
last-live-thread window as the handle close, with one added subtlety: devproc
`format_ns`/`format_cwd` read `p->territory` under [[lock-proc-table]] (the #66c
envelope), so the free cannot run lock-free the way the handle-table free does.
It is a **detach-under-lock + free-outside-lock split** — NULL `p->territory`
under the table lock (a concurrent devproc reader then renders empty; both
readers are NULL-safe), then `territory_unref` the detached, now-unreachable
pointer with the lock dropped (its `spoor_clunk` may sleep on a `Tclunk`).
Idempotent with `proc_free`'s later `territory_unref(NULL)`. Since fork
DEEP-COPIES the territory ([[sub-kernel-proc]]'s `territory_clone`; RFNAMEG
sharing is unsupported at v1.0), no sibling shares it and I-1 is trivially
preserved. **F2 (open):** a member that `SYS_SETSID`'s out of the session
escapes the hangup above and keeps its mount ref pinned — the same stall, for
that uncommon daemonizing case; the robust cure binds the mount to the DEK
lease, not to session membership (tracked, arm-6 audit F2).

**The vfork park, and why death pays nothing for it.** A fork that shares the
parent's address space suspends the parent until the child leaves it, and the
child leaving is one of three events — it exec'd, it died, or it is gone from
the children list. The park reuses the parent's `child_waiters` list, which is
what makes **the death release free**: the ZOMBIE chokepoint already wakes that
list, so a vfork child dying releases its parent through machinery that predates
vfork by months. Only the exec release needed a new wake, and it is one line
under the same lock at the address-space swap.

The design principle is stated in the source and is worth carrying, because it
is the same one the [[dec-2026-08-15-cutover]] decision rests on:

> The release condition is not a *record* of the release, it **is** the release.

"The child is off my frame" means "the child no longer maps my address space",
and that is a fact already written down — the child's address-space pointer. A
flag would have been the obvious design and is strictly worse: it records the
release somewhere other than where the release happens, so a third release path
added later would silently strand every vfork parent. The pointer comparison
cannot drift from reality because it *is* reality.

Three properties keep it sound:

- **The comparison is not an ABA** only because the parent still holds a
  reference to the shared address space, so the outgoing object cannot be freed
  and its address cannot be recycled underneath the comparison. That is a
  direct dividend of the extraction having moved the VMA drain into the address
  space's last drop.
- **"Gone from the list" counts as released, deliberately.** It would mean some
  path removed the child without passing either release site, and the only two
  dispositions available are "resume" and "hang forever". A parent that resumes
  early corrupts a frame the child has already stopped using; a parent that
  hangs looks unkillable and never recovers. It fails toward the one that
  terminates.
- **A parent killed while parked returns `SLEEP_INTR` and does not loop** —
  re-sleeping would re-interrupt forever — and leaves nothing behind, because it
  registered no state anywhere but its own stack. The park re-initialises its
  waiter on every iteration so a wake left over from the previous pass cannot
  make it spin.

The exec-side wake is unconditional rather than tested against "is anyone
suspended", and the reason is this dossier's recurring one: a test would be a
second place that has to agree with the park about who is waiting. A spurious
wake costs a re-scan.

**The held spawn shares the park, and is the exception to its principle
(2026-09-29).** A spawn asked with `SPAWN_DEBUG_HELD` suspends its caller until
the child has parked in front of its first instruction
([[sub-kernel-birth-hold]]). The waiting discipline now lives once, in
`await_child_release`, and the vfork suspend and the birth wait differ only in
the release predicate each passes. Death releases both for free, through the
same chokepoint wake. Here the release condition *is* a record, because
nothing already written down says "the child has finished loading": the
child's birth-hold mark. The principle above is kept by the next best means.
Every write of that mark goes through one setter pair that wakes
`child_waiters` under the lock, so no path out of UNBORN can skip the wake.
Since 2026-09-30 the park sleeps in `sleep_death_only` (DEBUG-FS-DESIGN 5g),
so only the caller's group death returns it early. Its own terminate latch is
revocable, since a peer can install a handler or open the notes file, and a
parent that returned on it could live on with a vfork child still on its stack
or a held child still loading.

**The stop park.** Two independent owners can park a thread —
`debug_stop_req` (I-39) and `job_stop_req` (I-20) — and they share one park
(`el0_return_stop_check`, the `sleep`/`tsleep` detour, and each Thread's own
`debug_rendez`). Each resume clears **only its own owner**; the park
predicate is the disjunction, false in a dying group. Death overrides both:
the stop-check runs *after* the die-check at the tail (and before the notes
leg, so a stop in turn wins over a note), and reads the owners'
flags (`proc_stop_owned`), not the predicate, so a thread killed between the
two still enters the park; the park loop re-checks `group_exit_msg` on every
wake, so a kill racing a stop terminates the thread inside the park rather
than eret-ing to EL0. Only death does: the park
sleeps in `sleep_death_only`, so a terminate latch's wake is absorbed and a
stopped thread stays stopped. Once a resume lets it run, it meets the note as
the park returns, in the notes leg that follows on the synchronous and birth
tails; a park on the IRQ tail, which delivers no notes, leaves it to the next
checkpoint (DEBUG-FS-DESIGN 4.2, 5g). The second owner and its fans are
[[sub-kernel-jobctl]]; [[spec-pty-stop]] is the composition.

The loop checks death twice per pass since 2026-09-29: at the top, and again
after the wake condition passes. The second check is for a release that
follows a terminate. The debugger's exitkill release terminates the group and
only then clears the stop, and a `start` sent after a `kill` clears after the
kill. A thread that passed the top check just before the terminate would read
the cleared flags and `eret` into a group already dying. Both clears are
RELEASE stores ordered after the terminate's, so the ACQUIRE re-check sees it.
The held model of [[spec-debug-stop]] found the gap on its first run, and it
was never specific to the birth hold. The birth park runs the same loop with
its own wake condition. The model's action property `NoEretIntoDeath` states
the rule for both parks, and two buggy cfgs remove the check, one at each
park: `no_death_recheck` at the birth park and `no_death_recheck_tail` at the
stop park.

## Data structures

`Proc.group_exit_msg` — NULL means no termination; non-NULL is the die flag
**and** the last-out msg string. Set once by CAS, never cleared. Read ACQUIRE at
every die-check. Since #91 the numeric status is a **separate** field,
`Proc.exit_status` (int): the real exit byte, stored verbatim, no longer the
`"ok"`→0/else→1 collapse derived from the msg. `group_exit_msg` decides *whether*
the Proc dies and carries the msg; `exit_status` decides *what status* the
parent's wait reads.

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
  never take the notes-queue lock. The five death-only waits read group death
  alone and register for the death wake exactly as every other sleep does.
- I-39/I-20 compatibility (`StopCompatI39`): neither resume may clear the
  other's owner.
- #713 composition: the die-check runs *before* the DAIF-masked
  ELR-set..eret window, and the die path is noreturn, so it never enters it.
- [[inv-i1]] (arm-6 Part D) — releasing the Territory at exit preserves per-Proc
  namespace isolation: fork deep-copies it (`territory_clone`; RFNAMEG
  unsupported), so a Proc's exit-time `territory_unref` decrements a count no
  other Proc holds. The arm-6 session hangup composes with I-24 above — every
  terminated member rides the same exactly-once cascade, and the leader's own
  hangup is idempotent under the set-once `group_exit_msg` CAS.

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
  `thread_die_pending` (through `thread_death_held`);
- that only a kill sets `PROC_FLAG_EXIT_CLOSE_FORCED` -- on a lost CAS or a
  set `PROC_FLAG_EXIT_CLOSING` -- and before the wake loop (a close that
  re-checks after the wake must see it);
- death winning over both stop owners at every branch, the exit close
  included (a dying group is never asked to park).

## Seams

- [[seam-exiting-tails-never-sleep]] — the recorded property a future
  anon-COW/pageout must re-establish.
- [[seam-close-flush-unbounded]] — a server that never answers held a flagged
  close; parts A-C (2026-10-07) bound it: the clunk never waits, and a second
  kill forces the close and hands the rest to the closer.
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
- **The re-admitted strand breaks on a second kill, not on the first.**
  `exit_close_active` suppresses both death legs, so a server that never
  answers holds the final close until a kill forces it (part B); the first
  kill, an `exit_group`, a hangup or `EXITKILL` never force. A forced exit's
  parent can read the file before the closer's write lands: the I-38 window
  the second kill buys.
- The interrupt-terminate wake deliberately omits both
  `torpor_wake_all_for_proc` and `smp_resched_others` — the former because
  torpor waiters are reachable via `rendez_blocked_on` anyway, the latter
  because the IRQ-from-EL0 tail evaluates only `group_exit_msg`, so an IPI
  cannot accelerate an interrupt-death. The no-IPI shape is also what lets
  the unit test drive the real waker on the single-CPU harness.
- **Logout reclaims the session's process *tree*, not every process that ever
  had the sid.** The arm-6 hangup sweeps by `sid`, so a member that `SYS_SETSID`s
  into its own session escapes it and keeps the per-user home mount pinned — the
  logout stall persists for that (uncommon, daemonizing) case. The reported bug
  (a plain background job) is fixed. The residual is arm-6 audit F2: the robust
  cure binds the mount lifetime to the DEK lease, not to session membership.

- **A single-thread guarantee bounds threads, and says nothing about other
  processes.** exec resets the signal dispositions, and for one release it did so
  by *freeing* the table — reasoning from the exec-alone gate that there could be
  only one reader. That gate bounds the *threads of this process*. It says
  nothing whatever about other processes, and the note-post path reaches this
  process's table with somebody else as the poster on essentially every call: the
  child-exit note to a parent, an explicit post, the process-group fan, the
  console interrupt, a terminal hangup. Those readers load the pointer with a
  bare acquire and hold no lock of exec's. So the free was a use-after-free
  across CPUs — one loads the pointer, this one frees it, the first dereferences
  freed slab.

  The fix resets **in place** and never frees; the allocation lives until reap,
  which is the lifetime it had before the free was moved forward to exec. Zeroing
  is byte-identical to the freshly-allocated table, so the dispositions really are
  back to default.

  Worth carrying as a shape, not just an incident: the wrong comment was not
  vague, it was *precise about the wrong scope*, and it cited a real guarantee
  that really does hold. The same exec path clears the hardware breakpoint and
  watchpoint slots under the same guarantee — and there the reasoning is sound,
  because a debugger can only have armed them while the target was fully stopped
  and this is the only live thread. Same gate, one valid use and one invalid one,
  forty lines apart. A third use is valid for the same reason: exec clears the
  caught-note claim sub-field of `proc_flags` (2026-09-30), and a claim is taken
  only by a thread of this process for its own wait, so with exec alone the clear
  is a guard, not a repair -- posters from other processes set caught bits, never
  claims ([[sub-kernel-notes]]).

## Provenance

[[chg-2026-08-15-proc-lineage]] is the re-sweep after the LINEAGE arc: the
vfork park that rides the existing child-waiter wake, and the exec-time
disposition reset that had to stop being a free.

**2026-08-16: flagged by co-tenancy, nothing owed.** The interval's `proc.c`
churn is entirely the exec path — the disposition reset this dossier already
records as having stopped being a free, plus its audit follow-up on store width.
The group-termination cascade, the zombie transition and the death-wake legs are
untouched. Checked by hunk context, not by reading the diff for anything
familiar; the exec-side content is on [[sub-kernel-proc]] and
[[sub-kernel-vivarium]].

[[chg-2026-09-05-death-exit-byte]] is the next earned interval: #91 (`f557beb2`)
made the ZOMBIE chokepoint capture the real exit byte in `exit_status` verbatim
instead of collapsing it to 0/1 — the exit-status half of self-hosting's C1 floor.

[[chg-2026-09-06-sys-thread-doc-absorb]] folds the `thread_clear_child_tid_handoff`
pthread-join wakeup absorbed from docs/reference/81: a peer Thread's exit atomically
zeroes its registered `clear_child_tid` and `torpor_wake`s joiners, silently
skipping a bad tidptr.

**2026-09-09: arm-6 — territory-at-exit (Part D, `8bcc2e3f`) + the kernel session
hangup (A1, `6758a1bd`); arc close `b1b68eaa`.** Part D moved the Territory
release from reap to the last-live-thread exit window (the detach-under-lock +
free-outside split), extending the #926/#68 close-at-exit discipline to the
namespace so a zombie/orphan stops pinning its inherited per-user home mount.
A1 added the session-leader-exit hangup to the ZOMBIE chokepoint (the
legate-teardown sibling), realizing A-5 decision (3)'s "no orphaned session
Proc" — kernel-driven because login lacks CAP_KILL (I-26 untouched). Scripture:
`564de51c` (IDENTITY-DESIGN 9.9.1). Formal audit (holotype-reviewer, Opus
fallback — Fable out of credits): SOUND 0/0/0/2 P3 — F1 fixed (the `sid==pid`
isolation's dependency on non-recycling pids documented at `session_hangup_cb`),
F2 tracked (the `setsid`-daemon residual, Mechanism + Caveats above). SMP gate
40 boots, 0 corruption; ls-imperium arms 0-6 PASS.

## PCI claims retained by register mappings (2026-09-17)

The device-quiesce VMA sweep now recognizes the PCI owner retained by ordinary
MMIO Burrows. Even after the PCI handle closes, the last address-space holder
quiesces DMA before draining mappings and DMA buffers. Live hostmem aliases keep
memory decoding enabled through the existing DMA-only quiesce rule. This extends
the existing mmap-then-close death protection to PCI register mappings; it is not
yet the approved interrupt-endpoint revocation path.

## PCI endpoints as the final owner (2026-09-17)

The handle sweep also resolves a PCI endpoint's retained function. It therefore
quiesces DMA and revokes delivery even if both parent handle and register mappings
were closed while an IRQ endpoint survived. Owner quiescence's terminal flag
prevents concurrent ARM/COMPLETE from restoring delivery during teardown.

## The exit close releases the phenotype's socket cache (2026-09-29, NP-5)

`proc_close_handles_at_exit` now also resets the Linux socket table
(`viv_socktab_reset`), right after `handle_table_free` and inside the same
`exit_close_active` window, for the same reason the handle table closes there
rather than at reap. NP-5 made each socket row hold a cached readiness Spoor, an
open fid at netd, and every fid under `/net/<proto>/N/` holds netd's slot N: a
cache released only at reap kept a socket the exiting process had closed open to
its peer (a forked worker's accepted connection, say) until the parent reaped the
zombie. The reset's clunks are close-time Tclunks, legal here for the same
reasons as the handle table's (the thread is still RUNNING, the Proc still
ALIVE). The table itself is still freed at `proc_free`
([[sub-kernel-proc]]); the reset is NULL-safe (a native Proc has no table).
`proc_close_handles_at_exit_for_test` drives the close on a Proc a test built
(`vivarium.socktab_ready_release_paths`).
